#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

struct Reminder;

// When reminders fall due, and how the user writes a due time. Pure functions
// of the reminders and "now", so tests can use fake clocks.
namespace Schedule {

// Reminders with a time at or before `now` whose notification has not been
// shown yet. After a reboot this is every overdue reminder, once.
QList<int> dueIds(const QList<Reminder> &reminders, const QDateTime &now);

// Milliseconds until the next un-notified due time, capped so that a suspend
// or a clock change is noticed within `cap`. -1 when nothing is waiting.
qint64 msUntilNext(const QList<Reminder> &reminders, const QDateTime &now, qint64 cap = 60000);

// Reads a due time the way people type it:
//   ""                       no due time (ok, invalid result)
//   2026-12-01T14:00, 2026-12-01 14:00, 2026-12-01 (09:00)   ISO 8601
//   14:00, 9:30, 9            today, or tomorrow when that time has passed
//   tomorrow, tomorrow 8:15   (tomorrow alone is 09:00)
//   today 18:00
//   +10m, +2h, +1d, in 10 min, in 2 hours, in 3 days
// Returns an invalid time and sets *ok to false when it cannot read it.
QDateTime parseDue(const QString &text, const QDateTime &now, bool *ok);

} // namespace Schedule
