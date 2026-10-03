// Engine: track inputs, recording, and MIDI input.
#include "Engine.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "Routing.h"

namespace sub {

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

}  // namespace sub
