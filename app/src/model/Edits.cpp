#include "model/Edits.h"

#include "model/Ids.h"
#include "model/Notes.h"
#include "model/Paths.h"

#include <QFileInfo>

#include <algorithm>
#include <limits>

namespace sub::app::edits {

namespace {

void sortByStart(std::vector<Clip>& clips) {
    std::stable_sort(clips.begin(), clips.end(), [](const Clip& a, const Clip& b) { return a.startBeat < b.startBeat; });
}

// The part of `clip` between two beats, its content left in place on the
// timeline; none if that is too short to keep.
std::optional<Clip> piece(const Clip& clip, double start, double end, double tempo, const QString& clipId) {
    Clip part = clip;
    part.id = clipId;
    part.startBeat = start;
    if (clip.isMidi()) {
        if (end - start < kMinMidiClipBeats) return std::nullopt;
        part.durationBeats = end - start;
        part.offsetBeats = clip.offsetBeats + (start - clip.startBeat);
        return part;
    }
    const double duration = clip.beatsToSource(end - start, tempo);
    if (duration < kMinClipSec) return std::nullopt;
    part.durationSec = duration;
    part.offsetSec = clip.offsetSec + clip.beatsToSource(start - clip.startBeat, tempo);
    // Its fades stay at the clip's ends: a piece from inside it has none there.
    if (start > clip.startBeat + kEps) part.fadeInSec = 0.0;
    if (end < clip.endBeat(tempo) - kEps) part.fadeOutSec = 0.0;
    part.fitFades();
    return part;
}

}  // namespace

std::vector<Interval> subtractIntervals(double start, double end, const std::vector<Interval>& cuts) {
    std::vector<Interval> pieces{{start, end}};
    for (const auto& [cutStart, cutEnd] : cuts) {
        std::vector<Interval> next;
        for (const auto& [a, b] : pieces) {
            if (cutEnd <= a + kEps || cutStart >= b - kEps) {
                next.emplace_back(a, b);
                continue;
            }
            if (cutStart > a + kEps) next.emplace_back(a, cutStart);
            if (cutEnd < b - kEps) next.emplace_back(cutEnd, b);
        }
        pieces = std::move(next);
    }
    return pieces;
}

std::vector<Clip> resolveOverlaps(const std::vector<Clip>& clips, const QSet<QString>& winners, double tempo) {
    std::vector<Interval> cuts;
    for (const Clip& c : clips) {
        if (winners.contains(c.id)) cuts.emplace_back(c.startBeat, c.endBeat(tempo));
    }
    std::sort(cuts.begin(), cuts.end());
    std::vector<Clip> result;
    if (cuts.empty()) {
        result = clips;
        sortByStart(result);
        return result;
    }
    for (const Clip& clip : clips) {
        if (winners.contains(clip.id)) {
            result.push_back(clip);
        } else {
            for (Clip& part : cutClip(clip, cuts, tempo)) result.push_back(std::move(part));
        }
    }
    sortByStart(result);
    return result;
}

std::vector<Clip> cutClip(const Clip& clip, const std::vector<Interval>& cuts, double tempo) {
    const double end = clip.endBeat(tempo);
    const std::vector<Interval> pieces = subtractIntervals(clip.startBeat, end, cuts);
    if (pieces.size() == 1 && pieces.front() == Interval{clip.startBeat, end}) {
        return {clip};  // untouched: avoid float drift from recomputing it
    }
    std::vector<Clip> result;
    for (const auto& [a, b] : pieces) {
        if (auto part = piece(clip, a, b, tempo, result.empty() ? clip.id : newId())) result.push_back(std::move(*part));
    }
    return result;
}

std::vector<Clip> removeRange(const std::vector<Clip>& clips, double start, double end, double tempo) {
    std::vector<Clip> result;
    for (const Clip& clip : clips) {
        for (Clip& part : cutClip(clip, {{start, end}}, tempo)) result.push_back(std::move(part));
    }
    sortByStart(result);
    return result;
}

std::vector<Clip> fitToTempo(const std::vector<Clip>& clips, double tempo, bool* changed) {
    std::vector<Clip> ordered = clips;
    sortByStart(ordered);
    std::vector<Clip> result;
    bool anyChange = false;
    for (size_t i = 0; i < ordered.size(); ++i) {
        const Clip& clip = ordered[i];
        if (i + 1 < ordered.size() && clip.endBeat(tempo) > ordered[i + 1].startBeat + kEps) {
            anyChange = true;
            auto trimmed = piece(clip, clip.startBeat, ordered[i + 1].startBeat, tempo, clip.id);
            if (!trimmed) continue;  // fully covered
            result.push_back(std::move(*trimmed));
        } else {
            result.push_back(clip);
        }
    }
    if (changed != nullptr) *changed = anyChange;
    return anyChange ? result : clips;
}

std::vector<Clip> sliceRange(const std::vector<Clip>& clips, double start, double end, double tempo, bool keepIds) {
    std::vector<Clip> result;
    for (const Clip& clip : clips) {
        const double clipEnd = clip.endBeat(tempo);
        if (clip.startBeat >= start - kEps && clipEnd <= end + kEps) {
            if (clip.startBeat < end && clipEnd > start) {
                Clip whole = clip;
                if (!keepIds) whole.id = newId();
                result.push_back(std::move(whole));
            }
            continue;
        }
        if (auto part = piece(clip, std::max(start, clip.startBeat), std::min(end, clipEnd), tempo, newId())) {
            result.push_back(std::move(*part));
        }
    }
    return result;
}

std::optional<std::pair<Clip, Clip>> splitClip(const Clip& clip, double atBeat, double tempo) {
    if (clip.isMidi()) {
        auto left = piece(clip, clip.startBeat, atBeat, tempo, clip.id);
        auto right = piece(clip, atBeat, clip.endBeat(), tempo, newId());
        if (!left || !right) return std::nullopt;
        return std::make_pair(std::move(*left), std::move(*right));
    }
    const double leftSec = clip.beatsToSource(atBeat - clip.startBeat, tempo);
    if (leftSec < kMinClipSec || clip.durationSec - leftSec < kMinClipSec) return std::nullopt;
    Clip left = clip;
    left.durationSec = leftSec;
    left.fadeOutSec = 0.0;  // (the fade out goes with the right half, the fade in stays)
    left.fitFades();
    Clip right = clip;
    right.id = newId();
    right.startBeat = atBeat;
    right.offsetSec = clip.offsetSec + leftSec;
    right.durationSec = clip.durationSec - leftSec;
    right.fadeInSec = 0.0;
    right.fitFades();
    return std::make_pair(std::move(left), std::move(right));
}

Clip trimStart(const Clip& clip, double newStartBeat, double tempo) {
    Clip trimmed = clip;
    if (clip.isMidi()) {
        const double end = clip.endBeat();
        const double start = std::max(0.0, std::min(newStartBeat, end - kMinMidiClipBeats));
        double offset = clip.offsetBeats + (start - clip.startBeat);
        if (offset < 0) {
            // Revealing time before the first content beat: the content grows at
            // its start, so every note moves along to stay put on the timeline.
            for (Note& n : trimmed.notes) n.start -= offset;
            offset = 0.0;
        }
        trimmed.startBeat = start;
        trimmed.durationBeats = end - start;
        trimmed.offsetBeats = offset;
        return trimmed;
    }
    double delta = clip.beatsToSource(newStartBeat - clip.startBeat, tempo);
    delta = std::max(delta, -clip.offsetSec);  // cannot reveal audio before the file starts
    delta = std::max(delta, -clip.beatsToSource(clip.startBeat, tempo));  // nor move before beat 0
    delta = std::min(delta, clip.durationSec - kMinClipSec);
    trimmed.startBeat = clip.startBeat + clip.sourceToBeats(delta, tempo);
    trimmed.offsetSec = clip.offsetSec + delta;
    trimmed.durationSec = clip.durationSec - delta;
    trimmed.fitFades();
    return trimmed;
}

Clip trimEnd(const Clip& clip, double newEndBeat, double tempo) {
    Clip trimmed = clip;
    if (clip.isMidi()) {
        trimmed.durationBeats = std::max(kMinMidiClipBeats, newEndBeat - clip.startBeat);
        return trimmed;
    }
    const double duration = clip.beatsToSource(newEndBeat - clip.startBeat, tempo);
    const double available = clip.sourceDurationSec > 0 ? clip.sourceDurationSec - clip.offsetSec
                                                        : std::numeric_limits<double>::infinity();
    trimmed.durationSec = std::max(kMinClipSec, std::min(duration, available));
    trimmed.fitFades();
    return trimmed;
}

Clip stretchClip(const Clip& clip, double edgeBeat, bool left, double tempo) {
    const double start = clip.startBeat, end = clip.endBeat(tempo);
    double length = left ? std::min(end - edgeBeat, end) : edgeBeat - start;  // (not before beat 0)
    Clip stretched = clip;
    if (clip.isMidi()) {
        if (clip.durationBeats <= 0) return clip;
        length = std::max(length, kMinMidiClipBeats);
        // The notes scale about the content's start, so the window stays on the same notes.
        const double factor = length / clip.durationBeats;
        for (Note& n : stretched.notes) {
            n.start *= factor;
            n.length *= factor;
        }
        stretched.offsetBeats = clip.offsetBeats * factor;
        stretched.durationBeats = length;
    } else {
        if (clip.durationSec <= 0) return clip;
        double bpm = std::clamp(length * 60.0 / clip.durationSec, kMinSegmentBpm, kMaxSegmentBpm);
        if (left) bpm = std::min(bpm, end * 60.0 / clip.durationSec);
        if (bpm < kMinSegmentBpm) return clip;  // (too close to beat 0 to grow from its start)
        length = clip.durationSec * bpm / 60.0;
        stretched.warp = true;
        stretched.segmentBpm = bpm;
    }
    stretched.startBeat = left ? end - length : start;
    return stretched;
}

Clip slipClip(const Clip& clip, double deltaBeats, double tempo) {
    Clip slipped = clip;
    if (clip.isMidi()) {
        double offset = clip.offsetBeats - deltaBeats;
        if (offset < 0) {  // (as trimStart: the content grows at its start)
            for (Note& n : slipped.notes) n.start -= offset;
            offset = 0.0;
        }
        slipped.offsetBeats = offset;
        return slipped;
    }
    const double latest = clip.sourceDurationSec > 0 ? std::max(0.0, clip.sourceDurationSec - clip.durationSec)
                                                     : std::numeric_limits<double>::infinity();
    slipped.offsetSec = std::clamp(clip.offsetSec - clip.beatsToSource(deltaBeats, tempo), 0.0,
                                   std::max(latest, 0.0));
    return slipped;
}

Clip fadeClip(const Clip& clip, bool out, double beats, double tempo) {
    if (!clip.isAudio()) return clip;
    Clip faded = clip;
    const double room = std::max(0.0, clip.durationSec - (out ? clip.fadeInSec : clip.fadeOutSec));
    (out ? faded.fadeOutSec : faded.fadeInSec) = std::clamp(clip.beatsToSource(beats, tempo), 0.0, room);
    faded.fitFades();
    return faded;
}

Clip curveFade(const Clip& clip, bool out, double curve) {
    if (!clip.isAudio()) return clip;
    Clip curved = clip;
    (out ? curved.fadeOutCurve : curved.fadeInCurve) = curve;
    curved.fitFades();
    return curved;
}

Clip reverseClip(const Clip& clip, const QString& path, double totalSec) {
    const double offset = std::max(0.0, totalSec - clip.offsetSec - clip.durationSec);
    const bool back = !clip.reversedFrom.isEmpty() && samePath(path, clip.reversedFrom);
    Clip reversed = clip;
    reversed.path = path;
    reversed.offsetSec = offset;
    reversed.sourceDurationSec = totalSec;
    reversed.durationSec = std::min(clip.durationSec, totalSec - offset);
    reversed.reversedFrom = back ? QString() : clip.path;
    return reversed;
}

bool playsWholeFile(const Clip& clip) {
    return clip.isAudio() && clip.offsetSec <= 1e-6 &&
           (clip.sourceDurationSec <= 0.0 || clip.durationSec >= clip.sourceDurationSec - 1e-3);
}

Clip replaceFile(const Clip& clip, const QString& path, double totalSec) {
    if (!clip.isAudio() || totalSec <= 0.0) return clip;
    Clip replaced = clip;
    replaced.path = path;
    replaced.name = QFileInfo(path).completeBaseName();
    replaced.reversedFrom.clear();
    replaced.sourceDurationSec = totalSec;
    if (playsWholeFile(clip)) {
        replaced.offsetSec = 0.0;
        replaced.durationSec = totalSec;
    } else {
        // (A stretch starting past the new file's end plays from its start.)
        const double offset = clip.offsetSec < totalSec - kMinClipSec ? std::max(0.0, clip.offsetSec) : 0.0;
        replaced.offsetSec = offset;
        replaced.durationSec = std::min(clip.durationSec, totalSec - offset);
    }
    replaced.fitFades();
    return replaced;
}

Clip relinkFile(const Clip& clip, const QString& from, const QString& to) {
    if (!clip.isAudio()) return clip;
    Clip relinked = clip;
    if (samePath(clip.path, from)) relinked.path = to;
    if (!clip.reversedFrom.isEmpty() && samePath(clip.reversedFrom, from)) relinked.reversedFrom = to;
    return relinked;
}

std::pair<double, double> selectionSpan(const std::vector<Clip>& clips, double tempo) {
    double start = clips.front().startBeat;
    double end = clips.front().endBeat(tempo);
    for (const Clip& c : clips) {
        start = std::min(start, c.startBeat);
        end = std::max(end, c.endBeat(tempo));
    }
    return {start, end};
}

Clip consolidateMidi(const std::vector<Clip>& clips) {
    std::vector<Clip> ordered = clips;
    sortByStart(ordered);
    const auto [start, end] = selectionSpan(ordered, 0.0);
    const bool allMuted = std::none_of(ordered.begin(), ordered.end(), [](const Clip& c) { return c.plays(); });
    std::vector<Note> played;
    for (const Clip& c : ordered) {
        for (const PlayedNote& p : c.playedNotes()) {
            Note note = p.note;
            note.start = p.start - start;
            note.length = p.end - p.start;
            note.muted = note.muted || (!c.plays() && !allMuted);  // (what was silent stays silent)
            played.push_back(note);
        }
    }
    Clip joined = Clip::midi(ordered.front().id, ordered.front().name, start, end - start, 0.0,
                             notes::normalize(notes::untangle(played)));
    joined.muted = allMuted;
    return joined;
}

}  // namespace sub::app::edits
