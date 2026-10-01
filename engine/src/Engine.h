#pragma once
// Public engine API used by the Python bindings.
//
// Threading model:
//  * The audio thread (device callback) only reads the published RenderSnapshot
//    and atomics. It never locks, allocates, frees, or touches Python.
//  * API calls may come from any Python thread. They serialise on `mutex_`,
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

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "AudioDevice.h"
#include "AudioSource.h"
#include "Automation.h"
#include "MidiInput.h"
#include "Processor.h"
#include "Recorder.h"
#include "Renderer.h"
#include "Snapshot.h"
#include "Transport.h"
#include "rt/RtUtils.h"

namespace gil {

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

struct MeterReading {
    uint32_t trackId = 0;  // 0 = master
    float left = 0.f;
    float right = 0.f;
};

// A processor's description for the UI.
struct ProcessorInfo {
    std::string typeId;
    std::string name;
    int latency = 0;  // samples
    int tail = 0;
    bool hasEditor = false;
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
    void setMasterGain(float gain) { setTrackGain(kMaster, gain); }
    void setMasterPan(float pan) { setTrackPan(kMaster, pan); }
    std::vector<MeterReading> takeMeters();

    // --- Input and recording ------------------------------------------------------
    // A track's input: device channels (0-based, as DeviceStatus lists them): none,
    // one (mono) or two (a stereo pair). Channels the device hasn't open are silent.
    void setTrackInput(uint32_t trackId, const std::vector<int>& channels);
    void setTrackMonitor(uint32_t trackId, MonitorMode mode);
    // Armed tracks are what Auto monitoring listens to; what records is up to startRecording().
    void setTrackArmed(uint32_t trackId, bool armed);
    // Records the targets' inputs from the next block the playhead moves in, and
    // starts playing (after `countInBeats` of count-in) if stopped. Throws
    // std::runtime_error (for the user) if no device runs, a target's input isn't
    // open, or a file can't be created; std::invalid_argument for a bad target.
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
    // target, either a mixer control (processorId 0, param "volume" or "pan") or
    // a parameter (by id) of one of the track's devices. Envelopes of devices
    // that are gone, or parameters they don't have, are kept but not played.
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
    void removeProcessor(uint32_t processorId);
    // Reorders a chain; `processorIds` must be the chain's processors, each once.
    void setChainOrder(uint32_t chainId, const std::vector<uint32_t>& processorIds);
    // Moves a processor to position `index` of a chain (its own or another, on
    // any strip), as the chain is without it (-1: last). Its state goes with it;
    // its automation stays with the strip it came from, and plays again if it comes back.
    void moveProcessor(uint32_t processorId, uint32_t toChainId, int index);
    ProcessorInfo processorInfo(uint32_t processorId);
    std::vector<ParamInfo> processorParams(uint32_t processorId);
    int processorParamIndex(uint32_t processorId, const std::string& paramId);  // -1: no such parameter
    float processorParam(uint32_t processorId, int index);
    void setProcessorParam(uint32_t processorId, int index, float value);
    std::string processorParamText(uint32_t processorId, int index, float value);
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
    void setClipFadeMs(double ms);

    // --- Browser preview ------------------------------------------------------
    void preview(const std::string& path);  // the source must already be loaded
    void stopPreview();
    bool isPreviewing() const { return shared_.previewActive.load(std::memory_order_relaxed); }

    // --- Offline rendering ----------------------------------------------------
    // Both temporarily silence live output and render the same graph.
    std::vector<float> renderOffline(double startBeat, int64_t frames, bool loop = false,
                                     bool metronome = false);  // interleaved stereo
    void exportWav(const std::string& path, double startBeat, double endBeat, int bitDepth);

    // --- Housekeeping ---------------------------------------------------------
    // Call regularly from the UI thread: frees retired snapshots and removed
    // processors, handles device loss, and does the main-thread work plug-ins
    // asked for (restarts, parameter updates, editor events).
    void idle();

private:
    struct TrackModel {
        uint32_t id = 0;
        std::shared_ptr<TrackParams> params;
        std::vector<ClipDesc> clips;
        std::vector<std::string> clipKeys;  // sourceKey() of each clip's path
        std::vector<NoteDesc> notes;
        uint32_t chainId = 0;               // its main chain
        std::shared_ptr<DelayLine> delay;   // delay compensation, kept across snapshots
        std::vector<AutomationLaneDesc> automation;
        std::vector<int> inputChannels;  // device channels: the input edge
        MidiInputRoute midiInput;
        MonitorMode monitor = MonitorMode::Auto;
        bool armed = false;
    };

    void audioCallback(const AudioIO& io) noexcept override;
    void deviceEvent(DeviceEvent event) noexcept override;
    // Any thread: a MIDI message from input `port`, stamped and queued for the audio thread.
    void midiInput(uint16_t port, const uint8_t* message, int size, int64_t hostTimeNs) noexcept;
    uint16_t midiPortLocked(const std::string& name);
    void discardMidiInputLocked();

    // A chain of devices: a strip's main chain, or (later) a chain of a rack on it.
    struct ChainModel {
        uint32_t id = 0;
        uint32_t stripId = 0;     // the track (or kMaster) whose signal it processes
        uint32_t parentRack = 0;  // the rack processor it belongs to; 0: a strip's main chain
        std::vector<std::shared_ptr<Processor>> inserts;
    };
    struct ProcessorEntry {
        uint32_t chainId = 0;
        std::shared_ptr<Processor> processor;
    };

    TrackModel& trackLocked(uint32_t trackId);           // the master too
    TrackModel& arrangementTrackLocked(uint32_t trackId);  // not the master
    std::shared_ptr<Processor> processorLocked(uint32_t processorId);
    std::shared_ptr<Processor> processor(uint32_t processorId);  // locks
    ChainModel& chainLocked(uint32_t chainId);
    const std::vector<std::shared_ptr<Processor>>& insertsLocked(const TrackModel& track) const;
    uint32_t addChainLocked(uint32_t stripId, uint32_t parentRack);
    uint32_t insertProcessorLocked(uint32_t chainId, std::shared_ptr<Processor> processor, int index);
    void retireProcessorLocked(std::shared_ptr<Processor> processor);
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
    void prepareOfflineLocked(Renderer& offline, WarpVoiceSet& voices,
                              std::vector<std::shared_ptr<DelayLine>>& delays, double startBeat);
    void resetProcessorsLocked();
    void ensureWarpVoicesLocked(const std::array<size_t, kNumStretchConfigs>& needed);
    // A strip's envelopes in the snapshot, in samples. `inputLatency`: how late
    // the strip's input hears the timeline (0 for a track; the tracks' latency
    // for the master). Its devices hear it that much later, plus the latency of
    // the devices before them; its fader after all of them and its compensation.
    void buildAutomationLocked(const TrackModel& track, int inputLatency, int faderLatency, double samplesPerBeat,
                               StripRender& strip);
    static int insertLatency(const std::vector<std::shared_ptr<Processor>>& inserts);
    static std::string sourceKey(const std::string& path);

    mutable std::recursive_mutex mutex_;

    AudioDevice device_;
    bool deviceRunning_ = false;
    uint32_t openInputs_ = 0;                        // input channels of the running device
    std::vector<int> openInputChannels_;             // which they are (device channels, callback order)
    std::atomic<uint32_t> pendingDeviceEvents_{0};  // DeviceEvent flags
    std::atomic<bool> liveSuspended_{false};
    std::atomic<uint64_t> audioEpoch_{0};

    std::atomic<const RenderSnapshot*> snapshot_{nullptr};
    std::shared_ptr<const RenderSnapshot> snapshotHold_;
    DeferredReleasePool releasePool_;
    SharedState shared_;
    Renderer renderer_;
    std::atomic<bool> requestedPlaying_{false};

    double sampleRate_ = 48000.0;
    double tempo_ = 120.0;
    int timeSigNum_ = 4;
    int timeSigDen_ = 4;
    bool loopEnabled_ = false;
    double loopStartBeat_ = 0.0;
    double loopEndBeat_ = 16.0;
    double clipFadeMs_ = 4.0;

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
    MidiInputDevices midiDevices_;  // last: closed first, while the rest still stands
};

}  // namespace gil
