#pragma once

// A small stand-in for Google's token, Calendar, and Tasks endpoints, for the
// sync tests. It runs its own thread (the code under test blocks while it
// waits for answers), keeps everything in memory, and records every request
// so the tests can check what left "the computer". The phone* helpers change
// things the way the iPhone apps would.

#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>

class FakeGoogle {
public:
    struct Request {
        QByteArray method, path, query, body, authorization;
    };

    // What the fake counts as true; the tests read and change it under lock().
    QString refreshToken = "refresh-1";
    QString accessToken = "access-1";
    int refreshCount = 0;
    bool offline = false;
    bool revokeAll = false;      // the refresh token is no longer accepted
    qint64 minSyncToken = 0;     // older calendar sync tokens get 410 Gone
    QString expectedChallenge;   // PKCE: the S256 challenge from the auth URL
    QString loginCode = "code-from-google";
    QList<Request> requests;

    struct Calendar {
        QString id, summary;
        QHash<QString, QJsonObject> events; // id -> event
    };
    struct TaskList {
        QString id, title;
        QHash<QString, QJsonObject> tasks;
    };
    QList<Calendar> calendars;
    QList<TaskList> lists;

    FakeGoogle() {
        // The user's own things, which the app must never touch.
        calendars.append(Calendar{"primary", "Me", {}});
        calendars[0].events.insert("mine1", QJsonObject{{"id", "mine1"}, {"summary", "Dentist"}, {"status", "confirmed"}});
        lists.append(TaskList{"MyTasksId", "My Tasks", {}});
        lists[0].tasks.insert("own1", QJsonObject{{"id", "own1"}, {"title", "Groceries"}, {"status", "needsAction"}});

        m_worker.moveToThread(&m_thread);
        m_thread.start();
        QMetaObject::invokeMethod(&m_worker, [this] {
            m_server = new QTcpServer(&m_worker);
            m_server->listen(QHostAddress::LocalHost, 0);
            QObject::connect(m_server, &QTcpServer::newConnection, &m_worker, [this] {
                while (QTcpSocket *socket = m_server->nextPendingConnection()) {
                    QObject::connect(socket, &QTcpSocket::readyRead, &m_worker, [this, socket] { serve(socket); });
                    QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                }
            });
            m_port = m_server->serverPort();
        }, Qt::BlockingQueuedConnection);
    }

    ~FakeGoogle() {
        QMetaObject::invokeMethod(&m_worker, [this] { delete m_server; }, Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait();
    }

    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_port); }
    QMutex &mutex() { return m_mutex; }

    // ---------- looking at the fake's state (call under lock) ----------

    Calendar *calendarNamed(const QString &summary) {
        for (Calendar &c : calendars)
            if (c.summary == summary)
                return &c;
        return nullptr;
    }
    TaskList *listNamed(const QString &title) {
        for (TaskList &l : lists)
            if (l.title == title)
                return &l;
        return nullptr;
    }
    QList<QJsonObject> liveEvents() {
        QList<QJsonObject> out;
        if (Calendar *c = calendarNamed("Omni Reminder"))
            for (const QJsonObject &e : c->events)
                if (e.value("status").toString() != "cancelled")
                    out << e;
        return out;
    }
    QList<QJsonObject> liveTasks() {
        QList<QJsonObject> out;
        if (TaskList *l = listNamed("Omni Reminder"))
            for (const QJsonObject &t : l->tasks)
                if (!t.value("deleted").toBool())
                    out << t;
        return out;
    }

    // ---------- the phone ----------

    QString phoneAddEvent(const QString &summary, const QDateTime &start) {
        QMutexLocker lock(&m_mutex);
        Calendar *c = calendarNamed("Omni Reminder");
        const QString id = QStringLiteral("phone%1").arg(++m_counter);
        c->events.insert(id, stamp(QJsonObject{{"id", id},
                                               {"summary", summary},
                                               {"status", "confirmed"},
                                               {"start", QJsonObject{{"dateTime", start.toUTC().toString(Qt::ISODate)}}},
                                               {"end", QJsonObject{{"dateTime", start.addSecs(3600).toUTC().toString(Qt::ISODate)}}}},
                                   {}));
        return id;
    }
    void phoneEditEvent(const QString &id, const QJsonObject &fields, const QDateTime &updated = {}) {
        QMutexLocker lock(&m_mutex);
        QJsonObject &e = calendarNamed("Omni Reminder")->events[id];
        for (auto it = fields.begin(); it != fields.end(); ++it)
            e.insert(it.key(), it.value());
        e = stamp(e, updated);
    }
    void phoneDeleteEvent(const QString &id) { phoneEditEvent(id, {{"status", "cancelled"}}); }

    QString phoneAddTask(const QString &title, const QDate &due = {}) {
        QMutexLocker lock(&m_mutex);
        TaskList *l = listNamed("Omni Reminder");
        const QString id = QStringLiteral("phonetask%1").arg(++m_counter);
        QJsonObject t{{"id", id}, {"title", title}, {"status", "needsAction"}};
        if (due.isValid())
            t.insert("due", due.toString(Qt::ISODate) + "T00:00:00.000Z");
        l->tasks.insert(id, stamp(t, {}));
        return id;
    }
    void phoneEditTask(const QString &id, const QJsonObject &fields, const QDateTime &updated = {}) {
        QMutexLocker lock(&m_mutex);
        QJsonObject &t = listNamed("Omni Reminder")->tasks[id];
        for (auto it = fields.begin(); it != fields.end(); ++it)
            t.insert(it.key(), it.value());
        t = stamp(t, updated);
    }
    // The Tasks app hides completed tasks once they are cleared: only a
    // client that asks with showHidden sees them.
    void phoneCompleteTask(const QString &id) {
        phoneEditTask(id, {{"status", "completed"}, {"hidden", true},
                           {"completed", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
    }
    void phoneDeleteTask(const QString &id) { phoneEditTask(id, {{"deleted", true}}); }
    void deleteOurCalendar() {
        QMutexLocker lock(&m_mutex);
        for (int i = 0; i < calendars.size(); ++i)
            if (calendars[i].summary == "Omni Reminder")
                calendars.removeAt(i--);
    }

private:
    QJsonObject stamp(QJsonObject item, const QDateTime &updated) {
        item.insert("updated", (updated.isValid() ? updated : QDateTime::currentDateTimeUtc()).toUTC().toString(Qt::ISODateWithMs));
        item.insert("seq", ++m_seq);
        return item;
    }

    static QJsonObject publicView(QJsonObject item) {
        item.remove("seq");
        return item;
    }

    void reply(QTcpSocket *socket, int status, const QByteArray &body = {}) {
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " X\r\nContent-Type: application/json\r\n"
                      "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->flush();
        socket->disconnectFromHost();
    }
    void reply(QTcpSocket *socket, int status, const QJsonObject &object) {
        reply(socket, status, QJsonDocument(object).toJson(QJsonDocument::Compact));
    }
    static QJsonObject error(int code, const QString &message) {
        return {{"error", QJsonObject{{"code", code}, {"message", message}}}};
    }

    void serve(QTcpSocket *socket) {
        const QByteArray all = socket->peek(1 << 20);
        const int headerEnd = all.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;
        const QByteArray head = all.left(headerEnd);
        qint64 length = 0;
        QByteArray authorization;
        for (const QByteArray &line : head.split('\n')) {
            const QByteArray l = line.trimmed();
            if (l.toLower().startsWith("content-length:"))
                length = l.mid(15).trimmed().toLongLong();
            if (l.toLower().startsWith("authorization:"))
                authorization = l.mid(14).trimmed();
        }
        if (all.size() < headerEnd + 4 + length)
            return;
        socket->read(headerEnd + 4 + length);
        const QList<QByteArray> requestLine = head.left(head.indexOf("\r\n")).split(' ');
        const QByteArray target = requestLine.value(1);
        Request r;
        r.method = requestLine.value(0);
        r.path = target.left(target.indexOf('?') < 0 ? target.size() : target.indexOf('?'));
        r.query = target.indexOf('?') < 0 ? QByteArray() : target.mid(target.indexOf('?') + 1);
        r.body = all.mid(headerEnd + 4, length);
        r.authorization = authorization;

        QMutexLocker lock(&m_mutex);
        requests.append(r);
        if (offline) {
            socket->abort(); // as good as no network
            return;
        }
        handle(socket, r);
    }

    void handle(QTcpSocket *socket, const Request &r) {
        const QString path = QUrl::fromPercentEncoding(r.path);
        const QUrlQuery query{QString::fromUtf8(r.query)};
        auto param = [&](const QString &key) { return query.queryItemValue(key, QUrl::FullyDecoded); };

        if (path == "/token") {
            const QUrlQuery form{QString::fromUtf8(r.body)};
            auto field = [&](const QString &key) { return form.queryItemValue(key, QUrl::FullyDecoded); };
            if (field("client_id") != "test-client.apps.googleusercontent.com")
                return reply(socket, 401, QJsonObject{{"error", "invalid_client"}});
            if (field("grant_type") == "refresh_token") {
                if (revokeAll || field("refresh_token") != refreshToken)
                    return reply(socket, 400, QJsonObject{{"error", "invalid_grant"}, {"error_description", "Token has been expired or revoked."}});
                ++refreshCount;
                accessToken = QStringLiteral("access-%1").arg(refreshCount + 1);
                return reply(socket, 200, QJsonObject{{"access_token", accessToken}, {"expires_in", 3599}, {"token_type", "Bearer"}});
            }
            if (field("grant_type") == "authorization_code") {
                const QString challenge = QString::fromLatin1(
                    QCryptographicHash::hash(field("code_verifier").toLatin1(), QCryptographicHash::Sha256)
                        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
                if (field("code") != loginCode || challenge != expectedChallenge ||
                    !field("redirect_uri").startsWith("http://127.0.0.1:"))
                    return reply(socket, 400, QJsonObject{{"error", "invalid_grant"}});
                return reply(socket, 200, QJsonObject{{"access_token", accessToken},
                                                      {"refresh_token", refreshToken},
                                                      {"expires_in", 3599},
                                                      {"scope", "https://www.googleapis.com/auth/tasks "
                                                                "https://www.googleapis.com/auth/calendar.app.created"}});
            }
            return reply(socket, 400, QJsonObject{{"error", "unsupported_grant_type"}});
        }
        if (path == "/revoke")
            return reply(socket, 200, QByteArray("{}"));

        if (r.authorization != "Bearer " + accessToken.toUtf8())
            return reply(socket, 401, error(401, "Request had invalid authentication credentials."));
        const QJsonObject body = QJsonDocument::fromJson(r.body).object();

        // ----- Calendar -----
        static const QRegularExpression calendarPath("^/calendar/v3/calendars(?:/([^/]+))?(?:/events(?:/([^/]+))?)?$");
        const auto c = calendarPath.match(path);
        if (c.hasMatch()) {
            const QString calendarId = c.captured(1), eventId = c.captured(2);
            const bool events = path.contains("/events");
            if (calendarId.isEmpty() && r.method == "POST") {
                const QString id = QStringLiteral("omni%1@group.calendar.google.com").arg(++m_counter);
                calendars.append(Calendar{id, body.value("summary").toString(), {}});
                return reply(socket, 200, QJsonObject{{"id", id}, {"summary", body.value("summary")}});
            }
            Calendar *calendar = nullptr;
            for (Calendar &each : calendars)
                if (each.id == calendarId)
                    calendar = &each;
            if (!calendar)
                return reply(socket, 404, error(404, "Not Found"));
            // calendar.app.created: only calendars this app made.
            if (calendar->id == "primary")
                return reply(socket, 403, error(403, "Insufficient Permission"));
            if (!events && r.method == "GET")
                return reply(socket, 200, QJsonObject{{"id", calendar->id}, {"summary", calendar->summary}});
            if (events && eventId.isEmpty() && r.method == "GET") {
                if (param("showDeleted") != "true")
                    return reply(socket, 400, error(400, "test expects showDeleted=true"));
                qint64 since = 0;
                if (query.hasQueryItem("syncToken")) {
                    since = param("syncToken").mid(1).toLongLong();
                    if (since < minSyncToken)
                        return reply(socket, 410, error(410, "Sync token is no longer valid, a full sync is required."));
                }
                QList<QJsonObject> all;
                for (const QJsonObject &e : calendar->events)
                    if (e.value("seq").toInteger() > since)
                        all << e;
                std::sort(all.begin(), all.end(), [](const QJsonObject &a, const QJsonObject &b) {
                    return a.value("seq").toInteger() < b.value("seq").toInteger();
                });
                // Pages of two, so paging is always exercised.
                const int offset = param("pageToken").toInt();
                QJsonArray items;
                for (int i = offset; i < qMin(offset + 2, all.size()); ++i)
                    items.append(publicView(all[i]));
                QJsonObject page{{"items", items}};
                if (offset + 2 < all.size())
                    page.insert("nextPageToken", QString::number(offset + 2));
                else
                    page.insert("nextSyncToken", QStringLiteral("s%1").arg(m_seq));
                return reply(socket, 200, page);
            }
            if (events && eventId.isEmpty() && r.method == "POST") {
                const QString id = body.value("id").toString();
                static const QRegularExpression base32hex("^[a-v0-9]{5,1024}$");
                if (!base32hex.match(id).hasMatch())
                    return reply(socket, 400, error(400, "Invalid resource id value."));
                if (calendar->events.contains(id))
                    return reply(socket, 409, error(409, "The requested identifier already exists."));
                QJsonObject e = body;
                if (!e.contains("status"))
                    e.insert("status", "confirmed");
                e = stamp(e, {});
                calendar->events.insert(id, e);
                return reply(socket, 200, publicView(e));
            }
            if (!calendar->events.contains(eventId))
                return reply(socket, 404, error(404, "Not Found"));
            QJsonObject &e = calendar->events[eventId];
            if (r.method == "PATCH") {
                for (auto it = body.begin(); it != body.end(); ++it)
                    e.insert(it.key(), it.value());
                e = stamp(e, {});
                return reply(socket, 200, publicView(e));
            }
            if (r.method == "DELETE") {
                if (e.value("status").toString() == "cancelled")
                    return reply(socket, 410, error(410, "Resource has been deleted"));
                e.insert("status", "cancelled");
                e = stamp(e, {});
                return reply(socket, 204);
            }
        }

        // ----- Tasks -----
        if (path == "/tasks/v1/users/@me/lists" && r.method == "GET") {
            QJsonArray items;
            for (const TaskList &l : lists)
                items.append(QJsonObject{{"id", l.id}, {"title", l.title}});
            return reply(socket, 200, QJsonObject{{"items", items}});
        }
        if (path == "/tasks/v1/users/@me/lists" && r.method == "POST") {
            const QString id = QStringLiteral("list%1").arg(++m_counter);
            lists.append(TaskList{id, body.value("title").toString(), {}});
            return reply(socket, 200, QJsonObject{{"id", id}, {"title", body.value("title")}});
        }
        static const QRegularExpression listPath("^/tasks/v1/users/@me/lists/([^/]+)$");
        if (const auto m = listPath.match(path); m.hasMatch()) {
            for (const TaskList &l : lists)
                if (l.id == m.captured(1))
                    return reply(socket, 200, QJsonObject{{"id", l.id}, {"title", l.title}});
            return reply(socket, 404, error(404, "Not Found"));
        }
        static const QRegularExpression tasksPath("^/tasks/v1/lists/([^/]+)/tasks(?:/([^/]+))?$");
        if (const auto m = tasksPath.match(path); m.hasMatch()) {
            TaskList *list = nullptr;
            for (TaskList &l : lists)
                if (l.id == m.captured(1))
                    list = &l;
            if (!list)
                return reply(socket, 404, error(404, "Not Found"));
            if (list->id == "MyTasksId")
                return reply(socket, 400, error(400, "test: the user's own list must not be touched"));
            const QString taskId = m.captured(2);
            if (taskId.isEmpty() && r.method == "GET") {
                const QDateTime since = QDateTime::fromString(param("updatedMin"), Qt::ISODateWithMs);
                QList<QJsonObject> all;
                for (const QJsonObject &t : list->tasks) {
                    if (since.isValid() && QDateTime::fromString(t.value("updated").toString(), Qt::ISODateWithMs) < since)
                        continue;
                    if (t.value("deleted").toBool() && param("showDeleted") != "true")
                        continue;
                    if (t.value("hidden").toBool() && param("showHidden") != "true")
                        continue;
                    if (t.value("status").toString() == "completed" && param("showCompleted") == "false")
                        continue;
                    all << t;
                }
                std::sort(all.begin(), all.end(), [](const QJsonObject &a, const QJsonObject &b) {
                    return a.value("seq").toInteger() < b.value("seq").toInteger();
                });
                const int offset = param("pageToken").toInt();
                QJsonArray items;
                for (int i = offset; i < qMin(offset + 2, all.size()); ++i)
                    items.append(publicView(all[i]));
                QJsonObject page{{"items", items}};
                if (offset + 2 < all.size())
                    page.insert("nextPageToken", QString::number(offset + 2));
                return reply(socket, 200, page);
            }
            if (taskId.isEmpty() && r.method == "POST") {
                const QString id = QStringLiteral("task%1").arg(++m_counter);
                QJsonObject t = body;
                t.insert("id", id);
                t = stamp(t, {});
                list->tasks.insert(id, t);
                return reply(socket, 200, publicView(t));
            }
            if (!list->tasks.contains(taskId) || list->tasks[taskId].value("deleted").toBool())
                return reply(socket, 404, error(404, "Not Found"));
            QJsonObject &t = list->tasks[taskId];
            if (r.method == "PATCH") {
                for (auto it = body.begin(); it != body.end(); ++it) {
                    if (it.value().isNull())
                        t.remove(it.key());
                    else
                        t.insert(it.key(), it.value());
                }
                t = stamp(t, {});
                return reply(socket, 200, publicView(t));
            }
            if (r.method == "DELETE") {
                t.insert("deleted", true);
                t = stamp(t, {});
                return reply(socket, 204);
            }
        }
        reply(socket, 404, error(404, "fake: no such endpoint " + path));
    }

    QThread m_thread;
    QObject m_worker;
    QTcpServer *m_server = nullptr;
    quint16 m_port = 0;
    QMutex m_mutex;
    qint64 m_seq = 0;
    int m_counter = 0;
};
