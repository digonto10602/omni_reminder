// omni-reminder: a reminder overlay for Omarchy. Tap the shortcut to see your
// reminders, hold it to speak a new one.

#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QUrl>
#include <cstdio>

#include "backend.h"
#include "cli.h"
#include "daemon.h"
#include "ipc.h"
#include "theme.h"
#include "translator.h"

static const char *Version = "0.1.0";

int main(int argc, char *argv[]) {
    // Taken before anything else, so a key press is timed as close to the
    // real keystroke as this process can get.
    const qint64 startedMs = Ipc::monotonicMs();

    const QString command = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    if (command.isEmpty()) {
        QCoreApplication app(argc, argv);
        app.setApplicationVersion(Version);
        Cli::printUsage(false);
        return 0;
    }
    if (Cli::isCliCommand(command)) {
        // No display needed, and no Qt GUI start-up cost for the key commands.
        QCoreApplication app(argc, argv);
        app.setApplicationName("omni-reminder");
        app.setApplicationVersion(Version);
        return Cli::run(app.arguments().mid(1), startedMs);
    }
    if (command != "daemon" && command != "open") {
        fprintf(stderr, "omni-reminder: unknown command \"%s\"\n\n", argv[1]);
        QCoreApplication app(argc, argv);
        Cli::printUsage(true);
        return 1;
    }
    // "open" hands over to a running daemon; without one, this process
    // becomes the daemon and shows the overlay.
    if (command == "open" && Ipc::send({{"cmd", "open"}}))
        return 0;

    // The overlay is see-through, which needs an alpha channel on every window.
    QQuickWindow::setDefaultAlphaBuffer(true);
    QGuiApplication app(argc, argv);
    app.setApplicationName("omni-reminder");
    app.setApplicationVersion(Version);
    // Matches the windows to omni-reminder.desktop and to the Hyprland rules
    // (Wayland app id "omni-reminder").
    app.setDesktopFileName("omni-reminder");
    app.setWindowIcon(QIcon::fromTheme("omni-reminder"));
    // Hiding the overlay must not end the daemon.
    app.setQuitOnLastWindowClosed(false);

    QQuickStyle::setStyle("Material");

    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    Backend backend;
    Daemon daemon(&backend);
    QString error;
    if (!daemon.listen(&error)) {
        fprintf(stderr, "omni-reminder: %s\n", qPrintable(error));
        return 1;
    }

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    engine.load(QUrl("qrc:/RecordingWindow.qml"));
    if (engine.rootObjects().size() < 2)
        return 1;

    // Overdue reminders notify once now; clips past their time go now.
    backend.runRetention();
    backend.checkDue();
    if (command == "open")
        backend.showOverlay();

    return app.exec();
}
