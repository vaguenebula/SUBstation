// Device presets in the device view (DevicePanel.qml's save button, its
// dialogs, preset drops; Session.deviceSelection does the work), as
// test_ui_presets.py had them: every device's save button saves it to the
// library under a name asked for (its name to start with), asking before
// replacing a preset of that name; a rack takes the name of the preset it is
// saved as; dropped on the device view a preset goes in as a new device, or
// loads into the device it is dropped onto if it is of its kind (outlined
// while the drag is over it; one undo step); Save as Default Preset and Clear
// Default Preset from a device's menu; Load Preset… beside the devices.
// Driven in a window with a real session, with synthesized mouse, key and drag
// events. With SUBSTATION_UI_SCREENSHOTS set, screenshots go there.

#include <QDir>
#include <QFileInfo>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>
#include <QUrl>

#include "DevicePanelTestSupport.h"
#include "TestSupport.h"
#include "devices/DeviceChainArea.h"
#include "editor/ProjectEditor.h"
#include "io/Presets.h"
#include "io/Serialization.h"
#include "model/Device.h"
#include "model/Project.h"
#include "session/DeviceSelection.h"
#include "session/Selection.h"

namespace test = sub::app::test;

class TestUiDevicePanelPresets : public QObject {
    Q_OBJECT

    test::PanelWindow ui_;

    sub::app::Session& session() { return ui_.session(); }
    sub::app::Project& project() { return *session().project(); }
    sub::app::ProjectEditor& editor() { return *session().editor(); }
    sub::app::DeviceSelection& devices() { return *session().deviceSelection(); }
    QUndoStack& undo() { return *session().undoStack(); }
    QQuickWindow* window() { return ui_.window(); }
    sub::ui::DeviceChainArea* area() { return ui_.area(); }

    QStringList kinds(const QString& trackId) {
        QStringList out;
        for (const sub::app::Device& device : project().track(trackId).devices) out << device.kind;
        return out;
    }
    QStringList chain(const QString& trackId) {
        QStringList out;
        for (const sub::app::Device& device : project().track(trackId).devices) out << device.id;
        return out;
    }
    // A preset's file in the library.
    static QString presetPath(const QString& group, const QString& name) {
        return sub::app::libraryDir() + u'/' + group + u'/' + name + sub::app::kPresetExtension;
    }

    // A new track, selected, with these devices.
    QString shownTrack(QStringList& added, const QStringList& kindsAdded = {QStringLiteral("utility")}) {
        const QString track = editor().addAudioTrack();
        session().selection()->selectTrack(track);
        for (const QString& kind : kindsAdded) added << editor().addDevice(track, kind);
        ui_.polish();
        return track;
    }

    QQuickItem* frame(const QString& deviceId) { return ui_.frame(deviceId); }
    QObject* nameDialog() { return ui_.panel()->property("presetNameDialog").value<QObject*>(); }
    QObject* replaceDialog() { return ui_.panel()->property("presetReplaceDialog").value<QObject*>(); }
    static bool opened(QObject* dialog) {
        return QTest::qWaitFor([dialog] { return dialog->property("opened").toBool(); }, 3000);
    }
    static bool closed(QObject* dialog) {
        return QTest::qWaitFor([dialog] { return !dialog->property("visible").toBool(); }, 3000);
    }

    // The device's save button, the name asked for answered with `name` (Enter);
    // the name the dialog offered.
    QString save(const QString& deviceId, const QString& name) {
        area()->scrollTo(deviceId);
        test::click(window(), test::centerOf(test::part(frame(deviceId), "saveButton")));
        if (!opened(nameDialog())) return QStringLiteral("<no dialog>");
        auto* field = nameDialog()->findChild<QQuickItem*>(QStringLiteral("presetNameField"));
        if (!field) return QStringLiteral("<no field>");
        if (!QTest::qWaitFor([field] { return field->hasActiveFocus(); }, 2000)) return QStringLiteral("<no focus>");
        const QString offered = field->property("text").toString();
        field->setProperty("text", name);
        QTest::keyClick(window(), Qt::Key_Return);
        closed(nameDialog());
        return offered;
    }
    // The question about replacing a preset answered with a click.
    void answer(const QString& button) {
        QVERIFY(opened(replaceDialog()));
        QTest::qWait(30);
        QQuickItem* item = test::dialogButton(replaceDialog(), button);
        QVERIFY2(item, qPrintable(button));
        test::click(window(), test::centerOf(item));
        QVERIFY(closed(replaceDialog()));
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
        if (ui_.menu()->property("visible").toBool()) QVERIFY(ui_.closeMenu());
        project().clear();
        undo().clear();
        ui_.messages.clear();
        area()->setContentX(0);
        test::moveTo(window(), QPoint(5, 5), {}, Qt::NoButton);
    }

    void theSaveButtonSavesToTheLibrary() {
        QStringList added;
        const QString track = shownTrack(added);
        const QString utility = added[0];
        editor().setDeviceParam(track, utility, QStringLiteral("gain"), -7.0);
        QSignalSpy saved(&devices(), &sub::app::DeviceSelection::presetSaved);
        QCOMPARE(save(utility, QStringLiteral("Quieter")), QStringLiteral("Utility"));  // its name to start with
        const QString path = presetPath(QStringLiteral("Utility"), QStringLiteral("Quieter"));
        QVERIFY(QFileInfo::exists(path));
        QCOMPARE(sub::app::loadPreset(path).params.value(QStringLiteral("gain")), -7.0);
        QVERIFY(ui_.lastMessage().startsWith(QStringLiteral("Saved the preset Quieter")));
        QCOMPARE(saved.count(), 1);  // (the browser lists it)
        QCOMPARE(QFileInfo(saved[0][0].toString()).absoluteFilePath(), QFileInfo(path).absoluteFilePath());

        // Saving again under that name asks first; "No" keeps the preset as it was.
        editor().setDeviceParam(track, utility, QStringLiteral("gain"), -1.0);
        save(utility, QStringLiteral("Quieter"));
        QVERIFY(opened(replaceDialog()));
        QCOMPARE(replaceDialog()->property("question").toString(),
                 QStringLiteral("There is a Utility preset called “Quieter” already. Replace it?"));
        ui_.screenshot(QStringLiteral("replace-preset"), true);
        answer(QStringLiteral("No"));
        QCOMPARE(sub::app::loadPreset(path).params.value(QStringLiteral("gain")), -7.0);
        save(utility, QStringLiteral("Quieter"));
        answer(QStringLiteral("Yes"));
        QCOMPARE(sub::app::loadPreset(path).params.value(QStringLiteral("gain")), -1.0);
        // A name that can't be a file's: said so, nothing saved.
        const int count = saved.count();
        save(utility, QStringLiteral("  "));
        QVERIFY(!replaceDialog()->property("visible").toBool());
        QCOMPARE(saved.count(), count);
        QCOMPARE(window()->property("said").toStringList().size(), 1);  // (the panel's own message)
        // Cancelled: nothing.
        test::click(window(), test::centerOf(test::part(frame(utility), "saveButton")));
        QVERIFY(opened(nameDialog()));
        QTest::keyClick(window(), Qt::Key_Escape);
        QVERIFY(closed(nameDialog()));
        QCOMPARE(saved.count(), count);
    }

    void everyKindOfDeviceHasAWorkingSaveButton() {
        QStringList added;
        shownTrack(added, {QStringLiteral("utility"), QStringLiteral("compressor"), QStringLiteral("eq")});
        test::click(window(), test::centerOf(test::part(frame(added[1]), "title")));
        QVERIFY(devices().groupSelected());
        const QString rack = chain(devices().trackId())[1];
        save(rack, QStringLiteral("Mine"));
        save(added[0], QStringLiteral("Mine"));
        save(added[2], QStringLiteral("Mine"));  // (a device with an editor of its own)
        QVERIFY(QFileInfo::exists(presetPath(QStringLiteral("Audio Effect Rack"), QStringLiteral("Mine"))));
        QVERIFY(QFileInfo::exists(presetPath(QStringLiteral("Utility"), QStringLiteral("Mine"))));
        QVERIFY(QFileInfo::exists(
            sub::app::presetPath(project().device(devices().trackId(), added[2]), QStringLiteral("Mine"))));
        // The menu's Save Preset… too.
        area()->scrollTo(added[0]);
        ui_.rightClick(test::centerOf(test::part(frame(added[0]), "title")));
        QVERIFY(ui_.choose(QStringLiteral("Save Preset…")));
        QVERIFY(opened(nameDialog()));
        QMetaObject::invokeMethod(nameDialog(), "reject");
        QVERIFY(closed(nameDialog()));
    }

    void droppingAPresetOnTheDeviceView() {
        QStringList added;
        const QString track = shownTrack(added, {QStringLiteral("utility"), QStringLiteral("compressor")});
        const QString source = added[0], compressor = added[1];
        editor().setDeviceParam(track, source, QStringLiteral("gain"), -11.0);
        save(source, QStringLiteral("Down"));
        const QString path = presetPath(QStringLiteral("Utility"), QStringLiteral("Down"));
        QVERIFY(QFileInfo::exists(path));
        editor().removeDevice(track, source);
        ui_.polish();
        auto mime = test::presetsMime({path});

        // Between devices (here: before the compressor): a new device.
        test::dragAndDrop(window(), ui_.inArea(4, 20), mime.get());
        QCOMPARE(kinds(track), (QStringList{"utility", "compressor"}));
        QCOMPARE(project().track(track).devices.front().params.value(QStringLiteral("gain")), -11.0);
        QCOMPARE(undo().undoText(), QStringLiteral("Load Preset Down"));

        // Onto a device of another kind: a new device too, where it was dropped (its middle: before it).
        test::dragAndDrop(window(), test::centerOf(frame(compressor)), mime.get());
        QCOMPARE(kinds(track), (QStringList{"utility", "utility", "compressor"}));
        undo().undo();

        // Onto a device of its kind: loads into it (outlined meanwhile), one undo step.
        const QString target = editor().addDevice(track, QStringLiteral("utility"));
        ui_.polish();
        const int count = undo().count();
        const QPoint onIt = test::centerOf(frame(target));
        QVERIFY(test::dragEnter(window(), onIt, mime.get()));
        test::dragMove(window(), onIt, mime.get());
        auto* loadMarker = test::part(ui_.panel(), "loadMarker");
        QVERIFY(loadMarker->isVisible());
        QVERIFY(!test::part(ui_.panel(), "dropMarker")->isVisible());
        QCOMPARE(loadMarker->mapRectToScene(QRectF(0, 0, loadMarker->width(), loadMarker->height())),
                 frame(target)->mapRectToScene(QRectF(0, 0, frame(target)->width(), frame(target)->height())));
        ui_.screenshot(QStringLiteral("load-marker"));
        test::dropAt(window(), onIt, mime.get());
        QVERIFY(!loadMarker->isVisible());
        QCOMPARE(chain(track).last(), target);
        QCOMPARE(project().device(track, target).params.value(QStringLiteral("gain")), -11.0);
        QCOMPARE(undo().count(), count + 1);
        undo().undo();
        QCOMPARE(project().device(track, target).params.value(QStringLiteral("gain")), 0.0);
        // Two presets at once go in as new devices, even onto one of their kind.
        auto two = test::presetsMime({path, path});
        test::dragAndDrop(window(), onIt, two.get());
        QCOMPARE(kinds(track), (QStringList{"utility", "compressor", "utility", "utility", "utility"}));
    }

    void saveAsDefaultPreset() {
        QStringList added;
        const QString track = shownTrack(added);
        editor().setDeviceParam(track, added[0], QStringLiteral("gain"), -5.0);
        ui_.rightClick(test::centerOf(test::part(frame(added[0]), "title")));
        QVERIFY(ui_.menuOpened());
        const bool hadDefault = test::menuEnabled(ui_.menu(), QStringLiteral("Clear Default Preset"));
        QVERIFY(!hadDefault);
        QVERIFY(ui_.choose(QStringLiteral("Save as Default Preset")));
        QVERIFY(ui_.lastMessage().contains(QStringLiteral("New Utility devices will start like this one")));
        session().addDeviceToSelectedTrack(QStringLiteral("utility"));  // as from the browser
        const QString added2 = chain(track).last();
        QCOMPARE(project().device(track, added2).params.value(QStringLiteral("gain")), -5.0);
        ui_.polish();
        ui_.rightClick(test::centerOf(test::part(frame(added2), "title")));
        QVERIFY(ui_.choose(QStringLiteral("Clear Default Preset")));
        session().addDeviceToSelectedTrack(QStringLiteral("utility"));
        QCOMPARE(project().device(track, chain(track).last()).params.value(QStringLiteral("gain")), 0.0);
    }

    void aRackIsTitledAsItsPreset() {
        QStringList added;
        const QString track = shownTrack(added);
        test::click(window(), test::centerOf(test::part(frame(added[0]), "title")));
        QVERIFY(devices().groupSelected());
        const QString rack = chain(track).front();
        ui_.polish();
        QCOMPARE(test::part(frame(rack), "title")->property("text").toString(), QStringLiteral("Audio Effect Rack"));
        QCOMPARE(save(rack, QStringLiteral("Glue")), QStringLiteral("Audio Effect Rack"));
        QCOMPARE(test::part(frame(rack), "title")->property("text").toString(), QStringLiteral("Glue"));
        QCOMPARE(undo().undoText(), QStringLiteral("Save Preset Glue"));
        // Loaded elsewhere, it is titled as the preset too.
        QVERIFY(devices().insertPreset(presetPath(QStringLiteral("Audio Effect Rack"), QStringLiteral("Glue"))));
        const QString loaded = chain(track).last();
        QCOMPARE(test::part(frame(loaded), "title")->property("text").toString(), QStringLiteral("Glue"));
    }

    void loadPresetBesideTheDevices() {
        QStringList added;
        const QString track = shownTrack(added, {QStringLiteral("utility"), QStringLiteral("utility")});
        editor().setDeviceParam(track, added[0], QStringLiteral("gain"), -3.0);
        const QString path = devices().savePreset(added[0], QStringLiteral("Between"));
        QVERIFY(!path.isEmpty());
        // Right-click between the two: there.
        ui_.rightClick(ui_.inArea(216 + 3, 40));
        QVERIFY(ui_.choose(QStringLiteral("Load Preset…")));
        auto* dialog = ui_.panel()->findChild<QObject*>(QStringLiteral("presetFileDialog"));
        QVERIFY(dialog);
        QCOMPARE(dialog->property("index").toInt(), 1);
        QCOMPARE(dialog->property("currentFolder").toUrl(), QUrl::fromLocalFile(devices().presetFolder()));
        QTRY_VERIFY(dialog->property("visible").toBool());
        QMetaObject::invokeMethod(dialog, "reject");
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(chain(track).size(), 2);
        // The file chosen goes where the menu was asked for (as the dialog's accepted does).
        QVERIFY(devices().loadPresetFile(path, dialog->property("chain").toString(), dialog->property("index").toInt()));
        QCOMPARE(chain(track).size(), 3);
        QCOMPARE(chain(track)[0], added[0]);
        QCOMPARE(chain(track)[2], added[1]);
        QCOMPARE(project().device(track, chain(track)[1]).params.value(QStringLiteral("gain")), -3.0);
        QCOMPARE(QFileInfo(devices().presetFolder()).absoluteFilePath(), QFileInfo(path).absolutePath());  // remembered
    }
};

QTEST_MAIN(TestUiDevicePanelPresets)
#include "test_ui_device_panel_presets.moc"
