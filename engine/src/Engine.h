#pragma once
// Public engine API used by the Python bindings.
//
// Threading model:
//  * The audio thread (device callback) only reads the published RenderSnapshot
//    and atomics. It never locks, allocates, frees, or touches Python.
//  * API calls may come from any Python thread. They serialise on `mutex_`,
//    mutate the edit model, then rebuild and publish a new snapshot.

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
    WarpMode warpMode = WarpMode::Beats;
    double transpose = 0.0;     // semitones (fractions for detune); ignored by Re-Pitch
    std::string id;             // stable clip identity, so edits don't interrupt a stretching clip
};

struct MeterReading {
    uint32_t trackId = 0;  // 0 = master
    float left = 0.f;
    float right = 0.f;
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
    void setTrackGain(uint32_t trackId, float gain);
    void setTrackPan(uint32_t trackId, float pan);
    void setTrackMute(uint32_t trackId, bool mute);
    void setTrackSolo(uint32_t trackId, bool solo);
    void setMasterGain(float gain);
    std::vector<MeterReading> takeMeters();

    // --- Devices on tracks (insert chain) ------------------------------------
    uint32_t addBuiltinProcessor(uint32_t trackId, const std::string& type, int index);
    void removeProcessor(uint32_t processorId);
    std::vector<ParamInfo> processorParams(uint32_t processorId);
    float processorParam(uint32_t processorId, int index);
    void setProcessorParam(uint32_t processorId, int index, float value);
    void setProcessorEnabled(uint32_t processorId, bool enabled);

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
    // Call regularly from the UI thread: frees retired snapshots, handles device
    // loss. Future home of plugin main-thread callbacks.
    void idle();

private:
    struct TrackModel {
        uint32_t id = 0;
        std::shared_ptr<TrackParams> params;
        std::vector<ClipDesc> clips;
        std::vector<std::string> clipKeys;  // sourceKey() of each clip's path
        std::vector<std::shared_ptr<Processor>> inserts;
    };

    void audioCallback(float* out, uint32_t frames, uint32_t channels) noexcept override;
    void deviceEvent(DeviceEvent event) noexcept override;

    TrackModel& trackLocked(uint32_t trackId);
    std::shared_ptr<Processor> processorLocked(uint32_t processorId);
    void rebuildSnapshotLocked();
    void pushCommandLocked(const TransportCommand& command);
    void serviceTransportIfIdleLocked();
    void collectGarbageLocked();
    void closeDeviceLocked();
    void reloadSourcesLocked();
    void suspendLiveLocked();
    void resumeLiveLocked();
    void renderOfflineLocked(double startBeat, int64_t frames, float* out, bool loop, bool metronome);
    void prepareOfflineLocked(Renderer& offline, WarpVoiceSet& voices, double startBeat);
    void ensureWarpVoicesLocked(const std::array<size_t, kNumStretchConfigs>& needed);
    static std::string sourceKey(const std::string& path);

    mutable std::mutex mutex_;

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
    std::unordered_map<uint32_t, std::pair<uint32_t, std::shared_ptr<Processor>>> processors_;
    uint32_t nextProcessorId_ = 1;

    // Stretchers for live playback, per configuration. They only grow (a voice
    // may still be in use by the audio thread) until the sample rate changes.
    WarpVoiceSet warpVoices_;

    std::unordered_map<std::string, std::shared_ptr<AudioSource>> sources_;
    std::shared_ptr<const AudioSource> previewHold_;
};

}  // namespace gil
