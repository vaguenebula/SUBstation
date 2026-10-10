#include "model/Notes.h"

#include "model/Numbers.h"

#include <QHash>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

namespace sub::app::notes {

namespace {

const char* const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

bool lessBend(const BendPoint& a, const BendPoint& b) {
    return std::tie(a.time, a.semitones, a.curve) < std::tie(b.time, b.semitones, b.curve);
}
bool lessVibrato(const Vibrato& a, const Vibrato& b) {
    return std::tie(a.start, a.length, a.depth, a.rate, a.fade) < std::tie(b.start, b.length, b.depth, b.rate, b.fade);
}
double clampBend(double semitones) { return std::clamp(semitones, -kMaxBendSemitones, kMaxBendSemitones); }

// A set of notes to ask "is this one of them?".
class NoteSet {
public:
    explicit NoteSet(std::vector<Note> notes) : notes_(std::move(notes)) {
        std::sort(notes_.begin(), notes_.end(), lessFull);
    }
    bool contains(const Note& note) const { return std::binary_search(notes_.begin(), notes_.end(), note, lessFull); }

private:
    std::vector<Note> notes_;
};

int floorDivInt(int value, int divisor) {
    const int quotient = value / divisor;
    return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

// Python's random.triangular(low, high, mode) with mode in the middle.
double triangular(QRandomGenerator& rng, double low, double high, double mode) {
    double u = rng.generateDouble();
    double c = (mode - low) / (high - low);
    if (u > c) {
        u = 1.0 - u;
        c = 1.0 - c;
        std::swap(low, high);
    }
    return low + (high - low) * std::sqrt(u * c);
}

}  // namespace

QString noteName(int pitch) {
    return QString::fromLatin1(kNoteNames[floorMod(pitch, 12)]) + QString::number(floorDivInt(pitch, 12) - 2);
}

bool isBlackKey(int pitch) {
    const int pc = floorMod(pitch, 12);
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

bool lessFull(const Note& a, const Note& b) {
    if (a.start != b.start) return a.start < b.start;
    if (a.pitch != b.pitch) return a.pitch < b.pitch;
    if (a.length != b.length) return a.length < b.length;
    if (a.velocity != b.velocity) return a.velocity < b.velocity;
    if (a.muted != b.muted) return a.muted < b.muted;
    if (a.bend != b.bend) return std::lexicographical_compare(a.bend.begin(), a.bend.end(), b.bend.begin(), b.bend.end(), lessBend);
    return std::lexicographical_compare(a.vibrato.begin(), a.vibrato.end(), b.vibrato.begin(), b.vibrato.end(), lessVibrato);
}

bool byTime(const Note& a, const Note& b) {
    if (a.start != b.start) return a.start < b.start;
    return a.pitch < b.pitch;
}

std::vector<Note> normalize(const std::vector<Note>& notes) {
    std::vector<Note> result = notes;
    std::sort(result.begin(), result.end(), lessFull);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::pair<double, double> span(const std::vector<Note>& notes) {
    double start = notes.front().start;
    double end = notes.front().end();
    for (const Note& n : notes) {
        start = std::min(start, n.start);
        end = std::max(end, n.end());
    }
    return {start, end};
}

std::vector<Note> resolveOverlaps(const std::vector<Note>& notes, const std::vector<Note>& winners) {
    std::vector<Note> sortedWinners = normalize(winners);
    std::stable_sort(sortedWinners.begin(), sortedWinners.end(),
                     [](const Note& a, const Note& b) { return a.start < b.start; });
    QHash<int, std::vector<Note>> byPitch;
    for (const Note& winner : sortedWinners) byPitch[winner.pitch].push_back(winner);
    const NoteSet isWinner(winners);
    std::vector<Note> result;
    for (Note note : notes) {
        if (isWinner.contains(note)) {
            result.push_back(note);
            continue;
        }
        bool gone = false;
        const auto found = byPitch.constFind(note.pitch);
        if (found != byPitch.constEnd()) {
            for (const Note& winner : *found) {
                if (winner.start >= note.end() - kEps || winner.end() <= note.start + kEps) continue;
                if (note.start < winner.start - kEps) {
                    note.length = winner.start - note.start;
                } else if (note.end() > winner.end() + kEps) {
                    const double end = note.end();
                    note = withStart(note, winner.end());
                    note.length = end - winner.end();
                } else {
                    gone = true;
                    break;
                }
            }
        }
        if (!gone && note.length >= kMinNoteBeats - kEps) result.push_back(note);
    }
    return result;
}

std::vector<Note> withActive(const std::vector<Note>& notes, const std::vector<Note>& targets, bool active) {
    const NoteSet isTarget(targets);
    std::vector<Note> result = notes;
    for (Note& n : result) {
        if (isTarget.contains(n)) n.muted = !active;
    }
    return normalize(result);
}

std::vector<Note> place(const std::vector<Note>& notes, const std::vector<Note>& removed,
                        const std::vector<Note>& added) {
    const NoteSet isRemoved(removed);
    std::vector<Note> kept;
    for (const Note& n : notes) {
        if (!isRemoved.contains(n)) kept.push_back(n);
    }
    kept.insert(kept.end(), added.begin(), added.end());
    return normalize(resolveOverlaps(kept, added));
}

std::pair<double, int> clampMove(const std::vector<Note>& notes, double deltaBeats, int deltaPitch) {
    if (notes.empty()) return {0.0, 0};
    double earliest = notes.front().start;
    int lowest = notes.front().pitch;
    int highest = notes.front().pitch;
    for (const Note& n : notes) {
        earliest = std::min(earliest, n.start);
        lowest = std::min(lowest, n.pitch);
        highest = std::max(highest, n.pitch);
    }
    deltaBeats = std::max(deltaBeats, -earliest);
    deltaPitch = std::max(-lowest, std::min(deltaPitch, 127 - highest));
    return {deltaBeats, deltaPitch};
}

std::vector<Note> shifted(const std::vector<Note>& notes, double deltaBeats, int deltaPitch) {
    std::vector<Note> result = notes;
    for (Note& n : result) {
        n.start += deltaBeats;
        n.pitch += deltaPitch;
    }
    return result;
}

std::vector<Note> resized(const std::vector<Note>& notes, Edge edge, double delta, double minLength) {
    std::vector<Note> result;
    for (Note n : notes) {
        if (edge == Edge::End) {
            n.length = std::max(std::min(minLength, n.length), n.length + delta);
        } else {
            const double end = n.end();
            const double start = std::max(0.0, std::min(n.start + delta, end - std::min(minLength, n.length)));
            n = withStart(n, start);
            n.length = end - start;
        }
        result.push_back(n);
    }
    return result;
}

std::vector<Note> withVelocity(const std::vector<Note>& notes, double delta) {
    std::vector<Note> result = notes;
    for (Note& n : result) {
        n.velocity = static_cast<int>(std::max(1.0, std::min(127.0, roundHalfEven(n.velocity + delta))));
    }
    return result;
}

std::vector<Note> untangle(const std::vector<Note>& notes) {
    std::vector<Note> ordered = notes;
    std::stable_sort(ordered.begin(), ordered.end(), [](const Note& a, const Note& b) {
        if (a.pitch != b.pitch) return a.pitch < b.pitch;
        if (a.start != b.start) return a.start < b.start;
        return a.length < b.length;
    });
    std::vector<Note> result;
    for (size_t i = 0; i < ordered.size(); ++i) {
        Note note = ordered[i];
        if (i + 1 < ordered.size()) {
            const Note& following = ordered[i + 1];
            if (following.pitch == note.pitch && following.start < note.end() - kEps) {
                note.length = following.start - note.start;
            }
        }
        if (note.length >= kMinNoteBeats - kEps) result.push_back(note);
    }
    return result;
}

std::vector<Note> legato(const std::vector<Note>& targets, const std::vector<Note>& clipNotes, double end) {
    const std::set<double> startSet = [&] {
        std::set<double> s;
        for (const Note& n : targets) s.insert(n.start);
        return s;
    }();
    const std::vector<double> starts(startSet.begin(), startSet.end());
    const double last = starts.empty() ? 0.0 : starts.back();
    double lastStop = end;
    bool anyAfter = false;
    for (const Note& n : clipNotes) {
        if (n.start > last + kEps) {
            lastStop = anyAfter ? std::min(lastStop, n.start) : n.start;
            anyAfter = true;
        }
    }
    QHash<int, std::vector<double>> sameKey;
    for (const Note& n : clipNotes) sameKey[n.pitch].push_back(n.start);
    std::vector<Note> result;
    for (Note n : targets) {
        const auto next = std::upper_bound(starts.begin(), starts.end(), n.start + kEps);
        double stop = next != starts.end() ? *next : lastStop;
        const auto found = sameKey.constFind(n.pitch);
        if (found != sameKey.constEnd()) {
            for (double s : *found) {
                if (s > n.start + kEps) stop = std::min(stop, s);
            }
        }
        if (stop - n.start >= kMinNoteBeats) n.length = stop - n.start;
        result.push_back(n);
    }
    return result;
}

std::vector<Note> timeScaled(const std::vector<Note>& notes, double factor) {
    if (notes.empty()) return {};
    double origin = notes.front().start;
    for (const Note& n : notes) origin = std::min(origin, n.start);
    std::vector<Note> scaled;
    for (Note n : notes) {
        n.start = origin + (n.start - origin) * factor;
        n.length = std::max(kMinNoteBeats, n.length * factor);
        for (BendPoint& point : n.bend) point.time *= factor;  // (vibratos keep their rates, in time)
        for (Vibrato& vibrato : n.vibrato) {
            vibrato.start *= factor;
            vibrato.length *= factor;
        }
        scaled.push_back(n);
    }
    return untangle(scaled);
}

std::vector<Note> quantized(const std::vector<Note>& notes, double step, double amount) {
    std::vector<Note> moved;
    for (Note n : notes) {
        n.start = std::max(0.0, n.start + (roundHalfEven(n.start / step) * step - n.start) * amount);
        moved.push_back(n);
    }
    return untangle(moved);
}

std::vector<Note> humanizedTiming(const std::vector<Note>& notes, QRandomGenerator& rng, double amount) {
    std::vector<Note> result;
    for (Note n : notes) {
        n.start = std::max(0.0, n.start + triangular(rng, -1.0, 1.0, 0.0) * amount * kHumanizeBeats);
        result.push_back(n);
    }
    return untangle(result);
}

// --- Bends ---------------------------------------------------------------------------------

Note withStart(Note note, double start) {
    const double moved = start - note.start;
    note.start = start;
    for (BendPoint& point : note.bend) point.time -= moved;
    for (Vibrato& vibrato : note.vibrato) vibrato.start -= moved;
    return note;
}

Note withBendPoint(Note note, BendPoint point, int* index) {
    point.semitones = clampBend(point.semitones);
    point.curve = std::clamp(point.curve, -1.0, 1.0);
    const auto at = std::upper_bound(note.bend.begin(), note.bend.end(), point.time,
                                     [](double time, const BendPoint& p) { return time < p.time; });
    const auto placed = note.bend.insert(at, point);
    if (index) *index = static_cast<int>(placed - note.bend.begin());
    return note;
}

Note withBendPointsMoved(Note note, const std::vector<int>& indices, double deltaTime, double deltaSemitones) {
    const auto count = static_cast<int>(note.bend.size());
    std::vector<char> moving(note.bend.size(), 0);
    for (const int i : indices) {
        if (i >= 0 && i < count) moving[static_cast<size_t>(i)] = 1;
    }
    // Each moving point stays between the nearest fixed points around it, and within the note.
    for (int i = 0; i < count; ++i) {
        if (!moving[static_cast<size_t>(i)]) continue;
        double low = 0.0, high = std::max(0.0, note.length);
        for (int j = i - 1; j >= 0; --j) {
            if (!moving[static_cast<size_t>(j)]) {
                low = std::max(low, note.bend[static_cast<size_t>(j)].time);
                break;
            }
        }
        for (int j = i + 1; j < count; ++j) {
            if (!moving[static_cast<size_t>(j)]) {
                high = std::min(high, note.bend[static_cast<size_t>(j)].time);
                break;
            }
        }
        const double time = note.bend[static_cast<size_t>(i)].time;
        deltaTime = std::clamp(deltaTime, std::min(0.0, low - time), std::max(0.0, high - time));
    }
    for (int i = 0; i < count; ++i) {
        if (!moving[static_cast<size_t>(i)]) continue;
        BendPoint& point = note.bend[static_cast<size_t>(i)];
        point.time += deltaTime;
        point.semitones = clampBend(point.semitones + deltaSemitones);
    }
    std::stable_sort(note.bend.begin(), note.bend.end(),
                     [](const BendPoint& a, const BendPoint& b) { return a.time < b.time; });
    return note;
}

Note withoutBendPoints(Note note, const std::vector<int>& indices) {
    std::vector<BendPoint> kept;
    for (int i = 0; i < static_cast<int>(note.bend.size()); ++i) {
        if (std::find(indices.begin(), indices.end(), i) == indices.end()) kept.push_back(note.bend[static_cast<size_t>(i)]);
    }
    note.bend = std::move(kept);
    return note;
}

Note withBendCurve(Note note, int index, double curve) {
    if (index >= 0 && index < static_cast<int>(note.bend.size())) {
        note.bend[static_cast<size_t>(index)].curve = std::clamp(curve, -1.0, 1.0);
    }
    return note;
}

Note withVibrato(Note note, Vibrato vibrato) {
    const double from = std::clamp(vibrato.start, 0.0, note.length);
    const double to = std::clamp(vibrato.start + vibrato.length, from, note.length);
    vibrato.start = from;
    vibrato.length = to - from;
    vibrato.depth = std::clamp(vibrato.depth, -kMaxBendSemitones, kMaxBendSemitones);
    vibrato.fade = std::clamp(vibrato.fade, 0.0, 1.0);
    std::vector<Vibrato> kept;
    for (const Vibrato& other : note.vibrato) {  // what of the others it doesn't cover
        const double end = other.start + other.length;
        if (other.start < from - kEps) {
            Vibrato before = other;
            before.length = std::min(end, from) - other.start;
            if (before.length >= kMinVibratoBeats - kEps) kept.push_back(before);
        }
        if (end > to + kEps) {
            Vibrato after = other;
            after.start = std::max(other.start, to);
            after.length = end - after.start;
            if (after.length >= kMinVibratoBeats - kEps) kept.push_back(after);
        }
    }
    if (vibrato.length >= kMinVibratoBeats - kEps) kept.push_back(vibrato);
    std::sort(kept.begin(), kept.end(), [](const Vibrato& a, const Vibrato& b) { return a.start < b.start; });
    note.vibrato = std::move(kept);
    return note;
}

Note withoutVibrato(Note note, int index) {
    if (index >= 0 && index < static_cast<int>(note.vibrato.size())) note.vibrato.erase(note.vibrato.begin() + index);
    return note;
}

std::optional<Note> nextNote(const std::vector<Note>& notes, const Note& note) {
    std::optional<Note> next;
    for (const Note& other : notes) {
        if (other.muted || other.start <= note.start + kEps) continue;
        if (!next || other.start < next->start - kEps) {
            next = other;
            continue;
        }
        if (other.start > next->start + kEps) continue;
        const int distance = std::abs(other.pitch - note.pitch), best = std::abs(next->pitch - note.pitch);
        if (distance < best || (distance == best && other.pitch < next->pitch)) next = other;
    }
    return next;
}

Note withSlide(Note note, double start, double end, double semitones, double curve) {
    const double length = std::max(0.0, note.length);
    start = std::clamp(start, 0.0, length);
    end = std::clamp(end, start, length);
    const double from = note.curveAt(start);  // (no step where it begins)
    std::erase_if(note.bend, [&](const BendPoint& p) { return p.time >= start - kEps && p.time <= end + kEps; });
    note = withBendPoint(note, {start, from, curve});
    return withBendPoint(note, {end, semitones, 0.0});
}

namespace {

void simplify(const std::vector<BendPoint>& points, size_t first, size_t last, double tolerance, std::vector<char>& keep) {
    if (last <= first + 1) return;
    const BendPoint& a = points[first];
    const BendPoint& b = points[last];
    double worst = -1.0;
    size_t at = first;
    for (size_t i = first + 1; i < last; ++i) {
        const double span = b.time - a.time;
        const double line = span > 0.0 ? a.semitones + (b.semitones - a.semitones) * (points[i].time - a.time) / span
                                       : a.semitones;
        const double miss = std::abs(points[i].semitones - line);
        if (miss > worst) {
            worst = miss;
            at = i;
        }
    }
    if (worst < tolerance) return;
    keep[at] = 1;
    simplify(points, first, at, tolerance, keep);
    simplify(points, at, last, tolerance, keep);
}

}  // namespace

std::vector<BendPoint> simplifiedBend(const std::vector<BendPoint>& points, double tolerance) {
    if (points.size() <= 2) return points;
    std::vector<char> keep(points.size(), 0);
    keep.front() = keep.back() = 1;
    simplify(points, 0, points.size() - 1, tolerance, keep);
    std::vector<BendPoint> result;
    for (size_t i = 0; i < points.size(); ++i) {
        if (keep[i]) result.push_back(points[i]);
    }
    return result;
}

}  // namespace sub::app::notes
