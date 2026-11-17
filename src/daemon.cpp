#include "daemon.h"

#include "backend.h"
#include "ipc.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>

Daemon::Daemon(Backend *backend, QObject *parent)
    : QObject(parent), m_backend(backend), m_machine(400, 5 * 60 * 1000) {
    m_backend->setMaxRecordingSeconds(m_machine.maxMs() / 1000);

    m_machineTimer.setSingleShot(true);
    connect(&m_machineTimer, &QTimer::timeout, this, [this] {
        m_machine.tick(Ipc::monotonicMs());
        armTimer();
    });
    m_secondsTimer.setInterval(250);
    connect(&m_secondsTimer, &QTimer::timeout, this, &Daemon::updateState);

    connect(&m_machine, &HoldMachine::captureStarted, this, &Daemon::startCapture);
    connect(&m_machine, &HoldMachine::captureCancelled, this, [this] {
        m_recorder.cancel();
        m_recordingPath.clear();
        updateState();
    });
    connect(&m_machine, &HoldMachine::holdConfirmed, this, &Daemon::updateState);
    connect(&m_machine, &HoldMachine::captureFinished, this, [this](bool atLimit) {
        if (atLimit)
            m_backend->showMessage(tr("Recording stopped at the %1 minute limit.").arg(m_machine.maxMs() / 60000),
                                   false);
        m_finishing = true;
        m_recorder.stop();
        updateState();
    });
    connect(&m_machine, &HoldMachine::tapped, this, [this] {
        m_backend->toggleOverlay(m_backend->overlayActive());
    });

    connect(&m_recorder, &Recorder::finished, this, &Daemon::onRecorded);
    connect(&m_transcriber, &Transcriber::transcribed, this, &Daemon::onTranscribed);

    // The overlay's own buttons and keys reach the recording through here.
    connect(m_backend, &Backend::recordToggleRequested, this, [this] {
        if (m_machine.state() == HoldMachine::Pressed || m_machine.state() == HoldMachine::Holding) {
            m_machine.stop();
        } else {
            // Start as if the key had been held past the threshold already.
            const qint64 now = Ipc::monotonicMs();
            m_machine.keyDown(now - m_machine.thresholdMs());
            m_machine.tick(now);
        }
        armTimer();
    });
    connect(m_backend, &Backend::recordStopRequested, this, [this] {
        m_machine.stop();
        armTimer();
    });
    connect(m_backend, &Backend::recordCancelRequested, this, [this] {
        m_machine.cancel();
        armTimer();
    });

    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *socket = m_server.nextPendingConnection()) {
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] { handle(socket); });
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            if (socket->canReadLine())
                handle(socket);
        }
    });
}

bool Daemon::listen(QString *error) {
    const QString name = Ipc::socketName();
    if (m_server.listen(name))
        return true;
    // Either another daemon owns it, or a crashed one left the file behind.
    if (Ipc::send({{"cmd", "ping"}}, nullptr, 500)) {
        if (error)
            *error = tr("omni-reminder is already running (%1).").arg(name);
        return false;
    }
    QLocalServer::removeServer(name);
    if (m_server.listen(name))
        return true;
    if (error)
        *error = tr("Cannot listen on %1: %2").arg(name, m_server.errorString());
    return false;
}

void Daemon::handle(QLocalSocket *socket) {
    while (socket->canReadLine()) {
        const QJsonObject request = QJsonDocument::fromJson(socket->readLine()).object();
        const QString command = request.value("cmd").toString();
        const qint64 at = qint64(request.value("t").toDouble(-1));
        QJsonObject reply{{"ok", true}};
        if (command == "key-down")
            keyDown(at);
        else if (command == "key-up")
            keyUp(at);
        else if (command == "open")
            m_backend->presentOverlay();
        else if (command == "toggle")
            m_backend->toggleOverlay(m_backend->overlayActive());
        else if (command == "hide")
            m_backend->hideOverlay();
        else if (command == "stop")
            m_machine.stop();
        else if (command == "reload")
            m_backend->reload();
        else if (command != "ping")
            reply = {{"ok", false}, {"error", tr("Unknown command: %1").arg(command)}};
        reply.insert("state", QVariant::fromValue(m_machine.state()).toString());
        reply.insert("recording", m_backend->recordingState());
        socket->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + "\n");
        socket->flush();
    }
}

// The key processes stamp their own start time, so the gap between press and
// release is measured where it happened; a stamp that is far off (a stalled
// process, a bad clock) falls back to the arrival time.
qint64 Daemon::clampToNow(qint64 at) const {
    const qint64 now = Ipc::monotonicMs();
    return (at < 0 || at > now || now - at > 3000) ? now : at;
}

void Daemon::keyDown(qint64 at) {
    m_machine.keyDown(clampToNow(at));
    armTimer();
}

void Daemon::keyUp(qint64 at) {
    m_machine.keyUp(clampToNow(at));
    armTimer();
}

void Daemon::armTimer() {
    const qint64 deadline = m_machine.nextDeadline();
    if (deadline < 0) {
        m_machineTimer.stop();
    } else {
        m_machineTimer.start(int(qBound<qint64>(0, deadline - Ipc::monotonicMs(), qint64(24) * 3600 * 1000)));
    }
    updateState();
}

void Daemon::startCapture() {
    if (m_recorder.isRecording()) {
        m_backend->showMessage(tr("Still saving the last voice memo; hold the key again in a moment."), true);
        m_machine.cancel();
        return;
    }
    QDir().mkpath(m_backend->store().clipsDirectory());
    m_recordingStarted = QDateTime::currentDateTime();
    const QString stem = m_recordingStarted.toString("yyyyMMdd-HHmmss");
    QString path = m_backend->store().clipPath(stem + ".ogg");
    for (int n = 2; QFileInfo::exists(path); ++n)
        path = m_backend->store().clipPath(QStringLiteral("%1-%2.ogg").arg(stem).arg(n));
    m_recordingPath = path;
    m_finishing = false;
    m_recorder.start(path);
}

void Daemon::onRecorded(const QString &path, double seconds, const QString &warning) {
    m_finishing = false;
    m_recordingPath.clear();
    if (!QFileInfo::exists(path)) {
        // Only a real hold reports a failure; a tap that raced a failing
        // microphone has already been dealt with.
        m_backend->showMessage(warning, true);
        m_backend->presentOverlay();
        updateState();
        return;
    }
    m_pending.insert(path, {m_recordingStarted, seconds});
    if (!warning.isEmpty())
        m_warnings.insert(path, warning);
    m_transcriber.transcribe(path);
    updateState();
}

void Daemon::onTranscribed(const QString &path, const QString &text, const QString &error) {
    const auto meta = m_pending.take(path);
    QString problem = error;
    const QString warning = m_warnings.take(path);
    if (!warning.isEmpty())
        problem = problem.isEmpty() ? warning : problem + " " + warning;
    if (!text.isEmpty() && !problem.isEmpty()) {
        m_backend->addVoiceMemo(path, meta.first, meta.second, text, {});
        m_backend->showMessage(problem, true);
    } else {
        m_backend->addVoiceMemo(path, meta.first, meta.second, text, problem);
    }
    m_backend->presentOverlay();
    updateState();
}

void Daemon::updateState() {
    const auto state = m_machine.state();
    if (state == HoldMachine::Holding) {
        if (!m_secondsTimer.isActive())
            m_secondsTimer.start();
        m_backend->setRecordingState("recording", int(m_recorder.elapsedMs() / 1000));
        return;
    }
    m_secondsTimer.stop();
    if (m_finishing || !m_pending.isEmpty() || m_transcriber.busy())
        m_backend->setRecordingState("transcribing");
    else
        m_backend->setRecordingState("idle");
}
