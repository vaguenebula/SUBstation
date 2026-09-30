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
#include "Processor.h"
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
    std::string backend;
    uint32_t sampleRate = 0;
    uint32_t bufferFrames = 0;
    double latencyMs = 0.0;
    bool exclusive = false;
};

class Engine final : private AudioCallback {
public:
    Engine();
    ~Engine() override;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // --- Device ---------------------------------------------------------------
    std::vector<OutputDeviceInfo> outputDevices();
    void openDevice(const std::string& name, uint32_t sampleRate, uint32_t bufferFrames, bool exclusive);
    void closeDevice();
    DeviceStatus deviceStatus();
    double sampleRate();
    float cpuLoad() const { return shared_.cpuLoad.load(std::memory_order_relaxed); }
    std::string takeDeviceEvent();  // "", "stopped" or "rerouted"

    // --- Sources --------------------------------------------------------------
    std::shared_ptr<AudioSource> loadSource(const std::string& path);  // cached; blocking
    std::shared_ptr<AudioSource> cachedSource(const std::string& path);
    void releaseUnusedSources();

    // --- Tracks ---------------------------------------------------------------
    uint32_t addTrack();
    void removeTrack(uint32_t trackId);
    void setTrackClips(uint32_t trackId, const std::vector<ClipDesc>& clips);
    void setTrackNotes(uint32_t trackId, const std::vector<NoteDesc>& notes);
    // Plays a note on the track's instrument right away (velocity 0 releases it),
    // e.g. while editing notes. Only heard while a device runs.
    void previewNote(uint32_t trackId, int key, int velocity);
    void setTrackGain(uint32_t trackId, float gain);
    void setTrackPan(uint32_t trackId, float pan);
    void setTrackMute(uint32_t trackId, bool mute);
    void setTrackSolo(uint32_t trackId, bool solo);
    void setMasterGain(float gain);
    std::vector<MeterReading> takeMeters();

    // --- Devices on tracks (insert chain) ------------------------------------
    uint32_t addBuiltinProcessor(uint32_t trackId, const std::string& type, int index);
    // Loads a plug-in ("VST3" format) into the chain. Main thread; throws
    // std::runtime_error with a message for the user if it can't be loaded.
    uint32_t addPluginProcessor(uint32_t trackId, const std::string& format, const std::string& path,
                                const std::string& uid, int index);
    void removeProcessor(uint32_t processorId);
    // Reorders a track's chain; `processorIds` must be the track's processors.
    void setTrackProcessorOrder(uint32_t trackId, const std::vector<uint32_t>& processorIds);
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
    void setEditorTitle(uint32_t processorId, const std::string& title);
    // What processors reported since the last call (edits in a plug-in's editor...).
    std::vector<ProcessorEventRecord> takeProcessorEvents();

    // --- Transport ------------------------------------------------------------
    void play();
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
        std::vector<std::shared_ptr<Processor>> inserts;
        std::shared_ptr<DelayLine> delay;   // delay compensation, kept across snapshots
    };

    void audioCallback(float* out, uint32_t frames, uint32_t channels) noexcept override;
    void deviceEvent(DeviceEvent event) noexcept override;

    TrackModel& trackLocked(uint32_t trackId);
    std::shared_ptr<Processor> processorLocked(uint32_t processorId);
    std::shared_ptr<Processor> processor(uint32_t processorId);  // locks
    uint32_t insertProcessorLocked(uint32_t trackId, std::shared_ptr<Processor> processor, int index);
    void retireProcessorLocked(std::shared_ptr<Processor> processor);
    void rebuildSnapshotLocked();
    void pushCommandLocked(const TransportCommand& command);
    void serviceTransportIfIdleLocked();
    void collectGarbageLocked();
    void closeDeviceLocked();
    void reloadSourcesLocked();
    void suspendLiveLocked();
    void resumeLiveLocked();
    void renderOfflineLocked(double startBeat, int64_t frames, float* out, bool loop, bool metronome);
    void prepareOfflineLocked(Renderer& offline, WarpVoiceSet& voices,
                              std::vector<std::shared_ptr<DelayLine>>& delays, double startBeat);
    void resetProcessorsLocked();
    void ensureWarpVoicesLocked(const std::array<size_t, kNumStretchConfigs>& needed);
    static std::string sourceKey(const std::string& path);

    mutable std::recursive_mutex mutex_;

    AudioDevice device_;
    bool deviceRunning_ = false;
    std::atomic<int> pendingDeviceEvent_{0};
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
    std::unordered_map<uint32_t, std::pair<uint32_t, std::shared_ptr<Processor>>> processors_;  // id -> track, processor
    uint32_t nextProcessorId_ = 1;
    // Removed processors wait here until no snapshot uses them, so that they are
    // destroyed in idle(), on the main thread (plug-ins require it).
    std::vector<std::shared_ptr<Processor>> graveyard_;

    // Stretchers for live playback, per configuration. They only grow (a voice
    // may still be in use by the audio thread) until the sample rate changes.
    WarpVoiceSet warpVoices_;

    std::unordered_map<std::string, std::shared_ptr<AudioSource>> sources_;
    std::shared_ptr<const AudioSource> previewHold_;
};

}  // namespace gil
