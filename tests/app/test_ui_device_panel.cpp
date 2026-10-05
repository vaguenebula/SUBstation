// The device view (ui/qml/devices/DevicePanel.qml): folding devices (saved, not
// undone; the fold button; double-clicks; several selected fold together; a
// folded rack hiding its chain), cut, copy, paste and duplicate (Ctrl+C/X/V/D
// while it has the focus; the focus taken by clicking beside the devices),
// switching a device off beside racks without making the frames again. And the
// panel as a whole (with built-in devices): building the chain and its hint,
// the device's menu and the one beside the devices, Shift+wheel and
// Ctrl+Alt-drag, a drag held near an edge scrolling the chain, dragging a
// device (its drag's payload), dropping devices moved and devices from the
// browser, scrolling to a device added; a device's content as far from its
// title bar as from its bottom. Driven in a window with a real session,
// with synthesized mouse, key and drag events; the project and the selection
// checked. With SUBSTATION_UI_SCREENSHOTS set, screenshots go there.

#include <QDrag>
#include <QGuiApplication>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QUndoStack>

#include <functional>
#include <memory>
#include <utility>

#include "DevicePanelTestSupport.h"
#include "TestSupport.h"
#include "audio/EngineBridge.h"
#include "controls/KnobItem.h"
#include "devices/DeviceChainArea.h"
#include "devices/DeviceInfo.h"
#include "editor/ProjectEditor.h"
#include "model/Device.h"
#include "model/Project.h"
#include "session/ArrangementActions.h"
#include "session/DeviceSelection.h"
#include "session/Selection.h"

namespace test = sub::app::test;
using sub::app::Selection;

class TestUiDevicePanel : public QObject {
    Q_OBJECT

    test::PanelWindow ui_;

    sub::app::Session& session() { return ui_.session(); }
    sub::app::Project& project() { return *session().project(); }
    sub::app::ProjectEditor& editor() { return *session().editor(); }
    sub::app::DeviceSelection& devices() { return *session().deviceSelection(); }
    QUndoStack& undo() { return *session().undoStack(); }
    QQuickWindow* window() { return ui_.window(); }
    sub::ui::DeviceChainArea* area() { return ui_.area(); }

    QStringList chain(const QString& trackId) {
        QStringList ids;
        for (const sub::app::Device& device : project().track(trackId).devices) ids << device.id;
        return ids;
    }

    // A new track, selected (its devices shown), with `count` utilities.
    QString shownTrack(QStringList* added = nullptr, int count = 3) {
        const QString track = editor().addAudioTrack();
        session().selection()->selectTrack(track);
        for (int i = 0; i < count; ++i) {
            const QString id = editor().addDevice(track, QStringLiteral("utility"));
            if (added) *added << id;
        }
        ui_.polish();
        return track;
    }

    QQuickItem* frame(const QString& deviceId) { return ui_.frame(deviceId); }
    QQuickItem* partOf(const QString& deviceId, const char* name) { return test::part(frame(deviceId), name); }
    QPoint centerOf(const QString& deviceId, const char* name) { return test::centerOf(partOf(deviceId, name)); }
    void clickTitle(const QString& deviceId, Qt::KeyboardModifiers modifiers = {}) {
        test::click(window(), centerOf(deviceId, "title"), modifiers);
    }
    void rightClick(QPoint at) { ui_.rightClick(at); }
    QPoint beside(int fromRight) { return ui_.beside(fromRight); }
    QObject* menu() { return ui_.menu(); }
    void closeMenu() { QVERIFY(ui_.closeMenu()); }
    void choose(const QString& text) { QVERIFY2(ui_.choose(text), qPrintable(text)); }
    // The device frames' order on screen.
    QStringList shownOrder() {
        ui_.polish();
        QStringList ids = devices().shownDevices();
        std::sort(ids.begin(), ids.end(), [this](const QString& a, const QString& b) {
            return frame(a)->mapToScene(QPointF()).x() < frame(b)->mapToScene(QPointF()).x();
        });
        return ids;
    }

private Q_SLOTS:
    void initTestCase() {
        test::prepareApplication();
        if (!test::haveDisplay()) QSKIP("needs a display: Qt Quick's software renderer draws none of this geometry");
        sub::ui::setUpApplication();
        QVERIFY(ui_.open());
    }

    void cleanupTestCase() { ui_.close(); }

    void init() {
        if (menu()->property("visible").toBool()) closeMenu();
        project().clear();
        undo().clear();
        ui_.messages.clear();
        area()->setContentX(0);
        test::moveTo(window(), QPoint(5, 5), {}, Qt::NoButton);
    }

    // --- Building it ------------------------------------------------------------------------

    void showsTheChainAndWhatCanBeDropped() {
        auto* hint = test::part(ui_.panel(), "hint");
        QCOMPARE(hint->property("text").toString(), QStringLiteral("No track selected"));
        QVERIFY(hint->isVisible());
        QStringList ids;
        const QString track = shownTrack(&ids, 0);
        QCOMPARE(hint->property("text").toString(), sub::app::kEffectsHint);
        ids << editor().addDevice(track, QStringLiteral("utility"));
        ids << editor().addDevice(track, QStringLiteral("compressor"));
        QTRY_VERIFY(!hint->isVisible());
        QCOMPARE(shownOrder(), ids);
        for (const QString& id : ids) {
            QQuickItem* f = frame(id);
            QVERIFY(f);
            QCOMPARE(f->height(), area()->height());  // as tall as the view: it never scrolls vertically
        }
        QCOMPARE(frame(ids[0])->width(), 216.0);  // DEVICE_WIDTH
        QCOMPARE(frame(ids[1])->mapToScene(QPointF()).x() - frame(ids[0])->mapToScene(QPointF(216, 0)).x(),
                 6.0);  // the chain's spacing
        QCOMPARE(partOf(ids[0], "title")->property("text").toString(), QStringLiteral("Utility"));
        // A MIDI track without an instrument asks for one, after its effects.
        const QString midi = editor().addMidiTrack(-1, QStringLiteral("Keys"));
        editor().removeDevices(midi, chain(midi));
        session().selection()->selectTrack(midi);
        QCOMPARE(hint->property("text").toString(), sub::app::kInstrumentHint);
        const QString effect = editor().addDevice(midi, QStringLiteral("utility"));
        ui_.polish();
        QVERIFY(hint->isVisible() && frame(effect));
        QVERIFY(hint->mapToScene(QPointF()).x() > frame(effect)->mapToScene(QPointF()).x() + 216);
        // The panel is as tall as the tallest device wants.
        QCOMPARE(int(ui_.panel()->implicitHeight()), 2 * 8 + ui_.panel()->property("deviceHeight").toInt());
    }

    // A device's content is as far from its title bar as from its bottom edge:
    // 6 px above and below the tallest device's page of knobs (two rows), an
    // editor's graph, a rack's chain list.
    void aDevicesContentIsAsFarFromItsTitleBarAsFromItsBottom() {
        const QString track = editor().addAudioTrack();
        session().selection()->selectTrack(track);
        const QString utility = editor().addDevice(track, QStringLiteral("utility"));
        const QString compressor = editor().addDevice(track, QStringLiteral("compressor"));
        const QString grouped = editor().addDevice(track, QStringLiteral("utility"));
        const QString rack = editor().groupDevices(track, {grouped});
        ui_.polish();
        // Above and below `rect` (in the frame's coordinates), inside its frame and under its title bar.
        auto margins = [&](const QString& id, const QRectF& rect) {
            QQuickItem* header = partOf(id, "header");
            return std::make_pair(rect.top() - (header->y() + header->height()), frame(id)->height() - 1 - rect.bottom());
        };
        auto inFrame = [&](const QString& id, QQuickItem* item) {
            return item->mapRectToItem(frame(id), item->boundingRect());
        };
        // The utility's knobs, two rows of them.
        QRectF knobs;
        std::function<void(QQuickItem*)> addKnobs = [&](QQuickItem* item) {
            for (QQuickItem* child : item->childItems()) {
                if (child->isVisible() && child->objectName().startsWith(QStringLiteral("param_")))
                    knobs |= inFrame(utility, child);
                addKnobs(child);
            }
        };
        addKnobs(frame(utility));
        QVERIFY(knobs.height() > 2 * 34);
        QCOMPARE(margins(utility, knobs), std::make_pair(6.0, 6.0));
        auto* graph = test::itemNamed(frame(compressor), QStringLiteral("reductionGraph"));
        QVERIFY(graph);
        QCOMPARE(margins(compressor, inFrame(compressor, graph)), std::make_pair(6.0, 6.0));
        auto* list = test::itemNamed(frame(rack), QStringLiteral("chainList"));
        QVERIFY(list);
        QCOMPARE(margins(rack, inFrame(rack, list)), std::make_pair(6.0, 6.0));
        ui_.screenshot(QStringLiteral("margins"));
    }

    void devicesWithEditorsOfTheirOwn() {
        const QString track = editor().addMidiTrack(-1, QStringLiteral("Keys"));
        session().selection()->selectTrack(track);
        const QString synth = chain(track).front();
        const QString sampler = editor().addDevice(track, QStringLiteral("sampler"));  // (it replaces the synth)
        const QString compressor = editor().addDevice(track, QStringLiteral("compressor"));
        Q_UNUSED(synth);
        area()->setContentX(0);
        QQuickItem* f = frame(sampler);
        QVERIFY(f);
        auto* body = f->property("body").value<QQuickItem*>();
        QCOMPARE(QString::fromLatin1(body->metaObject()->className()).section(u'_', 0, 0), QStringLiteral("SamplerEditor"));
        QCOMPARE(f->width(), body->implicitWidth() + 2);  // the editor's own width, in the frame's border
        QCOMPARE(body->height(), f->height() - 2 - partOf(sampler, "header")->height());  // the body's whole height
        // Its pages: the title bar's arrows.
        QCOMPARE(f->property("pages").toInt(), 2);
        QVERIFY(partOf(sampler, "nextButton")->isVisible());
        QCOMPARE(partOf(sampler, "pageLabel")->property("text").toString(), QStringLiteral("1/2"));
        test::click(window(), centerOf(sampler, "nextButton"));
        QCOMPARE(body->property("page").toInt(), 1);
        QCOMPARE(partOf(sampler, "pageLabel")->property("text").toString(), QStringLiteral("2/2"));
        session().selection()->selectTrack(QString());
        session().selection()->selectTrack(track);  // made again: on the same page
        QCOMPARE(frame(sampler)->property("body").value<QQuickItem*>()->property("page").toInt(), 1);
        // Its menu starts with its own entries.
        ui_.rightClick(centerOf(sampler, "title"));
        QVERIFY(ui_.menuOpened());
        QCOMPARE(test::menuTexts(menu()).mid(0, 3), (QStringList{"Load Sample…", "Clear Sample", "Fold"}));
        QVERIFY(!test::menuEnabled(menu(), QStringLiteral("Clear Sample")));
        closeMenu();
        // One page: no arrows.
        QVERIFY(!partOf(compressor, "nextButton")->isVisible());
        QVERIFY(partOf(compressor, "sidechainButton")->isVisible());
        ui_.screenshot(QStringLiteral("editors"));
    }

    // --- Folding ---------------------------------------------------------------------------

    void foldingDevicesInTheDeviceView() {
        QStringList ids;
        const QString track = shownTrack(&ids);
        const QString a = ids[0], b = ids[1], c = ids[2];
        const int steps = undo().count();
        test::click(window(), centerOf(a, "foldButton"));
        QVERIFY(project().isDeviceFolded(a));
        QQuickItem* folded = frame(a);
        QCOMPARE(folded->width(), 26.0);  // FOLDED_WIDTH
        QVERIFY(test::part(folded, "foldedBar")->isVisible());
        QVERIFY(!test::part(folded, "header")->isVisible());
        QVERIFY(!test::itemNamed(folded, QStringLiteral("body")));
        QCOMPARE(undo().count(), steps);  // view state: not undone
        test::doubleClick(window(), ui_.at(folded, 13, 60));  // the strip: unfolds it
        QVERIFY(!project().isDeviceFolded(a));
        QCOMPARE(frame(a)->width(), 216.0);
        test::doubleClick(window(), centerOf(a, "title"), Qt::ControlModifier);  // Ctrl+double-click folds it...
        QVERIFY(project().isDeviceFolded(a));
        test::doubleClick(window(), ui_.at(frame(a), 13, 60), Qt::ControlModifier);  // ...and unfolds it
        QVERIFY(!project().isDeviceFolded(a));
        // A plain double-click on a built-in device does nothing of the kind.
        test::doubleClick(window(), centerOf(c, "title"));
        QVERIFY(!project().isDeviceFolded(c));
        // With several selected, folding one of them folds them all.
        clickTitle(a);
        clickTitle(b, Qt::ControlModifier);
        QCOMPARE(devices().selected(), (QStringList{a, b}));
        test::click(window(), centerOf(b, "foldButton"));
        QVERIFY(project().isDeviceFolded(a) && project().isDeviceFolded(b) && !project().isDeviceFolded(c));
        QCOMPARE(devices().selected(), (QStringList{a, b}));  // still selected
        ui_.screenshot(QStringLiteral("folded"));
        // A folded rack hides its chain (and what is in it).
        const QString rack = editor().groupDevices(track, {c});
        ui_.polish();
        QVERIFY(frame(c));  // in the rack's chain, shown beside it
        QCOMPARE(frame(c)->property("chainId").toString(), devices().containerOf(c));
        test::click(window(), centerOf(rack, "foldButton"));
        QVERIFY(project().isDeviceFolded(rack));
        QVERIFY(frame(rack) && !frame(c));
        test::click(window(), test::centerOf(test::itemNamed(frame(rack), QStringLiteral("foldedFoldButton"))));
        QVERIFY(!project().isDeviceFolded(rack));
        QVERIFY(frame(c));
    }

    // --- The clipboard ----------------------------------------------------------------------

    void cutCopyPasteAndDuplicateDevices() {
        QStringList ids;
        const QString track = shownTrack(&ids);
        const QString a = ids[0], b = ids[1], c = ids[2];
        editor().setDeviceParam(track, a, QStringLiteral("gain"), 0.3);
        clickTitle(a);
        QCOMPARE(session().selection()->focus(), Selection::Focus::Devices);
        session().copy();  // Ctrl+C
        clickTitle(b);
        session().paste();  // Ctrl+V: after the selected device, and selected
        QStringList now = chain(track);
        QCOMPARE(now.size(), 4);
        QCOMPARE(now.mid(0, 2), (QStringList{a, b}));
        QCOMPARE(now[3], c);
        const QString pasted = now[2];
        QVERIFY(!ids.contains(pasted));
        QCOMPARE(project().device(track, pasted).params.value(QStringLiteral("gain")), 0.3);
        QCOMPARE(devices().selected(), QStringList{pasted});
        ui_.polish();
        QVERIFY(frame(pasted)->property("selected").toBool());
        session().cut();  // Ctrl+X: the pasted one goes (to the clipboard)
        QCOMPARE(chain(track), ids);
        undo().undo();
        QCOMPARE(chain(track).size(), 4);
        undo().undo();
        QCOMPARE(chain(track), ids);
        clickTitle(b);
        session().duplicate();  // Ctrl+D: a copy right after it; the clipboard keeps what was cut
        now = chain(track);
        QCOMPARE(now.size(), 4);
        QCOMPARE(now[1], b);
        QCOMPARE(devices().selected(), QStringList{now[2]});
        QCOMPARE(devices().clipboard().size(), size_t(1));
        QCOMPARE(devices().clipboard().front().params.value(QStringLiteral("gain")), 0.3);

        // Onto another track: click beside its devices (the device view takes the focus), then paste.
        const QString other = editor().addAudioTrack();
        session().selection()->selectTrack(other, true);
        QCOMPARE(session().selection()->focus(), Selection::Focus::Track);
        test::click(window(), beside(5));
        QCOMPARE(session().selection()->focus(), Selection::Focus::Devices);
        session().paste();
        QCOMPARE(chain(other).size(), 1);
        const QString copy = chain(other).front();
        QCOMPARE(project().device(other, copy).params.value(QStringLiteral("gain")), 0.3);
        QCOMPARE(devices().selected(), QStringList{copy});
        QVERIFY(!session().arrangement()->property("hasClipboard").toBool());  // (the clips' clipboard is another)
    }

    void switchingADeviceOffBesideRacksDoesntRebuildTheView() {
        QStringList ids;
        const QString track = shownTrack(&ids);
        const QString a = ids[0], b = ids[1];
        const QString rack = editor().groupDevices(track, {b});
        editor().addDevice(track, QStringLiteral("utility"), -1, editor().addRackChain(track, rack));
        for (const bool fold : {false, true}) {  // a chain not shown; then the rack folded
            editor().setDevicesFolded(track, {rack}, fold);
            ui_.polish();
            QList<QPointer<QQuickItem>> frames;
            for (const QString& id : devices().shownDevices()) frames << QPointer<QQuickItem>(frame(id));
            editor().setDeviceEnabled(track, a, false);
            ui_.polish();
            int i = 0;
            for (const QString& id : devices().shownDevices()) {
                QVERIFY(frames[i]);  // the same frames, refreshed
                QCOMPARE(frame(id), frames[i++].data());
            }
            QVERIFY(!test::part(frame(a), "enableButton")->property("checked").toBool());
            editor().setDeviceEnabled(track, a, true);
            QVERIFY(test::part(frame(a), "enableButton")->property("checked").toBool());
        }
        // Its on/off switch, clicked.
        test::click(window(), centerOf(a, "enableButton"));
        QVERIFY(!project().device(track, a).enabled);
        test::click(window(), centerOf(a, "enableButton"));
        QVERIFY(project().device(track, a).enabled);
    }

    // --- Selecting with the mouse ----------------------------------------------------------

    void selectingDevices() {
        QStringList ids;
        shownTrack(&ids, 4);
        clickTitle(ids[1]);
        QCOMPARE(devices().selected(), QStringList{ids[1]});
        QVERIFY(frame(ids[1])->property("selected").toBool());
        QCOMPARE(session().selection()->focus(), Selection::Focus::Devices);
        clickTitle(ids[3], Qt::ShiftModifier);  // a range
        QCOMPARE(devices().selected(), ids.mid(1));
        clickTitle(ids[2], Qt::ControlModifier);  // one less
        QCOMPARE(devices().selected(), (QStringList{ids[1], ids[3]}));
        // A press on one selected keeps the others (to drag them all); its release selects just it.
        test::press(window(), centerOf(ids[1], "title"));
        QCOMPARE(devices().selected(), (QStringList{ids[1], ids[3]}));
        test::release(window(), centerOf(ids[1], "title"));
        QCOMPARE(devices().selected(), QStringList{ids[1]});
        // The body selects as the title bar does; beside the devices, none.
        test::click(window(), ui_.at(frame(ids[0]), 108, frame(ids[0])->height() - 10));
        QCOMPARE(devices().selected(), QStringList{ids[0]});
        test::click(window(), beside(5));
        QVERIFY(devices().selected().isEmpty());
        QCOMPARE(session().selection()->focus(), Selection::Focus::Devices);
        // Selecting something else deselects them.
        clickTitle(ids[0]);
        session().selection()->selectTrack(devices().trackId(), true);
        QVERIFY(devices().selected().isEmpty());
        QVERIFY(!frame(ids[0])->property("selected").toBool());
    }

    // --- Menus ------------------------------------------------------------------------------

    void theDevicesMenu() {
        QStringList ids;
        const QString track = shownTrack(&ids);
        rightClick(centerOf(ids[1], "title"));
        QCOMPARE(devices().selected(), QStringList{ids[1]});  // selected before its menu shows
        QVERIFY(ui_.menuOpened());
        QCOMPARE(test::menuTexts(menu()),
                 (QStringList{"Fold", "", "Cut", "Copy", "Paste", "Duplicate", "", "Move Left", "Move Right", "",
                              "Save Preset…", "Save as Default Preset", "Clear Default Preset", "", "Group", "",
                              "Delete"}));
        QCOMPARE(test::menuEnabled(menu(), QStringLiteral("Paste")), devices().hasClipboard());  // (anything copied?)
        QVERIFY(!test::menuEnabled(menu(), QStringLiteral("Clear Default Preset")));
        QCOMPARE(test::menuItem(menu(), QStringLiteral("Cut"))->property("hintText").toString(),
                 QStringLiteral("Ctrl+X"));
        QCOMPARE(test::menuItem(menu(), QStringLiteral("Group"))->property("hintText").toString(),
                 QStringLiteral("Ctrl+G"));
        ui_.screenshot(QStringLiteral("menu"), true);
        choose(QStringLiteral("Move Left"));
        QCOMPARE(chain(track), (QStringList{ids[1], ids[0], ids[2]}));
        rightClick(centerOf(ids[1], "title"));
        QVERIFY(ui_.menuOpened());
        QVERIFY(!test::menuEnabled(menu(), QStringLiteral("Move Left")));  // first
        choose(QStringLiteral("Copy"));
        rightClick(centerOf(ids[2], "title"));
        QVERIFY(ui_.menuOpened());
        QVERIFY(!test::menuEnabled(menu(), QStringLiteral("Move Right")));  // last
        choose(QStringLiteral("Paste"));  // after it
        QCOMPARE(chain(track).size(), 4);
        QCOMPARE(chain(track).mid(0, 3), (QStringList{ids[1], ids[0], ids[2]}));
        // Delete: the selected devices (the pasted one).
        const QString pasted = chain(track)[3];
        QCOMPARE(devices().selected(), QStringList{pasted});
        rightClick(ui_.at(frame(pasted), 100, 8));
        QVERIFY(ui_.menuOpened());
        choose(QStringLiteral("Delete"));
        QCOMPARE(chain(track), (QStringList{ids[1], ids[0], ids[2]}));
        // Group: it alone, unless it is one of the selected; then the rack has Ungroup.
        rightClick(centerOf(ids[0], "title"));
        QVERIFY(ui_.menuOpened());
        choose(QStringLiteral("Group"));
        const QString rack = chain(track)[1];
        QVERIFY(project().device(track, rack).isRack());
        rightClick(centerOf(rack, "title"));
        QVERIFY(ui_.menuOpened());
        QCOMPARE(test::menuTexts(menu()).mid(0, 3), (QStringList{"Add Chain", "", "Fold"}));
        QVERIFY(!test::menuTexts(menu()).contains(QStringLiteral("Save as Default Preset")));  // racks have none
        QVERIFY(test::menuTexts(menu()).contains(QStringLiteral("Ungroup")));
        choose(QStringLiteral("Ungroup"));
        QCOMPARE(chain(track), (QStringList{ids[1], ids[0], ids[2]}));
        // Fold, from the menu.
        rightClick(centerOf(ids[2], "title"));
        QVERIFY(ui_.menuOpened());
        choose(QStringLiteral("Fold"));
        QVERIFY(project().isDeviceFolded(ids[2]));
        rightClick(ui_.at(frame(ids[2]), 13, 60));
        QVERIFY(ui_.menuOpened());
        choose(QStringLiteral("Unfold"));
        QVERIFY(!project().isDeviceFolded(ids[2]));
    }

    void theMenuBesideTheDevices() {
        QStringList ids;
        const QString track = shownTrack(&ids, 2);
        const QPoint besideThem = beside(20);
        session().selection()->selectTrack(track, true);
        rightClick(besideThem);
        QCOMPARE(session().selection()->focus(), Selection::Focus::Devices);
        QVERIFY(ui_.menuOpened());
        QCOMPARE(test::menuTexts(menu()), (QStringList{"Paste", "", "Load Preset…"}));
        QCOMPARE(test::menuEnabled(menu(), QStringLiteral("Paste")), devices().hasClipboard());
        closeMenu();
        clickTitle(ids[0]);
        session().copy();
        rightClick(besideThem);
        QVERIFY(ui_.menuOpened());
        choose(QStringLiteral("Paste"));  // there: last
        QCOMPARE(chain(track).size(), 3);
        QCOMPARE(chain(track).mid(0, 2), ids);
        // Between the devices: there.
        rightClick(ui_.inArea(216 + 3, 40));
        QVERIFY(ui_.menuOpened());
        choose(QStringLiteral("Paste"));
        QCOMPARE(chain(track).size(), 4);
        QCOMPARE(chain(track)[0], ids[0]);
        QCOMPARE(chain(track)[2], ids[1]);
    }

    // --- Scrolling ----------------------------------------------------------------------------

    void shiftWheelScrollsTheChainAndTheWheelNeverTurnsAKnob() {
        QStringList ids;
        shownTrack(&ids, 12);
        QTRY_VERIFY(area()->maxContentX() > 0);
        QTest::qWait(1);  // (scrolled to the last device added)
        area()->setContentX(0);
        auto* knob = qobject_cast<sub::ui::KnobItem*>(test::part(test::itemNamed(frame(ids[0]), QStringLiteral("param_gain")), "knob"));
        QVERIFY(knob);
        const double value = knob->value();
        const int steps = undo().count();
        test::wheel(window(), test::centerOf(knob), 3);
        QCOMPARE(knob->value(), value);
        QCOMPARE(area()->contentX(), 0.0);
        QCOMPARE(undo().count(), steps);
        test::wheel(window(), test::centerOf(knob), -2, Qt::ShiftModifier);  // Shift+wheel, even over a knob, scrolls
        QCOMPARE(area()->contentX(), 160.0);
        test::wheel(window(), beside(30), 1, Qt::ShiftModifier);
        QCOMPARE(area()->contentX(), 80.0);
        QCOMPARE(knob->value(), value);
        QCOMPARE(undo().count(), steps);
    }

    void ctrlAltDragScrollsTheChain() {
        QStringList ids;
        shownTrack(&ids, 12);
        QTRY_VERIFY(area()->maxContentX() > 0);
        QTest::qWait(1);
        area()->setContentX(0);
        QQuickItem* knob = test::part(test::itemNamed(frame(ids[0]), QStringLiteral("param_gain")), "knob");  // the press can land on a knob
        const QPoint start = test::centerOf(knob);
        const Qt::KeyboardModifiers pan = Qt::ControlModifier | Qt::AltModifier;
        test::press(window(), start, pan);
        QVERIFY(area()->panning());
        test::moveTo(window(), start - QPoint(150, 0), pan);  // dragged left: the chain scrolls right
        QCOMPARE(area()->contentX(), std::min(150.0, area()->maxContentX()));
        test::moveTo(window(), start - QPoint(50, 0), pan);
        QCOMPARE(area()->contentX(), 50.0);
        test::release(window(), start - QPoint(50, 0), pan);
        QVERIFY(!area()->panning());
        QVERIFY(devices().selected().isEmpty());
        QVERIFY(QGuiApplication::overrideCursor() == nullptr);
        test::moveTo(window(), start - QPoint(300, 0), {}, Qt::NoButton);  // the pan is over
        QCOMPARE(area()->contentX(), 50.0);
    }

    void addingADeviceScrollsToIt() {
        QStringList ids;
        const QString track = shownTrack(&ids, 12);
        QTRY_VERIFY(area()->maxContentX() > 0);
        auto inView = [this](const QString& id) {
            QQuickItem* f = frame(id);
            const qreal left = f->mapToItem(area(), QPointF()).x();
            return left >= 0 && left + f->width() <= area()->width();
        };
        QTRY_VERIFY(inView(ids.last()));  // scrolled to the last device added
        area()->setContentX(0);
        session().addDeviceToSelectedTrack(QStringLiteral("compressor"));  // as from the browser
        const QString added = chain(track).last();
        QTRY_VERIFY(area()->contentX() > 0);
        QVERIFY(inView(added));
        // Dropped on the chain, where the user is looking, it doesn't scroll.
        area()->setContentX(0);
        auto mime = test::deviceKindsMime({QStringLiteral("utility")});
        test::dragAndDrop(window(), ui_.inArea(100, 40), mime.get());
        QCOMPARE(chain(track).size(), 14);
        QTest::qWait(50);
        QCOMPARE(area()->contentX(), 0.0);
    }

    // --- Dragging and dropping -------------------------------------------------------------

    void draggingADeviceStartsAMoveOfTheSelected() {
        QStringList ids;
        const QString track = editor().addMidiTrack(-1, QStringLiteral("Keys"));  // with its synth
        session().selection()->selectTrack(track);
        for (int i = 0; i < 3; ++i) ids << editor().addDevice(track, QStringLiteral("utility"));
        const QString synth = chain(track).front();
        ui_.polish();
        QSignalSpy starting(area(), &sub::ui::DeviceChainArea::dragStarting);
        // (The drag runs once the press's handlers have returned, while the button is held; Escape ends it.)
        bool dragRan = false;
        QObject::connect(area(), &sub::ui::DeviceChainArea::dragStarting, this, [this, &dragRan] {
            QTimer::singleShot(150, this, [this, &dragRan] {
                dragRan = area()->findChild<QDrag*>() != nullptr;
                QTest::keyClick(window(), Qt::Key_Escape);
            });
        });
        clickTitle(ids[0]);
        clickTitle(ids[1], Qt::ShiftModifier);
        const QPoint from = centerOf(ids[0], "title");
        test::press(window(), from);
        test::moveTo(window(), from + QPoint(3, 0));  // not yet: less than the start distance
        QCOMPARE(starting.count(), 0);
        test::moveTo(window(), from + QPoint(30, 0));
        QCOMPARE(starting.count(), 1);
        QCOMPARE(starting[0][0].toString(), track);
        QCOMPARE(starting[0][1].toStringList(), (QStringList{ids[0], ids[1]}));  // the selected ones
        QTest::qWait(400);  // (the drag ran, and was called off)
#ifndef Q_OS_WIN  // (there Windows' own drag loop runs it, and its QDrag isn't the area's child)
        QVERIFY(dragRan);
#endif
        QVERIFY(!area()->findChild<QDrag*>());
        test::release(window(), from + QPoint(30, 0));
        QCOMPARE(devices().selected(), (QStringList{ids[0], ids[1]}));
        // Let go before the drag could start: none starts.
        dragRan = false;
        test::press(window(), from);
        test::moveTo(window(), from + QPoint(30, 0));
        test::release(window(), from + QPoint(30, 0));
        QCOMPARE(starting.count(), 2);
        QTest::qWait(300);
        QVERIFY(!dragRan);
        // An instrument stays first: it isn't dragged.
        const QPoint synthTitle = centerOf(synth, "title");
        test::press(window(), synthTitle);
        test::moveTo(window(), synthTitle + QPoint(30, 0));
        test::release(window(), synthTitle + QPoint(30, 0));
        QCOMPARE(starting.count(), 2);
        QObject::disconnect(area(), &sub::ui::DeviceChainArea::dragStarting, this, nullptr);
    }

    void droppingDevicesMovedAlongTheChain() {
        QStringList ids;
        const QString track = shownTrack(&ids, 4);
        // Before a device: its left half; the line shows where.
        auto moving = test::movedMime(track, {ids[2], ids[3]});
        const QPoint before = ui_.at(frame(ids[0]), 2, frame(ids[0])->height() / 2);
        QVERIFY(test::dragEnter(window(), before, moving.get()));
        test::dragMove(window(), before, moving.get());
        auto* marker = test::part(ui_.panel(), "dropMarker");
        QVERIFY(marker->isVisible());
        QCOMPARE(marker->width(), 2.0);
        QCOMPARE(marker->height(), frame(ids[0])->height());
        QCOMPARE(int(marker->mapToItem(frame(ids[0]), QPointF()).x()), -4);  // in the gap before it
        const QPoint beforeSecond = ui_.at(frame(ids[1]), 30, 60);  // (left of its middle: before it)
        test::dragMove(window(), beforeSecond, moving.get());
        QCOMPARE(int(marker->mapToItem(frame(ids[1]), QPointF()).x()), -4);
        ui_.screenshot(QStringLiteral("drop-marker"));
        test::dragMove(window(), before, moving.get());
        test::dropAt(window(), before, moving.get());
        QVERIFY(!marker->isVisible());
        QCOMPARE(chain(track), (QStringList{ids[2], ids[3], ids[0], ids[1]}));
        QCOMPARE(undo().undoText(), QStringLiteral("Move Devices"));
        // Another track's devices don't drop here (they go from the arrangement).
        auto foreign = test::movedMime(QStringLiteral("elsewhere"), {ids[0]});
        QVERIFY(!test::dragEnter(window(), before, foreign.get()));
        test::dragLeave(window());
        // From the browser: where dropped, each after the last.
        auto kinds = test::deviceKindsMime({QStringLiteral("compressor"), QStringLiteral("delay")});
        const QPoint between = ui_.at(frame(ids[0]), -3, 40);
        test::dragAndDrop(window(), between, kinds.get());
        const QStringList now = chain(track);
        QCOMPARE(now.size(), 6);
        QCOMPARE(project().device(track, now[2]).kind, QStringLiteral("compressor"));
        QCOMPARE(project().device(track, now[3]).kind, QStringLiteral("delay"));
        QCOMPARE(now[4], ids[0]);
        // An instrument dropped on an audio track is refused (said so).
        auto synth = test::deviceKindsMime({QStringLiteral("synth")});
        test::dragAndDrop(window(), between, synth.get());
        QCOMPARE(chain(track).size(), 6);
        QCOMPARE(ui_.lastMessage(), sub::app::kInstrumentRefused);
    }

    void aDragHeldNearAnEdgeScrollsTheChain() {
        QStringList ids;
        const QString track = shownTrack(&ids, 12);
        QTRY_VERIFY(area()->maxContentX() > 0);
        QTest::qWait(1);
        area()->setContentX(0);
        auto mime = test::movedMime(track, {ids[0]});
        const int middle = int(area()->height() / 2);
        QVERIFY(test::dragEnter(window(), ui_.inArea(area()->width() / 2, middle), mime.get()));
        test::dragMove(window(), ui_.inArea(area()->width() - 5, middle), mime.get());  // held at the right edge
        QTRY_COMPARE(area()->contentX(), area()->maxContentX());
        test::dragMove(window(), ui_.inArea(area()->width() / 2, middle), mime.get());  // away from the edges, it stops
        const qreal stopped = area()->contentX();
        QTest::qWait(50);
        QCOMPARE(area()->contentX(), stopped);
        test::dragMove(window(), ui_.inArea(5, middle), mime.get());
        QTRY_VERIFY(area()->contentX() < stopped);
        test::dragLeave(window());
        const qreal left = area()->contentX();
        QTest::qWait(50);
        QCOMPARE(area()->contentX(), left);
        QVERIFY(!test::part(ui_.panel(), "dropMarker")->isVisible());
    }
};

QTEST_MAIN(TestUiDevicePanel)
#include "test_ui_device_panel.moc"
