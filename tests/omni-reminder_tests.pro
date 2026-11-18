QT += core gui qml quick quickcontrols2 testlib network
CONFIG += c++17 testcase exceptions
TARGET = omni-reminder_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += \
    fakegoogle.h ../src/backend.h ../src/daemon.h ../src/googleapi.h ../src/googlesync.h ../src/holdmachine.h ../src/ipc.h ../src/recorder.h ../src/retention.h \
    ../src/schedule.h ../src/store.h ../src/theme.h ../src/transcriber.h ../src/translator.h
SOURCES += omni-reminder_tests.cpp \
    ../src/backend.cpp ../src/daemon.cpp ../src/googleapi.cpp ../src/googlesync.cpp ../src/holdmachine.cpp ../src/ipc.cpp ../src/recorder.cpp ../src/retention.cpp \
    ../src/schedule.cpp ../src/store.cpp ../src/theme.cpp ../src/transcriber.cpp ../src/translator.cpp
