#pragma once

#include <QObject>

// Turns key-down and key-up events from the global shortcut into "tap" or
// "hold". Times are milliseconds on the monotonic clock, taken by the
// key-down/key-up processes as they start, so tests can pass fake times and
// two processes racing to the daemon cannot swap a short tap around.
//
// The microphone starts on key-down, before we know whether it is a tap,
// so the first words of a voice memo are not lost; a tap throws that audio
// away. A hold that never sees its release (Hyprland can drop it) is ended by
// the length limit, by stop() from the overlay, or by the next key-down.
class HoldMachine : public QObject {
    Q_OBJECT

public:
    enum State { Idle, Pressed, Holding, AwaitRelease };
    Q_ENUM(State)

    explicit HoldMachine(int thresholdMs = 400, int maxMs = 5 * 60 * 1000, QObject *parent = nullptr);

    State state() const { return m_state; }
    int thresholdMs() const { return m_threshold; }
    int maxMs() const { return m_max; }

    void keyDown(qint64 now);
    void keyUp(qint64 now);
    // Called by a timer; promotes a press to a hold and ends a hold at the limit.
    void tick(qint64 now);
    // The user stopped or cancelled the recording from a window.
    void stop();
    void cancel();

    // When tick() next needs to run, or -1 when nothing is pending.
    qint64 nextDeadline() const;

signals:
    void captureStarted();         // start the microphone (provisionally)
    void captureCancelled();       // throw the audio away
    void holdConfirmed();          // it is a hold: show the recording indicator
    void captureFinished(bool atLimit); // keep the audio and transcribe it
    void tapped();                 // toggle the overlay

private:
    void finish(bool atLimit);

    int m_threshold;
    int m_max;
    State m_state = Idle;
    qint64 m_downAt = 0;
    // A key-up that arrived while idle, kept briefly in case its key-down
    // process was slower to reach the daemon.
    qint64 m_strayUpAt = -1;
};
