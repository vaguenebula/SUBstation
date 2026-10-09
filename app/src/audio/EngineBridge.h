#pragma once
// Keeps the audio engine in step with the project model, and feeds the UI with
// what the engine knows (playhead, meters, plug-in reports, device events)
// through Qt signals. The engine's mirror of the project: only the bridge talks
// to the engine about the project; the model never does. It listens to the
// Project's signals and reads the project by id (it holds no model objects
// across calls); the project and the engine outlive it.
//
// Everything here runs on the Qt main thread except decoding audio files (a
// thread pool of two: results come back on the main thread) and restoring
// built-in devices' states (a pool of one, in order). It never waits on the
// audio thread: the engine publishes immutable snapshots, and the bridge reads
// atomics and queues. It only pushes what changed: it keeps the last value it
// gave the engine for mixers, inputs, outputs, sends, sidechains, device on/off
// and rack chain mixers, and compares before calling.
//
// Plug-ins: a device's engine processor lives as long as the device is in its
// chain, so a plug-in keeps its state (and open editor) when the chain around it
// changes. A device moved to another chain (another track's, or a rack's) takes
// its processor along (one engine moveProcessor, nothing loads again); a rack
// moves with its chains and everything in them.
//
// Racks: a rack is an engine rack, its chains engine chains of it (with their
// faders), and the devices in them processors there, as on a track's own chain.
// A rack's macros are the model's business (they set parameters there); the
// engine has none. A macro's automation plays as the parameters mapped to it:
// each of them plays the macro's envelope over its range, instead of its own
// (unless overridden: changed by hand, as any automated target). When a
// plug-in device goes away (deleted, or its track), its state is kept here, so
// undo brings it back as it was. Edits made in a plug-in's own editor come back
// from the engine as pluginParamEdited, for the undo stack; only while that
// editor shows (a plug-in may report its own changes as edits, as some do while
// their state is restored: a copy pasted, a track duplicated, an undo).
//
// Audio devices (WASAPI or ASIO) open as the preferences describe. An ASIO
// driver whose settings change (in its control panel, or its clock) asks to be
// reset; the bridge then opens it again, with its new settings.
//
// The master is a track to the engine as to the model (engine track id
// sub::Engine::kMaster): its devices, mixer and automation go the same way as a
// track's.
//
// Groups are tracks to the engine too. It knows only where each track's output
// goes: into the engine track of the group it is in, or the master.
//
// Return tracks are engine tracks as well, going to the master, and a track's
// sends are engine sends into them (at the send's level, before or after the
// fader). A send automated without having been set yet is made, silent, so that
// its automation plays. Sends are pushed as each track changes, those going away
// first, so that no step closes a cycle.
//
// Sidechains: a device's sidechain is the engine processor's, from the source's
// engine track, tapped as the model says (after a device that has left the
// source: before the fader). They are pushed whenever devices or routes change,
// those changing taken away first; one the engine refuses for now (a cycle with a
// route another change hasn't undone yet) comes with that change. A processor
// moving to another track gives up its sidechain in the engine until it is there.
//
// Automation: every envelope of a track (or the master) goes to the engine,
// which plays it; its target follows it and the value the model holds for it
// (set by hand) counts again when the envelope goes. Changing an automated
// target by hand overrides its automation, as in Ableton: the engine stops
// playing that envelope until automation is re-enabled. Parameters of every kind
// are described to the UI as ParamSpecs (model/ParamSpec.h). Overrides are the
// bridge's: not saved, not undone, cleared on reset.
//
// Recording: armed audio tracks with an input record when recording starts; the
// engine writes each take to a WAV file in the recordings folder (the project's
// "Recordings" folder once it is saved). While it records, the bridge collects
// each take's peaks (liveTakes) for the arrangement's live waveform. When the
// recording ends (stopped, or a device change or a locate ended it) the takes
// come out as takesRecorded, for one undo step that adds them as clips. A track
// given an input the device hasn't open gets it: the device opens again with
// that input too (ASIO). A track taking its input from another track's output
// (or the master's: resampling) records that, in stereo, placed where it was heard.
//
// MIDI input: every MIDI input connected is opened, but those turned off in the
// preferences. MIDI tracks hear their MIDI input (every input, or one, on every
// channel or one) while monitored, and record it when armed: their takes come
// back with the notes played, for MIDI clips. Their live takes hold the notes so
// far. The computer MIDI keyboard is one more MIDI input, kComputerKeyboard,
// always there.
//
// Freezing: renderFreeze renders a track's signal before its fader (a group's
// bus, a return's) from the timeline's start into a WAV file in the freeze
// folder (the project's "Freeze" folder once it is saved), with its tail. A
// frozen track is frozen in the engine too: it plays its frozen audio as its
// clips (its segments: Freeze::playing; at first one playing all of it), and
// its devices' processors go (a plug-in's state is kept, as for a device
// deleted, and comes back when it is unfrozen). The tracks in a frozen group
// keep theirs, but the engine doesn't render them. A time selection edited
// over a frozen track changes its segments (clipsChanged): they play again.
//
// Opening a project: its plug-ins load after it shows, one at a time between
// the UI's events (each in a turn of the event loop of its own, kPluginGapMs
// apart); pluginsLoading reports how far they got. The project plays without
// them meanwhile. Renders wait for them (and for built-in devices' states:
// waitForDeviceStates, devicesReady). They load on the UI thread all the same:
// plug-ins must be created and set up there (VST3 says so, and JUCE plug-ins
// take the thread that creates them for their message thread), and some hang
// when loaded on another. What takes long is each plug-in, not the project.
//
// Renders in the background: startFreeze and startExport return a RenderTask at
// once (RenderTask.h); the UI shows its progress, may cancel it, and finishes it
// (finishFreeze gives the frozen audio). Meanwhile live output is silent, and
// the device isn't reopened. No nested event loops: the UI follows the task's
// signals.
//
// Reversing: a reversed clip plays a reversed copy of its file, written once
// (reversedCopy finds it again, after reopening too) into the reversed folder
// (the project's "Reversed" folder once it is saved), on a thread of its own
// (startReversed: a ReverseJob, followed like a render).
//
// Previews: while a clip is dragged, previewClips hands the engine what the drag
// would make of the tracks' clips, so they are heard where they are going; the
// model changes only when the drag ends (endClipPreview puts the model's clips
// back if it changed nothing).
//
// Re-entrancy: a plug-in (or a driver) may run a message loop inside a call (a
// licence dialog, a control panel) that calls back into the UI. Around such
// calls the bridge counts busy, and while it is busy it doesn't dispatch plug-in
// reports or device events (they must not change the chains under a call in
// progress), nor load plug-ins that wait.
//
// This header includes none of the engine's: the UI may include it (and
// BridgeTypes.h, Waveform.h, LiveTake.h, RenderTask.h, ReverseJob.h,
// AudioSettings.h, AudioFiles.h, EqResponse.h). EngineDescs.h and
// BridgePrivate.h are the application layer's own.

#include "audio/AudioSettings.h"
#include "audio/BridgeTypes.h"
#include "audio/LiveTake.h"
#include "audio/RenderTask.h"
#include "audio/ReverseJob.h"
#include "audio/Waveform.h"
#include "model/Automation.h"
#include "model/Clip.h"
#include "model/Device.h"
#include "model/ParamSpec.h"
#include "model/RecordedTake.h"
#include "model/Track.h"

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace sub {
class AudioSource;
class Engine;
enum class SidechainTap : std::uint8_t;
struct AutomationLaneDesc;
struct ParamInfo;
struct ProcessorEventRecord;
}  // namespace sub

namespace sub::app {

class Project;
struct PluginInfo;

// The MIDI input the computer keyboard plays into.
inline const QString kComputerKeyboard = QStringLiteral("Computer Keyboard");
// A frozen track renders on past the arrangement's end this long, while it sounds.
inline constexpr double kFreezeTailSeconds = 10.0;
// Plug-in editors of tracks not shown are hidden but keep running (animating,
// messaging their processors); this many at most, then the ones hidden longest close.
inline constexpr int kMaxHiddenEditors = 8;
// Between two plug-ins loading after a project opened: the window's turn (it
// paints, takes the mouse and keys).
inline constexpr int kPluginGapMs = 20;
// A plug-in's call is running a message loop: the next one loads after it.
inline constexpr int kPluginRetryMs = 50;
inline constexpr int kPositionPollMs = 16;  // the playhead, about 60 times a second
inline constexpr int kMeterPollMs = 33;     // meters, recording, plug-ins, the device: about 30 times a second

class BridgeTestAccess;

class EngineBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(bool playing READ isPlaying NOTIFY transportChanged)
    Q_PROPERTY(bool countingIn READ isCountingIn NOTIFY countingInChanged)
    Q_PROPERTY(bool recording READ isRecording NOTIFY recordingChanged)
    Q_PROPERTY(bool metronome READ metronome WRITE setMetronome NOTIFY metronomeChanged)
    Q_PROPERTY(double cpuLoad READ cpuLoad NOTIFY metersUpdated)
    Q_PROPERTY(double sampleRate READ sampleRate NOTIFY deviceChanged)
    Q_PROPERTY(sub::app::AudioDeviceStatus deviceStatus READ deviceStatus NOTIFY deviceChanged)
    Q_PROPERTY(sub::app::AudioDeviceCaps deviceCapabilities READ deviceCapabilities NOTIFY deviceChanged)
    // The master output for the oscilloscope: samples played so far (it stands
    // still while no device runs); scopeSamples() gives the newest.
    Q_PROPERTY(quint64 scopeWritten READ scopeWritten)
    Q_PROPERTY(int pluginsPending READ pluginsPending NOTIFY pluginsPendingChanged)

public:
    EngineBridge(sub::Engine& engine, Project* project, QObject* parent = nullptr);
    ~EngineBridge() override;
    EngineBridge(const EngineBridge&) = delete;
    EngineBridge& operator=(const EngineBridge&) = delete;

    sub::Engine& engine() const { return engine_; }
    Project* project() const { return project_; }

    // Unloads every plug-in now, while the application is still whole (not
    // whenever the engine goes): plug-ins still waiting to load are forgotten,
    // every editor closes, built-in devices' states finish restoring, every
    // engine track goes, MIDI inputs close, and removed plug-ins are destroyed.
    void shutdown();

    // --- Ids ------------------------------------------------------------------------
    // A track's (a group's, a return's; kMaster: the master's) engine track.
    std::optional<quint32> engineTrackId(const QString& trackId) const;
    // A device's processor, if it is on that track (in a rack too) and loaded.
    std::optional<quint32> engineDeviceId(const QString& trackId, const QString& deviceId) const;
    // A rack chain's engine chain.
    std::optional<quint32> engineChainId(const QString& chainId) const;

    // --- Transport and previews -------------------------------------------------------
    bool isPlaying() const;
    Q_INVOKABLE void play();
    // Stops playing (and ends a recording).
    Q_INVOKABLE void stop();
    Q_INVOKABLE void locate(double beat);
    double position() const;  // the playhead, in beats
    bool isCountingIn() const;
    bool metronome() const;
    void setMetronome(bool enabled);
    // Plays a file through the master (decoded first: a later preview or stop
    // cancels a preview still loading).
    Q_INVOKABLE void previewFile(const QString& path);
    Q_INVOKABLE void stopPreview();
    // Plays a note on a MIDI track's instrument now; velocity 0 releases it.
    Q_INVOKABLE void previewNote(const QString& trackId, int pitch, int velocity);
    // Plays these clips on these tracks instead of the model's, until
    // endClipPreview() (or the next change to their clips): what a drag would
    // make of them, heard while it goes on. Frozen tracks play their frozen
    // audio: the segments of it in `frozenByTrack` (what the drag would make of
    // them), or as they are.
    void previewClips(const QMap<QString, std::vector<Clip>>& clipsByTrack,
                      const QMap<QString, std::vector<Clip>>& frozenByTrack = {});
    // The previewed tracks play the model's clips again.
    void endClipPreview();
    // Renders on as many threads as the preferences say (the engine's default unless chosen).
    void applyAudioThreads();
    // The threads chosen in the preferences: saved (0, not pinned, when it is
    // the engine's default) and applied.
    Q_INVOKABLE void chooseAudioThreads(int threads);
    int audioThreads() const;  // the engine's now
    static int defaultAudioThreads();  // one per core but one

    // --- Meters and the scope -----------------------------------------------------------
    // Track id (or kMaster) -> its meter, as of the last metersUpdated.
    const QMap<QString, MeterLevel>& meters() const;
    // Rack chain id -> its fader's meter.
    const QMap<QString, MeterLevel>& chainMeters() const;
    Q_INVOKABLE sub::app::MeterLevel trackMeter(const QString& trackId) const;
    Q_INVOKABLE sub::app::MeterLevel chainMeter(const QString& chainId) const;
    // The peak of each open input channel (as deviceStatus().inputChannels lists them) since the last call.
    Q_INVOKABLE QList<float> takeInputMeters();
    quint64 scopeWritten() const;
    // The newest `frames` samples of the master output (mono, oldest first; fewer if there aren't as many).
    Q_INVOKABLE QList<float> scopeSamples(int frames) const;
    double cpuLoad() const;  // the audio callback's time over its budget (0..1, more when late)
    double sampleRate() const;  // the engine's

    // --- Sources and waveforms ------------------------------------------------------------
    // Decodes `path` in the background (once, at the engine's rate); `then` runs
    // on the main thread when it is ready (at once if it is).
    void requestSource(const QString& path, std::function<void()> then);
    Q_INVOKABLE void requestSource(const QString& path) { requestSource(path, std::function<void()>()); }
    // The decoded file (null: not decoded, or not yet).
    std::shared_ptr<const sub::AudioSource> source(const QString& path) const;
    Q_INVOKABLE sub::app::Waveform waveform(const QString& path) const;
    Q_INVOKABLE bool isLoading(const QString& path) const;
    // Why a file couldn't be decoded ("": it could, or hasn't been tried).
    Q_INVOKABLE QString loadError(const QString& path) const;
    // Length and format from the file's header (cached); none if unreadable (and said so).
    std::optional<AudioFileInfo> fileInfo(const QString& path);
    // After a sample-rate change: every source decoded again.
    void refreshSources();

    // --- Devices and plug-ins -----------------------------------------------------------------
    // Device id -> why its plug-in (or built-in device) isn't loaded.
    const QMap<QString, QString>& pluginErrors() const;
    Q_INVOKABLE QString pluginError(const QString& deviceId) const;
    // The scanned plug-ins: lets projects find plug-ins that moved (by class
    // id). Devices whose plug-in wasn't found get another try.
    void setKnownPlugins(const std::vector<PluginInfo>& plugins);
    // Where a plug-in is now: where it was, or where the scan found it.
    std::optional<QString> pluginPath(const PluginRef& plugin) const;
    // A plug-in device's current state (a .vstpreset); none if it isn't loaded.
    std::optional<QByteArray> pluginState(const QString& trackId, const QString& deviceId);
    // Gives a loaded plug-in device a state (a .vstpreset read from a file) now,
    // as the plug-in's own (no undo step: the caller records one). "" if the
    // plug-in took it, else why not (another plug-in's settings, a broken file).
    QString applyPluginState(const QString& trackId, const QString& deviceId, const QByteArray& state);
    // Copies every loaded plug-in's state (or those of `deviceIds`) into the
    // model, for saving the project (or copying devices, duplicating tracks,
    // saving presets), and where a plug-in was found if it moved. Device::state
    // of a plug-in is stale until then.
    void storePluginStates(const std::optional<QSet<QString>>& deviceIds = std::nullopt);
    // Whether a device has a sidechain (aux) input (not while it isn't loaded).
    Q_INVOKABLE bool hasSidechainInput(const QString& trackId, const QString& deviceId);
    // A device's parameter as the engine describes it (none: not loaded, or no
    // such one): the editor's macros map through it.
    std::optional<sub::ParamInfo> deviceParamInfo(const QString& trackId, const QString& deviceId,
                                                  const QString& paramId);
    // What a device's editor shows: its processor's parameters (empty while it isn't loaded).
    QList<ProcessorParam> deviceParams(const QString& trackId, const QString& deviceId);
    // A parameter's value as the processor has it now (none: not loaded).
    std::optional<double> deviceParamValue(const QString& trackId, const QString& deviceId, int index);
    // A plug-in's text for a value of its parameter, with its unit.
    QString deviceParamText(const QString& trackId, const QString& deviceId, int index, double value);
    // A device's latency in samples (0 while it isn't loaded).
    int deviceLatency(const QString& trackId, const QString& deviceId);
    // What a device's own editor draws besides its parameters (meters, curves).
    QList<ProcessorDisplay> processorDisplays(const QString& trackId, const QString& deviceId);
    // Display `index`'s values since `position` (0 at first), oldest first,
    // appended to `out`: returns where to read from next (`position` if the
    // device isn't loaded).
    quint64 readProcessorDisplay(const QString& trackId, const QString& deviceId, int index, quint64 position,
                                 std::vector<float>& out);

    // Plug-in editors, in windows owned by the main window (ownerWindow: its
    // native handle, an HWND; the UI sets it). Only the selected track's show.
    void setOwnerWindow(std::function<uintptr_t()> ownerWindow);
    // Opens a plug-in's editor (or raises it). False if it has none (said so, if `report`).
    Q_INVOKABLE bool openPluginEditor(const QString& trackId, const QString& deviceId, bool report = true);
    Q_INVOKABLE void closePluginEditor(const QString& trackId, const QString& deviceId);
    // Opens a plug-in's editor now if its track is the one shown, or when it is
    // (and loads its plug-in now if it waits to).
    Q_INVOKABLE void requestPluginEditor(const QString& trackId, const QString& deviceId);
    // Shows the editors of one track (the selected one; "": none): the ones the
    // user left open there come back where they were, and every other track's
    // are hidden until that track is shown again. Hidden editors keep running
    // (they come back as they were), but only kMaxHiddenEditors of them: beyond
    // that, the ones hidden longest close, and open again (where they were) when shown.
    Q_INVOKABLE void showPluginEditors(const QString& trackId);
    Q_INVOKABLE bool isPluginEditorOpen(const QString& trackId, const QString& deviceId);
    void closeAllEditors();
    // The engine's housekeeping, and what plug-ins reported since (the meter timer does it).
    void pollPlugins();

    // A project's plug-ins loading after it opened.
    Q_INVOKABLE bool pluginPending(const QString& deviceId) const;  // its plug-in waits to load
    int pluginsPending() const;  // how many wait
    // A track's plug-ins load next (the one shown in the device view).
    Q_INVOKABLE void prioritizePlugins(const QString& trackId);
    // A waiting plug-in loads now (its editor is asked for).
    void loadPluginNow(const QString& deviceId);
    // Every waiting plug-in loads now (before rendering offline).
    void loadPendingPlugins();
    // Until every device is ready to render offline: the plug-ins waiting to
    // load loaded, and every built-in device's state restored.
    void waitForDeviceStates();
    // Whether every device is ready to render offline (without waiting).
    Q_INVOKABLE bool devicesReady() const;

    // --- Parameters and automation -----------------------------------------------------------
    // Whether the engine plays this target's envelope (it has one, not overridden).
    Q_INVOKABLE bool isAutomated(const QString& owner, const QString& key) const;
    Q_INVOKABLE bool isOverridden(const QString& owner, const QString& key) const;
    Q_INVOKABLE bool hasOverrides() const;
    // The target was changed by hand: its automation stops until re-enabled.
    void overrideAutomation(const QString& owner, const QString& key);
    // Automation plays again where it was overridden (everywhere, or for one owner).
    Q_INVOKABLE void reEnableAutomation(const QString& owner = QString());
    // The parameters of a device that can be automated (none if it isn't
    // loaded); a rack's: its macros, then its chains' faders.
    std::vector<ParamSpec> deviceParamSpecs(const QString& trackId, const Device& device);
    // An owner's mixer controls: volume, pan, and its sends (to the returns it can send to).
    std::vector<ParamSpec> mixerSpecs(const QString& owner) const;
    // What an owner has that can be automated: its mixer ("mixer", with its
    // sends), then each device (by id, racks and what is in them too).
    std::vector<ParamGroup> paramGroups(const QString& owner);
    Q_INVOKABLE bool canAutomate(const QString& owner, const QString& key);
    // A target's description; none if it doesn't exist (a device that is gone).
    std::optional<ParamSpec> paramSpec(const QString& owner, const QString& key);
    // A target's value as set by hand (plain; a plug-in's read from the plug-in; none if not known).
    std::optional<double> ownValue(const QString& owner, const QString& key);
    // What a target is at `beat` (default: the playhead): its envelope's value
    // while that plays, else its own (plain; none if not known).
    std::optional<double> currentValue(const QString& owner, const QString& key,
                                       std::optional<double> beat = std::nullopt);
    // A plug-in's text for a value of its parameter (by index), with its unit.
    QString pluginParamText(quint32 processorId, int index, double value);

    // --- Recording -------------------------------------------------------------------------------
    Q_INVOKABLE bool isRecording() const;
    // The tracks that record: armed tracks with an input (audio, or MIDI for MIDI tracks), not frozen.
    QStringList recordTargets() const;
    // Records the armed tracks (playing, after the count-in, if stopped).
    // Returns why it couldn't ("": it records).
    Q_INVOKABLE QString startRecording(double countInBeats = 0.0);
    // Ends the recording (playing goes on); its takes go out as takesRecorded too.
    std::vector<RecordedTake> stopRecording();
    // Track id -> its take while it records.
    const QMap<QString, LiveTake>& liveTakes() const;

    // --- Freezing and reversing ------------------------------------------------------------------
    // Renders a track's signal before its fader (after its devices; a group's
    // bus) from the timeline's start to the arrangement's end, and its tail,
    // into a new WAV file in the freeze folder, waited for: its frozen audio.
    // Throws EditError (with a message for the user) if there is nothing to
    // render or the file can't be written. (The UI renders in the background:
    // startFreeze.)
    Freeze renderFreeze(const QString& trackId);
    // renderFreeze in the background: starts the render (the devices should be
    // ready: devicesReady) and returns at once; finishFreeze makes it the
    // track's frozen audio. Throws as renderFreeze does.
    std::unique_ptr<FreezeRender> startFreeze(const QString& trackId);
    // A freeze's render, ended: its frozen audio (decoded, so that it plays as
    // soon as the track is frozen), or none if it was cancelled (its file gone).
    // Throws EditError if it failed.
    std::optional<Freeze> finishFreeze(FreezeRender& render);
    // A render no track plays (no undo step holds it): its decoded audio is
    // forgotten and its file deleted.
    void discardFreeze(const Freeze& freeze);
    // Exports the arrangement from startBeat to endBeat as a WAV file of
    // `bitDepth` (16, 24 or 32 float) in the background; finish it with
    // finishExport (the frames written; none if cancelled). Throws EditError.
    std::unique_ptr<EngineRender> startExport(const QString& path, double startBeat, double endBeat, int bitDepth);
    std::optional<qint64> finishExport(EngineRender& render);
    // The reversed copy of `path` there is already, if any: one made this
    // session, or one a clip of the project plays (saved with it). Not decoded
    // yet, it is asked for.
    std::optional<QString> reversedCopy(const QString& path);
    // Starts writing a reversed copy of `path` (decoded already: see source())
    // as a new WAV file in the reversed folder; finishReversed takes it. Throws
    // EditError (with a message for the user) if `path` isn't decoded.
    std::unique_ptr<ReverseJob> startReversed(const QString& path);
    // A reversed copy of `path` written: its path and its length in seconds,
    // decoded (so that it plays without a gap), and given again for this file
    // from now on. None if it was cancelled. Throws EditError if it failed.
    std::optional<std::pair<QString, double>> finishReversed(const QString& path, ReverseJob& job);
    // A reversed copy of `path`, waited for: the one there is already, or a new
    // one. Its path and its length in seconds; throws as startReversed and finishReversed do.
    std::pair<QString, double> renderReversed(const QString& path);

    // --- The audio device, MIDI input ---------------------------------------------------------------
    static QStringList driverTypes();
    // The devices of a driver type (none if it can't list them).
    QList<AudioDeviceEntry> listDevices(const QString& driver);
    AudioDeviceStatus deviceStatus() const;
    AudioDeviceCaps deviceCapabilities() const;
    // Opens the device the settings describe, closing the one open. Returns an
    // error message ("": it opened).
    QString openDevice(const AudioSettings& settings);
    // Opens the device again, as its driver asked: its settings changed (in
    // its control panel, or its clock).
    QString resetDevice();
    // The ASIO driver's own settings. False if it has none.
    Q_INVOKABLE bool showDeviceControlPanel();
    Q_INVOKABLE void closeDevice();
    // Starts audio as the application starts: the render threads and MIDI
    // inputs from the preferences, and the saved device. If the device can't
    // run as saved (an ASIO driver on another clock, fewer outputs), it opens
    // with the device's own settings; if the driver is gone, the system's
    // default output with the saved buffer size; if nothing opens, audio is off.
    // Each says so in a status message. Returns whether a device opened.
    bool startAudio();
    // The device's inputs, by channel (none while no device is open).
    Q_INVOKABLE QStringList inputNames() const;
    // The MIDI inputs connected, by name.
    Q_INVOKABLE QStringList midiInputs() const;
    // What a track's MIDI input can be: the inputs connected, and the computer keyboard.
    Q_INVOKABLE QStringList midiInputChoices() const;
    // Plays a MIDI message (1-3 bytes) now, as if `device` sent it (dropped while no audio device runs).
    Q_INVOKABLE void sendMidi(const QList<int>& message, const QString& device = kComputerKeyboard);
    Q_INVOKABLE bool isMidiInputOpen(const QString& name) const;
    // Opens every MIDI input connected but those turned off, and closes those
    // turned off (or gone). Inputs that can't be opened are reported once.
    Q_INVOKABLE void openMidiInputs();
    Q_INVOKABLE void setMidiInputEnabled(const QString& name, bool enabled);
    // MIDI input -> why it couldn't be opened.
    const QMap<QString, QString>& midiErrors() const;

Q_SIGNALS:
    void sourceReady(const QString& path);  // a file finished decoding (its waveform is there)
    void sourceFailed(const QString& path, const QString& message);
    void positionChanged(double beat);
    void transportChanged(bool playing);
    void countingInChanged(bool countingIn);
    void metronomeChanged(bool enabled);
    void metersUpdated();  // meters() and chainMeters() were refreshed
    void deviceChanged();  // the audio device opened, closed, changed or reported new latencies
    void statusMessage(const QString& message);  // for the status line
    // Plug-ins (track id, device id first).
    // An edit in the plug-in's own editor: the window makes it an undo step, one per gesture (a knob drag).
    void pluginParamEdited(const QString& trackId, const QString& deviceId, const QString& paramId, double value,
                           double oldValue, quint32 gesture);
    void pluginParamTouched(const QString& trackId, const QString& deviceId, const QString& paramId);  // taken hold of
    void pluginParamsChanged(const QString& trackId, const QString& deviceId);  // values changed without edits
    void pluginParamsRebuilt(const QString& trackId, const QString& deviceId);  // the parameter list changed
    void pluginEditorChanged(const QString& trackId, const QString& deviceId);  // its editor opened or closed
    void pluginStateDirty();  // a plug-in changed in a way no edit shows: the project has changes
    void devicesLoaded(const QString& trackId);  // its devices' processors were (re)created
    // A project's plug-ins loading: loaded, of how many ((0, 0): all done).
    void pluginsLoading(int loaded, int total);
    void pluginsPendingChanged();
    // Automation owner (track id or kMaster): which of its envelopes play, or are overridden, changed.
    void automationStateChanged(const QString& owner);
    void recordingChanged(bool recording);  // recording started or ended
    void recordingUpdated();  // live takes grew
    void takesRecorded(const std::vector<sub::app::RecordedTake>& takes);  // a recording ended with these

private:
    friend class BridgeTestAccess;  // the tests' window on the engine's reports
    struct Private;  // BridgePrivate.h
    struct SidechainState;
    using ChainEntry = std::pair<QString, std::optional<quint32>>;  // (device id, its processor; none: not loaded)
    struct ModelChain {
        QString trackId;
        const std::vector<Device>* devices = nullptr;
    };

    // Tracks (BridgeTracks.cpp).
    void onReset();
    void removeEngineTracks();
    void addEngineTrack(const QString& trackId);
    void onTrackRemoved(const QString& trackId);
    void onTrackChanged(const QString& trackId);
    void overrideChangedSends(const QString& trackId);
    void overrideChangedMixer(const QString& owner, double volumeDb, double pan, bool mute);
    void pushMixer(const QString& trackId);
    struct OutputState;
    OutputState wantedOutput(const Track& track);
    void pushOutputs();
    // Every route to the engine, in the order that lets each go in: outputs,
    // inputs from tracks, then devices' sidechains.
    void pushRoutes();
    std::vector<std::pair<quint32, std::pair<double, bool>>> wantedSends(const Track& track) const;
    void pushSends(const QString& trackId);
    void pushAllSends();
    void pushClips(const QString& trackId);
    void pushSettings();
    // Inputs (BridgeInputs.cpp).
    std::optional<quint32> inputSource(const Track& track) const;
    void pushInput(const QString& trackId);
    void pushAllInputs();
    void openInputs(const std::vector<int>& channels);
    // Devices (BridgeDevices.cpp).
    QMap<QString, ModelChain> modelChains() const;
    QStringList ownedChains(const QString& trackId) const;
    void syncDevices(const QString& trackId);
    void place(const QString& trackId, const QString& key, const std::vector<Device>& devices, QSet<QString>& changed);
    void placeRack(const QString& trackId, const Device& rack, quint32 processorId, QSet<QString>& changed);
    std::pair<bool, std::optional<quint32>> takeOver(const QString& trackId, const QString& key, const Device& device);
    void dispose(const QString& trackId, const QString& key, const QString& deviceId,
                 std::optional<quint32> processorId);
    QStringList chainsOf(const QString& rackId) const;
    void dropChain(const QString& key);
    // Where the engine taps a source's signal for a tap as the model has it
    // (kPostFader, kPreFader, kPreFx: a MIDI track's after its instrument; or a
    // device's id, after it: before the fader while it isn't on the source).
    std::pair<sub::SidechainTap, quint32> engineTap(const QString& sourceId, const QString& tap);
    std::optional<SidechainState> wantedSidechain(const Device& device, quint32 processorId);
    void pushSidechains();
    void dropSidechain(quint32 processorId);
    void pushEnabled(const QString& trackId);
    void onChainChanged(const QString& trackId, const QString& chainId);
    void pushChainMixers(const QString& trackId);
    std::optional<quint32> createProcessor(quint32 chainId, const Device& device);
    std::optional<quint32> loadPlugin(quint32 chainId, const Device& device);
    void pluginFailed(const QString& deviceId, const QString& message);
    void setPluginState(quint32 processorId, const QString& name, const QByteArray& state);
    void forgetProcessor(const QString& deviceId, std::optional<quint32> processorId, bool remove = true);
    void forgetChainDevices(const QString& key, bool remove);
    void onDeviceParamChanged(const QString& trackId, const QString& deviceId, const QString& paramId);
    void pushDeviceParam(const QString& trackId, const QString& deviceId, const QString& paramId);
    void setParam(quint32 processorId, const QString& paramId, double value);
    void pushDeviceState(const QString& trackId, const QString& deviceId);
    void setBuiltinState(quint32 processorId, const Device& device);
    // Plug-ins (BridgePlugins.cpp).
    std::optional<QString> paramIdAt(quint32 processorId, int index);
    QString editorTitle(const QString& trackId, const QString& deviceId) const;
    void updateEditorTitles(const QString& trackId);
    void dispatchProcessorEvents(const std::vector<sub::ProcessorEventRecord>& events);
    // Loading (BridgeLoading.cpp).
    void deferPlugin(const QString& deviceId);
    void startLoadingPlugins();
    void stopLoadingPlugins();
    void loadNextPlugin();
    void reportPlugins();
    bool loadDeferred(const QString& deviceId);
    // Parameters (BridgeParameters.cpp).
    void onAutomationChanged(const QString& owner, const QString& key);
    void pushAutomation(const QString& owner);
    // What the automation of an owner's macros plays (those not overridden): for
    // each parameter mapped to one, by its key, the macro's envelope over its range.
    QMap<QString, Envelope> macroEnvelopes(const QString& owner) const;
    std::optional<sub::AutomationLaneDesc> engineLane(const QString& owner, const QString& key,
                                                      const Envelope& points);
    void pushOwnValue(const QString& owner, const QString& key);
    const std::vector<sub::ParamInfo>& paramInfos(quint32 processorId);
    // Sources (BridgeSources.cpp).
    void onLoaded(const QString& path, std::shared_ptr<sub::AudioSource> source);
    void onFailed(const QString& path, const QString& message);
    // Transport (BridgeTransport.cpp).
    void pollPosition();
    void pollMeters();
    // Recording (BridgeRecording.cpp).
    void pollRecording();
    // Freezing (BridgeFreezing.cpp).
    void pushFrozen(const QString& trackId);
    void onFreezeChanged(const QString& trackId);
    // The audio device (BridgeAudioDevice.cpp).
    QString changeDevice(const std::function<void()>& change);
    void pollDevice();

    sub::Engine& engine_;
    Project* project_;
    std::unique_ptr<Private> d_;
};

}  // namespace sub::app

