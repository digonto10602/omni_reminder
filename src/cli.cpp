#include "cli.h"

#include "ipc.h"
#include "schedule.h"
#include "store.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
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
        "  skill install              install a short agent skill to ~/.claude/skills/omni-reminder\n"
        "  help | --help | version\n"
        "\n"
        "Data: %1\n").arg(Store::defaultDirectory());
    fputs(qPrintable(usage), toStderr ? stderr : stdout);
}

bool isCliCommand(const QString &command) {
    static const QStringList commands{"list", "add", "remove", "key-down", "key-up", "skill",
                                      "help", "--help", "-h", "version", "--version", "stop", "toggle"};
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
    return 0;
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
    printUsage(true);
    return 1;
}

} // namespace Cli
