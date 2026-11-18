#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include "store.h"

// The reminders as QML sees them, and every change the overlay makes. It also
// keeps the daemon's clocks: due notifications and the daily clip cleanup.
// Changes from outside (the command line) are picked up from the file.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList reminders READ reminders NOTIFY remindersChanged)
    Q_PROPERTY(bool overlayVisible READ overlayVisible NOTIFY overlayVisibleChanged)
    Q_PROPERTY(QString recordingState READ recordingState NOTIFY recordingChanged)
    Q_PROPERTY(int recordingSeconds READ recordingSeconds NOTIFY recordingChanged)
    Q_PROPERTY(int maxRecordingSeconds READ maxRecordingSeconds CONSTANT)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(bool messageIsError READ messageIsError NOTIFY messageChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoChanged)
    Q_PROPERTY(QString dataDirectory READ dataDirectory CONSTANT)
    // One line on the Google sync, empty when it is not set up.
    Q_PROPERTY(QString syncStatus READ syncStatus NOTIFY syncStatusChanged)
    Q_PROPERTY(bool syncProblem READ syncProblem NOTIFY syncStatusChanged)
    // True while a pass with Google runs, so the Sync now button can say so.
    Q_PROPERTY(bool syncing READ syncing NOTIFY syncingChanged)
    // Whether the overlay window has keyboard focus; the window reports it.
    Q_PROPERTY(bool overlayActive READ overlayActive WRITE setOverlayActive NOTIFY overlayActiveChanged)

public:
    explicit Backend(const QString &dataDirectory = {}, QObject *parent = nullptr);

    QVariantList reminders() const { return m_items; }
    bool overlayVisible() const { return m_overlayVisible; }
    QString recordingState() const { return m_recordingState; }
    int recordingSeconds() const { return m_recordingSeconds; }
    int maxRecordingSeconds() const { return m_maxRecordingSeconds; }
    QString message() const { return m_message; }
    bool messageIsError() const { return m_messageIsError; }
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    QString dataDirectory() const { return m_store.directory(); }
    QString syncStatus() const { return m_syncStatus; }
    bool syncProblem() const { return m_syncProblem; }
    void setSyncStatus(const QString &status, bool problem);
    bool syncing() const { return m_syncing; }
    void setSyncing(bool syncing) {
        if (syncing != m_syncing) {
            m_syncing = syncing;
            emit syncingChanged();
        }
    }
    Store &store() { return m_store; }
    bool overlayActive() const { return m_overlayActive; }
    void setOverlayActive(bool active) {
        if (active != m_overlayActive) {
            m_overlayActive = active;
            emit overlayActiveChanged();
        }
    }

    // Overlay actions.
    Q_INVOKABLE int add(const QString &text, const QString &dueText);
    Q_INVOKABLE bool update(int id, const QString &text, const QString &dueText);
    Q_INVOKABLE bool remove(int id);
    Q_INVOKABLE bool setDone(int id, bool done);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    // "" when the text reads as a due time (or is empty), else what is wrong.
    Q_INVOKABLE QString dueProblem(const QString &dueText) const;
    // The due time a text would give, in milliseconds since the epoch, or 0.
    Q_INVOKABLE double duePreview(const QString &dueText) const;
    Q_INVOKABLE QString dueEditText(int id);
    Q_INVOKABLE void showOverlay();
    Q_INVOKABLE void hideOverlay();
    Q_INVOKABLE void toggleOverlay(bool overlayActive);
    // Shows the overlay with keyboard focus, whether or not it was open.
    Q_INVOKABLE void presentOverlay();
    Q_INVOKABLE void clearMessage();
    Q_INVOKABLE void toggleRecording() { emit recordToggleRequested(); }
    Q_INVOKABLE void stopRecording() { emit recordStopRequested(); }
    Q_INVOKABLE void cancelRecording() { emit recordCancelRequested(); }
    // The overlay's Sync now button: the daemon's sync runs a pass right away.
    Q_INVOKABLE void syncNow() { emit syncNowRequested(); }

    // Daemon side.
    void setRecordingState(const QString &state, int seconds = 0);
    void setMaxRecordingSeconds(int seconds) { m_maxRecordingSeconds = seconds; }
    Q_INVOKABLE void showMessage(const QString &message, bool error);
    int addVoiceMemo(const QString &clipPath, const QDateTime &recorded, double seconds, const QString &text,
                     const QString &problem);
    void runRetention();
    void checkDue();
    void reload();
    QString lastError() const { return m_lastError; }

signals:
    void remindersChanged();
    void overlayVisibleChanged();
    void overlayActiveChanged();
    void recordingChanged();
    void messageChanged();
    void undoChanged();
    void syncStatusChanged();
    void syncingChanged();
    void syncNowRequested();
    // The user changed a reminder here; the sync pushes it soon.
    void localChanged();
    // Select this reminder and open it for editing (after a voice memo).
    void editRequested(int id);
    // The overlay window should be shown again from scratch (to take focus).
    void overlayRemapRequested();
    void recordToggleRequested();
    void recordStopRequested();
    void recordCancelRequested();

private:
    bool mutate(const std::function<bool(Store &)> &change, bool undoable);
    void refresh();
    void rememberFile();
    void scheduleDue();
    QSet<QString> undoClips() const;
    static QList<Reminder> stamped(QList<Reminder> target, const QList<Reminder> &current);
    void notify(const Reminder &reminder);

    Store m_store;
    QVariantList m_items;
    QList<QList<Reminder>> m_undo, m_redo;
    bool m_overlayVisible = false;
    bool m_overlayActive = false;
    QString m_recordingState = "idle";
    int m_recordingSeconds = 0;
    int m_maxRecordingSeconds = 300;
    QString m_message;
    bool m_messageIsError = false;
    QString m_lastError;
    QString m_syncStatus;
    bool m_syncProblem = false;
    bool m_syncing = false;
    QByteArray m_lastFile;
    QFileSystemWatcher m_watcher;
    QTimer m_fileDebounce, m_dueTimer, m_retentionTimer;
};
