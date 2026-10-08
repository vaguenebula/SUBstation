// Racks in the device view (DevicePanel.qml's racks: RackDeviceBody,
// RackChainRow, RackMacroKnob, RackMacroMappings, RackChainView): Ctrl+G groups
// the selected devices into a rack (Ctrl+Shift+G ungroups it), which shows its
// macros, and its chain list when asked for; the chain clicked shows its
// devices beside the rack (unless they are hidden), where devices are dropped
// and selected as on the track's own chain; chain mixers and macros edit the
// model; macros added and taken away, renamed in place, automated and
// following their automation; mapping a parameter to a macro from its menu,
// the ranges of what a macro moves, and unmapping it; Ctrl+R renaming the rack
// chain clicked in place; a chain's menu; the view's height staying put.
// Driven in a window with a real session, with synthesized mouse, key and drag
// events. With SUBSTATION_UI_SCREENSHOTS set, screenshots go there.

#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>

#include "DevicePanelTestSupport.h"
#include "TestSupport.h"
#include "audio/BridgeTypes.h"
#include "audio/EngineBridge.h"
#include "controls/KnobItem.h"
#include "controls/ValueBoxItem.h"
#include "devices/DeviceChainArea.h"
#include "editor/ProjectEditor.h"
#include "model/Automation.h"
#include "model/Device.h"
#include "model/Project.h"
#include "session/DeviceSelection.h"
#include "session/Selection.h"

#include <cmath>

namespace test = sub::app::test;
using sub::app::Selection;

class TestUiDevicePanelRacks : public QObject {
    Q_OBJECT

    test::PanelWindow ui_;

    sub::app::Session& session() { return ui_.session(); }
    sub::app::Project& project() { return *session().project(); }
    sub::app::ProjectEditor& editor() { return *session().editor(); }
    sub::app::DeviceSelection& devices() { return *session().deviceSelection(); }
    QUndoStack& undo() { return *session().undoStack(); }
    QQuickWindow* window() { return ui_.window(); }
    sub::ui::DeviceChainArea* area() { return ui_.area(); }

    QStringList ids(const std::vector<sub::app::Device>& list) {
        QStringList out;
        for (const sub::app::Device& device : list) out << device.id;
        return out;
    }
    QStringList chainIds(const QString& trackId, const QString& rackId) {
        QStringList out;
        for (const sub::app::Chain& chain : project().device(trackId, rackId).chains) out << chain.id;
        return out;
    }

    // A new track, selected, with `count` utilities.
    QString shownTrack(QStringList& added, int count = 2) {
        const QString track = editor().addAudioTrack();
        session().selection()->selectTrack(track);
        for (int i = 0; i < count; ++i) added << editor().addDevice(track, QStringLiteral("utility"));
        ui_.polish();
        return track;
    }

    QQuickItem* frame(const QString& deviceId) { return ui_.frame(deviceId); }
    QPoint titleOf(const QString& deviceId) { return test::centerOf(test::part(frame(deviceId), "title")); }
    QQuickItem* row(const QString& rackId, const QString& chainId) {
        ui_.polish();
        return area()->chainRowOf(rackId, chainId);
    }
    // The rack chains shown beside their racks (their brackets), by chain.
    QMap<QString, QQuickItem*> chainViews() {
        ui_.polish();
        QMap<QString, QQuickItem*> views;
        for (QQuickItem* view : test::itemsNamed(ui_.panel(), QStringLiteral("chainView"))) {
            if (view->isVisible()) views.insert(view->property("chainId").toString(), view);
        }
        return views;
    }
    // The main window's Ctrl+R on a rack's chain.
    bool startChainRename(const QString& rackId, const QString& chainId) {
        QVariant started;
        QMetaObject::invokeMethod(ui_.panel(), "startChainRename", Q_RETURN_ARG(QVariant, started),
                                  Q_ARG(QVariant, rackId), Q_ARG(QVariant, chainId));
        return started.toBool();
    }
    // A click on a chain's row (on its name), showing it.
    void clickRow(const QString& rackId, const QString& chainId) {
        test::click(window(), test::centerOf(test::part(row(rackId, chainId), "nameLabel")));
    }
    // A rack's chain list shown (it is hidden by default).
    void showChainList(const QString& trackId, const QString& rackId) {
        editor().setChainListShown(trackId, rackId, true);
        ui_.polish();
    }
    QQuickItem* macroCell(const QString& rackId, int index) {
        ui_.polish();
        return test::itemNamed(frame(rackId), QStringLiteral("macro%1").arg(index));
    }
    QQuickItem* rackBody(const QString& rackId) { return frame(rackId)->property("body").value<QQuickItem*>(); }

private Q_SLOTS:
    void initTestCase() {
        test::prepareApplication();
        if (!test::haveDisplay()) QSKIP("needs a display: Qt Quick's software renderer draws none of this geometry");
        sub::ui::setUpApplication();
        QVERIFY(ui_.open());
    }

    void cleanupTestCase() { ui_.close(); }

    void init() {
        if (ui_.menu()->property("visible").toBool()) QVERIFY(ui_.closeMenu());
        project().clear();
        undo().clear();
        ui_.messages.clear();
        area()->setContentX(0);
        test::moveTo(window(), QPoint(5, 5), {}, Qt::NoButton);
    }

    void groupingAndUngroupingInTheDeviceView() {
        const qreal height = ui_.panel()->height();
        QStringList added;
        const QString track = shownTrack(added);
        const QString a = added[0], b = added[1];
        test::click(window(), titleOf(a));
        test::click(window(), titleOf(b), Qt::ShiftModifier);
        QCOMPARE(devices().selected(), added);
        QCOMPARE(session().selection()->focus(), Selection::Focus::Devices);
        session().groupSelected();  // Ctrl+G, in the device view
        const auto& list = project().track(track).devices;
        QCOMPARE(list.size(), size_t(1));
        QVERIFY(list.front().isRack());
        const QString rack = list.front().id;
        const QString chain = list.front().chains.front().id;
        QVERIFY(frame(rack));
        QCOMPARE(devices().selected(), QStringList{rack});
        QCOMPARE(test::part(frame(rack), "title")->property("text").toString(), QStringLiteral("Audio Effect Rack"));
        QCOMPARE(frame(rack)->width(), 200.0);  // its four macros: the chain list is hidden
        QVERIFY(!row(rack, chain));
        QCOMPARE(test::itemsNamed(frame(rack), QStringLiteral("macro3")).size(), 1);
        QVERIFY(test::itemsNamed(frame(rack), QStringLiteral("macro4")).isEmpty());
        // Its chain shows beside it, with its devices, which select as any.
        QCOMPARE(devices().shownDevices(), (QStringList{rack, a, b}));
        QVERIFY(frame(a) && frame(b));
        QCOMPARE(chainViews().keys(), QStringList{chain});
        QVERIFY(frame(a)->mapToScene(QPointF()).x() > frame(rack)->mapToScene(QPointF(frame(rack)->width(), 0)).x());
        test::click(window(), titleOf(b));
        QCOMPARE(devices().selected(), QStringList{b});
        test::click(window(), titleOf(rack), Qt::ControlModifier);  // another chain's: a selection of its own
        QCOMPARE(devices().selected(), QStringList{rack});
        QCOMPARE(ui_.panel()->height(), height);  // nested devices fit as the others do
        QCOMPARE(frame(a)->height(), frame(rack)->height());
        session().ungroupSelected();  // Ctrl+Shift+G
        QCOMPARE(ids(project().track(track).devices), added);
        QVERIFY(!frame(rack));
        QVERIFY(chainViews().isEmpty());
    }

    void theChainListAndTheChainsDevicesShowWhenAskedFor() {
        QStringList added;
        const QString track = shownTrack(added);
        const QString a = added[0];
        const QString rack = editor().groupDevices(track, {a});
        const QString chain = chainIds(track, rack).front();
        ui_.polish();
        QQuickItem* listButton = test::itemNamed(frame(rack), QStringLiteral("chainListButton"));
        QQuickItem* devicesButton = test::itemNamed(frame(rack), QStringLiteral("devicesButton"));
        QVERIFY(listButton && devicesButton);
        QVERIFY(!listButton->property("checked").toBool() && devicesButton->property("checked").toBool());
        QVERIFY(!test::itemNamed(frame(rack), QStringLiteral("chainList")));  // (not shown)
        const int steps = undo().count();
        test::click(window(), test::centerOf(listButton));  // shows the chain list: the rack widens
        QVERIFY(project().isChainListShown(rack));
        QVERIFY(row(rack, chain));
        QVERIFY(test::itemNamed(frame(rack), QStringLiteral("chainList")));
        QVERIFY(frame(rack)->width() > 400.0);
        QVERIFY(test::itemNamed(frame(rack), QStringLiteral("chainListButton"))->property("checked").toBool());
        QCOMPARE(undo().count(), steps);  // (view state)
        ui_.screenshot(QStringLiteral("rack-chain-list"));
        // The chain's devices hidden: no bracket beside the rack.
        test::click(window(), test::centerOf(test::itemNamed(frame(rack), QStringLiteral("devicesButton"))));
        QVERIFY(!project().areRackDevicesShown(rack));
        QVERIFY(chainViews().isEmpty());
        QVERIFY(!frame(a));
        QCOMPARE(devices().shownDevices(), (QStringList{rack, added[1]}));
        clickRow(rack, chain);  // a chain clicked shows its devices again
        QVERIFY(project().areRackDevicesShown(rack));
        QCOMPARE(chainViews().keys(), QStringList{chain});
        // The rack's own menu shows and hides them too.
        ui_.rightClick(titleOf(rack));
        QVERIFY(ui_.menuOpened());
        QVERIFY(test::menuTexts(ui_.menu()).contains(QStringLiteral("Hide Devices")));
        QVERIFY(ui_.choose(QStringLiteral("Hide Chain List")));
        QVERIFY(!project().isChainListShown(rack));
        QCOMPARE(frame(rack)->width(), 200.0);
        ui_.rightClick(titleOf(rack));
        QVERIFY(ui_.choose(QStringLiteral("Hide Devices")));
        QVERIFY(chainViews().isEmpty());
        ui_.rightClick(titleOf(rack));
        QVERIFY(ui_.choose(QStringLiteral("Show Devices")));
        QCOMPARE(chainViews().keys(), QStringList{chain});
        // Its menu's Add Chain shows the list, where the chain is.
        ui_.rightClick(titleOf(rack));
        QVERIFY(ui_.choose(QStringLiteral("Add Chain")));
        QCOMPARE(chainIds(track, rack).size(), 2);
        QVERIFY(project().isChainListShown(rack));
        QVERIFY(row(rack, chainIds(track, rack)[1]));
    }

    void chainsInTheDeviceView() {
        QStringList added;
        const QString track = shownTrack(added);
        const QString a = added[0], b = added[1];
        const QString rack = editor().groupDevices(track, {a});
        showChainList(track, rack);
        QQuickItem* add = test::itemNamed(frame(rack), QStringLiteral("addChain"));
        QVERIFY(add);
        test::click(window(), test::centerOf(add));  // + Chain
        QStringList chains = chainIds(track, rack);
        QCOMPARE(chains.size(), 2);
        const QString first = chains[0], second = chains[1];
        QVERIFY(row(rack, first) && row(rack, second));
        QCOMPARE(chainViews().keys(), QStringList{first});  // the first one shows
        QVERIFY(row(rack, first)->property("selected").toBool());
        clickRow(rack, second);  // shows the other one
        QCOMPARE(chainViews().keys(), QStringList{second});
        QVERIFY(row(rack, second)->property("selected").toBool());
        QVERIFY(!row(rack, first)->property("selected").toBool());
        QCOMPARE(devices().clickedChain(), second);
        // Dropped into the chain shown: into it (an empty chain shows its hint).
        QQuickItem* view = chainViews().value(second);
        QVERIFY(test::itemNamed(view, QStringLiteral("chainHint")));
        const QPointF viewCenter = view->mapToItem(area(), QPointF(view->width() / 2, view->height() / 2));
        QCOMPARE(area()->dropTarget(viewCenter.x(), viewCenter.y()),
                 (QVariantMap{{"chain", second}, {"index", 0}}));
        auto mime = test::deviceKindsMime({QStringLiteral("utility")});
        test::dragAndDrop(window(), test::centerOf(view), mime.get());
        const auto inSecond = project().chain(track, second).devices;
        QCOMPARE(inSecond.size(), size_t(1));
        QVERIFY(frame(inSecond.front().id));
        // Dragged from the track's chain onto a chain's row: into it, last.
        QQuickItem* firstRow = row(rack, first);
        const QPointF rowCenter = firstRow->mapToItem(area(), QPointF(firstRow->width() / 2, firstRow->height() / 2));
        QCOMPARE(area()->dropTarget(rowCenter.x(), rowCenter.y()), (QVariantMap{{"chain", first}, {"index", 1}}));
        auto moving = test::movedMime(track, {b});
        test::dragAndDrop(window(), test::centerOf(firstRow), moving.get());
        QCOMPARE(ids(project().chain(track, first).devices), (QStringList{a, b}));
        QCOMPARE(ids(project().track(track).devices), QStringList{rack});
        // Its mixer.
        QQuickItem* secondRow = row(rack, second);
        test::click(window(), test::centerOf(test::part(secondRow, "solo")));
        test::click(window(), test::centerOf(test::part(secondRow, "activator")));
        QVERIFY(project().chain(track, second).solo);
        QVERIFY(project().chain(track, second).mute);
        secondRow = row(rack, second);
        QVERIFY(test::part(secondRow, "solo")->property("checked").toBool());
        QVERIFY(!test::part(secondRow, "activator")->property("checked").toBool());
        auto* volume = qobject_cast<sub::ui::ValueBoxItem*>(test::part(secondRow, "volume"));
        QVERIFY(volume);
        Q_EMIT volume->moved(-6.0, QStringLiteral("gesture"));
        QCOMPARE(project().chain(track, second).volumeDb, -6.0);
        QCOMPARE(volume->value(), -6.0);
        editor().renameChain(track, second, QStringLiteral("Wet"));
        QCOMPARE(test::part(row(rack, second), "nameLabel")->property("text").toString(), QStringLiteral("Wet"));
        Q_EMIT session().bridge()->metersUpdated();  // (no error: its meter falls from there)
        test::click(window(), titleOf(rack));
        test::click(window(), test::centerOf(test::part(secondRow, "activator")));
        QVERIFY(!project().chain(track, second).mute);
        ui_.screenshot(QStringLiteral("rack"));
    }

    void macrosInTheDeviceView() {
        QStringList added;
        const QString track = shownTrack(added);
        const QString a = added[0];
        const QString rack = editor().groupDevices(track, {a});
        editor().mapMacro(track, rack, 0, a, QStringLiteral("gain"));
        ui_.polish();
        QQuickItem* cell = test::itemNamed(frame(rack), QStringLiteral("macro0"));
        auto* knob = qobject_cast<sub::ui::KnobItem*>(test::part(cell, "knob"));
        QVERIFY(knob);
        QObject* macro = cell->property("macro").value<QObject*>();
        QVERIFY(macro->property("toolTip").toString().contains(QStringLiteral("Utility: Gain")));
        QVERIFY(macro->property("mapped").toBool());
        QVERIFY(!test::itemNamed(frame(rack), QStringLiteral("macro1"))->property("macro").value<QObject*>()
                     ->property("mapped").toBool());
        const QPoint center = test::centerOf(knob);  // turned all the way up: 600 px
        test::press(window(), center);
        test::moveTo(window(), center - QPoint(0, 300));
        test::moveTo(window(), center - QPoint(0, 600));
        test::release(window(), center - QPoint(0, 600));
        QCOMPARE(project().device(track, rack).params.value(sub::app::macroParam(0)), 1.0);
        QCOMPARE(project().device(track, a).params.value(QStringLiteral("gain")), 24.0);
        QCOMPARE(knob->value(), 1.0);
        // The parameter's menu offers the rack's macros.
        QQuickItem* gain = test::itemNamed(frame(a), QStringLiteral("param_gain"));
        QObject* paramMenu = gain->property("menu").value<QObject*>();
        QMetaObject::invokeMethod(paramMenu, "build");
        QVERIFY(test::menuTexts(paramMenu).contains(QStringLiteral("Map to Macro")));
        QVERIFY(test::menuTexts(paramMenu).contains(QStringLiteral("Unmap from Macro 1")));
        undo().undo();
        QCOMPARE(project().device(track, a).params.value(QStringLiteral("gain")), 0.0);
        // A macro's menu: its automation, renaming it, its mappings, what it moves (to unmap), macros.
        ui_.rightClick(test::centerOf(test::itemNamed(frame(rack), QStringLiteral("macro1"))));
        QVERIFY(ui_.menuOpened());
        const QString nothing = QStringLiteral("Nothing mapped (right-click a parameter in the rack to map it)");
        QCOMPARE(test::menuTexts(ui_.menu()),
                 (QStringList{"Show Automation", "Delete Automation", "", "Rename", "Edit Mappings…", "", nothing, "",
                              "Add Macro", "Remove Last Macro"}));
        QVERIFY(!test::menuEnabled(ui_.menu(), nothing));
        QVERIFY(!test::menuEnabled(ui_.menu(), QStringLiteral("Edit Mappings…")));
        QVERIFY(test::menuEnabled(ui_.menu(), QStringLiteral("Show Automation")));
        QVERIFY(!test::menuEnabled(ui_.menu(), QStringLiteral("Delete Automation")));
        QVERIFY(ui_.closeMenu());
        ui_.rightClick(test::centerOf(cell));
        QVERIFY(ui_.choose(QStringLiteral("Unmap Utility: Gain")));
        QVERIFY(project().device(track, rack).macros.empty());
        QVERIFY(!macro->property("mapped").toBool());
    }

    void chainFadersAreAutomationTargets() {
        QStringList added;
        const QString track = shownTrack(added);
        const QString rack = editor().groupDevices(track, {added[0]});
        const QString chain = chainIds(track, rack).front();
        QMap<QString, QStringList> groups;
        for (const sub::app::ParamGroup& group : session().bridge()->paramGroups(track)) {
            for (const sub::app::ParamSpec& spec : group.specs) groups[group.id] << spec.key;
        }
        QStringList keys{sub::app::automation::deviceOnKey(rack)};  // its switch, its macros, then its chains' faders
        for (int i = 0; i < sub::app::kDefaultMacroCount; ++i)
            keys << sub::app::automation::deviceKey(rack, sub::app::macroParam(i));
        keys << sub::app::automation::chainKey(rack, chain, sub::app::automation::kChainVolume)
             << sub::app::automation::chainKey(rack, chain, sub::app::automation::kChainPan);
        QCOMPARE(groups.value(rack), keys);
        QVERIFY(groups.value(added[0]).contains(sub::app::automation::deviceKey(added[0], QStringLiteral("gain"))));
    }

    void ctrlRRenamesTheRackChainClicked() {
        QStringList added;
        const QString track = shownTrack(added);
        const QString trackName = project().track(track).name;
        const QString rack = editor().groupDevices(track, {added[0]});
        showChainList(track, rack);
        test::click(window(), test::centerOf(test::itemNamed(frame(rack), QStringLiteral("addChain"))));
        const QString second = chainIds(track, rack)[1];
        test::doubleClick(window(), test::centerOf(test::part(row(rack, second), "nameLabel")));
        QVERIFY(!row(rack, second)->property("renaming").toBool());  // a double-click doesn't rename
        session().selection()->selectTrack(track, true);
        clickRow(rack, second);
        QCOMPARE(session().selection()->focus(), Selection::Focus::Devices);
        const QVariantMap target = session().renameTarget();  // Ctrl+R: the chain clicked
        QCOMPARE(target.value("kind").toString(), QStringLiteral("chain"));
        QCOMPARE(target.value("rackId").toString(), rack);
        QCOMPARE(target.value("chainId").toString(), second);
        QVERIFY(startChainRename(rack, second));
        QQuickItem* field = test::part(row(rack, second), "renameField");
        QVERIFY(field->isVisible() && field->hasActiveFocus());
        QCOMPARE(field->property("selectedText").toString(), QStringLiteral("Chain 2"));
        QTest::keyClick(window(), Qt::Key_W);
        QTest::keyClick(window(), Qt::Key_E);
        QTest::keyClick(window(), Qt::Key_T);
        QTest::keyClick(window(), Qt::Key_Return);
        QCOMPARE(project().chain(track, second).name, QStringLiteral("wet"));
        QVERIFY(!field->isVisible());
        QCOMPARE(project().track(track).name, trackName);
        // Escape keeps the name typed too; an empty one keeps the old name.
        QVERIFY(startChainRename(rack, second));
        field->setProperty("text", QStringLiteral("Wet"));
        QTest::keyClick(window(), Qt::Key_Escape);
        QCOMPARE(project().chain(track, second).name, QStringLiteral("Wet"));
        QVERIFY(startChainRename(rack, second));
        field->setProperty("text", QString());
        QTest::keyClick(window(), Qt::Key_Return);
        QCOMPARE(project().chain(track, second).name, QStringLiteral("Wet"));
        // A device clicked since: Ctrl+R renames the track again.
        test::click(window(), titleOf(rack));
        QCOMPARE(session().renameTarget().value("kind").toString(), QStringLiteral("track"));
        // Its chain list hidden: it shows, and the chain is renamed there.
        editor().setChainListShown(track, rack, false);
        QVERIFY(!row(rack, second));
        QVERIFY(startChainRename(rack, second));
        QVERIFY(project().isChainListShown(rack));
        QTRY_VERIFY(row(rack, second) && row(rack, second)->property("renaming").toBool());
        QTest::keyClick(window(), Qt::Key_Escape);
        // A folded rack's chains aren't shown: nothing to rename there.
        editor().setDevicesFolded(track, {rack}, true);
        QVERIFY(!startChainRename(rack, second));
    }

    void aChainsMenu() {
        QStringList added;
        const QString track = shownTrack(added);
        const QString rack = editor().groupDevices(track, {added[0]});
        const QString first = chainIds(track, rack).front();
        showChainList(track, rack);
        ui_.rightClick(test::centerOf(test::part(row(rack, first), "nameLabel")));
        QVERIFY(ui_.menuOpened());
        QCOMPARE(test::menuTexts(ui_.menu()),
                 (QStringList{"Rename", "Duplicate", "Delete", "", "Add Chain", "", "Show Volume Automation",
                              "Show Pan Automation"}));
        ui_.screenshot(QStringLiteral("chain-menu"), true);
        QVERIFY(ui_.choose(QStringLiteral("Duplicate")));
        QStringList chains = chainIds(track, rack);
        QCOMPARE(chains.size(), 2);
        QCOMPARE(chains.front(), first);
        QCOMPARE(project().chain(track, chains[1]).devices.size(), size_t(1));  // (a copy, with its device)
        QTRY_COMPARE(devices().clickedChain(), first);  // after its menu, the chain shows
        ui_.rightClick(test::centerOf(test::part(row(rack, chains[1]), "nameLabel")));
        QVERIFY(ui_.choose(QStringLiteral("Show Volume Automation")));
        QCOMPARE(project().automationView(track).key,
                 sub::app::automation::chainKey(rack, chains[1], sub::app::automation::kChainVolume));
        QTRY_COMPARE(devices().shownChain(rack), chains[1]);
        ui_.rightClick(test::centerOf(test::part(row(rack, chains[1]), "nameLabel")));
        QVERIFY(ui_.choose(QStringLiteral("Add Chain")));
        QCOMPARE(chainIds(track, rack).size(), 3);
        ui_.rightClick(test::centerOf(test::part(row(rack, chains[1]), "nameLabel")));
        QVERIFY(ui_.choose(QStringLiteral("Delete")));
        QCOMPARE(chainIds(track, rack).size(), 2);
        QVERIFY(!chainIds(track, rack).contains(chains[1]));
        ui_.rightClick(test::centerOf(test::part(row(rack, first), "nameLabel")));
        QVERIFY(ui_.choose(QStringLiteral("Rename")));
        QTRY_VERIFY(row(rack, first)->property("renaming").toBool());
        QTest::keyClick(window(), Qt::Key_X);
        QTest::keyClick(window(), Qt::Key_Return);
        QCOMPARE(project().chain(track, first).name, QStringLiteral("x"));
        // A rack's own menu adds one too.
        ui_.rightClick(titleOf(rack));
        QVERIFY(ui_.choose(QStringLiteral("Add Chain")));
        QCOMPARE(chainIds(track, rack).size(), 3);
    }

    void aRackWithChainsAndMacros() {
        QStringList added;
        const QString track = shownTrack(added, 3);
        const QString rack = editor().groupDevices(track, {added[0], added[1]});
        const QString drums = chainIds(track, rack).front();
        editor().renameChain(track, drums, QStringLiteral("Drums"));
        const QString bass = editor().addRackChain(track, rack, -1, QStringLiteral("Bass"));
        editor().addDevice(track, QStringLiteral("compressor"), -1, bass);
        editor().addRackChain(track, rack, -1, QStringLiteral("Keys"));
        editor().mapMacro(track, rack, 0, added[0], QStringLiteral("gain"));
        editor().mapMacro(track, rack, 2, added[1], QStringLiteral("width"));
        editor().setMacro(track, rack, 0, 0.6);
        editor().setChainParam(track, bass, QStringLiteral("solo"), 1.0);
        editor().setChainParam(track, bass, QStringLiteral("volume_db"), -4.5);
        editor().setChainParam(track, bass, QStringLiteral("pan"), -0.4);
        devices().clickChain(rack, drums);
        showChainList(track, rack);
        QCOMPARE(chainViews().keys(), QStringList{drums});
        QVERIFY(frame(added[0]) && frame(added[1]) && frame(added[2]));
        ui_.screenshot(QStringLiteral("rack-chains-macros"));
        // Many chains: the list scrolls; the one shown stays in view.
        QString last;
        for (int i = 0; i < 6; ++i) last = editor().addRackChain(track, rack);
        devices().clickChain(rack, last);
        ui_.polish();
        QQuickItem* lastRow = row(rack, last);
        QQuickItem* list = test::itemNamed(frame(rack), QStringLiteral("chainList"));
        QTRY_VERIFY(lastRow->mapToItem(list, QPointF(0, lastRow->height())).y() <= list->height());
        ui_.screenshot(QStringLiteral("rack-many-chains"));
    }

    void addingAndTakingAwayMacros() {
        QStringList added;
        const QString track = shownTrack(added, 1);
        const QString rack = editor().groupDevices(track, {added[0]});
        ui_.polish();
        QQuickItem* add = test::itemNamed(frame(rack), QStringLiteral("addMacroButton"));
        QQuickItem* remove = test::itemNamed(frame(rack), QStringLiteral("removeMacroButton"));
        QVERIFY(add && remove);
        const qreal narrow = frame(rack)->width();
        for (int i = 0; i < 4; ++i) test::click(window(), test::centerOf(add));
        QCOMPARE(sub::app::macroCount(project().device(track, rack)), 8);
        QCOMPARE(undo().undoText(), QStringLiteral("Add Macro"));
        QVERIFY(macroCell(rack, 7) && !macroCell(rack, 8));
        // Two rows: the eighth under the fourth.
        QCOMPARE(macroCell(rack, 4)->mapToScene(QPointF()).x(), macroCell(rack, 0)->mapToScene(QPointF()).x());
        QVERIFY(macroCell(rack, 4)->mapToScene(QPointF()).y() > macroCell(rack, 3)->mapToScene(QPointF()).y());
        QVERIFY(frame(rack)->width() > narrow);
        ui_.screenshot(QStringLiteral("rack-eight-macros"));
        test::click(window(), test::centerOf(remove));
        QCOMPARE(sub::app::macroCount(project().device(track, rack)), 7);
        QVERIFY(!macroCell(rack, 7));
        editor().setMacroCount(track, rack, 1);
        ui_.polish();
        QVERIFY(!test::itemNamed(frame(rack), QStringLiteral("removeMacroButton"))->isEnabled());
        editor().setMacroCount(track, rack, sub::app::kMaxMacroCount);
        ui_.polish();
        QVERIFY(!test::itemNamed(frame(rack), QStringLiteral("addMacroButton"))->isEnabled());
        QVERIFY(macroCell(rack, 15));
        QCOMPARE(ui_.panel()->height(), ui_.panel()->implicitHeight());  // sixteen fit the view's height
        QVERIFY(macroCell(rack, 15)->mapToItem(frame(rack), QPointF(0, macroCell(rack, 15)->height())).y() <=
                frame(rack)->height());
        ui_.screenshot(QStringLiteral("rack-sixteen-macros"));
        // The menu's Remove Last Macro, and Add Macro.
        ui_.rightClick(test::centerOf(macroCell(rack, 0)));
        QVERIFY(ui_.choose(QStringLiteral("Remove Last Macro")));
        QCOMPARE(sub::app::macroCount(project().device(track, rack)), 15);
        ui_.rightClick(test::centerOf(macroCell(rack, 0)));
        QVERIFY(ui_.choose(QStringLiteral("Add Macro")));
        QCOMPARE(sub::app::macroCount(project().device(track, rack)), 16);
    }

    void renamingAMacroInPlace() {
        QStringList added;
        const QString track = shownTrack(added, 1);
        const QString rack = editor().groupDevices(track, {added[0]});
        editor().mapMacro(track, rack, 1, added[0], QStringLiteral("gain"));
        QQuickItem* cell = macroCell(rack, 1);
        test::doubleClick(window(), test::centerOf(test::part(cell, "nameLabel")));
        QQuickItem* field = test::part(cell, "renameField");
        QTRY_VERIFY(field->isVisible() && field->hasActiveFocus());
        QCOMPARE(field->property("selectedText").toString(), QStringLiteral("Macro 2"));
        for (const char key : {'D', 'r', 'i', 'v', 'e'}) QTest::keyClick(window(), key);
        QTest::keyClick(window(), Qt::Key_Return);
        QCOMPARE(sub::app::macroName(project().device(track, rack), 1), QStringLiteral("Drive"));
        QCOMPARE(undo().undoText(), QStringLiteral("Rename Macro"));
        QCOMPARE(test::part(macroCell(rack, 1), "nameLabel")->property("text").toString(), QStringLiteral("Drive"));
        // The parameter's menu names it so.
        QQuickItem* gain = test::itemNamed(frame(added[0]), QStringLiteral("param_gain"));
        QObject* paramMenu = gain->property("menu").value<QObject*>();
        QMetaObject::invokeMethod(paramMenu, "build");
        QVERIFY(test::menuTexts(paramMenu).contains(QStringLiteral("Unmap from Drive")));
        // From its menu; an empty name names it by its number again.
        ui_.rightClick(test::centerOf(macroCell(rack, 1)));
        QVERIFY(ui_.choose(QStringLiteral("Rename")));
        QTRY_VERIFY(test::part(macroCell(rack, 1), "renameField")->hasActiveFocus());
        QTest::keyClick(window(), Qt::Key_Backspace);
        QTest::keyClick(window(), Qt::Key_Return);
        QCOMPARE(sub::app::macroName(project().device(track, rack), 1), QStringLiteral("Macro 2"));
        // Its automation lane is called by its name.
        editor().renameMacro(track, rack, 0, QStringLiteral("Cutoff"));
        QCOMPARE(session().bridge()->paramSpec(track, sub::app::automation::deviceKey(rack, sub::app::macroParam(0)))
                     ->name,
                 QStringLiteral("Cutoff"));
    }

    void aMacrosAutomation() {
        QStringList added;
        const QString track = shownTrack(added, 1);
        const QString a = added[0];
        const QString rack = editor().groupDevices(track, {a});
        editor().mapMacro(track, rack, 0, a, QStringLiteral("gain"));
        const QString key = sub::app::automation::deviceKey(rack, sub::app::macroParam(0));
        QQuickItem* cell = macroCell(rack, 0);
        auto* knob = qobject_cast<sub::ui::KnobItem*>(test::part(cell, "knob"));
        QVERIFY(knob);
        // Pressing it shows its automation in the arrangement.
        test::click(window(), test::centerOf(knob));
        QCOMPARE(project().automationView(track).key, std::optional<QString>(key));
        // Automated: the dot, and it follows its envelope (as does what it moves).
        editor().setEnvelope(track, key, {{0.0, 0.25, 0.0}});
        QTRY_COMPARE(knob->automation(), QStringLiteral("on"));
        QCOMPARE(knob->value(), 0.25);
        QObject* gain = test::itemNamed(frame(a), QStringLiteral("param_gain"))->property("param").value<QObject*>();
        QCOMPARE(gain->property("automation").toString(), QStringLiteral("on"));
        QCOMPARE(gain->property("value").toDouble(), -60.0 + 0.25 * 84.0);
        // Turned by hand: overridden.
        QMetaObject::invokeMethod(cell->property("macro").value<QObject*>(), "set", Q_ARG(double, 1.0),
                                  Q_ARG(QString, QString()));
        QTRY_COMPARE(knob->automation(), QStringLiteral("off"));
        QCOMPARE(knob->value(), 1.0);
        ui_.rightClick(test::centerOf(cell));
        QVERIFY(ui_.menuOpened());
        QVERIFY(test::menuTexts(ui_.menu()).contains(QStringLiteral("Re-Enable Automation")));
        QVERIFY(ui_.choose(QStringLiteral("Re-Enable Automation")));
        QTRY_COMPARE(knob->automation(), QStringLiteral("on"));
        QCOMPARE(knob->value(), 0.25);
        ui_.rightClick(test::centerOf(cell));
        QVERIFY(ui_.choose(QStringLiteral("Delete Automation")));
        QVERIFY(project().envelope(track, key).empty());
        QTRY_COMPARE(knob->automation(), QString());
        QCOMPARE(gain->property("automation").toString(), QString());
    }

    void aMacrosMappingsAndTheirRanges() {
        QStringList added;
        const QString track = shownTrack(added, 2);
        const QString a = added[0], b = added[1];
        const QString rack = editor().groupDevices(track, {a, b});
        editor().mapMacro(track, rack, 0, a, QStringLiteral("gain"));
        editor().mapMacro(track, rack, 0, b, QStringLiteral("width"));
        editor().setMacro(track, rack, 0, 1.0);
        QQuickItem* cell = macroCell(rack, 0);
        ui_.rightClick(test::centerOf(cell));
        QVERIFY(ui_.choose(QStringLiteral("Edit Mappings…")));
        auto* popup = ui_.panel()->property("macroMappings").value<QObject*>();
        QTRY_VERIFY(popup->property("opened").toBool());
        QObject* rows = popup->property("rows").value<QObject*>();
        QCOMPARE(rows->property("count").toInt(), 2);
        QQuickItem* gainRow = nullptr;
        QMetaObject::invokeMethod(rows, "itemAt", Q_RETURN_ARG(QQuickItem*, gainRow), Q_ARG(int, 0));
        QVERIFY(gainRow);
        auto* low = qobject_cast<sub::ui::ValueBoxItem*>(test::part(gainRow, "low"));
        auto* high = qobject_cast<sub::ui::ValueBoxItem*>(test::part(gainRow, "high"));
        QVERIFY(low && high);
        QCOMPARE(low->value(), 0.0);
        QCOMPARE(high->value(), 100.0);
        ui_.screenshot(QStringLiteral("macro-mappings"), true);
        // Max to 50 %: the gain goes where the macro (at 100 %) puts it now, -18 dB; one step a drag.
        const int steps = undo().count();
        Q_EMIT high->moved(75.0, QStringLiteral("drag"));
        Q_EMIT high->moved(50.0, QStringLiteral("drag"));
        QCOMPARE(undo().count(), steps + 1);
        QCOMPARE(undo().undoText(), QStringLiteral("Change Macro Range"));
        QVERIFY(std::abs(project().device(track, a).params.value(QStringLiteral("gain")) - (-18.0)) < 1e-9);
        QCOMPARE(high->value(), 50.0);
        QCOMPARE(rows->property("count").toInt(), 2);
        QMetaObject::invokeMethod(rows, "itemAt", Q_RETURN_ARG(QQuickItem*, gainRow), Q_ARG(int, 0));
        QCOMPARE(qobject_cast<sub::ui::ValueBoxItem*>(test::part(gainRow, "high")), high);  // (the same row: not made again)
        // Invert: 50 to 0 %.
        test::click(window(), test::centerOf(test::part(gainRow, "invertButton")));
        const auto& mappings = project().device(track, rack).macros;
        QVERIFY(mappings[0].low == 0.5 && mappings[0].high == 0.0);
        QVERIFY(std::abs(project().device(track, a).params.value(QStringLiteral("gain")) - (-60.0)) < 1e-9);
        // Unmapped from the list: its row goes.
        QQuickItem* widthRow = nullptr;
        QMetaObject::invokeMethod(rows, "itemAt", Q_RETURN_ARG(QQuickItem*, widthRow), Q_ARG(int, 1));
        test::click(window(), test::centerOf(test::part(widthRow, "unmapButton")));
        QCOMPARE(project().device(track, rack).macros.size(), size_t(1));
        QTRY_COMPARE(rows->property("count").toInt(), 1);
        QMetaObject::invokeMethod(popup, "close");
        QTRY_VERIFY(!popup->property("visible").toBool());
    }
};

QTEST_MAIN(TestUiDevicePanelRacks)
#include "test_ui_device_panel_racks.moc"
