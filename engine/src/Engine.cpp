// The engine's lifetime, sources, transport, browser preview, audio threads and
// housekeeping. The rest of Engine is implemented by area: EngineDevice.cpp,
// EngineTracks.cpp, EngineInput.cpp, EngineChains.cpp, EngineSnapshot.cpp and
// EngineOffline.cpp.
#include "Engine.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>

#include "platform/Paths.h"

namespace sub {

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
    if (RenderJob* job = std::exchange(job_, nullptr)) {  // its thread mustn't outlive the engine
        job->cancel();
        if (job->thread_.joinable()) job->thread_.join();
        job->engine_ = nullptr;
    }
    midiDevices_.closeAll();
    closeDeviceLocked();
}

std::string Engine::sourceKey(const std::string& path) { return platform::fileKey(path); }

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
// Audio threads

int Engine::defaultAudioThreads() {
    const auto cores = static_cast<int>(std::thread::hardware_concurrency());
    return std::clamp(cores - 1, 1, kMaxAudioThreads);
}

void Engine::setAudioThreads(int threads) {
    threads = std::clamp(threads, 1, kMaxAudioThreads);
    std::lock_guard lock(mutex_);
    if (scheduler_->threads() == threads) return;
    checkNotRenderingLocked();  // (a render in the background uses the scheduler)
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
// Housekeeping

void Engine::collectGarbageLocked() {
    releasePool_.collect(audioEpoch_.load(std::memory_order_seq_cst), !deviceRunning_);
}

void Engine::idle(bool releaseAll) {
    std::vector<std::shared_ptr<Processor>> live;
    std::vector<std::shared_ptr<Processor>> dead;
    {
        std::lock_guard lock(mutex_);
        const bool rendering = job_ != nullptr;  // processors wait (a plug-in restarting would leave a gap in it)
        if (deviceRunning_ && (pendingDeviceEvents_.load() & static_cast<uint32_t>(DeviceEvent::Stopped))) {
            closeDeviceLocked();  // the backend lost the device; the UI reports it via takeDeviceEvent()
        }
        collectGarbageLocked();
        serviceTransportIfIdleLocked();
        if (!rendering) {
            for (const auto& [id, entry] : processors_) live.push_back(entry.processor);
        }
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
    // Plug-ins may take their time to go: not under the lock, and a few at a
    // time; those left wait for the next call.
    constexpr auto kReleaseBudget = std::chrono::milliseconds(20);
    const auto started = std::chrono::steady_clock::now();
    size_t released = 0;
    while (released < dead.size() && (releaseAll || std::chrono::steady_clock::now() - started < kReleaseBudget)) {
        dead[released++].reset();
    }
    if (released < dead.size()) {
        std::lock_guard lock(mutex_);
        graveyard_.insert(graveyard_.begin(), std::make_move_iterator(dead.begin() + static_cast<std::ptrdiff_t>(released)),
                          std::make_move_iterator(dead.end()));
    }
    dead.clear();

    bool realign = false;
    for (const auto& p : live) realign |= p->idle();
    if (live.empty()) return;
    std::lock_guard lock(mutex_);
    // A processor's idle() compares its latency with what it last reported, which may not be what the
    // snapshot was aligned to (a change, another edit rebuilding the snapshot, then the change undone):
    // check against the snapshot too.
    for (auto it = processors_.begin(); !realign && it != processors_.end(); ++it) {
        realign = !it->second.rack && insertLatency(*it->second.processor) != it->second.alignedLatency;
    }
    if (realign && job_ == nullptr) rebuildSnapshotLocked();  // a latency changed: new delay compensation
}

}  // namespace sub
