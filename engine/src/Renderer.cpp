#include "Renderer.h"

#include <algorithm>
#include <cmath>

namespace gil {

void Renderer::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    for (auto* buffer : {&trackLeft_, &trackRight_, &masterLeft_, &masterRight_, &warpLeft_, &warpRight_, &autoGain_,
                         &autoPanLeft_, &autoPanRight_, &silence_}) {
        buffer->assign(kMaxBlock, 0.f);
    }
    recordScratch_.assign(2 * kMaxBlock, 0.f);
    countIn_ = countInTotal_ = 0;
    metronome_.prepare(sampleRate);
    previewSource_ = nullptr;
    // Processors are prepared (silenced) along with the renderer.
    activeNotes_.assign(kMaxActiveNotes, {});
    numActiveNotes_ = 0;
    events_.assign(kMaxEvents, {});
    numEvents_ = 0;
    previewNotes_.assign(kMaxPreviewNotes, {});
    numPreviewNotes_ = 0;
    expectedPosition_ = -1;
    pendingTickStart_ = 0;
    numPendingTicks_ = 0;
    outputTime_ = 0;
}

void Renderer::syncTempo(const RenderSnapshot& snap) noexcept {
    // Tempo (or sample rate) changed: keep the playhead, and the ends of the
    // sounding notes, on the same beat.
    const double spb = snap.samplesPerBeat();
    if (samplesPerBeat_ > 0.0 && spb != samplesPerBeat_) {
        const double ratio = spb / samplesPerBeat_;
        const auto rescale = [ratio](int64_t t) { return std::llround(static_cast<double>(t) * ratio); };
        position_ = rescale(position_);
        if (expectedPosition_ >= 0) expectedPosition_ = rescale(expectedPosition_);
        for (int i = 0; i < numActiveNotes_; ++i) activeNotes_[i].end = rescale(activeNotes_[i].end);
    }
    samplesPerBeat_ = spb;
}

void Renderer::drainCommands(SharedState& shared) noexcept {
    TransportCommand command;
    while (shared.commands.pop(command)) applyCommand(command);
}

void Renderer::applyCommand(const TransportCommand& command) noexcept {
    switch (command.type) {
        case TransportCommand::Type::Play:
            chasePending_ = chasePending_ || !playing_;  // (live playback only: offline renders start clean)
            if (!playing_ && command.countInBeats > 0.0 && samplesPerBeat_ > 0.0) {
                countIn_ = countInTotal_ = std::llround(command.countInBeats * samplesPerBeat_);
            }
            playing_ = true;
            break;
        case TransportCommand::Type::Stop:
            playing_ = false;
            countIn_ = 0;
            break;
        case TransportCommand::Type::Locate:
            setPosition(std::llround(std::max(0.0, command.beat) * samplesPerBeat_));
            break;
    }
}

void Renderer::drainPreviewNotes(SharedState& shared) noexcept {
    PreviewNote note;
    while (shared.previewNotes.pop(note)) {
        if (numPreviewNotes_ < static_cast<int>(previewNotes_.size())) previewNotes_[numPreviewNotes_++] = note;
    }
}

void Renderer::discardPreviewNotes(SharedState& shared) noexcept {
    PreviewNote note;
    while (shared.previewNotes.pop(note)) {
    }
}

void Renderer::publishTransport(SharedState& shared) const noexcept {
    shared.positionSamples.store(position_, std::memory_order_relaxed);
    shared.positionBeats.store(samplesPerBeat_ > 0.0 ? position_ / samplesPerBeat_ : 0.0, std::memory_order_relaxed);
    shared.playing.store(playing_, std::memory_order_relaxed);
    shared.countingIn.store(playing_ && countIn_ > 0, std::memory_order_relaxed);
}

void Renderer::processLive(const RenderSnapshot& snap, SharedState& shared, const AudioIO& io,
                           RecordingSession* recording) noexcept {
    syncTempo(snap);
    drainCommands(shared);
    drainPreviewNotes(shared);
    inputs_ = io.inputs;
    numInputs_ = static_cast<int>(io.numInputs);
    recording_ = recording;
    float* const* outputs = io.outputs;
    const uint32_t numOutputs = io.numOutputs;
    const uint32_t frames = io.frames;

    uint32_t done = 0;
    while (done < frames) {
        const int n = static_cast<int>(std::min<uint32_t>(kMaxBlock, frames - done));
        inputOffset_ = static_cast<int>(done);
        renderChunk(snap, n,
                    {true, snap.loopEnabled, shared.metronome.load(std::memory_order_relaxed)});
        numPreviewNotes_ = 0;  // played in the first chunk
        mixPreview(shared, n);

        if (numOutputs == 1) {
            float* dst = outputs[0] + done;
            for (int i = 0; i < n; ++i) dst[i] = 0.5f * (masterLeft_[i] + masterRight_[i]);
        } else if (numOutputs >= 2) {
            std::copy_n(masterLeft_.data(), n, outputs[0] + done);
            std::copy_n(masterRight_.data(), n, outputs[1] + done);
            for (uint32_t c = 2; c < numOutputs; ++c) std::fill_n(outputs[c] + done, n, 0.f);
        }
        shared.pushScope(masterLeft_.data(), masterRight_.data(), n);
        done += static_cast<uint32_t>(n);
    }
    inputs_ = nullptr;
    numInputs_ = 0;
    recording_ = nullptr;
    publishTransport(shared);
}

void Renderer::renderOffline(const RenderSnapshot& snap, float* outStereo, int64_t frames,
                             bool loop, bool metronome) noexcept {
    syncTempo(snap);
    int64_t done = 0;
    while (done < frames) {
        const int n = static_cast<int>(std::min<int64_t>(kMaxBlock, frames - done));
        renderChunk(snap, n, {false, loop && snap.loopEnabled, metronome});
        float* dst = outStereo + done * 2;
        for (int i = 0; i < n; ++i) {
            dst[2 * i] = masterLeft_[i];
            dst[2 * i + 1] = masterRight_[i];
        }
        done += n;
    }
}

void Renderer::renderChunk(const RenderSnapshot& snap, int frames, ChunkFlags flags) noexcept {
    const int64_t chunkStart = position_;
    const WarpVoiceSet& voices = voiceOverride_ ? *voiceOverride_ : snap.warpVoices;
    ++blockCounter_;

    // 1. Split the chunk into contiguous timeline segments (loop wrap-around).
    numSegments_ = 0;
    numTicks_ = 0;
    if (playing_) {
        int done = 0;
        if (countIn_ > 0) {  // clicks only: the playhead waits
            done = static_cast<int>(std::min<int64_t>(frames, countIn_));
            scheduleCountIn(snap, done);
            countIn_ -= done;
        }
        bool chase = chasePending_ && done < frames;
        if (done < frames) chasePending_ = false;
        // Recording goes straight on: the loop doesn't wrap (punching in and out of it comes later).
        const bool loop = flags.loop && !recording_;
        while (done < frames) {
            int length = frames - done;
            const bool looping = loop && numSegments_ < kMaxSegments - 1;
            if (looping && position_ < snap.loopEnd && position_ + length > snap.loopEnd) {
                length = static_cast<int>(snap.loopEnd - position_);
            }
            segments_[numSegments_++] = {position_, length, done, position_ != expectedPosition_, chase};
            chase = false;
            if (flags.metronome) scheduleTicks(snap, position_, length, done);
            position_ += length;
            expectedPosition_ = position_;
            done += length;
            if (looping && position_ == snap.loopEnd) position_ = snap.loopStart;
        }
    }

    const double spb = snap.samplesPerBeat();
    ProcessContext context;
    context.sampleRate = snap.sampleRate;
    context.samplePos = chunkStart;
    context.beatPos = chunkStart / spb;
    context.tempo = snap.tempo;
    context.timeSigNum = snap.timeSigNum;
    context.timeSigDen = snap.timeSigDen;
    context.playing = playing_;
    context.looping = flags.loop && !recording_;
    context.loopStartBeat = snap.loopStart / spb;
    context.loopEndBeat = snap.loopEnd / spb;
    context.offline = !flags.live;

    bool anySolo = false;
    for (const TrackRender& track : snap.tracks) {
        if (track.params->solo.load(std::memory_order_relaxed)) {
            anySolo = true;
            break;
        }
    }

    if (recording_ && numSegments_ > 0) recordInput();

    float* masterL = masterLeft_.data();
    float* masterR = masterRight_.data();
    std::fill_n(masterL, frames, 0.f);
    std::fill_n(masterR, frames, 0.f);

    // 2. Tracks: clips and notes -> their strips -> the master's input.
    for (size_t t = 0; t < snap.tracks.size(); ++t) {
        const TrackRender& track = snap.tracks[t];
        float* left = trackLeft_.data();
        float* right = trackRight_.data();
        std::fill_n(left, frames, 0.f);
        std::fill_n(right, frames, 0.f);

        const bool monitored = isMonitored(track, flags);
        if (monitored) {
            readInput(track.input, left, right, frames);
        } else {
            for (int s = 0; s < numSegments_; ++s) renderClips(track, segments_[s], snap.clipFadeSamples, voices);
        }
        if (!track.notes.empty() || numActiveNotes_ > 0 || numPreviewNotes_ > 0) {
            buildNoteEvents(track);
        } else {
            numEvents_ = 0;
        }

        DelayLine* delay = delayOverride_ ? (t < delayOverride_->size() ? (*delayOverride_)[t].get() : nullptr)
                                          : track.delay.get();
        const TrackParams& params = *track.params;
        const bool audible = !params.mute.load(std::memory_order_relaxed) &&
                             (!anySolo || params.solo.load(std::memory_order_relaxed));
        processStrip(snap, track, context, left, right, frames, audible, flags, delay,
                     compensationFor(track, monitored));
        for (int i = 0; i < frames; ++i) {
            masterL[i] += left[i];
            masterR[i] += right[i];
        }
    }

    forgetNotesOfRemovedTracks(snap);

    // 3. The master strip (it never mutes, and nothing is later than it: no compensation).
    if (snap.master.params) {
        numEvents_ = 0;
        processStrip(snap, snap.master, context, masterL, masterR, frames, true, flags, nullptr, 0);
    }

    // 4. Metronome, after the master fader (a click may still be ringing out).
    renderTicks(frames);
    int cursor = 0;
    for (int t = 0; t < numTicks_; ++t) {
        metronome_.renderUntil(masterL, masterR, cursor, ticks_[t].offset);
        metronome_.start(ticks_[t].accent);
        cursor = ticks_[t].offset;
    }
    metronome_.renderUntil(masterL, masterR, cursor, frames);
    outputTime_ += frames;
}

void Renderer::processStrip(const RenderSnapshot& snap, const StripRender& strip, ProcessContext& context,
                            float* left, float* right, int frames, bool audible, ChunkFlags flags,
                            DelayLine* delay, int compensation) noexcept {
    processInserts(strip, context, left, right, frames, snap.samplesPerBeat());
    if (delay) delay->process(left, right, frames, compensation);
    applyFader(snap, *strip.params, strip.volume, strip.pan, audible, left, right, frames, flags.live);
}

int Renderer::compensationFor(const StripRender& strip, bool monitored) noexcept {
    // Strips are delayed to line up with the one whose devices add the most
    // latency (strip.compensation, worked out with the snapshot). The exception:
    // a monitored strip plays its live input as soon as it can, so a player hears
    // themselves with only the latency of the strip's own devices (monitoring
    // latency), not every other track's.
    return monitored ? 0 : strip.compensation;
}

bool Renderer::isMonitored(const TrackRender& track, ChunkFlags flags) const noexcept {
    if (!flags.live || !track.input.fromDevice()) return false;  // offline renders play the arrangement
    switch (track.monitor) {
        case MonitorMode::In: return true;
        case MonitorMode::Auto: return track.armed && (!playing_ || recording_ != nullptr);
        case MonitorMode::Off: break;
    }
    return false;
}

const float* Renderer::inputChannel(int index, int offset) const noexcept {
    if (index < 0 || index >= numInputs_ || !inputs_) return silence_.data() + offset;  // not open
    return inputs_[index] + inputOffset_ + offset;
}

void Renderer::readInput(const InputEdge& input, float* left, float* right, int frames) const noexcept {
    std::copy_n(inputChannel(input.left, 0), frames, left);
    std::copy_n(inputChannel(input.right, 0), frames, right);
}

void Renderer::recordInput() noexcept {
    RecordingSession& session = *recording_;
    for (int s = 0; s < numSegments_; ++s) {
        if (session.interrupted()) return;
        const Segment& segment = segments_[s];
        for (const auto& take : session.takes()) {
            if (take->start.load(std::memory_order_relaxed) == RecordingTake::kNotStarted) {
                take->start.store(segment.position, std::memory_order_release);
            } else if (segment.jump) {  // located, or stopped and started again: the takes end
                session.interrupt();
                return;
            }
        }
        for (const auto& take : session.takes()) {
            take->push(inputChannel(take->inputs[0], segment.offset), inputChannel(take->inputs[1], segment.offset),
                       segment.length, recordScratch_.data());
        }
    }
}

void Renderer::scheduleCountIn(const RenderSnapshot& snap, int length) noexcept {
    // A tick every beat (of the time signature) from the count-in's start, the
    // first of each bar accented; it ends where the playhead starts.
    const double tickLength = snap.samplesPerBeat() * 4.0 / snap.timeSigDen;
    if (tickLength <= 0.0) return;
    const int64_t from = countInTotal_ - countIn_;
    for (int64_t k = static_cast<int64_t>(std::ceil(from / tickLength - 1e-9));; ++k) {
        const int64_t t = std::llround(k * tickLength);
        if (t < from) continue;
        if (t >= from + length || t >= countInTotal_) break;
        if (numPendingTicks_ == kMaxPendingTicks) break;
        const int64_t time = outputTime_ + (t - from) + snap.outputLatency();  // as late as the tracks' audio
        pendingTicks_[(pendingTickStart_ + numPendingTicks_++) % kMaxPendingTicks] = {time, k % snap.timeSigNum == 0};
    }
}

void Renderer::processInserts(const StripRender& strip, ProcessContext& context, float* left, float* right,
                              int frames, double samplesPerBeat) noexcept {
    bool any = false;
    for (const auto& insert : strip.inserts) {
        if (!insert->isEnabled()) continue;
        if (insert->takeResetRequest()) insert->reset();
        any = true;
    }
    if (!any) return;

    // One call per continuous stretch of the timeline, with the events that fall in it.
    const bool split = playing_ && numSegments_ > 0;
    // A count-in that ends in this chunk: the playhead stood still until the first segment.
    const int lead = split && segments_[0].offset > 0 ? 1 : 0;
    const int slices = split ? numSegments_ + lead : 1;
    int next = 0;
    for (int s = 0; s < slices; ++s) {
        const int segment = s - lead;
        const bool moving = split && segment >= 0;
        const int offset = moving ? segments_[segment].offset : 0;
        const int length = moving ? segments_[segment].length : split ? segments_[0].offset : frames;
        // Stopped (or counting in): it stays put.
        const int64_t position = moving ? segments_[segment].position : split ? segments_[0].position : position_;
        const int first = next;
        while (next < numEvents_ && (s == slices - 1 || events_[next].sampleOffset < offset + length)) {
            events_[next].sampleOffset -= offset;
            ++next;
        }
        context.samplePos = position;
        context.beatPos = position / samplesPerBeat;
        context.inEvents = {events_.data() + first, static_cast<size_t>(next - first)};
        float* channels[2] = {left + offset, right + offset};
        for (size_t i = 0; i < strip.inserts.size(); ++i) {
            Processor& insert = *strip.inserts[i];
            if (!insert.isEnabled()) continue;
            for (const AutomationRender& lane : strip.automation) {
                if (lane.insert == static_cast<int>(i)) automateInsert(lane, position, length, moving);
            }
            insert.process(context, channels, 2, length);
            insert.clearAutomation();
        }
    }
}

void Renderer::automateInsert(const AutomationRender& lane, int64_t position, int length, bool moving) noexcept {
    Processor& processor = *lane.processor;
    const auto& nodes = lane.nodes;
    const int64_t from = automationTime(position, lane.latency);
    size_t index = automationIndex(nodes, from);
    if (!moving) {  // stopped: the value at the playhead
        processor.automate(lane.param, automationQuantize(automationValueAt(nodes, index, from), lane.steps), 0);
        return;
    }
    float last = -1.f;
    for (int offset = 0; offset < length;) {
        const int64_t t = from + offset;
        while (index < nodes.size() && nodes[index].time <= t) ++index;
        const float value = automationQuantize(automationValueAt(nodes, index, t), lane.steps);
        if (value != last) {
            if (!processor.automate(lane.param, value, offset)) return;
            last = value;
        }
        int64_t step = kAutomationStep;
        if (index < nodes.size()) step = std::min<int64_t>(step, nodes[index].time - t);  // > 0
        offset += static_cast<int>(step);
    }
}

void Renderer::fillLane(const AutomationRender& lane, int frames, float* out) const noexcept {
    if (!playing_ || numSegments_ == 0) {
        std::fill_n(out, frames, automationValue(lane.nodes, automationTime(position_, lane.latency)));
        return;
    }
    if (segments_[0].offset > 0) {  // the end of a count-in: still at the first segment's start
        std::fill_n(out, segments_[0].offset, automationValue(lane.nodes, automationTime(segments_[0].position, lane.latency)));
    }
    for (int s = 0; s < numSegments_; ++s) {
        const Segment& segment = segments_[s];
        fillAutomation(lane.nodes, automationTime(segment.position, lane.latency), segment.length,
                       out + segment.offset);
    }
}

void Renderer::applyFader(const RenderSnapshot& snap, TrackParams& params, const AutomationRender& volume,
                          const AutomationRender& pan, bool audible, float* left, float* right, int frames,
                          bool live) noexcept {
    const float* autoGain = nullptr;
    const float* autoLeft = nullptr;
    const float* autoRight = nullptr;
    if (!volume.empty()) {
        fillLane(volume, frames, autoGain_.data());
        for (int i = 0; i < frames; ++i) autoGain_[i] = automationVolumeGain(autoGain_[i]);
        autoGain = autoGain_.data();
    }
    if (!pan.empty()) {
        fillLane(pan, frames, autoPanLeft_.data());
        for (int i = 0; i < frames; ++i) {
            balanceGains(automationPan(autoPanLeft_[i]), autoPanLeft_[i], autoPanRight_[i]);
        }
        autoLeft = autoPanLeft_.data();
        autoRight = autoPanRight_.data();
    }
    const float on = audible ? 1.f : 0.f;
    const float gain = params.gain.load(std::memory_order_relaxed);
    float panLeft, panRight;
    balanceGains(params.pan.load(std::memory_order_relaxed), panLeft, panRight);

    if (!live) {
        // Offline renders hold the engine lock, so parameters cannot change
        // mid-render; apply them directly and leave the live ramps alone.
        for (int i = 0; i < frames; ++i) {
            const float g = on * (autoGain ? autoGain[i] : gain);
            left[i] *= g * (autoLeft ? autoLeft[i] : panLeft);
            right[i] *= g * (autoRight ? autoRight[i] : panRight);
        }
        return;
    }
    // Smoothing state lives with the track so it survives snapshot swaps.
    if (params.smoothingSampleRate != snap.sampleRate) {
        for (SmoothedValue* smoothed : {&params.audible, &params.volume, &params.panLeft, &params.panRight}) {
            smoothed->reset(snap.sampleRate, 0.02);
        }
        params.audible.snapTo(on);
        params.volume.snapTo(gain);
        params.panLeft.snapTo(panLeft);
        params.panRight.snapTo(panRight);
        params.smoothingSampleRate = snap.sampleRate;
    }
    params.audible.setTarget(on);
    params.volume.setTarget(gain);
    params.panLeft.setTarget(panLeft);
    params.panRight.setTarget(panRight);
    float peakL = 0.f, peakR = 0.f;
    for (int i = 0; i < frames; ++i) {
        const float a = params.audible.next();
        float g = params.volume.next();
        float l = params.panLeft.next();
        float r = params.panRight.next();
        if (autoGain) g = autoGain[i];
        if (autoLeft) {
            l = autoLeft[i];
            r = autoRight[i];
        }
        left[i] *= a * g * l;
        right[i] *= a * g * r;
        peakL = std::max(peakL, std::abs(left[i]));
        peakR = std::max(peakR, std::abs(right[i]));
    }
    // Where automation stops (or is overridden), the manual value takes over from its last value.
    if (autoGain && frames > 0) params.volume.snapTo(autoGain[frames - 1]);
    if (autoLeft && frames > 0) {
        params.panLeft.snapTo(autoLeft[frames - 1]);
        params.panRight.snapTo(autoRight[frames - 1]);
    }
    atomicStoreMax(params.peakLeft, peakL);
    atomicStoreMax(params.peakRight, peakR);
}

WarpVoice* Renderer::acquireVoice(const WarpVoiceSet& voices, const ClipRender& clip, bool& continuing) noexcept {
    // The voice that played this clip last, if it is still ours; otherwise the
    // voice idle the longest. A voice already used in this block is never taken.
    const auto& pool = voices[static_cast<size_t>(clip.stretchConfig)];
    WarpVoice* best = nullptr;
    for (const auto& voice : pool) {
        if (voice->key == clip.key && voice->lastUsed != 0) {
            best = voice.get();
            break;
        }
        if (voice->lastUsed < blockCounter_ && (!best || voice->lastUsed < best->lastUsed)) best = voice.get();
    }
    if (!best) return nullptr;  // more simultaneous warped clips than voices: this one stays silent
    // The previous block already stamped it (or, after a loop wrap, this block).
    continuing = best->key == clip.key && best->lastUsed + 1 >= blockCounter_;
    best->key = clip.key;
    best->lastUsed = blockCounter_;
    return best;
}

void Renderer::renderClips(const TrackRender& track, const Segment& segment, int64_t clipFade,
                           const WarpVoiceSet& voices) noexcept {
    const int64_t segStart = segment.position;
    const int64_t segEnd = segStart + segment.length;
    float* outL = trackLeft_.data() + segment.offset;
    float* outR = trackRight_.data() + segment.offset;

    // Clips are sorted by start; no clip starting before this bound can reach the segment.
    const int64_t earliest = segStart - track.maxClipLength;
    auto it = std::lower_bound(track.clips.begin(), track.clips.end(), earliest,
                               [](const ClipRender& clip, int64_t value) { return clip.start < value; });
    for (; it != track.clips.end() && it->start < segEnd; ++it) {
        const ClipRender& clip = *it;
        const int64_t from = std::max(segStart, clip.start);
        const int64_t to = std::min(segEnd, clip.start + clip.length);
        if (from >= to) continue;

        // Where this clip's unscaled audio for [from, to) comes from.
        const float* srcL;
        const float* srcR;
        int64_t srcBase;  // index into srcL/srcR of timeline sample t is t - srcBase
        if (clip.playback == ClipRender::Playback::Direct) {
            const AudioSource& source = *clip.source;
            srcL = source.channelData(0);
            srcR = source.channels() > 1 ? source.channelData(1) : srcL;
            srcBase = clip.start - clip.sourceOffset;
        } else {
            const int n = static_cast<int>(to - from);
            if (clip.playback == ClipRender::Playback::Resample) {
                renderResampled(clip, from, n, warpLeft_.data(), warpRight_.data());
            } else {
                bool continuing = false;
                WarpVoice* voice = acquireVoice(voices, clip, continuing);
                if (!voice) continue;
                voice->render(clip, from, n, continuing, warpLeft_.data(), warpRight_.data());
            }
            srcL = warpLeft_.data();
            srcR = warpRight_.data();
            srcBase = from;
        }

        const int64_t fade = std::min(clipFade, clip.length / 2);
        for (int64_t t = from; t < to; ++t) {
            const int64_t inClip = t - clip.start;
            float g = clip.gain;
            if (fade > 0) {
                if (inClip < fade) g *= static_cast<float>(inClip) / fade;
                const int64_t toEnd = clip.length - inClip;
                if (toEnd < fade) g *= static_cast<float>(toEnd) / fade;
            }
            const int64_t src = t - srcBase;
            const int64_t dst = t - segStart;
            outL[dst] += srcL[src] * g * clip.panLeft;
            outR[dst] += srcR[src] * g * clip.panRight;
        }
    }
}

bool Renderer::pushEvent(const ProcessEvent& event) noexcept {
    if (numEvents_ >= static_cast<int>(events_.size())) return false;
    events_[numEvents_++] = event;
    return true;
}

void Renderer::releaseNotes(uint32_t trackId, int offset) noexcept {
    for (int a = 0; a < numActiveNotes_;) {
        if (activeNotes_[a].trackId != trackId) {
            ++a;
            continue;
        }
        if (!pushEvent(ProcessEvent::noteOff(offset, activeNotes_[a].key))) return;  // next block
        activeNotes_[a] = activeNotes_[--numActiveNotes_];
    }
}

void Renderer::buildNoteEvents(const TrackRender& track) noexcept {
    // Notes played by hand come first, all at the block start and in the order
    // they were played: dragging a note across keys releases one key and plays
    // the next several times within a block, and reordering those would leave
    // notes playing whose note-off came first.
    numEvents_ = 0;
    for (int i = 0; i < numPreviewNotes_; ++i) {
        const PreviewNote& note = previewNotes_[i];
        if (note.trackId != track.id) continue;
        pushEvent(note.velocity > 0 ? ProcessEvent::noteOn(0, note.key, note.velocity)
                                    : ProcessEvent::noteOff(0, note.key));
    }
    const int previewEvents = numEvents_;
    if (!playing_) releaseNotes(track.id, 0);

    for (int s = 0; s < numSegments_; ++s) {
        const Segment& segment = segments_[s];
        if (segment.jump) releaseNotes(track.id, segment.offset);
        const int64_t segEnd = segment.position + segment.length;

        // Notes already underway where playback starts sound from there.
        if (segment.chase) {
            for (const NoteRender& note : track.notes) {
                if (note.start >= segment.position) break;
                if (note.end <= segment.position) continue;
                if (numActiveNotes_ == static_cast<int>(activeNotes_.size())) break;
                if (!pushEvent(ProcessEvent::noteOn(segment.offset, note.key, note.velocity))) break;
                activeNotes_[numActiveNotes_++] = {track.id, note.key, note.end};
            }
        }

        // Note-ons: notes starting in this segment.
        auto it = std::lower_bound(track.notes.begin(), track.notes.end(), segment.position,
                                   [](const NoteRender& note, int64_t value) { return note.start < value; });
        for (; it != track.notes.end() && it->start < segEnd; ++it) {
            if (numActiveNotes_ == static_cast<int>(activeNotes_.size())) break;
            const auto offset = static_cast<int32_t>(segment.offset + (it->start - segment.position));
            if (!pushEvent(ProcessEvent::noteOn(offset, it->key, it->velocity))) break;
            activeNotes_[numActiveNotes_++] = {track.id, it->key, it->end};
        }

        // Note-offs due in this segment, including for notes that just started.
        for (int a = 0; a < numActiveNotes_;) {
            const ActiveNote& note = activeNotes_[a];
            if (note.trackId != track.id || note.end >= segEnd) {
                ++a;
                continue;
            }
            const auto offset =
                static_cast<int32_t>(segment.offset + std::max<int64_t>(0, note.end - segment.position));
            if (!pushEvent(ProcessEvent::noteOff(offset, note.key))) break;
            activeNotes_[a] = activeNotes_[--numActiveNotes_];
        }
    }

    // The arrangement's notes in time order; at the same offset a note-off comes
    // first, so a note that ends where the next one on its key starts doesn't cut
    // the new one short.
    std::sort(events_.begin() + previewEvents, events_.begin() + numEvents_,
              [](const ProcessEvent& a, const ProcessEvent& b) {
                  if (a.sampleOffset != b.sampleOffset) return a.sampleOffset < b.sampleOffset;
                  return a.type == ProcessEvent::Type::NoteOff && b.type != ProcessEvent::Type::NoteOff;
              });
}

void Renderer::forgetNotesOfRemovedTracks(const RenderSnapshot& snap) noexcept {
    // Their instruments are gone along with the track.
    for (int a = 0; a < numActiveNotes_;) {
        const uint32_t id = activeNotes_[a].trackId;
        const bool exists = std::any_of(snap.tracks.begin(), snap.tracks.end(),
                                        [id](const TrackRender& track) { return track.id == id; });
        if (exists) {
            ++a;
        } else {
            activeNotes_[a] = activeNotes_[--numActiveNotes_];
        }
    }
}

void Renderer::scheduleTicks(const RenderSnapshot& snap, int64_t position, int length, int offset) noexcept {
    const double tickLength = snap.samplesPerBeat() * 4.0 / snap.timeSigDen;
    if (tickLength <= 0.0) return;
    for (int64_t k = static_cast<int64_t>(std::ceil(position / tickLength - 1e-9));; ++k) {
        const int64_t t = std::llround(k * tickLength);
        if (t < position) continue;
        if (t >= position + length) break;
        if (numPendingTicks_ == kMaxPendingTicks) break;
        // Heard when the tracks' audio for this position is: after the compensation delay
        // and the master's devices.
        const int64_t time = outputTime_ + offset + (t - position) + snap.outputLatency();
        pendingTicks_[(pendingTickStart_ + numPendingTicks_++) % kMaxPendingTicks] = {time, k % snap.timeSigNum == 0};
    }
}

void Renderer::renderTicks(int frames) noexcept {
    numTicks_ = 0;
    while (numPendingTicks_ > 0 && numTicks_ < kMaxTicks) {
        const PendingTick& tick = pendingTicks_[pendingTickStart_];
        if (tick.time >= outputTime_ + frames) break;
        ticks_[numTicks_++] = {static_cast<int>(std::max<int64_t>(0, tick.time - outputTime_)), tick.accent};
        pendingTickStart_ = (pendingTickStart_ + 1) % kMaxPendingTicks;
        --numPendingTicks_;
    }
}

void Renderer::mixPreview(SharedState& shared, int frames) noexcept {
    // seq_cst pairs with Engine::preview()'s serial bump + epoch read.
    const uint32_t serial = shared.previewSerial.load(std::memory_order_seq_cst);
    if (serial != previewSerial_) {
        previewSerial_ = serial;
        previewSource_ = shared.previewSource.load(std::memory_order_seq_cst);
        previewPosition_ = 0;
    }
    if (!previewSource_) return;

    const AudioSource& source = *previewSource_;
    const float gain = shared.previewGain.load(std::memory_order_relaxed);
    const float* srcL = source.channelData(0);
    const float* srcR = source.channels() > 1 ? source.channelData(1) : srcL;
    const int64_t n = std::min<int64_t>(frames, source.frames() - previewPosition_);
    for (int64_t i = 0; i < n; ++i) {
        masterLeft_[i] += srcL[previewPosition_ + i] * gain;
        masterRight_[i] += srcR[previewPosition_ + i] * gain;
    }
    previewPosition_ += n;
    if (previewPosition_ >= source.frames()) {
        previewSource_ = nullptr;
        if (shared.previewSerial.load(std::memory_order_acquire) == previewSerial_) {
            shared.previewActive.store(false, std::memory_order_relaxed);
        }
    }
}

}  // namespace gil
