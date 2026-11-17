#pragma once

#include <QDateTime>
#include <QHash>
#include <QLocalServer>
#include <QObject>
#include <QTimer>

#include "holdmachine.h"
#include "recorder.h"
#include "transcriber.h"

class Backend;

// The long-running process behind the global shortcut. It owns the socket
// the key-down/key-up commands talk to, the tap/hold state machine, the
// microphone recording and its transcription, and tells the backend (and so
// the windows) what is going on.
class Daemon : public QObject {
    Q_OBJECT

public:
    Daemon(Backend *backend, QObject *parent = nullptr);

    // Takes the socket. False (with *error) when another daemon has it.
    bool listen(QString *error);

    HoldMachine &machine() { return m_machine; }

    // Entry points shared by the socket and tests.
    void keyDown(qint64 at);
    void keyUp(qint64 at);

private:
    void handle(class QLocalSocket *socket);
    void armTimer();
    void startCapture();
    void onRecorded(const QString &path, double seconds, const QString &warning);
    void onTranscribed(const QString &path, const QString &text, const QString &error);
    void updateState();
    qint64 clampToNow(qint64 at) const;

    Backend *m_backend;
    QLocalServer m_server;
    HoldMachine m_machine;
    Recorder m_recorder;
    Transcriber m_transcriber;
    QTimer m_machineTimer, m_secondsTimer;
    QString m_recordingPath;
    QDateTime m_recordingStarted;
    bool m_finishing = false;   // stopped, waiting for ffmpeg to close the file
    QHash<QString, QPair<QDateTime, double>> m_pending; // clips being transcribed
    QHash<QString, QString> m_warnings;
};
