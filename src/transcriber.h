#pragma once

#include <QDateTime>
#include <QObject>
#include <QProcess>
#include <QQueue>
#include <QTimer>

// Turns a voice clip into text with the Superwhisper command line, using
// whatever mode the user has set up in Superwhisper (its settings are never
// changed here). `superwhisper transcribe FILE` only queues the work and
// prints {"ok":true,"session_id":N}; the text appears later as "last" in
// `superwhisper status` with the same session id, so this polls for it.
class Transcriber : public QObject {
    Q_OBJECT

public:
    explicit Transcriber(const QString &program = {}, QObject *parent = nullptr);

    void setTimeoutMs(int ms) { m_timeoutMs = ms; }
    void setPollMs(int ms) { m_pollMs = ms; }
    void transcribe(const QString &path);
    bool busy() const { return !m_current.isEmpty(); }

    // Parsing helpers, separate so tests can feed them JSON directly.
    // Returns the session id, 0 for "busy, try again", -1 for an error.
    static int parseStarted(const QByteArray &output, QString *error);
    // True once `status` shows the result for this session.
    static bool parseStatus(const QByteArray &output, int session, qint64 notBeforeEpoch, QString *text,
                            QString *warning);

signals:
    void transcribed(const QString &path, const QString &text, const QString &error);

private:
    void next();
    void submit();
    void poll();
    void fail(const QString &error);
    void done(const QString &text, const QString &error);

    QString m_program;
    QQueue<QString> m_queue;
    QString m_current;
    QProcess m_process;
    enum Phase { Submitting, Polling } m_phase = Submitting;
    int m_session = 0;
    int m_attempts = 0;
    qint64 m_startedEpoch = 0;
    QDateTime m_deadline;
    QTimer m_retry;
    int m_timeoutMs = 120000;
    int m_pollMs = 400;
};
