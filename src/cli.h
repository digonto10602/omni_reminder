#pragma once

#include <QStringList>

// The commands that need no display: they read and write the data directory
// directly (under the store's lock), or poke the running daemon. Each exits
// 0 on success and 1 on failure, with errors on stderr.
namespace Cli {

bool isCliCommand(const QString &command);
int run(const QStringList &arguments, qint64 startedMs);
void printUsage(bool toStderr);

// Talks to the daemon for key-down, key-up and open; starts it if needed.
int poke(const QString &command, qint64 startedMs, bool startDaemon);

} // namespace Cli
