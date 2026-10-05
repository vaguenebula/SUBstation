// VST3 plug-ins in the device view (DevicePanel.qml's plug-in devices:
// PluginDeviceBody, PluginParamKnob), with the test plug-ins
// (tests/vst3_plugins): a plug-in's parameters in pages (lists and knobs, the
// plug-in's own texts), edited undoably and following automation; devices
// fitting the view; the editor button, double-click and Show Editor (the test
// plug-ins have no editor here: said so); VST3 presets; plug-ins loading after
// a project opens, and missing ones; dropping plug-ins; selecting, deleting and
// reordering devices; effects dropped beside an instrument; scrolling to a
// plug-in added; the master's effects. Driven in a window with a real session,
// with synthesized mouse, key and drag events. With SUBSTATION_UI_SCREENSHOTS
// set, screenshots go there.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>

#include <cmath>
#include <optional>

#include "BridgeTestSupport.h"
#include "DevicePanelTestSupport.h"
#include "TestSupport.h"
#include "audio/EngineBridge.h"
#include "controls/KnobItem.h"
#include "devices/DeviceChainArea.h"
#include "devices/PluginParams.h"
#include "editor/ProjectEditor.h"
#include "model/Automation.h"
#include "model/Device.h"
#include "model/Project.h"
#include "session/DeviceSelection.h"
#include "session/Selection.h"

namespace test = sub::app::test;
using sub::app::BridgeTestAccess;
using sub::app::PluginRef;
using sub::app::Selection;

class TestUiDevicePanelPlugins : public QObject {
    Q_OBJECT

    test::PanelWindow ui_;
    std::unique_ptr<test::TempDir> dir_;

    sub::app::Session& session() { return ui_.session(); }
    sub::app::Project& project() { return *session().project(); }
    sub::app::ProjectEditor& editor() { return *session().editor(); }
    sub::app::DeviceSelection& devices() { return *session().deviceSelection(); }
    sub::app::EngineBridge& bridge() { return *session().bridge(); }
    QUndoStack& undo() { return *session().undoStack(); }
    QQuickWindow* window() { return ui_.window(); }
    sub::ui::DeviceChainArea* area() { return ui_.area(); }

    static PluginRef plugin(const QString& name) { return *test::testPlugin(test::testPluginsBundle(), name); }
    static QVariantMap refOf(const QString& name) {
        const PluginRef ref = plugin(name);
        return {{"format", ref.format}, {"uid", ref.uid},   {"name", ref.name},
                {"vendor", ref.vendor}, {"path", ref.path}, {"instrument", ref.instrument}};
    }
    QStringList chain(const QString& trackId) {
        QStringList out;
        for (const sub::app::Device& device : project().track(trackId).devices) out << device.id;
        return out;
    }
    QStringList names(const QString& trackId) {
        QStringList out;
        for (const sub::app::Device& device : project().track(trackId).devices)
            out << (device.plugin ? device.plugin->name : device.kind);
        return out;
    }
    double value(const QString& trackId, const QString& deviceId, int index) {
        return bridge().deviceParamValue(trackId, deviceId, index).value_or(-1.0);
    }

    // A MIDI track with SUB Test Synth, selected; (track, synth).
    std::pair<QString, QString> synthTrack() {
        const QString track = editor().addMidiTrack(-1, QString(), QString(), plugin(QStringLiteral("SUB Test Synth")));
        session().selection()->selectTrack(track);
        ui_.polish();
        return {track, chain(track).front()};
    }

    QQuickItem* frame(const QString& deviceId) { return ui_.frame(deviceId); }
    QQuickItem* cell(const QString& deviceId, int index) {
        return test::itemNamed(frame(deviceId), QStringLiteral("param_%1").arg(index));
    }
    // The parameters a plug-in's frame shows (their indices), in order.
    QList<int> shownParams(const QString& deviceId) {
        ui_.polish();
        QList<int> indices;
        QList<QQuickItem*> cells;
        for (int i = 0; i < 64; ++i) {
            if (QQuickItem* c = cell(deviceId, i)) cells << c;
        }
        std::sort(cells.begin(), cells.end(), [](QQuickItem* a, QQuickItem* b) {
            const QPointF pa = a->mapToScene(QPointF()), pb = b->mapToScene(QPointF());
            return pa.y() != pb.y() ? pa.y() < pb.y() : pa.x() < pb.x();
        });
        for (QQuickItem* c : cells) indices << c->property("index").toInt();
        return indices;
    }
    void clickTitle(const QString& deviceId, Qt::KeyboardModifiers modifiers = {}) {
        test::click(window(), test::centerOf(test::part(frame(deviceId), "title")), modifiers);
    }
    QString message(const QString& deviceId) {
        QQuickItem* text = test::itemNamed(frame(deviceId), QStringLiteral("pluginMessage"));
        return text ? text->property("text").toString() : QString();
    }

private Q_SLOTS:
    void initTestCase() {
        test::prepareApplication();
        if (!test::haveDisplay()) QSKIP("needs a display: Qt Quick's software renderer draws none of this geometry");
        if (!test::haveTestPlugins(test::testPluginsBundle())) QSKIP("the test plug-ins weren't built");
        sub::ui::setUpApplication();
        QVERIFY(ui_.open());
        bridge().setKnownPlugins(test::testPluginInfos(test::testPluginsBundle()));
    }

    void cleanupTestCase() { ui_.close(); }

    void init() {
        if (ui_.menu()->property("visible").toBool()) QVERIFY(ui_.closeMenu());
        project().clear();
        undo().clear();
        ui_.messages.clear();
        area()->setContentX(0);
        dir_ = std::make_unique<test::TempDir>();
        test::moveTo(window(), QPoint(5, 5), {}, Qt::NoButton);
    }

    void aPluginsParametersInPages() {
        const auto [track, synth] = synthTrack();
        QQuickItem* f = frame(synth);
        QVERIFY(f);
        QCOMPARE(test::part(f, "title")->property("text").toString(), QStringLiteral("SUB Test Synth"));
        QVERIFY(test::part(f, "editButton")->isVisible());
        // The generic editor offers what the user can change (not the read-only
        // parameters), four at a time in a 2×2 grid.
        QCOMPARE(shownParams(synth), (QList<int>{test::kSynthGain, test::kSynthWave, 6, 7}));
        QVERIFY(test::part(cell(synth, test::kSynthGain), "knob")->isVisible());
        QQuickItem* wave = test::part(cell(synth, test::kSynthWave), "list");
        QVERIFY(wave->isVisible());
        QCOMPARE(wave->property("currentText").toString(), QStringLiteral("Sine"));
        QCOMPARE(f->property("pages").toInt(), 3);
        QCOMPARE(test::part(f, "pageLabel")->property("text").toString(), QStringLiteral("1/3"));
        QVERIFY(!test::part(f, "previousButton")->isEnabled());
        const QPointF gainAt = cell(synth, test::kSynthGain)->mapToScene(QPointF());
        const QPointF waveAt = cell(synth, test::kSynthWave)->mapToScene(QPointF());
        const QPointF sixAt = cell(synth, 6)->mapToScene(QPointF());
        QCOMPARE(gainAt.y(), waveAt.y());
        QCOMPARE(waveAt.x() - gainAt.x(), 84.0 + 16.0);  // PARAM_WIDTH and the grid's spacing
        QCOMPARE(sixAt.x(), gainAt.x());
        QVERIFY(sixAt.y() > gainAt.y());
        test::click(window(), test::centerOf(test::part(f, "nextButton")));
        test::click(window(), test::centerOf(test::part(f, "nextButton")));
        QCOMPARE(shownParams(synth), (QList<int>{12, 13, 14, 15}));
        QCOMPARE(test::part(f, "pageLabel")->property("text").toString(), QStringLiteral("3/3"));
        QVERIFY(!test::part(f, "nextButton")->isEnabled());
        editor().addDevice(track, QStringLiteral("utility"));  // the device view changes...
        QCOMPARE(shownParams(synth), (QList<int>{12, 13, 14, 15}));  // ...on the same page
        session().selection()->selectTrack(QString());
        session().selection()->selectTrack(track);  // made again: still on that page
        QCOMPARE(shownParams(synth), (QList<int>{12, 13, 14, 15}));
        // The title's tooltip: its name, vendor and file.
        QObject* info = frame(synth)->property("info").value<QObject*>();
        QVERIFY(info->property("toolTip").toString().startsWith(QStringLiteral("SUB Test Synth\n")));
        QVERIFY(info->property("toolTip").toString().contains(QFileInfo(test::testPluginsBundle()).fileName()));
    }

    void knobsEditPluginsUndoably() {
        const auto [track, synth] = synthTrack();
        auto* knob = qobject_cast<sub::ui::KnobItem*>(test::part(cell(synth, test::kSynthGain), "knob"));
        QVERIFY(knob);
        const int height = window()->height();
        window()->setHeight(height + 320);  // (room below for the drag)
        const QPoint center = test::centerOf(knob);
        test::press(window(), center);
        test::moveTo(window(), center + QPoint(0, 150));
        test::moveTo(window(), center + QPoint(0, 300));  // half the range down
        test::release(window(), center + QPoint(0, 300));
        window()->setHeight(height);
        QVERIFY(std::abs(value(track, synth, test::kSynthGain) - 0.5) < 0.02);
        auto readout = [this, &synth] {
            return test::itemNamed(cell(synth, test::kSynthGain), QStringLiteral("readout"))->property("text").toString();
        };
        QVERIFY2(readout().startsWith(QStringLiteral("0.5")), qPrintable(readout()));
        QCOMPARE(undo().count(), 2);  // the track, one knob drag
        QQuickItem* wave = test::part(cell(synth, test::kSynthWave), "list");
        QMetaObject::invokeMethod(wave, "activated", Q_ARG(int, 0));
        QCOMPARE(value(track, synth, test::kSynthWave), 0.0);
        QCOMPARE(wave->property("currentText").toString(), QStringLiteral("DC"));
        undo().undo();
        undo().undo();
        QCOMPARE(value(track, synth, test::kSynthGain), 1.0);
        QCOMPARE(value(track, synth, test::kSynthWave), 1.0);
        QCOMPARE(wave->property("currentText").toString(), QStringLiteral("Sine"));
        QVERIFY2(readout().startsWith(QStringLiteral("1.0")), qPrintable(readout()));
        undo().redo();
        QVERIFY(std::abs(value(track, synth, test::kSynthGain) - 0.5) < 0.02);
        QVERIFY(std::abs(knob->value() - 0.5) < 0.02);
    }

    void automatingPluginParameters() {
        const auto [track, synth] = synthTrack();
        auto* knob = qobject_cast<sub::ui::KnobItem*>(test::part(cell(synth, test::kSynthGain), "knob"));
        const QString key = sub::app::automation::deviceKey(synth, QString::number(test::kSynthGain));
        const QPoint center = test::centerOf(knob);
        test::drag(window(), center, center + QPoint(0, 30));  // turning it shows its automation
        QCOMPARE(project().automationView(track).key, key);
        // An envelope drives it: the plug-in and the knob follow.
        editor().setEnvelope(track, key, {sub::app::AutomationPoint{0.0, 0.25}});
        ui_.engine().renderOffline(0.0, 256);
        bridge().pollPlugins();
        QVERIFY(std::abs(value(track, synth, test::kSynthGain) - 0.25) < 1e-3);
        QTRY_VERIFY(std::abs(knob->value() - 0.25) < 1e-3);
        QCOMPARE(knob->automation(), QStringLiteral("on"));
        // Turning it by hand overrides the envelope, until automation is re-enabled.
        test::drag(window(), center, center + QPoint(0, 30));
        QVERIFY(bridge().isOverridden(track, key));
        QCOMPARE(knob->automation(), QStringLiteral("off"));
        bridge().reEnableAutomation();
        QCOMPARE(knob->automation(), QStringLiteral("on"));
        // Its menu: the automation's entries.
        QObject* menu = cell(synth, test::kSynthGain)->property("menu").value<QObject*>();
        QMetaObject::invokeMethod(menu, "build");
        QCOMPARE(test::menuTexts(menu), (QStringList{"Show Automation", "Delete Automation"}));
    }

    void devicesFitTheDeviceView() {
        const auto [track, synth] = synthTrack();
        editor().addDevice(track, QStringLiteral("ott"));
        PluginRef missing;
        missing.uid = QString(32, u'0');
        missing.name = QStringLiteral("Missing");
        missing.path = QStringLiteral("C:/nowhere/Missing.vst3");
        const QString broken = editor().addDevice(track, sub::app::kPluginKind, -1, missing);
        for (int i = 0; i < 4; ++i) editor().addDevice(track, QStringLiteral("utility"));
        ui_.polish();
        QTRY_VERIFY(area()->maxContentX() > 0);  // it scrolls sideways (no scroll bar)...
        for (const QString& id : devices().shownDevices()) {  // ...never vertically: every device fits
            QQuickItem* f = frame(id);
            QCOMPARE(f->height(), area()->height());
            QQuickItem* body = test::itemNamed(f, QStringLiteral("body"));
            QVERIFY(body);
            const auto* loaded = f->property("body").value<QQuickItem*>();
            QVERIFY2(loaded && loaded->implicitHeight() <= body->height(), qPrintable(id));
        }
        // A plug-in that isn't there says why, in four lines at most (its tooltip has it all).
        QCOMPARE(message(broken), QStringLiteral("Missing is not installed."));
        QQuickItem* text = test::itemNamed(frame(broken), QStringLiteral("pluginMessage"));
        QCOMPARE(text->property("maximumLineCount").toInt(), 4);
        QVERIFY(!test::part(frame(broken), "editButton")->isEnabled());
        QVERIFY(!test::part(frame(broken), "sidechainButton")->isVisible());
    }

    void theEditorButtonAndDoubleClick() {
        const QString track = editor().addAudioTrack(-1, QStringLiteral("Drums"));
        session().selection()->selectTrack(track);
        const QString effect = editor().addDevice(track, sub::app::kPluginKind, -1, plugin(QStringLiteral("SUB Test Effect")));
        ui_.polish();
        QQuickItem* button = test::part(frame(effect), "editButton");
        QVERIFY(button->isVisible() && button->isEnabled());
#ifdef Q_OS_WIN
        // Here the test effect has an editor (a Win32 window). Adding it opened it (its
        // track is selected); the button closes it and opens it again; so does a double-click.
        QTRY_VERIFY(bridge().isPluginEditorOpen(track, effect));
        QTRY_VERIFY(button->property("checked").toBool());
        test::click(window(), test::centerOf(button));
        QTRY_VERIFY(!bridge().isPluginEditorOpen(track, effect));
        QTRY_VERIFY(!button->property("checked").toBool());
        test::doubleClick(window(), test::centerOf(test::part(frame(effect), "title")));
        QTRY_VERIFY(bridge().isPluginEditorOpen(track, effect));
        QTRY_VERIFY(button->property("checked").toBool());
        test::click(window(), test::centerOf(button));
        QTRY_VERIFY(!bridge().isPluginEditorOpen(track, effect));
#else
        QVERIFY(!button->property("checked").toBool());
        QTest::qWait(10);  // (adding it asked for its editor, quietly)
        QVERIFY(!ui_.lastMessage().contains(QStringLiteral("no editor")));
        ui_.messages.clear();
        test::click(window(), test::centerOf(button));  // the test plug-ins have no editor here
        QCOMPARE(ui_.lastMessage(), QStringLiteral("SUB Test Effect has no editor."));
        QVERIFY(!button->property("checked").toBool());
        ui_.messages.clear();
        test::doubleClick(window(), test::centerOf(test::part(frame(effect), "title")));  // double-click: its editor
        QCOMPARE(ui_.lastMessage(), QStringLiteral("SUB Test Effect has no editor."));
        ui_.messages.clear();
        ui_.rightClick(test::centerOf(test::part(frame(effect), "title")));
        QVERIFY(ui_.menuOpened());
        QCOMPARE(test::menuTexts(ui_.menu()).mid(0, 4),
                 (QStringList{"Show Editor", "Load VST3 Preset…", "Save VST3 Preset…", ""}));
        QVERIFY(ui_.choose(QStringLiteral("Show Editor")));
        QCOMPARE(ui_.lastMessage(), QStringLiteral("SUB Test Effect has no editor."));
        // Its state follows the bridge's word.
        Q_EMIT bridge().pluginEditorChanged(track, effect);
        QVERIFY(!button->property("checked").toBool());
#endif
    }

    void vst3Presets() {
        const auto [track, synth] = synthTrack();
        const quint32 processor = *bridge().engineDeviceId(track, synth);
        ui_.engine().setProcessorParam(processor, test::kSynthGain, 0.25f);
        const QString preset = dir_->path(QStringLiteral("Quiet.vstpreset"));
        ui_.rightClick(test::centerOf(test::part(frame(synth), "title")));
        QVERIFY(ui_.choose(QStringLiteral("Save VST3 Preset…")));  // (its dialog, where VST3 presets go)
        auto* saveDialog = ui_.panel()->findChild<QObject*>(QStringLiteral("vst3SaveDialog"));
        QTRY_VERIFY(saveDialog->property("visible").toBool());
        QCOMPARE(saveDialog->property("deviceId").toString(), synth);
        QVERIFY(saveDialog->property("selectedFile").toUrl().toLocalFile().endsWith(QStringLiteral("/SUB Test Synth.vstpreset")));
        QMetaObject::invokeMethod(saveDialog, "reject");
        QVERIFY(devices().saveVst3Preset(synth, preset));  // (the file chosen)
        QFile file(preset);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.read(4), QByteArray("VST3"));
        file.close();

        ui_.engine().setProcessorParam(processor, test::kSynthGain, 0.8f);
        ui_.rightClick(test::centerOf(test::part(frame(synth), "title")));
        QVERIFY(ui_.choose(QStringLiteral("Load VST3 Preset…")));
        auto* loadDialog = ui_.panel()->findChild<QObject*>(QStringLiteral("vst3LoadDialog"));
        QTRY_VERIFY(loadDialog->property("visible").toBool());
        QCOMPARE(loadDialog->property("currentFolder").toUrl(), QUrl::fromLocalFile(dir_->path()));  // remembered
        QMetaObject::invokeMethod(loadDialog, "reject");
        QVERIFY(devices().loadVst3Preset(synth, preset));
        QVERIFY(std::abs(value(track, synth, test::kSynthGain) - 0.25) < 1e-3);
        QCOMPARE(undo().undoText(), QStringLiteral("Load Preset Quiet"));
        auto* knob = qobject_cast<sub::ui::KnobItem*>(test::part(cell(synth, test::kSynthGain), "knob"));
        QTRY_VERIFY(std::abs(knob->value() - 0.25) < 1e-3);
        undo().undo();
        QVERIFY(std::abs(value(track, synth, test::kSynthGain) - 0.8) < 1e-3);

        // Another plug-in's preset is refused, and changes nothing.
        const QString other = editor().addDevice(track, sub::app::kPluginKind, -1, plugin(QStringLiteral("SUB Test Effect")));
        const QString otherPreset = dir_->path(QStringLiteral("Effect.vstpreset"));
        QVERIFY(devices().saveVst3Preset(other, otherPreset));
        const int commands = undo().count();
        QVERIFY(!devices().loadVst3Preset(synth, otherPreset));
        QCOMPARE(undo().count(), commands);
        QVERIFY2(ui_.lastMessage().contains(QStringLiteral("not for SUB Test Synth")), qPrintable(ui_.lastMessage()));
    }

    void pluginsLoadingAfterAProjectOpensAndMissingOnes() {
        const auto [track, synth] = synthTrack();
        Q_UNUSED(track);
        ui_.engine().setProcessorParam(*bridge().engineDeviceId(track, synth), test::kSynthGain, 0.3f);
        const QString path = dir_->path(QStringLiteral("song.gilproj"));
        QVERIFY(session().saveProjectAs(path));
        // A second device whose plug-in isn't installed.
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonObject data = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        QJsonArray tracks = data.value("tracks").toArray();
        QJsonObject first = tracks[0].toObject();
        QJsonArray list = first.value("devices").toArray();
        QJsonObject gone = list[0].toObject();
        gone["id"] = QStringLiteral("gone");
        QJsonObject ref = gone.value("plugin").toObject();
        ref["uid"] = QString(32, u'0');
        ref["name"] = QStringLiteral("Gone Synth");
        ref["path"] = dir_->path(QStringLiteral("moved away/Gone.vst3"));
        ref["instrument"] = false;
        gone["plugin"] = ref;
        list.append(gone);
        first["devices"] = list;
        tracks[0] = first;
        data["tracks"] = tracks;
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(data).toJson());
        file.close();

        QVERIFY(session().openProject(path));
        BridgeTestAccess::stopPluginTimer(bridge());  // (they load one by one, by hand here)
        const QString opened = project().tracks().front().id;
        session().selection()->selectTrack(opened);
        ui_.polish();
        QVERIFY(bridge().pluginPending(synth));
        QCOMPARE(message(synth), QStringLiteral("SUB Test Synth is loading…"));
        QCOMPARE(message(QStringLiteral("gone")), QStringLiteral("Gone Synth is not installed."));
        QVERIFY(!test::part(frame(QStringLiteral("gone")), "editButton")->isEnabled());
        ui_.screenshot(QStringLiteral("loading-and-missing"));
        while (bridge().pluginsPending() > 0) BridgeTestAccess::loadNextPlugin(bridge());
        ui_.polish();
        QCOMPARE(message(synth), QString());
        QCOMPARE(shownParams(synth).size(), 4);
        auto* knob = qobject_cast<sub::ui::KnobItem*>(test::part(cell(synth, test::kSynthGain), "knob"));
        QVERIFY(knob && std::abs(knob->value() - 0.3) < 1e-3);
        QVERIFY(test::part(frame(synth), "editButton")->isEnabled());
        QCOMPARE(message(QStringLiteral("gone")), QStringLiteral("Gone Synth is not installed."));
    }

    void droppingPlugins() {
        const auto [track, synth] = synthTrack();
        Q_UNUSED(synth);
        auto mime = test::pluginsMime({refOf(QStringLiteral("SUB Test Mono"))});
        test::dragAndDrop(window(), ui_.beside(40), mime.get());  // the track shown, last
        QCOMPARE(names(track), (QStringList{"SUB Test Synth", "SUB Test Mono"}));
        // An instrument dropped on an audio track is refused.
        const QString audio = editor().addAudioTrack();
        session().selection()->selectTrack(audio);
        auto synthMime = test::pluginsMime({refOf(QStringLiteral("SUB Test Synth"))});
        test::dragAndDrop(window(), ui_.beside(40), synthMime.get());
        QVERIFY(chain(audio).isEmpty());
        QCOMPARE(ui_.lastMessage(), sub::app::kInstrumentRefused);
    }

    void selectingDeletingAndReorderingDevices() {
        const auto [track, synth] = synthTrack();
        QStringList effects;
        for (const char* name : {"SUB Test Effect", "SUB Test Mono", "SUB Test Effect"})
            effects << editor().addDevice(track, sub::app::kPluginKind, -1, plugin(QString::fromLatin1(name)));
        const QString utility = editor().addDevice(track, QStringLiteral("utility"));
        area()->setContentX(0);
        ui_.polish();
        const QString first = effects[0], second = effects[1], third = effects[2];
        QCOMPARE(chain(track), (QStringList{synth, first, second, third, utility}));
        auto visible = [this](const QString& id) {
            area()->scrollTo(id);
            ui_.polish();
        };

        // Clicks on a device's title select it; Shift selects a range, Ctrl one more or less.
        visible(first);
        clickTitle(first);
        QCOMPARE(devices().selected(), QStringList{first});
        QVERIFY(frame(first)->property("selected").toBool());
        QCOMPARE(session().selection()->focus(), Selection::Focus::Devices);
        visible(utility);
        clickTitle(utility, Qt::ShiftModifier);
        QCOMPARE(devices().selected(), (QStringList{first, second, third, utility}));
        visible(second);
        clickTitle(second, Qt::ControlModifier);
        QCOMPARE(devices().selected(), (QStringList{first, third, utility}));
        QVERIFY(!frame(second)->property("selected").toBool());
        visible(synth);
        clickTitle(synth);
        visible(second);
        clickTitle(second, Qt::ShiftModifier);  // the instrument can be selected too
        QCOMPARE(devices().selected(), (QStringList{synth, first, second}));

        // Delete deletes them all, in one undo step.
        const int steps = undo().count();
        session().deleteSelection();
        QCOMPARE(chain(track), (QStringList{third, utility}));
        QCOMPARE(undo().count(), steps + 1);
        undo().undo();
        QCOMPARE(chain(track), (QStringList{synth, first, second, third, utility}));
        session().selection()->clear();  // selecting something else deselects the devices
        QVERIFY(devices().selected().isEmpty());
        session().deleteSelection();
        QCOMPARE(chain(track).size(), 5);

        // Dragging effects (selected together) reorders them; the instrument stays first.
        auto dropBefore = [&](const QString& deviceId, const QStringList& moving) {
            area()->scrollTo(deviceId);
            QQuickItem* f = frame(deviceId);
            auto mime = test::movedMime(track, moving);
            test::dragAndDrop(window(), ui_.at(f, 2, f->height() / 2), mime.get());
        };
        dropBefore(first, {third, utility});
        QCOMPARE(chain(track), (QStringList{synth, third, utility, first, second}));
        QCOMPARE(undo().undoText(), QStringLiteral("Move Devices"));
        dropBefore(synth, {second, synth});  // nothing goes before the instrument, which doesn't move
        QCOMPARE(chain(track), (QStringList{synth, second, third, utility, first}));
        undo().undo();
        undo().undo();
        QCOMPARE(chain(track), (QStringList{synth, first, second, third, utility}));
        // A device's menu offers Delete for all the selected devices.
        visible(first);
        clickTitle(first);
        visible(second);
        clickTitle(second, Qt::ShiftModifier);
        ui_.rightClick(test::centerOf(test::part(frame(second), "title")));
        QVERIFY(ui_.choose(QStringLiteral("Delete")));
        QCOMPARE(chain(track), (QStringList{synth, third, utility}));
    }

    void effectsDroppedBesideAnInstrument() {
        const QString track = editor().addMidiTrack(-1, QString(), QString());  // no instrument
        session().selection()->selectTrack(track);
        const QString fx1 = editor().addDevice(track, QStringLiteral("utility"));
        const QString fx2 = editor().addDevice(track, QStringLiteral("utility"));
        auto dropAt = [&](const QString& deviceId, const QStringList& plugins) {
            QVariantList refs;
            for (const QString& name : plugins) refs << refOf(name);
            auto mime = test::pluginsMime(refs);
            area()->scrollTo(deviceId);
            QQuickItem* f = frame(deviceId);
            test::dragAndDrop(window(), ui_.at(f, 2, f->height() / 2), mime.get());
        };
        dropAt(fx2, {QStringLiteral("SUB Test Synth"), QStringLiteral("SUB Test Effect")});  // between the two utilities
        QStringList now = chain(track);
        QCOMPARE(now.size(), 4);
        const QString synth = now[0], effect = now[2];
        QCOMPARE(now, (QStringList{synth, fx1, effect, fx2}));
        QCOMPARE(names(track)[0], QStringLiteral("SUB Test Synth"));
        dropAt(synth, {QStringLiteral("SUB Test Mono"), QStringLiteral("SUB Test Effect")});  // before the instrument: right after it, in order
        QCOMPARE(names(track).mid(1, 2), (QStringList{"SUB Test Mono", "SUB Test Effect"}));
        QCOMPARE(chain(track)[0], synth);
        QCOMPARE(chain(track).mid(3), (QStringList{fx1, effect, fx2}));
    }

    void addingAPluginScrollsToIt() {
        const QString track = editor().addMidiTrack(-1, QString(), QString());
        session().selection()->selectTrack(track);
        for (int i = 0; i < 12; ++i) editor().addDevice(track, QStringLiteral("utility"));
        QTRY_VERIFY(area()->maxContentX() > 0);
        auto inView = [this](const QString& id) {
            QQuickItem* f = frame(id);
            const qreal left = f->mapToItem(area(), QPointF()).x();
            return left >= 0 && left + f->width() <= area()->width();
        };
        // Added from the browser while scrolled to the start: the chain scrolls to it.
        QTest::qWait(10);
        area()->setContentX(0);
        session().addPluginToSelectedTrack(refOf(QStringLiteral("SUB Test Effect")));
        const QString added = chain(track).last();
        QTRY_VERIFY(area()->contentX() > 0);
        QVERIFY(inView(added));
        // Dropped on the chain, where the user is looking, it doesn't scroll, even for
        // an instrument, which goes first.
        area()->setContentX(area()->maxContentX());
        auto mime = test::pluginsMime({refOf(QStringLiteral("SUB Test Synth"))});
        test::dragAndDrop(window(), ui_.at(frame(added), 2, frame(added)->height() / 2), mime.get());
        QCOMPARE(names(track).front(), QStringLiteral("SUB Test Synth"));
        QTest::qWait(50);
        QVERIFY(inView(added));
        QVERIFY(!inView(chain(track).front()));
    }

    void effectsOnTheMaster() {
        session().selection()->selectTrack(sub::app::kMaster);
        QCOMPARE(devices().trackId(), sub::app::kMaster);
        QCOMPARE(test::part(ui_.panel(), "hint")->property("text").toString(), sub::app::kEffectsHint);
        session().addDeviceToSelectedTrack(QStringLiteral("utility"));  // from the browser
        const QString utility = chain(sub::app::kMaster).front();
        QVERIFY(frame(utility));
        QVERIFY(editor().addDevice(sub::app::kMaster, QStringLiteral("synth")).isEmpty());  // instruments go on MIDI tracks
        const QString effect =
            editor().addDevice(sub::app::kMaster, sub::app::kPluginKind, -1, plugin(QStringLiteral("SUB Test Effect")));
        ui_.polish();
        QVERIFY(frame(effect));
        QCOMPARE(chain(sub::app::kMaster), (QStringList{utility, effect}));
        QVERIFY(bridge().pluginErrors().isEmpty());
    }

    void aChainOfDevices() {
        const QString track = editor().addAudioTrack(-1, QStringLiteral("Bus"));
        session().selection()->selectTrack(track);
        editor().addDevice(track, QStringLiteral("utility"));
        editor().addDevice(track, QStringLiteral("compressor"));
        editor().addDevice(track, QStringLiteral("eq"));
        const QString effect = editor().addDevice(track, sub::app::kPluginKind, -1, plugin(QStringLiteral("SUB Test Effect")));
        PluginRef missing = plugin(QStringLiteral("SUB Test Effect"));
        missing.uid = QString(32, u'0');
        missing.name = QStringLiteral("Vintage Tape");
        missing.path = QStringLiteral("C:/nowhere/Tape.vst3");
        editor().addDevice(track, sub::app::kPluginKind, -1, missing);
        QTest::qWait(20);  // (scrolled to the last one added)
        area()->setContentX(0);
        ui_.screenshot(QStringLiteral("chain"));
        area()->scrollTo(effect);
        ui_.screenshot(QStringLiteral("chain-plugins"));
        // All of it, in a window wide enough.
        const int width = window()->width();
        window()->setWidth(1910);
        QTRY_COMPARE(int(ui_.panel()->width()), 1910);
        area()->setContentX(0);
        ui_.screenshot(QStringLiteral("chain-wide"));
        window()->setWidth(width);
        QTRY_COMPARE(int(ui_.panel()->width()), width);
    }
};

QTEST_MAIN(TestUiDevicePanelPlugins)
#include "test_ui_device_panel_plugins.moc"
