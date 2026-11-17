#pragma once

#include <QJsonObject>
#include <QString>

// The daemon listens on a local socket in $XDG_RUNTIME_DIR. Each request is
// one JSON line, each reply one JSON line.
namespace Ipc {

QString socketName();

// The monotonic clock in milliseconds, the same clock in every process.
qint64 monotonicMs();

// Sends one request and waits for the reply. False when no daemon answers.
bool send(const QJsonObject &request, QJsonObject *reply = nullptr, int timeoutMs = 1500);

} // namespace Ipc
