#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QThread>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

#include "backend.h"
#include "daemon.h"
#include "holdmachine.h"
#include "ipc.h"
#include "retention.h"
#include "schedule.h"
#include "store.h"
#include "theme.h"
#include "transcriber.h"
#include "translator.h"

static constexpr qint64 MB = 1000 * 1000;

class OmniReminderTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(m_tools.isValid());
        // Nothing in the tests may reach the real desktop, microphone, or daemon.
        qputenv("OMNI_REMINDER_SOCKET", m_tools.filePath("test.sock").toUtf8());
        qputenv("OMNI_REMINDER_NOTIFY", script("notify", "#!/bin/sh\nprintf '%s|' \"$@\" >> '"
                                                          + m_tools.filePath("notified.log").toUtf8() + "'\necho >> '"
                                                          + m_tools.filePath("notified.log").toUtf8() + "'\n")
                                            .toUtf8());
        // A stand-in for ffmpeg: writes some bytes to the output (its last
        // argument) and finishes when it reads "q", like the real one.
        qputenv("OMNI_REMINDER_FFMPEG", script("ffmpeg", "#!/bin/sh\nfor last; do :; done\n"
                                                         "head -c 4000 /dev/zero > \"$last\"\n"
                                                         "dd bs=1 count=1 status=none >/dev/null\nexit 0\n")
                                            .toUtf8());
        qputenv("OMNI_REMINDER_SUPERWHISPER", fakeSuperwhisper("Buy oat milk").toUtf8());
    }

    // ---------- store ----------

    void storeRoundTripsEveryField()
    {
        QTemporaryDir dir;
        Store store(dir.path());
        Reminder r;
        r.text = "Water the plants";
        r.created = QDateTime(QDate(2026, 11, 1), QTime(8, 0), QTimeZone::UTC);
        r.due = QDateTime(QDate(2026, 11, 2), QTime(9, 30), QTimeZone::UTC);
        r.clip = "20261101-080000.ogg";
        r.clipRecorded = r.created;
        r.clipSeconds = 4.5;
        const int id = store.add(r);
        QVERIFY(store.save());

        Store again(dir.path());
        QVERIFY(again.load());
        QCOMPARE(again.reminders.size(), 1);
        const Reminder &back = again.reminders.first();
        QCOMPARE(back.id, id);
        QCOMPARE(back.text, r.text);
        QCOMPARE(back.created, r.created);
        QCOMPARE(back.due, r.due);
        QCOMPARE(back.clip, r.clip);
        QCOMPARE(back.clipRecorded, r.clipRecorded);
        QCOMPARE(back.clipSeconds, 4.5);
        QCOMPARE(again.nextId, id + 1);
    }

    void storeTreatsMissingFileAsEmpty()
    {
        QTemporaryDir dir;
        Store store(dir.path() + "/not-yet");
        QVERIFY(store.load());
        QVERIFY(store.reminders.isEmpty());
    }

    void storeNeverOverwritesABrokenFile()
    {
        QTemporaryDir dir;
        write(dir.path() + "/reminders.json", "{ this is not json");
        Store store(dir.path());
        QString error;
        QVERIFY(!store.update([](Store &s) { s.add({}); return true; }, &error));
        QVERIFY(error.contains("not valid JSON"));
        QCOMPARE(read(dir.path() + "/reminders.json"), QByteArray("{ this is not json"));
    }

    void storeSavesAtomicallyWithoutLeftovers()
    {
        QTemporaryDir dir;
        Store store(dir.path());
        for (int i = 0; i < 5; ++i)
            QVERIFY(store.update([i](Store &s) { Reminder r; r.text = QString::number(i); s.add(r); return true; }));
        const QStringList files = QDir(dir.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
        QCOMPARE(files, QStringList{"reminders.json"});
        QVERIFY(QJsonDocument::fromJson(read(store.filePath())).isObject());
    }

    void storeTransactionsKeepEachOthersChanges()
    {
        QTemporaryDir dir;
        Store daemon(dir.path()), cli(dir.path());
        QVERIFY(daemon.load());
        QVERIFY(cli.update([](Store &s) { Reminder r; r.text = "from cli"; s.add(r); return true; }));
        // The daemon's copy is stale, but update() re-reads under the lock.
        QVERIFY(daemon.update([](Store &s) { Reminder r; r.text = "from daemon"; s.add(r); return true; }));
        Store check(dir.path());
        QVERIFY(check.load());
        QCOMPARE(check.reminders.size(), 2);
        QCOMPARE(check.reminders[0].id, 1);
        QCOMPARE(check.reminders[1].id, 2);
    }

    // ---------- retention ----------

    void retentionDeletesClipsAfterThirtyDays()
    {
        const QDateTime now(QDate(2026, 11, 17), QTime(12, 0), QTimeZone::UTC);
        const QList<Retention::Clip> clips{
            {"old.ogg", now.addDays(-31), 1000},
            {"boundary.ogg", now.addDays(-30), 1000},
            {"almost.ogg", now.addDays(-30).addSecs(1), 1000},
            {"new.ogg", now.addDays(-1), 1000},
        };
        QCOMPARE(Retention::plan(clips, now), (QStringList{"old.ogg", "boundary.ogg"}));
    }

    void retentionCapsTotalSizeOldestFirst()
    {
        const QDateTime now(QDate(2026, 11, 17), QTime(12, 0), QTimeZone::UTC);
        const QList<Retention::Clip> clips{
            {"c.ogg", now.addDays(-1), 40 * MB},
            {"a.ogg", now.addDays(-3), 40 * MB},
            {"b.ogg", now.addDays(-2), 40 * MB},
        };
        // 120 MB: dropping the oldest (a) leaves 80 MB, under the 100 MB cap.
        QCOMPARE(Retention::plan(clips, now), QStringList{"a.ogg"});
        // Exactly at the cap is fine.
        QCOMPARE(Retention::plan({{"x.ogg", now, 100 * MB}}, now), QStringList{});
        QCOMPARE(Retention::plan({{"x.ogg", now, 100 * MB + 1}}, now), QStringList{"x.ogg"});
    }

    void retentionAppliesAgeBeforeSize()
    {
        const QDateTime now(QDate(2026, 11, 17), QTime(12, 0), QTimeZone::UTC);
        const QList<Retention::Clip> clips{
            {"ancient.ogg", now.addDays(-40), 60 * MB},
            {"mid.ogg", now.addDays(-5), 60 * MB},
            {"fresh.ogg", now.addDays(-1), 30 * MB},
        };
        // Age removes "ancient"; the remaining 90 MB already fit.
        QCOMPARE(Retention::plan(clips, now), QStringList{"ancient.ogg"});
    }

    void retentionEnforcesOnDiskAndKeepsTheReminders()
    {
        QTemporaryDir dir;
        const QDateTime now(QDate(2026, 11, 17), QTime(12, 0), QTimeZone::UTC);
        Store store(dir.path());
        QVERIFY(store.update([&](Store &s) {
            s.add(withClip("expired by age", "a.ogg", now.addDays(-31)));
            s.add(withClip("oldest of the big ones", "b.ogg", now.addDays(-10)));
            s.add(withClip("newer big one", "c.ogg", now.addDays(-2)));
            s.add(withClip("small and new", "d.ogg", now.addDays(-1)));
            return true;
        }));
        makeFile(store.clipPath("a.ogg"), 1 * MB);
        makeFile(store.clipPath("b.ogg"), 60 * MB);
        makeFile(store.clipPath("c.ogg"), 50 * MB);
        makeFile(store.clipPath("d.ogg"), 1 * MB);
        makeFile(store.clipPath("orphan.ogg"), 1000);
        makeFile(store.clipPath("undo-kept.ogg"), 1000);
        makeFile(store.clipPath(".recording.ogg.part"), 1000);
        makeFile(store.clipPath(".crashed.ogg.part"), 1000, now.addDays(-1));

        const auto result = Retention::enforce(store, now, {"undo-kept.ogg"});
        QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY(!QFile::exists(store.clipPath("a.ogg")));
        QVERIFY(!QFile::exists(store.clipPath("b.ogg")));       // 112 MB -> 52 MB
        QVERIFY(QFile::exists(store.clipPath("c.ogg")));
        QVERIFY(QFile::exists(store.clipPath("d.ogg")));
        QVERIFY(!QFile::exists(store.clipPath("orphan.ogg")));
        QVERIFY(QFile::exists(store.clipPath("undo-kept.ogg")));
        QVERIFY(QFile::exists(store.clipPath(".recording.ogg.part")));
        QVERIFY(!QFile::exists(store.clipPath(".crashed.ogg.part")));

        Store check(dir.path());
        QVERIFY(check.load());
        QCOMPARE(check.reminders.size(), 4);
        QCOMPARE(check.reminders[0].text, QString("expired by age"));
        QVERIFY(check.reminders[0].clip.isEmpty());
        QVERIFY(check.reminders[0].clipExpired);
        QVERIFY(check.reminders[1].clipExpired);
        QCOMPARE(check.reminders[2].clip, QString("c.ogg"));
        QVERIFY(!check.reminders[2].clipExpired);
        QCOMPARE(result.expired, (QList<int>{1, 2}));
    }

    void retentionMarksVanishedClipsExpired()
    {
        QTemporaryDir dir;
        const QDateTime now = QDateTime::currentDateTime();
        Store store(dir.path());
        QVERIFY(store.update([&](Store &s) { s.add(withClip("gone", "gone.ogg", now)); return true; }));
        const auto result = Retention::enforce(store, now);
        QVERIFY(result.ok);
        QVERIFY(store.load());
        QVERIFY(store.reminders.first().clipExpired);
    }

    // ---------- tap / hold ----------

    void tapTogglesWithoutKeepingAudio()
    {
        HoldMachine m(400, 300000);
        Spies s(m);
        m.keyDown(1000);
        m.keyUp(1150);
        QCOMPARE(s.started.count(), 1);
        QCOMPARE(s.cancelled.count(), 1);
        QCOMPARE(s.tapped.count(), 1);
        QCOMPARE(s.finished.count(), 0);
        QCOMPARE(s.confirmed.count(), 0);
        QCOMPARE(m.state(), HoldMachine::Idle);
    }

    void holdRecordsUntilRelease()
    {
        HoldMachine m(400, 300000);
        Spies s(m);
        m.keyDown(1000);
        QCOMPARE(m.nextDeadline(), 1400);
        m.tick(1399);
        QCOMPARE(s.confirmed.count(), 0);
        m.tick(1400);
        QCOMPARE(s.confirmed.count(), 1);
        QCOMPARE(m.state(), HoldMachine::Holding);
        QCOMPARE(m.nextDeadline(), 301000);
        m.keyUp(4000);
        QCOMPARE(s.finished.count(), 1);
        QCOMPARE(s.finished.first().first().toBool(), false);
        QCOMPARE(s.tapped.count(), 0);
        QCOMPARE(s.cancelled.count(), 0);
        QCOMPARE(m.state(), HoldMachine::Idle);
    }

    void holdCountsEvenWhenNoTickRan()
    {
        HoldMachine m(400, 300000);
        Spies s(m);
        m.keyDown(0);
        m.keyUp(400);
        QCOMPARE(s.finished.count(), 1);
        QCOMPARE(s.tapped.count(), 0);
    }

    void holdStopsAtTheLengthLimitAndIgnoresTheLateRelease()
    {
        HoldMachine m(400, 300000);
        Spies s(m);
        m.keyDown(0);
        m.tick(400);
        m.tick(300000);
        QCOMPARE(s.finished.count(), 1);
        QCOMPARE(s.finished.first().first().toBool(), true);
        m.keyUp(310000);
        QCOMPARE(s.tapped.count(), 0);
        QCOMPARE(m.state(), HoldMachine::Idle);
    }

    void aLostReleaseIsEndedByTheNextPress()
    {
        HoldMachine m(400, 300000);
        Spies s(m);
        m.keyDown(0);
        m.tick(400);
        m.keyDown(20000); // the release never came
        QCOMPARE(s.finished.count(), 1);
        QCOMPARE(m.state(), HoldMachine::AwaitRelease);
        m.keyUp(20100);
        QCOMPARE(s.tapped.count(), 0);
        QCOMPARE(m.state(), HoldMachine::Idle);
        // And the shortcut works normally afterwards.
        m.keyDown(30000);
        m.keyUp(30100);
        QCOMPARE(s.tapped.count(), 1);
    }

    void releaseArrivingBeforeItsPressIsStillATap()
    {
        HoldMachine m(400, 300000);
        Spies s(m);
        m.keyUp(1120);  // the key-up process reached the daemon first
        m.keyDown(1000);
        QCOMPARE(s.tapped.count(), 1);
        QCOMPARE(s.started.count(), 0);
        QCOMPARE(m.state(), HoldMachine::Idle);
    }

    void aStaleStrayReleaseDoesNotEatTheNextPress()
    {
        HoldMachine m(400, 300000);
        Spies s(m);
        m.keyUp(500);
        m.keyDown(9000);
        QCOMPARE(s.started.count(), 1);
        QCOMPARE(s.tapped.count(), 0);
        QCOMPARE(m.state(), HoldMachine::Pressed);
    }

    void stopAndCancelFromTheWindow()
    {
        HoldMachine m(400, 300000);
        Spies s(m);
        m.keyDown(0);
        m.tick(400);
        m.stop();
        QCOMPARE(s.finished.count(), 1);
        m.keyUp(5000);
        QCOMPARE(s.tapped.count(), 0);

        m.keyDown(10000);
        m.tick(10400);
        m.cancel();
        QCOMPARE(s.cancelled.count(), 1);
        QCOMPARE(s.finished.count(), 1);
        m.keyUp(11000);
        QCOMPARE(s.tapped.count(), 0);
        QCOMPARE(m.state(), HoldMachine::Idle);
    }

    void duplicatePressesAreIgnored()
    {
        HoldMachine m(400, 300000);
        Spies s(m);
        m.keyDown(0);
        m.keyDown(50);
        QCOMPARE(s.started.count(), 1);
        m.keyUp(100);
        QCOMPARE(s.tapped.count(), 1);
    }

    // ---------- due times ----------

    void dueRemindersFireOnceAndInOrder()
    {
        const QDateTime now(QDate(2026, 11, 17), QTime(16, 0));
        QList<Reminder> list;
        list << dueAt(1, now.addSecs(-3600), false)  // overdue (e.g. after a reboot)
             << dueAt(2, now, false)                // due right now
             << dueAt(3, now.addSecs(-60), true)    // already notified
             << dueAt(4, now.addSecs(90), false)    // later
             << dueAt(5, {}, false);                // no time at all
        QCOMPARE(Schedule::dueIds(list, now), (QList<int>{1, 2}));
        list[0].notified = list[1].notified = true;
        QCOMPARE(Schedule::dueIds(list, now), QList<int>{});
        QCOMPARE(Schedule::msUntilNext(list, now), 60000);         // capped
        QCOMPARE(Schedule::msUntilNext(list, now, 600000), 90000);
        list[3].notified = true;
        QCOMPARE(Schedule::msUntilNext(list, now), -1);
    }

    void dueTextIsReadTheWayPeopleWriteIt()
    {
        const QDateTime now(QDate(2026, 11, 17), QTime(16, 0));
        auto parse = [&](const QString &text) {
            bool ok = false;
            const QDateTime at = Schedule::parseDue(text, now, &ok);
            return ok ? (at.isValid() ? at.toString("yyyy-MM-dd HH:mm") : QString("none")) : QString("bad");
        };
        QCOMPARE(parse(""), QString("none"));
        QCOMPARE(parse("18:30"), QString("2026-11-17 18:30"));
        QCOMPARE(parse("09:00"), QString("2026-11-18 09:00"));
        QCOMPARE(parse("9pm"), QString("2026-11-17 21:00"));
        QCOMPARE(parse("+10m"), QString("2026-11-17 16:10"));
        QCOMPARE(parse("in 2 hours"), QString("2026-11-17 18:00"));
        QCOMPARE(parse("+3d"), QString("2026-11-20 16:00"));
        QCOMPARE(parse("tomorrow"), QString("2026-11-18 09:00"));
        QCOMPARE(parse("Tomorrow 8:15"), QString("2026-11-18 08:15"));
        QCOMPARE(parse("today 23:00"), QString("2026-11-17 23:00"));
        QCOMPARE(parse("2026-12-01 14:00"), QString("2026-12-01 14:00"));
        QCOMPARE(parse("2026-12-01T14:00"), QString("2026-12-01 14:00"));
        QCOMPARE(parse("2026-12-01"), QString("2026-12-01 09:00"));
        bool utcOk = false;
        QCOMPARE(Schedule::parseDue("2026-12-01T14:00:00Z", now, &utcOk).toUTC().toString("HH:mm"), QString("14:00"));
        QVERIFY(utcOk);
        QCOMPARE(parse("25:00"), QString("bad"));
        QCOMPARE(parse("whenever"), QString("bad"));
    }

    void overdueRemindersNotifyOnceOnStart()
    {
        QTemporaryDir dir;
        QFile::remove(m_tools.filePath("notified.log"));
        Store store(dir.path());
        QVERIFY(store.update([](Store &s) {
            Reminder overdue;
            overdue.text = "-rf looks like an option";
            overdue.due = QDateTime::currentDateTime().addSecs(-7200);
            s.add(overdue);
            Reminder later;
            later.text = "later";
            later.due = QDateTime::currentDateTime().addDays(1);
            s.add(later);
            return true;
        }));

        Backend backend(dir.path());
        backend.checkDue();
        QTRY_VERIFY(QFile::exists(m_tools.filePath("notified.log")));
        QTRY_COMPARE(lines(m_tools.filePath("notified.log")).size(), 1);
        const QString call = lines(m_tools.filePath("notified.log")).first();
        // "--" keeps a reminder starting with a dash from being read as an option.
        QVERIFY2(call.contains("|--|Reminder|-rf looks like an option|"), qPrintable(call));

        backend.checkDue();
        Backend restarted(dir.path());
        restarted.checkDue();
        QTest::qWait(300);
        QCOMPARE(lines(m_tools.filePath("notified.log")).size(), 1);
        QVERIFY(store.load());
        QVERIFY(store.reminders[0].notified);
        QVERIFY(!store.reminders[1].notified);
    }

    void changingTheDueTimeNotifiesAgain()
    {
        QTemporaryDir dir;
        Backend backend(dir.path());
        const int id = backend.add("stretch", "+1h");
        QVERIFY(id > 0);
        QVERIFY(backend.store().update([&](Store &s) { s.find(id)->notified = true; return true; }));
        QVERIFY(backend.update(id, "stretch", "+2h"));
        QVERIFY(backend.store().load());
        QVERIFY(!backend.store().find(id)->notified);
    }

    // ---------- backend ----------

    void backendUndoesAndRedoes()
    {
        QTemporaryDir dir;
        Backend backend(dir.path());
        const int id = backend.add("one", "");
        backend.add("two", "");
        QCOMPARE(backend.reminders().size(), 2);
        QVERIFY(backend.remove(id));
        QCOMPARE(backend.reminders().size(), 1);
        backend.undo();
        QCOMPARE(backend.reminders().size(), 2);
        backend.redo();
        QCOMPARE(backend.reminders().size(), 1);
        QVERIFY(backend.add("", "") == 0);
        QVERIFY(backend.messageIsError());
        QVERIFY(backend.add("bad time", "whenever") == 0);
        QVERIFY(backend.message().contains("whenever"));
    }

    void backendSeesChangesFromTheCommandLine()
    {
        QTemporaryDir dir;
        Backend backend(dir.path());
        Store cli(dir.path());
        QVERIFY(cli.update([](Store &s) { Reminder r; r.text = "added elsewhere"; s.add(r); return true; }));
        QTRY_COMPARE(backend.reminders().size(), 1);
    }

    void voiceMemoWithoutTextGetsAFallbackTitle()
    {
        QTemporaryDir dir;
        Backend backend(dir.path());
        QDir().mkpath(backend.store().clipsDirectory());
        const QString clip = backend.store().clipPath("20261117-120000.ogg");
        makeFile(clip, 2000);
        QSignalSpy edit(&backend, &Backend::editRequested);
        const int id = backend.addVoiceMemo(clip, QDateTime::currentDateTime(), 2.5, "", "No speech detected");
        QVERIFY(id > 0);
        QCOMPARE(edit.count(), 1);
        QVERIFY(backend.store().load());
        const Reminder *r = backend.store().find(id);
        QVERIFY(r->text.startsWith("Voice memo "));
        QCOMPARE(r->clip, QString("20261117-120000.ogg"));
        QVERIFY(backend.message().contains("No speech detected"));
        QVERIFY(QFile::exists(clip));
    }

    // ---------- transcription ----------

    void superwhisperRepliesAreRead()
    {
        QString error;
        QCOMPARE(Transcriber::parseStarted(R"({"ok":true,"session_id":4})", &error), 4);
        QCOMPARE(Transcriber::parseStarted(R"({"error":"Not ready: transcribing"})", &error), 0);
        QCOMPARE(Transcriber::parseStarted(R"({"error":"Could not decode audio file"})", &error), -1);
        QCOMPARE(error, QString("Could not decode audio file"));

        QString text, warning;
        const QByteArray status = R"({"phase":"idle","last":{"session_id":4,"createdAt":1000,"text":"Hi","warning":null}})";
        QVERIFY(Transcriber::parseStatus(status, 4, 999, &text, &warning));
        QCOMPARE(text, QString("Hi"));
        QVERIFY(!Transcriber::parseStatus(status, 5, 999, &text, &warning));   // another session
        QVERIFY(!Transcriber::parseStatus(status, 4, 2000, &text, &warning));  // an old result
        QVERIFY(!Transcriber::parseStatus(R"({"last":null})", 4, 0, &text, &warning));
    }

    void transcriberWaitsWhileSuperwhisperIsBusy()
    {
        Transcriber transcriber(fakeSuperwhisper("Pick up the parcel", 2));
        transcriber.setPollMs(20);
        QSignalSpy done(&transcriber, &Transcriber::transcribed);
        transcriber.transcribe("/tmp/clip.ogg");
        QVERIFY(done.wait(8000));
        QCOMPARE(done.first().at(1).toString(), QString("Pick up the parcel"));
        QVERIFY(done.first().at(2).toString().isEmpty());
    }

    void transcriberReportsFailures()
    {
        Transcriber missing(m_tools.filePath("no-such-superwhisper"));
        QSignalSpy failed(&missing, &Transcriber::transcribed);
        missing.transcribe("/tmp/clip.ogg");
        QVERIFY(failed.wait(3000));
        QVERIFY(failed.first().at(2).toString().contains("not available"));

        Transcriber silent(fakeSuperwhisper(""));
        silent.setPollMs(20);
        QSignalSpy empty(&silent, &Transcriber::transcribed);
        silent.transcribe("/tmp/clip.ogg");
        QVERIFY(empty.wait(3000));
        QVERIFY(empty.first().at(1).toString().isEmpty());
        QCOMPARE(empty.first().at(2).toString(), QString("No speech detected"));
    }

    // ---------- the daemon, end to end with stand-ins ----------

    void daemonTapsToggleAndHoldsRecordAVoiceMemo()
    {
        QTemporaryDir dir;
        Backend backend(dir.path());
        Daemon daemon(&backend);
        QString error;
        QVERIFY2(daemon.listen(&error), qPrintable(error));

        // A tap, over the socket like the shortcut does it.
        const qint64 t = Ipc::monotonicMs();
        QVERIFY(sendFromAnotherThread({{"cmd", "key-down"}, {"t", double(t)}}));
        QVERIFY(sendFromAnotherThread({{"cmd", "key-up"}, {"t", double(t + 120)}}));
        QTRY_VERIFY(backend.overlayVisible());
        backend.setOverlayActive(true);
        daemon.keyDown(Ipc::monotonicMs());
        daemon.keyUp(Ipc::monotonicMs() + 50);
        QVERIFY(!backend.overlayVisible());
        QTRY_COMPARE(QDir(backend.store().clipsDirectory()).entryList(QDir::Files | QDir::Hidden).size(), 0);

        // A hold: recording shows, then the memo is transcribed and opened.
        QSignalSpy edit(&backend, &Backend::editRequested);
        daemon.keyDown(Ipc::monotonicMs());
        QTRY_COMPARE(backend.recordingState(), QString("recording"));
        QTest::qWait(300);
        daemon.keyUp(Ipc::monotonicMs());
        QTRY_VERIFY_WITH_TIMEOUT(edit.count() == 1, 8000);
        QCOMPARE(backend.recordingState(), QString("idle"));
        QVERIFY(backend.overlayVisible());
        QVERIFY(backend.store().load());
        QCOMPARE(backend.store().reminders.size(), 1);
        const Reminder memo = backend.store().reminders.first();
        QCOMPARE(memo.text, QString("Buy oat milk"));
        QVERIFY(QFile::exists(backend.store().clipPath(memo.clip)));
        QVERIFY(memo.clipSeconds > 0);
    }

    // ---------- theme and language (from the Omarchy app template) ----------

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\n");
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void themeFollowsHyprlandRounding()
    {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":0}");
        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        write(response, "{\"int\":6}");
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorReadsTheBundledCatalogs()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "bn\n");
        const QString catalogs = QFINDTESTDATA("../src/i18n");
        Translator translator(dir.path() + "/language", catalogs);
        QCOMPARE(translator.language(), QString("bn"));
        QCOMPARE(translator.translate("Main", "Save"), QString("সংরক্ষণ"));
        QVERIFY(translator.translate("Main", "Unknown string").isNull());
        Translator english(dir.path() + "/none", catalogs);
        QCOMPARE(english.translate("Main", "%n reminders", nullptr, 1), QString("%n reminder"));
        QCOMPARE(english.translate("Main", "%n reminders", nullptr, 3), QString("%n reminders"));
    }

private:
    struct Spies {
        explicit Spies(HoldMachine &m)
            : started(&m, &HoldMachine::captureStarted), cancelled(&m, &HoldMachine::captureCancelled),
              confirmed(&m, &HoldMachine::holdConfirmed), finished(&m, &HoldMachine::captureFinished),
              tapped(&m, &HoldMachine::tapped) {}
        QSignalSpy started, cancelled, confirmed, finished, tapped;
    };

    // The daemon answers on this thread's event loop, so a blocking client
    // has to run elsewhere, as the real key-down process does.
    static bool sendFromAnotherThread(const QJsonObject &request)
    {
        bool sent = false;
        QThread *thread = QThread::create([&] { sent = Ipc::send(request, nullptr, 3000); });
        thread->start();
        while (!thread->wait(10))
            QCoreApplication::processEvents();
        delete thread;
        return sent;
    }

    static Reminder withClip(const QString &text, const QString &clip, const QDateTime &recorded)
    {
        Reminder r;
        r.text = text;
        r.clip = clip;
        r.clipRecorded = recorded;
        return r;
    }

    static Reminder dueAt(int id, const QDateTime &due, bool notified)
    {
        Reminder r;
        r.id = id;
        r.text = QString::number(id);
        r.due = due;
        r.notified = notified;
        return r;
    }

    // A sparse file of the given size, so 60 MB costs no disk.
    static void makeFile(const QString &path, qint64 bytes, const QDateTime &modified = {})
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.resize(bytes));
        if (modified.isValid())
            QVERIFY(file.setFileTime(modified, QFileDevice::FileModificationTime));
    }

    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }

    static QByteArray read(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    static QStringList lines(const QString &path)
    {
        return QString::fromUtf8(read(path)).split('\n', Qt::SkipEmptyParts);
    }

    QString script(const QString &name, const QByteArray &body)
    {
        const QString path = m_tools.filePath(name);
        write(path, body);
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        return path;
    }

    // A stand-in for the Superwhisper CLI: says "busy" `busyTimes` times,
    // then queues session 7, whose result `status` reports with this text.
    QString fakeSuperwhisper(const QString &text, int busyTimes = 0)
    {
        static int counter = 0;
        const QString name = QStringLiteral("superwhisper-%1").arg(++counter);
        const QString busy = m_tools.filePath(name + ".busy");
        write(busy, QByteArray::number(busyTimes));
        const QByteArray warning = text.isEmpty() ? "\"No speech detected\"" : "null";
        return script(name,
                      "#!/bin/sh\n"
                      "case \"$1\" in\n"
                      "transcribe)\n"
                      "  n=$(cat '" + busy.toUtf8() + "')\n"
                      "  if [ \"$n\" -gt 0 ]; then echo $((n-1)) > '" + busy.toUtf8() + "';"
                      " echo '{\"error\":\"Not ready: transcribing\"}'; exit 1; fi\n"
                      "  echo '{\"ok\":true,\"session_id\":7}' ;;\n"
                      "status)\n"
                      "  printf '{\"phase\":\"idle\",\"last\":{\"session_id\":7,\"createdAt\":%s,\"text\":\"" +
                          text.toUtf8() + "\",\"warning\":" + warning + "}}\\n' $(date +%s) ;;\n"
                      "esac\n");
    }

    QTemporaryDir m_tools;
};

QTEST_MAIN(OmniReminderTests)
#include "omni-reminder_tests.moc"
