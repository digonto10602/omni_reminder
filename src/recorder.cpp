#include "recorder.h"

#include <QFile>
#include <QFileInfo>

static QString ffmpegProgram() {
    const QString overridden = qEnvironmentVariable("OMNI_REMINDER_FFMPEG");
    return overridden.isEmpty() ? QStringLiteral("ffmpeg") : overridden;
}

Recorder::Recorder(QObject *parent) : QObject(parent) {
    m_terminate.setSingleShot(true);
    connect(&m_terminate, &QTimer::timeout, this, [this] {
        // ffmpeg did not act on "q"; SIGTERM still makes it write the trailer.
        if (m_process.state() != QProcess::NotRunning)
            m_process.terminate();
        QTimer::singleShot(2000, this, [this] {
            if (m_process.state() != QProcess::NotRunning)
                m_process.kill();
        });
    });
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        m_errors += m_process.readAllStandardError();
        if (m_errors.size() > 8192)
            m_errors = m_errors.right(8192);
    });
    connect(&m_process, &QProcess::finished, this, &Recorder::onExited);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_clock.invalidate();
            emit finished(m_finalPath, 0,
                          tr("Could not start ffmpeg to record the microphone (%1).").arg(m_process.errorString()));
        }
    });
}

Recorder::~Recorder() {
    if (m_process.state() != QProcess::NotRunning) {
        m_cancelled = true;
        m_process.kill();
        m_process.waitForFinished(1000);
        QFile::remove(m_partPath);
    }
}

QStringList Recorder::ffmpegArguments(const QString &outputPath) {
    return {"-hide_banner", "-loglevel", "error", "-nostats",
            "-f", "pulse", "-name", "Omni Reminder", "-i", "default",
            "-ac", "1", "-ar", "48000",
            "-c:a", "libopus", "-b:a", "32k", "-application", "voip",
            "-f", "ogg", "-y", outputPath};
}

void Recorder::start(const QString &finalPath) {
    if (isRecording())
        return;
    m_finalPath = finalPath;
    const QFileInfo info(finalPath);
    m_partPath = info.absolutePath() + "/." + info.fileName() + ".part";
    m_cancelled = m_stopping = false;
    m_errors.clear();
    m_clock.start();
    m_process.start(ffmpegProgram(), ffmpegArguments(m_partPath));
}

void Recorder::stop() {
    if (!isRecording() || m_stopping)
        return;
    m_stopping = true;
    // "q" on stdin is ffmpeg's own clean stop: it flushes and closes the file.
    m_process.write("q");
    m_terminate.start(3000);
}

void Recorder::cancel() {
    if (!isRecording())
        return;
    m_cancelled = true;
    m_process.kill();
    // Reap it now, so a quick second press can start a new recording.
    m_process.waitForFinished(1000);
}

void Recorder::onExited(int code, QProcess::ExitStatus status) {
    m_terminate.stop();
    const double wallSeconds = m_clock.isValid() ? m_clock.elapsed() / 1000.0 : 0;
    m_clock.invalidate();
    if (m_cancelled) {
        QFile::remove(m_partPath);
        return;
    }
    const QString errors = QString::fromUtf8(m_errors).trimmed();
    const bool hasAudio = QFileInfo(m_partPath).size() > 200;
    if (!hasAudio) {
        QFile::remove(m_partPath);
        QString reason = errors.isEmpty() ? tr("ffmpeg exited with code %1").arg(code) : errors;
        if (!m_stopping)
            reason = tr("The microphone recording stopped by itself: %1").arg(reason);
        emit finished(m_finalPath, 0, tr("No audio was recorded. %1").arg(reason));
        return;
    }
    QFile::remove(m_finalPath);
    if (!QFile::rename(m_partPath, m_finalPath)) {
        emit finished(m_finalPath, 0, tr("Could not save the recording to %1").arg(m_finalPath));
        return;
    }
    double seconds = probeSeconds(m_finalPath);
    if (seconds <= 0)
        seconds = wallSeconds;
    // A crash of ffmpeg mid-recording still leaves usable audio; keep it but say so.
    const QString warning = (status == QProcess::NormalExit && (code == 0 || m_stopping))
                                ? QString()
                                : tr("The recording ended early: %1").arg(errors);
    emit finished(m_finalPath, seconds, warning);
}

double Recorder::probeSeconds(const QString &path) {
    QProcess probe;
    probe.start("ffprobe", {"-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path});
    if (!probe.waitForFinished(3000)) {
        probe.kill();
        return 0;
    }
    return QString::fromUtf8(probe.readAllStandardOutput()).trimmed().toDouble();
}
