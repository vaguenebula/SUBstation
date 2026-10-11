// MIDI inputs from other tracks in the device view (DeviceFrame.qml's MIDI From
// button, DeviceInfo's menu): a device that plays notes (an instrument, or an
// effect with a MIDI input, as SUB Test Note Effect has, standing in for a
// vocoder or a pitch corrector) has a note button in its title bar, lit while
// it takes another track's notes, its tooltip naming that track; its menu
// lists Own Track and the MIDI tracks it can take notes from. Effects that play
// no notes have none. Driven in a window with a real session and the test
// plug-ins, with synthesized mouse events.

#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QUndoStack>

#include "BridgeTestSupport.h"
#include "DevicePanelTestSupport.h"
#include "TestSupport.h"
#include "audio/EngineBridge.h"
#include "devices/DeviceChainArea.h"
#include "editor/ProjectEditor.h"
#include "model/Device.h"
#include "model/Project.h"
#include "session/Selection.h"

namespace test = sub::app::test;
using sub::app::PluginRef;

class TestUiDevicePanelMidiFrom : public QObject {
    Q_OBJECT

    test::PanelWindow ui_;

    sub::app::Session& session() { return ui_.session(); }
    sub::app::Project& project() { return *session().project(); }
    sub::app::ProjectEditor& editor() { return *session().editor(); }
    QUndoStack& undo() { return *session().undoStack(); }
    QQuickWindow* window() { return ui_.window(); }

    static PluginRef plugin(const QString& name) { return *test::testPlugin(test::testPluginsBundle(), name); }
    QQuickItem* button(const QString& deviceId) { return test::part(ui_.frame(deviceId), "midiFromButton"); }
    QString tip(const QString& deviceId) { return button(deviceId)->property("tooltip").toString(); }

    QStringList openMenu(const QString& deviceId) {
        ui_.area()->scrollTo(deviceId);
        ui_.polish();
        test::click(window(), test::centerOf(button(deviceId)));
        if (!ui_.menuOpened()) return {};
        return test::menuTexts(ui_.menu());
    }

private Q_SLOTS:
    void initTestCase() {
        test::prepareApplication();
        if (!test::haveDisplay()) QSKIP("needs a display: Qt Quick's software renderer draws none of this geometry");
        if (!test::haveTestPlugins(test::testPluginsBundle())) QSKIP("the test plug-ins weren't built");
        sub::ui::setUpApplication();
        QVERIFY(ui_.open());
    }

    void cleanupTestCase() { ui_.close(); }

    void init() {
        if (ui_.menu()->property("visible").toBool()) QVERIFY(ui_.closeMenu());
        project().clear();
        undo().clear();
        ui_.area()->setContentX(0);
        test::moveTo(window(), QPoint(5, 5), {}, Qt::NoButton);
    }

    void theMidiFromButton() {
        const QString keys = editor().addMidiTrack(-1, QStringLiteral("Keys"));
        const QString lead = editor().addMidiTrack(-1, QStringLiteral("Lead"));
        const QString vocals = editor().addAudioTrack(-1, QStringLiteral("Vocals"));
        session().selection()->selectTrack(vocals);
        const QString tuner = editor().addDevice(vocals, sub::app::kPluginKind, -1, plugin(QStringLiteral("SUB Test Note Effect")));
        const QString plain = editor().addDevice(vocals, QStringLiteral("utility"));
        QTRY_VERIFY(session().bridge()->acceptsMidi(vocals, tuner));
        ui_.polish();
        QVERIFY(!button(plain)->isVisible());  // it plays no notes: no button
        QTRY_VERIFY(button(tuner)->isVisible());
        QVERIFY(!button(tuner)->property("checked").toBool());
        QVERIFY(tip(tuner).contains(QStringLiteral("its own track")));
        const QStringList entries = openMenu(tuner);
        QVERIFY2(entries.size() >= 1 && entries.front() == QStringLiteral("Own Track"), qPrintable(entries.join(u'|')));
        QVERIFY(test::menuChecked(ui_.menu(), QStringLiteral("Own Track")));
        ui_.screenshot(QStringLiteral("midi-from-menu"), true);
        QVERIFY(ui_.choose(project().track(keys).name));
        QCOMPARE(project().device(vocals, tuner).midiFrom, keys);
        QTRY_VERIFY(button(tuner)->property("checked").toBool());
        QCOMPARE(tip(tuner), QStringLiteral("MIDI From: %1 (it plays that track's notes)").arg(project().track(keys).name));
        editor().renameTrack(keys, QStringLiteral("Chords"));  // it follows its source's name
        QTRY_VERIFY(tip(tuner).contains(QStringLiteral("Chords")));
        // The other MIDI track, then its own again.
        openMenu(tuner);
        QVERIFY(ui_.choose(project().track(lead).name));
        QCOMPARE(project().device(vocals, tuner).midiFrom, lead);
        openMenu(tuner);
        QVERIFY(ui_.choose(QStringLiteral("Own Track")));
        QCOMPARE(project().device(vocals, tuner).midiFrom, QString());
        QVERIFY(!button(tuner)->property("checked").toBool());
        undo().undo();
        QCOMPARE(project().device(vocals, tuner).midiFrom, lead);
        // Deleting its source takes it away.
        editor().deleteTracks({lead});
        QTRY_VERIFY(!button(tuner)->property("checked").toBool());
        // An instrument has one too (its own track's notes, or another's).
        session().selection()->selectTrack(keys);
        const QString synth = project().track(keys).devices[0].id;
        ui_.polish();
        QTRY_VERIFY(button(synth) && button(synth)->isVisible());
    }
};

QTEST_MAIN(TestUiDevicePanelMidiFrom)
#include "test_ui_device_panel_midi_from.moc"
