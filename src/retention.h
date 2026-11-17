#pragma once

#include <QDateTime>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

class Store;

// Voice clips are kept for a limited time and a limited total size: each clip
// goes 30 days after it was recorded, and when all clips together pass
// 100 MB the oldest go first until the rest fit. The reminders stay; they just
// lose their audio ("clip expired").
namespace Retention {

constexpr qint64 MaxAgeSeconds = 30LL * 24 * 60 * 60;
constexpr qint64 MaxTotalBytes = 100LL * 1000 * 1000;

struct Clip {
    QString name;
    QDateTime recorded;
    qint64 bytes = 0;
};

// The clips to delete, oldest first. Pure, so tests can pass fake times.
QStringList plan(QList<Clip> clips, const QDateTime &now, qint64 maxAgeSeconds = MaxAgeSeconds,
                 qint64 maxTotalBytes = MaxTotalBytes);

struct Result {
    QStringList deleted;   // clip files removed
    QList<int> expired;    // reminders that lost their clip
    qint64 bytesBefore = 0;
    qint64 bytesAfter = 0;
    QString error;
    bool ok = true;
};

// Applies the plan to a store's clips/ directory inside one store
// transaction. Files whose names start with "." are recordings in progress
// and are left alone. A clip file no reminder points to is deleted unless it
// is in `keep` (the daemon keeps clips that an undo could bring back).
// Reminders whose clip file is gone are marked expired.
Result enforce(Store &store, const QDateTime &now, const QSet<QString> &keep = {},
               qint64 maxAgeSeconds = MaxAgeSeconds, qint64 maxTotalBytes = MaxTotalBytes);

} // namespace Retention
