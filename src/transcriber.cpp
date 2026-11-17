#include "transcriber.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

Transcriber::Transcriber(const QString &program, QObject *parent) : QObject(parent) {
    m_program = program;
    if (m_program.isEmpty())
        m_program = qEnvironmentVariable("OMNI_REMINDER_SUPERWHISPER");
    if (m_program.isEmpty())
        m_program = QStringLiteral("superwhisper");

    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, [this] { m_phase == Submitting ? submit() : poll(); });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            fail(tr("Superwhisper is not available (%1: %2).").arg(m_program, m_process.errorString()));
    });
    connect(&m_process, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        if (m_current.isEmpty())
            return;
        const QByteArray output = m_process.readAllStandardOutput();
        const QByteArray errors = m_process.readAllStandardError();
        const bool late = QDateTime::currentDateTimeUtc() > m_deadline;
        if (m_phase == Submitting) {
            QString error;
            const int session = parseStarted(output.isEmpty() ? errors : output, &error);
            if (session > 0) {
                m_session = session;
                m_phase = Polling;
                m_retry.start(m_pollMs);
            } else if (session == 0 && !late) {
                m_retry.start(1000); // Superwhisper is busy with a dictation
            } else {
                fail(late ? tr("Superwhisper stayed busy; try again later.") : error);
            }
            return;
        }
        QString text, warning;
        if (parseStatus(output, m_session, m_startedEpoch, &text, &warning)) {
            done(text.trimmed(), text.trimmed().isEmpty()
                                     ? (warning.isEmpty() ? tr("Superwhisper heard no speech.") : warning)
                                     : QString());
        } else if (late) {
            fail(tr("Superwhisper did not finish within %1 s.").arg(m_timeoutMs / 1000));
        } else {
            m_retry.start(m_pollMs);
        }
    });
}

int Transcriber::parseStarted(const QByteArray &output, QString *error) {
    const QJsonObject object = QJsonDocument::fromJson(output.trimmed()).object();
    if (object.value("ok").toBool() && object.value("session_id").toInt() > 0)
        return object.value("session_id").toInt();
    const QString message = object.value("error").toString();
    if (message.startsWith("Not ready", Qt::CaseInsensitive))
        return 0;
    if (error)
        *error = message.isEmpty() ? QString::fromUtf8(output.trimmed()).left(300) : message;
    if (error && error->isEmpty())
        *error = QStringLiteral("Superwhisper gave no answer");
    return -1;
}

bool Transcriber::parseStatus(const QByteArray &output, int session, qint64 notBeforeEpoch, QString *text,
                              QString *warning) {
    const QJsonObject status = QJsonDocument::fromJson(output.trimmed()).object();
    const QJsonObject last = status.value("last").toObject();
    if (last.isEmpty() || last.value("session_id").toInt() != session)
        return false;
    // Session ids restart with Superwhisper, so an old result with the same
    // id is told apart by its time.
    if (last.value("createdAt").toDouble() + 2 < notBeforeEpoch)
        return false;
    *text = last.value("text").toString();
    *warning = last.value("warning").toString();
    return true;
}

void Transcriber::transcribe(const QString &path) {
    m_queue.enqueue(path);
    if (m_current.isEmpty())
        next();
}

void Transcriber::next() {
    if (m_queue.isEmpty())
        return;
    m_current = m_queue.dequeue();
    m_phase = Submitting;
    m_session = 0;
    m_attempts = 0;
    m_startedEpoch = QDateTime::currentSecsSinceEpoch();
    m_deadline = QDateTime::currentDateTimeUtc().addMSecs(m_timeoutMs);
    submit();
}

void Transcriber::submit() {
    m_process.start(m_program, {"transcribe", m_current});
}

void Transcriber::poll() {
    m_process.start(m_program, {"status"});
}

void Transcriber::fail(const QString &error) {
    done({}, error);
}

void Transcriber::done(const QString &text, const QString &error) {
    m_retry.stop();
    const QString path = m_current;
    m_current.clear();
    emit transcribed(path, text, error);
    next();
}
