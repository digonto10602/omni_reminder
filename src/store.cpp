#include "store.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>

QString Store::isoString(const QDateTime &time) {
    return time.isValid() ? time.toUTC().toString(Qt::ISODateWithMs) : QString();
}

// Accepts any ISO 8601 form Qt reads; one without an offset is local time.
QDateTime Store::parseIso(const QString &text) {
    if (text.isEmpty())
        return {};
    QDateTime time = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!time.isValid())
        time = QDateTime::fromString(text, Qt::ISODate);
    return time;
}

QString Store::newUid() {
    static const char alphabet[] = "0123456789abcdefghijklmnopqrstuv";
    QString uid;
    for (int i = 0; i < 32; ++i)
        uid += QChar(alphabet[QRandomGenerator::system()->bounded(32)]);
    return uid;
}

QJsonObject Reminder::toJson() const {
    QJsonObject object{{"id", id},
                       {"uid", uid},
                       {"text", text},
                       {"created", Store::isoString(created)},
                       {"due", due.isValid() ? QJsonValue(Store::isoString(due)) : QJsonValue()},
                       {"notified", notified},
                       {"clip", clip.isEmpty() ? QJsonValue() : QJsonValue(clip)},
                       {"clipExpired", clipExpired},
                       {"done", done}};
    if (updated.isValid())
        object.insert("updated", Store::isoString(updated));
    if (clipRecorded.isValid())
        object.insert("clipRecorded", Store::isoString(clipRecorded));
    if (clipSeconds > 0)
        object.insert("clipSeconds", clipSeconds);
    return object;
}

Reminder Reminder::fromJson(const QJsonObject &object) {
    Reminder r;
    r.id = object.value("id").toInt();
    r.text = object.value("text").toString();
    r.created = Store::parseIso(object.value("created").toString());
    r.due = Store::parseIso(object.value("due").toString());
    r.notified = object.value("notified").toBool();
    r.clip = object.value("clip").toString();
    r.clipRecorded = Store::parseIso(object.value("clipRecorded").toString());
    r.clipSeconds = object.value("clipSeconds").toDouble();
    r.clipExpired = object.value("clipExpired").toBool();
    r.uid = object.value("uid").toString();
    r.done = object.value("done").toBool();
    r.updated = Store::parseIso(object.value("updated").toString());
    return r;
}

Store::Store(const QString &directory) : m_directory(directory.isEmpty() ? defaultDirectory() : directory) {}

// OMNI_REMINDER_DATA_DIR moves everything elsewhere, for tests and trials.
QString Store::defaultDirectory() {
    const QString overridden = qEnvironmentVariable("OMNI_REMINDER_DATA_DIR");
    if (!overridden.isEmpty())
        return overridden;
    QString base = qEnvironmentVariable("XDG_DATA_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + "/.local/share";
    return base + "/omni-reminder";
}

bool Store::ensureDirectories(QString *error) {
    if (QDir().mkpath(clipsDirectory()))
        return true;
    if (error)
        *error = QStringLiteral("Cannot create %1").arg(clipsDirectory());
    return false;
}

bool Store::load(QString *error) {
    reminders.clear();
    upgraded = false;
    nextId = 1;
    QFile file(filePath());
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Cannot read %1: %2").arg(filePath(), file.errorString());
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QStringLiteral("%1 is not valid JSON (%2); fix or move it, it will not be overwritten")
                         .arg(filePath(), parseError.errorString());
        return false;
    }
    const QJsonObject root = document.object();
    int highest = 0;
    for (const QJsonValue &value : root.value("reminders").toArray()) {
        Reminder reminder = Reminder::fromJson(value.toObject());
        if (reminder.id <= 0)
            continue;
        // Files from 0.1 have neither; the next save keeps what is made here.
        if (reminder.uid.isEmpty()) {
            reminder.uid = newUid();
            upgraded = true;
        }
        if (!reminder.updated.isValid()) {
            reminder.updated = reminder.created.isValid() ? reminder.created : QDateTime::currentDateTimeUtc();
            upgraded = true;
        }
        highest = qMax(highest, reminder.id);
        reminders.append(reminder);
    }
    nextId = qMax(root.value("nextId").toInt(1), highest + 1);
    return true;
}

bool Store::save(QString *error) {
    if (!ensureDirectories(error))
        return false;
    QJsonArray array;
    for (const Reminder &reminder : reminders)
        array.append(reminder.toJson());
    const QJsonObject root{{"version", 1}, {"nextId", nextId}, {"reminders", array}};

    QSaveFile file(filePath());
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Cannot write %1: %2").arg(filePath(), file.errorString());
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("Cannot save %1: %2").arg(filePath(), file.errorString());
        return false;
    }
    return true;
}

bool Store::update(const std::function<bool(Store &)> &change, QString *error) {
    if (!ensureDirectories(error))
        return false;
    // The lock covers read-change-write, so a command-line add while the
    // daemon saves cannot drop either change. A lock left by a crashed
    // process is taken over once it is stale.
    QLockFile lock(m_directory + "/reminders.lock");
    lock.setStaleLockTime(10000);
    if (!lock.tryLock(5000)) {
        if (error)
            *error = QStringLiteral("Another omni-reminder process is holding %1/reminders.lock").arg(m_directory);
        return false;
    }
    if (!load(error))
        return false;
    if (!change(*this))
        return true;
    return save(error);
}

Reminder *Store::find(int id) {
    for (Reminder &reminder : reminders)
        if (reminder.id == id)
            return &reminder;
    return nullptr;
}

int Store::add(Reminder reminder) {
    reminder.id = nextId++;
    if (!reminder.created.isValid())
        reminder.created = QDateTime::currentDateTime();
    if (reminder.uid.isEmpty())
        reminder.uid = newUid();
    if (!reminder.updated.isValid())
        reminder.updated = QDateTime::currentDateTimeUtc();
    reminders.append(reminder);
    return reminder.id;
}
