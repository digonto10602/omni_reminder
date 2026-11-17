#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QTimer>

// Records the default PipeWire microphone straight into Opus in an Ogg file
// (mono, 32 kbps) with ffmpeg, through PipeWire's PulseAudio interface. The
// file is written under a dot-name and only renamed to its final name once
// ffmpeg has closed it cleanly, so retention never sees a half-written clip.
class Recorder : public QObject {
    Q_OBJECT

public:
    explicit Recorder(QObject *parent = nullptr);
    ~Recorder() override;

    bool isRecording() const { return m_process.state() != QProcess::NotRunning; }
    qint64 elapsedMs() const { return m_clock.isValid() ? m_clock.elapsed() : 0; }

    // Starts writing to `finalPath` (via a hidden temporary beside it).
    void start(const QString &finalPath);
    // Ends the recording cleanly; finished() follows.
    void stop();
    // Ends it and deletes the audio; nothing follows.
    void cancel();

    static QStringList ffmpegArguments(const QString &outputPath);
    static double probeSeconds(const QString &path);

signals:
    void finished(const QString &path, double seconds, const QString &error);

private:
    void onExited(int code, QProcess::ExitStatus status);

    QProcess m_process;
    QString m_finalPath, m_partPath;
    bool m_cancelled = false, m_stopping = false;
    QElapsedTimer m_clock;
    QTimer m_terminate;
    QByteArray m_errors;
};
