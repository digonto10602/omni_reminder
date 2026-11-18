#include "backend.h"

#include "retention.h"
#include "schedule.h"

#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <algorithm>

static constexpr int UndoDepth = 30;

Backend::Backend(const QString &dataDirectory, QObject *parent) : QObject(parent), m_store(dataDirectory) {
    // reminders.json is replaced by a rename on every save, so watch the
    // directory and re-arm the file watch after each change.
    m_fileDebounce.setSingleShot(true);
    m_fileDebounce.setInterval(120);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_fileDebounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_fileDebounce.start(); });
    connect(&m_fileDebounce, &QTimer::timeout, this, &Backend::reload);

    m_dueTimer.setSingleShot(true);
    connect(&m_dueTimer, &QTimer::timeout, this, &Backend::checkDue);

    // "At least daily": every six hours, plus on start and after each recording.
    m_retentionTimer.setInterval(6 * 60 * 60 * 1000);
    connect(&m_retentionTimer, &QTimer::timeout, this, &Backend::runRetention);
    m_retentionTimer.start();

    QString error;
    if (!m_store.load(&error))
        showMessage(error, true);
    rememberFile();
    refresh();
}

void Backend::rememberFile() {
    QFile file(m_store.filePath());
    m_lastFile = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    if (QFileInfo::exists(m_store.directory()))
        m_watcher.addPath(m_store.directory());
    if (QFileInfo::exists(m_store.filePath()))
        m_watcher.addPath(m_store.filePath());
}

// Picks up changes made by the command line (or by hand). Our own saves are
// recognised by their content and ignored.
void Backend::reload() {
    QFile file(m_store.filePath());
    const QByteArray now = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    if (now == m_lastFile) {
        rememberFile();
        return;
    }
    QString error;
    if (!m_store.load(&error)) {
        showMessage(error, true);
        rememberFile();
        return;
    }
    // Undo snapshots predate the outside change; replaying them would undo it.
    m_undo.clear();
    m_redo.clear();
    emit undoChanged();
    rememberFile();
    refresh();
}

bool Backend::mutate(const std::function<bool(Store &)> &change, bool undoable) {
    QList<Reminder> before;
    bool changed = false;
    QString error;
    const bool ok = m_store.update([&](Store &store) {
        before = store.reminders;
        changed = change(store);
        return changed;
    }, &error);
    if (!ok) {
        m_lastError = error;
        showMessage(error, true);
        return false;
    }
    if (changed && undoable) {
        emit localChanged();
        m_undo.append(before);
        while (m_undo.size() > UndoDepth)
            m_undo.removeFirst();
        m_redo.clear();
        emit undoChanged();
    }
    rememberFile();
    refresh();
    return true;
}

// Reminders with a time come first, soonest first; the rest follow, newest
// first, since a fresh note is what one usually looks for.
void Backend::refresh() {
    QList<Reminder> sorted = m_store.reminders;
    std::stable_sort(sorted.begin(), sorted.end(), [](const Reminder &a, const Reminder &b) {
        if (a.done != b.done)
            return !a.done;
        if (a.due.isValid() != b.due.isValid())
            return a.due.isValid();
        if (a.due.isValid())
            return a.due < b.due;
        return a.created > b.created;
    });
    m_items.clear();
    for (const Reminder &r : sorted) {
        const QString path = r.clip.isEmpty() ? QString() : m_store.clipPath(r.clip);
        m_items.append(QVariantMap{
            {"id", r.id},
            {"text", r.text},
            {"createdMs", r.created.isValid() ? double(r.created.toMSecsSinceEpoch()) : 0.0},
            {"hasDue", r.due.isValid()},
            {"dueMs", r.due.isValid() ? double(r.due.toMSecsSinceEpoch()) : 0.0},
            {"notified", r.notified},
            {"hasClip", !path.isEmpty()},
            {"clipUrl", path.isEmpty() ? QString() : QUrl::fromLocalFile(path).toString()},
            {"clipSeconds", r.clipSeconds},
            {"clipExpired", r.clipExpired},
            {"done", r.done},
        });
    }
    emit remindersChanged();
    scheduleDue();
}

int Backend::add(const QString &text, const QString &dueText) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        showMessage(tr("Write something to remember first."), true);
        return 0;
    }
    bool ok = false;
    const QDateTime due = Schedule::parseDue(dueText, QDateTime::currentDateTime(), &ok);
    if (!ok) {
        showMessage(dueProblem(dueText), true);
        return 0;
    }
    int id = 0;
    if (!mutate([&](Store &store) {
            Reminder r;
            r.text = trimmed;
            r.due = due;
            id = store.add(r);
            return true;
        }, true))
        return 0;
    clearMessage();
    return id;
}

bool Backend::update(int id, const QString &text, const QString &dueText) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        showMessage(tr("A reminder needs some text. Press Delete to remove it instead."), true);
        return false;
    }
    bool ok = false;
    const QDateTime due = Schedule::parseDue(dueText, QDateTime::currentDateTime(), &ok);
    if (!ok) {
        showMessage(dueProblem(dueText), true);
        return false;
    }
    bool found = false;
    const bool saved = mutate([&](Store &store) {
        Reminder *r = store.find(id);
        if (!r)
            return false;
        found = true;
        const bool dueChanged = r->due != due;
        if (r->text == trimmed && !dueChanged)
            return false;
        r->text = trimmed;
        r->updated = QDateTime::currentDateTimeUtc();
        if (dueChanged) {
            r->due = due;
            // A new time is a new promise: notify again when it comes.
            r->notified = false;
        }
        return true;
    }, true);
    if (saved && !found)
        showMessage(tr("That reminder was removed elsewhere."), true);
    else if (saved)
        clearMessage();
    return saved && found;
}

// The clip stays on disk while an undo could still bring the reminder back;
// retention removes it once no snapshot refers to it.
bool Backend::remove(int id) {
    return mutate([&](Store &store) {
        for (int i = 0; i < store.reminders.size(); ++i) {
            if (store.reminders[i].id == id) {
                store.reminders.removeAt(i);
                return true;
            }
        }
        return false;
    }, true);
}

// An undo is a new edit as far as the phone is concerned: whatever it brings
// back must count as newer than what it replaces.
QList<Reminder> Backend::stamped(QList<Reminder> target, const QList<Reminder> &current) {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (Reminder &r : target) {
        bool same = false;
        for (const Reminder &c : current)
            if (c.uid == r.uid)
                same = c.text == r.text && c.due == r.due && c.done == r.done;
        if (!same)
            r.updated = now;
    }
    return target;
}

bool Backend::setDone(int id, bool done) {
    return mutate([&](Store &store) {
        Reminder *r = store.find(id);
        if (!r || r->done == done)
            return false;
        r->done = done;
        r->updated = QDateTime::currentDateTimeUtc();
        return true;
    }, true);
}

void Backend::setSyncStatus(const QString &status, bool problem) {
    if (status == m_syncStatus && problem == m_syncProblem)
        return;
    m_syncStatus = status;
    m_syncProblem = problem;
    emit syncStatusChanged();
}

void Backend::undo() {
    if (m_undo.isEmpty())
        return;
    const QList<Reminder> target = m_undo.takeLast();
    QList<Reminder> current;
    QString error;
    if (!m_store.update([&](Store &store) {
            current = store.reminders;
            store.reminders = stamped(target, current);
            return true;
        }, &error)) {
        showMessage(error, true);
        return;
    }
    emit localChanged();
    m_redo.append(current);
    emit undoChanged();
    rememberFile();
    runRetention(); // a restored reminder whose clip is gone shows "clip expired"
    refresh();
}

void Backend::redo() {
    if (m_redo.isEmpty())
        return;
    const QList<Reminder> target = m_redo.takeLast();
    QList<Reminder> current;
    QString error;
    if (!m_store.update([&](Store &store) {
            current = store.reminders;
            store.reminders = stamped(target, current);
            return true;
        }, &error)) {
        showMessage(error, true);
        return;
    }
    emit localChanged();
    m_undo.append(current);
    emit undoChanged();
    rememberFile();
    refresh();
}

QString Backend::dueProblem(const QString &dueText) const {
    bool ok = false;
    Schedule::parseDue(dueText, QDateTime::currentDateTime(), &ok);
    return ok ? QString()
              : tr("Cannot read \"%1\" as a time. Try 18:30, tomorrow 9:00, +2h, or 2026-12-01 14:00.")
                    .arg(dueText.trimmed());
}

double Backend::duePreview(const QString &dueText) const {
    bool ok = false;
    const QDateTime due = Schedule::parseDue(dueText, QDateTime::currentDateTime(), &ok);
    return ok && due.isValid() ? double(due.toMSecsSinceEpoch()) : 0.0;
}

QString Backend::dueEditText(int id) {
    const Reminder *r = m_store.find(id);
    return r && r->due.isValid() ? r->due.toLocalTime().toString("yyyy-MM-dd HH:mm") : QString();
}

void Backend::showOverlay() {
    if (!m_overlayVisible) {
        m_overlayVisible = true;
        emit overlayVisibleChanged();
    }
}

void Backend::hideOverlay() {
    if (m_overlayVisible) {
        m_overlayVisible = false;
        emit overlayVisibleChanged();
    }
}

// A tap hides a focused overlay. One that is open but lost focus is shown
// afresh instead, since the compositor focuses a newly mapped window.
void Backend::toggleOverlay(bool overlayActive) {
    if (!m_overlayVisible)
        showOverlay();
    else if (overlayActive)
        hideOverlay();
    else
        emit overlayRemapRequested();
}

void Backend::presentOverlay() {
    if (!m_overlayVisible)
        showOverlay();
    else if (!m_overlayActive)
        emit overlayRemapRequested();
}

void Backend::clearMessage() {
    if (!m_message.isEmpty()) {
        m_message.clear();
        m_messageIsError = false;
        emit messageChanged();
    }
}

void Backend::showMessage(const QString &message, bool error) {
    m_message = message;
    m_messageIsError = error;
    emit messageChanged();
}

void Backend::setRecordingState(const QString &state, int seconds) {
    if (state == m_recordingState && seconds == m_recordingSeconds)
        return;
    m_recordingState = state;
    m_recordingSeconds = seconds;
    emit recordingChanged();
}

int Backend::addVoiceMemo(const QString &clipPath, const QDateTime &recorded, double seconds, const QString &text,
                          const QString &problem) {
    int id = 0;
    const QString name = QFileInfo(clipPath).fileName();
    const bool hasClip = QFileInfo::exists(clipPath);
    mutate([&](Store &store) {
        Reminder r;
        r.text = text.trimmed().isEmpty()
                     ? tr("Voice memo %1").arg(QLocale().toString(recorded.toLocalTime(), QLocale::ShortFormat))
                     : text.trimmed();
        r.created = recorded;
        if (hasClip) {
            r.clip = name;
            r.clipRecorded = recorded;
            r.clipSeconds = seconds;
        }
        id = store.add(r);
        return true;
    }, true);
    if (!problem.isEmpty())
        showMessage(tr("Saved as a voice memo without text: %1").arg(problem), true);
    else
        clearMessage();
    runRetention();
    if (id)
        emit editRequested(id);
    return id;
}

QSet<QString> Backend::undoClips() const {
    QSet<QString> clips;
    for (const auto &snapshot : m_undo)
        for (const Reminder &r : snapshot)
            if (!r.clip.isEmpty())
                clips.insert(r.clip);
    for (const auto &snapshot : m_redo)
        for (const Reminder &r : snapshot)
            if (!r.clip.isEmpty())
                clips.insert(r.clip);
    return clips;
}

void Backend::runRetention() {
    const auto result = Retention::enforce(m_store, QDateTime::currentDateTime(), undoClips());
    if (!result.ok)
        showMessage(tr("Clip cleanup failed: %1").arg(result.error), true);
    rememberFile();
    refresh();
}

void Backend::scheduleDue() {
    const qint64 wait = Schedule::msUntilNext(m_store.reminders, QDateTime::currentDateTime());
    if (wait < 0)
        m_dueTimer.stop();
    else
        m_dueTimer.start(int(qMax<qint64>(wait, 50)));
}

void Backend::checkDue() {
    const QList<int> ids = Schedule::dueIds(m_store.reminders, QDateTime::currentDateTime());
    if (ids.isEmpty()) {
        scheduleDue();
        return;
    }
    QList<Reminder> fired;
    mutate([&](Store &store) {
        // Re-read inside the lock: another process may have notified or
        // removed it meanwhile.
        for (int id : Schedule::dueIds(store.reminders, QDateTime::currentDateTime())) {
            if (Reminder *r = store.find(id)) {
                r->notified = true;
                fired << *r;
            }
        }
        return !fired.isEmpty();
    }, false);
    for (const Reminder &r : fired)
        notify(r);
}

// notify-send reads options from its arguments, so "--" keeps a reminder that
// starts with a dash from being taken as one.
void Backend::notify(const Reminder &reminder) {
    QString program = qEnvironmentVariable("OMNI_REMINDER_NOTIFY");
    if (program.isEmpty())
        program = QStringLiteral("notify-send");
    QString body = reminder.text;
    if (!reminder.clip.isEmpty())
        body += "\n" + tr("(voice memo attached)");
    const QStringList arguments{"--app-name=Omni Reminder", "--icon=omni-reminder", "--urgency=critical",
                                "--", tr("Reminder"), body};
    if (!QProcess::startDetached(program, arguments))
        showMessage(tr("Could not show a notification: %1 is missing.").arg(program), true);
}
