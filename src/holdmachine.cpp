#include "holdmachine.h"

HoldMachine::HoldMachine(int thresholdMs, int maxMs, QObject *parent)
    : QObject(parent), m_threshold(thresholdMs), m_max(maxMs) {}

void HoldMachine::keyDown(qint64 now) {
    switch (m_state) {
    case Idle:
    case AwaitRelease:
        // The release was seen first: the two processes raced and the
        // key-up won. Its time still tells how long the key was down.
        if (m_state == Idle && m_strayUpAt >= now && m_strayUpAt - now < 2000) {
            m_strayUpAt = -1;
            emit tapped();
            return;
        }
        m_strayUpAt = -1;
        m_state = Pressed;
        m_downAt = now;
        emit captureStarted();
        return;
    case Pressed:
        // Hyprland does not repeat this bind, so a second press while one is
        // pending is a duplicate; keep the first.
        return;
    case Holding:
        // The release of the last hold was lost and this is a fresh press:
        // end that recording and wait for this press's release.
        finish(false);
        return;
    }
}

void HoldMachine::keyUp(qint64 now) {
    switch (m_state) {
    case Idle:
        m_strayUpAt = now;
        return;
    case AwaitRelease:
        m_state = Idle;
        return;
    case Pressed:
        if (now - m_downAt < m_threshold) {
            m_state = Idle;
            emit captureCancelled();
            emit tapped();
        } else {
            // Long enough, but no tick ran in between (a busy event loop).
            emit holdConfirmed();
            m_state = Idle;
            emit captureFinished(false);
        }
        return;
    case Holding:
        m_state = Idle;
        emit captureFinished(false);
        return;
    }
}

void HoldMachine::tick(qint64 now) {
    if (m_state == Pressed && now - m_downAt >= m_threshold) {
        m_state = Holding;
        emit holdConfirmed();
    }
    if (m_state == Holding && now - m_downAt >= m_max)
        finish(true);
}

void HoldMachine::stop() {
    if (m_state == Pressed || m_state == Holding)
        finish(false);
}

void HoldMachine::cancel() {
    if (m_state == Pressed || m_state == Holding) {
        m_state = AwaitRelease;
        emit captureCancelled();
    }
}

void HoldMachine::finish(bool atLimit) {
    // The key may still be down; its release must not count as a new tap.
    m_state = AwaitRelease;
    emit captureFinished(atLimit);
}

qint64 HoldMachine::nextDeadline() const {
    if (m_state == Pressed)
        return m_downAt + m_threshold;
    if (m_state == Holding)
        return m_downAt + m_max;
    return -1;
}
