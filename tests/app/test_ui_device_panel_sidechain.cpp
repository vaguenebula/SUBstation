// Sidechains in the device view (DeviceFrame.qml's sidechain button,
// DeviceInfo's menu): a device with a sidechain input has a sidechain button in
// its title bar, lit while it has one, its tooltip naming the source and where
// it is taken; its menu lists the tracks, groups and returns it can come from
// (greyed out where that would close a cycle) and where it is taken, along the
// source's signal as in Ableton: Pre FX (before its devices; after a MIDI
// track's instrument), after one of its devices, Post FX (before its fader) or
// Post Mixer (after it). The master's devices take any track. Built-in devices
// with a sidechain input (the Compressor, the Sidechain device, whose hint over
// its curve opens the menu) have the button too. Driven in a window with a real
// session and the test plug-ins, with synthesized mouse events.

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
#include "model/Automation.h"
#include "model/Device.h"
#include "model/Project.h"
#include "session/DeviceSelection.h"
#include "session/Selection.h"

namespace test = sub::app::test;
using sub::app::PluginRef;
using sub::app::Sidechain;

class TestUiDevicePanelSidechain : public QObject {
    Q_OBJECT

    test::PanelWindow ui_;

    sub::app::Session& session() { return ui_.session(); }
    sub::app::Project& project() { return *session().project(); }
    sub::app::ProjectEditor& editor() { return *session().editor(); }
    QUndoStack& undo() { return *session().undoStack(); }
    QQuickWindow* window() { return ui_.window(); }

    static PluginRef plugin(const QString& name) { return *test::testPlugin(test::testPluginsBundle(), name); }
    QQuickItem* frame(const QString& deviceId) { return ui_.frame(deviceId); }
    QQuickItem* button(const QString& deviceId) { return test::part(frame(deviceId), "sidechainButton"); }
    QString tip(const QString& deviceId) { return button(deviceId)->property("tooltip").toString(); }
    std::optional<Sidechain> sidechain(const QString& trackId, const QString& deviceId) {
        return project().device(trackId, deviceId).sidechain;
    }

    // The device's sidechain menu, opened with a click on its button: its entries ("" a separator).
    QStringList openMenu(const QString& deviceId) {
        ui_.area()->scrollTo(deviceId);
        ui_.polish();
        test::click(window(), test::centerOf(button(deviceId)));
        if (!ui_.menuOpened()) return {};
        return test::menuTexts(ui_.menu());
    }
    // Chooses an entry of a device's sidechain menu.
    void choose(const QString& deviceId, const QString& entry) {
        QVERIFY(!openMenu(deviceId).isEmpty());
        QVERIFY2(ui_.choose(entry), qPrintable(entry));
    }
    bool checked(const QString& entry) { return test::menuChecked(ui_.menu(), entry); }

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
        ui_.messages.clear();
        ui_.area()->setContentX(0);
        test::moveTo(window(), QPoint(5, 5), {}, Qt::NoButton);
    }

    void theSidechainButton() {
        const QString kick = editor().addAudioTrack(-1, QStringLiteral("Kick"));
        const QString eq = editor().addDevice(kick, QStringLiteral("utility"));
        const QString bass = editor().addAudioTrack(-1, QStringLiteral("Bass"));
        const QString group = editor().groupTracks({bass});
        const QString groupName = project().track(group).name;
        const QString ret = editor().addReturnTrack();
        const QString retName = project().track(ret).name;
        session().selection()->selectTrack(bass);
        const QString keyed = editor().addDevice(bass, sub::app::kPluginKind, -1, plugin(QStringLiteral("SUB Test Sidechain")));
        const QString plain = editor().addDevice(bass, sub::app::kPluginKind, -1, plugin(QStringLiteral("SUB Test Effect")));
        ui_.polish();
        QVERIFY(!button(plain)->isVisible());  // no sidechain input: no button
        QVERIFY(button(keyed)->isVisible());
        QVERIFY(!button(keyed)->property("checked").toBool());
        QVERIFY(tip(keyed).contains(QStringLiteral("none")));

        QCOMPARE(openMenu(keyed),
                 (QStringList{"No Sidechain", "", "Kick", groupName + " (this track feeds it)", retName}));
        QVERIFY(checked(QStringLiteral("No Sidechain")));
        QVERIFY(!test::menuEnabled(ui_.menu(), groupName + " (this track feeds it)"));
        ui_.screenshot(QStringLiteral("sidechain-menu"), true);
        QVERIFY(ui_.choose(QStringLiteral("Kick")));
        QCOMPARE(sidechain(bass, keyed), (Sidechain{kick, sub::app::kPostFader}));
        QVERIFY(button(keyed)->property("checked").toBool());
        QCOMPARE(tip(keyed), QStringLiteral("Sidechain: Kick, Post Mixer"));

        QStringList entries = openMenu(keyed);  // now with the taps
        QCOMPARE(entries.mid(entries.size() - 4), (QStringList{"Pre FX", "After Utility", "Post FX", "Post Mixer"}));
        QVERIFY(checked(QStringLiteral("Kick")) && checked(QStringLiteral("Post Mixer")));
        QVERIFY(ui_.choose(QStringLiteral("Pre FX")));
        QCOMPARE(sidechain(bass, keyed), (Sidechain{kick, sub::app::kPreFx}));
        QCOMPARE(tip(keyed), QStringLiteral("Sidechain: Kick, Pre FX"));
        openMenu(keyed);
        QVERIFY(checked(QStringLiteral("Pre FX")));
        QVERIFY(ui_.choose(QStringLiteral("After Utility")));
        QCOMPARE(sidechain(bass, keyed), (Sidechain{kick, eq}));
        QCOMPARE(tip(keyed), QStringLiteral("Sidechain: Kick, After Utility"));
        editor().removeDevice(kick, eq);  // before the fader, until it comes back
        openMenu(keyed);
        QVERIFY(checked(QStringLiteral("Post FX")));
        QVERIFY(ui_.closeMenu());
        QCOMPARE(tip(keyed), QStringLiteral("Sidechain: Kick, Post FX"));  // (the kick's chain changed, not the bass's)
        undo().undo();
        QCOMPARE(tip(keyed), QStringLiteral("Sidechain: Kick, After Utility"));
        choose(keyed, QStringLiteral("Post FX"));
        QCOMPARE(sidechain(bass, keyed), (Sidechain{kick, sub::app::kPreFader}));
        choose(keyed, retName);  // another source: taken in the same place
        QCOMPARE(sidechain(bass, keyed), (Sidechain{ret, sub::app::kPreFader}));

        editor().renameTrack(ret, QStringLiteral("Verb"));
        QCOMPARE(tip(keyed), QStringLiteral("Sidechain: Verb, Post FX"));  // it follows its source's name
        choose(keyed, QStringLiteral("No Sidechain"));
        QVERIFY(!sidechain(bass, keyed));
        QVERIFY(!button(keyed)->property("checked").toBool());

        // Deleting the source turns it off.
        choose(keyed, QStringLiteral("Kick"));
        editor().deleteTracks({kick});
        QVERIFY(!button(keyed)->property("checked").toBool());
        QVERIFY(!sidechain(bass, keyed));
        undo().undo();
        QVERIFY(button(keyed)->property("checked").toBool());

        // A MIDI track's own audio is its instrument's: Pre FX is after it, so it isn't listed.
        const QString keys = editor().addMidiTrack(-1, QStringLiteral("Keys"));
        choose(keyed, QStringLiteral("Keys"));
        entries = openMenu(keyed);
        QCOMPARE(entries.mid(entries.size() - 3), (QStringList{"Pre FX", "Post FX", "Post Mixer"}));
        QVERIFY(ui_.closeMenu());
        editor().setDeviceSidechain(bass, keyed, Sidechain{keys, project().track(keys).devices.front().id});
        openMenu(keyed);
        QVERIFY(checked(QStringLiteral("Pre FX")));
        QVERIFY(ui_.closeMenu());
        QCOMPARE(tip(keyed), QStringLiteral("Sidechain: Keys, Pre FX"));
        ui_.screenshot(QStringLiteral("sidechain-lit"));
    }

    void theMastersDevicesTakeAnyTrack() {
        const QString track = editor().addAudioTrack(-1, QStringLiteral("Kick"));
        session().selection()->selectTrack(sub::app::kMaster);
        const QString keyed =
            editor().addDevice(sub::app::kMaster, sub::app::kPluginKind, -1, plugin(QStringLiteral("SUB Test Sidechain")));
        QCOMPARE(openMenu(keyed), (QStringList{"No Sidechain", "", "Kick"}));
        QVERIFY(test::menuEnabled(ui_.menu(), QStringLiteral("Kick")));
        QVERIFY(ui_.choose(QStringLiteral("Kick")));
        QCOMPARE(project().master().devices.front().sidechain, (Sidechain{track, sub::app::kPostFader}));
    }

    void builtInDevicesWithASidechainInput() {
        const QString kick = editor().addAudioTrack(-1, QStringLiteral("Kick"));
        const QString bass = editor().addAudioTrack(-1, QStringLiteral("Bass"));
        session().selection()->selectTrack(bass);
        const QString compressor = editor().addDevice(bass, QStringLiteral("compressor"));
        const QString utility = editor().addDevice(bass, QStringLiteral("utility"));
        const QString ducker = editor().addDevice(bass, QStringLiteral("sidechain"));
        ui_.polish();
        QVERIFY(button(compressor)->isVisible());
        QVERIFY(!button(utility)->isVisible());
        choose(compressor, QStringLiteral("Kick"));
        QCOMPARE(sidechain(bass, compressor), (Sidechain{kick, sub::app::kPostFader}));
        // The Sidechain device's hint over its curve (no sidechain chosen) asks for the menu.
        ui_.area()->scrollTo(ducker);
        ui_.polish();
        QObject* editorItem = frame(ducker)->property("body").value<QObject*>();
        QVERIFY(editorItem);
        QMetaObject::invokeMethod(editorItem, "sidechainMenuRequested");
        QVERIFY(ui_.menuOpened());
        QCOMPARE(test::menuTexts(ui_.menu()).mid(0, 3), (QStringList{"No Sidechain", "", "Kick"}));
        QVERIFY(ui_.choose(QStringLiteral("Kick")));
        QCOMPARE(sidechain(bass, ducker), (Sidechain{kick, sub::app::kPostFader}));
    }
};

QTEST_MAIN(TestUiDevicePanelSidechain)
#include "test_ui_device_panel_sidechain.moc"
