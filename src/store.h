#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <functional>

// One reminder. The text and the optional due time are the user's; the clip
// fields describe the voice memo it was made from, if any. A clip can expire
// (retention deletes the audio) while the reminder itself stays.
struct Reminder {
    int id = 0;
    QString text;
    QDateTime created;
    QDateTime due;            // invalid when the reminder has no time
    bool notified = false;    // the due notification has been shown
    QString clip;             // file name inside clips/, empty when none
    QDateTime clipRecorded;   // when the clip was recorded (retention clock)
    double clipSeconds = 0;
    bool clipExpired = false; // there was a clip, retention removed it

    QJsonObject toJson() const;
    static Reminder fromJson(const QJsonObject &object);
};

// reminders.json and clips/ under one data directory
// (~/.local/share/omni-reminder by default). Every change is a transaction:
// take the lock, re-read the file, change it, and save it atomically, so the
// daemon and the command line can both write without losing each other's
// edits.
class Store {
public:
    explicit Store(const QString &directory = {});

    static QString defaultDirectory();

    QString directory() const { return m_directory; }
    QString clipsDirectory() const { return m_directory + "/clips"; }
    QString filePath() const { return m_directory + "/reminders.json"; }
    QString clipPath(const QString &clip) const { return clipsDirectory() + "/" + clip; }

    // Reads reminders.json. A missing file is an empty list; a file that is
    // not valid JSON is an error and is never overwritten.
    bool load(QString *error = nullptr);
    // Writes beside the target, then renames over it (QSaveFile).
    bool save(QString *error = nullptr);
    // Lock, load, change, save. The change returns false to save nothing.
    bool update(const std::function<bool(Store &)> &change, QString *error = nullptr);

    QList<Reminder> reminders;
    int nextId = 1;

    Reminder *find(int id);
    int add(Reminder reminder); // assigns the id and returns it

    static QString isoString(const QDateTime &time);
    static QDateTime parseIso(const QString &text);

private:
    bool ensureDirectories(QString *error);

    QString m_directory;
};
