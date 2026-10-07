#pragma once
// The engine's public API: what the application layer's engine bridge
// (app/src/audio/EngineBridge.h), the plug-in scanner and the engine's tests use.
// No Qt here: the engine builds and runs on its own.
//
// Routing (Routing.h): every track's output goes to the master or into another
// track (a bus: a group track), and its sends into other tracks (return
// tracks); a track may take its input from another track's output (an input
// edge: resampling), and a device its sidechain from another track's signal (an
// edge into that device). The snapshot lists the tracks so that each comes after
// what feeds it, and lines up the edges going into every bus and device.
//
// Threading model:
//  * The audio thread (device callback) only reads the published RenderSnapshot
//    and atomics. It never locks, allocates or frees.
//  * API calls may come from any thread. They serialise on `mutex_`,
//    mutate the edit model, then rebuild and publish a new snapshot.
//  * Plug-ins are created, called and destroyed on the main (UI) thread, as
//    plug-in formats require: the plug-in calls below, and idle(), must come
//    from the thread that created the engine. Plug-ins may run a message loop
//    inside a call (a licence dialog), which can call back into the engine from
//    the same thread, so `mutex_` is recursive and slow plug-in calls are made
//    without holding it.
//  * Audio devices, likewise, are opened and closed on the main thread. An
//    ASIO driver's control panel may run a message loop too; the device stays
//    as it is until the panel closes.
//  * Recording: the audio thread hands the input to a RecordingSession's
//    rings, and its own thread writes the files (Recorder.h). Anything that
//    changes the device (or its sample rate), or renders offline, ends the
//    recording first, cleanly: the takes so far are kept for stopRecording().
//  * MIDI input devices are opened and closed on the main thread too. Their
//    messages arrive on the drivers' threads, are stamped against the audio
//    device's clock there and queued for the audio thread (MidiInput.h); they
//    never take `mutex_`. Without a running audio device they are dropped.
//  * Audio threads (Scheduler.h): the thread rendering (the audio thread, or the
//    one rendering offline) shares the tracks of each chunk with a pool of
//    workers. They touch only what the snapshot hands them, like the audio thread.
//  * Renders in the background (RenderJob.h) run on a thread of their own, on
//    the snapshot they began with (which they hold), without `mutex_`; live
//    output is silent until the main thread finishes them. Meanwhile processors
//    aren't idled (a plug-in restarting would leave a gap in the render).

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "AudioDevice.h"
#include "AudioSource.h"
#include "Automation.h"
#include "MidiInput.h"
#include "Processor.h"
#include "Recorder.h"
#include "RenderJob.h"
#include "Renderer.h"
#include "Routing.h"
#include "cache/BackgroundRenderer.h"
#include "Scheduler.h"
#include "Snapshot.h"
#include "Transport.h"
#include "cache/CacheStore.h"
#include "rt/RtUtils.h"

namespace sub {

struct ClipDesc {
    std::string path;
    double startBeat = 0.0;
    double durationSec = 0.0;   // of source audio
    double offsetSec = 0.0;     // into the source
    float gain = 1.f;
    float pan = 0.f;            // -1 (left) .. 1 (right), balance
    // Warping. Unwarped clips play at the file's own speed, so their length in
    // beats follows the tempo. Warped clips are locked to beats: `segmentBpm` is
    // the tempo of the source audio, and they play at tempo / segmentBpm speed.
    bool warp = false;
    double segmentBpm = 0.0;
    WarpMode warpMode = WarpMode::Standard;
    double transpose = 0.0;     // semitones (fractions for detune); ignored by Re-Pitch
    std::string id;             // stable clip identity, so edits don't interrupt a stretching clip
    // Fades at its ends, in seconds of source audio (so they stretch with it),
    // each bent by its curve (-1..1, automationShape()). Without one an edge
    // gets the short click-free fade (setClipFadeMs), unless it is the file's
    // own start or end.
    double fadeInSec = 0.0;
    double fadeOutSec = 0.0;
    float fadeInCurve = 0.f;
    float fadeOutCurve = 0.f;
};

// A note on a MIDI track, in timeline beats. The UI flattens MIDI clips into
// these, keeping only what the clips play (notes cut at their clip's end).
struct NoteDesc {
    double startBeat = 0.0;
    double lengthBeats = 0.25;
    int key = 60;  // MIDI note number; 60 = C3
    int velocity = 100;
};

// A track to record, and the file its take goes to (a WAV file, created anew).
// No file: the track records its MIDI input instead.
struct RecordTarget {
    uint32_t trackId = 0;
    std::string path;
};

// A take while it records, for the UI's live waveform (or notes).
struct RecordingProgress {
    uint32_t trackId = 0;
    bool started = false;     // the playhead moved (after any count-in) and input arrives
    int64_t startSample = 0;  // where it goes on the timeline (audio: latency-corrected, may be negative)
    int64_t frames = 0;
    // Peaks since the last call: (min, max) pairs, each over kPeakFrames frames of all its channels.
    std::vector<float> peaks;
    bool midi = false;
    std::vector<RecordedNote> notes;  // a MIDI take's notes so far (held ones end at -1)
};

// The audio device's sample clock as MIDI input sees it (MidiInput.h).
struct AudioClockStatus {
    bool running = false;
    int64_t hostTimeNs = 0;   // when the last callback began (hostTimeNs())
    int64_t sampleTime = 0;   // the device sample it began with
    int midiDelay = 0;        // frames between a MIDI message's arrival and when it plays
};

// Where a sidechain takes its source's signal: after its fader (and pan), before
// it (after all its devices), after one of its devices, or before all of them
// (PreFx: the strip's own input, as its devices hear it).
enum class SidechainTap : uint8_t { PostFader, PreFader, AfterDevice, PreFx };

// A device's sidechain (Engine::processorSidechain()).
struct SidechainInfo {
    uint32_t trackId = 0;  // its source
    SidechainTap tap = SidechainTap::PostFader;
    uint32_t tapProcessorId = 0;  // AfterDevice: the source's device it is taken after
};

// A track's send (Engine::trackSends()).
struct SendInfo {
    uint32_t trackId = 0;  // the track it goes into (a return)
    float gain = 1.f;
    bool preFader = false;
};

// What rendering a track takes lately (Engine::trackCosts()).
struct TrackCost {
    uint32_t trackId = 0;
    float nsPerFrame = 0.f;  // 0: not rendered yet
};

// Background freezing (docs/engine/background-freeze.md): what strips played
// live, unchanged, is kept and played again instead of running their devices;
// and what hasn't played yet is rendered into the cache in the background, by
// second instances of the devices, with CPU no one else wants.
struct BackgroundFreezingSettings {
    bool enabled = false;
    double idleSeconds = 10.0;  // a strip plays from its cache once nothing changed it for this long
    double warmSeconds = 8.0;   // how long devices must run before what they put out is kept
    double budgetMB = 1024.0;   // memory for the cache
    bool render = true;         // render in the background too
    bool renderThread = true;   // ... on a thread of its own (false: only renderInBackground() does: tests)
};

// What the cache holds, and what it saved (Engine::backgroundFreezingStats).
struct BackgroundFreezingStats {
    size_t blocks = 0;
    size_t silentBlocks = 0;
    size_t bytes = 0;
    size_t unfreedBytes = 0;       // let go of but not freed yet (bytes and these stay within the budget)
    uint64_t framesFromCache = 0;  // strips' chunks played from the cache (frames, summed over strips)
    uint64_t framesLive = 0;       // ... with their devices running
    uint64_t framesCaptured = 0;   // ... kept
    uint64_t framesRendered = 0;   // ... rendered in the background and kept
    uint64_t framesRenderedLive = 0;  // ... rendered in the background with their devices running
    uint64_t framesReplayed = 0;   // ... kept in the background from blocks a change had rung out of
    size_t shadows = 0;            // second instances of devices, for the background
};

struct MeterReading {
    uint32_t trackId = 0;  // 0 = master
    float left = 0.f;
    float right = 0.f;
    uint32_t chainId = 0;  // a rack chain's fader (on track trackId); 0: the track's own
};

// A processor's description for the UI.
struct ProcessorInfo {
    std::string typeId;
    std::string name;
    int latency = 0;  // samples
    int tail = 0;
    bool hasEditor = false;
    bool hasSidechain = false;  // it has a sidechain (aux) input: setProcessorSidechain()
};

// A ProcessorEvent and the processor it came from.
struct ProcessorEventRecord : ProcessorEvent {
    uint32_t processorId = 0;
};

struct DeviceStatus {
    bool open = false;
    std::string name;
    std::string backend;  // the driver type: "WASAPI" or "ASIO"
    uint32_t sampleRate = 0;
    uint32_t bufferFrames = 0;
    double latencyMs = 0.0;       // output latency
    double inputLatencyMs = 0.0;
    std::vector<int> inputChannels;   // the device channels open, in the order the engine sees them
    std::vector<int> outputChannels;  // the master plays on the first two (or mixed to mono on one)
    bool exclusive = false;
};

class Engine final : private AudioCallback {
public:
    Engine();
    ~Engine() override;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // --- Device ---------------------------------------------------------------
    // Opening and closing devices and their control panels: from the thread that
    // created the engine (ASIO drivers are COM objects in its apartment).
    static std::vector<std::string> driverTypes() { return AudioDevice::driverTypes(); }
    std::vector<AudioDeviceInfo> devices(const std::string& driver);
    void openDevice(const DeviceConfig& config);
    // Opens the last device again, with the settings its driver asked for: the
    // answer to a "reset" event.
    void reopenDevice();
    void closeDevice();
    DeviceStatus deviceStatus();
    DeviceCaps deviceCapabilities();
    // The driver's own settings dialog (ASIO). False if there is none.
    bool showDeviceControlPanel();
    double sampleRate();
    float cpuLoad() const { return shared_.cpuLoad.load(std::memory_order_relaxed); }
    // One event at a time: "", "stopped", "rerouted", "reset" (the driver needs
    // reopenDevice()) or "latency" (the device status has new latencies).
    std::string takeDeviceEvent();
    // The peak level of each open input channel since the last call.
    std::vector<float> takeInputMeters();
    // The last `frames` samples of the master output (mono, oldest first; at
    // most half the scope ring), and how many samples have been played in all
    // (it stands still while no device runs).
    std::vector<float> masterScope(size_t frames) const;
    uint64_t masterScopeWritten() const { return shared_.scopeWritten.load(std::memory_order_acquire); }

    // --- Sources --------------------------------------------------------------
    std::shared_ptr<AudioSource> loadSource(const std::string& path);  // cached; blocking
    std::shared_ptr<AudioSource> cachedSource(const std::string& path);
    void releaseUnusedSources();

    // --- Tracks ---------------------------------------------------------------
    // Track id 0 is the master: it has devices, a mixer and automation like a
    // track, but no clips or notes, and it never mutes or solos.
    static constexpr uint32_t kMaster = 0;
    uint32_t addTrack();
    void removeTrack(uint32_t trackId);
    void setTrackClips(uint32_t trackId, const std::vector<ClipDesc>& clips);
    void setTrackNotes(uint32_t trackId, const std::vector<NoteDesc>& notes);
    // Plays a note on the track's instrument right away (velocity 0 releases it),
    // e.g. while editing notes. Only heard while a device runs.
    void previewNote(uint32_t trackId, int key, int velocity);
    void setTrackGain(uint32_t trackId, float gain);  // also the master's (and pan)
    void setTrackPan(uint32_t trackId, float pan);
    void setTrackMute(uint32_t trackId, bool mute);
    void setTrackSolo(uint32_t trackId, bool solo);
    // Routing: where a track's output goes, the master (kMaster) or another
    // track, which then sums it into its own input (a group's bus). The engine
    // sees only these edges, never groups. Throws std::invalid_argument for an
    // unknown track, or a route that would close a cycle (a track into itself,
    // or into a track it feeds). When a track goes, what went into it goes to
    // the master. Delay compensation lines up the inputs of every bus.
    void setTrackOutput(uint32_t trackId, uint32_t outputTrackId);
    uint32_t trackOutput(uint32_t trackId);
    // Sends: a track's signal also goes into another track (a return track), at
    // `gain`, taken after its fader or before it (`preFader`; a muted track's
    // sends are silent either way). One send per pair; setting it again changes
    // it (a new gain alone is cheap: no new snapshot). Throws
    // std::invalid_argument for an unknown track, the master, or a send that
    // would close a cycle (through outputs and sends alike). When a track goes,
    // the sends into it go too. Its automation: a lane of the sending track,
    // processorId 0 and param "send:<track id>" (as volume: 0..1, +6 dB at 1).
    void setTrackSend(uint32_t trackId, uint32_t toTrackId, float gain, bool preFader);
    void removeTrackSend(uint32_t trackId, uint32_t toTrackId);
    std::vector<SendInfo> trackSends(uint32_t trackId);
    // Freezing: a frozen track plays its clips (its frozen audio: see
    // renderTrackToWav()) through its fader and on along its edges, and nothing
    // else: its devices, notes, input and what goes into it (outputs, sends) are
    // left out, and so is their latency. A track every edge of which ends at a
    // frozen track (or at a track like it: what is in a frozen group) isn't
    // rendered at all. Devices left out are reset when they play again.
    void setTrackFrozen(uint32_t trackId, bool frozen);
    bool trackFrozen(uint32_t trackId);
    void setMasterGain(float gain) { setTrackGain(kMaster, gain); }
    void setMasterPan(float pan) { setTrackPan(kMaster, pan); }
    // Peak levels since the last call: the master, each track, then each rack chain (chainId set).
    std::vector<MeterReading> takeMeters();

    // --- Input and recording ------------------------------------------------------
    // A track's input: device channels (0-based, as DeviceStatus lists them): none,
    // one (mono) or two (a stereo pair). Channels the device hasn't open are silent.
    void setTrackInput(uint32_t trackId, const std::vector<int>& channels);
    // A track's input from another track's output, after its fader (resampling
    // it: an input edge), or from the master's (kMaster: resampling the mix),
    // instead of device channels (setTrackInput() goes back to those). The source
    // renders first. Monitored, the track hears a track's output instead of its
    // clips, without delay compensation, as it hears the device; never the
    // master's (that would feed back), which it can only record. Throws
    // std::invalid_argument for an unknown track, the track itself, or a track
    // it feeds (a cycle, through outputs, sends and inputs alike). When the
    // source goes, the input goes too.
    void setTrackInputTrack(uint32_t trackId, uint32_t sourceTrackId);
    std::optional<uint32_t> trackInputTrack(uint32_t trackId);  // none: its input is the device's (or none)
    void setTrackMonitor(uint32_t trackId, MonitorMode mode);
    // Armed tracks are what Auto monitoring listens to; what records is up to startRecording().
    void setTrackArmed(uint32_t trackId, bool armed);
    // Records the targets' inputs from the next block the playhead moves in, and
    // starts playing (after `countInBeats` of count-in) if stopped. Throws
    // std::runtime_error (for the user) if no device runs, a target's input isn't
    // open, or a file can't be created; std::invalid_argument for a bad target.
    // A take lands where what it recorded was heard: device input is moved back
    // by the output's lag and the device's latencies; a track's output by how
    // late it leaves the track (its edge's arrival); the master's by the lag.
    void startRecording(const std::vector<RecordTarget>& targets, double countInBeats = 0.0);
    // Ends the recording (the transport plays on) and returns its takes, and those
    // of a recording ended otherwise since the last call (a device change).
    std::vector<RecordedTake> stopRecording();
    // Recording and still taking input: not after the playhead jumped (a locate,
    // stopping), nor once something else ended it.
    bool isRecording();
    std::vector<RecordingProgress> recordingProgress();
    static constexpr int kRecordPeakFrames = RecordingTake::kPeakFrames;

    // --- MIDI input -------------------------------------------------------------
    // Devices (main thread): those connected, by name; open ones play into the
    // tracks whose MIDI input takes them. open throws std::runtime_error for the user.
    std::vector<std::string> midiInputDevices();
    void openMidiInput(const std::string& name);
    void closeMidiInput(const std::string& name);
    std::vector<std::string> openMidiInputs();
    // A track's MIDI input: none (`enabled` false), every input ("") or one by
    // name (open or not), on every channel (0) or one (1-16). With a MIDI input
    // the track is recorded by a RecordTarget without a file; it hears the input
    // while monitored (In, or Auto and armed).
    void setTrackMidiInput(uint32_t trackId, bool enabled, const std::string& device, int channel);
    // A message as if it came from the input `device` at `hostTimeNs` (the
    // hostTimeNs() clock; 0: now), e.g. from a computer keyboard or a test. 1-3
    // bytes, starting with a status byte. Dropped if no audio device runs.
    void sendMidiInput(const std::string& device, const std::vector<uint8_t>& message, int64_t hostTimeNs = 0);
    AudioClockStatus audioClock() const;

    // --- Automation -------------------------------------------------------------
    // Replaces the automation of a track (trackId 0: the master): an envelope per
    // target, either a mixer control (processorId 0, param "volume" or "pan"), a
    // parameter (by id) of one of the track's devices (in a rack too), or the
    // fader of a rack's chain (the rack's id, param "chain:<chain id>:volume" or
    // "...:pan"). Envelopes of devices that are gone (or are on another track),
    // or parameters they don't have, are kept but not played.
    // A target automated here follows its envelope; its own value (set above)
    // counts again once its envelope is taken away.
    void setTrackAutomation(uint32_t trackId, const std::vector<AutomationLaneDesc>& lanes);

    // --- Device chains ------------------------------------------------------------
    // Devices live in chains, each with an id: every track and the master has a
    // main chain (later, each chain of a rack is one more). Processor ids are
    // unique across chains, so a processor can move from one chain to another
    // and keep its state (a plug-in isn't loaded again).
    uint32_t trackChain(uint32_t trackId);  // a track's (or the master's) main chain
    uint32_t processorChain(uint32_t processorId);
    // `index`: where in the chain (-1 or past the end: last).
    uint32_t addBuiltinProcessor(uint32_t chainId, const std::string& type, int index);
    // Loads a plug-in ("VST3" format) into the chain. Main thread; throws
    // std::runtime_error with a message for the user if it can't be loaded.
    uint32_t addPluginProcessor(uint32_t chainId, const std::string& format, const std::string& path,
                                const std::string& uid, int index);
    // Removes a device (a rack with its chains and everything in them).
    void removeProcessor(uint32_t processorId);
    // Reorders a chain; `processorIds` must be the chain's processors, each once.
    void setChainOrder(uint32_t chainId, const std::vector<uint32_t>& processorIds);
    // Moves a processor to position `index` of a chain (its own or another, on
    // any strip, in a rack too), as the chain is without it (-1: last). Its state
    // goes with it (a rack's chains and devices too), and its sidechain (and
    // those of the devices in a rack; std::invalid_argument if one would make a
    // cycle there, as for a rack moving into itself or nesting too deep); its
    // automation stays with the strip it came from, and plays again if it comes back.
    void moveProcessor(uint32_t processorId, uint32_t toChainId, int index);

    // Racks (device groups): a rack is a device in a chain whose own chains each
    // process its input, side by side; it puts out their sum (an empty rack
    // passes its input on). Each chain has its devices (racks too, at most
    // kMaxRackDepth deep), and a fader: gain, pan, mute and solo (solo among the
    // rack's chains), metered like a track's. The rack's latency is its slowest
    // chain's: the others are delayed to line up with it. Every chain's devices
    // hear the track's notes (layering instruments). Throws std::invalid_argument
    // for an unknown chain or rack, or a rack nested too deep.
    static constexpr int kMaxRackDepth = sub::kMaxRackDepth;
    uint32_t addRack(uint32_t chainId, int index);
    uint32_t addRackChain(uint32_t rackId, int index);  // a new chain, at `index` (-1: last)
    void removeRackChain(uint32_t chainId);              // with its devices
    void setRackChainOrder(uint32_t rackId, const std::vector<uint32_t>& chainIds);
    std::vector<uint32_t> rackChains(uint32_t rackId);
    uint32_t chainRack(uint32_t chainId);  // the rack a chain belongs to; 0: a strip's main chain
    std::vector<uint32_t> chainProcessors(uint32_t chainId);  // in order
    void setChainGain(uint32_t chainId, float gain);
    void setChainPan(uint32_t chainId, float pan);
    void setChainMute(uint32_t chainId, bool mute);
    void setChainSolo(uint32_t chainId, bool solo);
    // A device's sidechain: what its aux input hears (hasSidechain in its
    // ProcessorInfo), from a track (a track, a group or a return; not the master,
    // which renders after everything), tapped after the track's fader, before it,
    // after one of its devices (`tapProcessorId`, in the track's main chain;
    // should that device leave it, the tap is before the fader), or before all of
    // them (PreFx). It is lined up
    // with the signal at the device: delayed, or that signal is (just before the
    // device). Throws std::invalid_argument for an unknown device or track, a
    // device without a sidechain input, the master, or a sidechain that would
    // close a cycle (its own track, or one its track feeds: through outputs,
    // sends, inputs and sidechains alike). It stays with the device when the
    // device moves (moveProcessor refuses a move that would make it a cycle);
    // when its track goes, it goes too.
    void setProcessorSidechain(uint32_t processorId, uint32_t sourceTrackId,
                               SidechainTap tap = SidechainTap::PostFader, uint32_t tapProcessorId = 0);
    void clearProcessorSidechain(uint32_t processorId);
    std::optional<SidechainInfo> processorSidechain(uint32_t processorId);
    ProcessorInfo processorInfo(uint32_t processorId);
    std::vector<ParamInfo> processorParams(uint32_t processorId);
    int processorParamIndex(uint32_t processorId, const std::string& paramId);  // -1: no such parameter
    float processorParam(uint32_t processorId, int index);
    void setProcessorParam(uint32_t processorId, int index, float value);
    std::string processorParamText(uint32_t processorId, int index, float value);
    // What a device's own editor draws besides its parameters (Processor::displays()),
    // and display `index`'s values since `position` (0 at first), appended to
    // `out`; returns where to read from next.
    std::vector<DisplayInfo> processorDisplays(uint32_t processorId);
    uint64_t readProcessorDisplay(uint32_t processorId, int index, uint64_t position, std::vector<float>& out);
    void setProcessorEnabled(uint32_t processorId, bool enabled);
    std::vector<uint8_t> processorState(uint32_t processorId);
    void setProcessorState(uint32_t processorId, const std::vector<uint8_t>& state);
    // Plug-in editors, in windows owned by `ownerWindow` (an HWND; 0 for none).
    bool openEditor(uint32_t processorId, uintptr_t ownerWindow, const std::string& title);
    void closeEditor(uint32_t processorId);
    bool isEditorOpen(uint32_t processorId);
    bool setEditorVisible(uint32_t processorId, bool visible);
    void setEditorTitle(uint32_t processorId, const std::string& title);
    // What processors reported since the last call (edits in a plug-in's editor...).
    std::vector<ProcessorEventRecord> takeProcessorEvents();

    // --- Transport ------------------------------------------------------------
    void play(double countInBeats = 0.0);  // a count-in: the metronome clicks, then the playhead moves
    bool isCountingIn() const { return shared_.countingIn.load(std::memory_order_relaxed); }
    void stop();
    bool isPlaying() const { return requestedPlaying_.load(std::memory_order_relaxed); }
    double positionBeats() const { return shared_.positionBeats.load(std::memory_order_relaxed); }
    void setPositionBeats(double beat);
    void setTempo(double bpm);
    double tempo();
    void setTimeSignature(int numerator, int denominator);
    void setLoop(bool enabled, double startBeat, double endBeat);
    void setMetronome(bool enabled);
    bool metronome() const { return shared_.metronome.load(std::memory_order_relaxed); }
    // The short fade against clicks at a clip's edge that cuts into its file
    // (4 ms by default, 0 to 100): where the clip has no fade of its own there.
    void setClipFadeMs(double ms);

    // --- Browser preview ------------------------------------------------------
    void preview(const std::string& path);  // the source must already be loaded
    void stopPreview();
    bool isPreviewing() const { return shared_.previewActive.load(std::memory_order_relaxed); }

    // --- Offline rendering ----------------------------------------------------
    // All of them temporarily silence live output and render the same graph.
    std::vector<float> renderOffline(double startBeat, int64_t frames, bool loop = false,
                                     bool metronome = false);  // interleaved stereo
    void exportWav(const std::string& path, double startBeat, double endBeat, int bitDepth);
    // One track's signal before its fader (after its devices: what freezing it
    // keeps), lined up with the timeline: what it plays at startBeat comes first.
    // Solo is ignored (what goes into a group is heard as if nothing were
    // soloed); mute isn't. Interleaved stereo.
    std::vector<float> renderTrackOffline(uint32_t trackId, double startBeat, int64_t frames);
    // The same from startBeat to endBeat, then on for up to `tailSeconds` while
    // it isn't silent (a reverb's tail), into a new 32-bit float WAV file.
    // Returns the frames written.
    int64_t renderTrackToWav(uint32_t trackId, const std::string& path, double startBeat, double endBeat,
                             double tailSeconds);
    // exportWav() and renderTrackToWav() on a thread of their own (RenderJob.h):
    // they return at once with the job, its file created (or throw why not).
    // One job at a time. Until it is finished live output is silent, and what
    // would disturb it is refused (std::runtime_error): another render, opening
    // a device, the audio threads, recording. Other changes are taken, but the
    // job renders the project as it was when it started.
    std::shared_ptr<RenderJob> startExport(const std::string& path, double startBeat, double endBeat, int bitDepth);
    std::shared_ptr<RenderJob> startTrackRender(uint32_t trackId, const std::string& path, double startBeat,
                                                double endBeat, double tailSeconds);
    bool isRendering();  // a job is running (or done, not finished yet)

    // --- Audio threads -----------------------------------------------------------
    // How many threads render (the audio thread and the workers); 1 renders
    // every track on the audio thread. Renders are the same whatever the number.
    static int defaultAudioThreads();  // one per core but one (at least 1)
    static constexpr int kMaxAudioThreads = 64;
    void setAudioThreads(int threads);
    int audioThreads();
    // Tracks the workers have rendered since the number of threads was last set
    // (tests and benchmarks see them work).
    uint64_t nodesOnWorkers();
    // Each track's render is timed. Tracks start with those with the most work
    // hanging off them (their own and the groups they go into), unless this is
    // off (for benchmarks): then in routing order. Results are the same either way.
    void setCostOrdering(bool on);
    bool costOrdering() const { return renderer_.costOrdering(); }
    std::vector<TrackCost> trackCosts();

    // --- Background freezing --------------------------------------------------
    // Off by default. Turning it off drops what was cached.
    void setBackgroundFreezing(const BackgroundFreezingSettings& settings);
    BackgroundFreezingSettings backgroundFreezing();
    // Everything, or one strip's (a track, or kMaster) counts.
    BackgroundFreezingStats backgroundFreezingStats();
    BackgroundFreezingStats backgroundFreezingStats(uint32_t trackId);
    // A strip whose devices the UI shows keeps them running (their displays and
    // meters move), and plays live.
    void setTrackObserved(uint32_t trackId, bool observed);
    bool trackObserved(uint32_t trackId);
    // One round of the cache's own thread, now (it does one every few
    // milliseconds; tests call it to be sure what was captured is published).
    void serviceBackgroundFreezing();
    // Brings the background renderer up to date (its shadows, its snapshot) and
    // renders up to `frames` frames of what it has to do now, on the calling
    // thread (tests, benchmarks). Returns the frames rendered (0: nothing to do).
    int64_t renderInBackground(int64_t frames);

    // --- Housekeeping ---------------------------------------------------------
    // Call regularly from the UI thread: frees retired snapshots and removed
    // processors, handles device loss, and does the main-thread work plug-ins
    // asked for (restarts, parameter updates, editor events). Plug-ins take
    // their time to go, so a call destroys removed processors for a few
    // milliseconds at most (a project closed doesn't hold up the UI), and the
    // next calls go on; `releaseAll` destroys every one now (shutting down).
    void idle(bool releaseAll = false);

private:
    // A send (a routing edge), with its state and delay line kept across snapshots.
    struct SendModel {
        uint32_t to = 0;
        bool preFader = false;
        std::shared_ptr<EdgeState> state;
        std::shared_ptr<DelayLine> delay;
    };
    struct TrackModel {
        uint32_t id = 0;
        std::shared_ptr<TrackParams> params;
        std::vector<ClipDesc> clips;
        std::vector<std::string> clipKeys;  // sourceKey() of each clip's path
        std::vector<NoteDesc> notes;
        uint32_t chainId = 0;               // its main chain
        uint32_t output = 0;                // where its output goes: kMaster or a track (a routing edge)
        std::shared_ptr<EdgeState> outputState;  // its output edge's, kept across snapshots (not the master)
        std::shared_ptr<DelayLine> delay;   // its output edge's delay compensation, kept across snapshots
        std::vector<SendModel> sends;       // more edges, in the order they were made
        std::shared_ptr<TrackBuffers> buffers;  // its signal and chunk state, kept across snapshots (not the master)
        std::vector<AutomationLaneDesc> automation;
        std::vector<int> inputChannels;  // device channels: its input (unless inputTrack)
        // Its input from a track's output (an input edge) or the master's (kMaster), instead of device channels.
        std::optional<uint32_t> inputTrack;
        std::shared_ptr<EdgeState> inputState;  // the input edge's (once it had one), kept across snapshots
        MidiInputRoute midiInput;
        MonitorMode monitor = MonitorMode::Auto;
        bool armed = false;
        bool frozen = false;  // plays its clips through its fader, nothing else (setTrackFrozen)
        bool silenced = false;  // its devices were left out of the last snapshot (frozen, or not rendered)
        // Background freezing: its cache point and what the edit side keeps of its
        // dependencies (EngineCache.cpp).
        struct Cache {
            std::shared_ptr<CachePoint> point = std::make_shared<CachePoint>();
            uint64_t signature = 0;  // of what it is made of (its devices, what feeds it, the tempo...)
            std::deque<DirtyLog::Entry> dirty;  // oldest first
            uint64_t horizon = 0;
            std::shared_ptr<const DirtyLog> log;
            uint64_t resetEpoch = 0;  // the idle stretch its plug-ins were last reset in
            // Its devices' real changes (Processor::realChangeCount) the version
            // accounts for; CachePoint::accountedChanges has all they counted.
            uint64_t accountedReal = 0;
        };
        Cache cache;
    };

    void audioCallback(const AudioIO& io) noexcept override;
    void deviceEvent(DeviceEvent event) noexcept override;
    // Any thread: a MIDI message from input `port`, stamped and queued for the audio thread.
    void midiInput(uint16_t port, const uint8_t* message, int size, int64_t hostTimeNs) noexcept;
    uint16_t midiPortLocked(const std::string& name);
    void discardMidiInputLocked();

    // A chain of devices: a strip's main chain, or a chain of a rack on it.
    struct ChainModel {
        uint32_t id = 0;
        uint32_t stripId = 0;     // the track (or kMaster) whose signal it processes
        uint32_t parentRack = 0;  // the rack processor it belongs to; 0: a strip's main chain
        std::vector<std::shared_ptr<Processor>> inserts;
        std::shared_ptr<TrackParams> params;  // a rack chain's fader (null for a main chain)
        std::shared_ptr<DelayLine> delay;     // a rack chain's delay compensation, kept across snapshots
    };
    // A device's sidechain (a routing edge into it), with its state and delay
    // lines kept across snapshots.
    struct SidechainModel {
        uint32_t source = 0;  // the track
        SidechainTap tap = SidechainTap::PostFader;
        uint32_t tapProcessor = 0;  // AfterDevice: the source's device
        std::shared_ptr<EdgeState> state;
        std::shared_ptr<DelayLine> delay;        // the sidechain's
        std::shared_ptr<DelayLine> deviceDelay;  // the device's own signal's, before it
    };
    struct ProcessorEntry {
        uint32_t chainId = 0;
        std::shared_ptr<Processor> processor;
        std::optional<SidechainModel> sidechain;
        bool rack = false;             // a RackProcessor:
        std::vector<uint32_t> chains;  // its chains, in order
    };
    // A strip's devices depth first (Routing.h's slots): each device of its main
    // chain, and after a rack the devices of its chains, chain by chain.
    struct StripSlot {
        uint32_t processorId = 0;
        std::shared_ptr<Processor> processor;
        uint32_t chainId = 0;  // the chain it is in
        int index = 0;         // its place there
        int rack = -1;         // the slot of the rack that chain belongs to (-1: the strip's main chain)
        int chain = 0;         // and which of its chains it is
        bool enabled = true;   // it is switched on, and so is every rack it is in
        bool isRack = false;
    };
    using ProcessorIds = std::unordered_map<const Processor*, uint32_t>;

    TrackModel& trackLocked(uint32_t trackId);           // the master too
    TrackModel& arrangementTrackLocked(uint32_t trackId);  // not the master
    std::shared_ptr<Processor> processorLocked(uint32_t processorId);
    std::shared_ptr<Processor> processor(uint32_t processorId);  // locks
    ChainModel& chainLocked(uint32_t chainId);
    const std::vector<std::shared_ptr<Processor>>& insertsLocked(const TrackModel& track) const;
    uint32_t addChainLocked(uint32_t stripId, uint32_t parentRack);
    uint32_t insertProcessorLocked(uint32_t chainId, std::shared_ptr<Processor> processor, int index,
                                   bool rack = false);
    void retireProcessorLocked(std::shared_ptr<Processor> processor);
    // Removes a rack chain and everything in it (not from its rack's list).
    void removeChainContentsLocked(uint32_t chainId);
    ProcessorEntry& rackLocked(uint32_t rackId);
    ChainModel& rackChainLocked(uint32_t chainId);  // a rack's chain (not a strip's main chain)
    int chainDepthLocked(uint32_t chainId) const;  // how many racks a chain is in (0: a strip's main chain)
    int rackHeightLocked(uint32_t rackId) const;   // 1 + the deepest nesting of racks in it
    // The devices in a rack's chains (and in the racks in them), and those chains, appended.
    void rackContentsLocked(uint32_t rackId, std::vector<uint32_t>& processors, std::vector<uint32_t>& chains) const;
    ProcessorIds processorIdsLocked() const;
    std::vector<StripSlot> stripSlotsLocked(const TrackModel& track, const ProcessorIds& ids) const;
    void rebuildSnapshotLocked();
    void pushCommandLocked(const TransportCommand& command);
    void serviceTransportIfIdleLocked();
    void collectGarbageLocked();
    void openDeviceLocked(const DeviceConfig& config);
    void closeDeviceLocked();
    void reloadSourcesLocked();
    void waitForCallbackLocked();
    InputEdge inputEdgeLocked(const TrackModel& track) const;
    void finishRecordingLocked();
    void suspendLiveLocked();
    void resumeLiveLocked();
    void renderOfflineLocked(double startBeat, int64_t frames, float* out, bool loop, bool metronome);
    // Delay lines an offline render brings, so that it neither disturbs live playback nor depends on it.
    struct OfflineLines {
        WarpVoiceSet voices;
        std::vector<std::shared_ptr<DelayLine>> delays, deviceDelays, chainDelays;
    };
    void prepareOfflineLocked(const RenderSnapshot& snap, Renderer& offline, OfflineLines& lines, double startBeat);
    // A render in the background: its own renderer and delay lines, and the snapshot it renders.
    struct OfflineRender {
        std::shared_ptr<const RenderSnapshot> snapshot;
        Renderer renderer;
        OfflineLines lines;
    };
    // Silences live output and resets the processors (as every offline render
    // does), and prepares a render of the snapshot from startBeat.
    std::shared_ptr<OfflineRender> beginOfflineLocked(double startBeat);
    void endOfflineLocked();  // the processors reset again, live output back
    // Starts `body` as the background job (RenderJob::start) on a begun render;
    // if it can't start, the render ends.
    std::shared_ptr<RenderJob> startJobLocked(const std::string& path, int64_t total,
                                              std::function<std::optional<int64_t>(RenderJob&)> body);
    void checkNotRenderingLocked() const;  // throws std::runtime_error while a job runs
    friend class RenderJob;
    void endJob(RenderJob& job);  // RenderJob::finish(): it is over
    // renderTrackOffline(): `frames` of the track's signal, handed to `sink` a
    // piece at a time (interleaved stereo, and how many frames).
    void renderTrackLocked(uint32_t trackId, double startBeat, int64_t frames,
                           const std::function<void(const float*, int64_t)>& sink);
    void resetProcessorsLocked();
    void ensureWarpVoicesLocked(const std::array<size_t, kNumStretchConfigs>& needed);
    // What building a strip's part of the snapshot needs to know about it.
    struct StripBuild {
        const TrackModel* track = nullptr;
        const std::vector<StripSlot>* slots = nullptr;
        std::unordered_map<const Processor*, int> slotOf;  // its devices' slots
        int inputLatency = 0;  // how late the strip's input hears the timeline
        // GraphLatencies' for this strip: from its input, by slot.
        const std::vector<int>* deviceLatency = nullptr;
        const std::vector<std::vector<int>>* chainEnd = nullptr;
        const std::vector<std::vector<int>>* chainCompensation = nullptr;
        std::vector<int> sidechainOf;  // per slot: the snapshot edge into its sidechain input (-1: none)
        double samplesPerBeat = 0.0;
    };
    // A strip's mixer envelopes in the snapshot, in samples, as late as
    // `faderLatency` (delay compensation comes after the fader, on the strip's
    // edges). Its devices' come with them (buildChainLocked).
    void buildAutomationLocked(const StripBuild& build, int faderLatency, StripRender& strip);
    // A chain's devices in the snapshot (a strip's main chain, or a rack's chain,
    // into `chain`), with the sidechains into them, their envelopes and racks. A
    // device hears the timeline as late as the strip's input, plus the latency
    // before it (the devices before it, and the delays before sidechained ones).
    void buildChainLocked(uint32_t chainId, const StripBuild& build, int depth, RenderSnapshot& snap,
                          StripRender& chain);
    // An envelope's breakpoints in samples, sorted.
    static std::vector<AutomationNode> automationNodes(const AutomationLaneDesc& desc, double samplesPerBeat);
    // The routing graph's edges, as indices into tracks_ (-1: the master): each
    // track's output, then its sends; then the input edges; then the sidechains,
    // by destination and device. `origins` (if given) gets what each is.
    static constexpr int kOutputEdge = -1;
    static constexpr int kInputEdge = -2;
    static constexpr int kSidechainEdge = -3;
    struct EdgeOrigin {
        // The track (index) it belongs to: its source, or its destination for an
        // input or a sidechain (-1: the master).
        int track = 0;
        int send = kOutputEdge;  // its send index, or kOutputEdge, kInputEdge, kSidechainEdge
        uint32_t processor = 0;  // a sidechain's device
        int device = -1;         // and that device's place in its chain
    };
    std::vector<RouteEdge> routeEdgesLocked(std::vector<EdgeOrigin>* origins = nullptr) const;

    // Background freezing (EngineCache.cpp). While a snapshot is built: each
    // strip's cache as the snapshot shows it, its version bumped if what it is
    // made of changed, and what changed time-locally (against the last snapshot)
    // in its dirty log, downstream too.
    void updateCacheLocked(RenderSnapshot& snap, const RenderSnapshot* previous);
    // A change no snapshot shows (a fader, a device's parameter): the versions of
    // the strips it changes, from `stripId` itself (or only what it feeds) on.
    void touchCacheLocked(uint32_t stripId, bool itself);
    void touchCacheLocked(TrackModel& strip);
    // A device changed (its parameters, its state): its strip, and what that feeds.
    void processorChangedLocked(uint32_t processorId);
    TrackModel* stripOfProcessorLocked(uint32_t processorId);
    // idle(): changes devices made themselves, editors open, and resetting the
    // plug-ins of strips that play from their cache.
    void cacheIdle(bool rendering);

    // Background rendering (EngineBackground.cpp, cache/BackgroundRenderer.h).
    struct Shadow {
        std::shared_ptr<Processor> live;    // the device (kept while its shadow is)
        std::shared_ptr<Processor> shadow;  // null: it can have none
    };
    // idle(): shadows made, brought up to date and dropped, the background
    // renderer's snapshot rebuilt (`now`: without waiting for edits to settle).
    void backgroundIdle(bool now);
    // The live snapshot with its devices swapped for their shadows and render state of its own.
    std::shared_ptr<const RenderSnapshot> buildShadowSnapshotLocked(const RenderSnapshot& live, int64_t& songEnd);
    // Where a sidechain leaves its source: after its fader, before it, or after
    // its first `n` devices (returns n; -1: after all of them). A tap after a
    // device that isn't in the source's main chain (any more; or is in a rack
    // there) is before the fader.
    int sidechainTapLocked(const SidechainModel& sidechain, EdgeRender::Tap& tap) const;
    // Throws std::invalid_argument if a sidechain from `source` into a device on `strip` would close a cycle.
    void checkSidechainLocked(uint32_t source, uint32_t strip) const;
    // The same for every sidechained device in a rack's chains (it moves to `strip`).
    void checkRackSidechainsLocked(uint32_t rackId, uint32_t strip);
    // Throws std::invalid_argument if an edge from `from` into `to` would close a cycle.
    void checkRouteLocked(uint32_t from, uint32_t to, const char* what) const;
    int trackIndexLocked(uint32_t trackId) const;  // -1: the master (or none)
    static int insertLatency(const Processor& insert);  // its latency (0 when switched off)
    static std::string sourceKey(const std::string& path);

    mutable std::recursive_mutex mutex_;

    AudioDevice device_;
    bool deviceRunning_ = false;
    uint32_t openInputs_ = 0;                        // input channels of the running device
    std::vector<int> openInputChannels_;             // which they are (device channels, callback order)
    std::atomic<uint32_t> pendingDeviceEvents_{0};  // DeviceEvent flags
    std::atomic<bool> liveSuspended_{false};
    std::atomic<uint64_t> audioEpoch_{0};
    std::atomic<bool> deviceRunningFlag_{false};  // deviceRunning_, for the cache's thread
    std::atomic<uint64_t> backgroundEpoch_{0};    // the background renderer's chunks (BackgroundRenderer::Watch)
    std::atomic<bool> backgroundBusy_{false};

    std::atomic<const RenderSnapshot*> snapshot_{nullptr};
    std::shared_ptr<const RenderSnapshot> snapshotHold_;
    DeferredReleasePool releasePool_;
    SharedState shared_;
    std::unique_ptr<Scheduler> scheduler_;  // the live and offline renderers share it (never at once)
    Renderer renderer_;
    RenderJob* job_ = nullptr;  // the render in the background, until finished (it holds itself)
    std::atomic<bool> requestedPlaying_{false};

    double sampleRate_ = 48000.0;
    double tempo_ = 120.0;
    int timeSigNum_ = 4;
    int timeSigDen_ = 4;
    bool loopEnabled_ = false;
    double loopStartBeat_ = 0.0;
    double loopEndBeat_ = 16.0;
    double clipFadeMs_ = 4.0;

    // Background freezing.
    CacheSettings cacheSettings_;
    uint64_t generation_ = 0;  // snapshots built

    std::vector<TrackModel> tracks_;
    uint32_t nextTrackId_ = 1;
    TrackModel master_{kMaster, std::make_shared<TrackParams>()};  // never has clips or notes
    std::unordered_map<uint32_t, ChainModel> chains_;              // id -> chain (with its owning strip and rack)
    uint32_t nextChainId_ = 1;
    std::unordered_map<uint32_t, ProcessorEntry> processors_;      // id -> chain, processor
    uint32_t nextProcessorId_ = 1;
    // Removed processors wait here until no snapshot uses them, so that they are
    // destroyed in idle(), on the main thread (plug-ins require it).
    std::vector<std::shared_ptr<Processor>> graveyard_;

    // Stretchers for live playback, per configuration. They only grow (a voice
    // may still be in use by the audio thread) until the sample rate changes.
    WarpVoiceSet warpVoices_;

    std::unordered_map<std::string, std::shared_ptr<AudioSource>> sources_;
    std::shared_ptr<const AudioSource> previewHold_;

    // The recording: the audio thread sees it through liveRecording_ (published
    // and taken away like the snapshot).
    std::unique_ptr<RecordingSession> recording_;
    std::atomic<RecordingSession*> liveRecording_{nullptr};
    std::vector<RecordedTake> finishedTakes_;

    // MIDI inputs: port ids are indices into midiPorts_ (names), kept for the
    // engine's life, so that tracks can name inputs that aren't open (yet).
    std::vector<std::string> midiPorts_;
    // The cache's own thread (background freezing): after what it reads, so it ends first.
    std::unique_ptr<CacheStore> cacheStore_;
    // Background rendering (main thread): the shadows, by the device they shadow;
    // the version of each strip they are in the state of; the live snapshot the
    // background renderer's was built from. The renderer is last: it ends first.
    bool backgroundRender_ = true;
    bool backgroundThread_ = true;
    double backgroundRate_ = 0.0;
    bool backgroundHasThread_ = false;
    std::unordered_map<const Processor*, Shadow> shadows_;
    std::unordered_map<uint32_t, uint64_t> shadowVersions_;
    uint64_t shadowGeneration_ = 0;
    std::unique_ptr<BackgroundRenderer> background_;
    MidiInputDevices midiDevices_;  // last: closed first, while the rest still stands
};

}  // namespace sub
