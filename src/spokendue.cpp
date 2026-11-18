#include "spokendue.h"

#include <QHash>
#include <QRegularExpression>
#include <QStringList>
#include <QTimeZone>

#include <algorithm>
#include <initializer_list>
#include <utility>

namespace Schedule {
namespace {

// Bengali has two ways to write some letters (য় as one code point or as য
// plus a dot below); NFC turns both into the same text, for the patterns and
// for the input alike.
QString nfc(const QString &text) {
    return text.normalized(QString::NormalizationForm_C);
}

// Word edges. Bengali vowel signs are marks (\p{M}), so a letter or a mark on
// either side means "inside a word": "কাল" (tomorrow) must not match inside
// "সকাল" (morning).
QString before() {
    return QStringLiteral("(?<![\\p{L}\\p{M}\\p{N}])");
}
QString after() {
    return QStringLiteral("(?![\\p{L}\\p{M}\\p{N}])");
}

QRegularExpression rx(const QString &pattern) {
    return QRegularExpression(nfc(before() + "(?:" + pattern + ")" + after()),
                              QRegularExpression::CaseInsensitiveOption |
                                  QRegularExpression::UseUnicodePropertiesOption);
}

// Number words and their values, with a regex alternation that tries the
// longest word first ("দুই" before "দু", "forty five" before "forty").
struct Words {
    QHash<QString, int> values;
    QString pattern;

    Words(std::initializer_list<std::pair<const char *, int>> list) {
        QStringList keys;
        for (const auto &[word, value] : list) {
            const QString key = nfc(QString::fromUtf8(word));
            values.insert(key, value);
            keys << key;
        }
        std::sort(keys.begin(), keys.end(), [](const QString &a, const QString &b) { return a.size() > b.size(); });
        for (QString &key : keys)
            key = QRegularExpression::escape(key).replace("\\ ", "[\\s-]+");
        pattern = keys.join('|');
    }
    int value(const QString &token) const {
        bool ok = false;
        const int number = token.toInt(&ok);
        if (ok)
            return number;
        QString key = nfc(token).toLower();
        key.replace(QRegularExpression("[\\s-]+"), " ");
        return values.value(key, -1);
    }
};

const Words &englishAmounts() {
    static const Words words{{"a", 1},       {"an", 1},         {"one", 1},         {"two", 2},
                             {"three", 3},   {"four", 4},       {"five", 5},        {"six", 6},
                             {"seven", 7},   {"eight", 8},      {"nine", 9},        {"ten", 10},
                             {"eleven", 11}, {"twelve", 12},    {"fifteen", 15},    {"twenty", 20},
                             {"thirty", 30}, {"forty five", 45}, {"forty", 40},     {"fifty", 50},
                             {"sixty", 60},  {"ninety", 90}};
    return words;
}

const Words &englishHours() {
    static const Words words{{"one", 1},  {"two", 2},   {"three", 3},   {"four", 4},
                             {"five", 5}, {"six", 6},   {"seven", 7},   {"eight", 8},
                             {"nine", 9}, {"ten", 10},  {"eleven", 11}, {"twelve", 12}};
    return words;
}

const Words &englishMinutes() {
    static const Words words{{"oh five", 5},  {"ten", 10},     {"fifteen", 15},     {"twenty", 20},
                             {"twenty five", 25}, {"thirty", 30}, {"forty", 40}, {"forty five", 45},
                             {"fifty", 50}};
    return words;
}

const Words &bengaliAmounts() {
    static const Words words{{"এক", 1},     {"দুই", 2},     {"দু", 2},       {"তিন", 3},     {"চার", 4},
                             {"পাঁচ", 5},    {"ছয়", 6},      {"ছ", 6},        {"সাত", 7},     {"আট", 8},
                             {"নয়", 9},     {"দশ", 10},     {"এগারো", 11},   {"বারো", 12},   {"পনেরো", 15},
                             {"পনের", 15},  {"বিশ", 20},    {"কুড়ি", 20},    {"পঁচিশ", 25},  {"ত্রিশ", 30},
                             {"তিরিশ", 30}, {"চল্লিশ", 40}, {"পঁয়তাল্লিশ", 45}, {"পঞ্চাশ", 50}};
    return words;
}

// Hours as they come before the "টা" of "পাঁচটা" (five o'clock).
const Words &bengaliHours() {
    static const Words words{{"এক", 1}, {"দুই", 2}, {"দু", 2},  {"তিন", 3},  {"চার", 4},    {"পাঁচ", 5},
                             {"ছয়", 6}, {"ছ", 6},   {"সাত", 7}, {"আট", 8},   {"নয়", 9},     {"ন", 9},
                             {"দশ", 10}, {"এগারো", 11}, {"বারো", 12}};
    return words;
}

int englishMonth(const QString &word) {
    static const QStringList names{"jan", "feb", "mar", "apr", "may", "jun",
                                   "jul", "aug", "sep", "oct", "nov", "dec"};
    return int(names.indexOf(word.left(3).toLower())) + 1;
}

const QString &englishMonths() {
    static const QString names = "january|february|march|april|may|june|july|august|september|october|november|december"
                                 "|jan|feb|mar|apr|jun|jul|aug|sept|sep|oct|nov|dec";
    return names;
}

const QHash<QString, int> &bengaliMonthValues() {
    static const QHash<QString, int> months = [] {
        QHash<QString, int> m;
        const std::initializer_list<std::pair<const char *, int>> list{
            {"জানুয়ারি", 1}, {"জানুয়ারী", 1}, {"ফেব্রুয়ারি", 2}, {"ফেব্রুয়ারী", 2}, {"মার্চ", 3},
            {"এপ্রিল", 4},  {"মে", 5},       {"জুন", 6},         {"জুলাই", 7},       {"আগস্ট", 8},
            {"অগাস্ট", 8},  {"অগস্ট", 8},    {"সেপ্টেম্বর", 9},    {"অক্টোবর", 10},     {"নভেম্বর", 11},
            {"ডিসেম্বর", 12}};
        for (const auto &[name, value] : list)
            m.insert(nfc(QString::fromUtf8(name)), value);
        return m;
    }();
    return months;
}

const QString &bengaliMonths() {
    static const QString names = [] {
        QStringList keys = bengaliMonthValues().keys();
        std::sort(keys.begin(), keys.end(), [](const QString &a, const QString &b) { return a.size() > b.size(); });
        return keys.join('|');
    }();
    return names;
}

// Bengali ordinal endings on a day of the month: ১লা, ২রা, ৪ঠা, ৫ই, ২১শে.
const QString bengaliOrdinal = QStringLiteral("(?:লা|রা|ঠা|ই|শে|এ)?");

enum class Part { None, Dawn, Morning, Midday, Afternoon, LateAfternoon, Evening, Night, Tonight };

Part englishPart(const QString &word) {
    if (word == "morning")
        return Part::Morning;
    if (word == "afternoon")
        return Part::Afternoon;
    if (word == "evening")
        return Part::Evening;
    if (word == "night")
        return Part::Night;
    return Part::None;
}

Part bengaliPart(const QString &word) {
    static const QHash<QString, Part> parts = [] {
        QHash<QString, Part> p;
        const std::initializer_list<std::pair<const char *, Part>> list{
            {"ভোর", Part::Dawn},       {"সকাল", Part::Morning},       {"দুপুর", Part::Midday},
            {"বিকাল", Part::LateAfternoon}, {"বিকেল", Part::LateAfternoon}, {"সন্ধ্যা", Part::Evening},
            {"সন্ধ্যে", Part::Evening}, {"রাত", Part::Night}};
        for (const auto &[name, part] : list)
            p.insert(nfc(QString::fromUtf8(name)), part);
        return p;
    }();
    return parts.value(nfc(word), Part::None);
}

QString bengaliParts() {
    return QStringLiteral("ভোর|সকাল|দুপুর|বিকাল|বিকেল|সন্ধ্যা|সন্ধ্যে|রাত");
}

QTime typicalTime(Part part) {
    switch (part) {
    case Part::Dawn: return QTime(6, 0);
    case Part::Morning: return QTime(9, 0);
    case Part::Midday: return QTime(13, 0);
    case Part::Afternoon: return QTime(15, 0);
    case Part::LateAfternoon: return QTime(16, 0);
    case Part::Evening: return QTime(18, 0);
    case Part::Night: return QTime(21, 0);
    case Part::Tonight: return QTime(20, 0);
    case Part::None: break;
    }
    return QTime(9, 0);
}

// An hour said with a part of the day: "5 in the evening" is 17:00. Sets
// *nextDay for the small hours of a night ("রাত ২টা" is 02:00 the next day).
int hourInPart(int hour, Part part, bool *nextDay) {
    switch (part) {
    case Part::Midday:
        return hour <= 6 ? hour + 12 : hour;
    case Part::Afternoon:
    case Part::LateAfternoon:
    case Part::Evening:
        return hour < 12 ? hour + 12 : hour;
    case Part::Night:
    case Part::Tonight:
        if (hour == 12) {
            *nextDay = true;
            return 0;
        }
        if (hour < 6) {
            *nextDay = true;
            return hour;
        }
        return hour < 12 ? hour + 12 : hour;
    case Part::Dawn:
    case Part::Morning:
        return hour == 12 ? 0 : hour;
    case Part::None:
        break;
    }
    return hour;
}

// The next date with this day of the month, from today on.
QDate nextDayOfMonth(const QDate &today, int day) {
    QDate month(today.year(), today.month(), 1);
    for (int i = 0; i < 13; ++i, month = month.addMonths(1)) {
        const QDate date(month.year(), month.month(), day);
        if (date.isValid() && date >= today)
            return date;
    }
    return {};
}

// A day and month said without a year: this year, or next year once passed.
QDate dayMonth(const QDate &today, int day, int month, int year) {
    if (year > 0)
        return QDate(year, month, day);
    QDate date(today.year(), month, day);
    if (date.isValid() && date < today)
        date = QDate(today.year() + 1, month, day);
    return date;
}

} // namespace

QDateTime findDue(const QString &input, const QDateTime &now, QString *heard) {
    QString text = nfc(input).toLower();
    for (QChar &c : text) {
        const char16_t u = c.unicode();
        if (u >= 0x09E6 && u <= 0x09EF) // Bengali digits ০-৯
            c = QChar(char16_t(u'0' + (u - 0x09E6)));
    }
    static const QRegularExpression dottedMeridiem(R"(([ap])\.\s?m\.?)");
    text.replace(dottedMeridiem, "\\1m");
    text.replace(QChar(0x2019), '\''); // o’clock

    QStringList words;
    auto note = [&](const QRegularExpressionMatch &match) { words << match.captured(0).simplified(); };
    auto finish = [&](const QDateTime &due) {
        if (heard)
            *heard = due.isValid() ? words.join(QStringLiteral(" … ")) : QString();
        return due;
    };

    const QDate today = now.date();
    const Words &enAmount = englishAmounts();
    const Words &bnAmount = bengaliAmounts();

    // ---------- "in 10 minutes", "১০ মিনিট পরে": from now ----------
    QDate date;
    static const QRegularExpression halfHour = rx("in\\s+half\\s+an\\s+hour|আধা?\\s+ঘণ্টা\\s+পরে?|আধা?\\s+ঘন্টা\\s+পরে?");
    if (const auto m = halfHour.match(text); m.hasMatch()) {
        note(m);
        return finish(now.addSecs(30 * 60));
    }
    static const QRegularExpression inEnglish =
        rx("in\\s+(" + enAmount.pattern + "|\\d+)\\s+(minutes?|mins?|hours?|hrs?|days?|weeks?)");
    static const QRegularExpression inBengali =
        rx("(" + bnAmount.pattern + "|\\d+)\\s*(মিনিট|ঘণ্টা|ঘন্টা|দিন|সপ্তাহ)\\s*(?:পরে|পর)");
    for (const QRegularExpression *pattern : {&inEnglish, &inBengali}) {
        const auto m = pattern->match(text);
        if (!m.hasMatch())
            continue;
        const int amount = (pattern == &inEnglish ? enAmount : bnAmount).value(m.captured(1));
        const QString unit = m.captured(2);
        if (amount <= 0)
            continue;
        note(m);
        if (unit.startsWith('m') || unit.startsWith(nfc("মি")))
            return finish(now.addSecs(qint64(amount) * 60));
        if (unit.startsWith('h') || unit.startsWith(nfc("ঘ")))
            return finish(now.addSecs(qint64(amount) * 3600));
        const bool week = unit.startsWith('w') || unit.startsWith(nfc("স"));
        date = today.addDays(qint64(amount) * (week ? 7 : 1));
        break;
    }

    // ---------- the day ----------
    static const QRegularExpression isoDate = rx("(\\d{4})-(\\d{1,2})-(\\d{1,2})");
    static const QRegularExpression dayThenMonth =
        rx("(?:the\\s+)?(\\d{1,2})(?:st|nd|rd|th)?(?:\\s+of)?\\s+(" + englishMonths() + ")\\.?(?:,?\\s+(\\d{4}))?");
    static const QRegularExpression monthThenDay =
        rx("(" + englishMonths() + ")\\.?\\s+(?:the\\s+)?(\\d{1,2})(?:st|nd|rd|th)?(?:,?\\s+(\\d{4}))?");
    static const QRegularExpression bnDayThenMonth = rx("(\\d{1,2})\\s*" + bengaliOrdinal + "\\s*(" + bengaliMonths() +
                                                        ")(?:ের|এর|ে|র)?(?:\\s*,?\\s*(\\d{4})(?:\\s*সালের|\\s*সালে|\\s*সাল)?)?");
    static const QRegularExpression bnMonthThenDay = rx("(" + bengaliMonths() + ")(?:ের|এর|ে|র)?\\s+(\\d{1,2})\\s*" +
                                                        bengaliOrdinal + "(?:\\s*তারিখের|\\s*তারিখে|\\s*তারিখ)?");
    static const QRegularExpression dayOfMonth =
        rx("(?:on\\s+the\\s+(\\d{1,2})(?:st|nd|rd|th))|(?:(\\d{1,2})\\s*" + bengaliOrdinal + "\\s*(?:তারিখের|তারিখে|তারিখ))");

    if (!date.isValid()) {
        if (const auto m = isoDate.match(text); m.hasMatch()) {
            date = QDate(m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt());
            if (date.isValid())
                note(m);
        }
    }
    if (!date.isValid()) {
        if (const auto m = dayThenMonth.match(text); m.hasMatch()) {
            date = dayMonth(today, m.captured(1).toInt(), englishMonth(m.captured(2)), m.captured(3).toInt());
            if (date.isValid())
                note(m);
        }
    }
    if (!date.isValid()) {
        if (const auto m = monthThenDay.match(text); m.hasMatch()) {
            date = dayMonth(today, m.captured(2).toInt(), englishMonth(m.captured(1)), m.captured(3).toInt());
            if (date.isValid())
                note(m);
        }
    }
    if (!date.isValid()) {
        if (const auto m = bnDayThenMonth.match(text); m.hasMatch()) {
            date = dayMonth(today, m.captured(1).toInt(), bengaliMonthValues().value(m.captured(2)), m.captured(3).toInt());
            if (date.isValid())
                note(m);
        }
    }
    if (!date.isValid()) {
        if (const auto m = bnMonthThenDay.match(text); m.hasMatch()) {
            date = dayMonth(today, m.captured(2).toInt(), bengaliMonthValues().value(m.captured(1)), 0);
            if (date.isValid())
                note(m);
        }
    }
    if (!date.isValid()) {
        if (const auto m = dayOfMonth.match(text); m.hasMatch()) {
            const QString day = m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
            date = nextDayOfMonth(today, day.toInt());
            if (date.isValid())
                note(m);
        }
    }

    // Weekdays: the next one to come ("this Friday" on a Friday is today).
    static const QRegularExpression enWeekday =
        rx("(?:(next|this|coming|on)\\s+)?(monday|tuesday|wednesday|thursday|friday|saturday|sunday)");
    static const QRegularExpression bnWeekday =
        rx("(?:(আগামী)\\s+)?(সোম|মঙ্গল|বুধ|বৃহস্পতি|বিষ্যুদ|শুক্র|শনি|রবি|রোব)বার(?:ের|ে|দিন)?");
    if (!date.isValid()) {
        static const QStringList english{"monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"};
        static const QStringList bengali = [] {
            QStringList days;
            for (const char *day : {"সোম", "মঙ্গল", "বুধ", "বৃহস্পতি", "শুক্র", "শনি", "রবি"})
                days << nfc(QString::fromUtf8(day));
            return days;
        }();
        int weekday = 0;
        bool thisOne = false;
        if (const auto m = enWeekday.match(text); m.hasMatch()) {
            weekday = int(english.indexOf(m.captured(2))) + 1;
            thisOne = m.captured(1) == "this";
            note(m);
        } else if (const auto b = bnWeekday.match(text); b.hasMatch()) {
            QString day = b.captured(2);
            if (day == nfc("বিষ্যুদ"))
                day = nfc("বৃহস্পতি");
            else if (day == nfc("রোব"))
                day = nfc("রবি");
            weekday = int(bengali.indexOf(day)) + 1;
            note(b);
        }
        if (weekday > 0) {
            int ahead = (weekday - today.dayOfWeek() + 7) % 7;
            if (ahead == 0 && !thisOne)
                ahead = 7;
            date = today.addDays(ahead);
        }
    }

    Part part = Part::None;
    static const QRegularExpression dayWord = rx(
        "(?:the\\s+)?day\\s+after\\s+tomorrow|tomorrow|today|tonight|পরশুদিন|পরশু|আগামীকালকের|আগামীকালকে|আগামীকাল"
        "|কালকের|কালকে|কাল|আজকের|আজকে|আজ");
    static const QRegularExpression nextWeek = rx("next\\s+week|আগামী\\s+সপ্তাহে|আগামী\\s+সপ্তাহ");
    if (!date.isValid()) {
        if (const auto m = dayWord.match(text); m.hasMatch()) {
            const QString word = m.captured(0);
            if (word.contains("after") || word.startsWith(nfc("পরশু")))
                date = today.addDays(2);
            else if (word == "tomorrow" || word.contains(nfc("কাল")))
                date = today.addDays(1);
            else
                date = today;
            if (word == "tonight")
                part = Part::Tonight;
            note(m);
        } else if (const auto w = nextWeek.match(text); w.hasMatch()) {
            date = today.addDays(7);
            note(w);
        }
    } else if (const auto m = dayWord.match(text); m.hasMatch() && m.captured(0) == "tonight") {
        part = Part::Tonight;
    }

    // ---------- the part of the day ----------
    static const QRegularExpression enPart = rx("(?:(this)\\s+)?(morning|afternoon|evening|night)");
    static const QRegularExpression bnPart = rx("(" + bengaliParts() + ")(?:ের|ে|বেলায়|বেলা|য়)?");
    if (part == Part::None) {
        if (const auto m = enPart.match(text); m.hasMatch()) {
            part = englishPart(m.captured(2));
            if (!m.captured(1).isEmpty() && !date.isValid())
                date = today;
            note(m);
        } else if (const auto b = bnPart.match(text); b.hasMatch()) {
            part = bengaliPart(b.captured(1));
            note(b);
        }
    }

    // ---------- the time ----------
    int hour = -1, minute = 0;
    bool exact = false; // said with am/pm or as 13:00 and later: no guessing
    const Words &enHour = englishHours();
    const Words &enMinute = englishMinutes();
    const Words &bnHour = bengaliHours();

    static const QRegularExpression withMeridiem = rx("(\\d{1,2})(?:[:.](\\d{2}))?\\s*(am|pm)");
    static const QRegularExpression wordsWithMeridiem =
        rx("(" + enHour.pattern + ")(?:[\\s-]+(" + enMinute.pattern + "))?\\s*(am|pm|o'?\\s?clock)");
    static const QRegularExpression pastOrTo =
        rx("(half|(?:a\\s+)?quarter)\\s+(past|to)\\s+(\\d{1,2}|" + enHour.pattern + ")");
    static const QRegularExpression oClock = rx("(\\d{1,2})\\s*o'?\\s?clock");
    static const QRegularExpression noonWord = rx("noon|midday|midnight");
    static const QRegularExpression atNumber = rx("(?:at|by|around|before)\\s+(\\d{1,2})(?::(\\d{2}))?");
    static const QRegularExpression atWords =
        rx("(?:at|by|around|before)\\s+(" + enHour.pattern + ")(?:[\\s-]+(" + enMinute.pattern + "))?");
    static const QRegularExpression clock = rx("(\\d{1,2}):(\\d{2})");
    static const QRegularExpression hourInThe = rx("(\\d{1,2}|" + enHour.pattern +
                                                   ")(?::(\\d{2}))?\\s+(?:in\\s+the|at)\\s+(morning|afternoon|evening|night)");
    // Bengali: "৫টায়", "সাড়ে পাঁচটায়", "বিকেল ৫টা", "৫টা ৩০ মিনিটে", "দেড়টায়".
    static const QRegularExpression bnHalves = rx("(দেড়|আড়াই)\\s*(?:টা|টো)(?:য়|র|তে)?");
    static const QRegularExpression bnClock =
        rx("(?:(" + bengaliParts() + ")(?:ের|ে|বেলা)?\\s+)?(?:(সাড়ে|সোয়া|পৌনে)\\s*)?(" + bnHour.pattern +
           "|\\d{1,2})\\s*(?:টা|টো)(য়|তে|র\\s+দিকে|র\\s+সময়)?(\\s*বাজে)?(?:\\s*(?:বেজে\\s*)?(\\d{1,2}|" +
           bnAmount.pattern + ")\\s*মিনিট(?:ে)?)?");

    if (const auto m = withMeridiem.match(text); m.hasMatch()) {
        hour = m.captured(1).toInt();
        minute = m.captured(2).toInt();
        if (hour >= 1 && hour <= 12) {
            hour = hour % 12 + (m.captured(3) == "pm" ? 12 : 0);
            exact = true;
            note(m);
        } else {
            hour = -1;
        }
    }
    if (hour < 0) {
        if (const auto m = pastOrTo.match(text); m.hasMatch()) {
            const int h = enHour.value(m.captured(3));
            if (h >= 1 && h <= 12) {
                const int minutes = m.captured(1) == "half" ? 30 : 15;
                if (m.captured(2) == "past") {
                    hour = h;
                    minute = minutes;
                } else {
                    hour = h == 1 ? 12 : h - 1;
                    minute = 60 - minutes;
                }
                note(m);
            }
        }
    }
    if (hour < 0) {
        if (const auto m = wordsWithMeridiem.match(text); m.hasMatch()) {
            hour = enHour.value(m.captured(1));
            minute = m.captured(2).isEmpty() ? 0 : enMinute.value(m.captured(2));
            const QString suffix = m.captured(3);
            if (suffix == "am" || suffix == "pm") {
                hour = hour % 12 + (suffix == "pm" ? 12 : 0);
                exact = true;
            }
            note(m);
        }
    }
    if (hour < 0) {
        if (const auto m = oClock.match(text); m.hasMatch() && m.captured(1).toInt() <= 12) {
            hour = m.captured(1).toInt();
            note(m);
        }
    }
    if (hour < 0) {
        if (const auto m = noonWord.match(text); m.hasMatch()) {
            if (m.captured(0) == "midnight") {
                hour = 0;
                if (date.isValid())
                    date = date.addDays(1);
            } else {
                hour = 12;
            }
            exact = true;
            note(m);
        }
    }
    if (hour < 0) {
        if (const auto m = bnHalves.match(text); m.hasMatch()) {
            hour = m.captured(1) == nfc("দেড়") ? 1 : 2;
            minute = 30;
            note(m);
        }
    }
    if (hour < 0) {
        // "পাঁচটা" alone is also "five (things)", so it counts as a time only
        // with "য়" (at), a part of the day, "সাড়ে" and the like, or "বাজে".
        auto it = bnClock.globalMatch(text);
        while (it.hasNext()) {
            const auto m = it.next();
            if (m.captured(1).isEmpty() && m.captured(2).isEmpty() && m.captured(4).isEmpty() && m.captured(5).isEmpty())
                continue;
            const int h = bnHour.value(m.captured(3));
            if (h < 0 || h > 12)
                continue;
            hour = h;
            const QString half = m.captured(2);
            if (half == nfc("সাড়ে")) {
                minute = 30;
            } else if (half == nfc("সোয়া")) {
                minute = 15;
            } else if (half == nfc("পৌনে")) {
                hour = h == 1 ? 12 : h - 1;
                minute = 45;
            } else if (!m.captured(6).isEmpty()) {
                minute = bnAmount.value(m.captured(6));
            }
            if (!m.captured(1).isEmpty())
                part = bengaliPart(m.captured(1));
            note(m);
            break;
        }
    }
    if (hour < 0) {
        if (const auto m = hourInThe.match(text); m.hasMatch()) {
            hour = enHour.value(m.captured(1));
            minute = m.captured(2).toInt();
            part = englishPart(m.captured(3));
            note(m);
        }
    }
    if (hour < 0) {
        if (const auto m = atNumber.match(text); m.hasMatch()) {
            hour = m.captured(1).toInt();
            minute = m.captured(2).toInt();
            exact = hour > 12 || hour == 0;
            note(m);
        }
    }
    if (hour < 0) {
        if (const auto m = atWords.match(text); m.hasMatch()) {
            hour = enHour.value(m.captured(1));
            minute = m.captured(2).isEmpty() ? 0 : enMinute.value(m.captured(2));
            note(m);
        }
    }
    if (hour < 0) {
        if (const auto m = clock.match(text); m.hasMatch()) {
            hour = m.captured(1).toInt();
            minute = m.captured(2).toInt();
            exact = hour > 12 || hour == 0;
            note(m);
        }
    }
    if (hour > 23 || minute < 0 || minute > 59)
        hour = -1;

    if (!date.isValid() && hour < 0 && part == Part::None)
        return finish({});

    // ---------- put it together ----------
    const QTimeZone zone = now.timeZone();
    if (hour < 0) {
        const QTime time = part == Part::None ? QTime(9, 0) : typicalTime(part);
        if (date.isValid())
            return finish(QDateTime(date, time, zone));
        QDateTime at(today, time, zone);
        return finish(at > now ? at : at.addDays(1));
    }

    bool nextDay = false;
    if (!exact && part != Part::None)
    {
        hour = hourInPart(hour, part, &nextDay);
        exact = true;
    }

    if (date.isValid()) {
        if (!exact && hour >= 1 && hour <= 6)
            hour += 12; // "Friday at 3" is the afternoon
        return finish(QDateTime(nextDay ? date.addDays(1) : date, QTime(hour, minute), zone));
    }
    if (exact) {
        QDateTime at(today, QTime(hour, minute), zone);
        return finish(at > now ? at : at.addDays(1));
    }
    // "at 9" with no day: the next 9 o'clock to come, morning or evening.
    for (const int h : {hour % 12, hour % 12 + 12}) {
        const QDateTime at(today, QTime(h == 0 && hour == 12 ? 12 : h, minute), zone);
        if (at > now)
            return finish(at);
    }
    return finish(QDateTime(today.addDays(1), QTime(hour <= 6 ? hour + 12 : hour, minute), zone));
}

} // namespace Schedule
