#include "ipc.h"

#include <QDir>
#include <QJsonDocument>
#include <QLocalSocket>
#include <time.h>

QString Ipc::socketName() {
    const QString overridden = qEnvironmentVariable("OMNI_REMINDER_SOCKET");
    if (!overridden.isEmpty())
        return overridden;
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty())
        runtime = QDir::tempPath();
    return runtime + "/omni-reminder.sock";
}

qint64 Ipc::monotonicMs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return qint64(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

bool Ipc::send(const QJsonObject &request, QJsonObject *reply, int timeoutMs) {
    QLocalSocket socket;
    socket.connectToServer(socketName());
    if (!socket.waitForConnected(timeoutMs))
        return false;
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + "\n");
    if (!socket.waitForBytesWritten(timeoutMs))
        return false;
    while (!socket.canReadLine()) {
        if (!socket.waitForReadyRead(timeoutMs))
            return false;
    }
    if (reply)
        *reply = QJsonDocument::fromJson(socket.readLine()).object();
    socket.disconnectFromServer();
    return true;
}
