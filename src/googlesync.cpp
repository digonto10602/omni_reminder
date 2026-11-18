#include "googlesync.h"

#include "store.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QSaveFile>
#include <QSet>
#include <QTimeZone>
#include <QUrl>

namespace Sync {

static const char *ListTitle = "Omni Reminder";

// ---------- state and status files ----------

QString State::path(const QString &dataDirectory) {
    return dataDirectory + "/sync.json";
}

static bool writeJson(const QString &path, const QJsonObject &object) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return file.commit();
}

static QJsonObject readJson(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

bool State::load(const QString &dataDirectory) {
    const QJsonObject root = readJson(path(dataDirectory));
    calendarId = root.value("calendarId").toString();
    taskListId = root.value("taskListId").toString();
    calendarSyncToken = root.value("calendarSyncToken").toString();
    tasksUpdatedMin = Store::parseIso(root.value("tasksUpdatedMin").toString());
    links.clear();
    const QJsonObject all = root.value("links").toObject();
    for (auto it = all.begin(); it != all.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        links.insert(it.key(), Link{o.value("kind").toString(), o.value("id").toString(), o.value("text").toString(),
                                    Store::parseIso(o.value("due").toString()), o.value("done").toBool()});
    }
    return true;
}

bool State::save(const QString &dataDirectory) const {
    QJsonObject all;
    for (auto it = links.begin(); it != links.end(); ++it)
        all.insert(it.key(), QJsonObject{{"kind", it->kind},
                                         {"id", it->id},
                                         {"text", it->text},
                                         {"due", Store::isoString(it->due)},
                                         {"done", it->done}});
    return writeJson(path(dataDirectory), QJsonObject{{"calendarId", calendarId},
                                                      {"taskListId", taskListId},
                                                      {"calendarSyncToken", calendarSyncToken},
                                                      {"tasksUpdatedMin", Store::isoString(tasksUpdatedMin)},
                                                      {"links", all}});
}

QString Status::path(const QString &dataDirectory) {
    return dataDirectory + "/sync-status.json";
}

Status Status::load(const QString &dataDirectory) {
    const QJsonObject o = readJson(path(dataDirectory));
    Status s;
    s.state = o.value("state").toString("never");
    s.message = o.value("message").toString();
    s.lastAttempt = Store::parseIso(o.value("lastAttempt").toString());
    s.lastSuccess = Store::parseIso(o.value("lastSuccess").toString());
    s.nextRetry = Store::parseIso(o.value("nextRetry").toString());
    s.pending = o.value("pending").toInt();
    return s;
}

QJsonObject Status::toJson() const {
    auto iso = [](const QDateTime &t) { return t.isValid() ? QJsonValue(Store::isoString(t)) : QJsonValue(); };
    return {{"state", state},           {"message", message},       {"lastAttempt", iso(lastAttempt)},
            {"lastSuccess", iso(lastSuccess)}, {"nextRetry", iso(nextRetry)}, {"pending", pending},
            {"pulled", pulled},         {"pushed", pushed}};
}

bool Status::save(const QString &dataDirectory) const {
    return writeJson(path(dataDirectory), toJson());
}

// ---------- comparing the two sides ----------

QString kindFor(const Reminder &reminder) {
    return reminder.due.isValid() && !reminder.done ? QStringLiteral("event") : QStringLiteral("task");
}

// Tasks carry only a date, so for them a due time matches by day.
static bool sameValues(const QString &kind, const QString &textA, const QDateTime &dueA, bool doneA,
                       const QString &textB, const QDateTime &dueB, bool doneB) {
    if (textA != textB || doneA != doneB || dueA.isValid() != dueB.isValid())
        return false;
    if (!dueA.isValid())
        return true;
    if (kind == "task")
        return dueA.toLocalTime().date() == dueB.toLocalTime().date();
    return dueA.toSecsSinceEpoch() == dueB.toSecsSinceEpoch();
}

static bool matchesLink(const Link &link, const Reminder &r) {
    return link.kind == kindFor(r) && sameValues(link.kind, link.text, link.due, link.done, r.text, r.due, r.done);
}

static Link linkFor(const QString &kind, const QString &id, const Reminder &r) {
    return Link{kind, id, r.text, r.due, r.done};
}

int pendingCount(const QString &dataDirectory) {
    Store store(dataDirectory);
    State state;
    if (!store.load() || !state.load(dataDirectory))
        return 0;
    int pending = 0;
    QSet<QString> seen;
    for (const Reminder &r : store.reminders) {
        seen.insert(r.uid);
        if (!state.links.contains(r.uid) || !matchesLink(state.links.value(r.uid), r))
            ++pending;
    }
    for (auto it = state.links.begin(); it != state.links.end(); ++it)
        if (!seen.contains(it.key()))
            ++pending;
    return pending;
}

// ---------- the engine ----------

Engine::Engine(const QString &dataDirectory, const Google::Endpoints &endpoints, const QString &clientPath,
               const QString &tokenMode)
    : m_data(dataDirectory), m_endpoints(endpoints),
      m_clientPath(clientPath.isEmpty() ? Google::Client::defaultPath() : clientPath), m_tokenMode(tokenMode) {
    m_timeZone = QString::fromUtf8(QTimeZone::systemTimeZoneId());
}

Engine::~Engine() = default;

Status Engine::run() {
    Status status = Status::load(m_data);
    status.lastAttempt = QDateTime::currentDateTimeUtc();
    status.nextRetry = {};
    m_pulled = m_pushed = 0;

    QLockFile lock(m_data + "/sync.lock");
    lock.setStaleLockTime(10 * 60 * 1000);
    if (!lock.tryLock(60000)) {
        status.state = "failed";
        status.message = QStringLiteral("another sync is still running");
        return status;
    }

    QString error;
    if (!m_client.load(m_clientPath, &error)) {
        status.state = "not-configured";
        status.message = error;
        status.pending = pendingCount(m_data);
        status.save(m_data);
        return status;
    }
    const QString refreshToken = Google::TokenStore(m_tokenMode).load();
    if (refreshToken.isEmpty()) {
        status.state = "needs-login";
        status.message = QStringLiteral("not signed in to Google");
        status.pending = pendingCount(m_data);
        status.save(m_data);
        return status;
    }
    if (refreshToken != m_refreshToken) {
        m_refreshToken = refreshToken;
        m_accessToken.clear();
    }
    if (!m_network)
        m_network = std::make_unique<QNetworkAccessManager>();

    // A file from 0.1 gets its new ids saved first, so they stay the same
    // from one pass to the next.
    QString upgradeError;
    if (!Store(m_data).update([](Store &s) { return s.upgraded; }, &upgradeError)) {
        status.state = "failed";
        status.message = upgradeError;
        status.save(m_data);
        return status;
    }

    m_state.load(m_data);
    try {
        ensureCalendar();
        ensureTaskList();
        const QList<Remote> remote = pullEvents() + pullTasks();
        merge(remote);
        push();
        m_state.save(m_data);
        status.state = "ok";
        status.message.clear();
        status.lastSuccess = QDateTime::currentDateTimeUtc();
    } catch (const Failure &failure) {
        // Whatever finished before the failure is kept; the rest is retried.
        m_state.save(m_data);
        status.state = failure.state;
        status.message = failure.message;
    }
    status.pulled = m_pulled;
    status.pushed = m_pushed;
    status.pending = pendingCount(m_data);
    status.save(m_data);
    return status;
}

void Engine::ensureToken() {
    if (!m_accessToken.isEmpty() && QDateTime::currentDateTimeUtc() < m_accessExpires)
        return;
    const Google::Tokens tokens = Google::refresh(*m_network, m_endpoints, m_client, m_refreshToken);
    if (tokens.ok()) {
        m_accessToken = tokens.access;
        m_accessExpires = tokens.expires;
        return;
    }
    if (tokens.offline)
        throw Failure{"offline", tokens.error};
    if (tokens.invalidGrant)
        throw Failure{"needs-login", QStringLiteral("Google no longer accepts the sign-in (%1)").arg(tokens.error)};
    throw Failure{"failed", tokens.error};
}

Google::Response Engine::api(const QByteArray &method, const QString &url, const QJsonObject &body, bool hasBody) {
    const QByteArray bytes = hasBody ? QJsonDocument(body).toJson(QJsonDocument::Compact) : QByteArray();
    if (hasBody)
        sentBodies.append(bytes);
    for (int attempt = 0; attempt < 2; ++attempt) {
        ensureToken();
        const Google::Response response = Google::send(*m_network, method, QUrl(url), bytes,
                                                       hasBody ? QByteArray("application/json") : QByteArray(),
                                                       m_accessToken.toUtf8());
        if (response.status != 401)
            return response;
        // The access token ran out early or was revoked: refresh once.
        m_accessToken.clear();
    }
    throw Failure{"needs-login", QStringLiteral("Google refused the sign-in; run omni-reminder sync login")};
}

void Engine::fail(const Google::Response &response, const QString &what) {
    if (response.status == 0)
        throw Failure{"offline", response.networkError};
    if (response.status >= 500 || response.status == 429)
        throw Failure{"offline", QStringLiteral("Google is busy (%1) while trying to %2").arg(response.status).arg(what)};
    const QJsonObject error = response.json().value("error").toObject();
    const QString detail = error.value("message").toString();
    throw Failure{"failed", QStringLiteral("could not %1: %2 %3").arg(what).arg(response.status).arg(detail).trimmed()};
}

static QString enc(const QString &id) {
    return QString::fromUtf8(QUrl::toPercentEncoding(id));
}

void Engine::ensureCalendar() {
    if (!m_state.calendarId.isEmpty()) {
        const auto got = api("GET", m_endpoints.calendar + "/calendars/" + enc(m_state.calendarId));
        if (got.ok())
            return;
        if (got.status != 404 && got.status != 410)
            fail(got, "reach the Omni Reminder calendar");
        // It was deleted (or belongs to another account): start a new one,
        // and every timed reminder goes up again.
        m_state.calendarId.clear();
        m_state.calendarSyncToken.clear();
        for (auto it = m_state.links.begin(); it != m_state.links.end();)
            it = it->kind == "event" ? m_state.links.erase(it) : std::next(it);
    }
    const auto made = api("POST", m_endpoints.calendar + "/calendars",
                          QJsonObject{{"summary", ListTitle}, {"timeZone", m_timeZone}}, true);
    if (!made.ok())
        fail(made, "create the Omni Reminder calendar");
    m_state.calendarId = made.json().value("id").toString();
    m_state.save(m_data); // never create it twice
}

void Engine::ensureTaskList() {
    if (!m_state.taskListId.isEmpty()) {
        const auto got = api("GET", m_endpoints.tasks + "/users/@me/lists/" + enc(m_state.taskListId));
        if (got.ok())
            return;
        if (got.status != 404)
            fail(got, "reach the Omni Reminder task list");
        m_state.taskListId.clear();
    }
    // Pick up our list if it exists (after a reinstall), else make it.
    QString pageToken;
    do {
        const auto lists = api("GET", m_endpoints.tasks + "/users/@me/lists?maxResults=100" +
                                          (pageToken.isEmpty() ? QString() : "&pageToken=" + enc(pageToken)));
        if (!lists.ok())
            fail(lists, "list task lists");
        for (const QJsonValue &v : lists.json().value("items").toArray())
            if (v.toObject().value("title").toString() == ListTitle && m_state.taskListId.isEmpty())
                m_state.taskListId = v.toObject().value("id").toString();
        pageToken = lists.json().value("nextPageToken").toString();
    } while (!pageToken.isEmpty() && m_state.taskListId.isEmpty());
    if (m_state.taskListId.isEmpty()) {
        const auto made = api("POST", m_endpoints.tasks + "/users/@me/lists", QJsonObject{{"title", ListTitle}}, true);
        if (!made.ok())
            fail(made, "create the Omni Reminder task list");
        m_state.taskListId = made.json().value("id").toString();
    }
    m_state.tasksUpdatedMin = {};
    for (auto it = m_state.links.begin(); it != m_state.links.end();)
        it = it->kind == "task" ? m_state.links.erase(it) : std::next(it);
    m_state.save(m_data);
}

static QDateTime nineOn(const QDate &date) {
    return QDateTime(date, QTime(9, 0));
}

QList<Engine::Remote> Engine::pullEvents() {
    QList<Remote> found;
    bool full = m_state.calendarSyncToken.isEmpty();
    QString pageToken, nextSync;
    const QString base = m_endpoints.calendar + "/calendars/" + enc(m_state.calendarId) + "/events?showDeleted=true&maxResults=250";
    for (;;) {
        QString url = base;
        if (!full)
            url += "&syncToken=" + enc(m_state.calendarSyncToken);
        if (!pageToken.isEmpty())
            url += "&pageToken=" + enc(pageToken);
        const auto page = api("GET", url);
        if (page.status == 410 && !full) {
            // The sync token expired: start over with a full list.
            m_state.calendarSyncToken.clear();
            full = true;
            found.clear();
            pageToken.clear();
            continue;
        }
        if (!page.ok())
            fail(page, "read calendar changes");
        for (const QJsonValue &v : page.json().value("items").toArray()) {
            const QJsonObject e = v.toObject();
            Remote r;
            r.kind = "event";
            r.id = e.value("id").toString();
            r.deleted = e.value("status").toString() == "cancelled";
            r.text = e.value("summary").toString();
            const QJsonObject start = e.value("start").toObject();
            r.due = start.contains("dateTime") ? QDateTime::fromString(start.value("dateTime").toString(), Qt::ISODate)
                                               : nineOn(QDate::fromString(start.value("date").toString(), Qt::ISODate));
            r.updated = QDateTime::fromString(e.value("updated").toString(), Qt::ISODateWithMs);
            found << r;
        }
        pageToken = page.json().value("nextPageToken").toString();
        if (pageToken.isEmpty()) {
            nextSync = page.json().value("nextSyncToken").toString();
            break;
        }
    }
    if (full) {
        // Linked events missing from a full list are gone.
        QSet<QString> present;
        for (const Remote &r : found)
            present.insert(r.id);
        for (const Link &link : m_state.links)
            if (link.kind == "event" && !present.contains(link.id))
                found << Remote{"event", link.id, {}, {}, {}, false, true};
    }
    m_pendingSyncToken = nextSync;
    return found;
}

QList<Engine::Remote> Engine::pullTasks() {
    QList<Remote> found;
    const bool full = !m_state.tasksUpdatedMin.isValid();
    // Read from a little before now next time, so clock skew loses nothing;
    // repeats are harmless, they compare equal.
    m_pendingTasksMin = QDateTime::currentDateTimeUtc().addSecs(-120);
    QString pageToken;
    for (;;) {
        QString url = m_endpoints.tasks + "/lists/" + enc(m_state.taskListId) +
                      "/tasks?maxResults=100&showCompleted=true&showDeleted=true&showHidden=true";
        if (!full)
            url += "&updatedMin=" + enc(m_state.tasksUpdatedMin.toString(Qt::ISODateWithMs));
        if (!pageToken.isEmpty())
            url += "&pageToken=" + enc(pageToken);
        const auto page = api("GET", url);
        if (!page.ok())
            fail(page, "read task changes");
        for (const QJsonValue &v : page.json().value("items").toArray()) {
            const QJsonObject t = v.toObject();
            Remote r;
            r.kind = "task";
            r.id = t.value("id").toString();
            r.deleted = t.value("deleted").toBool();
            r.text = t.value("title").toString();
            r.done = t.value("status").toString() == "completed";
            const QString due = t.value("due").toString();
            if (!due.isEmpty())
                r.due = nineOn(QDate::fromString(due.left(10), Qt::ISODate));
            r.updated = QDateTime::fromString(t.value("updated").toString(), Qt::ISODateWithMs);
            found << r;
        }
        pageToken = page.json().value("nextPageToken").toString();
        if (pageToken.isEmpty())
            break;
    }
    if (full) {
        QSet<QString> present;
        for (const Remote &r : found)
            present.insert(r.id);
        for (const Link &link : m_state.links)
            if (link.kind == "task" && !present.contains(link.id))
                found << Remote{"task", link.id, {}, {}, {}, false, true};
    }
    return found;
}

void Engine::merge(const QList<Remote> &remote) {
    Store store(m_data);
    QString error;
    const bool ok = store.update([&](Store &s) {
        bool changed = false;
        QHash<QString, QString> byRemote; // kind:id -> uid
        for (auto it = m_state.links.begin(); it != m_state.links.end(); ++it)
            byRemote.insert(it->kind + ":" + it->id, it.key());
        auto local = [&](const QString &uid) -> Reminder * {
            for (Reminder &r : s.reminders)
                if (r.uid == uid)
                    return &r;
            return nullptr;
        };
        auto apply = [&](Reminder *r, const Remote &x) {
            if (r->due != x.due)
                r->notified = false;
            r->text = x.text;
            r->due = x.due;
            r->done = x.done;
            r->updated = x.updated.isValid() ? x.updated : QDateTime::currentDateTimeUtc();
            changed = true;
            ++m_pulled;
        };

        for (Remote x : remote) {
            if (x.text.trimmed().isEmpty() && !x.deleted)
                x.text = QStringLiteral("(no title)");
            const QString uid = byRemote.value(x.kind + ":" + x.id);
            Reminder *r = uid.isEmpty() ? nullptr : local(uid);
            // A task's due is a date; keep our time of day when the date agrees.
            if (x.kind == "task" && x.due.isValid() && r && r->due.isValid() &&
                r->due.toLocalTime().date() == x.due.date())
                x.due = r->due;

            if (uid.isEmpty()) {
                if (x.deleted)
                    continue; // our own clean-up, or never ours
                // An event we made whose link was lost: link it again.
                if (x.kind == "event") {
                    if (Reminder *mine = local(x.id)) {
                        m_state.links.insert(mine->uid, Link{"event", x.id, x.text, x.due, x.done});
                        byRemote.insert("event:" + x.id, mine->uid);
                        continue;
                    }
                }
                // New on the phone: it arrives here, without any clip.
                Reminder fresh;
                fresh.text = x.text;
                fresh.due = x.due;
                fresh.done = x.done;
                fresh.updated = x.updated.isValid() ? x.updated : QDateTime::currentDateTimeUtc();
                const int id = s.add(fresh);
                const QString freshUid = s.find(id)->uid;
                m_state.links.insert(freshUid, Link{x.kind, x.id, x.text, x.due, x.done});
                byRemote.insert(x.kind + ":" + x.id, freshUid);
                changed = true;
                ++m_pulled;
                continue;
            }

            Link &link = m_state.links[uid];
            if (x.deleted) {
                // Deleted on the phone. It goes here too, unless it was changed
                // here since the last sync; then the change survives and goes
                // up again as a new item.
                if (r && sameValues(link.kind, link.text, link.due, link.done, r->text, r->due, r->done)) {
                    for (int i = 0; i < s.reminders.size(); ++i)
                        if (s.reminders[i].uid == uid) {
                            s.reminders.removeAt(i);
                            break;
                        }
                    changed = true;
                    ++m_pulled;
                }
                m_state.links.remove(uid);
                byRemote.remove(x.kind + ":" + x.id);
                continue;
            }
            if (sameValues(link.kind, link.text, link.due, link.done, x.text, x.due, x.done))
                continue; // nothing new over there (often our own push)

            if (!r) {
                // Removed here, but edited on the phone since: keep the edit.
                Reminder back;
                back.uid = uid;
                back.text = x.text;
                back.due = x.due;
                back.done = x.done;
                back.updated = x.updated;
                s.add(back);
                link = Link{link.kind, link.id, x.text, x.due, x.done};
                changed = true;
                ++m_pulled;
                continue;
            }
            const bool localChanged = !sameValues(link.kind, link.text, link.due, link.done, r->text, r->due, r->done);
            if (!localChanged || x.updated > r->updated)
                apply(r, x); // the phone's edit is the only one, or the newer one
            // Either way the remote now holds x; push() sends ours if it differs.
            link.text = x.text;
            link.due = x.due;
            link.done = x.done;
        }
        return changed;
    }, &error);
    if (!ok)
        throw Failure{"failed", error};
    // Only now that the changes are safely merged may the cursors move on.
    if (!m_pendingSyncToken.isEmpty())
        m_state.calendarSyncToken = m_pendingSyncToken;
    m_state.tasksUpdatedMin = m_pendingTasksMin;
    m_state.save(m_data);
}

static QString rfc3339(const QDateTime &time) {
    const QDateTime local = time.toLocalTime();
    const int offset = local.offsetFromUtc();
    const QString sign = offset < 0 ? "-" : "+";
    return local.toString("yyyy-MM-ddTHH:mm:ss") + sign +
           QStringLiteral("%1:%2").arg(qAbs(offset) / 3600, 2, 10, QChar('0')).arg(qAbs(offset) % 3600 / 60, 2, 10, QChar('0'));
}

// Only the text and the time. A short, see-through slot with a pop-up at
// the start: it reminds without blocking the day.
QJsonObject Engine::eventBody(const Reminder &r) const {
    const QDateTime start = r.due.toLocalTime();
    return {{"summary", r.text},
            {"start", QJsonObject{{"dateTime", rfc3339(start)}, {"timeZone", m_timeZone}}},
            {"end", QJsonObject{{"dateTime", rfc3339(start.addSecs(15 * 60))}, {"timeZone", m_timeZone}}},
            {"reminders", QJsonObject{{"useDefault", false},
                                      {"overrides", QJsonArray{QJsonObject{{"method", "popup"}, {"minutes", 0}}}}}},
            {"transparency", "transparent"},
            {"status", "confirmed"}};
}

QJsonObject Engine::taskBody(const Reminder &r, bool forPatch) const {
    QJsonObject body{{"title", r.text}, {"status", r.done ? "completed" : "needsAction"}};
    if (r.due.isValid())
        body.insert("due", r.due.toLocalTime().date().toString(Qt::ISODate) + "T00:00:00.000Z");
    else if (forPatch)
        body.insert("due", QJsonValue::Null);
    if (!r.done && forPatch)
        body.insert("completed", QJsonValue::Null);
    return body;
}

QString Engine::create(const QString &kind, const Reminder &r) {
    if (kind == "event") {
        const QString events = m_endpoints.calendar + "/calendars/" + enc(m_state.calendarId) + "/events";
        QJsonObject body = eventBody(r);
        body.insert("id", r.uid); // our own id: a retried insert cannot duplicate
        const auto made = api("POST", events, body, true);
        if (made.ok())
            return made.json().value("id").toString();
        if (made.status == 409) {
            // It exists already (an earlier attempt, or it was here before):
            // bring it back with the current values.
            const auto revived = api("PATCH", events + "/" + enc(r.uid), eventBody(r), true);
            if (revived.ok())
                return r.uid;
            fail(revived, "restore a calendar event");
        }
        fail(made, "create a calendar event");
    }
    const auto made = api("POST", m_endpoints.tasks + "/lists/" + enc(m_state.taskListId) + "/tasks",
                          taskBody(r, false), true);
    if (!made.ok())
        fail(made, "create a task");
    return made.json().value("id").toString();
}

bool Engine::update(const Link &link, const Reminder &r) {
    const auto response =
        link.kind == "event"
            ? api("PATCH", m_endpoints.calendar + "/calendars/" + enc(m_state.calendarId) + "/events/" + enc(link.id),
                  eventBody(r), true)
            : api("PATCH", m_endpoints.tasks + "/lists/" + enc(m_state.taskListId) + "/tasks/" + enc(link.id),
                  taskBody(r, true), true);
    if (response.ok())
        return true;
    if (response.status == 404 || response.status == 410)
        return false;
    fail(response, link.kind == "event" ? "update a calendar event" : "update a task");
}

void Engine::removeRemote(const Link &link) {
    const auto response =
        link.kind == "event"
            ? api("DELETE", m_endpoints.calendar + "/calendars/" + enc(m_state.calendarId) + "/events/" + enc(link.id))
            : api("DELETE", m_endpoints.tasks + "/lists/" + enc(m_state.taskListId) + "/tasks/" + enc(link.id));
    if (response.ok() || response.status == 404 || response.status == 410)
        return;
    fail(response, link.kind == "event" ? "delete a calendar event" : "delete a task");
}

void Engine::push() {
    Store store(m_data);
    QString error;
    if (!store.load(&error))
        throw Failure{"failed", error};
    QSet<QString> seen;
    for (const Reminder &r : store.reminders) {
        seen.insert(r.uid);
        const QString kind = kindFor(r);
        if (!m_state.links.contains(r.uid)) {
            m_state.links.insert(r.uid, linkFor(kind, create(kind, r), r));
            ++m_pushed;
        } else {
            const Link link = m_state.links.value(r.uid);
            if (matchesLink(link, r))
                continue;
            if (link.kind != kind) {
                // Timed <-> untimed (or done): it moves between Calendar and Tasks.
                const QString id = create(kind, r);
                m_state.links.insert(r.uid, linkFor(kind, id, r));
                m_state.save(m_data);
                removeRemote(link);
            } else if (update(link, r)) {
                m_state.links.insert(r.uid, linkFor(kind, link.id, r));
            } else {
                m_state.links.insert(r.uid, linkFor(kind, create(kind, r), r));
            }
            ++m_pushed;
        }
        m_state.save(m_data); // a crash now cannot make a duplicate later
    }
    for (const QString &uid : m_state.links.keys()) {
        if (seen.contains(uid))
            continue;
        removeRemote(m_state.links.value(uid));
        m_state.links.remove(uid);
        m_state.save(m_data);
        ++m_pushed;
    }
}

// ---------- the daemon's side ----------

Controller::Controller(const QString &dataDirectory, QObject *parent) : QObject(parent), m_data(dataDirectory) {
    m_status = Status::load(m_data);
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(3000);
    connect(&m_debounce, &QTimer::timeout, this, &Controller::start);
    m_next.setSingleShot(true);
    connect(&m_next, &QTimer::timeout, this, &Controller::start);
    m_worker = new QObject;
    m_worker->moveToThread(&m_thread);
    m_engine = new Engine(m_data, Google::Endpoints::fromEnvironment());
    // finished is emitted on the worker thread itself: tidy up there.
    connect(&m_thread, &QThread::finished, [this] {
        delete m_engine;
        m_engine = nullptr;
        delete m_worker;
        m_worker = nullptr;
    });
    m_thread.start();
    m_next.start(2000); // first pass shortly after start-up
}

Controller::~Controller() {
    m_thread.quit();
    m_thread.wait(35000);
}

void Controller::localChanged() {
    m_debounce.start();
}

void Controller::syncNow() {
    m_debounce.stop();
    start();
}

void Controller::start() {
    if (m_running) {
        m_again = true;
        return;
    }
    m_running = true;
    m_next.stop();
    emit passStarted();
    // The engine keeps its access token between passes.
    QMetaObject::invokeMethod(m_worker, [this] {
        m_engine->sentBodies.clear();
        const Status status = m_engine->run();
        QMetaObject::invokeMethod(this, [this, status] { finished(status); }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void Controller::finished(const Status &result) {
    m_running = false;
    m_status = result;
    int delayMs;
    if (result.state == "ok") {
        m_failures = 0;
        delayMs = 3 * 60 * 1000;
    } else if (result.state == "offline" || result.state == "failed") {
        // 30 s, 1, 2, 4, 8 minutes, then every 15.
        delayMs = int(qMin<qint64>(30000LL << qMin(m_failures, 5), 15 * 60 * 1000));
        ++m_failures;
    } else {
        delayMs = 15 * 60 * 1000; // nothing to do until someone signs in
    }
    m_status.nextRetry = QDateTime::currentDateTimeUtc().addMSecs(delayMs);
    m_status.save(m_data);
    m_next.start(delayMs);
    const bool problem = result.state != "ok" && result.state != "not-configured";
    emit statusChanged(describe(m_status), problem);
    emit passFinished();
    if (m_again) {
        m_again = false;
        start();
    }
}

QString Controller::describe(const Status &s) {
    const QString retry = s.nextRetry.isValid() ? s.nextRetry.toLocalTime().toString("HH:mm") : QString();
    if (s.state == "ok")
        return s.pending > 0 ? tr("Google: synced %1, %n change(s) waiting", "", s.pending).arg(s.lastSuccess.toLocalTime().toString("HH:mm"))
                             : tr("Google: synced %1").arg(s.lastSuccess.toLocalTime().toString("HH:mm"));
    if (s.state == "offline")
        return tr("Google: offline, %n change(s) waiting, retrying at %1", "", s.pending).arg(retry);
    if (s.state == "failed")
        return tr("Google sync failed: %1. Retrying at %2").arg(s.message, retry);
    if (s.state == "needs-login")
        return tr("Google: not signed in. Run omni-reminder sync login");
    return QString(); // not set up: say nothing
}

} // namespace Sync
