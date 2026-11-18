#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QThread>
#include <QTimer>
#include <memory>

#include "googleapi.h"

struct Reminder;
class QNetworkAccessManager;

// Two-way sync of reminder text, due time, and done with Google: timed
// reminders become events (with a pop-up at the due time) in a calendar the
// app creates, untimed or done ones become tasks in a task list of their own.
// Only text, due time, and done ever leave the computer: never audio, clip
// names, or anything else about a voice memo.
namespace Sync {

// What both sides last agreed on for one reminder, keyed by its uid.
struct Link {
    QString kind; // "event" or "task"
    QString id;   // the Google id
    QString text;
    QDateTime due;
    bool done = false;
};

struct State {
    QString calendarId, taskListId, calendarSyncToken;
    QDateTime tasksUpdatedMin;
    QHash<QString, Link> links;

    static QString path(const QString &dataDirectory);
    bool load(const QString &dataDirectory);
    bool save(const QString &dataDirectory) const;
};

// The last outcome, for the overlay and `omni-reminder sync status`.
struct Status {
    QString state; // ok, offline, failed, needs-login, not-configured, never
    QString message;
    QDateTime lastAttempt, lastSuccess, nextRetry;
    int pending = 0;
    int pulled = 0, pushed = 0;

    static QString path(const QString &dataDirectory);
    static Status load(const QString &dataDirectory);
    bool save(const QString &dataDirectory) const;
    QJsonObject toJson() const;
};

// Which side a reminder lives on at Google.
QString kindFor(const Reminder &reminder);
// Changes waiting to go up: reminders never sent, changed since, or removed.
int pendingCount(const QString &dataDirectory);

// One sync pass, start to end. Blocking; run it on a worker thread or in a
// command-line process. Holds sync.lock throughout so two processes never
// sync at once.
class Engine {
public:
    Engine(const QString &dataDirectory, const Google::Endpoints &endpoints, const QString &clientPath = {},
           const QString &tokenMode = {});
    ~Engine();

    Status run();

    // Every request body sent, for the tests' "no clip data leaves" check.
    QList<QByteArray> sentBodies;

private:
    struct Failure {
        QString state, message;
    };
    struct Remote {
        QString kind, id, text;
        QDateTime due, updated;
        bool done = false, deleted = false;
    };

    Google::Response api(const QByteArray &method, const QString &url, const QJsonObject &body = {},
                         bool hasBody = false);
    void ensureToken();
    [[noreturn]] void fail(const Google::Response &response, const QString &what);
    void ensureCalendar();
    void ensureTaskList();
    QList<Remote> pullEvents();
    QList<Remote> pullTasks();
    void merge(const QList<Remote> &remote);
    void push();
    QString create(const QString &kind, const Reminder &reminder);
    bool update(const Link &link, const Reminder &reminder);
    void removeRemote(const Link &link);
    QJsonObject eventBody(const Reminder &reminder) const;
    QJsonObject taskBody(const Reminder &reminder, bool forPatch) const;

    QString m_data;
    Google::Endpoints m_endpoints;
    QString m_clientPath, m_tokenMode;
    Google::Client m_client;
    QString m_refreshToken, m_accessToken;
    QDateTime m_accessExpires;
    std::unique_ptr<QNetworkAccessManager> m_network;
    State m_state;
    int m_pulled = 0, m_pushed = 0;
    QString m_timeZone;
    // Where the next pass reads from; stored only after a merge succeeded.
    QString m_pendingSyncToken;
    QDateTime m_pendingTasksMin;
};

// Lives in the daemon: runs passes on a worker thread when something changed
// here (after a short pause), every few minutes, and on request, and backs
// off when Google cannot be reached. Local data never waits on it.
class Controller : public QObject {
    Q_OBJECT

public:
    explicit Controller(const QString &dataDirectory, QObject *parent = nullptr);
    ~Controller() override;

    void localChanged(); // debounced
    void syncNow();
    const Status &status() const { return m_status; }
    static QString describe(const Status &status);

signals:
    void statusChanged(const QString &line, bool problem);
    void passStarted();
    void passFinished();

private:
    void start();
    void finished(const Status &status);

    QString m_data;
    QThread m_thread;
    QObject *m_worker = nullptr;
    Engine *m_engine = nullptr; // used and deleted on the worker thread only
    QTimer m_debounce, m_next;
    bool m_running = false, m_again = false;
    int m_failures = 0;
    Status m_status;
};

} // namespace Sync
