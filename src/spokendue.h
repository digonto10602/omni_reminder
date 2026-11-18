#pragma once

#include <QDateTime>
#include <QString>

namespace Schedule {

// Finds a due time inside a sentence, the way Superwhisper writes down what
// was said, in English or Bengali (or both mixed). Unlike parseDue, which
// reads the "When?" field, the date and time may sit anywhere in the text:
//
//   "Call the bank tomorrow at 5 p.m."          tomorrow 17:00
//   "Dentist on the 25th November"              25 Nov, 09:00
//   "Remind me in 10 minutes"                   now + 10 min
//   "Dinner on Friday at 8"                     Friday 20:00
//   "আগামীকাল বিকেল ৫টায় ব্যাংকে ফোন"            tomorrow 17:00
//   "২৫শে নভেম্বর ডাক্তার"                       25 Nov, 09:00
//
// A day without a time is 09:00, as in the "When?" field; a part of the day
// without a time takes a typical hour (morning 9, afternoon 15, evening 18,
// night 21, tonight 20). A time without a day is the next time it comes
// round. An hour said without am/pm or a part of the day is read as the
// afternoon for 1 to 6 when a day is named, and as the next one to come
// otherwise. Returns an invalid time when the text names no day or time;
// *heard receives the words that were read, for the message to the user.
QDateTime findDue(const QString &text, const QDateTime &now, QString *heard = nullptr);

} // namespace Schedule
