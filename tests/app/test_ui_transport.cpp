// The transport bar (TransportBar.qml) on a real session: each control
// changes the project, the bridge or the session (tempo, time signature,
// metronome, key, computer keyboard, play/stop/record, count-in, lock
// envelopes, loop, follow) and shows what they have, whoever changed it; the
// position, the CPU and the device. The checked state of Play and Record
// follows the bridge, never the click; no control takes the keyboard. Runs on
// a display (xvfb here). With $SUBSTATION_SCREENS set, it saves a screenshot.

#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "UiTestSupport.h"
#include "audio/EngineBridge.h"
#include "editor/ProjectEditor.h"
#include "mainwindow/TransportState.h"
#include "model/Keys.h"
#include "model/Project.h"
#include "session/ComputerKeyboard.h"
#include "session/Selection.h"

namespace test = sub::app::test;
using sub::ui::TransportState;

namespace {

// The window (at the end of the file: moc skips what follows a raw string).
extern const char* const kWindow;

}  // namespace

class TestUiTransport : public QObject {
    Q_OBJECT

    std::unique_ptr<test::UiSession> ui_;
    QQuickWindow* window_ = nullptr;
    QQuickItem* bar_ = nullptr;
    QList<QQmlError> warnings_;

    sub::app::Session& session() { return ui_->session(); }
    sub::app::Project& project() { return *session().project(); }
    sub::app::ProjectEditor& editor() { return *session().editor(); }
    sub::app::EngineBridge& bridge() { return *session().bridge(); }
    QQuickItem* item(const char* name) { return bar_->findChild<QQuickItem*>(QString::fromLatin1(name)); }
    void click(const char* name) { test::click(window_, test::centerOf(item(name))); }
    bool checked(const char* name) { return item(name)->property("checked").toBool(); }
    QString text(const char* name) { return item(name)->property("text").toString(); }
    QString combo(const char* name) { return item(name)->property("displayText").toString(); }
    void choose(const char* combo, int index) {
        QVERIFY(QMetaObject::invokeMethod(item(combo), "activated", Q_ARG(int, index)));
    }

private Q_SLOTS:
    void initTestCase() {
        test::prepareApplication();
        if (!test::haveDisplay()) QSKIP("needs a display: Qt Quick's software renderer draws none of this geometry");
        sub::ui::setUpApplication();
        ui_ = std::make_unique<test::UiSession>();
        connect(&ui_->qml(), &QQmlEngine::warnings, this, [this](const QList<QQmlError>& list) { warnings_ += list; });
        window_ = ui_->show(kWindow);
        QVERIFY(window_);
        bar_ = window_->findChild<QQuickItem*>(QStringLiteral("transportBar"));
        QVERIFY(bar_);
        QTest::qWait(50);
    }

    void cleanupTestCase() { ui_.reset(); }

    void init() { session().newProject(); }

    void cleanup() {
        const QList<QQmlError> warnings = std::exchange(warnings_, {});
        for (const QQmlError& warning : warnings) qWarning() << warning.toString();
        QVERIFY(warnings.isEmpty());
    }

    // As the Python UI laid it out: 40 px, every control 28 px, PANEL with a BORDER line below.
    void layout() {
        QCOMPARE(bar_->height(), 40.0);
        for (const char* name : {"tempo", "numerator", "denominator", "metronome", "key", "computerKeys", "position",
                                 "play", "stop", "record", "countIn", "reEnable", "lockEnvelopes", "scope", "loop",
                                 "follow"}) {
            QVERIFY2(item(name), name);
            QCOMPARE(item(name)->height(), 28.0);
        }
        auto left = [&](const char* name) { return item(name)->mapToScene(QPointF(0, 0)).x(); };
        QVERIFY(left("tempo") < left("numerator") && left("numerator") < left("metronome"));
        QVERIFY(left("computerKeys") < left("position") && left("position") < left("play"));
        QVERIFY(left("play") < left("stop") && left("stop") < left("record") && left("record") < left("countIn"));
        QVERIFY(left("lockEnvelopes") < left("scope") && left("scope") < left("loop"));
        QVERIFY(left("loop") < left("follow") && left("follow") < left("cpu") && left("cpu") < left("device"));
        QCOMPARE(text("position"), QStringLiteral("  1. 1. 1"));
        QCOMPARE(text("cpu"), QStringLiteral("CPU 0%"));
        QCOMPARE(text("device"), QStringLiteral("No audio device"));
        test::screenshot(window_, QStringLiteral("transport-bar"), QRect(0, 0, window_->width(), 40));
    }

    // Tempo and time signature: the value boxes edit the project and show it.
    void tempoAndTimeSignature() {
        test::wheel(window_, test::centerOf(item("tempo")), 1);
        QCOMPARE(project().tempo(), 122.5);  // ten steps of 0.25 a notch
        test::wheel(window_, test::centerOf(item("tempo")), 1);
        QCOMPARE(project().tempo(), 125.0);
        QCOMPARE(session().undoStack()->count(), 1);  // (one wheel run: one undo step)
        editor().setTempo(98.0);
        QCOMPARE(item("tempo")->property("value").toDouble(), 98.0);

        test::wheel(window_, test::centerOf(item("numerator")), 1);
        QCOMPARE(project().timeSignature().numerator, 5);
        test::wheel(window_, test::centerOf(item("denominator")), 1);
        QCOMPARE(project().timeSignature().denominator, 8);
        QCOMPARE(item("denominator")->property("value").toDouble(), 8.0);
        editor().setTimeSignature(3, 4);
        QCOMPARE(item("numerator")->property("value").toDouble(), 3.0);
        QCOMPARE(item("denominator")->property("value").toDouble(), 4.0);
        session().undoStack()->undo();
        QCOMPARE(item("numerator")->property("value").toDouble(), 5.0);
    }

    // The metronome, the key and the computer keyboard.
    void metronomeKeyAndKeyboard() {
        click("metronome");
        QVERIFY(bridge().metronome() && checked("metronome"));
        click("metronome");
        QVERIFY(!bridge().metronome() && !checked("metronome"));

        QCOMPARE(combo("key"), QStringLiteral("No Key"));
        const std::vector<sub::app::Key> keys = sub::app::allKeys();
        choose("key", 4);
        QVERIFY(project().key().has_value());
        QCOMPARE(*project().key(), keys[3]);
        QCOMPARE(combo("key"), keys[3].label());
        editor().setKey(keys[10]);
        QCOMPARE(item("key")->property("currentIndex").toInt(), 11);
        choose("key", 0);
        QVERIFY(!project().key().has_value());

        click("computerKeys");
        QVERIFY(session().computerKeyboard()->enabled() && checked("computerKeys"));
        QCOMPARE(item("computerKeys")->property("tooltip").toString(), session().computerKeyboard()->toolTip());
        QVERIFY(item("computerKeys")->property("tooltip").toString().contains(QStringLiteral("C3")));
        session().computerKeyboard()->setEnabled(false);
        QVERIFY(!checked("computerKeys"));
    }

    // Play, Stop, Record: they call the session; Play's and Record's state is the bridge's.
    void transportButtons() {
        session().selection()->setInsert(4.0);
        click("play");
        QVERIFY(bridge().isPlaying());
        QVERIFY(checked("play"));
        click("play");
        QVERIFY(!bridge().isPlaying() && !checked("play"));
        QCOMPARE(bridge().position(), 4.0);  // back where it started
        bridge().play();
        QVERIFY(checked("play"));  // (from the bridge, not a click)
        click("stop");
        QVERIFY(!bridge().isPlaying());
        QCOMPARE(bridge().position(), 4.0);
        click("stop");  // stopped: to the start
        QCOMPARE(bridge().position(), 0.0);

        QSignalSpy messages(&session(), &sub::app::Session::statusMessage);
        click("record");  // nothing armed: the bridge says so, and it isn't lit
        QVERIFY(!bridge().isRecording() && !checked("record"));
        QCOMPARE(messages.size(), 1);

        // None of them takes the keyboard (Space stays play/stop).
        for (const char* name : {"play", "stop", "record", "metronome", "loop"}) QVERIFY(!item(name)->hasActiveFocus());
    }

    // The position, as bar. beat. sixteenth of the project's time signature.
    void position() {
        bridge().locate(6.0);
        QTRY_COMPARE(text("position"), QStringLiteral("  2. 3. 1"));
        bridge().locate(6.25);
        QTRY_COMPARE(text("position"), QStringLiteral("  2. 3. 2"));
        editor().setTimeSignature(3, 4);
        QCOMPARE(text("position"), QStringLiteral("  3. 1. 2"));
        QCOMPARE(TransportState::formatPosition(0.0, 4, 4), QStringLiteral("  1. 1. 1"));
        QCOMPARE(TransportState::formatPosition(4.0 * 120, 4, 4), QStringLiteral("121. 1. 1"));
        QCOMPARE(TransportState::formatPosition(1.5, 6, 8), QStringLiteral("  1. 4. 1"));
    }

    // Count-in, Lock Envelopes, Loop and Re-Enable Automation.
    void countInLockAndLoop() {
        QCOMPARE(combo("countIn"), QStringLiteral("No Count-In"));
        choose("countIn", 2);
        QCOMPARE(session().countInBars(), 2);
        QCOMPARE(QSettings().value(QStringLiteral("transport/count_in_bars")).toInt(), 2);
        QCOMPARE(combo("countIn"), QStringLiteral("Count-In 2 Bars"));
        session().setCountInBars(0);
        QCOMPARE(item("countIn")->property("currentIndex").toInt(), 0);

        click("lockEnvelopes");
        QVERIFY(project().automationLocked() && checked("lockEnvelopes"));
        editor().setAutomationLocked(false);
        QVERIFY(!checked("lockEnvelopes"));

        click("loop");
        QVERIFY(project().loopEnabled() && checked("loop"));
        session().undoStack()->undo();
        QVERIFY(!project().loopEnabled() && !checked("loop"));

        QVERIFY(!item("reEnable")->isEnabled());  // nothing overridden
        QVERIFY(!checked("reEnable"));
    }

    // A project opened shows its own settings at once: its tempo, Lock Envelopes
    // and the loop, not what the bar showed before (it kept 120 until moved).
    void openingAProjectShowsItsSettings() {
        test::TempDir dir;
        editor().setTempo(160.0);
        editor().setAutomationLocked(true);
        editor().setLoopEnabled(true);
        const QString path = dir.path(QStringLiteral("fast.gilproj"));
        QVERIFY(session().saveProjectAs(path));
        session().newProject();
        QCOMPARE(item("tempo")->property("value").toDouble(), 120.0);
        QVERIFY(!checked("lockEnvelopes") && !checked("loop"));

        QVERIFY(session().openProject(path));
        QCOMPARE(item("tempo")->property("value").toDouble(), 160.0);
        QVERIFY(checked("lockEnvelopes") && checked("loop"));
    }

    // Follow: the arrangement's.
    void follow() {
        auto* arrangement = window_->findChild<QObject*>(QStringLiteral("arrangement"));
        QVERIFY(checked("follow"));
        click("follow");
        QVERIFY(!arrangement->property("follow").toBool() && !checked("follow"));
        arrangement->setProperty("follow", true);
        QVERIFY(checked("follow"));
    }

    // The CPU load every 15th meter update; the device, and a click on it.
    void cpuAndDevice() {
        for (int i = 0; i < 14; ++i) Q_EMIT bridge().metersUpdated();
        QCOMPARE(text("cpu"), QStringLiteral("CPU 0%"));
        Q_EMIT bridge().metersUpdated();
        QCOMPARE(text("cpu"), QStringLiteral("CPU %1%").arg(QString::number(bridge().cpuLoad() * 100, 'f', 0)));

        QSignalSpy preferences(bar_, SIGNAL(preferencesRequested()));
        click("device");
        QCOMPARE(preferences.size(), 1);
        QCOMPARE(bar_->property("transportState").value<QObject*>()->property("deviceToolTip").toString(),
                 QStringLiteral("No audio output is open. Click to open Preferences."));
    }
};

namespace {

const char* const kWindow = R"(
import QtQuick
import SUBstation

Window {
    width: 1440
    height: 120
    color: Theme.window

    // The arrangement's interface the bar uses: its follow.
    Item {
        id: fakeArrangement
        objectName: "arrangement"
        property bool follow: true
    }

    TransportBar {
        id: bar
        width: parent.width
        arrangement: fakeArrangement
    }
}
)";

}  // namespace

QTEST_MAIN(TestUiTransport)
#include "test_ui_transport.moc"
