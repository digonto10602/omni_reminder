QT += core gui qml quick quickcontrols2 multimedia network

CONFIG += c++17 release
TARGET = omni-reminder
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/cli.h \
    src/daemon.h \
    src/holdmachine.h \
    src/ipc.h \
    src/recorder.h \
    src/retention.h \
    src/schedule.h \
    src/store.h \
    src/theme.h \
    src/transcriber.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/cli.cpp \
    src/daemon.cpp \
    src/holdmachine.cpp \
    src/ipc.cpp \
    src/recorder.cpp \
    src/retention.cpp \
    src/schedule.cpp \
    src/store.cpp \
    src/theme.cpp \
    src/transcriber.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
