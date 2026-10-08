#include "model/Notes.h"

#include "model/Numbers.h"

#include <QHash>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>
#include <set>

namespace sub::app::notes {

namespace {

const char* const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// A total order on notes: by start, pitch, length, velocity, deactivated last.
bool lessFull(const Note& a, const Note& b) {
    if (a.start != b.start) return a.start < b.start;
    if (a.pitch != b.pitch) return a.pitch < b.pitch;
    if (a.length != b.length) return a.length < b.length;
    if (a.velocity != b.velocity) return a.velocity < b.velocity;
    return a.muted < b.muted;
}

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

int floorMod(int value, int divisor) { return ((value % divisor) + divisor) % divisor; }

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
                    note.start = winner.end();
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
            n.start = start;
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

}  // namespace sub::app::notes
