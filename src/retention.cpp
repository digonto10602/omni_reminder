#include "retention.h"

#include "store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <algorithm>

namespace Retention {

QStringList plan(QList<Clip> clips, const QDateTime &now, qint64 maxAgeSeconds, qint64 maxTotalBytes) {
    std::stable_sort(clips.begin(), clips.end(), [](const Clip &a, const Clip &b) {
        return a.recorded < b.recorded;
    });

    QStringList doomed;
    QList<Clip> kept;
    for (const Clip &clip : clips) {
        // A clip without a known time counts as recorded now: it is never
        // aged out by mistake, and it still counts toward the size cap.
        const QDateTime recorded = clip.recorded.isValid() ? clip.recorded : now;
        if (recorded.secsTo(now) >= maxAgeSeconds)
            doomed << clip.name;
        else
            kept << clip;
    }

    qint64 total = 0;
    for (const Clip &clip : kept)
        total += clip.bytes;
    // Oldest first until the rest fit under the cap.
    for (const Clip &clip : kept) {
        if (total <= maxTotalBytes)
            break;
        doomed << clip.name;
        total -= clip.bytes;
    }
    return doomed;
}

Result enforce(Store &store, const QDateTime &now, const QSet<QString> &keep, qint64 maxAgeSeconds,
               qint64 maxTotalBytes) {
    Result result;
    QString error;
    const bool ok = store.update([&](Store &s) {
        bool changed = false;
        QHash<QString, Reminder *> owners;
        for (Reminder &reminder : s.reminders)
            if (!reminder.clip.isEmpty())
                owners.insert(reminder.clip, &reminder);

        const QDir directory(s.clipsDirectory());
        QList<Clip> clips;
        for (const QFileInfo &info : directory.entryInfoList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name)) {
            const QString name = info.fileName();
            if (name.startsWith('.')) {
                // A recording in progress, or one a crash left half-written
                // (a recording is at most minutes long, so hours means dead).
                if (info.lastModified().secsTo(now) > 6 * 3600 && QFile::remove(info.absoluteFilePath()))
                    result.deleted << name;
                continue;
            }
            if (!owners.contains(name) && !keep.contains(name)) {
                // A leftover from a crash or a removed reminder.
                if (QFile::remove(info.absoluteFilePath()))
                    result.deleted << name;
                continue;
            }
            Clip clip{name, {}, info.size()};
            if (Reminder *owner = owners.value(name))
                clip.recorded = owner->clipRecorded;
            if (!clip.recorded.isValid())
                clip.recorded = info.lastModified();
            clips << clip;
            result.bytesBefore += clip.bytes;
        }

        result.bytesAfter = result.bytesBefore;
        for (const QString &name : plan(clips, now, maxAgeSeconds, maxTotalBytes)) {
            const QString path = s.clipPath(name);
            const qint64 bytes = QFileInfo(path).size();
            if (!QFile::remove(path)) {
                result.error = QStringLiteral("Cannot delete %1").arg(path);
                continue;
            }
            result.deleted << name;
            result.bytesAfter -= bytes;
        }

        // Any reminder whose clip is no longer on disk has expired, whether
        // retention just took it or it vanished some other way.
        for (Reminder &reminder : s.reminders) {
            if (!reminder.clip.isEmpty() && !QFileInfo::exists(s.clipPath(reminder.clip))) {
                reminder.clip.clear();
                reminder.clipExpired = true;
                result.expired << reminder.id;
                changed = true;
            }
        }
        return changed;
    }, &error);
    if (!ok) {
        result.ok = false;
        result.error = error;
    }
    return result;
}

} // namespace Retention
