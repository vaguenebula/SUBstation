#include "Renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include "Ump.h"

namespace sub {

namespace {

// Calls f(clip, from, to) for each clip that plays in [segStart, segEnd), in
// order, with the part of the segment it covers.
template <typename F>
void forEachClip(const TrackRender& track, int64_t segStart, int64_t segEnd, F&& f) {
    // Clips are sorted by start; no clip starting before this bound can reach the segment.
    const int64_t earliest = segStart - track.maxClipLength;
    auto it = std::lower_bound(track.clips.begin(), track.clips.end(), earliest,
                               [](const ClipRender& clip, int64_t value) { return clip.start < value; });
    for (; it != track.clips.end() && it->start < segEnd; ++it) {
        const int64_t from = std::max(segStart, it->start);
        const int64_t to = std::min(segEnd, it->start + it->length);
        if (from < to) f(*it, from, to);
    }
}

}  // namespace

void Renderer::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    for (auto* buffer : {&masterLeft_, &masterRight_, &silence_, &captureLeft_, &captureRight_}) {
        buffer->assign(kMaxBlock, 0.f);
    }
    setScheduler(scheduler_);
    recordScratch_.assign(2 * kMaxBlock, 0.f);
    countIn_ = countInTotal_ = 0;
    metronome_.prepare(sampleRate);
    previewSource_ = nullptr;
    // Processors are prepared (silenced) along with the renderer.
    activeNotes_.assign(kMaxActiveNotes, {});
    numActiveNotes_ = 0;
    previewNotes_.assign(kMaxPreviewNotes, {});
    numPreviewNotes_ = 0;
    heldPreviews_.assign(kMaxPreviewNotes, {});
    numHeldPreviews_ = 0;
    pendingInput_.assign(kMaxPendingInput, {});
    numPendingInput_ = 0;
    inputEvents_.assign(kMaxInputEvents, {});
    numInputEvents_ = 0;
    liveNotes_.assign(kMaxLiveNotes, {});
    numLiveNotes_ = 0;
    releaseLiveNotes_ = false;
    wasPlaying_ = false;
    expectedPosition_ = -1;
    pendingTickStart_ = 0;
    numPendingTicks_ = 0;
    outputTime_ = 0;
}

void Renderer::setScheduler(Scheduler* scheduler) {
    scheduler_ = scheduler;
    scratch_.resize(static_cast<size_t>(scheduler ? scheduler->threads() : 1));
    for (WorkerScratch& scratch : scratch_) {
        for (auto* buffer : {&scratch.warpLeft, &scratch.warpRight, &scratch.autoGain, &scratch.autoPanLeft,
                             &scratch.autoPanRight, &scratch.audible, &scratch.edgeGain, &scratch.activator,
                             &scratch.keyLeft, &scratch.keyRight}) {
            buffer->assign(kMaxBlock, 0.f);
        }
        scratch.switchStates.assign(kMaxBlock + kMaxSwitchFade, 0.f);
        for (WorkerScratch::Switch& device : scratch.switches) {
            for (auto* buffer : {&device.gain, &device.dryLeft, &device.dryRight}) buffer->assign(kMaxBlock, 0.f);
        }
        for (WorkerScratch::Rack& rack : scratch.racks) {
            for (auto* buffer : {&rack.sumLeft, &rack.sumRight, &rack.chainLeft, &rack.chainRight}) {
                buffer->assign(kMaxBlock, 0.f);
            }
        }
    }
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
        for (int i = 0; i < numActiveNotes_; ++i) {
            activeNotes_[i].end = rescale(activeNotes_[i].end);
            activeNotes_[i].start = rescale(activeNotes_[i].start);
        }
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

void Renderer::discardPreviewNotes(SharedState& shared) noexcept { shared.previewNotes.clear(); }

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
    drainMidiInput(shared);
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
        deviceTime_ = io.sampleTime + done;
        gatherMidiInput(n);
        renderChunk(snap, n,
                    {true, snap.loopEnabled, shared.metronome.load(std::memory_order_relaxed)});
        numPreviewNotes_ = 0;  // played in the first chunk
        numInputEvents_ = 0;
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
        interleave(masterLeft_.data(), masterRight_.data(), n, outStereo + done * 2);
        done += n;
    }
}

void Renderer::renderTrackOffline(const RenderSnapshot& snap, int track, float* outStereo,
                                  int64_t frames) noexcept {
    syncTempo(snap);
    captureTrack_ = track;
    ignoreSolo_ = true;
    int64_t done = 0;
    while (done < frames) {
        const int n = static_cast<int>(std::min<int64_t>(kMaxBlock, frames - done));
        std::fill_n(captureLeft_.data(), n, 0.f);  // (a track not in the snapshot: silence)
        std::fill_n(captureRight_.data(), n, 0.f);
        renderChunk(snap, n, {false, false, false});
        interleave(captureLeft_.data(), captureRight_.data(), n, outStereo + done * 2);
        done += n;
    }
    captureTrack_ = -1;
    ignoreSolo_ = false;
}

void Renderer::renderChunk(const RenderSnapshot& snap, int frames, ChunkFlags flags) noexcept {
    const int64_t chunkStart = position_;
    const WarpVoiceSet& voices = voiceOverride_ ? *voiceOverride_ : snap.warpVoices;
    ++blockCounter_;

    // The prologue (serial): everything the tracks share, worked out before the
    // graph renders them (Renderer.h).
    // 1. Split the chunk into contiguous timeline segments (loop wrap-around).
    numSegments_ = 0;
    numTicks_ = 0;
    chunkFrom_ = wasPlaying_ ? expectedPosition_ : -1;
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
    ProcessContext& context = chunkContext_;
    context = {};
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

    recordSegments_ = 0;
    if (recording_ && numSegments_ > 0) recordInput();
    if (wasPlaying_ && !playing_) releaseLiveNotes_ = true;
    wasPlaying_ = playing_;

    // 2. What each track hears in this chunk, worked out in order: everything
    // here touches state the tracks share (sounding notes, MIDI input, the
    // recording, stretchers).
    for (const TrackRender& track : snap.tracks) {
        TrackBuffers& buffers = *track.buffers;
        buffers.monitored = isMonitored(track, flags);
        buffers.recorded = flags.live && isRecorded(track.id);
        const bool hearsMidi = hearsMidiInput(track, flags);
        MidiRecordingTake* take = recording_ ? midiTake(track.id) : nullptr;
        buffers.numEvents = 0;
        if (!track.notes.empty() || numActiveNotes_ > 0 || numPreviewNotes_ > 0 || numInputEvents_ > 0 ||
            numLiveNotes_ > 0) {
            const bool clipNotes = !buffers.recorded && !(hearsMidi && track.monitor == MonitorMode::In);
            buildNoteEvents(track, buffers, hearsMidi, clipNotes, take);
        }
        assignVoices(track, voices, buffers);
    }
    // Devices taking another track's notes get a copy of them, before the graph
    // (where that track's own devices rebase theirs to their stretches).
    for (const MidiFeedRender& feed : snap.midiFeeds) {
        MidiFeed& into = *feed.feed;
        into.numEvents = 0;
        if (feed.source < 0) continue;
        const TrackBuffers& from = *snap.tracks[static_cast<size_t>(feed.source)].buffers;
        into.numEvents = std::min(from.numEvents, static_cast<int>(into.events.size()));
        std::copy_n(from.events.begin(), into.numEvents, into.events.begin());
    }
    forgetNotesOfRemovedTracks(snap);
    releaseLiveNotes_ = false;
    workOutSolo(snap);  // and mute: which tracks and edges are heard (after monitoring: input edges count while monitored)

    // 3. The tracks, each after what feeds it (on any thread): its inputs, clips
    // and notes -> its strip -> its buffer, which the bus it goes into reads.
    chunkSnap_ = &snap;
    chunkFrames_ = frames;
    chunkFlags_ = flags;
    if (snap.graph && scheduler_) {
        const bool parallel = static_cast<int64_t>(snap.parallelWork) * frames >= kMinParallelWork &&
                              snap.parallelWork >= 2;
        if (parallel) {  // the heaviest paths first (by what the tracks took lately)
            TaskGraph& graph = *snap.graph;
            const bool byCost = costOrdering();
            for (int t = 0; t < graph.size(); ++t) {
                graph.setCost(t, byCost ? snap.tracks[static_cast<size_t>(t)].buffers->cost.load(
                                              std::memory_order_relaxed)
                                        : 0.f);
            }
            graph.orderRoots();
        }
        scheduler_->run(*snap.graph, &Renderer::renderNode, this, parallel);
    } else {
        for (int t = 0; t < static_cast<int>(snap.tracks.size()); ++t) renderTrack(snap, t, scratch_[0]);
    }

    // 4. The master strip: what goes into it, summed in order (it never mutes,
    // and nothing is later than it: it has no edges of its own).
    float* masterL = masterLeft_.data();
    float* masterR = masterRight_.data();
    std::fill_n(masterL, frames, 0.f);
    std::fill_n(masterR, frames, 0.f);
    for (const int e : snap.masterInputs) sumEdge(snap, snap.edges[static_cast<size_t>(e)], masterL, masterR, frames, scratch_[0]);
    if (snap.master.params) {
        processStrip(snap, snap.master, context, nullptr, 0, masterL, masterR, frames, true, flags, scratch_[0]);
    }
    // What the tracks being recorded take from other tracks' outputs, or the
    // master's (resampling), now that they are rendered.
    if (recordSegments_ > 0) recordRendered(snap);

    // 5. Metronome, after the master fader (a click may still be ringing out).
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

void Renderer::renderNode(void* self, int node, int worker) noexcept {
    auto& renderer = *static_cast<Renderer*>(self);
    renderer.renderTrack(*renderer.chunkSnap_, node, renderer.scratch_[static_cast<size_t>(worker)]);
}

void Renderer::renderTrack(const RenderSnapshot& snap, int t, WorkerScratch& scratch) noexcept {
    // Timed from here to the end: nothing in between waits (its inputs are done).
    const auto started = std::chrono::steady_clock::now();
    const TrackRender& track = snap.tracks[static_cast<size_t>(t)];
    TrackBuffers& buffers = *track.buffers;
    const int frames = chunkFrames_;
    float* left = buffers.left.data();
    float* right = buffers.right.data();
    std::fill_n(left, frames, 0.f);
    std::fill_n(right, frames, 0.f);
    // A bus (a group, a return): what goes into it, in a fixed order (it is done:
    // the scheduler runs it after its sources), so the sum is the same whichever
    // finished first.
    // (Frozen, it plays its frozen audio instead: what went into it is in that.)
    for (const int e : track.incoming) {
        const EdgeRender& edge = snap.edges[static_cast<size_t>(e)];
        if (edge.sums() && !track.frozen) sumEdge(snap, edge, left, right, frames, scratch);
    }
    if (buffers.monitored) {  // its input: the device's, or another track's output (rendered: it fed this one)
        if (track.input.source == InputEdge::Source::Track) {
            sumEdge(snap, snap.edges[static_cast<size_t>(track.input.edge)], left, right, frames, scratch);
        } else if (track.input.fromDevice()) {
            readInput(track.input, left, right, frames);
        }
        // And what other tracks' outputs bring into it (Track In), in a fixed order.
        if (track.trackIn) {
            for (const int e : track.incoming) {
                const EdgeRender& edge = snap.edges[static_cast<size_t>(e)];
                if (edge.kind == EdgeRender::Kind::TrackIn) sumEdge(snap, edge, left, right, frames, scratch);
            }
        }
    } else if (!buffers.recorded) {  // (being recorded, unmonitored: silence; the take replaces its clips)
        int nextVoice = 0;
        for (int s = 0; s < numSegments_; ++s) {
            renderClips(track, segments_[s], buffers, nextVoice, scratch, left, right);
        }
    }
    ProcessContext context = chunkContext_;  // its own: the inserts move it along the chunk's stretches
    processInserts(snap, track, context, buffers.events.data(), buffers.numEvents, left, right, frames,
                   buffers.monitored, scratch);
    if (t == captureTrack_) {  // what freezing it keeps
        std::copy_n(left, frames, captureLeft_.data());
        std::copy_n(right, frames, captureRight_.data());
    }
    // Pre-fader taps take the signal here, into their own buffers (taps after a
    // device took theirs as it processed).
    bool preFaderSend = false;
    for (const int e : track.outgoing) {
        const EdgeRender& edge = snap.edges[static_cast<size_t>(e)];
        if (edge.tap != EdgeRender::Tap::PreFader) continue;
        std::copy_n(left, frames, edge.state->left.data());
        std::copy_n(right, frames, edge.state->right.data());
        preFaderSend = preFaderSend || edge.kind != EdgeRender::Kind::Sidechain;
    }
    float* audible = preFaderSend ? scratch.audible.data() : nullptr;
    applyFader(snap, *track.params, track.volume, track.pan, &track.on, buffers.audible, left, right, frames,
               chunkFlags_.live, scratch, audible);
    // The edges that need a signal of their own: a pre-fader send is muted with
    // the track (a sidechain isn't: it isn't heard); a post-fader edge delayed
    // for its destination alone is a copy. Then each is delayed to line up where it goes.
    for (const int e : track.outgoing) {
        const EdgeRender& edge = snap.edges[static_cast<size_t>(e)];
        if (!edge.ownSignal()) continue;
        float* edgeL = edge.state->left.data();
        float* edgeR = edge.state->right.data();
        if (edge.tap == EdgeRender::Tap::PreFader) {
            if (edge.kind != EdgeRender::Kind::Sidechain) {
                for (int i = 0; i < frames; ++i) {
                    edgeL[i] *= audible[i];
                    edgeR[i] *= audible[i];
                }
            }
        } else if (edge.tap == EdgeRender::Tap::PostFader) {
            std::copy_n(left, frames, edgeL);
            std::copy_n(right, frames, edgeR);
        }
        if (DelayLine* delay = edgeDelayLine(edge, e)) {
            delay->process(edgeL, edgeR, frames, compensationFor(edge, buffers.monitored));
        }
    }

    // Smoothed over roughly the last ten chunks: a synth costs more while it
    // plays a chord, a plug-in's first blocks after loading more than later ones.
    if (frames > 0) {
        const auto elapsed = std::chrono::duration<float, std::nano>(std::chrono::steady_clock::now() - started);
        const float perFrame = elapsed.count() / static_cast<float>(frames);
        const float last = buffers.cost.load(std::memory_order_relaxed);
        buffers.cost.store(last > 0.f ? last + kCostSmoothing * (perFrame - last) : perFrame,
                           std::memory_order_relaxed);
    }
}

void Renderer::processStrip(const RenderSnapshot& snap, const StripRender& strip, ProcessContext& context,
                            ProcessEvent* events, int numEvents, float* left, float* right, int frames, bool audible,
                            ChunkFlags flags, WorkerScratch& scratch, float* audibleOut) noexcept {
    processInserts(snap, strip, context, events, numEvents, left, right, frames, false, scratch);
    applyFader(snap, *strip.params, strip.volume, strip.pan, &strip.on, audible, left, right, frames, flags.live, scratch,
               audibleOut);
}

int Renderer::compensationFor(const EdgeRender& edge, bool monitored) noexcept {
    // Edges are delayed to line up with the latest one going into the same place
    // (edge.compensation, worked out with the snapshot). The exception: a
    // monitored track plays its live input as soon as it can, so a player hears
    // themselves with only the latency of the track's own devices (monitoring
    // latency), not every other track's.
    return monitored ? 0 : edge.compensation;
}

namespace {

// An offline render's own delay line at `index` of `lines` (null if there is none).
DelayLine* lineAt(const std::vector<std::shared_ptr<DelayLine>>* lines, int index) noexcept {
    if (!lines || index < 0 || static_cast<size_t>(index) >= lines->size()) return nullptr;
    return (*lines)[static_cast<size_t>(index)].get();
}

}  // namespace

DelayLine* Renderer::edgeDelayLine(const EdgeRender& edge, int e) const noexcept {
    return delayOverride_ ? lineAt(delayOverride_, e) : edge.delay.get();
}

DelayLine* Renderer::deviceDelayLine(const EdgeRender& edge, int e) const noexcept {
    return delayOverride_ ? lineAt(deviceDelayOverride_, e) : edge.deviceDelayLine.get();
}

DelayLine* Renderer::chainDelayLine(const ChainRender& chain) const noexcept {
    return delayOverride_ ? lineAt(chainDelayOverride_, chain.delayIndex) : chain.delay.get();
}

void Renderer::edgeSignal(const RenderSnapshot& snap, const EdgeRender& edge, const float*& left,
                          const float*& right) noexcept {
    if (edge.ownSignal()) {
        left = edge.state->left.data();
        right = edge.state->right.data();
        return;
    }
    const TrackBuffers& source = *snap.tracks[static_cast<size_t>(edge.from)].buffers;
    left = source.left.data();
    right = source.right.data();
}

void Renderer::workOutSolo(const RenderSnapshot& snap) noexcept {
    // Each solo button is read once, so the chunk sees one consistent state. An
    // input edge (a Track In edge too) only counts while its track hears it
    // (monitored): otherwise its track plays its clips, and the edge only feeds
    // a recording. A sidechain
    // isn't heard: solo goes up it (what keys a strip that is heard keeps keying
    // it: one soloed, fed by a solo or feeding one; the master's devices are
    // always heard) but not down it (soloing what keys a strip doesn't make that
    // strip heard).
    const auto passes = [&snap](const EdgeRender& edge) {  // its signal goes on into its destination's
        return edge.sums() || ((edge.kind == EdgeRender::Kind::Input || edge.kind == EdgeRender::Kind::TrackIn) &&
                               snap.tracks[static_cast<size_t>(edge.to)].buffers->monitored);
    };
    const auto carries = [&passes](const EdgeRender& edge) {
        return passes(edge) || edge.kind == EdgeRender::Kind::Sidechain;
    };
    // Where it goes is soloed or feeds a solo; for a sidechain, also fed by one
    // (soloDown is worked out before soloUp: sources come first).
    const auto upstream = [&snap](const EdgeRender& edge) {
        if (edge.to < 0) return edge.kind == EdgeRender::Kind::Sidechain;
        const TrackBuffers& to = *snap.tracks[static_cast<size_t>(edge.to)].buffers;
        return to.soloUp || (edge.kind == EdgeRender::Kind::Sidechain && to.soloDown);
    };
    bool anySolo = false;
    for (const TrackRender& track : snap.tracks) {
        TrackBuffers& buffers = *track.buffers;
        buffers.soloed = !ignoreSolo_ && track.params->solo.load(std::memory_order_relaxed);
        anySolo = anySolo || buffers.soloed;
    }
    if (anySolo) {
        // Downstream of a solo: sources come first in the snapshot. Upstream: backwards.
        for (const TrackRender& track : snap.tracks) {
            bool down = track.buffers->soloed;
            for (const int e : track.incoming) {
                const EdgeRender& edge = snap.edges[static_cast<size_t>(e)];
                down = down || (passes(edge) && snap.tracks[static_cast<size_t>(edge.from)].buffers->soloDown);
            }
            track.buffers->soloDown = down;
        }
        for (auto it = snap.tracks.rbegin(); it != snap.tracks.rend(); ++it) {
            bool up = it->buffers->soloed;
            for (const int e : it->outgoing) {
                const EdgeRender& edge = snap.edges[static_cast<size_t>(e)];
                up = up || (carries(edge) && upstream(edge));
            }
            it->buffers->soloUp = up;
        }
    }
    for (const EdgeRender& edge : snap.edges) {
        edge.state->live = !anySolo || snap.tracks[static_cast<size_t>(edge.from)].buffers->soloDown || upstream(edge);
    }
    for (const TrackRender& track : snap.tracks) {
        const bool heard = std::any_of(track.outgoing.begin(), track.outgoing.end(), [&](int e) {
            const EdgeRender& edge = snap.edges[static_cast<size_t>(e)];
            return edge.state->live && carries(edge);
        });
        // (An automated switch stands in for its mute: applyFader.)
        const bool muted = track.on.empty() && track.params->mute.load(std::memory_order_relaxed);
        track.buffers->audible = !muted && heard;
    }
}

void Renderer::sumEdge(const RenderSnapshot& snap, const EdgeRender& edge, float* left, float* right, int frames,
                       WorkerScratch& scratch) noexcept {
    EdgeState& state = *edge.state;
    const float* srcL;
    const float* srcR;
    edgeSignal(snap, edge, srcL, srcR);
    const float on = state.live ? 1.f : 0.f;
    const float gain = state.gain.load(std::memory_order_relaxed);
    const float* gains = nullptr;
    if (!edge.level.empty()) {  // an automated send level
        float* lane = scratch.edgeGain.data();
        fillLane(edge.level, frames, lane);
        for (int i = 0; i < frames; ++i) lane[i] = automationVolumeGain(lane[i]);
        gains = lane;
    }
    const auto add = [&](float g) {
        if (g == 1.f) {  // (an output, heard: just the sum)
            addStereo(left, right, srcL, srcR, frames);
        } else if (g != 0.f) {
            addStereo(left, right, srcL, srcR, frames, g);
        }
    };
    if (!chunkFlags_.live) {  // as applyFader: offline renders hold the engine lock, nothing changes meanwhile
        if (!gains) {
            add(on * gain);
            return;
        }
        for (int i = 0; i < frames; ++i) {
            const float g = on * gains[i];
            left[i] += srcL[i] * g;
            right[i] += srcR[i] * g;
        }
        return;
    }
    // Smoothing state lives with the edge, so it survives snapshot swaps.
    if (state.smoothingSampleRate != snap.sampleRate) {
        state.level.reset(snap.sampleRate, 0.02);
        state.audible.reset(snap.sampleRate, 0.02);
        state.level.snapTo(gain);
        state.audible.snapTo(on);
        state.smoothingSampleRate = snap.sampleRate;
    }
    state.level.setTarget(gain);
    state.audible.setTarget(on);
    if (!gains && !state.level.isSmoothing() && !state.audible.isSmoothing()) {
        add(state.audible.current() * state.level.current());
        return;
    }
    for (int i = 0; i < frames; ++i) {
        const float a = state.audible.next();
        float g = state.level.next();
        if (gains) g = gains[i];
        left[i] += srcL[i] * a * g;
        right[i] += srcR[i] * a * g;
    }
    // Where automation stops (or is overridden), the manual level takes over from its last value.
    if (gains && frames > 0) state.level.snapTo(gains[frames - 1]);
}

bool Renderer::isMonitored(const TrackRender& track, ChunkFlags flags) const noexcept {
    // Offline renders play the arrangement, but for what a track monitoring In
    // hears from other tracks (its input from one, what comes in as its Track
    // In): the device's input isn't there. The master's output can't be heard on
    // a track: the track goes into it.
    if (!track.input.monitorable() && !track.trackIn) return false;
    if (!flags.live) {
        return track.monitor == MonitorMode::In && (track.input.source == InputEdge::Source::Track || track.trackIn);
    }
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
    addStereo(left, right, inputChannel(input.left, 0), inputChannel(input.right, 0), frames);
}

void Renderer::recordInput() noexcept {
    RecordingSession& session = *recording_;
    for (int s = 0; s < numSegments_; ++s) {
        if (session.interrupted()) return;
        const Segment& segment = segments_[s];
        bool jumped = false;
        const auto begin = [&](std::atomic<int64_t>& start) {
            if (start.load(std::memory_order_relaxed) == RecordingTake::kNotStarted) {
                start.store(segment.position, std::memory_order_release);
            } else if (segment.jump) {
                jumped = true;
            }
        };
        for (const auto& take : session.takes()) begin(take->start);
        for (const auto& take : session.midiTakes()) begin(take->start);
        if (jumped) {  // located, or stopped and started again: the takes end
            session.interrupt();
            return;
        }
        for (const auto& take : session.takes()) {
            if (take->source != RecordingTake::Source::Device) continue;  // (once rendered: recordRendered())
            take->push(inputChannel(take->inputs[0], segment.offset), inputChannel(take->inputs[1], segment.offset),
                       segment.length, recordScratch_.data());
        }
        for (const auto& take : session.midiTakes()) {
            take->frames.fetch_add(segment.length, std::memory_order_release);
        }
        recordSegments_ = s + 1;
    }
}

void Renderer::recordRendered(const RenderSnapshot& snap) noexcept {
    // The same stretches of the chunk recordInput() took from the device.
    for (const auto& take : recording_->takes()) {
        if (take->source == RecordingTake::Source::Device) continue;
        const float* left = silence_.data();  // a source gone meanwhile: silence, so the take stays in time
        const float* right = silence_.data();
        if (take->source == RecordingTake::Source::Master) {
            left = masterLeft_.data();
            right = masterRight_.data();
        } else {
            // Its source's signal where its input edge taps it: after its fader
            // (its buffer, before any edge's delay), before it, or after a device.
            if (const TrackRender* track = snap.findTrack(take->trackId)) {
                const int e = track->input.source == InputEdge::Source::Track ? track->input.edge : -1;
                if (e >= 0 && snap.tracks[static_cast<size_t>(snap.edges[static_cast<size_t>(e)].from)].id == take->sourceTrackId) {
                    edgeSignal(snap, snap.edges[static_cast<size_t>(e)], left, right);
                }
            }
        }
        for (int s = 0; s < recordSegments_; ++s) {
            const Segment& segment = segments_[s];
            take->push(left + segment.offset, right + segment.offset, segment.length, recordScratch_.data());
        }
    }
}

// ---------------------------------------------------------------------------
// MIDI input

void Renderer::drainMidiInput(SharedState& shared) noexcept {
    while (numPendingInput_ < kMaxPendingInput && shared.midiInput.pop(pendingInput_[numPendingInput_])) {
        ++numPendingInput_;
    }
}

void Renderer::gatherMidiInput(int frames) noexcept {
    // The messages due in this chunk, in the order they came. Those more than a
    // second off weren't stamped against this run of the clock (it was reset, or
    // live output was suspended meanwhile): they play now, but a note-on that
    // late is dropped rather than played out of time (its note-off is harmless).
    numInputEvents_ = 0;
    const int64_t end = deviceTime_ + frames;
    const auto second = static_cast<int64_t>(sampleRate_);
    int kept = 0;
    bool deferring = false;  // one was put off: so are those that came after it
    for (int i = 0; i < numPendingInput_; ++i) {
        const MidiInputEvent& event = pendingInput_[i];
        const uint8_t type = event.status & 0xF0;
        const bool stale = event.time < deviceTime_ - second || event.time > end + second;
        if (stale && type == 0x90 && event.data2 > 0) continue;
        bool due = !deferring && (event.time < end || stale);
        if (due && numInputEvents_ == kMaxInputEvents) due = false;
        int offset = stale ? 0 : static_cast<int>(std::clamp<int64_t>(event.time - deviceTime_, 0, frames - 1));
        if (due && (type == 0x80 || (type == 0x90 && event.data2 == 0))) {
            // A note-off never plays before its note-on (they may be stamped out
            // of order, or both late): just after it, or in the next chunk.
            for (int j = numInputEvents_ - 1; j >= 0; --j) {
                const InputEvent& on = inputEvents_[j];
                if (on.port != event.port || (on.status & 0x0F) != (event.status & 0x0F) || on.data1 != event.data1 ||
                    (on.status & 0xF0) != 0x90 || on.data2 == 0) {
                    continue;
                }
                if (on.offset >= offset) {
                    if (on.offset + 1 < frames) {
                        offset = on.offset + 1;
                    } else {
                        due = false;
                    }
                }
                break;
            }
        }
        if (due) {
            inputEvents_[numInputEvents_++] = {offset,      event.port,
                                               event.status, event.data1,
                                               event.data2, static_cast<uint8_t>(event.kind),
                                               event.value};
        } else {
            deferring = deferring || event.time < end || stale;
            pendingInput_[kept++] = event;
        }
    }
    numPendingInput_ = kept;
}

bool Renderer::hearsMidiInput(const TrackRender& track, ChunkFlags flags) const noexcept {
    if (!flags.live || !track.midiInput.enabled) return false;  // offline renders play the arrangement
    switch (track.monitor) {
        case MonitorMode::In: return true;
        case MonitorMode::Auto: return track.armed;  // clips' notes go on playing alongside
        case MonitorMode::Off: break;
    }
    return false;
}

bool Renderer::isRecorded(uint32_t trackId) const noexcept {
    // A take of it records (until the playhead jumps: the takes end there).
    if (!recording_ || recording_->interrupted()) return false;
    for (const auto& take : recording_->takes()) {
        if (take->trackId == trackId) return true;
    }
    return midiTake(trackId) != nullptr;
}

MidiRecordingTake* Renderer::midiTake(uint32_t trackId) const noexcept {
    if (!recording_ || recording_->interrupted()) return nullptr;
    for (const auto& take : recording_->midiTakes()) {
        if (take->trackId == trackId) return take.get();
    }
    return nullptr;
}

int Renderer::findLiveNote(uint32_t trackId, uint16_t port, uint8_t channel, uint8_t key) const noexcept {
    for (int i = 0; i < numLiveNotes_; ++i) {
        const LiveNote& note = liveNotes_[i];
        if (note.trackId == trackId && note.port == port && note.channel == channel && note.key == key) return i;
    }
    return -1;
}

void Renderer::recordMidi(MidiRecordingTake* take, int offset, uint8_t channel, uint8_t key, uint8_t velocity,
                          bool bend, float semitones) noexcept {
    // Where the playhead was at that offset; nothing is recorded while it stands (a count-in).
    if (!take || !playing_) return;
    for (int s = 0; s < numSegments_; ++s) {
        const Segment& segment = segments_[s];
        if (offset >= segment.offset && offset < segment.offset + segment.length) {
            take->push({segment.position + (offset - segment.offset), channel, key, velocity, bend, semitones});
            return;
        }
    }
}

void Renderer::routeMidiInput(const TrackRender& track, bool hears, MidiRecordingTake* take,
                              TrackBuffers& out) noexcept {
    // Live notes the track no longer hears (its input or monitoring changed, or
    // the transport stopped) are released at the chunk's start.
    for (int i = 0; i < numLiveNotes_;) {
        const LiveNote& note = liveNotes_[i];
        if (note.trackId != track.id ||
            (hears && !releaseLiveNotes_ && track.midiInput.accepts(note.port, note.channel))) {
            ++i;
            continue;
        }
        ProcessEvent off = ProcessEvent::noteOff(0, note.key);
        off.data[2] = note.channel;
        off.noteId = note.noteId;
        if (!out.pushEvent(off)) return;  // next chunk
        recordMidi(take, 0, note.channel, note.key, 0);
        liveNotes_[i] = liveNotes_[--numLiveNotes_];
    }
    if (!hears && !take) return;
    for (int e = 0; e < numInputEvents_; ++e) {
        const InputEvent& in = inputEvents_[e];
        if (!track.midiInput.accepts(in.port, in.status)) continue;
        const uint8_t type = in.status & 0xF0;
        const uint8_t channel = in.status & 0x0F;
        if (in.kind == static_cast<uint8_t>(MidiInputEvent::Kind::NoteBend)) {  // MIDI 2.0: one note bends
            const auto semitones = static_cast<float>(ump::bendSemitones(in.value));
            recordMidi(take, in.offset, channel, in.data1, 0, true, semitones);
            if (!hears) continue;
            const int held = findLiveNote(track.id, in.port, channel, in.data1);
            if (held < 0) continue;  // not one this track holds
            ProcessEvent bend = ProcessEvent::noteBend(in.offset, in.data1, liveNotes_[held].noteId, semitones);
            bend.data[2] = channel;
            if (!out.pushEvent(bend)) break;
        } else if (type == 0x90 && in.data2 > 0) {
            recordMidi(take, in.offset, channel, in.data1, in.data2);
            if (!hears) continue;
            if (const int held = findLiveNote(track.id, in.port, channel, in.data1); held >= 0) {
                ProcessEvent off = ProcessEvent::noteOff(in.offset, in.data1);  // played again: it starts over
                off.data[2] = channel;
                off.noteId = liveNotes_[held].noteId;
                if (!out.pushEvent(off)) break;
                liveNotes_[held] = liveNotes_[--numLiveNotes_];
            }
            if (numLiveNotes_ == kMaxLiveNotes) continue;  // it couldn't be released: not played
            ProcessEvent on = ProcessEvent::noteOn(in.offset, in.data1, in.data2);
            on.data[2] = channel;
            on.noteId = newNoteId();
            if (!out.pushEvent(on)) break;
            liveNotes_[numLiveNotes_++] = {track.id, in.port, channel, in.data1, on.noteId};
        } else if (type == 0x80 || type == 0x90) {
            recordMidi(take, in.offset, channel, in.data1, 0);
            const int held = findLiveNote(track.id, in.port, channel, in.data1);
            if (held < 0) continue;  // not one this track started
            ProcessEvent off = ProcessEvent::noteOff(in.offset, in.data1);
            off.data[2] = channel;
            off.noteId = liveNotes_[held].noteId;
            if (!out.pushEvent(off)) break;
            liveNotes_[held] = liveNotes_[--numLiveNotes_];
        } else if (hears && type >= 0xA0 && type <= 0xE0) {  // pressure, controllers, programs, pitch bend
            ProcessEvent raw;
            raw.type = ProcessEvent::Type::Midi;
            raw.sampleOffset = in.offset;
            raw.data[0] = in.status;
            raw.data[1] = in.data1;
            raw.data[2] = in.data2;
            if (!out.pushEvent(raw)) break;
        }
    }
}

void Renderer::scheduleCountIn(const RenderSnapshot& snap, int length) noexcept {
    // A tick every beat (of the time signature) from the count-in's start, the
    // first of each bar accented; it ends where the playhead starts.
    scheduleTicks(snap, countInTotal_ - countIn_, length, 0, countInTotal_);
}

namespace {

// A tap after a device takes the signal there, into its edge's own buffer.
void tapInto(const RenderSnapshot& snap, int e, const float* left, const float* right, int frames) noexcept {
    EdgeState& state = *snap.edges[static_cast<size_t>(e)].state;
    std::copy_n(left, frames, state.left.data());
    std::copy_n(right, frames, state.right.data());
}

}  // namespace

bool Renderer::takeResets(const StripRender& chain) noexcept {
    bool any = false;
    for (size_t i = 0; i < chain.inserts.size(); ++i) {
        Processor& insert = *chain.inserts[i];
        if (!insert.isEnabled()) continue;
        any = true;
        if (!insert.takeResetRequest()) continue;
        resetInsert(insert, i < chain.racks.size() ? chain.racks[i].get() : nullptr);
    }
    return any;
}

void Renderer::resetInsert(Processor& insert, const RackRender* rack) noexcept {
    insert.reset();
    // A rack's devices reset as they run next (and so, in turn, do those of racks in it).
    if (!rack) return;
    for (const ChainRender& inner : rack->chains) {
        for (const auto& device : inner.inserts) device->requestReset();
    }
}

void Renderer::tapSkippedRack(const RenderSnapshot& snap, const RackRender& rack, const float* left,
                              const float* right, int frames) noexcept {
    for (const ChainRender& chain : rack.chains) {
        for (const int e : chain.deviceTaps) tapInto(snap, e, left, right, frames);
        for (const auto& inner : chain.racks) {
            if (inner) tapSkippedRack(snap, *inner, left, right, frames);
        }
    }
}

void Renderer::processInserts(const RenderSnapshot& snap, const StripRender& strip, ProcessContext& context,
                              ProcessEvent* events, int numEvents, float* left, float* right, int frames,
                              bool monitored, WorkerScratch& scratch) noexcept {
    if (!takeResets(strip)) {  // nothing changes the signal along the chain
        for (const int e : strip.deviceTaps) tapInto(snap, e, left, right, frames);
        return;
    }

    // One call per continuous stretch of the timeline, with the events that fall in it.
    static_assert(kMaxSegments + 1 <= std::tuple_size_v<decltype(Slices::slice)>);
    Slices slices;
    const bool split = playing_ && numSegments_ > 0;
    // A count-in that ends in this chunk: the playhead stood still until the first segment.
    const int lead = split && segments_[0].offset > 0 ? 1 : 0;
    slices.count = split ? numSegments_ + lead : 1;
    int next = 0;
    for (int s = 0; s < slices.count; ++s) {
        const int segment = s - lead;
        const bool moving = split && segment >= 0;
        const int offset = moving ? segments_[segment].offset : 0;
        const int length = moving ? segments_[segment].length : split ? segments_[0].offset : frames;
        // Stopped (or counting in): it stays put.
        const int64_t position = moving ? segments_[segment].position : split ? segments_[0].position : position_;
        const int first = next;
        while (next < numEvents && (s == slices.count - 1 || events[next].sampleOffset < offset + length)) {
            events[next].sampleOffset -= offset;
            ++next;
        }
        slices.slice[static_cast<size_t>(s)] = {offset, length, position, moving, first, next};
    }
    processChain(snap, strip, context, slices, events, left, right, frames, monitored, 0, scratch);
}

void Renderer::processChain(const RenderSnapshot& snap, const StripRender& chain, ProcessContext& context,
                            const Slices& slices, ProcessEvent* events, float* left, float* right, int frames,
                            bool monitored, int depth, WorkerScratch& scratch) noexcept {
    // Each device over the whole chunk, then the next: a device sees the same
    // calls as if they went stretch by stretch through the chain, and a rack can
    // run its chains over the chunk.
    size_t tap = 0;  // chain.deviceTaps, by device
    for (; tap < chain.deviceTaps.size(); ++tap) {  // those before the first device
        const int e = chain.deviceTaps[tap];
        if (snap.edges[static_cast<size_t>(e)].tapDevice >= 0) break;
        tapInto(snap, e, left, right, frames);
    }
    size_t nextSwitch = 0;  // chain.switches, by device
    for (size_t i = 0; i < chain.inserts.size(); ++i) {
        Processor& insert = *chain.inserts[i];
        const RackRender* rack = i < chain.racks.size() ? chain.racks[i].get() : nullptr;
        while (nextSwitch < chain.switches.size() && chain.switches[nextSwitch].lane.insert < static_cast<int>(i)) {
            ++nextSwitch;
        }
        const SwitchRender* switched =
            nextSwitch < chain.switches.size() && chain.switches[nextSwitch].lane.insert == static_cast<int>(i)
                ? &chain.switches[nextSwitch]
                : nullptr;
        if (insert.isEnabled()) {
            // The strip's signal waits for a sidechain that comes later than it (but
            // not while monitored: a player hears only the devices' own latency).
            // (Each edge into it has the same delay: one waits for them all.)
            const int e = !rack && i < chain.sidechains.size() && !chain.sidechains[i].empty() ? chain.sidechains[i].front() : -1;
            if (e >= 0) {
                const EdgeRender& edge = snap.edges[static_cast<size_t>(e)];
                if (DelayLine* wait = deviceDelayLine(edge, e)) {
                    wait->process(left, right, frames, monitored ? 0 : edge.deviceDelay);
                }
            }
            // Switched by its automation: its gain over the chunk, and its input
            // as late as it would come out of it.
            WorkerScratch::Switch& own = scratch.switches[static_cast<size_t>(std::clamp(depth, 0, kMaxRackDepth))];
            float* gain = own.gain.data();
            float* dryL = own.dryLeft.data();
            float* dryR = own.dryRight.data();
            bool on = true, full = true;
            if (switched) {
                fillSwitch(snap, switched->lane, frames, gain, scratch);
                std::copy_n(left, frames, dryL);
                std::copy_n(right, frames, dryR);
                if (DelayLine* line = switchDelayLine(*switched)) line->process(dryL, dryR, frames, switched->latency);
                on = std::any_of(gain, gain + frames, [](float g) { return g > 0.f; });
                full = std::all_of(gain, gain + frames, [](float g) { return g >= 1.f; });
                insert.setSwitchFadeIn(0);
            } else if (insert.switchedOff() || insert.switchFadeIn() > 0) {
                // Its lane gone while it was off (overridden, deleted): it fades in
                // from its input over a switch's fade, as its lane would have.
                const int fade = switchFade(snap.sampleRate);
                if (insert.switchedOff()) insert.setSwitchFadeIn(fade);
                const int done = fade - insert.switchFadeIn();
                for (int s = 0; s < frames; ++s) {
                    gain[s] = std::min(1.f, static_cast<float>(done + s + 1) / static_cast<float>(fade));
                }
                std::copy_n(left, frames, dryL);
                std::copy_n(right, frames, dryR);
                full = false;
                insert.setSwitchFadeIn(std::max(0, insert.switchFadeIn() - frames));
            }
            // Back on (by its lane, or its lane gone): from silence, as if just switched on.
            if (on && insert.switchedOff()) resetInsert(insert, rack);
            insert.setSwitchedOff(!on);
            if (on && rack) {
                processRack(snap, *rack, context, slices, events, left, right, frames, monitored, scratch);
            } else if (on) {
                processDevice(snap, chain, i, context, slices, events, left, right, frames, scratch);
            }
            if (!on) {
                std::copy_n(dryL, frames, left);
                std::copy_n(dryR, frames, right);
                // The taps after the devices in it take what it passes on.
                if (rack) tapSkippedRack(snap, *rack, left, right, frames);
            } else if (!full) {
                for (int s = 0; s < frames; ++s) {
                    left[s] = dryL[s] + gain[s] * (left[s] - dryL[s]);
                    right[s] = dryR[s] + gain[s] * (right[s] - dryR[s]);
                }
            }
        }
        for (; tap < chain.deviceTaps.size(); ++tap) {
            const int e = chain.deviceTaps[tap];
            if (snap.edges[static_cast<size_t>(e)].tapDevice > static_cast<int>(i)) break;
            tapInto(snap, e, left, right, frames);
        }
    }
}

void Renderer::processDevice(const RenderSnapshot& snap, const StripRender& chain, size_t i, ProcessContext& context,
                             const Slices& slices, ProcessEvent* events, float* left, float* right, int frames,
                             WorkerScratch& scratch) noexcept {
    Processor& insert = *chain.inserts[i];
    const double samplesPerBeat = snap.samplesPerBeat();
    static const std::vector<int> kNone;
    const std::vector<int>& keys = i < chain.sidechains.size() ? chain.sidechains[i] : kNone;
    insert.setSidechainConnected(!keys.empty());
    // What its sidechain hears: the edge's signal; several edges' summed (solo may leave some out).
    const float* keyL = nullptr;
    const float* keyR = nullptr;
    bool summing = false;
    for (const int e : keys) {
        const EdgeRender& edge = snap.edges[static_cast<size_t>(e)];
        if (!edge.state->live) continue;
        const float* l;
        const float* r;
        edgeSignal(snap, edge, l, r);
        if (!keyL) {
            keyL = l;
            keyR = r;
            continue;
        }
        float* sumL = scratch.keyLeft.data();
        float* sumR = scratch.keyRight.data();
        if (!summing) {
            std::copy_n(keyL, frames, sumL);
            std::copy_n(keyR, frames, sumR);
            keyL = sumL;
            keyR = sumR;
            summing = true;
        }
        addStereo(sumL, sumR, l, r, frames);
    }
    // Another track's notes instead of the strip's: its own copy, split by slice
    // as processInserts() splits the strip's.
    MidiFeed* feed = i < chain.midiFeeds.size() ? chain.midiFeeds[i].get() : nullptr;
    std::array<std::pair<int, int>, std::tuple_size_v<decltype(Slices::slice)>> fed{};
    if (feed) {
        int next = 0;
        for (int s = 0; s < slices.count; ++s) {
            const Slice& slice = slices.slice[static_cast<size_t>(s)];
            const int first = next;
            while (next < feed->numEvents &&
                   (s == slices.count - 1 || feed->events[static_cast<size_t>(next)].sampleOffset < slice.offset + slice.length)) {
                feed->events[static_cast<size_t>(next++)].sampleOffset -= slice.offset;
            }
            fed[static_cast<size_t>(s)] = {first, next};
        }
    }
    for (int s = 0; s < slices.count; ++s) {
        const Slice& slice = slices.slice[static_cast<size_t>(s)];
        float* channels[2] = {left + slice.offset, right + slice.offset};
        if (keyL) insert.setSidechain(keyL + slice.offset, keyR + slice.offset);
        context.samplePos = slice.position;
        context.beatPos = slice.position / samplesPerBeat;
        if (feed) {
            const auto [first, end] = fed[static_cast<size_t>(s)];
            context.inEvents = {feed->events.data() + first, static_cast<size_t>(end - first)};
        } else {
            context.inEvents = {events + slice.firstEvent, static_cast<size_t>(slice.endEvent - slice.firstEvent)};
        }
        for (const AutomationRender& lane : chain.automation) {
            if (lane.insert == static_cast<int>(i)) automateInsert(lane, slice.position, slice.length, slice.moving);
        }
        insert.process(context, channels, 2, slice.length);
        insert.clearAutomation();
        insert.setSidechain(nullptr, nullptr);
    }
}

DelayLine* Renderer::switchDelayLine(const SwitchRender& device) const noexcept {
    return delayOverride_ ? lineAt(switchDelayOverride_, device.delayIndex) : device.delay.get();
}

int Renderer::switchFade(double sampleRate) noexcept {
    return std::clamp(static_cast<int>(std::lround(sampleRate * kSwitchFade)), 1, kMaxSwitchFade);
}

void Renderer::fillSwitch(const RenderSnapshot& snap, const AutomationRender& lane, int frames, float* out,
                          WorkerScratch& scratch) const noexcept {
    // The switch (0 or 1) at each of the n samples of the timeline from t, as
    // late as the lane is (before the timeline's start: as at its start).
    const auto states = [&lane](int64_t t, int n, float* to) {
        t -= lane.latency;
        const int before = t < 0 ? static_cast<int>(std::min<int64_t>(n, -t)) : 0;
        if (before > 0) std::fill_n(to, before, automationValue(lane.nodes, 0));
        if (n > before) fillAutomation(lane.nodes, std::max<int64_t>(t, 0), n - before, to + before);
        for (int i = 0; i < n; ++i) to[i] = automationSwitchOn(to[i]) ? 1.f : 0.f;
    };
    if (!playing_ || numSegments_ == 0) {  // stopped: as at the playhead
        float state = 0.f;
        states(position_, 1, &state);
        std::fill_n(out, frames, state);
        return;
    }
    if (segments_[0].offset > 0) {  // the end of a count-in: still at the first segment's start
        float state = 0.f;
        states(segments_[0].position, 1, &state);
        std::fill_n(out, segments_[0].offset, state);
    }
    // Over each stretch of the timeline, each sample's gain is the share of the
    // fade's samples played up to it that the switch was on (a window sliding
    // along). After a jump (a loop's wrap, a locate) those come from where the
    // playhead was, so a switch across it fades too.
    const int fade = switchFade(snap.sampleRate);
    float* on = scratch.switchStates.data();
    for (int s = 0; s < numSegments_; ++s) {
        const Segment& segment = segments_[s];
        int64_t before = segment.position;  // the timeline played just before it, up to here
        if (segment.jump) {
            const int64_t previous = s > 0 ? segments_[s - 1].position + segments_[s - 1].length : chunkFrom_;
            if (previous >= 0) before = previous;
        }
        states(before - (fade - 1), fade - 1, on);
        states(segment.position, segment.length, on + fade - 1);
        int count = 0;
        for (int i = 0; i < fade - 1; ++i) count += on[i] > 0.f ? 1 : 0;
        float* to = out + segment.offset;
        for (int i = 0; i < segment.length; ++i) {
            count += on[i + fade - 1] > 0.f ? 1 : 0;
            to[i] = static_cast<float>(count) / static_cast<float>(fade);
            count -= on[i] > 0.f ? 1 : 0;
        }
    }
}

void Renderer::processRack(const RenderSnapshot& snap, const RackRender& rack, ProcessContext& context,
                           const Slices& slices, ProcessEvent* events, float* left, float* right, int frames,
                           [[maybe_unused]] bool monitored, WorkerScratch& scratch) noexcept {
    if (rack.chains.empty()) return;  // it passes its input on
    WorkerScratch::Rack& buffers = scratch.racks[static_cast<size_t>(std::clamp(rack.depth, 0, kMaxRackDepth - 1))];
    float* sumL = buffers.sumLeft.data();
    float* sumR = buffers.sumRight.data();
    float* chainL = buffers.chainLeft.data();
    float* chainR = buffers.chainRight.data();
    std::fill_n(sumL, frames, 0.f);
    std::fill_n(sumR, frames, 0.f);
    // Solo among the rack's chains: read once, so its chains agree.
    bool anySolo = false;
    for (const ChainRender& chain : rack.chains) anySolo = anySolo || chain.params->solo.load(std::memory_order_relaxed);
    for (const ChainRender& chain : rack.chains) {
        std::copy_n(left, frames, chainL);
        std::copy_n(right, frames, chainR);
        // (Never as monitored: a chain skipping its sidechain waits would play early
        // against the chains lined up to it by chain.compensation.)
        if (takeResets(chain)) {
            processChain(snap, chain, context, slices, events, chainL, chainR, frames, false, rack.depth + 1, scratch);
        } else {  // nothing changes the signal along it
            for (const int e : chain.deviceTaps) tapInto(snap, e, chainL, chainR, frames);
        }
        const bool audible = !chain.params->mute.load(std::memory_order_relaxed) &&
                             (!anySolo || chain.params->solo.load(std::memory_order_relaxed));
        applyFader(snap, *chain.params, chain.volume, chain.pan, nullptr, audible, chainL, chainR, frames,
                   chunkFlags_.live,
                   scratch, nullptr);
        // Lined up with the slowest chain (not skipped while monitored: chains
        // out of line with each other would comb-filter).
        if (DelayLine* delay = chainDelayLine(chain)) delay->process(chainL, chainR, frames, chain.compensation);
        addStereo(sumL, sumR, chainL, chainR, frames);
    }
    std::copy_n(sumL, frames, left);
    std::copy_n(sumR, frames, right);
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
                          const AutomationRender& pan, const AutomationRender* activator, bool audible, float* left,
                          float* right, int frames, bool live, WorkerScratch& scratch, float* audibleOut) noexcept {
    const float* autoGain = nullptr;
    const float* autoLeft = nullptr;
    const float* autoRight = nullptr;
    const float* autoOn = nullptr;  // the switch's gain (it is heard where it is on)
    if (activator && !activator->empty()) {
        float* gains = scratch.activator.data();
        fillSwitch(snap, *activator, frames, gains, scratch);
        autoOn = gains;
    }
    if (!volume.empty()) {
        float* gains = scratch.autoGain.data();
        fillLane(volume, frames, gains);
        for (int i = 0; i < frames; ++i) gains[i] = automationVolumeGain(gains[i]);
        autoGain = gains;
    }
    if (!pan.empty()) {
        float* lefts = scratch.autoPanLeft.data();
        float* rights = scratch.autoPanRight.data();
        fillLane(pan, frames, lefts);
        for (int i = 0; i < frames; ++i) balanceGains(automationPan(lefts[i]), lefts[i], rights[i]);
        autoLeft = lefts;
        autoRight = rights;
    }
    const float on = audible ? 1.f : 0.f;
    const float gain = params.gain.load(std::memory_order_relaxed);
    float panLeft, panRight;
    balanceGains(params.pan.load(std::memory_order_relaxed), panLeft, panRight);

    if (!live) {
        // Offline renders hold the engine lock, so parameters cannot change
        // mid-render; apply them directly and leave the live ramps alone.
        if (audibleOut) {
            for (int i = 0; i < frames; ++i) audibleOut[i] = on * (autoOn ? autoOn[i] : 1.f);
        }
        for (int i = 0; i < frames; ++i) {
            const float g = on * (autoOn ? autoOn[i] : 1.f) * (autoGain ? autoGain[i] : gain);
            left[i] *= g * (autoLeft ? autoLeft[i] : panLeft);
            right[i] *= g * (autoRight ? autoRight[i] : panRight);
        }
        return;
    }
    // Smoothing state lives with the track so it survives snapshot swaps.
    if (params.smoothingSampleRate != snap.sampleRate) {
        params.switchGain = 1.f;
        for (SmoothedValue* smoothed : {&params.audible, &params.volume, &params.panLeft, &params.panRight}) {
            smoothed->reset(snap.sampleRate, 0.02);
        }
        params.audible.snapTo(on);
        params.volume.snapTo(gain);
        params.panLeft.snapTo(panLeft);
        params.panRight.snapTo(panRight);
        params.smoothingSampleRate = snap.sampleRate;
    }
    // Its switch's automation gone (overridden, deleted): its own mute takes over
    // from where the switch left it.
    if (!autoOn && params.switchGain != 1.f) {
        params.audible.snapTo(params.audible.current() * params.switchGain);
        params.switchGain = 1.f;
    }
    params.audible.setTarget(on);
    params.volume.setTarget(gain);
    params.panLeft.setTarget(panLeft);
    params.panRight.setTarget(panRight);
    float peakL = 0.f, peakR = 0.f;
    const bool ramping = params.audible.isSmoothing() || params.volume.isSmoothing() ||
                         params.panLeft.isSmoothing() || params.panRight.isSmoothing();
    if (!ramping && !autoGain && !autoLeft && !autoOn) {
        // Settled and unautomated (most strips, most of the time): the gains are
        // constant for the whole chunk, so the ramps' next() would change nothing.
        const float a = params.audible.current();
        if (audibleOut) std::fill_n(audibleOut, frames, a);
        if (a == 0.f) {
            std::fill_n(left, frames, 0.f);
            std::fill_n(right, frames, 0.f);
        } else {
            const float g = a * params.volume.current();  // a * g * l, in the loop's order
            const float l = g * params.panLeft.current();
            const float r = g * params.panRight.current();
            for (int i = 0; i < frames; ++i) {
                left[i] *= l;
                right[i] *= r;
                peakL = std::max(peakL, std::abs(left[i]));
                peakR = std::max(peakR, std::abs(right[i]));
            }
        }
        atomicStoreMax(params.peakLeft, peakL);
        atomicStoreMax(params.peakRight, peakR);
        return;
    }
    for (int i = 0; i < frames; ++i) {
        float a = params.audible.next();
        if (autoOn) a *= autoOn[i];
        if (audibleOut) audibleOut[i] = a;
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
    if (autoOn && frames > 0) params.switchGain = autoOn[frames - 1];
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

void Renderer::assignVoices(const TrackRender& track, const WarpVoiceSet& voices, TrackBuffers& buffers) noexcept {
    // In the order renderClips() plays the clips, one entry per stretched clip
    // (without a voice if there are more than voices: it stays silent).
    buffers.numVoices = 0;
    if (buffers.monitored || buffers.recorded) return;
    for (int s = 0; s < numSegments_; ++s) {
        const Segment& segment = segments_[s];
        forEachClip(track, segment.position, segment.position + segment.length,
                    [&](const ClipRender& clip, int64_t, int64_t) {
                        if (clip.playback != ClipRender::Playback::Stretch) return;
                        if (buffers.numVoices == TrackBuffers::kMaxClipVoices) return;
                        TrackBuffers::ClipVoice& slot = buffers.voices[static_cast<size_t>(buffers.numVoices++)];
                        slot.clip = &clip;
                        slot.continuing = false;
                        slot.voice = acquireVoice(voices, clip, slot.continuing);
                    });
    }
}

void Renderer::renderClips(const TrackRender& track, const Segment& segment, const TrackBuffers& buffers,
                           int& nextVoice, WorkerScratch& scratch, float* left, float* right) noexcept {
    const int64_t segStart = segment.position;
    const int64_t segEnd = segStart + segment.length;
    float* outL = left + segment.offset;
    float* outR = right + segment.offset;

    forEachClip(track, segStart, segEnd, [&](const ClipRender& clip, int64_t from, int64_t to) {
        // Where this clip's unscaled audio for [from, to) comes from.
        const float* srcL;
        const float* srcR;
        int64_t srcBase;  // index into srcL/srcR of timeline sample t is t - srcBase
        if (clip.playback == ClipRender::Playback::Direct) {
            const AudioSource& source = *clip.source;
            srcL = source.stereoChannel(0);
            srcR = source.stereoChannel(1);
            srcBase = clip.start - clip.sourceOffset;
        } else {
            const int n = static_cast<int>(to - from);
            if (clip.playback == ClipRender::Playback::Resample) {
                renderResampled(clip, from, n, scratch.warpLeft.data(), scratch.warpRight.data());
            } else {
                if (nextVoice >= buffers.numVoices || buffers.voices[static_cast<size_t>(nextVoice)].clip != &clip) {
                    return;  // past what the prologue could hand out
                }
                const TrackBuffers::ClipVoice& slot = buffers.voices[static_cast<size_t>(nextVoice++)];
                if (!slot.voice) return;
                slot.voice->render(clip, from, n, slot.continuing, scratch.warpLeft.data(), scratch.warpRight.data());
            }
            srcL = scratch.warpLeft.data();
            srcR = scratch.warpRight.data();
            srcBase = from;
        }

        for (int64_t t = from; t < to; ++t) {
            const int64_t inClip = t - clip.start;
            float g = clip.gain;
            if (inClip < clip.fadeIn) {
                const auto x = static_cast<float>(static_cast<double>(inClip) / static_cast<double>(clip.fadeIn));
                g *= automationShape(x, clip.fadeInCurve);
            }
            const int64_t toEnd = clip.length - inClip;
            if (toEnd < clip.fadeOut) {
                const auto x = static_cast<float>(static_cast<double>(toEnd) / static_cast<double>(clip.fadeOut));
                g *= automationShape(x, clip.fadeOutCurve);
            }
            const int64_t src = t - srcBase;
            const int64_t dst = t - segStart;
            outL[dst] += srcL[src] * g * clip.panLeft;
            outR[dst] += srcR[src] * g * clip.panRight;
        }
    });
}

void Renderer::releaseNotes(uint32_t trackId, int offset, TrackBuffers& out) noexcept {
    for (int a = 0; a < numActiveNotes_;) {
        if (activeNotes_[a].trackId != trackId) {
            ++a;
            continue;
        }
        ProcessEvent off = ProcessEvent::noteOff(offset, activeNotes_[a].key);
        off.noteId = activeNotes_[a].noteId;
        if (!out.pushEvent(off)) return;  // next block
        activeNotes_[a] = activeNotes_[--numActiveNotes_];
    }
}

bool Renderer::startNote(const TrackRender& track, size_t index, int32_t offset, int64_t from,
                         TrackBuffers& out) noexcept {
    const NoteRender& note = track.notes[index];
    if (numActiveNotes_ == static_cast<int>(activeNotes_.size())) return false;
    ProcessEvent on = ProcessEvent::noteOn(offset, note.key, note.velocity);
    on.noteId = newNoteId();
    if (!out.pushEvent(on)) return false;
    float bend = 0.f;
    if (note.bend) {  // where it starts bent (or is joined bent: chased), it sounds so from its start
        bend = static_cast<float>(note.bend->at(static_cast<double>(from)));
        if (std::abs(bend) < kBendEpsilon || !out.pushEvent(ProcessEvent::noteBend(offset, note.key, on.noteId, bend))) {
            bend = 0.f;
        }
    }
    activeNotes_[numActiveNotes_++] = {track.id, note.key, note.end, on.noteId, note.start, static_cast<int>(index), bend};
    return true;
}

const NoteRender* Renderer::findNote(const TrackRender& track, ActiveNote& note) const noexcept {
    const auto& notes = track.notes;
    const auto at = static_cast<size_t>(note.index);
    if (at < notes.size() && notes[at].start == note.start && notes[at].key == note.key) return &notes[at];
    // In a newer snapshot (or after a tempo change, which may round its start a sample either way).
    constexpr int64_t kSlack = 2;
    auto it = std::lower_bound(notes.begin(), notes.end(), note.start - kSlack,
                               [](const NoteRender& n, int64_t value) { return n.start < value; });
    for (; it != notes.end() && it->start <= note.start + kSlack; ++it) {
        if (it->key != note.key) continue;
        note.index = static_cast<int>(it - notes.begin());
        return &*it;
    }
    return nullptr;
}

void Renderer::bendNotes(const TrackRender& track, const Segment& segment, TrackBuffers& out) noexcept {
    const int64_t segEnd = segment.position + segment.length;
    for (int a = 0; a < numActiveNotes_; ++a) {
        ActiveNote& note = activeNotes_[a];
        if (note.trackId != track.id) continue;
        // (Found again where it was, unless the notes changed: a bend drawn while
        // it sounds is heard at once; one taken away, or the note, leaves it as it was.)
        const NoteRender* render = findNote(track, note);
        if (!render || !render->bend) continue;
        const NoteBendRender& bend = *render->bend;
        // Along the note's own grid (so the values don't depend on where blocks
        // start), after its start (its note-on sent that), until it ends.
        const int64_t until = std::min(segEnd, note.end);
        const int64_t first = std::max<int64_t>(segment.position - note.start, 1);
        const auto send = [&](int64_t t) {
            const auto value = static_cast<float>(bend.at(static_cast<double>(t)));
            if (std::abs(value - note.bend) < kBendEpsilon) return true;
            if (out.numEvents >= TrackBuffers::kMaxEvents - kBendHeadroom) return false;
            const auto offset = static_cast<int32_t>(segment.offset + (note.start + t - segment.position));
            out.pushEvent(ProcessEvent::noteBend(offset, note.key, note.noteId, value));
            note.bend = value;
            return true;
        };
        for (int64_t t = (first + kBendStep - 1) / kBendStep * kBendStep; note.start + t < until; t += kBendStep) {
            if (!send(t)) return;
        }
        // Ending here: its last sample too, so its release holds where the curve
        // ends (a slide into the next note lands on it, not a step short).
        const int64_t last = note.end - 1 - note.start;
        if (note.end <= segEnd && last >= first && !send(last)) return;
    }
}

void Renderer::buildNoteEvents(const TrackRender& track, TrackBuffers& out, bool hearsInput, bool clipNotes,
                               MidiRecordingTake* take) noexcept {
    // Notes played by hand come first, all at the block start and in the order
    // they were played: dragging a note across keys releases one key and plays
    // the next several times within a block, and reordering those would leave
    // notes playing whose note-off came first.
    // Each has an id, as the arrangement's notes do (a plug-in then knows it
    // isn't bent: Vst3Processor tells it so).
    out.numEvents = 0;
    for (int i = 0; i < numPreviewNotes_; ++i) {
        const PreviewNote& note = previewNotes_[i];
        if (note.trackId != track.id) continue;
        if (note.velocity > 0) {
            ProcessEvent on = ProcessEvent::noteOn(0, note.key, note.velocity);
            on.noteId = newNoteId();
            if (!out.pushEvent(on)) continue;
            if (numHeldPreviews_ == static_cast<int>(heldPreviews_.size())) {  // (one never let go: forgotten)
                std::copy(heldPreviews_.begin() + 1, heldPreviews_.end(), heldPreviews_.begin());
                --numHeldPreviews_;
            }
            heldPreviews_[static_cast<size_t>(numHeldPreviews_++)] = {track.id, note.key, on.noteId};
        } else {
            ProcessEvent off = ProcessEvent::noteOff(0, note.key);
            for (int h = 0; h < numHeldPreviews_; ++h) {  // the oldest of that key, as a synth releases it
                const HeldPreview& held = heldPreviews_[static_cast<size_t>(h)];
                if (held.trackId != track.id || held.key != note.key) continue;
                off.noteId = held.noteId;
                std::copy(heldPreviews_.begin() + h + 1, heldPreviews_.begin() + numHeldPreviews_, heldPreviews_.begin() + h);
                --numHeldPreviews_;
                break;
            }
            out.pushEvent(off);
        }
    }
    const int previewEvents = out.numEvents;
    routeMidiInput(track, hearsInput, take, out);
    if (!playing_ || !clipNotes) releaseNotes(track.id, 0, out);

    for (int s = 0; s < numSegments_ && clipNotes; ++s) {
        const Segment& segment = segments_[s];
        if (segment.jump) releaseNotes(track.id, segment.offset, out);
        const int64_t segEnd = segment.position + segment.length;

        // Notes already underway where playback starts sound from there.
        if (segment.chase) {
            for (size_t i = 0; i < track.notes.size(); ++i) {
                const NoteRender& note = track.notes[i];
                if (note.start >= segment.position) break;
                if (note.end <= segment.position) continue;
                if (!startNote(track, i, segment.offset, segment.position - note.start, out)) break;
            }
        }

        // Note-ons: notes starting in this segment.
        auto it = std::lower_bound(track.notes.begin(), track.notes.end(), segment.position,
                                   [](const NoteRender& note, int64_t value) { return note.start < value; });
        for (; it != track.notes.end() && it->start < segEnd; ++it) {
            const auto offset = static_cast<int32_t>(segment.offset + (it->start - segment.position));
            if (!startNote(track, static_cast<size_t>(it - track.notes.begin()), offset, 0, out)) break;
        }

        // The sounding notes' bends, those ending here too (before their note-offs go).
        bendNotes(track, segment, out);

        // Note-offs due in this segment, including for notes that just started.
        for (int a = 0; a < numActiveNotes_;) {
            const ActiveNote& note = activeNotes_[a];
            if (note.trackId != track.id || note.end >= segEnd) {
                ++a;
                continue;
            }
            const auto offset =
                static_cast<int32_t>(segment.offset + std::max<int64_t>(0, note.end - segment.position));
            ProcessEvent off = ProcessEvent::noteOff(offset, note.key);
            off.noteId = note.noteId;
            if (!out.pushEvent(off)) break;
            activeNotes_[a] = activeNotes_[--numActiveNotes_];
        }
    }

    // The arrangement's notes and the input in time order; at the same offset a
    // note-off comes first, so a note that ends where the next one on its key
    // starts doesn't cut the new one short, and a bend after the note-on it bends.
    const auto rank = [](const ProcessEvent& event) {
        return event.type == ProcessEvent::Type::NoteOff ? 0 : event.type == ProcessEvent::Type::NoteBend ? 2 : 1;
    };
    std::sort(out.events.begin() + previewEvents, out.events.begin() + out.numEvents,
              [&rank](const ProcessEvent& a, const ProcessEvent& b) {
                  if (a.sampleOffset != b.sampleOffset) return a.sampleOffset < b.sampleOffset;
                  return rank(a) < rank(b);
              });
}

void Renderer::forgetNotesOfRemovedTracks(const RenderSnapshot& snap) noexcept {
    // Their instruments are gone along with the track.
    const auto exists = [&snap](uint32_t id) {
        return std::any_of(snap.tracks.begin(), snap.tracks.end(),
                           [id](const TrackRender& track) { return track.id == id; });
    };
    for (int a = 0; a < numActiveNotes_;) {
        if (exists(activeNotes_[a].trackId)) {
            ++a;
        } else {
            activeNotes_[a] = activeNotes_[--numActiveNotes_];
        }
    }
    for (int a = 0; a < numLiveNotes_;) {
        if (exists(liveNotes_[a].trackId)) {
            ++a;
        } else {
            liveNotes_[a] = liveNotes_[--numLiveNotes_];
        }
    }
}

void Renderer::scheduleTicks(const RenderSnapshot& snap, int64_t position, int length, int offset,
                             int64_t end) noexcept {
    const double tickLength = snap.samplesPerBeat() * 4.0 / snap.timeSigDen;
    if (tickLength <= 0.0) return;
    for (int64_t k = static_cast<int64_t>(std::ceil(position / tickLength - 1e-9));; ++k) {
        const int64_t t = std::llround(k * tickLength);
        if (t < position) continue;
        if (t >= position + length || t >= end) break;
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
    const float* srcL = source.stereoChannel(0);
    const float* srcR = source.stereoChannel(1);
    const int64_t n = std::min<int64_t>(frames, source.frames() - previewPosition_);
    addStereo(masterLeft_.data(), masterRight_.data(), srcL + previewPosition_, srcR + previewPosition_,
              static_cast<int>(n), gain);
    previewPosition_ += n;
    if (previewPosition_ >= source.frames()) {
        previewSource_ = nullptr;
        if (shared.previewSerial.load(std::memory_order_acquire) == previewSerial_) {
            shared.previewActive.store(false, std::memory_order_relaxed);
        }
    }
}

}  // namespace sub
