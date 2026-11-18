#include "cli.h"

#include "googleapi.h"
#include "googlesync.h"
#include "ipc.h"
#include "schedule.h"
#include "store.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QUrl>
#include <QProcess>
#include <QThread>
#include <cstdio>

namespace Cli {

static void out(const QString &text) {
    fputs(qPrintable(text + "\n"), stdout);
}

static int fail(const QString &text) {
    fputs(qPrintable("omni-reminder: " + text + "\n"), stderr);
    return 1;
}

void printUsage(bool toStderr) {
    const QString usage = QStringLiteral(
        "Usage: omni-reminder <command>\n"
        "\n"
        "  open                       show the reminders overlay (starts the daemon if needed)\n"
        "  daemon                     run in the background: shortcut, recording, due notifications\n"
        "  key-down | key-up          the global shortcut's press and release (tap: overlay, hold: voice memo)\n"
        "  list [--json]              print all reminders\n"
        "  add TEXT [--due TIME] [--json]\n"
        "                             add a reminder; TIME is ISO 8601 (2026-12-01T14:00) or\n"
        "                             18:30, tomorrow 9:00, +2h, in 10 min\n"
        "  remove ID                  remove a reminder and its voice clip\n"
        "  sync login                 connect a Google account (Calendar for timed reminders, Tasks for the rest)\n"
        "  sync logout                disconnect it and forget the sign-in\n"
        "  sync status [--json]       show the last sync, errors, and changes waiting\n"
        "  sync now                   sync with Google right away\n"
        "  skill install              install a short agent skill to ~/.claude/skills/omni-reminder\n"
        "  help | --help | version\n"
        "\n"
        "Data: %1\n").arg(Store::defaultDirectory());
    fputs(qPrintable(usage), toStderr ? stderr : stdout);
}

bool isCliCommand(const QString &command) {
    static const QStringList commands{"list", "add", "remove", "key-down", "key-up", "skill",
                                      "help", "--help", "-h", "version", "--version", "stop", "toggle", "sync"};
    return commands.contains(command);
}

static QJsonObject publicJson(const Store &store, const Reminder &r) {
    QJsonObject object = r.toJson();
    object.insert("clipPath", r.clip.isEmpty() ? QJsonValue() : QJsonValue(store.clipPath(r.clip)));
    if (r.due.isValid())
        object.insert("dueLocal", r.due.toLocalTime().toString(Qt::ISODate));
    return object;
}

static int list(bool json) {
    Store store;
    QString error;
    if (!store.load(&error))
        return fail(error);
    if (json) {
        QJsonArray array;
        for (const Reminder &r : store.reminders)
            array.append(publicJson(store, r));
        out(QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Indented)).trimmed());
        return 0;
    }
    if (store.reminders.isEmpty()) {
        out("No reminders.");
        return 0;
    }
    for (const Reminder &r : store.reminders) {
        const QString due = r.due.isValid() ? r.due.toLocalTime().toString("yyyy-MM-dd HH:mm") : QString("-");
        const QString clip = !r.clip.isEmpty() ? QStringLiteral(" [voice %1 s]").arg(r.clipSeconds, 0, 'f', 1)
                             : r.clipExpired ? QStringLiteral(" [clip expired]") : QString();
        out(QStringLiteral("%1\t%2\t%3%4").arg(r.id).arg(due, r.text.simplified(), clip));
    }
    return 0;
}

static int add(const QStringList &args) {
    QString text, dueText;
    bool json = false;
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == "--due") {
            if (i + 1 >= args.size())
                return fail("--due needs a time");
            dueText = args[++i];
        } else if (args[i].startsWith("--due=")) {
            dueText = args[i].mid(6);
        } else if (args[i] == "--json") {
            json = true;
        } else if (text.isEmpty()) {
            text = args[i];
        } else {
            text += " " + args[i];
        }
    }
    if (text.trimmed().isEmpty())
        return fail("add needs the reminder text");
    bool ok = false;
    const QDateTime due = Schedule::parseDue(dueText, QDateTime::currentDateTime(), &ok);
    if (!ok)
        return fail(QStringLiteral("cannot read \"%1\" as a time (try 2026-12-01T14:00, 18:30, +2h)").arg(dueText));

    Store store;
    QString error;
    Reminder added;
    if (!store.update([&](Store &s) {
            Reminder r;
            r.text = text.trimmed();
            r.due = due;
            const int id = s.add(r);
            added = *s.find(id);
            return true;
        }, &error))
        return fail(error);
    Ipc::send({{"cmd", "changed"}}, nullptr, 500); // a running daemon syncs it soon
    if (json)
        out(QString::fromUtf8(QJsonDocument(publicJson(store, added)).toJson(QJsonDocument::Compact)));
    else
        out(QString::number(added.id));
    return 0;
}

static int remove(const QStringList &args) {
    if (args.size() != 1)
        return fail("remove needs exactly one ID");
    bool isNumber = false;
    const int id = args[0].toInt(&isNumber);
    if (!isNumber || id <= 0)
        return fail(QStringLiteral("\"%1\" is not a reminder ID").arg(args[0]));
    Store store;
    QString error, clip;
    bool found = false;
    if (!store.update([&](Store &s) {
            for (int i = 0; i < s.reminders.size(); ++i) {
                if (s.reminders[i].id == id) {
                    clip = s.reminders[i].clip;
                    s.reminders.removeAt(i);
                    found = true;
                    return true;
                }
            }
            return false;
        }, &error))
        return fail(error);
    if (!found)
        return fail(QStringLiteral("no reminder with ID %1").arg(id));
    if (!clip.isEmpty())
        QFile::remove(store.clipPath(clip));
    Ipc::send({{"cmd", "changed"}}, nullptr, 500);
    return 0;
}

// ---------- Google sync ----------

static QString localTime(const QDateTime &time) {
    return time.isValid() ? time.toLocalTime().toString("yyyy-MM-dd HH:mm:ss") : QStringLiteral("never");
}

static int syncLogin() {
    Google::Client client;
    QString error;
    if (!client.load(Google::Client::defaultPath(), &error))
        return fail(error);
    const Google::Tokens tokens = Google::login(
        Google::Endpoints::fromEnvironment(), client,
        [](const QUrl &url) {
            const QString opener = qEnvironmentVariable("OMNI_REMINDER_BROWSER", "xdg-open");
            return QProcess::startDetached(opener, {url.toString(QUrl::FullyEncoded)});
        },
        [](const QString &line) { out(line); });
    if (!tokens.ok() || !tokens.error.isEmpty() || tokens.refresh.isEmpty()) {
        if (!tokens.refresh.isEmpty()) {
            QNetworkAccessManager network;
            Google::revoke(network, Google::Endpoints::fromEnvironment(), tokens.refresh);
        }
        return fail(tokens.error.isEmpty() ? QStringLiteral("sign-in failed") : tokens.error);
    }
    Google::TokenStore store;
    if (!store.save(tokens.refresh, &error))
        return fail(error);
    out(QStringLiteral("Signed in. The sign-in is kept in: %1").arg(store.location()));
    if (Ipc::send({{"cmd", "sync-now"}}, nullptr, 1000)) {
        out("The running omni-reminder is syncing now.");
        return 0;
    }
    out("Syncing once now (the daemon is not running)...");
    const Sync::Status status = Sync::Engine(Store::defaultDirectory(), Google::Endpoints::fromEnvironment()).run();
    out(status.state == "ok" ? QStringLiteral("Synced.") : QStringLiteral("Sync: %1 %2").arg(status.state, status.message));
    return status.state == "ok" ? 0 : 1;
}

static int syncLogout() {
    Google::TokenStore store;
    const QString token = store.load();
    if (!token.isEmpty()) {
        // Best effort: tell Google to drop it too; it is forgotten here either way.
        QNetworkAccessManager network;
        Google::revoke(network, Google::Endpoints::fromEnvironment(), token);
    }
    store.clear();
    out(token.isEmpty() ? QStringLiteral("Not signed in.")
                        : QStringLiteral("Signed out. Reminders stay here; what is at Google stays there."));
    Ipc::send({{"cmd", "sync-now"}}, nullptr, 1000); // the overlay shows the new state
    return 0;
}

static int syncStatus(bool json) {
    const QString data = Store::defaultDirectory();
    Google::Client client;
    QString clientError;
    const bool configured = client.load(Google::Client::defaultPath(), &clientError);
    Google::TokenStore tokens;
    const QString location = tokens.location();
    Sync::Status status = Sync::Status::load(data);
    status.pending = Sync::pendingCount(data);
    const bool daemon = Ipc::send({{"cmd", "ping"}}, nullptr, 500);
    if (json) {
        QJsonObject object = status.toJson();
        object.insert("configured", configured);
        object.insert("clientFile", Google::Client::defaultPath());
        object.insert("signedIn", !location.isEmpty());
        object.insert("tokenLocation", location);
        object.insert("daemonRunning", daemon);
        out(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented)).trimmed());
        return 0;
    }
    out(QStringLiteral("OAuth client:   %1").arg(configured ? Google::Client::defaultPath() : clientError));
    out(QStringLiteral("Signed in:      %1").arg(location.isEmpty() ? QStringLiteral("no (run: omni-reminder sync login)")
                                                                     : "yes, kept in " + location));
    out(QStringLiteral("Last result:    %1%2").arg(status.state, status.message.isEmpty() ? QString() : " - " + status.message));
    out(QStringLiteral("Last attempt:   %1").arg(localTime(status.lastAttempt)));
    out(QStringLiteral("Last success:   %1").arg(localTime(status.lastSuccess)));
    if (status.nextRetry.isValid() && daemon)
        out(QStringLiteral("Next try:       %1").arg(localTime(status.nextRetry)));
    out(QStringLiteral("Waiting to go:  %1 change(s)").arg(status.pending));
    out(QStringLiteral("Daemon:         %1").arg(daemon ? "running (syncs every 3 minutes and after changes)"
                                                         : "not running (only `sync now` syncs)"));
    return 0;
}

static int syncNow() {
    const Sync::Status status = Sync::Engine(Store::defaultDirectory(), Google::Endpoints::fromEnvironment()).run();
    Ipc::send({{"cmd", "reload"}}, nullptr, 500); // the overlay shows what arrived
    if (status.state == "ok") {
        out(QStringLiteral("Synced: %1 change(s) from Google, %2 sent.").arg(status.pulled).arg(status.pushed));
        return 0;
    }
    return fail(QStringLiteral("sync %1: %2").arg(status.state, status.message));
}

static int sync(const QStringList &args) {
    const QString what = args.value(0);
    if (what == "login")
        return syncLogin();
    if (what == "logout")
        return syncLogout();
    if (what == "status")
        return syncStatus(args.contains("--json"));
    if (what == "now")
        return syncNow();
    return fail("usage: omni-reminder sync login | logout | status [--json] | now");
}

static int installSkill() {
    QFile source(":/skill/SKILL.md");
    if (!source.open(QIODevice::ReadOnly))
        return fail("the bundled skill is missing");
    const QString directory = QDir::homePath() + "/.claude/skills/omni-reminder";
    if (!QDir().mkpath(directory))
        return fail("cannot create " + directory);
    QFile target(directory + "/SKILL.md");
    if (!target.open(QIODevice::WriteOnly) || target.write(source.readAll()) < 0)
        return fail("cannot write " + target.fileName());
    out(target.fileName());
    return 0;
}

int poke(const QString &command, qint64 startedMs, bool startDaemon) {
    QJsonObject request{{"cmd", command}, {"t", double(startedMs)}};
    QJsonObject reply;
    if (Ipc::send(request, &reply))
        return reply.value("ok").toBool() ? 0 : fail(reply.value("error").toString());
    if (startDaemon) {
        // The shortcut should work even if autostart did not run.
        if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), {"daemon"}))
            return fail("the daemon is not running and could not be started");
    }
    QElapsedTimer waited;
    waited.start();
    while (waited.elapsed() < 4000) {
        QThread::msleep(100);
        if (Ipc::send(request, &reply))
            return reply.value("ok").toBool() ? 0 : fail(reply.value("error").toString());
    }
    return fail(QStringLiteral("the daemon is not running (no answer on %1); start it with: omni-reminder daemon")
                    .arg(Ipc::socketName()));
}

int run(const QStringList &arguments, qint64 startedMs) {
    const QString command = arguments.value(0);
    const QStringList rest = arguments.mid(1);
    if (command == "help" || command == "--help" || command == "-h") {
        printUsage(false);
        return 0;
    }
    if (command == "version" || command == "--version") {
        out(QCoreApplication::applicationVersion());
        return 0;
    }
    if (command == "list")
        return list(rest.contains("--json"));
    if (command == "add")
        return add(rest);
    if (command == "remove")
        return remove(rest);
    if (command == "key-down")
        return poke("key-down", startedMs, true);
    if (command == "key-up")
        return poke("key-up", startedMs, false);
    if (command == "stop" || command == "toggle")
        return poke(command, startedMs, false);
    if (command == "skill" && rest.value(0) == "install")
        return installSkill();
    if (command == "sync")
        return sync(rest);
    printUsage(true);
    return 1;
}

} // namespace Cli
