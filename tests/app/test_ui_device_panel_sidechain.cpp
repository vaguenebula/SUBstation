// Sidechains in the device view (DeviceFrame.qml's sidechain button,
// DeviceInfo's menu): a device with a sidechain input has a sidechain button in
// its title bar, lit while it has one, its tooltip naming the source and where
// it is taken; its menu lists the tracks, groups and returns it can come from
// (greyed out where that would close a cycle) and where it is taken, along the
// source's signal as in Ableton: Pre FX (before its devices; after a MIDI
// track's instrument), after one of its devices (in its racks too), Post FX
// (before its fader) or Post Mixer (after it). The master's devices take any track. Built-in devices
// with a sidechain input (the Compressor, the Sidechain device, whose hint over
// its curve opens the menu) have the button too. The tracks are under a search
// field that has the keyboard as the menu opens. Driven in a window with a real
// session and the test plug-ins, with synthesized mouse events.

#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QUndoStack>
#include <QWheelEvent>

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
    // The open menu's search field (MenuSearch), and its list.
    QQuickItem* search() {
        QObject* menu = ui_.menu();
        for (int i = 0; i < menu->property("count").toInt(); ++i) {
            QQuickItem* item = nullptr;
            QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, i));
            if (test::isSearch(item)) return item;
        }
        return nullptr;
    }
    QQuickItem* list() { return test::part(search(), "list"); }
    // The open menu's entries shown: its own (separators aside), and the search field's rows shown.
    QStringList shown() {
        QStringList texts;
        QObject* menu = ui_.menu();
        for (int i = 0; i < menu->property("count").toInt(); ++i) {
            QQuickItem* item = nullptr;
            QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, i));
            const QString text = item ? item->property("text").toString() : QString();
            if (test::isSearch(item)) {
                texts += test::searchTexts(item, true);
            } else if (item && item->isVisible() && !text.isEmpty()) {
                texts << text;
            }
        }
        return texts;
    }
    // The row highlighted in the search field's list (of those shown; -1: none), and the menu's own entry.
    int row() { return search()->property("current").toInt(); }
    int highlighted() { return ui_.menu()->property("currentIndex").toInt(); }
    int indexOf(const QString& entry) {
        QObject* menu = ui_.menu();
        for (int i = 0; i < menu->property("count").toInt(); ++i) {
            QQuickItem* item = nullptr;
            QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, i));
            if (item && item->property("text").toString() == entry) return i;
        }
        return -1;
    }
    // Typed into the window (what has the focus), a key at a time.
    void typeText(const QString& text) {
        for (const QChar ch : text) QTest::keyClick(window(), ch.toLatin1());
    }
    QString focused() {
        QQuickItem* item = window()->activeFocusItem();
        return item ? item->objectName() : QString();
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

    void tapsAfterDevicesInRacks() {
        // Along the source's signal: a rack's chains' devices (named by the rack,
        // and the chain if it has several), then the rack itself.
        const QString kick = editor().addAudioTrack(-1, QStringLiteral("Kick"));
        const QString first = editor().addDevice(kick, QStringLiteral("utility"));
        const QString second = editor().addDevice(kick, QStringLiteral("compressor"));
        const QString rack = editor().groupDevices(kick, {first, second});
        editor().renameRack(kick, rack, QStringLiteral("Glue"));
        editor().addDevice(kick, QStringLiteral("utility"));  // after the rack
        const QString bass = editor().addAudioTrack(-1, QStringLiteral("Bass"));
        session().selection()->selectTrack(bass);
        const QString keyed = editor().addDevice(bass, QStringLiteral("compressor"));
        editor().setDeviceSidechain(bass, keyed, Sidechain{kick});
        QStringList entries = openMenu(keyed);
        QCOMPARE(entries.mid(entries.indexOf(QStringLiteral("Pre FX"))),
                 (QStringList{"Pre FX", "After Glue › Utility", "After Glue › Compressor", "After Glue",
                              "After Utility", "Post FX", "Post Mixer"}));
        QVERIFY(ui_.choose(QStringLiteral("After Glue › Compressor")));
        QCOMPARE(project().device(bass, keyed).sidechain, (Sidechain{kick, second}));
        QCOMPARE(tip(keyed), QStringLiteral("Sidechain: Kick, After Glue › Compressor"));
        // With several chains, the chain's name too.
        const QString wet = editor().addRackChain(kick, rack, -1, QStringLiteral("Wet"));
        editor().addDevice(kick, QStringLiteral("utility"), -1, wet);
        entries = openMenu(keyed);
        const QString dry = project().device(kick, rack).chains.front().name;
        QVERIFY(entries.contains(QStringLiteral("After Glue › %1 › Compressor").arg(dry)));
        QVERIFY(entries.contains(QStringLiteral("After Glue › Wet › Utility")));
        QVERIFY(checked(QStringLiteral("After Glue › %1 › Compressor").arg(dry)));
        QVERIFY(ui_.closeMenu());
        // The device out of the source: before its fader.
        editor().removeDevice(kick, second);
        QCOMPARE(tip(keyed), QStringLiteral("Sidechain: Kick, Post FX"));
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

    void theMenuSearchesTheTracks() {
        // The tracks are a list under a search field (where the separator after
        // No Sidechain was) that has the keyboard as the menu opens: what is
        // typed shows the tracks with every word of it, in any case, the first
        // highlighted; Enter takes the one highlighted.
        const QString kick = editor().addAudioTrack(-1, QStringLiteral("Kick"));
        const QString top = editor().addAudioTrack(-1, QStringLiteral("Snare Top"));
        const QString bottom = editor().addAudioTrack(-1, QStringLiteral("Snare Bottom"));
        const QString bass = editor().addAudioTrack(-1, QStringLiteral("Bass"));
        session().selection()->selectTrack(bass);
        const QString keyed = editor().addDevice(bass, QStringLiteral("compressor"));
        ui_.polish();
        QCOMPARE(openMenu(keyed), (QStringList{"No Sidechain", "", "Kick", "Snare Top", "Snare Bottom"}));
        QCOMPARE(focused(), QStringLiteral("menuSearch"));
        QCOMPARE(row(), -1);  // (nothing typed: nothing highlighted)
        typeText(QStringLiteral("sNARE"));
        QCOMPARE(shown(), (QStringList{"No Sidechain", "Snare Top", "Snare Bottom"}));
        QCOMPARE(row(), 0);
        typeText(QStringLiteral(" bot"));
        QCOMPARE(shown(), (QStringList{"No Sidechain", "Snare Bottom"}));
        ui_.screenshot(QStringLiteral("sidechain-menu-search"), true);
        for (int i = 0; i < 4; ++i) QTest::keyClick(window(), Qt::Key_Backspace);
        QCOMPARE(shown(), (QStringList{"No Sidechain", "Snare Top", "Snare Bottom"}));
        // The arrows move along the rows, and on to the menu's entries; typing
        // there types into the field, and Down comes back into the list.
        QTest::keyClick(window(), Qt::Key_Down);
        QCOMPARE(row(), 1);
        QTest::keyClick(window(), Qt::Key_Up);
        QCOMPARE(row(), 0);
        QTest::keyClick(window(), Qt::Key_Up);
        QCOMPARE(highlighted(), indexOf(QStringLiteral("No Sidechain")));
        QCOMPARE(row(), -1);
        QTest::keyClick(window(), Qt::Key_X);
        QCOMPARE(focused(), QStringLiteral("menuSearch"));
        QCOMPARE(highlighted(), -1);
        QCOMPARE(shown(), QStringList{"No Sidechain"});
        QTest::keyClick(window(), Qt::Key_Backspace);  // (the x: typed at the end)
        QCOMPARE(shown(), (QStringList{"No Sidechain", "Snare Top", "Snare Bottom"}));
        QTest::keyClick(window(), Qt::Key_Up);
        QTest::keyClick(window(), Qt::Key_Space);  // (back to the field: "snare ")
        QCOMPARE(focused(), QStringLiteral("menuSearch"));
        QTest::keyClick(window(), Qt::Key_B);
        QCOMPARE(shown(), (QStringList{"No Sidechain", "Snare Bottom"}));
        QTest::keyClick(window(), Qt::Key_Backspace);
        QTest::keyClick(window(), Qt::Key_Up);
        QTest::keyClick(window(), Qt::Key_Down);
        QCOMPARE(focused(), QStringLiteral("menuSearch"));
        QCOMPARE(row(), 0);
        QTest::keyClick(window(), Qt::Key_Return);
        QTRY_VERIFY(!ui_.menu()->property("visible").toBool());
        QCOMPARE(sidechain(bass, keyed), (Sidechain{top, sub::app::kPostFader}));

        // Open again: all of them, nothing typed; a track that would close a
        // cycle shown but passed by; Down past the last row goes on to the
        // taps, and Up from there back to it.
        editor().setTrackOutput(bass, sub::app::Output::track(bottom));
        QStringList entries = openMenu(keyed);
        QCOMPARE(entries.mid(0, 5), (QStringList{"No Sidechain", "", "Kick", "Snare Top", "Snare Bottom (this track feeds it)"}));
        QVERIFY(!test::menuEnabled(ui_.menu(), QStringLiteral("Snare Bottom (this track feeds it)")));
        QTest::keyClick(window(), Qt::Key_Return);
        QVERIFY(ui_.menu()->property("visible").toBool());
        typeText(QStringLiteral("  "));  // (only spaces: nothing typed either)
        QTest::keyClick(window(), Qt::Key_Return);
        QVERIFY(ui_.menu()->property("visible").toBool());
        QTest::keyClick(window(), Qt::Key_Backspace);
        QTest::keyClick(window(), Qt::Key_Backspace);
        typeText(QStringLiteral("bottom"));
        QCOMPARE(shown().mid(0, 2), (QStringList{"No Sidechain", "Snare Bottom (this track feeds it)"}));
        QCOMPARE(row(), -1);
        QTest::keyClick(window(), Qt::Key_Return);
        QVERIFY(ui_.menu()->property("visible").toBool());
        for (int i = 0; i < 6; ++i) QTest::keyClick(window(), Qt::Key_Backspace);
        QTest::keyClick(window(), Qt::Key_Down);
        QCOMPARE(row(), 0);
        QTest::keyClick(window(), Qt::Key_Down);
        QCOMPARE(row(), 1);
        QTest::keyClick(window(), Qt::Key_Down);
        QCOMPARE(highlighted(), indexOf(QStringLiteral("Pre FX")));
        QTest::keyClick(window(), Qt::Key_Up);
        QCOMPARE(focused(), QStringLiteral("menuSearch"));
        QCOMPARE(row(), 1);
        // Enter on one of the menu's entries highlighted (a tap) takes it.
        QTest::keyClick(window(), Qt::Key_Down);
        QCOMPARE(highlighted(), indexOf(QStringLiteral("Pre FX")));
        QTest::keyClick(window(), Qt::Key_Return);
        QTRY_VERIFY(!ui_.menu()->property("visible").toBool());
        QCOMPARE(sidechain(bass, keyed), (Sidechain{top, sub::app::kPreFx}));
        // Esc closes it, nothing chosen; Enter on a row highlighted with the
        // arrows takes it (where the old one was taken).
        openMenu(keyed);
        QTest::keyClick(window(), Qt::Key_Escape);
        QTRY_VERIFY(!ui_.menu()->property("visible").toBool());
        QCOMPARE(sidechain(bass, keyed), (Sidechain{top, sub::app::kPreFx}));
        openMenu(keyed);
        QTest::keyClick(window(), Qt::Key_Down);
        QTest::keyClick(window(), Qt::Key_Return);
        QTRY_VERIFY(!ui_.menu()->property("visible").toBool());
        QCOMPARE(sidechain(bass, keyed), (Sidechain{kick, sub::app::kPreFx}));
    }

    void manyTracksScroll() {
        // However many tracks there are, the list shows 8 rows at a time and
        // scrolls: with the wheel, and to keep the row the arrows reach in view.
        QStringList names;
        QStringList ids;
        for (int i = 1; i <= 30; ++i) {
            names << QStringLiteral("Track %1").arg(i);
            ids << editor().addAudioTrack(-1, names.back());
        }
        const QString bass = editor().addAudioTrack(-1, QStringLiteral("Bass"));
        session().selection()->selectTrack(bass);
        const QString keyed = editor().addDevice(bass, QStringLiteral("compressor"));
        ui_.polish();
        QCOMPARE(openMenu(keyed).mid(2), names);
        QQuickItem* rows = list();
        QVERIFY(rows);
        const qreal rowHeight = search()->property("rowHeight").toReal();
        QVERIFY(rowHeight > 0);
        QCOMPARE(rows->height(), 8 * rowHeight);
        QVERIFY(ui_.menu()->property("height").toReal() < 12 * rowHeight);
        QCOMPARE(rows->property("contentY").toReal(), 0.0);
        ui_.screenshot(QStringLiteral("sidechain-menu-many"), true);

        // The arrows: down to the 12th, in view. (First: on some platforms the
        // wheel's event below hovers a row, which highlights it.)
        for (int i = 0; i < 12; ++i) QTest::keyClick(window(), Qt::Key_Down);
        QCOMPARE(row(), 11);
        const qreal y = 11 * rowHeight - rows->property("contentY").toReal();
        QVERIFY2(y > 0 && y + rowHeight <= rows->height(), qPrintable(QString::number(y)));
        // The wheel scrolls it on.
        const qreal before = rows->property("contentY").toReal();
        const QPoint over = test::centerOf(rows);
        QWheelEvent wheel(QPointF(over), QPointF(window()->mapToGlobal(over)), QPoint(), QPoint(0, -120), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        QGuiApplication::sendEvent(window(), &wheel);
        QTRY_VERIFY(rows->property("contentY").toReal() > before);
        QVERIFY(ui_.menu()->property("visible").toBool());
        // Typing: back to the top, the rows that match.
        typeText(QStringLiteral("track 2"));
        QCOMPARE(rows->property("contentY").toReal(), 0.0);
        QCOMPARE(row(), 0);
        QCOMPARE(test::searchTexts(search(), true).mid(0, 3), (QStringList{"Track 2", "Track 12", "Track 20"}));
        QCOMPARE(rows->height(), 8 * rowHeight);  // (as tall while filtering)
        QTest::keyClick(window(), Qt::Key_Return);
        QTRY_VERIFY(!ui_.menu()->property("visible").toBool());
        QCOMPARE(sidechain(bass, keyed), (Sidechain{ids[1], sub::app::kPostFader}));
        // A row far down: found (scrolled into view), ticked, clicked.
        openMenu(keyed);
        QVERIFY(checked(QStringLiteral("Track 2")));
        QVERIFY(!checked(QStringLiteral("Track 25")));
        QVERIFY(ui_.choose(QStringLiteral("Track 25")));
        QCOMPARE(sidechain(bass, keyed), (Sidechain{ids[24], sub::app::kPostFader}));
        openMenu(keyed);
        QVERIFY(checked(QStringLiteral("Track 25")) && !checked(QStringLiteral("Track 2")));
        QVERIFY(ui_.closeMenu());
    }
};

QTEST_MAIN(TestUiDevicePanelSidechain)
#include "test_ui_device_panel_sidechain.moc"
