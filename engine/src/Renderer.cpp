#include "Renderer.h"

#include <algorithm>
#include <cmath>

namespace gil {

void Renderer::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    for (auto* buffer : {&trackLeft_, &trackRight_, &masterLeft_, &masterRight_, &warpLeft_, &warpRight_}) {
        buffer->assign(kMaxBlock, 0.f);
    }
    metronome_.prepare(sampleRate);
    masterGain_.reset(sampleRate, 0.02);
    masterNeedsSnap_ = true;
    previewSource_ = nullptr;
    // Processors are prepared (silenced) along with the renderer.
    activeNotes_.assign(kMaxActiveNotes, {});
    numActiveNotes_ = 0;
    events_.assign(kMaxEvents, {});
    numEvents_ = 0;
    previewNotes_.assign(kMaxPreviewNotes, {});
    numPreviewNotes_ = 0;
    expectedPosition_ = -1;
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
        case TransportCommand::Type::Play: playing_ = true; break;
        case TransportCommand::Type::Stop: playing_ = false; break;
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
}

void Renderer::processLive(const RenderSnapshot& snap, SharedState& shared, float* out, uint32_t frames,
                           uint32_t channels) noexcept {
    syncTempo(snap);
    drainCommands(shared);
    drainPreviewNotes(shared);

    uint32_t done = 0;
    while (done < frames) {
        const int n = static_cast<int>(std::min<uint32_t>(kMaxBlock, frames - done));
        renderChunk(snap, shared, n,
                    {true, snap.loopEnabled, shared.metronome.load(std::memory_order_relaxed)});
        numPreviewNotes_ = 0;  // played in the first chunk
        mixPreview(shared, n);

        float* dst = out + static_cast<size_t>(done) * channels;
        if (channels == 1) {
            for (int i = 0; i < n; ++i) dst[i] = 0.5f * (masterLeft_[i] + masterRight_[i]);
        } else {
            for (int i = 0; i < n; ++i) {
                float* frame = dst + static_cast<size_t>(i) * channels;
                frame[0] = masterLeft_[i];
                frame[1] = masterRight_[i];
                for (uint32_t c = 2; c < channels; ++c) frame[c] = 0.f;
            }
        }
        done += static_cast<uint32_t>(n);
    }
    publishTransport(shared);
}

void Renderer::renderOffline(const RenderSnapshot& snap, SharedState& shared, float* outStereo, int64_t frames,
                             bool loop, bool metronome) noexcept {
    syncTempo(snap);
    int64_t done = 0;
    while (done < frames) {
        const int n = static_cast<int>(std::min<int64_t>(kMaxBlock, frames - done));
        renderChunk(snap, shared, n, {false, loop && snap.loopEnabled, metronome});
        float* dst = outStereo + done * 2;
        for (int i = 0; i < n; ++i) {
            dst[2 * i] = masterLeft_[i];
            dst[2 * i + 1] = masterRight_[i];
        }
        done += n;
    }
}

void Renderer::renderChunk(const RenderSnapshot& snap, SharedState& shared, int frames, ChunkFlags flags) noexcept {
    const int64_t chunkStart = position_;
    const WarpVoiceSet& voices = voiceOverride_ ? *voiceOverride_ : snap.warpVoices;
    ++blockCounter_;

    // 1. Split the chunk into contiguous timeline segments (loop wrap-around).
    numSegments_ = 0;
    numTicks_ = 0;
    if (playing_) {
        int done = 0;
        while (done < frames) {
            int length = frames - done;
            const bool looping = flags.loop && numSegments_ < kMaxSegments - 1;
            if (looping && position_ < snap.loopEnd && position_ + length > snap.loopEnd) {
                length = static_cast<int>(snap.loopEnd - position_);
            }
            segments_[numSegments_++] = {position_, length, done, position_ != expectedPosition_};
            if (flags.metronome) scheduleTicks(snap, position_, length, done);
            position_ += length;
            expectedPosition_ = position_;
            done += length;
            if (looping && position_ == snap.loopEnd) position_ = snap.loopStart;
        }
    }

    ProcessContext context;
    context.sampleRate = snap.sampleRate;
    context.samplePos = chunkStart;
    context.beatPos = chunkStart / snap.samplesPerBeat();
    context.tempo = snap.tempo;
    context.timeSigNum = snap.timeSigNum;
    context.timeSigDen = snap.timeSigDen;
    context.playing = playing_;

    bool anySolo = false;
    for (const TrackRender& track : snap.tracks) {
        if (track.params->solo.load(std::memory_order_relaxed)) {
            anySolo = true;
            break;
        }
    }

    float* masterL = masterLeft_.data();
    float* masterR = masterRight_.data();
    std::fill_n(masterL, frames, 0.f);
    std::fill_n(masterR, frames, 0.f);

    // 2. Tracks: clips and notes -> inserts -> fader/pan -> master.
    for (const TrackRender& track : snap.tracks) {
        float* left = trackLeft_.data();
        float* right = trackRight_.data();
        std::fill_n(left, frames, 0.f);
        std::fill_n(right, frames, 0.f);

        for (int s = 0; s < numSegments_; ++s) renderClips(track, segments_[s], snap.clipFadeSamples, voices);
        if (!track.notes.empty() || numActiveNotes_ > 0 || numPreviewNotes_ > 0) {
            buildNoteEvents(track);
        } else {
            numEvents_ = 0;
        }

        context.inEvents = {events_.data(), static_cast<size_t>(numEvents_)};
        float* channels[2] = {left, right};
        for (const auto& insert : track.inserts) {
            if (!insert->isEnabled()) continue;
            if (insert->takeResetRequest()) insert->reset();
            insert->process(context, channels, 2, frames);
        }

        TrackParams& params = *track.params;
        const bool audible = !params.mute.load(std::memory_order_relaxed) &&
                             (!anySolo || params.solo.load(std::memory_order_relaxed));
        const float gain = audible ? params.gain.load(std::memory_order_relaxed) : 0.f;
        float panL, panR;
        balanceGains(params.pan.load(std::memory_order_relaxed), panL, panR);
        const float targetL = gain * panL;
        const float targetR = gain * panR;

        if (flags.live) {
            // Smoothing state lives with the track so it survives snapshot swaps.
            if (params.smoothingSampleRate != snap.sampleRate) {
                params.gainLeft.reset(snap.sampleRate, 0.02);
                params.gainRight.reset(snap.sampleRate, 0.02);
                params.gainLeft.snapTo(targetL);
                params.gainRight.snapTo(targetR);
                params.smoothingSampleRate = snap.sampleRate;
            }
            params.gainLeft.setTarget(targetL);
            params.gainRight.setTarget(targetR);
            float peakL = 0.f, peakR = 0.f;
            for (int i = 0; i < frames; ++i) {
                const float l = left[i] * params.gainLeft.next();
                const float r = right[i] * params.gainRight.next();
                masterL[i] += l;
                masterR[i] += r;
                peakL = std::max(peakL, std::abs(l));
                peakR = std::max(peakR, std::abs(r));
            }
            atomicStoreMax(params.peakLeft, peakL);
            atomicStoreMax(params.peakRight, peakR);
        } else {
            // Offline renders hold the engine lock, so parameters cannot change
            // mid-render; apply them directly and leave the live ramps alone.
            for (int i = 0; i < frames; ++i) {
                masterL[i] += left[i] * targetL;
                masterR[i] += right[i] * targetR;
            }
        }
    }

    forgetNotesOfRemovedTracks(snap);

    // 3. Master fader and meter.
    const float masterTarget = shared.masterGain.load(std::memory_order_relaxed);
    if (flags.live) {
        if (masterNeedsSnap_) {
            masterGain_.snapTo(masterTarget);
            masterNeedsSnap_ = false;
        }
        masterGain_.setTarget(masterTarget);
        float peakL = 0.f, peakR = 0.f;
        for (int i = 0; i < frames; ++i) {
            const float g = masterGain_.next();
            masterL[i] *= g;
            masterR[i] *= g;
            peakL = std::max(peakL, std::abs(masterL[i]));
            peakR = std::max(peakR, std::abs(masterR[i]));
        }
        atomicStoreMax(shared.masterPeakLeft, peakL);
        atomicStoreMax(shared.masterPeakRight, peakR);
    } else {
        for (int i = 0; i < frames; ++i) {
            masterL[i] *= masterTarget;
            masterR[i] *= masterTarget;
        }
    }

    // 4. Metronome, after the master fader (a click may still be ringing out).
    int cursor = 0;
    for (int t = 0; t < numTicks_; ++t) {
        metronome_.renderUntil(masterL, masterR, cursor, ticks_[t].offset);
        metronome_.start(ticks_[t].accent);
        cursor = ticks_[t].offset;
    }
    metronome_.renderUntil(masterL, masterR, cursor, frames);
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

        // Note-ons: notes starting in this segment. Notes that started earlier
        // are not chased, as in Ableton.
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
        if (numTicks_ < kMaxTicks) ticks_[numTicks_++] = {offset + static_cast<int>(t - position), k % snap.timeSigNum == 0};
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
