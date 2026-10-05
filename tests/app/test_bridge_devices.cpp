// The engine taking the project's devices through the bridge: racks keeping
// each device's processor as they are made, undone, moved and deleted; chain
// faders and nested devices automated, and overridden by hand; macros moving
// plug-in parameters; sidechains; presets (a plug-in's renders as its device,
// a built-in device's reaches the engine, a missing plug-in, a new plug-in's
// default state). The changes are made as the editor makes them (the model's
// commands). Plug-in parts skip without the test plug-ins.

#include "BridgeTestSupport.h"
#include "TestSupport.h"

#include "io/Presets.h"
#include "io/Serialization.h"
#include "model/Devices.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

#include <cmath>
#include <random>

using namespace sub::app;
using sub::app::test::Studio;

namespace {

constexpr int kSpb = test::kSampleRate / 2;  // samples per beat at 120 BPM

// One second of noise (uniform, -0.5..0.5) in both channels.
QString noiseWav(const test::TempDir& dir, unsigned seed = 1) {
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> uniform(-0.5f, 0.5f);
    std::vector<float> samples(2 * test::kSampleRate);
    for (float& sample : samples) sample = uniform(random);
    return test::writeWav(dir.path(QStringLiteral("noise%1.wav").arg(seed)), samples, 2);
}

std::vector<float> render(Studio& studio, double beats = 1.0) {
    studio.bridge->waitForDeviceStates();
    return studio.render(static_cast<int64_t>(beats * kSpb));
}

// A render once parameters have got where they were set (they glide there on the first).
std::vector<float> settled(Studio& studio) {
    render(studio);
    return render(studio);
}

bool allClose(const std::vector<float>& a, const std::vector<float>& b, double tolerance) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i])) > tolerance) return false;
    }
    return true;
}

std::vector<float> plus(const std::vector<float>& a, const std::vector<float>& b) {
    std::vector<float> sum(a.size());
    for (size_t i = 0; i < a.size(); ++i) sum[i] = a[i] + b[i];
    return sum;
}

std::vector<float> scaled(std::vector<float> samples, float gain) {
    for (float& sample : samples) sample *= gain;
    return samples;
}

// The engine's devices on a track: processor ids, a rack as "id[chain|chain]".
QString engineTree(Studio& studio, const QString& trackId) {
    sub::Engine& engine = studio.engine;
    std::function<QString(uint32_t)> chain = [&](uint32_t chainId) {
        QStringList items;
        for (const uint32_t pid : engine.chainProcessors(chainId)) {
            if (engine.processorInfo(pid).typeId == "rack") {
                QStringList chains;
                for (const uint32_t c : engine.rackChains(pid)) chains.append(chain(c));
                items.append(QString::number(pid) + u'[' + chains.join(u'|') + u']');
            } else {
                items.append(QString::number(pid));
            }
        }
        return items.join(u' ');
    };
    return chain(engine.trackChain(*studio.bridge->engineTrackId(trackId)));
}

QString pid(Studio& studio, const QString& trackId, const QString& deviceId) {
    const auto id = studio.bridge->engineDeviceId(trackId, deviceId);
    return id ? QString::number(*id) : QStringLiteral("none");
}

// The editor's macro hooks: a parameter as the engine describes it (a built-in
// device's as the registry does), and its value set by hand.
std::optional<sub::ParamInfo> paramInfo(Studio& studio, const QString& trackId, const QString& deviceId,
                                        const QString& paramId) {
    const Device& device = studio.project.device(trackId, deviceId);
    if (!device.isPlugin() && !device.isRack()) {
        const sub::ParamInfo* info = builtinParamInfo(device.kind, paramId);
        return info != nullptr ? std::optional(*info) : std::nullopt;
    }
    return studio.bridge->deviceParamInfo(trackId, deviceId, paramId);
}

void mapMacro(Studio& studio, const QString& trackId, const QString& rackId, int index, const QString& deviceId,
              const QString& paramId) {
    std::vector<MacroMapping> macros;
    for (const MacroMapping& m : studio.project.device(trackId, rackId).macros) {
        if (m.deviceId != deviceId || m.paramId != paramId) macros.push_back(m);
    }
    macros.push_back({index, deviceId, paramId, 0.0, 1.0});
    studio.edit.setMacros(trackId, rackId, macros);
}

// Turns a rack's macro: it and every parameter mapped to it, one undo step (as the editor's set_macro).
void setMacro(Studio& studio, const QString& trackId, const QString& rackId, int index, double value) {
    QMap<DeviceParam, double> now{{{rackId, macroParam(index)}, value}};
    for (const MacroMapping& mapping : studio.project.device(trackId, rackId).macros) {
        if (mapping.macro != index || !studio.project.hasDevice(trackId, mapping.deviceId)) continue;
        if (const auto info = paramInfo(studio, trackId, mapping.deviceId, mapping.paramId)) {
            now.insert({mapping.deviceId, mapping.paramId}, info->fromNormalized(static_cast<float>(mapping.target(value))));
        }
    }
    QMap<DeviceParam, double> old;
    for (auto it = now.constBegin(); it != now.constEnd(); ++it) {
        const auto& [deviceId, paramId] = it.key();
        std::optional<double> own;
        const Device& device = studio.project.device(trackId, deviceId);
        if (device.params.contains(paramId)) own = device.params.value(paramId);
        if (!own) own = studio.bridge->ownValue(trackId, automation::deviceKey(deviceId, paramId));  // a plug-in's own
        if (!own) {
            const auto info = paramInfo(studio, trackId, deviceId, paramId);
            own = info ? info->defaultValue : it.value();
        }
        old.insert(it.key(), *own);
    }
    studio.stack.push(new SetDeviceParamsCommand(&studio.project, trackId, old, now, QStringLiteral("Change Macro")));
}

// A preset loaded into a device of its kind, as the editor does it (a built-in
// device takes its parameters, a plug-in its state). A plug-in's state before is
// the model's: store it first.
void loadPresetInto(Studio& studio, const QString& trackId, const QString& deviceId, const Device& preset) {
    const Device device = studio.project.device(trackId, deviceId);
    const QString text = QStringLiteral("Load Preset");
    studio.stack.beginMacro(text);
    if (!device.isPlugin()) {
        QMap<DeviceParam, double> now;
        QMap<DeviceParam, double> old;
        for (auto it = preset.params.constBegin(); it != preset.params.constEnd(); ++it) {
            now.insert({deviceId, it.key()}, it.value());
            double own = it.value();
            if (device.params.contains(it.key())) {
                own = device.params.value(it.key());
            } else if (const sub::ParamInfo* info = builtinParamInfo(device.kind, it.key())) {
                own = info->defaultValue;
            }
            old.insert({deviceId, it.key()}, own);
        }
        if (old != now) studio.stack.push(new SetDeviceParamsCommand(&studio.project, trackId, old, now, text));
    }
    if (preset.state != device.state && (preset.state || !device.isPlugin())) {
        studio.stack.push(new SetDeviceStateCommand(&studio.project, trackId, deviceId, device.state, preset.state, text));
    }
    studio.stack.endMacro();
}

// Only `keep` is heard (the others muted).
void solo(Studio& studio, const QString& keep) {
    QStringList ids;
    for (const Track& track : studio.project.tracks()) ids.append(track.id);
    for (const QString& id : ids) studio.edit.setTrack(id, TrackField::Mute, id != keep);
}

}  // namespace

class TestBridgeDevices : public QObject {
    Q_OBJECT

    std::optional<PluginRef> plugin(const QString& name) {
        return test::testPlugin(test::testPluginsBundle(), name);
    }

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void theEngineFollowsRacks() {
        test::TempDir dir;
        Studio studio;
        QUndoStack& stack = studio.stack;
        const QString track = studio.clipTrack(noiseWav(dir));
        const std::vector<float> raw = render(studio);  // the clip, nothing in its way
        const QString a = studio.edit.addDevice(track, QStringLiteral("utility"));
        const QString b = studio.edit.addDevice(track, QStringLiteral("utility"));
        studio.edit.setDeviceParam(track, a, QStringLiteral("gain"), -6.0);
        const std::vector<float> plain = settled(studio);
        QString pa = pid(studio, track, a);
        const QString pb = pid(studio, track, b);
        const QString rack = studio.edit.groupDevices(track, {a, b});  // the same processors, in the rack's chain
        QString pr = pid(studio, track, rack);
        QCOMPARE(engineTree(studio, track), QStringLiteral("%1[%2 %3]").arg(pr, pa, pb));
        QVERIFY(render(studio) == plain);  // one chain: as without the rack
        const QString wet = studio.edit.addRackChain(track, rack);  // an empty chain beside it: its input
        QVERIFY(allClose(render(studio), plus(plain, raw), 1e-6));
        studio.edit.setChain(track, wet, ChainField::Mute, true);
        QVERIFY(render(studio) == plain);
        studio.edit.setChain(track, wet, ChainField::Mute, false);
        studio.edit.setChain(track, wet, ChainField::Solo, true);
        QVERIFY(allClose(render(studio), raw, 1e-6));
        studio.edit.setChain(track, wet, ChainField::Solo, false);
        studio.edit.setChain(track, wet, ChainField::VolumeDb, -120.0);  // the faders' floor: silent
        QVERIFY(allClose(render(studio), plain, 1e-6));
        // Moving a device out of the rack, and undoing everything: the same processors throughout.
        studio.edit.moveDevices(track, {b}, 2);
        QCOMPARE(engineTree(studio, track), QStringLiteral("%1[%2|] %3").arg(pr, pa, pb));
        while (studio.project.hasDevice(track, rack)) stack.undo();
        QCOMPARE(engineTree(studio, track), QStringLiteral("%1 %2").arg(pa, pb));
        QVERIFY(render(studio) == plain);
        while (stack.canRedo()) stack.redo();
        pr = pid(studio, track, rack);  // (a new rack: it wasn't kept)
        QCOMPARE(engineTree(studio, track), QStringLiteral("%1[%2|] %3").arg(pr, pa, pb));
        // The rack to another track, with everything in it; then deleted, and back.
        const QString other = studio.edit.addAudioTrack();
        studio.edit.moveDevicesToTrack(track, {rack}, other);
        QCOMPARE(engineTree(studio, other), QStringLiteral("%1[%2|]").arg(pr, pa));
        QCOMPARE(engineTree(studio, track), pb);
        studio.edit.removeDevices(other, {rack});
        QCOMPARE(engineTree(studio, other), QString());
        QVERIFY(!studio.bridge->engineDeviceId(other, a));
        stack.undo();
        pr = pid(studio, other, rack);
        pa = pid(studio, other, a);
        QCOMPARE(engineTree(studio, other), QStringLiteral("%1[%2|]").arg(pr, pa));
        // Rack chains' meters, by the model's chain ids.
        BridgeTestAccess::pollMeters(*studio.bridge);
        QVERIFY(studio.bridge->chainMeters().contains(wet));
    }

    void automationOfNestedDevicesAndChainFaders() {
        test::TempDir dir;
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        const QString track = studio.clipTrack(noiseWav(dir));
        const QString a = studio.edit.addDevice(track, QStringLiteral("utility"));
        const QString rack = studio.edit.groupDevices(track, {a});
        const Chain chain = studio.project.device(track, rack).chains.front();
        const QString key = automation::chainKey(rack, chain.id, automation::kChainVolume);
        QVERIFY(bridge.canAutomate(track, key));
        QVERIFY(bridge.canAutomate(track, automation::deviceKey(a, QStringLiteral("gain"))));
        QCOMPARE(bridge.paramSpec(track, key)->name, chain.name + QStringLiteral(" Volume"));
        QCOMPARE(*bridge.ownValue(track, key), 0.0);
        studio.edit.setEnvelope(track, key, {{0.0, 0.0, 0.0}});  // silent
        QVERIFY(bridge.isAutomated(track, key));
        QCOMPARE(test::peak(render(studio)), 0.f);
        studio.edit.setChain(track, chain.id, ChainField::VolumeDb, -6.0);  // by hand: its automation stops
        QVERIFY(bridge.isOverridden(track, key));
        QVERIFY(test::peak(render(studio)) > 0.1f);
        bridge.reEnableAutomation(track);
        QCOMPARE(test::peak(render(studio)), 0.f);
        studio.edit.setEnvelope(track, key, {});
        const QString gain = automation::deviceKey(a, QStringLiteral("gain"));
        studio.edit.setEnvelope(track, gain, {{0.0, 0.0, 0.0}});  // -60 dB
        QVERIFY(test::peak(render(studio), 2 * test::kSampleRate / 20) < 0.01f);  // (after the gain glides there)
        // What the automation lanes show: the mixer, then each device (the rack, the utility in it).
        const std::vector<ParamGroup> groups = bridge.paramGroups(track);
        QCOMPARE(groups.size(), size_t{3});
        QCOMPARE(groups[0].id, QStringLiteral("mixer"));
        QCOMPARE(groups[1].id, rack);
        QCOMPARE(groups[2].id, a);
        QVERIFY(bridge.isAutomated(track, gain));
        QCOMPARE(*bridge.currentValue(track, gain, 0.0), -60.0);
        QCOMPARE(*bridge.ownValue(track, gain), 0.0);  // (its default: the model has none)
        // Changed by hand while automated: overridden, its own value back in the engine.
        studio.edit.setDeviceParam(track, a, QStringLiteral("gain"), -6.0);
        QVERIFY(bridge.isOverridden(track, gain) && !bridge.isAutomated(track, gain));
        QCOMPARE(*bridge.currentValue(track, gain, 0.0), -6.0);
        // A device that isn't loaded: its values still show.
        QVERIFY(!bridge.paramSpec(track, automation::deviceKey(QStringLiteral("gone"), QStringLiteral("gain"))));
    }

    void theEngineTakesSidechains() {
        const auto ref = plugin(QStringLiteral("SUB Test Sidechain"));
        if (!ref) QSKIP("test plug-ins not built");
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        sub::Engine& engine = studio.engine;
        const auto sidechain = [&](const QString& trackId, const QString& deviceId) -> QString {
            const auto info = engine.processorSidechain(*bridge.engineDeviceId(trackId, deviceId));
            if (!info) return QStringLiteral("none");
            return QStringLiteral("%1 %2 %3").arg(info->trackId).arg(static_cast<int>(info->tap)).arg(info->tapProcessorId);
        };
        const auto expect = [&](const QString& source, sub::SidechainTap tap, quint32 tapped = 0) {
            return QStringLiteral("%1 %2 %3").arg(*bridge.engineTrackId(source)).arg(static_cast<int>(tap)).arg(tapped);
        };
        const QString kick = studio.edit.addAudioTrack();
        const QString bass = studio.edit.addAudioTrack();
        const QString keyed = studio.edit.addDevice(bass, kPluginKind, ref);
        const QString utility = studio.edit.addDevice(bass, QStringLiteral("utility"));
        QVERIFY(bridge.hasSidechainInput(bass, keyed) && !bridge.hasSidechainInput(bass, utility));
        QCOMPARE(sidechain(bass, keyed), QStringLiteral("none"));
        studio.edit.setDeviceSidechain(bass, keyed, Sidechain{kick});
        QCOMPARE(sidechain(bass, keyed), expect(kick, sub::SidechainTap::PostFader));
        const QString tapped = studio.edit.addDevice(kick, QStringLiteral("utility"));
        studio.edit.setDeviceSidechain(bass, keyed, Sidechain{kick, tapped});
        quint32 tappedId = *bridge.engineDeviceId(kick, tapped);
        QCOMPARE(sidechain(bass, keyed), expect(kick, sub::SidechainTap::AfterDevice, tappedId));
        studio.edit.removeDevices(kick, {tapped});  // before the fader, until it comes back
        QCOMPARE(sidechain(bass, keyed), expect(kick, sub::SidechainTap::PreFader));
        studio.stack.undo();
        tappedId = *bridge.engineDeviceId(kick, tapped);
        QCOMPARE(sidechain(bass, keyed), expect(kick, sub::SidechainTap::AfterDevice, tappedId));
        // Moving the device keeps it (a processor moving gives it up in the engine until it is there).
        const QString pad = studio.edit.addAudioTrack();
        studio.edit.moveDevicesToTrack(bass, {keyed}, pad);
        QCOMPARE(sidechain(pad, keyed), expect(kick, sub::SidechainTap::AfterDevice, tappedId));
        // Into its source's group: the sidechain goes before the route that would close the cycle.
        const QString group = studio.edit.groupTracks({kick});
        studio.edit.setDeviceSidechain(pad, keyed, Sidechain{group});
        QCOMPARE(sidechain(pad, keyed), expect(group, sub::SidechainTap::PostFader));
        studio.edit.moveTracks({pad}, studio.project.trackIndex(kick) + 1, group);
        QCOMPARE(sidechain(pad, keyed), QStringLiteral("none"));
        QCOMPARE(engine.trackOutput(*bridge.engineTrackId(pad)), *bridge.engineTrackId(group));
        studio.stack.undo();
        QCOMPARE(sidechain(pad, keyed), expect(group, sub::SidechainTap::PostFader));
        // Deleting the source, and undoing it.
        studio.edit.deleteTracks({group});
        QCOMPARE(sidechain(pad, keyed), QStringLiteral("none"));
        studio.stack.undo();
        QCOMPARE(sidechain(pad, keyed), expect(group, sub::SidechainTap::PostFader));
        // A project loaded with a device keyed by a track listed after its own.
        const QString later = studio.edit.addAudioTrack();
        studio.edit.setDeviceSidechain(pad, keyed, Sidechain{later, kPreFader});
        loadInto(studio.project, projectToJson(studio.project));
        QVERIFY(bridge.pluginPending(keyed));  // (its plug-in loads after the project: with its sidechain)
        bridge.loadPendingPlugins();
        QCOMPARE(sidechain(pad, keyed), expect(later, sub::SidechainTap::PreFader));
        // Before the source's devices; a MIDI track's own audio is its instrument's.
        studio.edit.setDeviceSidechain(pad, keyed, Sidechain{later, kPreFx});
        loadInto(studio.project, projectToJson(studio.project));
        QCOMPARE(*studio.project.device(pad, keyed).sidechain, (Sidechain{later, kPreFx}));
        bridge.loadPendingPlugins();
        QCOMPARE(sidechain(pad, keyed), expect(later, sub::SidechainTap::PreFx));
        const QString keys = studio.edit.addMidiTrack();
        studio.edit.setDeviceSidechain(pad, keyed, Sidechain{keys, kPreFx});
        quint32 synth = *bridge.engineDeviceId(keys, studio.project.track(keys).devices.front().id);
        QCOMPARE(sidechain(pad, keyed), expect(keys, sub::SidechainTap::AfterDevice, synth));
        studio.edit.addDevice(keys, QStringLiteral("synth"));  // another instrument, in its place
        synth = *bridge.engineDeviceId(keys, studio.project.track(keys).devices.front().id);
        QCOMPARE(sidechain(pad, keyed), expect(keys, sub::SidechainTap::AfterDevice, synth));
    }

    void macrosReachTheEngine() {
        const auto ref = plugin(QStringLiteral("SUB Test Effect"));
        if (!ref) QSKIP("test plug-ins not built");
        test::TempDir dir;
        Studio studio;
        const QString track = studio.clipTrack(noiseWav(dir));
        const QString fx = studio.edit.addDevice(track, kPluginKind, ref);
        const QString rack = studio.edit.groupDevices(track, {fx});
        const quint32 processor = *studio.bridge->engineDeviceId(track, fx);
        const sub::ParamInfo gain = studio.engine.processorParams(processor)[0];  // SUB Test Effect's gain
        mapMacro(studio, track, rack, 0, fx, QString::fromStdString(gain.id));
        setMacro(studio, track, rack, 0, 0.25);
        QVERIFY(std::abs(studio.engine.processorParam(processor, 0) - gain.fromNormalized(0.25f)) < 1e-6f);
        studio.stack.undo();
        QVERIFY(std::abs(studio.engine.processorParam(processor, 0) - 0.5f) < 1e-6f);  // as it was (its default)
    }

    void aSavedRackLoadsAndRendersTheSame() {
        // A preset of a rack: plug-in state, macros and all; loaded twice, two new racks.
        const auto ref = plugin(QStringLiteral("SUB Test Effect"));
        if (!ref) QSKIP("test plug-ins not built");
        test::TempDir dir;
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        const QString wav = noiseWav(dir);
        const QString track = studio.clipTrack(wav);
        const QString fx = studio.edit.addDevice(track, kPluginKind, ref);
        studio.engine.setProcessorParam(*bridge.engineDeviceId(track, fx), 0, 0.3f);  // in the plug-in only
        const QString gain = studio.edit.addDevice(track, QStringLiteral("utility"));
        const QString rack = studio.edit.groupDevices(track, {fx});
        const QString wet = studio.edit.addRackChain(track, rack);
        studio.edit.moveDevices(track, {gain}, 0, wet);
        studio.edit.setChain(track, wet, ChainField::Pan, -0.5);
        mapMacro(studio, track, rack, 0, gain, QStringLiteral("gain"));
        setMacro(studio, track, rack, 0, 0.6);
        const std::vector<float> original = settled(studio);
        bridge.storePluginStates();  // (as saving does)
        const QString path = dir.path("rack.gilpreset");
        savePreset(studio.project.device(track, rack), path);
        std::vector<std::pair<QString, Device>> loaded;
        for (int i = 0; i < 2; ++i) {
            const QString copy = studio.clipTrack(wav);
            const Device device = loadPreset(path);
            studio.edit.insertDevice(copy, device);
            loaded.emplace_back(copy, device);
            solo(studio, copy);
            QVERIFY(allClose(settled(studio), original, 1e-6));  // alone, it sounds as the original
        }
        const Device& r1 = loaded[0].second;
        const auto& [t2, r2] = loaded[1];
        QVERIFY(r1.id != r2.id && r1.id != rack);
        // Its macros move its own devices.
        const QString innerGain = studio.project.chain(t2, r2.chains[1].id).devices.front().id;
        setMacro(studio, t2, r2.id, 0, 0.0);
        QCOMPARE(studio.project.device(t2, innerGain).params.value("gain"), -60.0);
        QVERIFY(std::abs(studio.project.device(track, gain).params.value("gain") - (-60 + 0.6 * 84)) < 1e-4);
    }

    void aPlugInPresetRendersAsTheDeviceItWasSavedFrom() {
        const auto ref = plugin(QStringLiteral("SUB Test Effect"));
        if (!ref) QSKIP("test plug-ins not built");
        test::TempDir dir;
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        sub::Engine& engine = studio.engine;
        const QString wav = noiseWav(dir);
        const QString original = studio.clipTrack(wav);
        const QString fx = studio.edit.addDevice(original, kPluginKind, ref);
        engine.setProcessorParam(*bridge.engineDeviceId(original, fx), 0, 0.3f);  // in the plug-in only
        const std::vector<float> expected = render(studio);
        bridge.storePluginStates();  // (as the save button does)
        const QString path = saveToLibrary(studio.project.device(original, fx), QStringLiteral("Quiet"), dir.path());

        // As a new device...
        const QString added = studio.clipTrack(wav);
        const Device device = loadPreset(path);
        studio.edit.insertDevice(added, device);
        QVERIFY(device.id != fx);
        QVERIFY(std::abs(engine.processorParam(*bridge.engineDeviceId(added, device.id), 0) - 0.3f) < 1e-6f);
        solo(studio, added);
        QVERIFY(allClose(render(studio), expected, 1e-6));

        // ...and into a device of the same plug-in, undoably.
        const QString into = studio.clipTrack(wav);
        const QString target = studio.edit.addDevice(into, kPluginKind, ref);
        const quint32 processor = *bridge.engineDeviceId(into, target);
        solo(studio, into);
        const std::vector<float> untouched = render(studio);
        bridge.storePluginStates(QSet<QString>{target});
        QVERIFY(studio.project.device(into, target).state.has_value());
        loadPresetInto(studio, into, target, loadPreset(path));
        QCOMPARE(*bridge.engineDeviceId(into, target), processor);  // the same plug-in, with the preset's state
        QVERIFY(std::abs(engine.processorParam(processor, 0) - 0.3f) < 1e-6f);
        QVERIFY(allClose(render(studio), expected, 1e-6));
        studio.stack.undo();
        QVERIFY(std::abs(engine.processorParam(processor, 0) - 0.5f) < 1e-6f);
        QVERIFY(allClose(render(studio), untouched, 1e-6));
        studio.stack.redo();
        QVERIFY(std::abs(engine.processorParam(processor, 0) - 0.3f) < 1e-6f);
    }

    void aBuiltInPresetLoadedIntoADeviceReachesTheEngine() {
        test::TempDir dir;
        Studio studio;
        const QString track = studio.clipTrack(noiseWav(dir));
        const QString source = studio.edit.addDevice(track, QStringLiteral("utility"));
        studio.edit.setDeviceParam(track, source, QStringLiteral("gain"), -20.0);
        const std::vector<float> expected = settled(studio);
        const QString path = saveToLibrary(studio.project.device(track, source), QStringLiteral("Down"), dir.path());
        studio.edit.removeDevices(track, {source});
        const QString target = studio.edit.addDevice(track, QStringLiteral("utility"));
        const std::vector<float> untouched = settled(studio);
        loadPresetInto(studio, track, target, loadPreset(path));
        QVERIFY(allClose(settled(studio), expected, 1e-6));
        studio.stack.undo();
        QVERIFY(allClose(settled(studio), untouched, 1e-6));
    }

    void aPresetOfAMissingPlugInLoadsAsAMissingDevice() {
        // Kept, and marked missing (as when loading a project); in a rack, the rest of the rack works.
        test::TempDir dir;
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        const QString track = studio.clipTrack(noiseWav(dir));
        PluginRef gone;
        gone.uid = QString(32, u'0');
        gone.name = QStringLiteral("Long Gone");
        gone.vendor = QStringLiteral("Nobody");
        gone.path = QStringLiteral("C:/nowhere/Gone.vst3");
        Device missing;
        missing.id = QStringLiteral("m");
        missing.kind = kPluginKind;
        missing.plugin = gone;
        missing.state = QStringLiteral("AAAA");
        QSignalSpy messages(&bridge, &EngineBridge::statusMessage);
        const Device device = presetDevice(deviceToPreset(missing));
        studio.edit.insertDevice(track, device);
        QCOMPARE(*studio.project.device(track, device.id).plugin, gone);  // kept, as it was
        QVERIFY(!bridge.engineDeviceId(track, device.id));
        QVERIFY(bridge.pluginErrors().value(device.id).contains("Long Gone is not installed"));
        QVERIFY(bridge.pluginError(device.id).contains("Long Gone is not installed"));
        QVERIFY(!messages.isEmpty());

        const QString gain = studio.edit.addDevice(track, QStringLiteral("utility"));
        studio.edit.setDeviceParam(track, gain, QStringLiteral("gain"), -20.0);
        const QString unknown = studio.edit.addDevice(track, QStringLiteral("utility"));
        const QString rack = studio.edit.groupDevices(track, {gain, unknown});
        QJsonObject data = deviceToPreset(studio.project.device(track, rack));
        QJsonObject rackData = data["device"].toObject();
        QJsonArray chains = rackData["chains"].toArray();
        QJsonObject chain = chains[0].toObject();
        QJsonArray devices = chain["devices"].toArray();
        devices.insert(1, deviceToPreset(missing)["device"]);
        QJsonObject later = devices[2].toObject();
        later["kind"] = QStringLiteral("a device of a later version");
        devices[2] = later;
        chain["devices"] = devices;
        chains[0] = chain;
        rackData["chains"] = chains;
        data["device"] = rackData;
        studio.edit.removeDevices(track, {rack, device.id});
        const std::vector<float> expectedQuiet = scaled(settled(studio), 0.1f);  // -20 dB
        const Device loaded = presetDevice(data);
        studio.edit.insertDevice(track, loaded);
        const auto& inner = loaded.chains.front().devices;
        QCOMPARE(inner.size(), size_t{3});
        QVERIFY(bridge.engineDeviceId(track, loaded.id));
        QVERIFY(bridge.engineDeviceId(track, inner[0].id));
        QVERIFY(!bridge.engineDeviceId(track, inner[1].id));
        QVERIFY(!bridge.engineDeviceId(track, inner[2].id));
        QVERIFY(bridge.pluginError(inner[2].id).contains("not a device this version"));
        QVERIFY(allClose(settled(studio), expectedQuiet, 1e-4));
    }

    void aNewPlugInStartsWithItsDefaultState() {
        const auto ref = plugin(QStringLiteral("SUB Test Effect"));
        if (!ref) QSKIP("test plug-ins not built");
        test::TempDir dir;
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        const QString track = studio.edit.addAudioTrack();
        const QString fx = studio.edit.addDevice(track, kPluginKind, ref);
        studio.engine.setProcessorParam(*bridge.engineDeviceId(track, fx), 0, 0.3f);
        bridge.storePluginStates(QSet<QString>{fx});
        saveDefault(studio.project.device(track, fx), dir.path());
        const std::optional<Device> fresh = defaultDevice(kPluginKind, ref, dir.path());  // (the editor's device defaults)
        QVERIFY(fresh && fresh->state);
        studio.edit.insertDevice(track, *fresh);
        QVERIFY(std::abs(studio.engine.processorParam(*bridge.engineDeviceId(track, fresh->id), 0) - 0.3f) < 1e-6f);
    }

    void deviceParametersForTheEditors() {
        const auto ref = plugin(QStringLiteral("SUB Test Effect"));
        test::TempDir dir;
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        const QString track = studio.edit.addAudioTrack();
        const QString utility = studio.edit.addDevice(track, QStringLiteral("utility"));
        const QList<ProcessorParam> params = bridge.deviceParams(track, utility);
        QVERIFY(!params.isEmpty());
        const int gainIndex = static_cast<int>(
            std::find_if(params.begin(), params.end(), [](const ProcessorParam& p) { return p.id == u"gain"; }) -
            params.begin());
        QVERIFY(gainIndex < params.size());
        const ProcessorParam& gain = params[gainIndex];
        const auto info = bridge.deviceParamInfo(track, utility, QStringLiteral("gain"));
        QVERIFY(info && gain.name == QString::fromStdString(info->name));
        QCOMPARE(gain.toNormalized(gain.defaultValue), static_cast<double>(info->toNormalized(info->defaultValue)));
        QCOMPARE(gain.fromNormalized(1.0), static_cast<double>(info->maxValue));
        QCOMPARE(*bridge.deviceParamValue(track, utility, gainIndex), gain.defaultValue);
        QCOMPARE(bridge.deviceLatency(track, utility), 0);
        QVERIFY(!bridge.deviceParamValue(track, QStringLiteral("nope"), 0));
        QVERIFY(bridge.deviceParams(track, QStringLiteral("nope")).isEmpty());
        // A compressor's own editor draws its gain reduction (a display).
        const QString compressor = studio.edit.addDevice(track, QStringLiteral("compressor"));
        const QList<ProcessorDisplay> displays = bridge.processorDisplays(track, compressor);
        QVERIFY(!displays.isEmpty());
        std::vector<float> values;
        const quint64 next = bridge.readProcessorDisplay(track, compressor, 0, 0, values);
        QCOMPARE(next, quint64{values.size()} + 0);
        if (!ref) return;
        // A plug-in's own text for its values, with its unit; specs that show it.
        const QString fx = studio.edit.addDevice(track, kPluginKind, ref);
        QVERIFY(!bridge.deviceParamText(track, fx, 0, 0.5).isEmpty());
        const std::vector<ParamSpec> specs = bridge.deviceParamSpecs(track, studio.project.device(track, fx));
        QVERIFY(!specs.empty() && specs.front().text);
        QCOMPARE(specs.front().format(0.5), bridge.deviceParamText(track, fx, 0, 0.5));
    }
};

QTEST_GUILESS_MAIN(TestBridgeDevices)
#include "test_bridge_devices.moc"
