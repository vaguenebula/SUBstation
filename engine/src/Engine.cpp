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
#include "plugins/Vst3Format.h"
#include "processors/Synth.h"
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

std::vector<AudioDeviceInfo> Engine::devices(const std::string& driver) {
    std::lock_guard lock(mutex_);
    return device_.devices(driver);
}

void Engine::openDevice(const DeviceConfig& config) {
    std::lock_guard lock(mutex_);
    openDeviceLocked(config);
}

void Engine::reopenDevice() {
    std::lock_guard lock(mutex_);
    if (!device_.hasConfig()) throw std::runtime_error("No audio device has been opened");
    openDeviceLocked(device_.resetConfig());  // asks the open driver first, so before closing it
}

void Engine::openDeviceLocked(const DeviceConfig& config) {
    // A driver's control panel may run a message loop that calls us back: its
    // driver has to stay until the panel is closed.
    if (device_.inControlPanel()) throw std::runtime_error("Close the driver's control panel first");
    closeDeviceLocked();
    device_.open(config, this);
    try {
        const DeviceState state = device_.state();
        const double rate = state.sampleRate;
        if (rate != sampleRate_) {
            sampleRate_ = rate;
            reloadSourcesLocked();
            warpVoices_ = {};  // stretchers are sized for the old rate; the next snapshot makes new ones
        }
        openInputs_ = static_cast<uint32_t>(state.inputChannels.size());
        for (auto& peak : shared_.inputPeaks) peak.store(0.f, std::memory_order_relaxed);
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
        pendingDeviceEvents_.store(0);  // about the device just closed
        device_.start();
        deviceRunning_ = true;
    } catch (...) {
        device_.close();
        deviceRunning_ = false;
        openInputs_ = 0;
        throw;
    }
}

void Engine::closeDevice() {
    std::lock_guard lock(mutex_);
    if (device_.inControlPanel()) throw std::runtime_error("Close the driver's control panel first");
    closeDeviceLocked();
}

void Engine::closeDeviceLocked() {
    if (device_.isOpen()) device_.close();  // waits for the audio thread to exit
    deviceRunning_ = false;
    openInputs_ = 0;
    collectGarbageLocked();
    serviceTransportIfIdleLocked();
}

DeviceStatus Engine::deviceStatus() {
    std::lock_guard lock(mutex_);
    DeviceStatus status;
    status.open = deviceRunning_;
    if (device_.isOpen()) {
        const DeviceState state = device_.state();
        status.name = state.name;
        status.backend = state.driver;
        status.sampleRate = state.sampleRate;
        status.bufferFrames = state.bufferFrames;
        if (state.sampleRate > 0) {
            status.latencyMs = state.outputLatency * 1000.0 / state.sampleRate;
            status.inputLatencyMs = state.inputLatency * 1000.0 / state.sampleRate;
        }
        status.inputChannels = state.inputChannels;
        status.outputChannels = state.outputChannels;
        status.exclusive = state.exclusive;
    }
    return status;
}

DeviceCaps Engine::deviceCapabilities() {
    std::lock_guard lock(mutex_);
    return device_.state().capabilities;
}

bool Engine::showDeviceControlPanel() {
    // Holds the lock: nothing may close the driver while its panel is up.
    std::lock_guard lock(mutex_);
    return device_.showControlPanel();
}

double Engine::sampleRate() {
    std::lock_guard lock(mutex_);
    return sampleRate_;
}

std::string Engine::takeDeviceEvent() {
    std::lock_guard lock(mutex_);
    const auto take = [this](DeviceEvent event) {
        const auto flag = static_cast<uint32_t>(event);
        return (pendingDeviceEvents_.fetch_and(~flag) & flag) != 0;
    };
    if (take(DeviceEvent::Stopped)) {
        pendingDeviceEvents_.store(0);  // the rest were about the device that is gone
        return "stopped";
    }
    if (!deviceRunning_) {
        pendingDeviceEvents_.store(0);
        return "";
    }
    if (!device_.inControlPanel() && take(DeviceEvent::ResetRequest)) return "reset";
    if (take(DeviceEvent::Rerouted)) return "rerouted";
    if (take(DeviceEvent::LatencyChanged)) {
        device_.refreshLatencies();
        return "latency";
    }
    return "";
}

std::vector<float> Engine::takeInputMeters() {
    std::lock_guard lock(mutex_);
    const size_t count = std::min<size_t>(openInputs_, SharedState::kMaxInputMeters);
    std::vector<float> peaks(count);
    for (size_t c = 0; c < count; ++c) peaks[c] = shared_.inputPeaks[c].exchange(0.f);
    return peaks;
}

void Engine::deviceEvent(DeviceEvent event) noexcept {
    pendingDeviceEvents_.fetch_or(static_cast<uint32_t>(event));
}

void Engine::audioCallback(const AudioIO& io) noexcept {
    ScopedNoDenormals noDenormals;
    const auto started = std::chrono::steady_clock::now();
    const uint32_t metered = std::min<uint32_t>(io.numInputs, SharedState::kMaxInputMeters);
    for (uint32_t c = 0; c < metered; ++c) {
        float peak = 0.f;
        for (uint32_t i = 0; i < io.frames; ++i) peak = std::max(peak, std::abs(io.inputs[c][i]));
        atomicStoreMax(shared_.inputPeaks[c], peak);
    }
    // seq_cst pairs with the store/epoch-load sequence in rebuildSnapshotLocked().
    const RenderSnapshot* snap = snapshot_.load(std::memory_order_seq_cst);
    if (!snap || liveSuspended_.load(std::memory_order_seq_cst)) {
        for (uint32_t c = 0; c < io.numOutputs; ++c) std::fill_n(io.outputs[c], io.frames, 0.f);
    } else {
        renderer_.processLive(*snap, shared_, io.outputs, io.numOutputs, io.frames);
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const double budget = io.frames / snap->sampleRate;
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
    for (auto& insert : trackLocked(trackId).inserts) retireProcessorLocked(insert);
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

void Engine::setTrackNotes(uint32_t trackId, const std::vector<NoteDesc>& notes) {
    std::lock_guard lock(mutex_);
    trackLocked(trackId).notes = notes;
    rebuildSnapshotLocked();
}

void Engine::previewNote(uint32_t trackId, int key, int velocity) {
    std::lock_guard lock(mutex_);
    trackLocked(trackId);
    shared_.previewNotes.push({trackId, static_cast<uint8_t>(std::clamp(key, 0, 127)),
                               static_cast<uint8_t>(std::clamp(velocity, 0, 127))});
    serviceTransportIfIdleLocked();
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

std::shared_ptr<Processor> Engine::processor(uint32_t processorId) {
    std::lock_guard lock(mutex_);
    return processorLocked(processorId);
}

uint32_t Engine::insertProcessorLocked(uint32_t trackId, std::shared_ptr<Processor> processor, int index) {
    TrackModel& track = trackLocked(trackId);
    const auto count = static_cast<int>(track.inserts.size());
    const int at = (index < 0 || index > count) ? count : index;
    track.inserts.insert(track.inserts.begin() + at, processor);
    const uint32_t id = nextProcessorId_++;
    processors_[id] = {trackId, std::move(processor)};
    rebuildSnapshotLocked();
    return id;
}

void Engine::retireProcessorLocked(std::shared_ptr<Processor> processor) {
    processor->closeEditor();
    graveyard_.push_back(std::move(processor));  // destroyed in idle(), once no snapshot uses it
}

uint32_t Engine::addBuiltinProcessor(uint32_t trackId, const std::string& type, int index) {
    std::shared_ptr<Processor> processor;
    if (type == "utility") {
        processor = std::make_shared<UtilityProcessor>();
    } else if (type == "synth") {
        processor = std::make_shared<SynthProcessor>();
    } else {
        throw std::invalid_argument("Unknown built-in device: " + type);
    }
    std::lock_guard lock(mutex_);
    trackLocked(trackId);
    processor->prepare(sampleRate_, Renderer::kMaxBlock);  // before the audio thread can see it
    return insertProcessorLocked(trackId, std::move(processor), index);
}

uint32_t Engine::addPluginProcessor(uint32_t trackId, const std::string& format, const std::string& path,
                                    const std::string& uid, int index) {
    if (format != "VST3") throw std::invalid_argument("Unsupported plug-in format: " + format);
    double rate = 0.0;
    {
        std::lock_guard lock(mutex_);
        trackLocked(trackId);
        rate = sampleRate_;
    }
    // Loading can take a while (and show dialogs), so it doesn't hold the lock.
    auto plugin = vst3::Vst3Format::instance().instantiate(path, uid, rate, Renderer::kMaxBlock);
    std::lock_guard lock(mutex_);
    if (sampleRate_ != rate) plugin->prepare(sampleRate_, Renderer::kMaxBlock);  // the device changed meanwhile
    return insertProcessorLocked(trackId, std::move(plugin), index);
}

void Engine::removeProcessor(uint32_t processorId) {
    std::lock_guard lock(mutex_);
    auto processor = processorLocked(processorId);
    TrackModel& track = trackLocked(processors_[processorId].first);
    std::erase(track.inserts, processor);
    processors_.erase(processorId);
    retireProcessorLocked(std::move(processor));
    rebuildSnapshotLocked();
}

void Engine::setTrackProcessorOrder(uint32_t trackId, const std::vector<uint32_t>& processorIds) {
    std::lock_guard lock(mutex_);
    TrackModel& track = trackLocked(trackId);
    std::vector<std::shared_ptr<Processor>> order;
    for (const uint32_t id : processorIds) {
        auto it = processors_.find(id);
        if (it == processors_.end() || it->second.first != trackId) {
            throw std::invalid_argument("Device " + std::to_string(id) + " is not on track " + std::to_string(trackId));
        }
        if (std::find(order.begin(), order.end(), it->second.second) != order.end()) {
            throw std::invalid_argument("Device " + std::to_string(id) + " is listed twice");
        }
        order.push_back(it->second.second);
    }
    if (order.size() != track.inserts.size()) {
        throw std::invalid_argument("The order must list all of the track's devices");
    }
    track.inserts = std::move(order);
    rebuildSnapshotLocked();
}

ProcessorInfo Engine::processorInfo(uint32_t processorId) {
    auto p = processor(processorId);
    return {p->typeId(), p->name(), p->latencySamples(), p->tailSamples(), p->hasEditor()};
}

std::vector<ParamInfo> Engine::processorParams(uint32_t processorId) { return processor(processorId)->params(); }

int Engine::processorParamIndex(uint32_t processorId, const std::string& paramId) {
    auto p = processor(processorId);
    const auto& params = p->params();
    for (size_t i = 0; i < params.size(); ++i) {
        if (params[i].id == paramId) return static_cast<int>(i);
    }
    return -1;
}

float Engine::processorParam(uint32_t processorId, int index) { return processor(processorId)->getParam(index); }

// Plug-in calls from here on are made without holding the lock (see Engine.h).
void Engine::setProcessorParam(uint32_t processorId, int index, float value) {
    processor(processorId)->setParam(index, value);
}

std::string Engine::processorParamText(uint32_t processorId, int index, float value) {
    return processor(processorId)->paramText(index, value);
}

void Engine::setProcessorEnabled(uint32_t processorId, bool enabled) {
    std::lock_guard lock(mutex_);
    processorLocked(processorId)->setEnabled(enabled);
    rebuildSnapshotLocked();  // a switched-off device adds no latency
}

std::vector<uint8_t> Engine::processorState(uint32_t processorId) { return processor(processorId)->getState(); }

void Engine::setProcessorState(uint32_t processorId, const std::vector<uint8_t>& state) {
    processor(processorId)->setState(state);
}

bool Engine::openEditor(uint32_t processorId, uintptr_t ownerWindow, const std::string& title) {
    return processor(processorId)->openEditor(reinterpret_cast<void*>(ownerWindow), title);
}

void Engine::closeEditor(uint32_t processorId) { processor(processorId)->closeEditor(); }

bool Engine::isEditorOpen(uint32_t processorId) { return processor(processorId)->isEditorOpen(); }

bool Engine::setEditorVisible(uint32_t processorId, bool visible) {
    return processor(processorId)->setEditorVisible(visible);
}

void Engine::setEditorTitle(uint32_t processorId, const std::string& title) {
    processor(processorId)->setEditorTitle(title);
}

std::vector<ProcessorEventRecord> Engine::takeProcessorEvents() {
    std::vector<std::pair<uint32_t, std::shared_ptr<Processor>>> current;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [id, entry] : processors_) current.emplace_back(id, entry.second);
    }
    std::vector<ProcessorEventRecord> records;
    std::vector<ProcessorEvent> events;
    for (const auto& [id, p] : current) {
        events.clear();
        p->takeEvents(events);
        for (const ProcessorEvent& event : events) {
            ProcessorEventRecord record;
            static_cast<ProcessorEvent&>(record) = event;
            record.processorId = id;
            records.push_back(record);
        }
    }
    return records;
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
    Renderer::discardPreviewNotes(shared_);  // nobody would hear them, and a stale note-on could hang
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

void Engine::resetProcessorsLocked() {
    // Offline renders share the processors with live playback. Resetting them
    // before keeps live notes (and a plug-in's reverb tail) out of the render;
    // after, keeps the render's last notes from hanging in live playback.
    for (auto& track : tracks_) {
        for (auto& insert : track.inserts) {
            insert->resetOffline();
            insert->requestReset();
        }
    }
}

void Engine::prepareOfflineLocked(Renderer& offline, WarpVoiceSet& voices,
                                  std::vector<std::shared_ptr<DelayLine>>& delays, double startBeat) {
    // Fresh stretchers, as many as live playback has, so an offline render
    // starts from a clean state and leaves the live voices alone. Likewise
    // delay-compensation lines.
    for (int c = 0; c < kNumStretchConfigs; ++c) {
        for (size_t i = 0; i < snapshotHold_->warpVoices[c].size(); ++i) {
            voices[c].push_back(std::make_shared<WarpVoice>(static_cast<StretchConfig>(c), sampleRate_));
        }
    }
    for (const TrackRender& track : snapshotHold_->tracks) {
        delays.push_back(track.compensation > 0 ? std::make_shared<DelayLine>(track.compensation + 1) : nullptr);
    }
    offline.prepare(sampleRate_);
    offline.setWarpVoices(&voices);
    offline.setDelayLines(&delays);
    offline.syncTempo(*snapshotHold_);
    offline.setPosition(std::llround(std::max(0.0, startBeat) * snapshotHold_->samplesPerBeat()));
    offline.setPlaying(true);
}

void Engine::renderOfflineLocked(double startBeat, int64_t frames, float* out, bool loop, bool metronome) {
    ScopedNoDenormals noDenormals;
    suspendLiveLocked();
    resetProcessorsLocked();
    ScopeExit resume([this] {
        resetProcessorsLocked();
        resumeLiveLocked();
    });
    Renderer offline;
    WarpVoiceSet voices;
    std::vector<std::shared_ptr<DelayLine>> delays;
    prepareOfflineLocked(offline, voices, delays, startBeat);
    // With delay compensation the output lags the timeline: render the lag first and drop it.
    if (const int64_t lag = snapshotHold_->maxLatency; lag > 0) {
        std::vector<float> discarded(static_cast<size_t>(lag) * 2);
        offline.renderOffline(*snapshotHold_, shared_, discarded.data(), lag, loop, metronome);
    }
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

    ScopedNoDenormals noDenormals;
    suspendLiveLocked();
    resetProcessorsLocked();
    ScopeExit resume([this] {
        resetProcessorsLocked();
        resumeLiveLocked();
    });
    Renderer offline;
    WarpVoiceSet voices;
    std::vector<std::shared_ptr<DelayLine>> delays;
    prepareOfflineLocked(offline, voices, delays, startBeat);

    constexpr int64_t kChunk = 16384;
    std::vector<float> rendered(kChunk * 2);
    for (int64_t lag = snapshotHold_->maxLatency; lag > 0;) {  // see renderOfflineLocked()
        const int64_t n = std::min(kChunk, lag);
        offline.renderOffline(*snapshotHold_, shared_, rendered.data(), n);
        lag -= n;
    }
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

    // Plug-in delay compensation: each track is delayed to line up with the one
    // whose enabled devices add the most latency.
    constexpr int kMaxLatency = 1 << 20;
    std::vector<int> latencies;
    latencies.reserve(tracks_.size());
    for (const TrackModel& track : tracks_) {
        int latency = 0;
        for (const auto& insert : track.inserts) {
            if (insert->isEnabled()) latency = std::min(kMaxLatency, latency + std::max(0, insert->latencySamples()));
        }
        latencies.push_back(latency);
        snap->maxLatency = std::max(snap->maxLatency, latency);
    }

    const auto rate = static_cast<uint32_t>(sampleRate_);
    std::array<size_t, kNumStretchConfigs> voicesNeeded{};
    snap->tracks.reserve(tracks_.size());
    for (size_t t = 0; t < tracks_.size(); ++t) {
        TrackModel& track = tracks_[t];
        TrackRender render;
        render.id = track.id;
        render.params = track.params;
        render.inserts = track.inserts;
        render.latency = latencies[t];
        render.compensation = snap->maxLatency - latencies[t];
        if (render.compensation > 0 && (!track.delay || track.delay->capacity() <= render.compensation)) {
            track.delay = std::make_shared<DelayLine>(2 * render.compensation + Renderer::kMaxBlock);
        }
        render.delay = track.delay;
        render.notes.reserve(track.notes.size());
        for (const NoteDesc& note : track.notes) {
            NoteRender nr;
            nr.start = std::max<int64_t>(0, std::llround(note.startBeat * spb));
            nr.end = std::max<int64_t>(nr.start + 1, std::llround((note.startBeat + note.lengthBeats) * spb));
            nr.key = static_cast<uint8_t>(std::clamp(note.key, 0, 127));
            nr.velocity = static_cast<uint8_t>(std::clamp(note.velocity, 1, 127));
            render.notes.push_back(nr);
        }
        std::sort(render.notes.begin(), render.notes.end(), [](const NoteRender& a, const NoteRender& b) {
            return a.start != b.start ? a.start < b.start : a.key < b.key;
        });
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
                cr.preserveFormants = clip.warpMode == WarpMode::Formants;
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
    std::vector<std::shared_ptr<Processor>> live;
    std::vector<std::shared_ptr<Processor>> dead;
    {
        std::lock_guard lock(mutex_);
        if (deviceRunning_ && (pendingDeviceEvents_.load() & static_cast<uint32_t>(DeviceEvent::Stopped))) {
            closeDeviceLocked();  // the backend lost the device; the UI reports it via takeDeviceEvent()
        }
        collectGarbageLocked();
        serviceTransportIfIdleLocked();
        for (const auto& [id, entry] : processors_) live.push_back(entry.second);
        // Removed processors that no snapshot holds any more.
        for (auto it = graveyard_.begin(); it != graveyard_.end();) {
            if (it->use_count() == 1) {
                dead.push_back(std::move(*it));
                it = graveyard_.erase(it);
            } else {
                ++it;
            }
        }
    }
    dead.clear();  // plug-ins may take their time to go: not under the lock

    bool realign = false;
    for (const auto& p : live) realign |= p->idle();
    if (realign) {
        std::lock_guard lock(mutex_);
        rebuildSnapshotLocked();  // a latency changed: new delay compensation
    }
}

}  // namespace gil
