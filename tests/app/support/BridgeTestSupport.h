#pragma once
// What the engine bridge's tests share: a studio (an engine, a project, an undo
// stack and the bridge between them), the edits the tests make the way the
// ProjectEditor makes them (the model's undo commands, in the same order), the
// test plug-ins, and a window on the bridge's plug-in reports.

#include "audio/EngineBridge.h"
#include "model/Clip.h"
#include "model/Commands.h"
#include "model/Device.h"
#include "model/Project.h"
#include "model/Track.h"
#include "plugins/PluginInfo.h"

#include "Engine.h"

#include <QString>
#include <QStringList>
#include <QUndoStack>

#include <memory>
#include <optional>
#include <vector>

namespace sub::app {

// The tests' window on the bridge's insides (EngineBridge's friend).
class BridgeTestAccess {
public:
    // Reports the next pollPlugins() takes, as if the plug-ins had sent them.
    static void injectEvents(EngineBridge& bridge, const std::vector<sub::ProcessorEventRecord>& events);
    // The plug-ins waiting to load are loaded by hand from here (the timer stopped).
    static void stopPluginTimer(EngineBridge& bridge);
    static void loadNextPlugin(EngineBridge& bridge);
    static void pollMeters(EngineBridge& bridge);
    // As if a plug-in's (or driver's) call were running a message loop.
    static void setBusy(EngineBridge& bridge, bool busy);
};

}  // namespace sub::app

namespace sub::app::test {

// The test VST3 bundle (tests/vst3_plugins), built with the tests; empty if it wasn't.
// (Each test's own: only the tests have SUBSTATION_TEST_PLUGINS_BUNDLE.)
static inline QString testPluginsBundle() {
#ifdef SUBSTATION_TEST_PLUGINS_BUNDLE
    return QStringLiteral(SUBSTATION_TEST_PLUGINS_BUNDLE);
#else
    return {};
#endif
}
bool haveTestPlugins(const QString& bundle);
// One of the test plug-ins ("SUB Test Effect", "SUB Test Synth", "SUB Test
// Sidechain", "SUB Test Mono") as a device's PluginRef; none without them.
std::optional<PluginRef> testPlugin(const QString& bundle, const QString& name);
// The test plug-ins as the scanner lists them (EngineBridge::setKnownPlugins takes them).
std::vector<PluginInfo> testPluginInfos(const QString& bundle);

// SUB Test Synth's parameters, and SUB Test Effect's (by index).
inline constexpr int kSynthGain = 0;
inline constexpr int kSynthWave = 1;
inline constexpr int kEffectGain = 0;

// What the editor does, as it does it: the model's undo commands on the stack.
class Edits {
public:
    Edits(Project& project, QUndoStack& stack) : project_(project), stack_(stack) {}

    QString addAudioTrack(const QString& name = {}, const std::optional<QString>& parent = std::nullopt);
    // A MIDI track with this instrument (none: none; by default a synth).
    QString addMidiTrack(const QString& name = {}, const std::optional<QString>& instrument = QStringLiteral("synth"),
                         const std::optional<PluginRef>& plugin = std::nullopt);
    QString addReturnTrack(const QString& name = {});
    void setClips(const QString& trackId, std::vector<Clip> clips);
    void setTrack(const QString& trackId, TrackField field, const TrackValue& value, const QString& mergeKey = {});
    // A track's input: device channels, or another track's output (kMaster: the master's).
    void setInput(const QString& trackId, const std::vector<int>& channels, const std::optional<QString>& source);
    void setSettings(const SettingsValues& values);
    void setSend(const QString& trackId, const QString& returnId, std::optional<double> levelDb,
                 std::optional<bool> preFader = std::nullopt);
    void setEnvelope(const QString& owner, const QString& key, const Envelope& points);
    // A group holding these tracks, where the first was (one undo step); its id.
    QString groupTracks(const QStringList& trackIds);
    // Tracks (and what is in them) to before the track at `index`, into group `parent`.
    void moveTracks(const QStringList& trackIds, int index, const std::optional<QString>& parent);
    // Tracks and returns go (a group with what is in it, a return with the sends
    // into it), the inputs and sidechains they were the source of first. One step.
    void deleteTracks(const QStringList& trackIds);

    // A device on a track (at `index`, or last; an instrument stays first); its id.
    QString addDevice(const QString& trackId, const QString& kind, const std::optional<PluginRef>& plugin = std::nullopt,
                      std::optional<int> index = std::nullopt);
    // A device as it is (a preset loaded, with its own ids), last.
    void insertDevice(const QString& trackId, const Device& device);
    void removeDevices(const QString& trackId, const QStringList& deviceIds);
    // Devices to `index` of a chain of the same track (none: its own chain).
    void moveDevices(const QString& trackId, const QStringList& deviceIds, int index,
                     const std::optional<QString>& chain = std::nullopt);
    // Devices to the end of another track's chain, their automation along. One step.
    void moveDevicesToTrack(const QString& trackId, const QStringList& deviceIds, const QString& toTrackId);
    // These devices into a new rack, in one chain, where the first was; its id.
    QString groupDevices(const QString& trackId, const QStringList& deviceIds);
    // A new, empty chain of a rack, last; its id.
    QString addRackChain(const QString& trackId, const QString& rackId);
    void setChain(const QString& trackId, const QString& chainId, ChainField field, const ChainValue& value);
    void setDeviceParam(const QString& trackId, const QString& deviceId, const QString& paramId, double value);
    void setDeviceParams(const QString& trackId, const QMap<DeviceParam, double>& values);
    void setDeviceSidechain(const QString& trackId, const QString& deviceId, const std::optional<Sidechain>& sidechain);
    void setDeviceState(const QString& trackId, const QString& deviceId, const std::optional<QString>& state);
    void setMacros(const QString& trackId, const QString& rackId, const std::vector<MacroMapping>& macros);
    void freeze(const QString& trackId, const Freeze& freeze);
    void unfreeze(const QString& trackId);

private:
    void arrange(const TrackTree& tree, const QString& text);

    Project& project_;
    QUndoStack& stack_;
};

// A project, its undo stack, an engine (no device) and the bridge between them.
// Shut down (plug-ins unloaded) when it goes.
struct Studio {
    explicit Studio(bool clipFades = false);
    ~Studio();

    sub::Engine engine;
    Project project;
    QUndoStack stack;
    std::unique_ptr<EngineBridge> bridge;
    Edits edit{project, stack};

    // `frames` of the arrangement from beat 0, rendered offline (interleaved stereo).
    std::vector<float> render(int64_t frames, double startBeat = 0.0);
    // The left channel of the last frame of `frames` rendered.
    float level(int64_t frames = 4000);
    // An audio track playing `path` (decoded first) from beat 0 for `seconds`; its id.
    QString clipTrack(const QString& path, double seconds = 1.0, const QString& name = {});
};

// A clip of `path` from `startBeat`, `seconds` long.
Clip audioClip(const QString& id, const QString& path, double startBeat = 0.0, double seconds = 1.0);
// The largest absolute sample.
float peak(const std::vector<float>& samples, size_t from = 0);

}  // namespace sub::app::test
