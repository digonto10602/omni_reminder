#include "schedule.h"

#include "store.h"

#include <QRegularExpression>
#include <QTimeZone>

namespace Schedule {

QList<int> dueIds(const QList<Reminder> &reminders, const QDateTime &now) {
    QList<int> ids;
    for (const Reminder &reminder : reminders)
        if (reminder.due.isValid() && !reminder.notified && !reminder.done && reminder.due <= now)
            ids << reminder.id;
    return ids;
}

qint64 msUntilNext(const QList<Reminder> &reminders, const QDateTime &now, qint64 cap) {
    qint64 best = -1;
    for (const Reminder &reminder : reminders) {
        if (!reminder.due.isValid() || reminder.notified || reminder.done)
            continue;
        const qint64 wait = qMax<qint64>(0, now.msecsTo(reminder.due));
        if (best < 0 || wait < best)
            best = wait;
    }
    return best < 0 ? -1 : qMin(best, cap);
}

static bool readClock(const QString &text, QTime *time) {
    static const QRegularExpression clock(R"(^(\d{1,2})(?::(\d{2}))?\s*(am|pm)?$)",
                                          QRegularExpression::CaseInsensitiveOption);
    const auto match = clock.match(text.trimmed());
    if (!match.hasMatch())
        return false;
    int hour = match.captured(1).toInt();
    const int minute = match.captured(2).isEmpty() ? 0 : match.captured(2).toInt();
    const QString half = match.captured(3).toLower();
    if (!half.isEmpty()) {
        if (hour < 1 || hour > 12)
            return false;
        hour = hour % 12 + (half == "pm" ? 12 : 0);
    }
    *time = QTime(hour, minute);
    return time->isValid();
}

QDateTime parseDue(const QString &input, const QDateTime &now, bool *ok) {
    *ok = true;
    const QString text = input.trimmed().toLower();
    if (text.isEmpty())
        return {};

    static const QRegularExpression relative(
        R"(^(?:\+|in\s+)(\d+)\s*(m|min|mins|minute|minutes|h|hr|hrs|hour|hours|d|day|days)$)");
    if (const auto match = relative.match(text); match.hasMatch()) {
        const qint64 amount = match.captured(1).toLongLong();
        const QChar unit = match.captured(2).at(0);
        if (unit == 'd')
            return now.addDays(amount);
        return now.addSecs(amount * (unit == 'h' ? 3600 : 60));
    }

    QTime clock;
    if (text.startsWith("tomorrow") || text.startsWith("today")) {
        const bool tomorrow = text.startsWith("tomorrow");
        const QString rest = text.mid(tomorrow ? 8 : 5).trimmed();
        if (rest.isEmpty() && tomorrow)
            clock = QTime(9, 0);
        else if (!readClock(rest, &clock)) {
            *ok = false;
            return {};
        }
        return QDateTime(now.date().addDays(tomorrow ? 1 : 0), clock, now.timeZone());
    }

    if (readClock(text, &clock)) {
        QDateTime at(now.date(), clock, now.timeZone());
        if (at <= now)
            at = at.addDays(1);
        return at;
    }

    // ISO 8601, with "T" or a space between date and time; a date alone is 09:00.
    QString iso = input.trimmed();
    static const QRegularExpression dateSpaceTime(R"(^(\d{4}-\d{2}-\d{2})\s+(\d{1,2}:\d{2}(?::\d{2})?)$)");
    if (const auto match = dateSpaceTime.match(iso); match.hasMatch())
        iso = match.captured(1) + "T" + match.captured(2).rightJustified(5, '0');
    static const QRegularExpression dateOnly(R"(^\d{4}-\d{2}-\d{2}$)");
    if (dateOnly.match(iso).hasMatch()) {
        const QDate date = QDate::fromString(iso, Qt::ISODate);
        if (date.isValid())
            return QDateTime(date, QTime(9, 0), now.timeZone());
    }
    const QDateTime parsed = Store::parseIso(iso);
    if (parsed.isValid())
        return parsed;

    *ok = false;
    return {};
}

} // namespace Schedule
