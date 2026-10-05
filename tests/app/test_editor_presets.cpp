// Presets through the editor: loading a preset into a device of its kind (one
// undo step), new devices starting as their default preset, and racks named as
// the preset they come from (the editor parts of tests/test_presets.py).

#include "EditorFixture.h"
#include "TestSupport.h"

#include "io/Presets.h"
#include "io/Serialization.h"
#include "model/Devices.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

using namespace sub::app;
using test::EditorFixture;
using test::env;
namespace autom = sub::app::automation;

namespace {

// The device as a preset would load (a new device: fresh ids).
Device presetOf(const Device& device) { return presetDevice(deviceToPreset(device)); }

// The editor making new devices from the default presets in `root`.
void withDefaults(EditorFixture& f, const QString& root) {
    f.editor.setDeviceDefaults([root](const QString& kind, const std::optional<PluginRef>& plugin) {
        return defaultDevice(kind, plugin, root);
    });
}

}  // namespace

class TestEditorPresets : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    // --- Loading into a device ---

    void aPresetLoadsIntoABuiltInDeviceInOneUndoStep() {
        EditorFixture f;
        Project& p = f.project;
        const QString track = f.editor.addAudioTrack();
        const QString source = f.editor.addDevice(track, "utility");
        const QString target = f.editor.addDevice(track, "utility");
        f.editor.setDeviceParam(track, source, "gain", -9.0);
        f.editor.setDeviceParam(track, source, "pan", 0.5);
        f.editor.setDeviceEnabled(track, target, false);
        const Device preset = presetOf(p.device(track, source));
        const QMap<QString, double> before = p.device(track, target).params;
        const int count = f.stack.count();
        QVERIFY(f.editor.loadPresetInto(track, target, preset, "Load Preset Wide"));
        const Device& loaded = p.device(track, target);
        QCOMPARE(loaded.params, p.device(track, source).params);
        QVERIFY(!loaded.enabled && loaded.id == target);  // the device stays, with its switch
        QCOMPARE(f.stack.count(), count + 1);
        QCOMPARE(f.stack.undoText(), QStringLiteral("Load Preset Wide"));
        f.stack.undo();
        QCOMPARE(p.device(track, target).params, before);
        f.stack.redo();
        QCOMPARE(p.device(track, target).params.value("gain"), -9.0);
    }

    void aPresetLoadsOnlyIntoADeviceOfItsKind() {
        EditorFixture f;
        Project& p = f.project;
        const QString track = f.editor.addAudioTrack();
        const QString utility = f.editor.addDevice(track, "utility");
        const QString compressor = f.editor.addDevice(track, "compressor");
        int count = f.stack.count();
        QVERIFY(!f.editor.loadPresetInto(track, compressor, presetOf(p.device(track, utility))));
        QCOMPARE(f.stack.count(), count);
        const QString keys = f.editor.addMidiTrack();
        const QString instrumentRack = f.editor.groupDevices(keys, {f.track(keys).devices[0].id});
        const QString effectRack = f.editor.groupDevices(track, {utility});
        count = f.stack.count();
        QVERIFY(!f.editor.loadPresetInto(track, effectRack, presetOf(p.device(keys, instrumentRack))));
        QCOMPARE(f.stack.count(), count);
    }

    // Its chains (new devices) and macros replace the rack's; the rack stays,
    // and the automation of the devices that went goes with them, in the same step.
    void aRackPresetLoadsIntoARack() {
        EditorFixture f;
        Project& p = f.project;
        const QString track = f.editor.addAudioTrack();
        const QString a = f.editor.addDevice(track, "utility");
        const QString b = f.editor.addDevice(track, "utility");
        const QString source = f.editor.groupDevices(track, {a});
        f.editor.mapMacro(track, source, 0, a, "gain");
        f.editor.setMacro(track, source, 0, 0.25);
        const QString target = f.editor.groupDevices(track, {b});
        const QString key = autom::deviceKey(b, "gain");
        f.editor.setEnvelope(track, key, env({{0.0, 0.25}, {4.0, 0.5}}));
        const Device before = p.device(track, target);
        const int count = f.stack.count();
        QVERIFY(f.editor.loadPresetInto(track, target, presetOf(p.device(track, source))));
        const Device& rack = p.device(track, target);
        QCOMPARE(rack.chains[0].devices.size(), size_t(1));
        const Device& inner = rack.chains[0].devices[0];
        QVERIFY(inner.id != a && inner.id != b);
        QCOMPARE(inner.params, p.device(track, a).params);
        QCOMPARE(rack.macros.size(), size_t(1));
        QVERIFY(rack.macros[0].macro == 0 && rack.macros[0].deviceId == inner.id);
        QCOMPARE(rack.params, p.device(track, source).params);
        QVERIFY(p.envelope(track, key).empty());
        QCOMPARE(f.stack.count(), count + 1);
        f.stack.undo();
        QVERIFY(p.device(track, target) == before);
        QVERIFY(!p.envelope(track, key).empty());
    }

    void aPresetsStateLoadsWithIt() {
        // A plug-in takes the preset's state; a built-in device its parameters and state, as one step.
        EditorFixture f;
        Project& p = f.project;
        const QString track = f.editor.addAudioTrack();
        const PluginRef effect{"VST3", "B", "Effect B"};
        const QString fx = f.editor.addDevice(track, kPluginKind, -1, effect);
        Device preset = newDevice(kPluginKind, effect);
        preset.state = "c3RhdGU=";
        QVERIFY(f.editor.loadPresetInto(track, fx, preset));
        QCOMPARE(p.device(track, fx).state, std::optional<QString>("c3RhdGU="));
        QCOMPARE(f.stack.undoText(), QStringLiteral("Load Preset"));
        f.stack.undo();
        QVERIFY(!p.device(track, fx).state);
        preset.state.reset();  // (a plug-in's preset without a state: the plug-in's stays)
        const int count = f.stack.count();
        QVERIFY(f.editor.loadPresetInto(track, fx, preset));
        QCOMPARE(f.stack.count(), count);

        const QString builtin = f.editor.addDevice(track, "utility");
        Device built = p.device(track, builtin);
        built.params.insert("gain", -3.0);
        built.state = "c2FtcGxl";
        const int steps = f.stack.count();
        QVERIFY(f.editor.loadPresetInto(track, builtin, presetOf(built)));
        QCOMPARE(f.stack.count(), steps + 1);  // (the parameters and the state: one step)
        QCOMPARE(p.device(track, builtin).params.value("gain"), -3.0);
        QCOMPARE(p.device(track, builtin).state, std::optional<QString>("c2FtcGxl"));
        f.stack.undo();
        QVERIFY(!p.device(track, builtin).state);
        QCOMPARE(p.device(track, builtin).params.value("gain"), 0.0);
        f.editor.setDeviceState(track, builtin, std::nullopt, QStringLiteral("c3RhdGU="), "Load Sample");
        QCOMPARE(p.device(track, builtin).state, std::optional<QString>("c3RhdGU="));
        QCOMPARE(f.stack.undoText(), QStringLiteral("Load Sample"));
    }

    // --- Default presets ---

    void newDevicesStartAsTheirDefaultPreset() {
        EditorFixture f;
        Project& p = f.project;
        test::TempDir dir;
        const QString root = dir.path();
        withDefaults(f, root);
        const QString track = f.editor.addAudioTrack();
        const QString factory = f.editor.addDevice(track, "utility");
        f.editor.setDeviceParam(track, factory, "gain", -4.0);
        const QString path = saveDefault(p.device(track, factory), root);
        QCOMPARE(path, root + "/" + kDefaultsFolder + "/Utility" + kPresetExtension);
        QVERIFY(hasDefault("utility", std::nullopt, root));
        QVERIFY(listPresets(root).empty());  // (the browser doesn't list defaults)

        const QString first = f.editor.addDevice(track, "utility");
        const QString second = f.editor.addDevice(track, "utility");
        QCOMPARE(p.device(track, first).params, p.device(track, factory).params);
        QCOMPARE(p.device(track, second).params, p.device(track, factory).params);
        QVERIFY(first != second && first != factory && second != factory);
        QCOMPARE(p.device(track, f.editor.addDevice(track, "compressor")).params, newDevice("compressor").params);  // (no default)
        // A new MIDI track's instrument starts as its default too.
        const QString keys = f.editor.addMidiTrack();
        f.editor.setDeviceParam(keys, f.track(keys).devices[0].id, "volume", -9.0);
        saveDefault(f.track(keys).devices[0], root);
        QCOMPARE(f.track(f.editor.addMidiTrack()).devices[0].params.value("volume"), -9.0);

        QVERIFY(clearDefault("utility", std::nullopt, root));
        QVERIFY(!clearDefault("utility", std::nullopt, root));
        QCOMPARE(p.device(track, f.editor.addDevice(track, "utility")).params, newDevice("utility").params);
    }

    void aDefaultPresetIsPerPlugInAndKeepsWhereThePlugInIs() {
        EditorFixture f;
        Project& p = f.project;
        test::TempDir dir;
        withDefaults(f, dir.path());
        const PluginRef a{"VST3", "AAAA", "Same Name", "", "C:/old/A.vst3"};
        const PluginRef b{"VST3", "BBBB", "Same Name"};
        Device saved = newDevice(kPluginKind, a);
        saved.state = "c3RhdGU=";
        saveDefault(saved, dir.path());
        const QString track = f.editor.addAudioTrack();
        const PluginRef moved{"VST3", "AAAA", "Same Name", "", "D:/new/A.vst3"};
        QSignalSpy added(&f.editor, &ProjectEditor::pluginAdded);
        const QString device = f.editor.addDevice(track, kPluginKind, -1, moved);
        QCOMPARE(p.device(track, device).state, std::optional<QString>("c3RhdGU="));
        QVERIFY(p.device(track, device).plugin == moved);
        QVERIFY(!p.device(track, f.editor.addDevice(track, kPluginKind, -1, b)).state);
        QCOMPARE(added.size(), 2);  // (their editors show)
        QCOMPARE(added[0][1].toString(), device);
    }

    void anUnreadableDefaultPresetIsIgnored() {
        EditorFixture f;
        Project& p = f.project;
        test::TempDir dir;
        withDefaults(f, dir.path());
        const QString path = *defaultPath("utility", std::nullopt, dir.path());
        QDir().mkpath(QFileInfo(path).path());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not a preset");
        file.close();
        const QString track = f.editor.addAudioTrack();
        QCOMPARE(p.device(track, f.editor.addDevice(track, "utility")).params, newDevice("utility").params);
        saveDefault(test::makeDevice("c", "compressor"), dir.path());
        QFile::remove(path);
        QFile::rename(*defaultPath("compressor", std::nullopt, dir.path()), path);  // a compressor where the utility's goes
        QCOMPARE(p.device(track, f.editor.addDevice(track, "utility")).params, newDevice("utility").params);
    }

    void aMidiTrackWithAnInstrumentPreset() {
        EditorFixture f;
        const Device synth = newDevice("synth");
        const int steps = f.stack.count();
        const QString track = f.editor.addMidiTrackWith(synth);
        QCOMPARE(f.stack.count(), steps + 1);  // one step
        QCOMPARE(f.stack.undoText(), QStringLiteral("Insert MIDI Track"));
        QCOMPARE(f.track(track).devices.size(), size_t(1));
        QCOMPARE(f.track(track).devices[0].id, synth.id);
        f.stack.undo();
        QVERIFY(f.project.tracks().empty());
    }

    // --- Rack names ---

    void aRackIsNamedAsThePresetItComesFrom() {
        EditorFixture f;
        Project& p = f.project;
        test::TempDir dir;
        const QString track = f.editor.addAudioTrack();
        const QString rack = f.editor.groupDevices(track, {f.editor.addDevice(track, "utility")});
        QCOMPARE(deviceName(p.device(track, rack)), QStringLiteral("Audio Effect Rack"));
        QString path = saveToLibrary(p.device(track, rack), "Glue", dir.path());
        path = renamePreset(path, "Bus Glue");
        // Loaded into a rack, the rack takes the name; saved as a preset, the name is kept in the file and projects.
        QVERIFY(f.editor.loadPresetInto(track, rack, loadPreset(path)));
        QCOMPARE(p.device(track, rack).name, std::optional<QString>("Bus Glue"));
        f.stack.undo();
        QVERIFY(!p.device(track, rack).name);
        f.editor.renameRack(track, rack, "Mine");
        QCOMPARE(deviceName(p.device(track, rack)), QStringLiteral("Mine"));
        QCOMPARE(QFileInfo(saveToLibrary(p.device(track, rack), "Other", dir.path())).dir().dirName(),
                 QStringLiteral("Audio Effect Rack"));
        Project copy;
        loadInto(copy, projectToJson(p));
        QCOMPARE(copy.device(track, rack).name, std::optional<QString>("Mine"));
        f.editor.renameRack(track, rack, {});
        QCOMPARE(deviceName(p.device(track, rack)), QStringLiteral("Audio Effect Rack"));
    }
};

QTEST_GUILESS_MAIN(TestEditorPresets)
#include "test_editor_presets.moc"
