#include "Engine.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>

#include "PathUtils.h"
#include "Routing.h"
#include "miniaudio.h"
#include "plugins/Vst3Format.h"
#include "processors/Ott.h"
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

Engine::Engine()
    : midiDevices_([this](uint16_t port, const uint8_t* message, int size, int64_t hostTime) {
          midiInput(port, message, size, hostTime);
      }) {
    std::lock_guard lock(mutex_);
    master_.chainId = addChainLocked(kMaster, 0);
    scheduler_ = std::make_unique<Scheduler>(defaultAudioThreads());
    renderer_.setScheduler(scheduler_.get());
    renderer_.prepare(sampleRate_);
    rebuildSnapshotLocked();
}

Engine::~Engine() {
    std::lock_guard lock(mutex_);
    midiDevices_.closeAll();
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
        openInputChannels_ = state.inputChannels;
        for (auto& peak : shared_.inputPeaks) peak.store(0.f, std::memory_order_relaxed);
        // The audio thread is not running, so the preview can be dropped directly.
        shared_.previewSource.store(nullptr, std::memory_order_seq_cst);
        shared_.previewSerial.fetch_add(1, std::memory_order_seq_cst);
        shared_.previewActive.store(false);
        previewHold_.reset();

        renderer_.prepare(sampleRate_);
        for (auto& [id, entry] : processors_) entry.processor->prepare(sampleRate_, Renderer::kMaxBlock);
        // MIDI input plays a device buffer after it arrives (MidiInput.h).
        shared_.midiInputDelay.store(state.bufferFrames > 0 ? static_cast<int>(state.bufferFrames) : 512);
        shared_.midiSampleRate.store(sampleRate_);
        rebuildSnapshotLocked();
        serviceTransportIfIdleLocked();
        pendingDeviceEvents_.store(0);  // about the device just closed
        device_.start();
        deviceRunning_ = true;
    } catch (...) {
        device_.close();
        deviceRunning_ = false;
        openInputs_ = 0;
        openInputChannels_.clear();
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
    shared_.clock.stop();  // MIDI input is dropped from here on
    deviceRunning_ = false;
    openInputs_ = 0;
    openInputChannels_.clear();
    finishRecordingLocked();  // a device change (or a new sample rate) ends a recording
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

std::vector<float> Engine::masterScope(size_t frames) const {
    frames = std::min(frames, SharedState::kScopeSize / 2);
    const uint64_t written = shared_.scopeWritten.load(std::memory_order_acquire);
    std::vector<float> samples(frames, 0.f);
    const size_t available = static_cast<size_t>(std::min<uint64_t>(written, frames));
    const uint64_t first = written - available;
    for (size_t i = 0; i < available; ++i) {
        samples[frames - available + i] =
            shared_.scope[(first + i) & (SharedState::kScopeSize - 1)].load(std::memory_order_relaxed);
    }
    return samples;
}

void Engine::deviceEvent(DeviceEvent event) noexcept {
    pendingDeviceEvents_.fetch_or(static_cast<uint32_t>(event));
}

void Engine::audioCallback(const AudioIO& io) noexcept {
    ScopedNoDenormals noDenormals;
    const auto started = std::chrono::steady_clock::now();
    shared_.clock.update(io.hostTimeNs, io.sampleTime);
    const uint32_t metered = std::min<uint32_t>(io.numInputs, SharedState::kMaxInputMeters);
    for (uint32_t c = 0; c < metered; ++c) {
        float peak = 0.f;
        for (uint32_t i = 0; i < io.frames; ++i) peak = std::max(peak, std::abs(io.inputs[c][i]));
        atomicStoreMax(shared_.inputPeaks[c], peak);
    }
    // seq_cst pairs with the store/epoch-load sequence in rebuildSnapshotLocked().
    const RenderSnapshot* snap = snapshot_.load(std::memory_order_seq_cst);
    RecordingSession* recording = liveRecording_.load(std::memory_order_seq_cst);
    if (!snap || liveSuspended_.load(std::memory_order_seq_cst)) {
        for (uint32_t c = 0; c < io.numOutputs; ++c) std::fill_n(io.outputs[c], io.frames, 0.f);
    } else {
        renderer_.processLive(*snap, shared_, io, recording);
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
    if (trackId == kMaster) return master_;
    return arrangementTrackLocked(trackId);
}

Engine::TrackModel& Engine::arrangementTrackLocked(uint32_t trackId) {
    if (trackId == kMaster) throw std::invalid_argument("The master has no clips or notes, and stays");
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
    track.chainId = addChainLocked(track.id, 0);
    track.buffers = std::make_shared<TrackBuffers>(Renderer::kMaxBlock);
    track.outputState = std::make_shared<EdgeState>(Renderer::kMaxBlock);
    tracks_.push_back(std::move(track));
    rebuildSnapshotLocked();
    return tracks_.back().id;
}

void Engine::removeTrack(uint32_t trackId) {
    std::lock_guard lock(mutex_);
    arrangementTrackLocked(trackId);
    // What went into it goes to the master; the sends into it go, and so do the
    // inputs and sidechains from it.
    for (TrackModel& track : tracks_) {
        if (track.output == trackId) track.output = kMaster;
        std::erase_if(track.sends, [trackId](const SendModel& send) { return send.to == trackId; });
        if (track.inputTrack == trackId) track.inputTrack.reset();
    }
    for (auto& [id, entry] : processors_) {
        if (entry.sidechain && entry.sidechain->source == trackId) entry.sidechain.reset();
    }
    // Its chains go, with their devices.
    for (auto it = processors_.begin(); it != processors_.end();) {
        if (chainLocked(it->second.chainId).stripId == trackId) {
            retireProcessorLocked(std::move(it->second.processor));
            it = processors_.erase(it);
        } else {
            ++it;
        }
    }
    std::erase_if(chains_, [&](const auto& entry) { return entry.second.stripId == trackId; });
    std::erase_if(tracks_, [&](const TrackModel& track) { return track.id == trackId; });
    rebuildSnapshotLocked();
}

void Engine::setTrackClips(uint32_t trackId, const std::vector<ClipDesc>& clips) {
    std::lock_guard lock(mutex_);
    TrackModel& track = arrangementTrackLocked(trackId);
    track.clips = clips;
    track.clipKeys.clear();
    for (const auto& clip : clips) track.clipKeys.push_back(sourceKey(clip.path));
    rebuildSnapshotLocked();
}

void Engine::setTrackNotes(uint32_t trackId, const std::vector<NoteDesc>& notes) {
    std::lock_guard lock(mutex_);
    arrangementTrackLocked(trackId).notes = notes;
    rebuildSnapshotLocked();
}

void Engine::previewNote(uint32_t trackId, int key, int velocity) {
    std::lock_guard lock(mutex_);
    arrangementTrackLocked(trackId);
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

int Engine::trackIndexLocked(uint32_t trackId) const {
    for (size_t i = 0; i < tracks_.size(); ++i) {
        if (tracks_[i].id == trackId) return static_cast<int>(i);
    }
    return -1;
}

std::vector<RouteEdge> Engine::routeEdgesLocked(std::vector<EdgeOrigin>* origins) const {
    std::vector<RouteEdge> edges;
    if (origins) origins->clear();
    for (size_t t = 0; t < tracks_.size(); ++t) {
        const TrackModel& track = tracks_[t];
        const int from = static_cast<int>(t);
        edges.push_back({from, track.output == kMaster ? -1 : trackIndexLocked(track.output)});
        if (origins) origins->push_back({from, kOutputEdge});
        for (size_t s = 0; s < track.sends.size(); ++s) {
            edges.push_back({from, trackIndexLocked(track.sends[s].to)});
            if (origins) origins->push_back({from, static_cast<int>(s)});
        }
    }
    // Last, so that each track's own edges lead its outgoing ones. (The master
    // as a source is no edge: it renders after every track.)
    for (size_t t = 0; t < tracks_.size(); ++t) {
        const TrackModel& track = tracks_[t];
        if (!track.inputTrack || *track.inputTrack == kMaster) continue;
        edges.push_back({trackIndexLocked(*track.inputTrack), static_cast<int>(t), false});
        if (origins) origins->push_back({static_cast<int>(t), kInputEdge});
    }
    // Then the sidechains, by destination (the tracks, then the master's devices) and device.
    std::unordered_map<const Processor*, uint32_t> sidechained;
    for (const auto& [id, entry] : processors_) {
        if (entry.sidechain) sidechained.emplace(entry.processor.get(), id);
    }
    if (sidechained.empty()) return edges;
    const auto addSidechains = [&](const TrackModel& strip, int to) {
        const auto& inserts = insertsLocked(strip);
        for (size_t d = 0; d < inserts.size(); ++d) {
            const auto found = sidechained.find(inserts[d].get());
            if (found == sidechained.end()) continue;
            const SidechainModel& sidechain = *processors_.at(found->second).sidechain;
            RouteEdge edge{trackIndexLocked(sidechain.source), to, false};
            if (edge.from < 0) continue;  // (its source went: removeTrack() takes it away)
            EdgeRender::Tap tap;
            edge.tap = sidechainTapLocked(sidechain, tap);
            edge.device = inserts[d]->isEnabled() ? static_cast<int>(d) : -1;  // one switched off isn't lined up
            edges.push_back(edge);
            if (origins) origins->push_back({to, kSidechainEdge, found->second, static_cast<int>(d)});
        }
    };
    for (size_t t = 0; t < tracks_.size(); ++t) addSidechains(tracks_[t], static_cast<int>(t));
    addSidechains(master_, -1);
    return edges;
}

int Engine::sidechainTapLocked(const SidechainModel& sidechain, EdgeRender::Tap& tap) const {
    tap = sidechain.tap == SidechainTap::PostFader ? EdgeRender::Tap::PostFader : EdgeRender::Tap::PreFader;
    if (sidechain.tap != SidechainTap::AfterDevice) return -1;
    const int source = trackIndexLocked(sidechain.source);
    const auto entry = processors_.find(sidechain.tapProcessor);
    if (source < 0 || entry == processors_.end()) return -1;  // before the fader
    const auto& inserts = insertsLocked(tracks_[static_cast<size_t>(source)]);
    const auto place = std::find(inserts.begin(), inserts.end(), entry->second.processor);
    if (place == inserts.end()) return -1;  // the device left the source: before the fader
    tap = EdgeRender::Tap::AfterDevice;
    return static_cast<int>(place - inserts.begin()) + 1;
}

void Engine::checkSidechainLocked(uint32_t source, uint32_t strip) const {
    if (strip == kMaster) return;  // everything goes into the master: nothing it feeds feeds a track
    if (wouldCycle(static_cast<int>(tracks_.size()), routeEdgesLocked(), trackIndexLocked(source),
                   trackIndexLocked(strip))) {
        throw std::invalid_argument("A device on track " + std::to_string(strip) +
                                    " can't take its sidechain from track " + std::to_string(source) + ": " +
                                    (source == strip ? "that is its own track" : "its track feeds that one"));
    }
}

void Engine::checkRouteLocked(uint32_t from, uint32_t to, const char* what) const {
    if (wouldCycle(static_cast<int>(tracks_.size()), routeEdgesLocked(), trackIndexLocked(from), trackIndexLocked(to))) {
        throw std::invalid_argument("Track " + std::to_string(from) + " can't " + what + " track " +
                                    std::to_string(to) + ": that track feeds it");
    }
}

void Engine::setTrackOutput(uint32_t trackId, uint32_t outputTrackId) {
    std::lock_guard lock(mutex_);
    TrackModel& track = arrangementTrackLocked(trackId);
    if (outputTrackId != kMaster) {
        arrangementTrackLocked(outputTrackId);
        checkRouteLocked(trackId, outputTrackId, "go into");
    }
    if (track.output == outputTrackId) return;
    track.output = outputTrackId;
    rebuildSnapshotLocked();
}

uint32_t Engine::trackOutput(uint32_t trackId) {
    std::lock_guard lock(mutex_);
    return arrangementTrackLocked(trackId).output;
}

void Engine::setTrackSend(uint32_t trackId, uint32_t toTrackId, float gain, bool preFader) {
    std::lock_guard lock(mutex_);
    TrackModel& track = arrangementTrackLocked(trackId);
    arrangementTrackLocked(toTrackId);  // not the master: everything reaches it anyway
    gain = std::max(0.f, gain);
    const auto it = std::find_if(track.sends.begin(), track.sends.end(),
                                 [toTrackId](const SendModel& send) { return send.to == toTrackId; });
    if (it != track.sends.end()) {
        it->state->gain.store(gain);
        if (it->preFader != preFader) {
            it->preFader = preFader;
            rebuildSnapshotLocked();
        }
        return;
    }
    checkRouteLocked(trackId, toTrackId, "send to");
    SendModel send;
    send.to = toTrackId;
    send.preFader = preFader;
    send.state = std::make_shared<EdgeState>(Renderer::kMaxBlock);
    send.state->gain.store(gain);
    track.sends.push_back(std::move(send));
    rebuildSnapshotLocked();
}

void Engine::removeTrackSend(uint32_t trackId, uint32_t toTrackId) {
    std::lock_guard lock(mutex_);
    TrackModel& track = arrangementTrackLocked(trackId);
    if (std::erase_if(track.sends, [toTrackId](const SendModel& send) { return send.to == toTrackId; }) > 0) {
        rebuildSnapshotLocked();
    }
}

std::vector<SendInfo> Engine::trackSends(uint32_t trackId) {
    std::lock_guard lock(mutex_);
    std::vector<SendInfo> sends;
    for (const SendModel& send : arrangementTrackLocked(trackId).sends) {
        sends.push_back({send.to, send.state->gain.load(), send.preFader});
    }
    return sends;
}

std::vector<MeterReading> Engine::takeMeters() {
    std::lock_guard lock(mutex_);
    std::vector<MeterReading> meters;
    meters.reserve(tracks_.size() + 1);
    meters.push_back({kMaster, master_.params->peakLeft.exchange(0.f), master_.params->peakRight.exchange(0.f)});
    for (const auto& track : tracks_) {
        meters.push_back({track.id, track.params->peakLeft.exchange(0.f), track.params->peakRight.exchange(0.f)});
    }
    return meters;
}

// ---------------------------------------------------------------------------
// Input and recording

void Engine::setTrackInput(uint32_t trackId, const std::vector<int>& channels) {
    if (channels.size() > 2 || std::any_of(channels.begin(), channels.end(), [](int c) { return c < 0; })) {
        throw std::invalid_argument("An input is no channel, one, or a pair");
    }
    std::lock_guard lock(mutex_);
    TrackModel& track = arrangementTrackLocked(trackId);
    track.inputChannels = channels;
    track.inputTrack.reset();
    rebuildSnapshotLocked();
}

void Engine::setTrackInputTrack(uint32_t trackId, uint32_t sourceTrackId) {
    std::lock_guard lock(mutex_);
    TrackModel& track = arrangementTrackLocked(trackId);
    if (sourceTrackId != kMaster) {
        arrangementTrackLocked(sourceTrackId);
        if (wouldCycle(static_cast<int>(tracks_.size()), routeEdgesLocked(), trackIndexLocked(sourceTrackId),
                       trackIndexLocked(trackId))) {
            throw std::invalid_argument("Track " + std::to_string(trackId) + " can't take its input from track " +
                                        std::to_string(sourceTrackId) + ": it feeds that track");
        }
    }
    if (track.inputTrack == sourceTrackId) return;
    track.inputTrack = sourceTrackId;
    if (!track.inputState) track.inputState = std::make_shared<EdgeState>(0);  // (it never needs a signal of its own)
    rebuildSnapshotLocked();
}

void Engine::setTrackMonitor(uint32_t trackId, MonitorMode mode) {
    std::lock_guard lock(mutex_);
    arrangementTrackLocked(trackId).monitor = mode;
    rebuildSnapshotLocked();
}

void Engine::setTrackArmed(uint32_t trackId, bool armed) {
    std::lock_guard lock(mutex_);
    arrangementTrackLocked(trackId).armed = armed;
    rebuildSnapshotLocked();
}

std::optional<uint32_t> Engine::trackInputTrack(uint32_t trackId) {
    std::lock_guard lock(mutex_);
    return arrangementTrackLocked(trackId).inputTrack;
}

InputEdge Engine::inputEdgeLocked(const TrackModel& track) const {
    InputEdge edge;
    if (track.inputTrack) {  // (the snapshot fills in a track's edge)
        edge.source = *track.inputTrack == kMaster ? InputEdge::Source::Master : InputEdge::Source::Track;
        return edge;
    }
    if (track.inputChannels.empty()) return edge;
    const auto index = [this](int channel) {
        const auto it = std::find(openInputChannels_.begin(), openInputChannels_.end(), channel);
        return it == openInputChannels_.end() ? -1 : static_cast<int>(it - openInputChannels_.begin());
    };
    edge.source = InputEdge::Source::Device;
    edge.left = index(track.inputChannels[0]);
    edge.right = track.inputChannels.size() > 1 ? index(track.inputChannels[1]) : edge.left;
    return edge;
}

void Engine::startRecording(const std::vector<RecordTarget>& targets, double countInBeats) {
    std::lock_guard lock(mutex_);
    if (!deviceRunning_) throw std::runtime_error("No audio device is running");
    if (recording_) throw std::runtime_error("Already recording");
    if (targets.empty()) throw std::invalid_argument("Nothing to record");
    const auto ringFrames = static_cast<size_t>(sampleRate_ * 8.0);  // the writer may fall this far behind
    // A sample taken in a block came back through the input after leaving the
    // output, where the timeline was heard this much earlier than the renderer was.
    // A MIDI message was played in response to what was heard when it arrived:
    // the renderer meets it one MIDI delay later, a block ahead of the output.
    // A track's output leaves it as late as its devices and what feeds it make
    // it (its edges' arrival); the master's, as late as the output's lag.
    const DeviceState state = device_.state();
    const auto lag = static_cast<int64_t>(snapshotHold_->outputLatency());
    const int64_t devicePlacement = lag + state.inputLatency + state.outputLatency;
    const int64_t midiPlacement = lag + state.outputLatency + shared_.midiInputDelay.load();
    const auto arrival = [this](uint32_t trackId) -> int64_t {
        for (const TrackRender& render : snapshotHold_->tracks) {
            if (render.id == trackId) return render.inputLatency + render.latency;
        }
        return 0;
    };
    std::vector<std::unique_ptr<RecordingTake>> takes;
    std::vector<std::unique_ptr<MidiRecordingTake>> midiTakes;
    for (const RecordTarget& target : targets) {
        const TrackModel& track = arrangementTrackLocked(target.trackId);
        const auto listed = [&](const auto& list) {
            return std::any_of(list.begin(), list.end(), [&](const auto& t) { return t->trackId == target.trackId; });
        };
        if (listed(takes) || listed(midiTakes)) throw std::invalid_argument("A track is listed twice");
        if (target.path.empty()) {
            if (!track.midiInput.enabled) {
                throw std::invalid_argument("Track " + std::to_string(target.trackId) + " has no MIDI input");
            }
            midiTakes.push_back(std::make_unique<MidiRecordingTake>(target.trackId));
            continue;
        }
        const InputEdge edge = inputEdgeLocked(track);
        switch (edge.source) {
            case InputEdge::Source::None:
                throw std::invalid_argument("Track " + std::to_string(target.trackId) + " has no input");
            case InputEdge::Source::Device:
                if (edge.left < 0 || edge.right < 0) {
                    throw std::runtime_error("A track's input is not open on the audio device");
                }
                takes.push_back(std::make_unique<RecordingTake>(target.trackId, target.path, edge.left, edge.right,
                                                                ringFrames, devicePlacement));
                break;
            case InputEdge::Source::Track:
                takes.push_back(std::make_unique<RecordingTake>(target.trackId, target.path,
                                                                RecordingTake::Source::Track, *track.inputTrack,
                                                                ringFrames, arrival(*track.inputTrack)));
                break;
            case InputEdge::Source::Master:
                takes.push_back(std::make_unique<RecordingTake>(target.trackId, target.path,
                                                                RecordingTake::Source::Master, kMaster, ringFrames, lag));
                break;
        }
    }
    recording_ = std::make_unique<RecordingSession>(std::move(takes), std::move(midiTakes), sampleRate_, midiPlacement);
    liveRecording_.store(recording_.get(), std::memory_order_seq_cst);
    if (!requestedPlaying_.load()) {
        requestedPlaying_.store(true);
        pushCommandLocked({TransportCommand::Type::Play, 0.0, std::max(0.0, countInBeats)});
    }
}

void Engine::finishRecordingLocked() {
    if (!recording_) return;
    liveRecording_.store(nullptr, std::memory_order_seq_cst);
    waitForCallbackLocked();  // the audio thread no longer sees it
    auto takes = recording_->finish();
    recording_.reset();
    for (auto& take : takes) finishedTakes_.push_back(std::move(take));
}

std::vector<RecordedTake> Engine::stopRecording() {
    std::lock_guard lock(mutex_);
    finishRecordingLocked();
    return std::exchange(finishedTakes_, {});
}

bool Engine::isRecording() {
    std::lock_guard lock(mutex_);
    return recording_ && !recording_->interrupted();
}

std::vector<RecordingProgress> Engine::recordingProgress() {
    std::lock_guard lock(mutex_);
    std::vector<RecordingProgress> progress;
    if (!recording_) return progress;
    for (const auto& take : recording_->midiTakes()) {
        RecordingProgress p;
        p.trackId = take->trackId;
        p.midi = true;
        const int64_t start = take->start.load(std::memory_order_acquire);
        p.started = start != RecordingTake::kNotStarted;
        p.startSample = p.started ? start : 0;
        p.frames = take->frames.load(std::memory_order_acquire);
        p.notes = recording_->midiNotes(*take);
        progress.push_back(std::move(p));
    }
    for (const auto& take : recording_->takes()) {
        RecordingProgress p;
        p.trackId = take->trackId;
        const int64_t start = take->start.load(std::memory_order_acquire);
        p.started = start != RecordingTake::kNotStarted;
        p.startSample = p.started ? start - take->placement : 0;
        p.frames = take->frames.load(std::memory_order_acquire);
        RecordingTake::Peak peak;
        while (take->peaks.pop(peak)) {
            p.peaks.push_back(peak.min);
            p.peaks.push_back(peak.max);
        }
        progress.push_back(std::move(p));
    }
    return progress;
}

// ---------------------------------------------------------------------------
// MIDI input

std::vector<std::string> Engine::midiInputDevices() { return midiDevices_.available(); }

void Engine::openMidiInput(const std::string& name) {
    uint16_t port = 0;
    {
        std::lock_guard lock(mutex_);
        port = midiPortLocked(name);
    }
    midiDevices_.open(name, port);  // main thread only, like the other device calls
}

void Engine::closeMidiInput(const std::string& name) { midiDevices_.close(name); }

std::vector<std::string> Engine::openMidiInputs() { return midiDevices_.openNames(); }

uint16_t Engine::midiPortLocked(const std::string& name) {
    const auto it = std::find(midiPorts_.begin(), midiPorts_.end(), name);
    if (it != midiPorts_.end()) return static_cast<uint16_t>(it - midiPorts_.begin());
    if (midiPorts_.size() >= 0xFFFF) throw std::runtime_error("Too many MIDI inputs");
    midiPorts_.push_back(name);
    return static_cast<uint16_t>(midiPorts_.size() - 1);
}

void Engine::setTrackMidiInput(uint32_t trackId, bool enabled, const std::string& device, int channel) {
    if (channel < 0 || channel > 16) throw std::invalid_argument("A MIDI channel is 1-16, or 0 for all");
    std::lock_guard lock(mutex_);
    TrackModel& track = arrangementTrackLocked(trackId);
    MidiInputRoute route;
    route.enabled = enabled;
    route.port = device.empty() ? MidiInputRoute::kAllPorts : midiPortLocked(device);
    route.channel = channel == 0 ? MidiInputRoute::kAllChannels : channel - 1;
    track.midiInput = enabled ? route : MidiInputRoute{};
    rebuildSnapshotLocked();
}

void Engine::sendMidiInput(const std::string& device, const std::vector<uint8_t>& message, int64_t hostTime) {
    if (message.empty() || message.size() > 3 || !(message[0] & 0x80) ||
        std::any_of(message.begin() + 1, message.end(), [](uint8_t b) { return (b & 0x80) != 0; })) {
        throw std::invalid_argument("A MIDI message is a status byte and up to two data bytes");
    }
    uint16_t port = 0;
    {
        std::lock_guard lock(mutex_);
        port = midiPortLocked(device);
    }
    midiInput(port, message.data(), static_cast<int>(message.size()), hostTime != 0 ? hostTime : hostTimeNs());
}

void Engine::midiInput(uint16_t port, const uint8_t* message, int size, int64_t hostTime) noexcept {
    // Where on the device's clock this arrived, from the last callback's start;
    // it plays one MIDI delay (a buffer) later, in the next block.
    const AudioClock::Reading clock = shared_.clock.read();
    if (!clock.running || size < 1) return;
    const double elapsed = static_cast<double>(hostTime - clock.hostTimeNs) * 1e-9;
    MidiInputEvent event;
    event.time = clock.sampleTime + std::llround(elapsed * shared_.midiSampleRate.load(std::memory_order_relaxed)) +
                 shared_.midiInputDelay.load(std::memory_order_relaxed);
    event.port = port;
    event.status = message[0];
    event.data1 = size > 1 ? message[1] : 0;
    event.data2 = size > 2 ? message[2] : 0;
    std::lock_guard lock(shared_.midiInputMutex);  // the producers' side only: the audio thread never waits
    shared_.midiInput.push(event);                  // full: dropped
}

void Engine::discardMidiInputLocked() {
    // Only while no callback runs (it is the queue's consumer otherwise).
    MidiInputEvent event;
    while (shared_.midiInput.pop(event)) {
    }
}

AudioClockStatus Engine::audioClock() const {
    const AudioClock::Reading clock = shared_.clock.read();
    return {clock.running, clock.hostTimeNs, clock.sampleTime, shared_.midiInputDelay.load()};
}

// ---------------------------------------------------------------------------
// Automation

void Engine::setTrackAutomation(uint32_t trackId, const std::vector<AutomationLaneDesc>& lanes) {
    std::lock_guard lock(mutex_);
    trackLocked(trackId).automation = lanes;
    rebuildSnapshotLocked();
}

int Engine::insertLatency(const Processor& insert) {
    constexpr int kMaxLatency = 1 << 20;
    return insert.isEnabled() ? std::clamp(insert.latencySamples(), 0, kMaxLatency) : 0;
}

std::vector<AutomationNode> Engine::automationNodes(const AutomationLaneDesc& desc, double samplesPerBeat) {
    std::vector<AutomationNode> nodes;
    nodes.reserve(desc.points.size());
    for (const AutomationPoint& point : desc.points) {
        nodes.push_back({std::llround(std::max(0.0, point.beat) * samplesPerBeat), std::clamp(point.value, 0.f, 1.f),
                         std::clamp(point.curve, -1.f, 1.f)});
    }
    std::stable_sort(nodes.begin(), nodes.end(),
                     [](const AutomationNode& a, const AutomationNode& b) { return a.time < b.time; });
    return nodes;
}

void Engine::buildAutomationLocked(const TrackModel& track, int inputLatency, const std::vector<int>& deviceLatency,
                                   int faderLatency, double samplesPerBeat, StripRender& strip) {
    const auto& inserts = insertsLocked(track);
    for (const AutomationLaneDesc& desc : track.automation) {
        if (desc.points.empty()) continue;
        AutomationRender lane;
        AutomationRender* target = nullptr;
        if (desc.processorId == 0) {
            if (desc.param == "volume") {
                target = &strip.volume;
            } else if (desc.param == "pan") {
                target = &strip.pan;
            } else {
                continue;
            }
            lane.latency = faderLatency;
        } else {
            const auto found = processors_.find(desc.processorId);
            if (found == processors_.end() || found->second.chainId != track.chainId) continue;
            const std::shared_ptr<Processor>& processor = found->second.processor;
            const auto place = std::find(inserts.begin(), inserts.end(), processor);
            if (place == inserts.end()) continue;
            const auto& infos = processor->params();
            const auto info = std::find_if(infos.begin(), infos.end(),
                                           [&](const ParamInfo& p) { return p.id == desc.param; });
            if (info == infos.end() || info->readOnly) continue;
            lane.processor = processor;
            lane.param = static_cast<int>(info - infos.begin());
            lane.steps = info->stepCount();
            lane.insert = static_cast<int>(place - inserts.begin());
            const auto at = static_cast<size_t>(lane.insert);
            lane.latency = inputLatency + (at < deviceLatency.size() ? deviceLatency[at] : 0);
        }
        lane.nodes = automationNodes(desc, samplesPerBeat);
        if (target) {
            *target = std::move(lane);
        } else {
            strip.automation.push_back(std::move(lane));
        }
    }
    std::stable_sort(strip.automation.begin(), strip.automation.end(),
                     [](const AutomationRender& a, const AutomationRender& b) { return a.insert < b.insert; });
}

// ---------------------------------------------------------------------------
// Device chains

Engine::ChainModel& Engine::chainLocked(uint32_t chainId) {
    auto it = chains_.find(chainId);
    if (it == chains_.end()) throw std::invalid_argument("Unknown chain id " + std::to_string(chainId));
    return it->second;
}

const std::vector<std::shared_ptr<Processor>>& Engine::insertsLocked(const TrackModel& track) const {
    return chains_.at(track.chainId).inserts;
}

uint32_t Engine::addChainLocked(uint32_t stripId, uint32_t parentRack) {
    const uint32_t id = nextChainId_++;
    chains_[id] = ChainModel{id, stripId, parentRack, {}};
    return id;
}

uint32_t Engine::trackChain(uint32_t trackId) {
    std::lock_guard lock(mutex_);
    return trackLocked(trackId).chainId;
}

uint32_t Engine::processorChain(uint32_t processorId) {
    std::lock_guard lock(mutex_);
    processorLocked(processorId);
    return processors_[processorId].chainId;
}

std::shared_ptr<Processor> Engine::processorLocked(uint32_t processorId) {
    auto it = processors_.find(processorId);
    if (it == processors_.end()) throw std::invalid_argument("Unknown device id " + std::to_string(processorId));
    return it->second.processor;
}

std::shared_ptr<Processor> Engine::processor(uint32_t processorId) {
    std::lock_guard lock(mutex_);
    return processorLocked(processorId);
}

namespace {

// Where `index` puts a device in a chain of `count` (-1 or past the end: last).
int insertPosition(int index, size_t count) {
    const auto size = static_cast<int>(count);
    return (index < 0 || index > size) ? size : index;
}

}  // namespace

uint32_t Engine::insertProcessorLocked(uint32_t chainId, std::shared_ptr<Processor> processor, int index) {
    ChainModel& chain = chainLocked(chainId);
    chain.inserts.insert(chain.inserts.begin() + insertPosition(index, chain.inserts.size()), processor);
    const uint32_t id = nextProcessorId_++;
    processors_[id] = {chainId, std::move(processor)};
    rebuildSnapshotLocked();
    return id;
}

void Engine::retireProcessorLocked(std::shared_ptr<Processor> processor) {
    processor->closeEditor();
    graveyard_.push_back(std::move(processor));  // destroyed in idle(), once no snapshot uses it
}

uint32_t Engine::addBuiltinProcessor(uint32_t chainId, const std::string& type, int index) {
    std::shared_ptr<Processor> processor;
    if (type == "utility") {
        processor = std::make_shared<UtilityProcessor>();
    } else if (type == "synth") {
        processor = std::make_shared<SynthProcessor>();
    } else if (type == "ott") {
        processor = std::make_shared<OttProcessor>();
    } else {
        throw std::invalid_argument("Unknown built-in device: " + type);
    }
    std::lock_guard lock(mutex_);
    chainLocked(chainId);
    processor->prepare(sampleRate_, Renderer::kMaxBlock);  // before the audio thread can see it
    return insertProcessorLocked(chainId, std::move(processor), index);
}

uint32_t Engine::addPluginProcessor(uint32_t chainId, const std::string& format, const std::string& path,
                                    const std::string& uid, int index) {
    if (format != "VST3") throw std::invalid_argument("Unsupported plug-in format: " + format);
    double rate = 0.0;
    {
        std::lock_guard lock(mutex_);
        chainLocked(chainId);
        rate = sampleRate_;
    }
    // Loading can take a while (and show dialogs), so it doesn't hold the lock.
    auto plugin = vst3::Vst3Format::instance().instantiate(path, uid, rate, Renderer::kMaxBlock);
    std::lock_guard lock(mutex_);
    if (sampleRate_ != rate) plugin->prepare(sampleRate_, Renderer::kMaxBlock);  // the device changed meanwhile
    if (!chains_.contains(chainId)) {  // its track went while the plug-in loaded (a dialog's message loop)
        retireProcessorLocked(std::move(plugin));
        throw std::invalid_argument("Unknown chain id " + std::to_string(chainId));
    }
    return insertProcessorLocked(chainId, std::move(plugin), index);
}

void Engine::removeProcessor(uint32_t processorId) {
    std::lock_guard lock(mutex_);
    auto processor = processorLocked(processorId);
    std::erase(chainLocked(processors_[processorId].chainId).inserts, processor);
    processors_.erase(processorId);
    retireProcessorLocked(std::move(processor));
    rebuildSnapshotLocked();
}

void Engine::setChainOrder(uint32_t chainId, const std::vector<uint32_t>& processorIds) {
    std::lock_guard lock(mutex_);
    ChainModel& chain = chainLocked(chainId);
    std::vector<std::shared_ptr<Processor>> order;
    for (const uint32_t id : processorIds) {
        auto it = processors_.find(id);
        if (it == processors_.end() || it->second.chainId != chainId) {
            throw std::invalid_argument("Device " + std::to_string(id) + " is not in chain " + std::to_string(chainId));
        }
        if (std::find(order.begin(), order.end(), it->second.processor) != order.end()) {
            throw std::invalid_argument("Device " + std::to_string(id) + " is listed twice");
        }
        order.push_back(it->second.processor);
    }
    if (order.size() != chain.inserts.size()) {
        throw std::invalid_argument("The order must list all of the chain's devices");
    }
    chain.inserts = std::move(order);
    rebuildSnapshotLocked();
}

void Engine::moveProcessor(uint32_t processorId, uint32_t toChainId, int index) {
    std::lock_guard lock(mutex_);
    auto processor = processorLocked(processorId);
    ChainModel& to = chainLocked(toChainId);
    ProcessorEntry& entry = processors_[processorId];
    ChainModel& from = chainLocked(entry.chainId);
    if (entry.sidechain && from.stripId != to.stripId) checkSidechainLocked(entry.sidechain->source, to.stripId);
    std::erase(from.inserts, processor);
    to.inserts.insert(to.inserts.begin() + insertPosition(index, to.inserts.size()), processor);
    if (from.stripId != to.stripId) {
        // Another strip's signal: what it holds of the old one (a reverb's tail,
        // notes that strip will release elsewhere) stops.
        processor->requestReset();
    }
    entry.chainId = toChainId;
    rebuildSnapshotLocked();
}

ProcessorInfo Engine::processorInfo(uint32_t processorId) {
    auto p = processor(processorId);
    return {p->typeId(), p->name(), p->latencySamples(), p->tailSamples(), p->hasEditor(), p->hasSidechain()};
}

void Engine::setProcessorSidechain(uint32_t processorId, uint32_t sourceTrackId, SidechainTap tap,
                                   uint32_t tapProcessorId) {
    std::lock_guard lock(mutex_);
    const auto processor = processorLocked(processorId);
    ProcessorEntry& entry = processors_[processorId];
    if (!processor->hasSidechain()) {
        throw std::invalid_argument("Device " + std::to_string(processorId) + " has no sidechain input");
    }
    if (sourceTrackId == kMaster) {
        throw std::invalid_argument("The master can't be a sidechain: it renders after every track");
    }
    const TrackModel& source = arrangementTrackLocked(sourceTrackId);
    if (tap == SidechainTap::AfterDevice) {
        const auto found = processors_.find(tapProcessorId);
        const auto& inserts = insertsLocked(source);
        if (found == processors_.end() ||
            std::find(inserts.begin(), inserts.end(), found->second.processor) == inserts.end()) {
            throw std::invalid_argument("Device " + std::to_string(tapProcessorId) + " is not on track " +
                                        std::to_string(sourceTrackId));
        }
    } else {
        tapProcessorId = 0;
    }
    if (!entry.sidechain || entry.sidechain->source != sourceTrackId) {
        checkSidechainLocked(sourceTrackId, chainLocked(entry.chainId).stripId);
    }
    if (!entry.sidechain) {
        entry.sidechain.emplace();
        entry.sidechain->state = std::make_shared<EdgeState>(Renderer::kMaxBlock);
    }
    entry.sidechain->source = sourceTrackId;
    entry.sidechain->tap = tap;
    entry.sidechain->tapProcessor = tapProcessorId;
    rebuildSnapshotLocked();
}

void Engine::clearProcessorSidechain(uint32_t processorId) {
    std::lock_guard lock(mutex_);
    processorLocked(processorId);
    ProcessorEntry& entry = processors_[processorId];
    if (!entry.sidechain) return;
    entry.sidechain.reset();
    rebuildSnapshotLocked();
}

std::optional<SidechainInfo> Engine::processorSidechain(uint32_t processorId) {
    std::lock_guard lock(mutex_);
    processorLocked(processorId);
    const ProcessorEntry& entry = processors_[processorId];
    if (!entry.sidechain) return std::nullopt;
    return SidechainInfo{entry.sidechain->source, entry.sidechain->tap, entry.sidechain->tapProcessor};
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
        for (const auto& [id, entry] : processors_) current.emplace_back(id, entry.processor);
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
    discardMidiInputLocked();
}

void Engine::play(double countInBeats) {
    std::lock_guard lock(mutex_);
    requestedPlaying_.store(true);
    pushCommandLocked({TransportCommand::Type::Play, 0.0, std::max(0.0, countInBeats)});
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
    finishRecordingLocked();  // its input would have a hole
    if (!deviceRunning_) return;
    liveSuspended_.store(true, std::memory_order_seq_cst);
    // Once the epoch advances, any callback that started before the flag was
    // visible has finished; later callbacks output silence.
    waitForCallbackLocked();
}

void Engine::waitForCallbackLocked() {
    if (!deviceRunning_) return;
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
    for (auto& [id, entry] : processors_) {
        entry.processor->resetOffline();
        entry.processor->requestReset();
    }
}

void Engine::prepareOfflineLocked(Renderer& offline, WarpVoiceSet& voices,
                                  std::vector<std::shared_ptr<DelayLine>>& delays,
                                  std::vector<std::shared_ptr<DelayLine>>& deviceDelays, double startBeat) {
    // Fresh stretchers, as many as live playback has, so an offline render
    // starts from a clean state and leaves the live voices alone. Likewise
    // delay-compensation lines.
    for (int c = 0; c < kNumStretchConfigs; ++c) {
        for (size_t i = 0; i < snapshotHold_->warpVoices[c].size(); ++i) {
            voices[c].push_back(std::make_shared<WarpVoice>(static_cast<StretchConfig>(c), sampleRate_));
        }
    }
    for (const EdgeRender& edge : snapshotHold_->edges) {
        delays.push_back(edge.compensation > 0 ? std::make_shared<DelayLine>(edge.compensation + 1) : nullptr);
        deviceDelays.push_back(edge.deviceDelay > 0 ? std::make_shared<DelayLine>(edge.deviceDelay + 1) : nullptr);
    }
    offline.setScheduler(scheduler_.get());
    offline.setCostOrdering(renderer_.costOrdering());
    offline.prepare(sampleRate_);
    offline.setWarpVoices(&voices);
    offline.setDelayLines(&delays, &deviceDelays);
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
    std::vector<std::shared_ptr<DelayLine>> delays, deviceDelays;
    prepareOfflineLocked(offline, voices, delays, deviceDelays, startBeat);
    // With delay compensation the output lags the timeline: render the lag first and drop it.
    if (const int64_t lag = snapshotHold_->outputLatency(); lag > 0) {
        std::vector<float> discarded(static_cast<size_t>(lag) * 2);
        offline.renderOffline(*snapshotHold_, discarded.data(), lag, loop, metronome);
    }
    offline.renderOffline(*snapshotHold_, out, frames, loop, metronome);
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
    std::vector<std::shared_ptr<DelayLine>> delays, deviceDelays;
    prepareOfflineLocked(offline, voices, delays, deviceDelays, startBeat);

    constexpr int64_t kChunk = 16384;
    std::vector<float> rendered(kChunk * 2);
    for (int64_t lag = snapshotHold_->outputLatency(); lag > 0;) {  // see renderOfflineLocked()
        const int64_t n = std::min(kChunk, lag);
        offline.renderOffline(*snapshotHold_, rendered.data(), n);
        lag -= n;
    }
    std::vector<uint8_t> converted(kChunk * 2 * sizeof(float));
    const ma_dither_mode dither = bitDepth == 16 ? ma_dither_mode_triangle : ma_dither_mode_none;
    for (int64_t done = 0; done < total;) {
        const int64_t n = std::min(kChunk, total - done);
        offline.renderOffline(*snapshotHold_, rendered.data(), n);
        ma_pcm_convert(converted.data(), format, rendered.data(), ma_format_f32, static_cast<ma_uint64>(n * 2), dither);
        ma_encoder_write_pcm_frames(&encoder, converted.data(), static_cast<ma_uint64>(n), nullptr);
        done += n;
    }
    ma_encoder_uninit(&encoder);
}

// ---------------------------------------------------------------------------
// Audio threads

int Engine::defaultAudioThreads() {
    const auto cores = static_cast<int>(std::thread::hardware_concurrency());
    return std::clamp(cores - 1, 1, kMaxAudioThreads);
}

void Engine::setAudioThreads(int threads) {
    threads = std::clamp(threads, 1, kMaxAudioThreads);
    std::lock_guard lock(mutex_);
    if (scheduler_->threads() == threads) return;
    // The audio thread mustn't be inside the scheduler (or the renderer's
    // scratch) while they change: it outputs silence meanwhile. A recording goes
    // on (the playhead waits as well).
    const bool suspended = liveSuspended_.exchange(true, std::memory_order_seq_cst);
    waitForCallbackLocked();
    auto old = std::exchange(scheduler_, std::make_unique<Scheduler>(threads));
    renderer_.setScheduler(scheduler_.get());
    old.reset();  // its workers end
    if (!suspended) resumeLiveLocked();
}

int Engine::audioThreads() {
    std::lock_guard lock(mutex_);
    return scheduler_->threads();
}

uint64_t Engine::nodesOnWorkers() {
    std::lock_guard lock(mutex_);
    return scheduler_->nodesOnWorkers();
}

void Engine::setCostOrdering(bool on) { renderer_.setCostOrdering(on); }  // offline renders copy it

std::vector<TrackCost> Engine::trackCosts() {
    std::lock_guard lock(mutex_);
    std::vector<TrackCost> costs;
    costs.reserve(tracks_.size());
    for (const TrackModel& track : tracks_) {
        costs.push_back({track.id, track.buffers->cost.load(std::memory_order_relaxed)});
    }
    return costs;
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

    // Routing: the tracks in an order in which each comes after what feeds it
    // (through outputs, sends, inputs and sidechains alike).
    std::vector<EdgeOrigin> origins;  // per edge: what it is
    std::vector<RouteEdge> edges = routeEdgesLocked(&origins);
    const int count = static_cast<int>(tracks_.size());
    std::vector<int> order = topologicalOrder(count, edges);
    if (static_cast<int>(order.size()) != count) {
        // Every edge that would close a cycle is refused, so there is none; were
        // there one, the tracks would play straight into the master, without
        // sends, inputs from tracks or sidechains.
        assert(false && "the routing graph has a cycle");
        for (TrackModel& track : tracks_) {
            track.output = kMaster;
            track.sends.clear();
            if (track.inputTrack != kMaster) track.inputTrack.reset();
        }
        for (auto& [id, entry] : processors_) entry.sidechain.reset();
        edges = routeEdgesLocked(&origins);
        order = topologicalOrder(count, edges);
    }
    std::vector<int> position(tracks_.size());  // tracks_ index -> snapshot index
    for (size_t i = 0; i < order.size(); ++i) position[order[i]] = static_cast<int>(i);

    // Plug-in delay compensation at every summing point, per edge: each bus (a
    // group, a return, the master) hears its inputs as late as the latest of
    // them, which the enabled devices before each edge's tap (and those of what
    // feeds them) make; the other edges are delayed to line up with it. Both
    // taps (pre- and post-fader) come after every device of a strip. Input edges
    // aren't summed, so nothing lines up with them. A sidechain lines up with the
    // signal at its device (Routing.h): it is delayed, or that signal is, just
    // before the device.
    const auto chainLatencies = [this](const TrackModel& track) {
        constexpr int kMaxChainLatency = 1 << 20;  // in all
        std::vector<int> latencies;
        int total = 0;
        for (const auto& insert : insertsLocked(track)) {
            latencies.push_back(std::min(insertLatency(*insert), kMaxChainLatency - total));
            total += latencies.back();
        }
        return latencies;
    };
    std::vector<std::vector<int>> chains;  // each track's devices' latencies, then the master's
    chains.reserve(tracks_.size() + 1);
    for (const TrackModel& track : tracks_) chains.push_back(chainLatencies(track));
    chains.push_back(chainLatencies(master_));
    const GraphLatencies aligned = alignGraph(order, edges, chains);
    snap->maxLatency = aligned.masterInput;

    // The edges in snapshot order (by source; each track's output, then its
    // sends, then the input edges and sidechains it feeds), each node's incoming
    // and outgoing ones (what a bus sums, in that order), the sidechains into
    // each strip's devices and the taps after its devices, and the graph the
    // scheduler runs.
    const auto ensureDelay = [](std::shared_ptr<DelayLine>& delay, int samples) {
        if (samples > 0 && (!delay || delay->capacity() <= samples)) {
            delay = std::make_shared<DelayLine>(2 * samples + Renderer::kMaxBlock);
        }
    };
    std::vector<std::vector<int>> edgesOf(tracks_.size());
    for (size_t e = 0; e < edges.size(); ++e) edgesOf[static_cast<size_t>(edges[e].from)].push_back(static_cast<int>(e));
    std::vector<std::vector<int>> incoming(tracks_.size()), outgoing(tracks_.size());  // by snapshot index
    std::vector<std::vector<int>> deviceTaps(tracks_.size());                             // by snapshot index
    std::vector<std::vector<std::pair<int, int>>> sidechains(tracks_.size() + 1);       // (device, edge), the master last
    std::vector<int> inputEdge(tracks_.size(), -1);  // by snapshot index: the input edge it takes, if any
    std::vector<std::pair<int, int>> graphEdges;
    snap->edges.reserve(edges.size());
    for (const int t : order) {
        TrackModel& track = tracks_[static_cast<size_t>(t)];
        for (const int e : edgesOf[static_cast<size_t>(t)]) {
            const EdgeOrigin& origin = origins[static_cast<size_t>(e)];
            const int send = origin.send;
            const RouteEdge& route = edges[static_cast<size_t>(e)];
            const int index = static_cast<int>(snap->edges.size());
            EdgeRender edge;
            edge.from = position[static_cast<size_t>(t)];
            edge.to = route.to >= 0 ? position[static_cast<size_t>(route.to)] : -1;
            edge.compensation = aligned.compensation[static_cast<size_t>(e)];
            if (send == kInputEdge) {  // the destination's input: not summed, nor delayed
                edge.kind = EdgeRender::Kind::Input;
                edge.state = tracks_[static_cast<size_t>(origin.track)].inputState;
                inputEdge[static_cast<size_t>(edge.to)] = index;
            } else if (send == kSidechainEdge) {  // into one of the destination's devices
                SidechainModel& sidechain = *processors_.at(origin.processor).sidechain;
                edge.kind = EdgeRender::Kind::Sidechain;
                edge.state = sidechain.state;
                sidechainTapLocked(sidechain, edge.tap);
                edge.tapDevice = edge.tap == EdgeRender::Tap::AfterDevice ? route.tap - 1 : -1;
                edge.device = origin.device;
                edge.deviceDelay = aligned.deviceDelay[static_cast<size_t>(e)];
                ensureDelay(sidechain.delay, edge.compensation);
                ensureDelay(sidechain.deviceDelay, edge.deviceDelay);
                edge.delay = sidechain.delay;
                edge.deviceDelayLine = sidechain.deviceDelay;
                sidechains[edge.to >= 0 ? static_cast<size_t>(edge.to) : tracks_.size()].emplace_back(edge.device, index);
                if (edge.tap == EdgeRender::Tap::AfterDevice) deviceTaps[static_cast<size_t>(edge.from)].push_back(index);
            } else {
                std::shared_ptr<DelayLine>& delay =
                    send == kOutputEdge ? track.delay : track.sends[static_cast<size_t>(send)].delay;
                ensureDelay(delay, edge.compensation);
                edge.delay = delay;
            }
            if (send == kOutputEdge) {
                edge.state = track.outputState;
            } else if (send >= 0) {
                const SendModel& model = track.sends[static_cast<size_t>(send)];
                edge.kind = EdgeRender::Kind::Send;
                edge.tap = model.preFader ? EdgeRender::Tap::PreFader : EdgeRender::Tap::PostFader;
                edge.state = model.state;
                // Its level is applied where it is summed: as late as its destination hears its inputs.
                const std::string param = "send:" + std::to_string(model.to);
                for (const AutomationLaneDesc& desc : track.automation) {
                    if (desc.processorId != 0 || desc.param != param || desc.points.empty()) continue;
                    edge.level.nodes = automationNodes(desc, spb);
                    edge.level.latency = aligned.inputLatency[static_cast<size_t>(route.to)];
                }
            }
            outgoing[static_cast<size_t>(edge.from)].push_back(index);
            if (edge.to >= 0) {
                incoming[static_cast<size_t>(edge.to)].push_back(index);
                graphEdges.emplace_back(edge.from, edge.to);
            } else if (edge.sums()) {
                snap->masterInputs.push_back(index);
            }
            snap->edges.push_back(std::move(edge));
        }
    }
    snap->graph = std::make_shared<TaskGraph>(count, graphEdges);
    for (auto& taps : deviceTaps) {
        std::stable_sort(taps.begin(), taps.end(), [&](int a, int b) {
            return snap->edges[static_cast<size_t>(a)].tapDevice < snap->edges[static_cast<size_t>(b)].tapDevice;
        });
    }
    const auto sidechainsOf = [&](size_t strip, size_t devices) {
        std::vector<int> into;
        if (sidechains[strip].empty()) return into;
        into.assign(devices, -1);
        for (const auto [device, edge] : sidechains[strip]) {
            if (device >= 0 && static_cast<size_t>(device) < devices) into[static_cast<size_t>(device)] = edge;
        }
        return into;
    };

    // The master: its input is the sum of what goes into it, which comes maxLatency late.
    StripRender& master = snap->master;
    master.params = master_.params;
    master.inserts = insertsLocked(master_);
    const std::vector<int>& masterDevices = aligned.deviceLatency[tracks_.size()];
    master.latency = masterDevices.back();
    master.sidechains = sidechainsOf(tracks_.size(), master.inserts.size());
    buildAutomationLocked(master_, snap->maxLatency, masterDevices, snap->outputLatency(), spb, master);

    const auto rate = static_cast<uint32_t>(sampleRate_);
    std::array<size_t, kNumStretchConfigs> voicesNeeded{};
    snap->tracks.reserve(tracks_.size());
    for (const int t : order) {
        TrackModel& track = tracks_[t];
        TrackRender render;
        render.id = track.id;
        render.params = track.params;
        render.inserts = insertsLocked(track);
        const std::vector<int>& devices = aligned.deviceLatency[t];
        render.latency = devices.back();
        render.inputLatency = aligned.inputLatency[t];
        const size_t at = static_cast<size_t>(position[static_cast<size_t>(t)]);
        render.incoming = std::move(incoming[at]);
        render.outgoing = std::move(outgoing[at]);
        render.sidechains = sidechainsOf(at, render.inserts.size());
        render.deviceTaps = std::move(deviceTaps[at]);
        render.inputCount = static_cast<int>(render.incoming.size());
        render.buffers = track.buffers;
        render.input = inputEdgeLocked(track);
        render.input.edge = inputEdge[at];
        render.midiInput = track.midiInput;
        render.monitor = track.monitor;
        render.armed = track.armed;
        // Its devices hear the timeline as late as its input; its fader after them
        // (its edges are delayed after the fader, to line up where they go).
        const int faderLatency = render.inputLatency + render.latency;
        buildAutomationLocked(track, render.inputLatency, devices, faderLatency, spb, render);
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
        // Worth a thread of its own: devices to run, or clips to stretch.
        const bool enabledDevice = std::any_of(render.inserts.begin(), render.inserts.end(),
                                               [](const auto& insert) { return insert->isEnabled(); });
        const bool warping = std::any_of(render.clips.begin(), render.clips.end(), [](const ClipRender& clip) {
            return clip.playback != ClipRender::Playback::Direct;
        });
        if (enabledDevice || warping) ++snap->parallelWork;
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
        for (const auto& [id, entry] : processors_) live.push_back(entry.processor);
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
