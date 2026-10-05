// What the engine hears of the editor's edits, now that the session has both
// (the tests the editor and the bridge ports left for it): groups as buses,
// returns and sends, inputs from tracks, sidechains, racks (their chains,
// faders, automation and macros), presets rendering as the devices they were
// saved from (plug-ins too; missing ones loading as missing devices; default
// presets), frozen tracks playing their frozen audio, and the notes a MIDI
// clip plays.
// From tests/test_groups_model.py, test_sends_model.py,
// test_resampling_model.py, test_sidechain_model.py, test_racks_model.py,
// test_presets.py, test_freeze_model.py and test_midi_model.py.

#include "BridgeTestSupport.h"
#include "EditorFixture.h"
#include "SessionFixture.h"
#include "TestSupport.h"

#include "audio/EngineDescs.h"
#include "io/Presets.h"
#include "io/Serialization.h"
#include "model/Devices.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>
#include <QTest>

#include <cmath>
#include <functional>
#include <random>

using namespace sub::app;
using sub::app::test::SessionFixture;
using sub::app::test::TempDir;

namespace {

constexpr int kRate = test::kSampleRate;
constexpr int kSamplesPerBeat = kRate / 2;  // at 120 BPM

QString dcWav(const TempDir& dir) {
    return test::writeWav(dir.path(QStringLiteral("dc.wav")), std::vector<float>(2 * kRate, 0.5f), 2);
}

bool near(double a, double b, double tolerance = 1e-3) { return std::abs(a - b) <= tolerance; }

bool allClose(const std::vector<float>& a, const std::vector<float>& b, double tolerance) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::abs(static_cast<double>(a[i]) - b[i]) > tolerance) return false;
    }
    return true;
}

std::vector<float> scaled(const std::vector<float>& samples, float by) {
    std::vector<float> out(samples);
    for (float& sample : out) sample *= by;
    return out;
}

std::vector<float> plus(const std::vector<float>& a, const std::vector<float>& b) {
    std::vector<float> out(a);
    for (size_t i = 0; i < out.size() && i < b.size(); ++i) out[i] += b[i];
    return out;
}

// test_racks_model.py's bridged fixture's helpers, through a session.
struct Bridged : SessionFixture {
    Bridged() : SessionFixture(false) {}

    // An audio track playing a second of noise (seeded) from beat 0.
    QString noiseTrack(const TempDir& dir, unsigned seed = 1) {
        std::mt19937 random(seed);
        std::uniform_real_distribution<float> uniform(-0.5f, 0.5f);
        std::vector<float> samples(2 * kRate);
        for (float& sample : samples) sample = uniform(random);
        const QString path = test::writeWav(dir.path(QStringLiteral("noise%1-%2.wav").arg(seed).arg(++made)), samples, 2);
        engine.loadSource(path.toStdString());
        const QString track = editor().addAudioTrack();
        project().setClips(track, {Clip::audio(QStringLiteral("c"), path, QStringLiteral("noise"), 0.0, 1.0, 0.0, 1.0)});
        return track;
    }
    // A beat rendered (once the devices' states are restored).
    std::vector<float> renderBeats(double beats = 1.0) { return render(static_cast<int64_t>(beats * kSamplesPerBeat)); }
    // A render once parameters have got where they were set (they glide there on the first).
    std::vector<float> settled() {
        renderBeats();
        return renderBeats();
    }
    // Every track muted but `keep`.
    void solo(const QString& keep) {
        for (const Track& track : project().tracks()) editor().setTrackParam(track.id, TrackField::Mute, track.id == keep ? 0.0 : 1.0);
    }
    quint32 trackOf(const QString& trackId) const { return *bridge().engineTrackId(trackId); }
    quint32 deviceOf(const QString& trackId, const QString& deviceId) const {
        return *bridge().engineDeviceId(trackId, deviceId);
    }
    // The engine's devices on a track: processor ids, a rack as "id[chain|chain]".
    QString engineTree(const QString& trackId) {
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
        return chain(engine.trackChain(trackOf(trackId)));
    }
    QString pid(const QString& trackId, const QString& deviceId) const {
        return QString::number(deviceOf(trackId, deviceId));
    }
    int made = 0;
};

std::optional<PluginRef> testPlugin(const QString& name) { return test::testPlugin(test::testPluginsBundle(), name); }

}  // namespace

class TestSessionEngine : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }
    void init() { QSettings().clear(); }

    // --- test_groups_model.py ---

    void theEngineHearsGroupsAsBuses() {
        Bridged f;
        TempDir dir;
        const QString wav = dcWav(dir);
        const QString a = f.clipTrack(wav, 1.0, QStringLiteral("A"));
        const QString b = f.clipTrack(wav, 1.0, QStringLiteral("B"));
        QVERIFY(near(f.level(), 1.0));
        const QString group = f.editor().groupTracks({a, b});
        f.editor().setTrackParam(group, TrackField::VolumeDb, -6.0206);
        QVERIFY(near(f.level(), 0.5));
        f.editor().setTrackParam(group, TrackField::Mute, 1.0);
        QVERIFY(near(f.level(), 0.0));
        f.editor().moveTracks({b}, static_cast<int>(f.project().tracks().size()), QString());  // out of the muted group
        QVERIFY(near(f.level(), 0.5));
        f.stack().undo();
        QVERIFY(near(f.level(), 0.0));
        f.editor().setTrackParam(group, TrackField::Mute, 0.0);  // (mute isn't undone)
        const QString inner = f.editor().groupTracks({a});  // a group in the group
        QVERIFY(near(f.level(), 0.5));
        f.editor().deleteTracks({group});  // with what is in it
        QVERIFY(near(f.level(), 0.0));
        f.stack().undo();
        QVERIFY(near(f.level(), 0.5));
        QCOMPARE(f.project().track(inner).parent, std::optional<QString>(group));
    }

    // --- test_sends_model.py ---

    void theEngineHearsReturnsAndSends() {
        Bridged f;
        TempDir dir;
        const QString track = f.clipTrack(dcWav(dir), 1.0, QStringLiteral("A"));
        QVERIFY(near(f.level(), 0.5));
        const QString ret = f.editor().addReturnTrack();
        QVERIFY(near(f.level(), 0.5));  // no send yet
        f.editor().setSend(track, ret, -6.0206);
        QVERIFY(near(f.level(), 0.75));
        f.editor().setTrackParam(track, TrackField::VolumeDb, -70.0);  // silent: only a pre-fader send is heard
        QVERIFY(near(f.level(), 0.0));
        f.editor().setSend(track, ret, std::nullopt, true);
        QVERIFY(near(f.level(), 0.25));
        const QString other = f.editor().addReturnTrack();
        f.editor().setSend(ret, other, 0.0);
        QVERIFY(near(f.level(), 0.5));
        f.editor().setTrackParam(ret, TrackField::Mute, 1.0);
        QVERIFY(near(f.level(), 0.0));
        f.editor().setTrackParam(ret, TrackField::Mute, 0.0);
        f.editor().setTrackParam(track, TrackField::VolumeDb, 0.0);
        QVERIFY(near(f.level(), 1.0));  // the track, ret, other
        // Automating a send that wasn't set makes it, silent, so that the automation plays.
        const QString third = f.editor().addReturnTrack();
        f.editor().setEnvelope(track, automation::sendKey(third), test::env({{0.0, 1.0}}));
        QStringList sentTo;
        for (const sub::SendInfo& send : f.engine.trackSends(f.trackOf(track))) sentTo.append(QString::number(send.trackId));
        QCOMPARE(sentTo, (QStringList{QString::number(f.trackOf(ret)), QString::number(f.trackOf(third))}));
        QVERIFY(near(f.level(), 1.0 + 0.5 * std::pow(10.0, 6.0 / 20.0)));  // +6 dB at the lane's top
        f.editor().clearEnvelope(track, automation::sendKey(third));
        QVERIFY(near(f.level(), 1.0));
        // Deleting a return takes the sends into it along; undo brings them back.
        f.editor().deleteTracks({ret});
        QVERIFY(near(f.level(), 0.5));
        QVERIFY(f.engine.trackSends(f.trackOf(track)).empty());
        f.stack().undo();
        QVERIFY(near(f.level(), 1.0));
        // A project loaded with returns and sends plays them.
        loadInto(f.project(), projectToJson(f.project()));
        QVERIFY(near(f.level(), 1.0));
    }

    // --- test_resampling_model.py ---

    void theEngineTakesInputs() {
        Bridged f;
        Project& project = f.project();
        const auto engineInput = [&](const QString& track) { return f.engine.trackInputTrack(f.trackOf(track)); };
        const QString source = f.editor().addAudioTrack();
        const QString group = f.editor().groupTracks({source});
        const QString track = f.editor().addAudioTrack();
        f.editor().setTrackInputTrack(track, group);
        QCOMPARE(engineInput(track), std::optional<uint32_t>(f.trackOf(group)));
        f.editor().setTrackInputTrack(track, kMaster);
        QCOMPARE(engineInput(track), std::optional<uint32_t>(sub::Engine::kMaster));
        f.editor().setTrackInput(track, {0});
        QVERIFY(!engineInput(track));
        f.editor().setTrackInputTrack(track, source);
        QCOMPARE(engineInput(track), std::optional<uint32_t>(f.trackOf(source)));
        // Into the source's group: the input goes before the route that would close the cycle.
        f.editor().setTrackInputTrack(track, group);
        f.editor().moveTracks({track}, project.trackIndex(source) + 1, group);
        QVERIFY(!engineInput(track));
        QCOMPARE(f.engine.trackOutput(f.trackOf(track)), f.trackOf(group));
        f.stack().undo();
        QCOMPARE(engineInput(track), std::optional<uint32_t>(f.trackOf(group)));
        // Deleting the source, and undoing it.
        f.editor().deleteTracks({group});
        QVERIFY(!engineInput(track));
        f.stack().undo();
        QCOMPARE(engineInput(track), std::optional<uint32_t>(f.trackOf(group)));
        // A project loaded with a track taking the output of one listed after it.
        const QString later = f.editor().addAudioTrack();
        f.editor().setTrackInputTrack(track, later);
        loadInto(project, projectToJson(project));
        QCOMPARE(engineInput(track), std::optional<uint32_t>(f.trackOf(later)));
    }

    // --- test_sidechain_model.py ---

    void theEngineTakesSidechains() {
        const auto ref = testPlugin(QStringLiteral("SUB Test Sidechain"));
        if (!ref) QSKIP("test plug-ins not built");
        Bridged f;
        Project& project = f.project();
        sub::Engine& engine = f.engine;
        const auto sidechain = [&](const QString& track, const QString& device) -> QString {
            const auto info = engine.processorSidechain(f.deviceOf(track, device));
            if (!info) return QStringLiteral("none");
            return QStringLiteral("%1 %2 %3").arg(info->trackId).arg(static_cast<int>(info->tap)).arg(info->tapProcessorId);
        };
        const auto expect = [&](const QString& source, sub::SidechainTap tap, quint32 tapped = 0) {
            return QStringLiteral("%1 %2 %3").arg(f.trackOf(source)).arg(static_cast<int>(tap)).arg(tapped);
        };
        const QString kick = f.editor().addAudioTrack();
        const QString bass = f.editor().addAudioTrack();
        const QString keyed = f.editor().addDevice(bass, kPluginKind, -1, ref);
        const QString utility = f.editor().addDevice(bass, QStringLiteral("utility"));
        QVERIFY(f.bridge().hasSidechainInput(bass, keyed) && !f.bridge().hasSidechainInput(bass, utility));
        QCOMPARE(sidechain(bass, keyed), QStringLiteral("none"));
        f.editor().setDeviceSidechain(bass, keyed, Sidechain{kick});
        QCOMPARE(sidechain(bass, keyed), expect(kick, sub::SidechainTap::PostFader));
        const QString tapped = f.editor().addDevice(kick, QStringLiteral("utility"));
        f.editor().setDeviceSidechain(bass, keyed, Sidechain{kick, tapped});
        QCOMPARE(sidechain(bass, keyed), expect(kick, sub::SidechainTap::AfterDevice, f.deviceOf(kick, tapped)));
        f.editor().removeDevice(kick, tapped);  // before the fader, until it comes back
        QCOMPARE(sidechain(bass, keyed), expect(kick, sub::SidechainTap::PreFader));
        f.stack().undo();
        QCOMPARE(sidechain(bass, keyed), expect(kick, sub::SidechainTap::AfterDevice, f.deviceOf(kick, tapped)));
        // Moving the device keeps it (a processor moving gives it up in the engine until it is there).
        const QString pad = f.editor().addAudioTrack();
        f.editor().moveDevicesToTrack(bass, {keyed}, pad);
        QCOMPARE(sidechain(pad, keyed), expect(kick, sub::SidechainTap::AfterDevice, f.deviceOf(kick, tapped)));
        // Into its source's group: the sidechain goes before the route that would close the cycle.
        const QString group = f.editor().groupTracks({kick});
        f.editor().setDeviceSidechain(pad, keyed, Sidechain{group});
        QCOMPARE(sidechain(pad, keyed), expect(group, sub::SidechainTap::PostFader));
        f.editor().moveTracks({pad}, project.trackIndex(kick) + 1, group);
        QCOMPARE(sidechain(pad, keyed), QStringLiteral("none"));
        QCOMPARE(engine.trackOutput(f.trackOf(pad)), f.trackOf(group));
        f.stack().undo();
        QCOMPARE(sidechain(pad, keyed), expect(group, sub::SidechainTap::PostFader));
        // Deleting the source, and undoing it.
        f.editor().deleteTracks({group});
        QCOMPARE(sidechain(pad, keyed), QStringLiteral("none"));
        f.stack().undo();
        QCOMPARE(sidechain(pad, keyed), expect(group, sub::SidechainTap::PostFader));
        // A project loaded with a device keyed by a track listed after its own.
        const QString later = f.editor().addAudioTrack();
        f.editor().setDeviceSidechain(pad, keyed, Sidechain{later, kPreFader});
        loadInto(project, projectToJson(project));
        QVERIFY(f.bridge().pluginPending(keyed));  // (its plug-in loads after the project: with its sidechain)
        f.bridge().loadPendingPlugins();
        QCOMPARE(sidechain(pad, keyed), expect(later, sub::SidechainTap::PreFader));
        // Before the source's devices; a MIDI track's own audio is its instrument's.
        f.editor().setDeviceSidechain(pad, keyed, Sidechain{later, kPreFx});
        loadInto(project, projectToJson(project));
        QCOMPARE(project.device(pad, keyed).sidechain, std::optional<Sidechain>(Sidechain{later, kPreFx}));
        f.bridge().loadPendingPlugins();
        QCOMPARE(sidechain(pad, keyed), expect(later, sub::SidechainTap::PreFx));
        const QString keys = f.editor().addMidiTrack();
        f.editor().setDeviceSidechain(pad, keyed, Sidechain{keys, kPreFx});
        QCOMPARE(sidechain(pad, keyed),
                 expect(keys, sub::SidechainTap::AfterDevice, f.deviceOf(keys, project.track(keys).devices[0].id)));
        f.editor().addDevice(keys, QStringLiteral("synth"));  // another instrument, in its place
        QCOMPARE(sidechain(pad, keyed),
                 expect(keys, sub::SidechainTap::AfterDevice, f.deviceOf(keys, project.track(keys).devices[0].id)));
    }

    // --- test_racks_model.py ---

    void theEngineFollowsRacks() {
        Bridged f;
        TempDir dir;
        Project& p = f.project();
        QUndoStack& stack = f.stack();
        const QString track = f.noiseTrack(dir);
        const auto raw = f.renderBeats();  // the clip, nothing in its way
        const QString a = f.editor().addDevice(track, QStringLiteral("utility"));
        const QString b = f.editor().addDevice(track, QStringLiteral("utility"));
        f.editor().setDeviceParam(track, a, QStringLiteral("gain"), -6.0);
        f.renderBeats();  // (the gain glides to -6 dB, once)
        const auto plain = f.renderBeats();
        const QString pa = f.pid(track, a), pb = f.pid(track, b);
        const QString rack = f.editor().groupDevices(track, {a, b});  // the same processors, in the rack's chain
        QString pr = f.pid(track, rack);
        QCOMPARE(f.engineTree(track), QStringLiteral("%1[%2 %3]").arg(pr, pa, pb));
        QVERIFY(f.renderBeats() == plain);  // one chain: as without the rack
        const QString wet = f.editor().tryAddRackChain(track, rack);  // an empty chain beside it: its input
        QVERIFY(allClose(f.renderBeats(), plus(plain, raw), 1e-6));
        f.editor().setChainParam(track, wet, ChainField::Mute, 1.0);
        QVERIFY(f.renderBeats() == plain);
        f.editor().setChainParam(track, wet, ChainField::Mute, 0.0);
        f.editor().setChainParam(track, wet, ChainField::Solo, 1.0);
        QVERIFY(allClose(f.renderBeats(), raw, 1e-6));
        f.editor().setChainParam(track, wet, ChainField::Solo, 0.0);
        f.editor().setChainParam(track, wet, ChainField::VolumeDb, -120.0);  // the faders' floor: silent
        QVERIFY(allClose(f.renderBeats(), plain, 1e-6));
        // Moving a device out of the rack, and undoing everything: the same processors throughout.
        f.editor().moveDevices(track, {b}, 2);
        QCOMPARE(f.engineTree(track), QStringLiteral("%1[%2|] %3").arg(pr, pa, pb));
        while (p.hasDevice(track, rack)) stack.undo();
        QCOMPARE(f.engineTree(track), QStringLiteral("%1 %2").arg(pa, pb));
        QVERIFY(f.renderBeats() == plain);
        while (stack.canRedo()) stack.redo();
        pr = f.pid(track, rack);  // (a new rack: it wasn't kept)
        QCOMPARE(f.engineTree(track), QStringLiteral("%1[%2|] %3").arg(pr, pa, pb));
        // The rack to another track, with everything in it; then deleted, and back.
        const QString other = f.editor().addAudioTrack();
        f.editor().moveDevicesToTrack(track, {rack}, other);
        QCOMPARE(f.engineTree(other), QStringLiteral("%1[%2|]").arg(pr, pa));
        QCOMPARE(f.engineTree(track), pb);
        f.editor().removeDevice(other, rack);
        QCOMPARE(f.engineTree(other), QString());
        QVERIFY(!f.bridge().engineDeviceId(other, a));
        stack.undo();
        QCOMPARE(f.engineTree(other), QStringLiteral("%1[%2|]").arg(f.pid(other, rack), f.pid(other, a)));
    }

    void automationOfNestedDevicesAndChainFaders() {
        Bridged f;
        TempDir dir;
        EngineBridge& bridge = f.bridge();
        const QString track = f.noiseTrack(dir);
        const QString a = f.editor().addDevice(track, QStringLiteral("utility"));
        const QString rack = f.editor().groupDevices(track, {a});
        const Chain chain = f.project().device(track, rack).chains[0];
        const QString key = automation::chainKey(rack, chain.id, automation::kChainVolume);
        QVERIFY(bridge.canAutomate(track, key) && bridge.canAutomate(track, automation::deviceKey(a, QStringLiteral("gain"))));
        QCOMPARE(bridge.paramSpec(track, key)->name, chain.name + QStringLiteral(" Volume"));
        QCOMPARE(bridge.ownValue(track, key), std::optional<double>(0.0));
        f.editor().setEnvelope(track, key, test::env({{0.0, 0.0}}));  // silent
        QVERIFY(bridge.isAutomated(track, key));
        QVERIFY(test::peak(f.renderBeats()) == 0.f);
        f.editor().setChainParam(track, chain.id, ChainField::VolumeDb, -6.0);  // by hand: its automation stops
        QVERIFY(bridge.isOverridden(track, key));
        QVERIFY(test::peak(f.renderBeats()) > 0.1f);
        bridge.reEnableAutomation(track);
        QVERIFY(test::peak(f.renderBeats()) == 0.f);
        f.editor().clearEnvelope(track, key);
        const QString gain = automation::deviceKey(a, QStringLiteral("gain"));
        f.editor().setEnvelope(track, gain, test::env({{0.0, 0.0}}));  // -60 dB
        QVERIFY(test::peak(f.renderBeats(), 2 * (kRate / 20)) < 0.01f);  // (after the utility's gain glides there)
    }

    void macrosReachTheEngine() {
        const auto ref = testPlugin(QStringLiteral("SUB Test Effect"));
        if (!ref) QSKIP("test plug-ins not built");
        Bridged f;
        TempDir dir;
        const QString track = f.noiseTrack(dir);
        const QString fx = f.editor().addDevice(track, kPluginKind, -1, ref);
        const QString rack = f.editor().groupDevices(track, {fx});
        const quint32 processor = f.deviceOf(track, fx);
        const sub::ParamInfo gain = f.engine.processorParams(processor)[0];  // SUB Test Effect's gain (0..1: 0..2 times)
        f.editor().mapMacro(track, rack, 0, fx, QString::fromStdString(gain.id));
        f.editor().setMacro(track, rack, 0, 0.25);
        QVERIFY(near(f.engine.processorParam(processor, 0), gain.fromNormalized(0.25f), 1e-6));
        f.stack().undo();
        QVERIFY(near(f.engine.processorParam(processor, 0), 0.5, 1e-6));  // as it was (the plug-in's default)
    }

    void aSavedRackLoadsAndRendersTheSame() {
        const auto ref = testPlugin(QStringLiteral("SUB Test Effect"));
        if (!ref) QSKIP("test plug-ins not built");
        Bridged f;
        TempDir dir;
        Project& p = f.project();
        const QString track = f.noiseTrack(dir);
        const QString fx = f.editor().addDevice(track, kPluginKind, -1, ref);
        f.engine.setProcessorParam(f.deviceOf(track, fx), 0, 0.3f);  // in the plug-in only
        const QString gain = f.editor().addDevice(track, QStringLiteral("utility"));
        const QString rack = f.editor().groupDevices(track, {fx});
        const QString wet = f.editor().tryAddRackChain(track, rack);
        f.editor().moveDevices(track, {gain}, 0, wet);
        f.editor().setChainParam(track, wet, ChainField::Pan, -0.5);
        f.editor().mapMacro(track, rack, 0, gain, QStringLiteral("gain"));
        f.editor().setMacro(track, rack, 0, 0.6);
        const auto original = f.renderBeats();
        f.bridge().storePluginStates();  // (as saving does)
        const QString path = dir.path(QStringLiteral("rack.gilpreset"));
        savePreset(p.device(track, rack), path);
        std::vector<std::pair<QString, QString>> loaded;
        for (int i = 0; i < 2; ++i) {
            const QString copyTrack = f.noiseTrack(dir);
            const Device device = loadPreset(path);
            QVERIFY(f.editor().insertDevice(copyTrack, device));
            loaded.emplace_back(copyTrack, device.id);
            f.solo(copyTrack);
            QVERIFY(allClose(f.renderBeats(), original, 1e-6));  // alone, it sounds as the original
        }
        const auto& [t1, r1] = loaded[0];
        const auto& [t2, r2] = loaded[1];
        QVERIFY(r1 != r2 && r1 != rack);
        Q_UNUSED(t1);
        // Its macros move its own devices.
        const QString innerGain = p.device(t2, r2).chains[1].devices[0].id;
        f.editor().setMacro(t2, r2, 0, 0.0);
        QCOMPARE(p.device(t2, innerGain).params.value(QStringLiteral("gain")), -60.0);
        QVERIFY(near(p.device(track, gain).params.value(QStringLiteral("gain")), -60 + 0.6 * 84, 1e-9));
    }

    // --- test_presets.py ---

    void aPluginPresetRendersAsTheDeviceItWasSavedFrom() {
        const auto ref = testPlugin(QStringLiteral("SUB Test Effect"));
        if (!ref) QSKIP("test plug-ins not built");
        Bridged f;
        TempDir dir;
        const QString original = f.noiseTrack(dir);
        const QString fx = f.editor().addDevice(original, kPluginKind, -1, ref);
        f.engine.setProcessorParam(f.deviceOf(original, fx), 0, 0.3f);  // in the plug-in only
        const auto expected = f.renderBeats();
        f.bridge().storePluginStates();  // (as the save button does)
        const QString path = saveToLibrary(f.project().device(original, fx), QStringLiteral("Quiet"), dir.path(QStringLiteral("Presets")));

        // As a new device...
        const QString added = f.noiseTrack(dir);
        const Device device = loadPreset(path);
        QVERIFY(f.editor().insertDevice(added, device));
        QVERIFY(device.id != fx);
        QVERIFY(near(f.engine.processorParam(f.deviceOf(added, device.id), 0), 0.3, 1e-6));
        f.solo(added);
        QVERIFY(allClose(f.renderBeats(), expected, 1e-6));

        // ...and into a device of the same plug-in, undoably.
        const QString into = f.noiseTrack(dir);
        const QString target = f.editor().addDevice(into, kPluginKind, -1, ref);
        const quint32 processor = f.deviceOf(into, target);
        f.solo(into);
        const auto untouched = f.renderBeats();
        f.bridge().storePluginStates(QSet<QString>{target});
        QVERIFY(f.editor().loadPresetInto(into, target, loadPreset(path), QStringLiteral("Load Preset Quiet")));
        QCOMPARE(f.deviceOf(into, target), processor);  // the same plug-in, with the preset's state
        QVERIFY(near(f.engine.processorParam(processor, 0), 0.3, 1e-6));
        QVERIFY(allClose(f.renderBeats(), expected, 1e-6));
        f.stack().undo();
        QVERIFY(near(f.engine.processorParam(processor, 0), 0.5, 1e-6));
        QVERIFY(allClose(f.renderBeats(), untouched, 1e-6));
        f.stack().redo();
        QVERIFY(near(f.engine.processorParam(processor, 0), 0.3, 1e-6));
    }

    void aBuiltInPresetLoadedIntoADeviceReachesTheEngine() {
        Bridged f;
        TempDir dir;
        const QString track = f.noiseTrack(dir);
        const QString source = f.editor().addDevice(track, QStringLiteral("utility"));
        f.editor().setDeviceParam(track, source, QStringLiteral("gain"), -20.0);
        const auto expected = f.settled();
        const QString path = saveToLibrary(f.project().device(track, source), QStringLiteral("Down"), dir.path(QStringLiteral("Presets")));
        f.editor().removeDevice(track, source);
        const QString target = f.editor().addDevice(track, QStringLiteral("utility"));
        const auto untouched = f.settled();
        QVERIFY(f.editor().loadPresetInto(track, target, loadPreset(path)));
        QVERIFY(allClose(f.settled(), expected, 1e-6));
        f.stack().undo();
        QVERIFY(allClose(f.settled(), untouched, 1e-6));
    }

    void aPresetOfAMissingPluginLoadsAsAMissingDevice() {
        Bridged f;
        TempDir dir;
        Project& p = f.project();
        const QString track = f.noiseTrack(dir);
        PluginRef gone;
        gone.uid = QString(32, u'0');
        gone.name = QStringLiteral("Long Gone");
        gone.vendor = QStringLiteral("Nobody");
        gone.path = QStringLiteral("C:\\nowhere\\Gone.vst3");
        Device missing;
        missing.id = QStringLiteral("m");
        missing.kind = kPluginKind;
        missing.plugin = gone;
        missing.state = QStringLiteral("AAAA");
        const Device device = presetDevice(deviceToPreset(missing));
        QVERIFY(f.editor().insertDevice(track, device));
        QCOMPARE(p.device(track, device.id).plugin, std::optional<PluginRef>(gone));  // kept, as it was
        QVERIFY(!f.bridge().engineDeviceId(track, device.id));
        QVERIFY(f.bridge().pluginError(device.id).contains(QStringLiteral("Long Gone is not installed")));

        const QString gain = f.editor().addDevice(track, QStringLiteral("utility"));
        f.editor().setDeviceParam(track, gain, QStringLiteral("gain"), -20.0);
        const QString unknown = f.editor().addDevice(track, QStringLiteral("utility"));
        const QString rack = f.editor().groupDevices(track, {gain, unknown});
        QJsonObject data = deviceToPreset(p.device(track, rack));
        QJsonObject rackData = data.value(QStringLiteral("device")).toObject();
        QJsonArray chains = rackData.value(QStringLiteral("chains")).toArray();
        QJsonObject chain = chains.at(0).toObject();
        QJsonArray devices = chain.value(QStringLiteral("devices")).toArray();
        devices.insert(1, deviceToPreset(missing).value(QStringLiteral("device")));
        QJsonObject later = devices.at(2).toObject();
        later.insert(QStringLiteral("kind"), QStringLiteral("a device of a later version"));
        devices.replace(2, later);
        chain.insert(QStringLiteral("devices"), devices);
        chains.replace(0, chain);
        rackData.insert(QStringLiteral("chains"), chains);
        data.insert(QStringLiteral("device"), rackData);
        f.editor().removeDevices(track, {rack, device.id});
        const auto expectedQuiet = scaled(f.settled(), 0.1f);  // -20 dB
        const Device loaded = presetDevice(data);
        QVERIFY(f.editor().insertDevice(track, loaded));
        const auto& inside = loaded.chains[0].devices;
        QVERIFY(f.bridge().engineDeviceId(track, loaded.id));
        QVERIFY(f.bridge().engineDeviceId(track, inside[0].id));
        QVERIFY(!f.bridge().engineDeviceId(track, inside[1].id));
        QVERIFY(!f.bridge().engineDeviceId(track, inside[2].id));
        QVERIFY(f.bridge().pluginError(inside[2].id).contains(QStringLiteral("not a device this version")));
        QVERIFY(allClose(f.settled(), expectedQuiet, 1e-4));
    }

    void aNewPluginStartsWithItsDefaultState() {
        const auto ref = testPlugin(QStringLiteral("SUB Test Effect"));
        if (!ref) QSKIP("test plug-ins not built");
        Bridged f;
        TempDir dir;
        const QString root = dir.path(QStringLiteral("Presets"));
        f.editor().setDeviceDefaults([root](const QString& kind, const std::optional<PluginRef>& plugin) {
            return defaultDevice(kind, plugin, root);
        });
        const QString track = f.editor().addAudioTrack();
        const QString fx = f.editor().addDevice(track, kPluginKind, -1, ref);
        f.engine.setProcessorParam(f.deviceOf(track, fx), 0, 0.3f);
        f.bridge().storePluginStates(QSet<QString>{fx});
        saveDefault(f.project().device(track, fx), root);
        const QString fresh = f.editor().addDevice(track, kPluginKind, -1, ref);
        QVERIFY(near(f.engine.processorParam(f.deviceOf(track, fresh), 0), 0.3, 1e-6));
    }

    // --- test_freeze_model.py ---

    void aFrozenTrackPlaysItsFrozenAudioWithoutItsDevices() {
        TempDir dir;
        const test::ScopedEnv recordings("SUBSTATION_RECORDINGS", dir.path(QStringLiteral("Recordings")));
        Bridged f;
        Project& p = f.project();
        const auto level = [&] {
            const auto out = f.engine.renderOffline(0.0, 4000);
            return std::vector<float>(out.end() - 200, out.end());
        };
        const auto allNear = [](const std::vector<float>& samples, float value) {
            return std::all_of(samples.begin(), samples.end(), [&](float s) { return std::abs(s - value) < 1e-4f; });
        };
        const QString track = f.clipTrack(dcWav(dir), 1.0, QStringLiteral("A"));
        const QString utility = f.editor().addDevice(track, QStringLiteral("utility"));
        f.editor().setDeviceParam(track, utility, QStringLiteral("gain"), -6.0206);  // halves it
        f.bridge().waitForDeviceStates();
        level();
        QVERIFY(allNear(level(), 0.25f));

        const Freeze freeze = f.bridge().renderFreeze(track);
        QCOMPARE(freeze.tempo, p.tempo());
        QVERIFY(freeze.durationSec >= p.endBeat() / 2);
        QVERIFY(QFileInfo::exists(dir.path(QStringLiteral("Recordings/Freeze"))));
        OrderedMap<QString, Freeze> freezes;
        freezes.insert(track, freeze);
        QCOMPARE(f.editor().freezeTracks(freezes), QStringList{track});
        QVERIFY(!f.bridge().engineDeviceId(track, utility));  // unloaded
        QVERIFY(f.engine.trackFrozen(f.trackOf(track)));
        QVERIFY(allNear(level(), 0.25f));
        f.editor().setTrackParam(track, TrackField::VolumeDb, -6.0206);  // its fader stays live
        QVERIFY(allNear(level(), 0.125f));

        f.editor().unfreezeTracks({track});  // its devices come back as they were
        QVERIFY(f.bridge().engineDeviceId(track, utility));
        f.bridge().waitForDeviceStates();
        level();
        QVERIFY(allNear(level(), 0.125f));

        f.stack().undo();  // frozen again, then flattened: its frozen audio as a clip
        QCOMPARE(f.editor().flattenTracks({track}), QStringList{track});
        QVERIFY(p.track(track).devices.empty());
        QVERIFY(allNear(level(), 0.125f));
    }

    void aFrozenMidiTrackPlaysItsFrozenAudio() {
        TempDir dir;
        const test::ScopedEnv recordings("SUBSTATION_RECORDINGS", dir.path(QStringLiteral("Recordings")));
        Bridged f;
        const QString track = f.editor().addMidiTrack(-1, QStringLiteral("Keys"));
        f.editor().commitClips(QStringLiteral("Add"),
                               {{track, {Clip::midi(QStringLiteral("m"), QStringLiteral("m"), 0.0, 2.0, 0.0, {Note{60, 0.0, 1.0}})}}});
        const auto before = f.render(kRate);
        QVERIFY(test::peak(before) > 0.01f);
        OrderedMap<QString, Freeze> freezes;
        freezes.insert(track, f.bridge().renderFreeze(track));
        f.editor().freezeTracks(freezes);
        QVERIFY(allClose(f.render(kRate), before, 1e-5));
        f.editor().unfreezeTracks({track});  // its notes again, not its frozen audio as well
        QVERIFY(allClose(f.render(kRate), before, 1e-5));
    }

    void nothingToFreeze() {
        Bridged f;
        const QString track = f.editor().addAudioTrack();
        try {
            f.bridge().renderFreeze(track);
            QFAIL("froze an empty arrangement");
        } catch (const EditError& error) {
            QVERIFY(error.message().contains(QStringLiteral("nothing to freeze")));
        }
    }

    void aPluginComesBackAsItWas() {
        const auto ref = testPlugin(QStringLiteral("SUB Test Effect"));
        if (!ref) QSKIP("test plug-ins not built");
        TempDir dir;
        const test::ScopedEnv recordings("SUBSTATION_RECORDINGS", dir.path(QStringLiteral("Recordings")));
        Bridged f;
        const QString track = f.clipTrack(dcWav(dir), 1.0, QStringLiteral("A"));
        const QString effect = f.editor().addDevice(track, kPluginKind, -1, ref);
        f.engine.setProcessorParam(f.deviceOf(track, effect), 0, 0.25f);  // as if in its own editor: only the plug-in knows
        OrderedMap<QString, Freeze> freezes;
        freezes.insert(track, f.bridge().renderFreeze(track));
        f.editor().freezeTracks(freezes);
        QVERIFY(!f.bridge().engineDeviceId(track, effect));
        QVERIFY(f.project().track(track).devices[0].state.has_value());  // (stored with the project meanwhile)
        f.editor().unfreezeTracks({track});
        const auto processor = f.bridge().engineDeviceId(track, effect);
        QVERIFY(processor && near(f.engine.processorParam(*processor, 0), 0.25, 1e-6));
    }

    // (test_renders_no_track_plays_are_deleted: test_session_renders.cpp, through the session's freezing.)

    // --- test_midi_model.py ---

    void aMidiClipPlaysTheNotesStartingInItsWindow() {
        constexpr int c = 60, e = 64, g = 67;
        const Clip clip = Clip::midi(QStringLiteral("m"), QStringLiteral("m"), 8.0, 2.0, 1.0,
                                     {Note{c, 0.0, 4.0},    // starts before the window: not played (no chasing)
                                      Note{e, 1.0, 0.5},    // at the window start: plays at the clip start
                                      Note{g, 2.5, 1.0},    // runs past the window: cut at the clip end
                                      Note{c, 3.0, 1.0}});  // at the window end: not played
        Track track = test::makeTrack(QStringLiteral("t"), QStringLiteral("t"), kMidiKind);
        track.clips = {clip};
        const std::vector<sub::NoteDesc> notes = noteDescs(track);
        QCOMPARE(notes.size(), size_t{2});
        QCOMPARE(notes[0].startBeat, 8.0);
        QCOMPARE(notes[0].lengthBeats, 0.5);
        QCOMPARE(notes[0].key, e);
        QCOMPARE(notes[0].velocity, 100);
        QCOMPARE(notes[1].startBeat, 9.5);
        QCOMPARE(notes[1].lengthBeats, 0.5);
    }
};

QTEST_GUILESS_MAIN(TestSessionEngine)
#include "test_session_engine.moc"
