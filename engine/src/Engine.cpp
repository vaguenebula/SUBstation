#include "Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <stdexcept>
#include <thread>
#include <unordered_set>

#include "PathUtils.h"
#include "miniaudio.h"
#include "processors/Utility.h"

namespace gil {
namespace {

// Runs a callable when the scope ends (used to resume live output).
template <typename F>
class ScopeExit {
public:
    explicit ScopeExit(F f) : f_(std::move(f)) {}
    ~ScopeExit() { f_(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    F f_;
};

}  // namespace

Engine::Engine() {
    std::lock_guard lock(mutex_);
    renderer_.prepare(sampleRate_);
    rebuildSnapshotLocked();
}

Engine::~Engine() {
    std::lock_guard lock(mutex_);
    closeDeviceLocked();
}

std::string Engine::sourceKey(const std::string& path) {
    std::wstring wide = pathFromUtf8(path).lexically_normal().make_preferred().wstring();
    std::transform(wide.begin(), wide.end(), wide.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    const std::u8string utf8 = std::filesystem::path(wide).u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

// ---------------------------------------------------------------------------
// Device

std::vector<OutputDeviceInfo> Engine::outputDevices() {
    std::lock_guard lock(mutex_);
    return device_.outputDevices();
}

void Engine::openDevice(const std::string& name, uint32_t sampleRate, uint32_t bufferFrames, bool exclusive) {
    std::lock_guard lock(mutex_);
    closeDeviceLocked();
    device_.init(name, sampleRate, bufferFrames, exclusive, this);
    try {
        const double rate = device_.sampleRate();
        if (rate != sampleRate_) {
            sampleRate_ = rate;
            reloadSourcesLocked();
            warpVoices_ = {};  // stretchers are sized for the old rate; the next snapshot makes new ones
        }
        // The audio thread is not running, so the preview can be dropped directly.
        shared_.previewSource.store(nullptr, std::memory_order_seq_cst);
        shared_.previewSerial.fetch_add(1, std::memory_order_seq_cst);
        shared_.previewActive.store(false);
        previewHold_.reset();

        renderer_.prepare(sampleRate_);
        for (auto& track : tracks_) {
            for (auto& insert : track.inserts) insert->prepare(sampleRate_, Renderer::kMaxBlock);
        }
        rebuildSnapshotLocked();
        serviceTransportIfIdleLocked();
        pendingDeviceEvent_.store(0);
        device_.start();
        deviceRunning_ = true;
    } catch (...) {
        device_.close();
        deviceRunning_ = false;
        throw;
    }
}

void Engine::closeDevice() {
    std::lock_guard lock(mutex_);
    closeDeviceLocked();
}

void Engine::closeDeviceLocked() {
    if (device_.isOpen()) device_.close();  // waits for the audio thread to exit
    deviceRunning_ = false;
    collectGarbageLocked();
    serviceTransportIfIdleLocked();
}

DeviceStatus Engine::deviceStatus() {
    std::lock_guard lock(mutex_);
    DeviceStatus status;
    status.open = deviceRunning_;
    status.backend = device_.backendName();
    if (device_.isOpen()) {
        status.name = device_.deviceName();
        status.sampleRate = device_.sampleRate();
        status.bufferFrames = device_.bufferFrames();
        status.latencyMs = device_.latencySeconds() * 1000.0;
        status.exclusive = device_.exclusive();
    }
    return status;
}

double Engine::sampleRate() {
    std::lock_guard lock(mutex_);
    return sampleRate_;
}

std::string Engine::takeDeviceEvent() {
    switch (static_cast<DeviceEvent>(pendingDeviceEvent_.exchange(0))) {
        case DeviceEvent::Stopped: return "stopped";
        case DeviceEvent::Rerouted: return "rerouted";
        default: return "";
    }
}

void Engine::deviceEvent(DeviceEvent event) noexcept { pendingDeviceEvent_.store(static_cast<int>(event)); }

void Engine::audioCallback(float* out, uint32_t frames, uint32_t channels) noexcept {
    ScopedNoDenormals noDenormals;
    const auto started = std::chrono::steady_clock::now();
    // seq_cst pairs with the store/epoch-load sequence in rebuildSnapshotLocked().
    const RenderSnapshot* snap = snapshot_.load(std::memory_order_seq_cst);
    if (!snap || liveSuspended_.load(std::memory_order_seq_cst)) {
        std::fill_n(out, static_cast<size_t>(frames) * channels, 0.f);
    } else {
        renderer_.processLive(*snap, shared_, out, frames, channels);
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const double budget = frames / snap->sampleRate;
        const float load = static_cast<float>(elapsed / budget);
        const float previous = shared_.cpuLoad.load(std::memory_order_relaxed);
        shared_.cpuLoad.store(previous + 0.1f * (load - previous), std::memory_order_relaxed);
    }
    audioEpoch_.fetch_add(1, std::memory_order_seq_cst);
}

// ---------------------------------------------------------------------------
// Sources

std::shared_ptr<AudioSource> Engine::loadSource(const std::string& path) {
    const std::string key = sourceKey(path);
    uint32_t rate = 0;
    {
        std::lock_guard lock(mutex_);
        auto it = sources_.find(key);
        if (it != sources_.end() && it->second->sampleRate() == static_cast<uint32_t>(sampleRate_)) return it->second;
        rate = static_cast<uint32_t>(sampleRate_);
    }
    // Decode without holding the lock; retry if the device changed rate meanwhile.
    for (int attempt = 0; attempt < 4; ++attempt) {
        auto source = AudioSource::load(path, rate);
        std::lock_guard lock(mutex_);
        const auto current = static_cast<uint32_t>(sampleRate_);
        if (current != rate) {
            rate = current;
            continue;
        }
        auto& slot = sources_[key];
        if (slot && slot->sampleRate() == rate) return slot;
        slot = std::move(source);
        rebuildSnapshotLocked();  // clips waiting for this file become audible
        return slot;
    }
    throw std::runtime_error("The sample rate kept changing while loading " + path);
}

std::shared_ptr<AudioSource> Engine::cachedSource(const std::string& path) {
    std::lock_guard lock(mutex_);
    auto it = sources_.find(sourceKey(path));
    return it == sources_.end() ? nullptr : it->second;
}

void Engine::releaseUnusedSources() {
    std::lock_guard lock(mutex_);
    std::unordered_set<std::string> used;
    for (const auto& track : tracks_) used.insert(track.clipKeys.begin(), track.clipKeys.end());
    std::erase_if(sources_, [&](const auto& entry) {
        return !used.contains(entry.first) && entry.second != previewHold_;
    });
}

void Engine::reloadSourcesLocked() {
    std::unordered_map<std::string, std::shared_ptr<AudioSource>> reloaded;
    for (const auto& [key, source] : sources_) {
        try {
            reloaded[key] = AudioSource::load(source->path(), static_cast<uint32_t>(sampleRate_));
        } catch (const std::exception&) {
            // The file vanished or became unreadable; its clips go silent.
        }
    }
    sources_ = std::move(reloaded);
}

// ---------------------------------------------------------------------------
// Tracks

Engine::TrackModel& Engine::trackLocked(uint32_t trackId) {
    for (auto& track : tracks_) {
        if (track.id == trackId) return track;
    }
    throw std::invalid_argument("Unknown track id " + std::to_string(trackId));
}

uint32_t Engine::addTrack() {
    std::lock_guard lock(mutex_);
    TrackModel track;
    track.id = nextTrackId_++;
    track.params = std::make_shared<TrackParams>();
    tracks_.push_back(std::move(track));
    rebuildSnapshotLocked();
    return tracks_.back().id;
}

void Engine::removeTrack(uint32_t trackId) {
    std::lock_guard lock(mutex_);
    trackLocked(trackId);
    std::erase_if(tracks_, [&](const TrackModel& track) { return track.id == trackId; });
    std::erase_if(processors_, [&](const auto& entry) { return entry.second.first == trackId; });
    rebuildSnapshotLocked();
}

void Engine::setTrackClips(uint32_t trackId, const std::vector<ClipDesc>& clips) {
    std::lock_guard lock(mutex_);
    TrackModel& track = trackLocked(trackId);
    track.clips = clips;
    track.clipKeys.clear();
    for (const auto& clip : clips) track.clipKeys.push_back(sourceKey(clip.path));
    rebuildSnapshotLocked();
}

void Engine::setTrackGain(uint32_t trackId, float gain) {
    std::lock_guard lock(mutex_);
    trackLocked(trackId).params->gain.store(std::max(0.f, gain));
}

void Engine::setTrackPan(uint32_t trackId, float pan) {
    std::lock_guard lock(mutex_);
    trackLocked(trackId).params->pan.store(std::clamp(pan, -1.f, 1.f));
}

void Engine::setTrackMute(uint32_t trackId, bool mute) {
    std::lock_guard lock(mutex_);
    trackLocked(trackId).params->mute.store(mute);
}

void Engine::setTrackSolo(uint32_t trackId, bool solo) {
    std::lock_guard lock(mutex_);
    trackLocked(trackId).params->solo.store(solo);
}

void Engine::setMasterGain(float gain) { shared_.masterGain.store(std::max(0.f, gain)); }

std::vector<MeterReading> Engine::takeMeters() {
    std::lock_guard lock(mutex_);
    std::vector<MeterReading> meters;
    meters.reserve(tracks_.size() + 1);
    meters.push_back({0, shared_.masterPeakLeft.exchange(0.f), shared_.masterPeakRight.exchange(0.f)});
    for (const auto& track : tracks_) {
        meters.push_back({track.id, track.params->peakLeft.exchange(0.f), track.params->peakRight.exchange(0.f)});
    }
    return meters;
}

// ---------------------------------------------------------------------------
// Insert chain

std::shared_ptr<Processor> Engine::processorLocked(uint32_t processorId) {
    auto it = processors_.find(processorId);
    if (it == processors_.end()) throw std::invalid_argument("Unknown device id " + std::to_string(processorId));
    return it->second.second;
}

uint32_t Engine::addBuiltinProcessor(uint32_t trackId, const std::string& type, int index) {
    std::shared_ptr<Processor> processor;
    if (type == "utility") {
        processor = std::make_shared<UtilityProcessor>();
    } else {
        throw std::invalid_argument("Unknown built-in device: " + type);
    }
    std::lock_guard lock(mutex_);
    TrackModel& track = trackLocked(trackId);
    processor->prepare(sampleRate_, Renderer::kMaxBlock);  // before the audio thread can see it
    const auto count = static_cast<int>(track.inserts.size());
    const int at = (index < 0 || index > count) ? count : index;
    track.inserts.insert(track.inserts.begin() + at, processor);
    const uint32_t id = nextProcessorId_++;
    processors_[id] = {trackId, processor};
    rebuildSnapshotLocked();
    return id;
}

void Engine::removeProcessor(uint32_t processorId) {
    std::lock_guard lock(mutex_);
    auto processor = processorLocked(processorId);
    TrackModel& track = trackLocked(processors_[processorId].first);
    std::erase(track.inserts, processor);
    processors_.erase(processorId);
    rebuildSnapshotLocked();  // the old snapshot (and the processor) is freed later, off the audio thread
}

std::vector<ParamInfo> Engine::processorParams(uint32_t processorId) {
    std::lock_guard lock(mutex_);
    return processorLocked(processorId)->params();
}

float Engine::processorParam(uint32_t processorId, int index) {
    std::lock_guard lock(mutex_);
    return processorLocked(processorId)->getParam(index);
}

void Engine::setProcessorParam(uint32_t processorId, int index, float value) {
    std::lock_guard lock(mutex_);
    processorLocked(processorId)->setParam(index, value);
}

void Engine::setProcessorEnabled(uint32_t processorId, bool enabled) {
    std::lock_guard lock(mutex_);
    processorLocked(processorId)->setEnabled(enabled);
}

// ---------------------------------------------------------------------------
// Transport

void Engine::pushCommandLocked(const TransportCommand& command) {
    shared_.commands.push(command);  // 256 slots; a full queue only drops UI requests
    serviceTransportIfIdleLocked();
}

void Engine::serviceTransportIfIdleLocked() {
    // Without a running device nobody else consumes transport commands, so the
    // edit side applies them to keep the state consistent.
    if (deviceRunning_ || !snapshotHold_) return;
    renderer_.syncTempo(*snapshotHold_);
    renderer_.drainCommands(shared_);
    renderer_.publishTransport(shared_);
}

void Engine::play() {
    std::lock_guard lock(mutex_);
    requestedPlaying_.store(true);
    pushCommandLocked({TransportCommand::Type::Play});
}

void Engine::stop() {
    std::lock_guard lock(mutex_);
    requestedPlaying_.store(false);
    pushCommandLocked({TransportCommand::Type::Stop});
}

void Engine::setPositionBeats(double beat) {
    std::lock_guard lock(mutex_);
    beat = std::max(0.0, beat);
    shared_.positionBeats.store(beat);  // immediate feedback; the audio thread confirms it
    pushCommandLocked({TransportCommand::Type::Locate, beat});
}

void Engine::setTempo(double bpm) {
    std::lock_guard lock(mutex_);
    tempo_ = std::clamp(bpm, 20.0, 999.0);
    rebuildSnapshotLocked();
}

double Engine::tempo() {
    std::lock_guard lock(mutex_);
    return tempo_;
}

void Engine::setTimeSignature(int numerator, int denominator) {
    const bool powerOfTwo = denominator > 0 && (denominator & (denominator - 1)) == 0;
    if (numerator < 1 || numerator > 32 || !powerOfTwo || denominator > 32) {
        throw std::invalid_argument("Unsupported time signature");
    }
    std::lock_guard lock(mutex_);
    timeSigNum_ = numerator;
    timeSigDen_ = denominator;
    rebuildSnapshotLocked();
}

void Engine::setLoop(bool enabled, double startBeat, double endBeat) {
    std::lock_guard lock(mutex_);
    loopEnabled_ = enabled;
    loopStartBeat_ = std::max(0.0, startBeat);
    loopEndBeat_ = std::max(loopStartBeat_, endBeat);
    rebuildSnapshotLocked();
}

void Engine::setMetronome(bool enabled) { shared_.metronome.store(enabled); }

void Engine::setClipFadeMs(double ms) {
    std::lock_guard lock(mutex_);
    clipFadeMs_ = std::clamp(ms, 0.0, 100.0);
    rebuildSnapshotLocked();
}

// ---------------------------------------------------------------------------
// Preview

void Engine::preview(const std::string& path) {
    std::lock_guard lock(mutex_);
    auto it = sources_.find(sourceKey(path));
    if (it == sources_.end() || it->second->sampleRate() != static_cast<uint32_t>(sampleRate_)) {
        throw std::invalid_argument("Audio file is not loaded: " + path);
    }
    std::shared_ptr<const AudioSource> old = std::move(previewHold_);
    previewHold_ = it->second;
    shared_.previewSource.store(previewHold_.get(), std::memory_order_seq_cst);
    shared_.previewActive.store(true);
    shared_.previewSerial.fetch_add(1, std::memory_order_seq_cst);
    releasePool_.retire(std::move(old), audioEpoch_.load(std::memory_order_seq_cst));
}

void Engine::stopPreview() {
    std::lock_guard lock(mutex_);
    std::shared_ptr<const AudioSource> old = std::move(previewHold_);
    shared_.previewSource.store(nullptr, std::memory_order_seq_cst);
    shared_.previewActive.store(false);
    shared_.previewSerial.fetch_add(1, std::memory_order_seq_cst);
    releasePool_.retire(std::move(old), audioEpoch_.load(std::memory_order_seq_cst));
}

// ---------------------------------------------------------------------------
// Offline rendering

void Engine::suspendLiveLocked() {
    if (!deviceRunning_) return;
    liveSuspended_.store(true, std::memory_order_seq_cst);
    // Once the epoch advances, any callback that started before the flag was
    // visible has finished; later callbacks output silence.
    const uint64_t start = audioEpoch_.load(std::memory_order_seq_cst);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (audioEpoch_.load(std::memory_order_seq_cst) <= start && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Engine::resumeLiveLocked() { liveSuspended_.store(false, std::memory_order_seq_cst); }

void Engine::prepareOfflineLocked(Renderer& offline, WarpVoiceSet& voices, double startBeat) {
    // Fresh stretchers, as many as live playback has, so an offline render
    // starts from a clean state and leaves the live voices alone.
    for (int c = 0; c < kNumStretchConfigs; ++c) {
        for (size_t i = 0; i < snapshotHold_->warpVoices[c].size(); ++i) {
            voices[c].push_back(std::make_shared<WarpVoice>(static_cast<StretchConfig>(c), sampleRate_));
        }
    }
    offline.prepare(sampleRate_);
    offline.setWarpVoices(&voices);
    offline.syncTempo(*snapshotHold_);
    offline.setPosition(std::llround(std::max(0.0, startBeat) * snapshotHold_->samplesPerBeat()));
    offline.setPlaying(true);
}

void Engine::renderOfflineLocked(double startBeat, int64_t frames, float* out, bool loop, bool metronome) {
    suspendLiveLocked();
    ScopeExit resume([this] { resumeLiveLocked(); });
    Renderer offline;
    WarpVoiceSet voices;
    prepareOfflineLocked(offline, voices, startBeat);
    offline.renderOffline(*snapshotHold_, shared_, out, frames, loop, metronome);
}

std::vector<float> Engine::renderOffline(double startBeat, int64_t frames, bool loop, bool metronome) {
    if (frames < 0) throw std::invalid_argument("frames must be >= 0");
    std::vector<float> out(static_cast<size_t>(frames) * 2);
    std::lock_guard lock(mutex_);
    renderOfflineLocked(startBeat, frames, out.data(), loop, metronome);
    return out;
}

void Engine::exportWav(const std::string& path, double startBeat, double endBeat, int bitDepth) {
    if (endBeat <= startBeat) throw std::invalid_argument("Export range is empty");
    ma_format format;
    switch (bitDepth) {
        case 16: format = ma_format_s16; break;
        case 24: format = ma_format_s24; break;
        case 32: format = ma_format_f32; break;
        default: throw std::invalid_argument("Bit depth must be 16, 24 or 32");
    }

    std::lock_guard lock(mutex_);
    const double spb = snapshotHold_->samplesPerBeat();
    const int64_t total = std::llround((endBeat - startBeat) * spb);

    ma_encoder_config config =
        ma_encoder_config_init(ma_encoding_format_wav, format, 2, static_cast<ma_uint32>(sampleRate_));
    ma_encoder encoder;
    if (ma_encoder_init_file_w(widen(path).c_str(), &config, &encoder) != MA_SUCCESS) {
        throw std::runtime_error("Could not create " + path);
    }

    suspendLiveLocked();
    ScopeExit resume([this] { resumeLiveLocked(); });
    Renderer offline;
    WarpVoiceSet voices;
    prepareOfflineLocked(offline, voices, startBeat);

    constexpr int64_t kChunk = 16384;
    std::vector<float> rendered(kChunk * 2);
    std::vector<uint8_t> converted(kChunk * 2 * sizeof(float));
    const ma_dither_mode dither = bitDepth == 16 ? ma_dither_mode_triangle : ma_dither_mode_none;
    for (int64_t done = 0; done < total;) {
        const int64_t n = std::min(kChunk, total - done);
        offline.renderOffline(*snapshotHold_, shared_, rendered.data(), n);
        ma_pcm_convert(converted.data(), format, rendered.data(), ma_format_f32, static_cast<ma_uint64>(n * 2), dither);
        ma_encoder_write_pcm_frames(&encoder, converted.data(), static_cast<ma_uint64>(n), nullptr);
        done += n;
    }
    ma_encoder_uninit(&encoder);
}

// ---------------------------------------------------------------------------
// Snapshot publishing and housekeeping

void Engine::rebuildSnapshotLocked() {
    auto snap = std::make_shared<RenderSnapshot>();
    snap->sampleRate = sampleRate_;
    snap->tempo = tempo_;
    snap->timeSigNum = timeSigNum_;
    snap->timeSigDen = timeSigDen_;
    const double spb = snap->samplesPerBeat();
    snap->loopStart = std::llround(loopStartBeat_ * spb);
    snap->loopEnd = std::llround(loopEndBeat_ * spb);
    snap->loopEnabled = loopEnabled_ && snap->loopEnd - snap->loopStart >= 256;
    snap->clipFadeSamples = std::llround(clipFadeMs_ * 0.001 * sampleRate_);

    const auto rate = static_cast<uint32_t>(sampleRate_);
    std::array<size_t, kNumStretchConfigs> voicesNeeded{};
    snap->tracks.reserve(tracks_.size());
    for (const TrackModel& track : tracks_) {
        TrackRender render;
        render.id = track.id;
        render.params = track.params;
        render.inserts = track.inserts;
        std::array<size_t, kNumStretchConfigs> stretching{};
        for (size_t i = 0; i < track.clips.size(); ++i) {
            const ClipDesc& clip = track.clips[i];
            auto it = sources_.find(track.clipKeys[i]);
            if (it == sources_.end() || it->second->sampleRate() != rate) continue;  // still loading
            const auto& source = it->second;
            ClipRender cr;
            cr.source = source;
            cr.start = std::max<int64_t>(0, std::llround(clip.startBeat * spb));
            cr.sourceOffset = std::clamp<int64_t>(std::llround(clip.offsetSec * sampleRate_), 0, source->frames());
            const bool warped = clip.warp && clip.segmentBpm > 0.0;
            if (warped) {
                // Locked to beats: both ends sit on their beats at any tempo.
                cr.rate = tempo_ / clip.segmentBpm;
                const double endBeat = clip.startBeat + clip.durationSec * clip.segmentBpm / 60.0;
                cr.length = std::llround(endBeat * spb) - cr.start;
            } else {
                cr.length = std::llround(clip.durationSec * sampleRate_);
            }
            // Never play past the end of the file.
            const auto available = static_cast<double>(source->frames() - cr.sourceOffset);
            cr.length = std::min<int64_t>(cr.length, static_cast<int64_t>(std::floor(available / cr.rate)));
            if (cr.length <= 0) continue;

            cr.gain = clip.gain;
            balanceGains(clip.pan, cr.panLeft, cr.panRight);
            const bool repitch = warped && clip.warpMode == WarpMode::RePitch;
            const bool speedChanges = std::abs(cr.rate - 1.0) > 1e-9;
            if (repitch) {
                cr.playback = speedChanges ? ClipRender::Playback::Resample : ClipRender::Playback::Direct;
            } else if (speedChanges || clip.transpose != 0.0) {
                cr.playback = ClipRender::Playback::Stretch;
                cr.stretchConfig = stretchConfigFor(clip.warpMode);
                cr.transpose = static_cast<float>(clip.transpose);
                cr.preserveFormants = clip.warpMode == WarpMode::ComplexPro;
                ++stretching[static_cast<size_t>(cr.stretchConfig)];
            }
            const uint64_t identity = clip.id.empty() ? std::hash<size_t>{}(i) : std::hash<std::string>{}(clip.id);
            cr.key = identity ^ (static_cast<uint64_t>(track.id) * 0x9E3779B97F4A7C15ull);

            render.maxClipLength = std::max(render.maxClipLength, cr.length);
            render.clips.push_back(std::move(cr));
        }
        // Clips on a track don't overlap, so only a few play in any one block
        // (more only if they are shorter than a block).
        for (int c = 0; c < kNumStretchConfigs; ++c) voicesNeeded[c] += std::min<size_t>(stretching[c], 3);
        std::sort(render.clips.begin(), render.clips.end(),
                  [](const ClipRender& a, const ClipRender& b) { return a.start < b.start; });
        snap->tracks.push_back(std::move(render));
    }
    ensureWarpVoicesLocked(voicesNeeded);
    snap->warpVoices = warpVoices_;

    std::shared_ptr<const RenderSnapshot> old = std::move(snapshotHold_);
    snapshotHold_ = snap;
    // Publish, then read the epoch (both seq_cst; see DeferredReleasePool).
    snapshot_.store(snap.get(), std::memory_order_seq_cst);
    releasePool_.retire(std::move(old), audioEpoch_.load(std::memory_order_seq_cst));
    collectGarbageLocked();
    serviceTransportIfIdleLocked();
}

void Engine::ensureWarpVoicesLocked(const std::array<size_t, kNumStretchConfigs>& needed) {
    constexpr size_t kMaxVoices = 64;  // per configuration
    for (int c = 0; c < kNumStretchConfigs; ++c) {
        auto& pool = warpVoices_[c];
        while (pool.size() < std::min(needed[c], kMaxVoices)) {
            pool.push_back(std::make_shared<WarpVoice>(static_cast<StretchConfig>(c), sampleRate_));
        }
    }
}

void Engine::collectGarbageLocked() {
    releasePool_.collect(audioEpoch_.load(std::memory_order_seq_cst), !deviceRunning_);
}

void Engine::idle() {
    std::lock_guard lock(mutex_);
    if (deviceRunning_ && pendingDeviceEvent_.load() == static_cast<int>(DeviceEvent::Stopped)) {
        closeDeviceLocked();  // the backend lost the device; the UI reports it via takeDeviceEvent()
    }
    collectGarbageLocked();
    serviceTransportIfIdleLocked();
}

}  // namespace gil
