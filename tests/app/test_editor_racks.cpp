// Racks through the editor: grouping devices (Ctrl+G) and ungrouping, chains
// and their mixers, devices moving into and out of racks, nesting limits,
// instrument racks, macros (how many, their names, their ranges), sidechains
// into devices in racks and taken after them, saving, presets loading as new
// devices, and showing a rack's chain list and its devices.

#include "EditorFixture.h"
#include "TestSupport.h"

#include "io/Serialization.h"
#include "model/Devices.h"
#include "model/Errors.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

#include <cmath>

using namespace sub::app;
using test::EditorFixture;
using test::env;
namespace autom = sub::app::automation;

namespace {

QStringList utilities(EditorFixture& f, const QString& trackId, int count) {
    QStringList ids;
    for (int i = 0; i < count; ++i) ids.append(f.editor.addDevice(trackId, "utility"));
    return ids;
}

QStringList ids(const std::vector<Device>& devices) {
    QStringList result;
    for (const Device& d : devices) result.append(d.id);
    return result;
}

QStringList ids(const std::vector<Chain>& chains) {
    QStringList result;
    for (const Chain& c : chains) result.append(c.id);
    return result;
}

bool near(double a, double b, double tolerance = 1e-6) { return std::abs(a - b) <= tolerance; }

}  // namespace

class TestEditorRacks : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void groupingDevicesIntoARackAndBack() {
        EditorFixture f;
        Project& p = f.project;
        const QString track = f.editor.addAudioTrack();
        const QStringList abc = utilities(f, track, 3);
        const QString a = abc[0], b = abc[1], c = abc[2];
        f.editor.setEnvelope(track, autom::deviceKey(b, "gain"), env({{0.0, 0.5}}));
        const int steps = f.stack.count();
        const QString rackId = f.editor.groupDevices(track, {c, b});  // in chain order, where the first was
        QCOMPARE(f.stack.count(), steps + 1);
        QCOMPARE(f.stack.undoText(), QStringLiteral("Group Devices"));
        const auto& devices = f.track(track).devices;
        QCOMPARE(ids(devices), (QStringList{a, rackId}));
        const Device& rack = p.device(track, rackId);
        QVERIFY(rack.isRack() && rack.kind == kRackKind);
        QCOMPARE(rack.chains.size(), size_t(1));
        QCOMPARE(ids(rack.chains[0].devices), (QStringList{b, c}));
        QCOMPARE(rack.chains[0].name, QStringLiteral("Utility"));
        QCOMPARE(deviceName(rack), QStringLiteral("Audio Effect Rack"));
        QMap<QString, double> macros;  // four, at 0
        for (int i = 0; i < kDefaultMacroCount; ++i) macros.insert(macroParam(i), 0.0);
        QCOMPARE(rack.params, macros);
        QCOMPARE(macroCount(rack), 4);
        QCOMPARE(macroName(rack, 0), QStringLiteral("Macro 1"));
        // Nested devices are the track's: found wherever they are, their automation still theirs.
        QCOMPARE(&p.device(track, c), &rack.chains[0].devices[1]);
        QCOMPARE(devicePath(devices, c), std::optional<std::vector<int>>(std::vector<int>{1, 0, 1}));
        QCOMPARE(containerOf(devices, c), std::optional<QString>(rack.chains[0].id));
        QVERIFY(!containerOf(devices, a));
        QCOMPARE(p.deviceOwner(c), std::optional<QString>(track));
        QVERIFY(!p.envelope(track, autom::deviceKey(b, "gain")).empty());
        QVERIFY(f.editor.groupDevices(track, {a, b}).isEmpty());  // not in one chain
        f.stack.undo();
        QCOMPARE(ids(f.track(track).devices), (QStringList{a, b, c}));
        f.stack.redo();
        QVERIFY(f.editor.ungroupRack(track, rackId));
        QCOMPARE(ids(f.track(track).devices), (QStringList{a, b, c}));
        QCOMPARE(f.stack.undoText(), QStringLiteral("Ungroup Rack"));
        QVERIFY(!f.editor.ungroupRack(track, a));
        f.stack.undo();
        QCOMPARE(ids(f.track(track).devices), (QStringList{a, rackId}));
    }

    void chainsAndTheirMixers() {
        EditorFixture f;
        Project& p = f.project;
        const QString track = f.editor.addAudioTrack();
        const QString a = utilities(f, track, 1)[0];
        const QString rack = f.editor.groupDevices(track, {a});
        const QString first = p.device(track, rack).chains[0].id;
        const QString second = f.editor.addRackChain(track, rack);
        QCOMPARE(p.chain(track, second).name, QStringLiteral("Chain 2"));
        QCOMPARE(ids(p.device(track, rack).chains), (QStringList{first, second}));
        const QString inner = f.editor.addDevice(track, "utility", -1, second);
        QCOMPARE(ids(p.chain(track, second).devices), QStringList{inner});
        QCOMPARE(p.chainRack(track, second).id, rack);
        f.editor.renameChain(track, second, "Wet");
        f.editor.setChainParam(track, second, ChainField::VolumeDb, -6.0);
        f.editor.setChainParam(track, second, ChainField::Pan, 0.5);
        f.editor.setChainParam(track, second, ChainField::Mute, true);
        f.editor.setChainParam(track, second, ChainField::Solo, true);
        const Chain& wet = p.chain(track, second);
        QVERIFY(wet.volumeDb == -6.0 && wet.pan == 0.5 && wet.mute && wet.solo);
        QCOMPARE(wet.name, QStringLiteral("Wet"));
        f.editor.setChainParam(track, second, ChainField::VolumeDb, 100.0);  // clamped like a fader
        QCOMPARE(p.chain(track, second).volumeDb, autom::kMaxVolumeDb);
        for (int i = 0; i < 5; ++i) f.stack.undo();  // (solo isn't undone: a listening aid)
        QCOMPARE(p.chain(track, second).volumeDb, 0.0);
        QCOMPARE(p.chain(track, second).name, QStringLiteral("Chain 2"));
        QVERIFY(p.chain(track, second).solo);
        f.editor.setChainParam(track, second, "solo", 0.0);  // (by its name, as QML has it)
        QVERIFY(!p.chain(track, second).solo);
        const QString copy = f.editor.duplicateRackChain(track, second);  // new devices, the same settings
        QVERIFY(copy != second);
        QCOMPARE(p.chain(track, copy).devices.size(), size_t(1));
        QVERIFY(p.chain(track, copy).devices[0].id != inner);
        f.editor.moveRackChain(track, copy, 0);
        QCOMPARE(ids(p.device(track, rack).chains), (QStringList{copy, first, second}));
        // A chain going takes its devices' automation, and its fader's, along.
        f.editor.setEnvelope(track, autom::deviceKey(inner, "gain"), env({{0.0, 0.5}}));
        const QString fader = autom::chainKey(rack, second, autom::kChainVolume);
        f.editor.setEnvelope(track, fader, env({{0.0, 0.5}}));
        QCOMPARE(autom::keyChain(fader), std::optional<QString>(second));
        QCOMPARE(autom::keyDevice(fader), std::optional<QString>(rack));
        f.editor.removeRackChains(track, {second});
        QCOMPARE(ids(p.device(track, rack).chains), (QStringList{copy, first}));
        QVERIFY(p.automation(track).isEmpty());
        f.stack.undo();
        QCOMPARE(p.automation(track).size(), 2);
        QCOMPARE(p.chain(track, second).devices[0].id, inner);
        // Only racks have chains.
        QVERIFY_THROWS_EXCEPTION(EditError, f.editor.addRackChain(track, inner));
        QVERIFY(f.editor.tryAddRackChain(track, inner).isEmpty());
        QCOMPARE(f.messages.size(), 1);
    }

    void touchingAChainFaderShowsItsLane() {
        EditorFixture f;
        const QString track = f.editor.addAudioTrack();
        const QString rack = f.editor.groupDevices(track, utilities(f, track, 1));
        const QString chain = f.project.device(track, rack).chains[0].id;
        QSignalSpy touched(&f.editor, &ProjectEditor::parameterTouched);
        f.editor.setChainParam(track, chain, ChainField::Pan, -0.5, "drag");
        f.editor.setChainParam(track, chain, ChainField::Pan, -0.75, "drag");
        f.editor.setChainParam(track, chain, ChainField::Mute, true);
        QCOMPARE(touched.size(), 2);
        QCOMPARE(touched[0][1].toString(), autom::chainKey(rack, chain, autom::kChainPan));
        f.stack.undo();
        f.stack.undo();  // one drag: one step
        QCOMPARE(f.project.chain(track, chain).pan, 0.0);
    }

    void movingDevicesIntoAndOutOfRacks() {
        EditorFixture f;
        Project& p = f.project;
        const QString track = f.editor.addAudioTrack();
        const QStringList abc = utilities(f, track, 3);
        const QString a = abc[0], b = abc[1], c = abc[2];
        const QString rack = f.editor.groupDevices(track, {c});
        const QString chain = p.device(track, rack).chains[0].id;
        QVERIFY(f.editor.moveDevices(track, {a, b}, 0, chain));  // in, before what is there
        QCOMPARE(ids(f.track(track).devices), QStringList{rack});
        QCOMPARE(ids(p.chain(track, chain).devices), (QStringList{a, b, c}));
        QVERIFY(f.editor.moveDevices(track, {b}, 5));  // out again, last
        QCOMPARE(ids(f.track(track).devices), (QStringList{rack, b}));
        QVERIFY(!f.editor.moveDevices(track, {rack}, 0, chain));  // not into itself
        const QString inner = f.editor.groupDevices(track, {a});  // a rack in the rack
        QVERIFY(!f.editor.moveDevices(track, {rack}, 0, p.device(track, inner).chains[0].id));
        f.editor.moveDevice(track, a, 0);  // (alone in its chain: nothing)
        // Another track's rack chain, with the automation going along.
        const QString other = f.editor.addAudioTrack();
        const QString otherRack = f.editor.groupDevices(other, {f.editor.addDevice(other, "utility")});
        const QString otherChain = p.device(other, otherRack).chains[0].id;
        f.editor.setEnvelope(track, autom::deviceKey(a, "gain"), env({{0.0, 0.5}}));
        QVERIFY(f.editor.moveDevicesToTrack(track, {inner}, other, 0, otherChain));
        QCOMPARE(ids(p.chain(other, otherChain).devices).front(), inner);
        QVERIFY(p.hasDevice(other, a) && !p.hasDevice(track, a));
        QVERIFY(!p.envelope(other, autom::deviceKey(a, "gain")).empty());
        QVERIFY(p.automation(track).isEmpty());
        f.stack.undo();  // (one step: the devices and their automation)
        QVERIFY(p.hasDevice(track, a) && !p.envelope(track, autom::deviceKey(a, "gain")).empty());
        QVERIFY(p.automation(other).isEmpty());
    }

    void racksNestAtMostMaxRackDepthDeep() {
        EditorFixture f;
        const QString track = f.editor.addAudioTrack();
        const QString device = utilities(f, track, 1)[0];
        QStringList racks;
        for (int i = 0; i < kMaxRackDepth; ++i) racks.append(f.editor.groupDevices(track, {racks.isEmpty() ? device : racks.back()}));
        for (const QString& rack : racks) QVERIFY(!rack.isEmpty());
        QVERIFY(f.editor.groupDevices(track, {racks.back()}).isEmpty());
        const QString deepest = f.project.device(track, racks.front()).chains[0].id;  // the innermost rack's chain
        QVERIFY(f.editor.addDevice(track, kRackKind, -1, deepest).isEmpty());
        QVERIFY(!f.editor.addDevice(track, "utility", -1, deepest).isEmpty());
    }

    void instrumentRacks() {
        EditorFixture f;
        Project& p = f.project;
        const QString keys = f.editor.addMidiTrack();
        const QString synth = f.track(keys).devices[0].id;
        const QString rack = f.editor.groupDevices(keys, {synth});
        QVERIFY(deviceIsInstrument(p.device(keys, rack)));
        QCOMPARE(deviceName(p.device(keys, rack)), QStringLiteral("Instrument Rack"));
        const QString second = f.editor.addRackChain(keys, rack);
        const QString layered = f.editor.addDevice(keys, "synth", -1, second);  // an instrument in each chain
        QVERIFY(!layered.isEmpty());
        QCOMPARE(ids(p.chain(keys, second).devices), QStringList{layered});
        const QString fx = f.editor.addDevice(keys, "utility", 0);  // effects go after the instrument rack
        QCOMPARE(ids(f.track(keys).devices), (QStringList{rack, fx}));
        QVERIFY(f.editor.addDevice(f.editor.addAudioTrack(), "synth").isEmpty());  // not on an audio track, as ever
    }

    void macrosMoveTheParametersMappedToThem() {
        EditorFixture f;
        Project& p = f.project;
        const QString track = f.editor.addAudioTrack();
        const QStringList ab = utilities(f, track, 2);
        const QString a = ab[0], b = ab[1];
        const QString rack = f.editor.groupDevices(track, {a, b});
        f.editor.mapMacro(track, rack, 0, a, "gain");  // -60..24 dB over the macro
        f.editor.mapMacro(track, rack, 0, b, "width", 1.0, 0.5);  // 200..100 %, the other way round
        QCOMPARE(f.editor.macroOf(track, a, "gain"), (std::optional<std::pair<QString, int>>({rack, 0})));
        QVERIFY(!f.editor.macroOf(track, a, "pan"));
        // Not in the rack.
        QVERIFY_THROWS_EXCEPTION(EditError, f.editor.mapMacro(track, rack, 1, f.editor.addDevice(track, "utility"), "gain"));
        f.editor.setMacro(track, rack, 0, 0.5, "drag");
        f.editor.setMacro(track, rack, 0, 0.75, "drag");  // one gesture: one step
        QCOMPARE(f.stack.undoText(), QStringLiteral("Change Macro"));
        QCOMPARE(p.device(track, rack).params.value(macroParam(0)), 0.75);
        QVERIFY(near(p.device(track, a).params.value("gain"), -60 + 0.75 * 84));
        QVERIFY(near(p.device(track, b).params.value("width"), 200 - 0.75 * 100));
        f.stack.undo();
        QCOMPARE(p.device(track, rack).params.value(macroParam(0)), 0.0);
        QCOMPARE(p.device(track, a).params.value("gain"), 0.0);
        QCOMPARE(p.device(track, b).params.value("width"), 100.0);
        // A mapped device going takes its mapping along (in the same step).
        f.editor.removeDevice(track, a);
        QVERIFY((p.device(track, rack).macros == std::vector<MacroMapping>{{0, b, "width", 1.0, 0.5}}));
        f.stack.undo();
        QCOMPARE(p.device(track, rack).macros.size(), size_t(2));
        f.editor.unmapMacro(track, rack, b, "width");
        QVERIFY((p.device(track, rack).macros == std::vector<MacroMapping>{{0, a, "gain", 0.0, 1.0}}));
        QVERIFY(!f.editor.tryMapMacro(track, rack, 9, a, "gain"));  // (from QML: no such macro)
        QVERIFY(f.editor.tryMapMacro(track, rack, 1, b, "pan"));
        QCOMPARE(p.device(track, rack).macros.size(), size_t(2));
    }

    void addingAndTakingAwayMacros() {
        EditorFixture f;
        Project& p = f.project;
        const QString track = f.editor.addAudioTrack();
        const QStringList ab = utilities(f, track, 2);
        const QString a = ab[0], b = ab[1];
        const QString rack = f.editor.groupDevices(track, {a, b});
        f.editor.setMacroCount(track, rack, 6);
        QCOMPARE(f.stack.undoText(), QStringLiteral("Add Macro"));
        QCOMPARE(macroCount(p.device(track, rack)), 6);
        QCOMPARE(p.device(track, rack).params.value(macroParam(5), -1.0), 0.0);
        f.editor.mapMacro(track, rack, 5, a, "gain");
        f.editor.mapMacro(track, rack, 1, b, "gain");
        f.editor.setMacro(track, rack, 5, 0.5);
        f.editor.setEnvelope(track, autom::deviceKey(rack, macroParam(5)), env({{0.0, 0.25}}));
        f.editor.setEnvelope(track, autom::deviceKey(rack, macroParam(1)), env({{0.0, 0.75}}));
        const int steps = f.stack.count();
        // The last ones go, with their values, mappings and automation: one step.
        f.editor.setMacroCount(track, rack, 3);
        QCOMPARE(f.stack.count(), steps + 1);
        QCOMPARE(f.stack.undoText(), QStringLiteral("Remove Macro"));
        const Device& fewer = p.device(track, rack);
        QCOMPARE(macroCount(fewer), 3);
        QVERIFY(!fewer.params.contains(macroParam(5)) && !fewer.params.contains(macroParam(3)));
        QVERIFY((fewer.macros == std::vector<MacroMapping>{{1, b, "gain", 0.0, 1.0}}));
        QVERIFY(p.envelope(track, autom::deviceKey(rack, macroParam(5))).empty());
        QVERIFY(!p.envelope(track, autom::deviceKey(rack, macroParam(1))).empty());
        QVERIFY(!f.editor.tryMapMacro(track, rack, 4, a, "gain"));  // (no such macro now)
        f.stack.undo();
        QCOMPARE(macroCount(p.device(track, rack)), 6);
        QCOMPARE(p.device(track, rack).params.value(macroParam(5)), 0.5);
        QCOMPARE(p.device(track, rack).macros.size(), size_t(2));
        QVERIFY(!p.envelope(track, autom::deviceKey(rack, macroParam(5))).empty());
        // One to sixteen.
        f.editor.setMacroCount(track, rack, 0);
        QCOMPARE(macroCount(p.device(track, rack)), 1);
        f.editor.setMacroCount(track, rack, 40);
        QCOMPARE(macroCount(p.device(track, rack)), kMaxMacroCount);
        f.editor.setMacroCount(track, a, 2);  // (not a rack: nothing)
        QCOMPARE(macroCount(p.device(track, a)), 0);
    }

    void renamingMacros() {
        EditorFixture f;
        Project& p = f.project;
        const QString track = f.editor.addAudioTrack();
        const QString rack = f.editor.groupDevices(track, utilities(f, track, 1));
        f.editor.renameMacro(track, rack, 1, "  Drive ");
        QCOMPARE(f.stack.undoText(), QStringLiteral("Rename Macro"));
        QCOMPARE(macroName(p.device(track, rack), 1), QStringLiteral("Drive"));
        QCOMPARE(macroName(p.device(track, rack), 0), QStringLiteral("Macro 1"));
        const int steps = f.stack.count();
        f.editor.renameMacro(track, rack, 1, "Drive");  // (the same: no step)
        f.editor.renameMacro(track, rack, 7, "Nothing");  // (no such macro)
        QCOMPARE(f.stack.count(), steps);
        f.editor.renameMacro(track, rack, 1, "Macro 2");  // by its number again
        QCOMPARE(p.device(track, rack).macroNames[1], QString());
        f.stack.undo();
        QCOMPARE(macroName(p.device(track, rack), 1), QStringLiteral("Drive"));
        f.editor.renameMacro(track, rack, 1, "");
        QCOMPARE(macroName(p.device(track, rack), 1), QStringLiteral("Macro 2"));
        // Its name goes with the rack: in a preset (and a copy).
        f.editor.renameMacro(track, rack, 2, "Space");
        const std::vector<Device> copied = f.editor.copyDevices(track, {rack});
        QCOMPARE(macroName(copied.front(), 2), QStringLiteral("Space"));
    }

    void macroRanges() {
        // A mapped parameter moves over part of its range, or the other way round,
        // and goes where its macro puts it in the new one.
        EditorFixture f;
        Project& p = f.project;
        const QString track = f.editor.addAudioTrack();
        const QStringList ab = utilities(f, track, 2);
        const QString a = ab[0], b = ab[1];
        const QString rack = f.editor.groupDevices(track, {a, b});
        f.editor.mapMacro(track, rack, 0, a, "width");  // 0..200 %
        f.editor.setMacro(track, rack, 0, 1.0);
        QVERIFY(near(p.device(track, a).params.value("width"), 200.0));
        const int steps = f.stack.count();
        f.editor.setMacroRange(track, rack, a, "width", 0.0, 0.75, "drag");
        f.editor.setMacroRange(track, rack, a, "width", 0.0, 0.5, "drag");  // one gesture: one step
        QCOMPARE(f.stack.count(), steps + 1);
        QCOMPARE(f.stack.undoText(), QStringLiteral("Change Macro Range"));
        QVERIFY((p.device(track, rack).macros == std::vector<MacroMapping>{{0, a, "width", 0.0, 0.5}}));
        QVERIFY(near(p.device(track, a).params.value("width"), 100.0));  // where the macro puts it now
        f.editor.setMacroRange(track, rack, a, "width", 1.0, 0.0);  // 100 to 0: the other way round
        QVERIFY(near(p.device(track, a).params.value("width"), 0.0));
        f.editor.setMacro(track, rack, 0, 0.25);
        QVERIFY(near(p.device(track, a).params.value("width"), 150.0));
        f.editor.setMacroRange(track, rack, a, "width", 0.5, 1.0);  // 50 to 100
        QVERIFY(near(p.device(track, a).params.value("width"), 125.0));
        f.editor.setMacroRange(track, rack, a, "width", -1.0, 3.0);  // (held to the parameter's range)
        QVERIFY((p.device(track, rack).macros == std::vector<MacroMapping>{{0, a, "width", 0.0, 1.0}}));
        f.stack.undo();
        f.stack.undo();
        f.stack.undo();
        f.stack.undo();
        f.stack.undo();
        QVERIFY((p.device(track, rack).macros == std::vector<MacroMapping>{{0, a, "width", 0.0, 1.0}}));
        QVERIFY(near(p.device(track, a).params.value("width"), 200.0));
        const int before = f.stack.count();
        f.editor.setMacroRange(track, rack, b, "width", 0.0, 0.5);  // (not mapped: nothing)
        QCOMPARE(f.stack.count(), before);
    }

    void macrosOfPlugInsAskTheHooks() {
        // A plug-in's parameters: their mapping and their values come from the
        // engine bridge (setParamInfo, setOwnValue).
        EditorFixture f;
        Project& p = f.project;
        const QString track = f.editor.addAudioTrack();
        const PluginRef effect{"VST3", "B", "Effect B"};
        const QString fx = f.editor.addDevice(track, kPluginKind, -1, effect);
        const QString rack = f.editor.groupDevices(track, {fx});
        f.editor.mapMacro(track, rack, 0, fx, "7");
        f.editor.mapMacro(track, rack, 0, fx, "8");
        f.editor.setMacro(track, rack, 0, 0.5);
        QVERIFY(p.device(track, fx).params.isEmpty());  // (nothing knows its parameters yet)
        f.stack.undo();
        ParamSpec gain;
        gain.minimum = 0.0;
        gain.maximum = 2.0;
        gain.defaultValue = 1.0;
        f.editor.setParamInfo([&](const QString& t, const QString& d, const QString&) -> std::optional<ParamSpec> {
            if (t == track && d == fx) return gain;
            return std::nullopt;
        });
        f.editor.setOwnValue([&](const QString& owner, const QString& key) -> std::optional<double> {
            if (owner == track && key == autom::deviceKey(fx, "8")) return 1.5;  // set in its own editor
            return std::nullopt;
        });
        f.editor.setMacro(track, rack, 0, 0.25);
        QVERIFY(near(p.device(track, fx).params.value("7"), 0.5));
        QVERIFY(near(p.device(track, fx).params.value("8"), 0.5));
        f.stack.undo();
        QCOMPARE(p.device(track, fx).params.value("7"), 1.0);  // as it was: its default
        QCOMPARE(p.device(track, fx).params.value("8"), 1.5);  // as it was: as its own editor set it
        QVERIFY(near(f.editor.macroTargets(track, rack, 0, 1.0).value({fx, "7"}), 2.0));
        QVERIFY(f.editor.paramInfo(track, fx, "7") == gain);
    }

    void sidechainsIntoDevicesInRacksAreRoutingEdges() {
        EditorFixture f;
        Project& p = f.project;
        const QString kick = f.editor.addAudioTrack();
        const QString bass = f.editor.addAudioTrack();
        const QString comp = f.editor.addDevice(bass, "utility");
        const QString rack = f.editor.groupDevices(bass, {comp});
        f.editor.setDeviceSidechain(bass, comp, Sidechain{kick});
        QVERIFY(p.wouldCycle(kick, kick) && p.inputWouldCycle(kick, bass));  // bass is fed by kick
        QVERIFY_THROWS_EXCEPTION(EditError, f.editor.setTrackInputTrack(kick, bass));
        // Grouping the source with the device's track drops a sidechain that would close a cycle.
        const QString group = f.editor.groupTracks({kick});
        f.editor.setDeviceSidechain(bass, comp, Sidechain{group});
        f.editor.moveTracks({bass}, p.trackIndex(kick) + 1, group);
        QVERIFY(!p.device(bass, comp).sidechain);
        f.stack.undo();
        QVERIFY(p.device(bass, comp).sidechain == Sidechain{group});
        f.editor.deleteTracks({group});  // the source going takes it along
        QVERIFY(!p.device(bass, comp).sidechain && p.hasDevice(bass, rack));
    }

    void sidechainsTakenAfterADeviceInARack() {
        EditorFixture f;
        Project& p = f.project;
        const QString kick = f.editor.addAudioTrack();
        const QString bass = f.editor.addAudioTrack();
        const QStringList ab = utilities(f, kick, 2);
        const QString rack = f.editor.groupDevices(kick, {ab[0], ab[1]});
        const QString comp = f.editor.addDevice(bass, "utility");
        f.editor.setDeviceSidechain(bass, comp, Sidechain{kick, ab[1]});  // after a device in the kick's rack
        QCOMPARE(p.device(bass, comp).sidechain->tapDevice(), std::optional<QString>(ab[1]));
        QVERIFY_THROWS_EXCEPTION(EditError, f.editor.setDeviceSidechain(bass, comp, Sidechain{kick, comp}));
        f.editor.setDeviceSidechain(bass, comp, Sidechain{kick, rack});  // after the rack
        QCOMPARE(p.device(bass, comp).sidechain->tap, rack);
    }

    void showingAChainListAndARacksDevices() {
        // View state: saved, not undone.
        EditorFixture f;
        Project& p = f.project;
        const QString track = f.editor.addAudioTrack();
        const QString rack = f.editor.groupDevices(track, utilities(f, track, 1));
        QVERIFY(!p.isChainListShown(rack) && p.areRackDevicesShown(rack));  // by default
        QSignalSpy viewChanged(&p, &Project::rackViewChanged);
        const int steps = f.stack.count();
        f.editor.setChainListShown(track, rack, true);
        f.editor.setRackDevicesShown(track, rack, false);
        QVERIFY(p.isChainListShown(rack) && !p.areRackDevicesShown(rack));
        QCOMPARE(viewChanged.count(), 2);
        QCOMPARE(viewChanged.first().first().toString(), track);
        QCOMPARE(f.stack.count(), steps);
        f.editor.setChainListShown(track, rack, true);  // (as it is: no signal)
        QCOMPARE(viewChanged.count(), 2);
        f.editor.setChainListShown(track, utilities(f, track, 1)[0], true);  // (not a rack)
        QCOMPARE(viewChanged.count(), 2);
    }

    void racksAreSavedAndLoaded() {
        EditorFixture f;
        Project& p = f.project;
        const QString track = f.editor.addAudioTrack();
        const QStringList ab = utilities(f, track, 2);
        const QString a = ab[0], b = ab[1];
        const QString rack = f.editor.groupDevices(track, {a, b});
        const QString wet = f.editor.addRackChain(track, rack, -1, "Wet");
        f.editor.moveDevices(track, {b}, 0, wet);
        f.editor.setChainParam(track, wet, ChainField::VolumeDb, -3.0);
        f.editor.setChainParam(track, wet, ChainField::Solo, true);
        f.editor.mapMacro(track, rack, 2, b, "pan", 0.25, 0.75);
        f.editor.setEnvelope(track, autom::chainKey(rack, wet, autom::kChainPan), env({{1.0, 0.25}}));
        const QString source = f.editor.addAudioTrack();
        f.editor.setDeviceSidechain(track, a, Sidechain{source});
        const QJsonObject data = projectToJson(p);
        Project loaded;
        loadInto(loaded, data);
        QCOMPARE(projectToJson(loaded), data);
        const Device& again = loaded.device(track, rack);
        QCOMPARE(again.chains[0].name, p.device(track, rack).chains[0].name);
        QCOMPARE(again.chains[1].name, QStringLiteral("Wet"));
        QVERIFY(again.chains[1].volumeDb == -3.0 && again.chains[1].solo);
        QVERIFY((again.macros == std::vector<MacroMapping>{{2, b, "pan", 0.25, 0.75}}));
        QVERIFY(loaded.device(track, a).sidechain == Sidechain{source});
    }

    void presetsLoadAsNewDevices() {
        EditorFixture f;
        Project& p = f.project;
        test::TempDir dir;
        const QString track = f.editor.addAudioTrack();
        const QStringList ab = utilities(f, track, 2);
        const QString rack = f.editor.groupDevices(track, ab);
        f.editor.mapMacro(track, rack, 0, ab[1], "gain");
        f.editor.setDeviceSidechain(track, ab[0], Sidechain{f.editor.addAudioTrack()});
        const QString path = dir.path("Rack.gilpreset");
        savePreset(p.device(track, rack), path);
        const Device first = loadPreset(path);
        const Device second = loadPreset(path);
        const auto allIds = [](const Device& device) {
            std::vector<Device> holder{device};
            QSet<QString> result;
            for (const Device* d : iterDevices(holder)) result.insert(d->id);
            for (const Chain& c : device.chains) result.insert(c.id);
            return result;
        };
        QVERIFY(!allIds(first).intersects(allIds(second)));  // each load is new
        QVERIFY(!allIds(first).intersects(allIds(p.device(track, rack))));
        QCOMPARE(first.chains[0].devices.size(), size_t(2));
        QCOMPARE(second.chains[0].devices.size(), size_t(2));
        QCOMPARE(first.macros[0].deviceId, first.chains[0].devices[1].id);  // mappings follow their devices
        std::vector<Device> holder{first};
        for (const Device* d : iterDevices(holder)) QVERIFY(!d->sidechain);  // sidechains name the project's tracks
        QVERIFY(f.editor.insertDevice(track, first));
        QVERIFY(f.editor.insertDevice(track, second));
        QCOMPARE(f.track(track).devices.size(), size_t(3));
    }

    void ungroupingLayeredInstrumentsIsRefused() {
        EditorFixture f;
        Project& p = f.project;
        const QString keys = f.editor.addMidiTrack();
        const QString rack = f.editor.groupDevices(keys, {f.track(keys).devices[0].id});
        const QString second = f.editor.addRackChain(keys, rack);
        f.editor.addDevice(keys, "synth", -1, second);  // two instruments: a chain has one
        const int steps = f.stack.count();
        QVERIFY(!f.editor.ungroupRack(keys, rack));
        QCOMPARE(f.stack.count(), steps);
        QVERIFY(p.device(keys, rack).isRack());
    }

    void aDeviceLeavingItsRackLeavesItsMacroMapping() {
        EditorFixture f;
        Project& p = f.project;
        const QString track = f.editor.addAudioTrack();
        const QString other = f.editor.addAudioTrack();
        const QStringList abc = utilities(f, track, 3);
        const QString a = abc[0], b = abc[1];
        const QString rack = f.editor.groupDevices(track, {a, b});
        f.editor.mapMacro(track, rack, 0, a, "gain");
        f.editor.mapMacro(track, rack, 1, b, "gain");
        QVERIFY(f.editor.moveDevices(track, {a}, 0));  // out of the rack, onto the track's own chain
        QVERIFY((p.device(track, rack).macros == std::vector<MacroMapping>{{1, b, "gain", 0.0, 1.0}}));
        f.stack.undo();
        QCOMPARE(p.device(track, rack).macros.size(), size_t(2));
        QVERIFY(f.editor.moveDevicesToTrack(track, {b}, other));  // to another track
        QVERIFY((p.device(track, rack).macros == std::vector<MacroMapping>{{0, a, "gain", 0.0, 1.0}}));
    }

    void renamingARack() {
        EditorFixture f;
        Project& p = f.project;
        const QString track = f.editor.addAudioTrack();
        const QString rack = f.editor.groupDevices(track, utilities(f, track, 1));
        f.editor.renameRack(track, rack, "Mine");
        QCOMPARE(deviceName(p.device(track, rack)), QStringLiteral("Mine"));
        QCOMPARE(f.stack.undoText(), QStringLiteral("Rename Rack"));
        f.editor.renameRack(track, rack, {});
        QCOMPARE(deviceName(p.device(track, rack)), QStringLiteral("Audio Effect Rack"));
        const int steps = f.stack.count();
        f.editor.renameRack(track, p.device(track, rack).chains[0].devices[0].id, "Not a rack");
        QCOMPARE(f.stack.count(), steps);
    }

    void copyingAndPastingDevices() {
        EditorFixture f;
        Project& p = f.project;
        const QString track = f.editor.addAudioTrack();
        const QString other = f.editor.addAudioTrack();
        const QString source = f.editor.addAudioTrack();
        const QStringList ab = utilities(f, track, 2);
        const QString rack = f.editor.groupDevices(track, {ab[1]});
        f.editor.setDeviceSidechain(track, ab[0], Sidechain{source});
        f.editor.setDevicesFolded(track, {ab[1]}, true);
        // Those in a selected rack go with it.
        const std::vector<Device> copied = f.editor.copyDevices(track, {ab[0], rack, ab[1]});
        QCOMPARE(copied.size(), size_t(2));
        QCOMPARE(copied[0].id, ab[0]);
        QCOMPARE(copied[1].id, rack);
        const QStringList pasted = f.editor.pasteDevices(other, copied, -1, {}, {ab[1]});
        QCOMPARE(pasted.size(), 2);
        QCOMPARE(f.stack.undoText(), QStringLiteral("Paste Devices"));
        QVERIFY(!pasted.contains(ab[0]) && !pasted.contains(rack));
        QVERIFY(p.device(other, pasted[0]).sidechain == Sidechain{source});  // kept
        const QString innerCopy = p.device(other, pasted[1]).chains[0].devices[0].id;
        QVERIFY(p.isDeviceFolded(innerCopy));  // folded as its original was
        // Onto the sidechain's source, it would close a cycle: it goes.
        const QStringList ontoSource = f.editor.pasteDevices(source, {copied[0]});
        QCOMPARE(f.stack.undoText(), QStringLiteral("Paste Utility"));
        QVERIFY(!p.device(source, ontoSource[0]).sidechain);
    }
};

QTEST_GUILESS_MAIN(TestEditorRacks)
#include "test_editor_racks.moc"
