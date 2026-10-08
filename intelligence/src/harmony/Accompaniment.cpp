#include "harmony/Accompaniment.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace sub::intelligence::harmony {

namespace {

constexpr double kEps = 1e-9;
constexpr double kInversionCost = 3.0;  // the first chord: semitones a root position is worth
constexpr double kDrift = 0.25;         // later chords: how much straying from the centre costs, against moving

using Voicing = std::vector<int>;

double mean(const Voicing& v) {
    double sum = 0.0;
    for (const int p : v) sum += p;
    return v.empty() ? 0.0 : sum / static_cast<double>(v.size());
}

// How far the voices move: each note to the nearest of the other chord's, both ways.
double movement(const Voicing& from, const Voicing& to) {
    const auto nearest = [](const Voicing& set, int pitch) {
        int best = std::numeric_limits<int>::max();
        for (const int p : set) best = std::min(best, std::abs(p - pitch));
        return best;
    };
    double sum = 0.0;
    for (const int p : to) sum += nearest(from, p);
    for (const int p : from) sum += nearest(to, p);
    return sum;
}

// The chord's close voicings in range: each inversion, from each octave.
std::vector<std::pair<Voicing, int>> voicings(const Chord& chord) {
    const std::array<int, 4> tones = chord.tones();
    const int size = chord.size();
    std::vector<std::pair<Voicing, int>> all;
    for (int inversion = 0; inversion < size; ++inversion) {
        for (int bottom = kChordLowest; bottom < kChordLowest + 24; ++bottom) {
            if (pitchClass(bottom) != tones[static_cast<size_t>(inversion)]) continue;
            Voicing v{bottom};
            for (int k = 1; k < size; ++k) {
                int next = v.back() + 1;
                while (pitchClass(next) != tones[static_cast<size_t>((inversion + k) % size)]) ++next;
                v.push_back(next);
            }
            if (v.back() <= kChordHighest) all.emplace_back(std::move(v), inversion);
        }
    }
    return all;
}

Voicing voice(const Chord& chord, const Voicing& previous) {
    Voicing best;
    double bestCost = std::numeric_limits<double>::max();
    for (const auto& [v, inversion] : voicings(chord)) {
        const double drift = std::abs(mean(v) - kChordCentre);
        const double cost = previous.empty() ? drift + (inversion ? kInversionCost : 0.0)
                                             : movement(previous, v) + kDrift * drift;
        if (cost < bestCost - kEps) {
            bestCost = cost;
            best = v;
        }
    }
    return best;
}

// A span cut at the bar lines in it: (start, length) of each piece.
std::vector<std::pair<double, double>> pieces(const ChordSpan& span, double barBeats) {
    std::vector<std::pair<double, double>> result;
    double start = span.start;
    while (start < span.end - kEps) {
        double end = span.end;
        if (barBeats > 0.0) {
            const double nextBar = (std::floor(start / barBeats + kEps) + 1.0) * barBeats;
            end = std::min(end, nextBar);
        }
        result.emplace_back(start, end - start);
        start = end;
    }
    return result;
}

}  // namespace

std::vector<GeneratedNote> chordPart(const std::vector<ChordSpan>& chords, double barBeats) {
    std::vector<GeneratedNote> notes;
    Voicing previous;
    for (const ChordSpan& span : chords) {
        const Voicing v = voice(span.chord, previous);
        if (v.empty()) continue;
        for (const auto& [start, length] : pieces(span, barBeats)) {
            for (const int pitch : v) notes.push_back({pitch, start, length, kChordVelocity});
        }
        previous = v;
    }
    return notes;
}

std::vector<GeneratedNote> bassPart(const std::vector<ChordSpan>& chords, double barBeats) {
    std::vector<GeneratedNote> notes;
    for (const ChordSpan& span : chords) {
        const int pitch = kBassLowest + pitchClass(span.chord.bassClass() - kBassLowest);
        for (const auto& [start, length] : pieces(span, barBeats)) notes.push_back({pitch, start, length, kBassVelocity});
    }
    return notes;
}

std::vector<ChordSpan> starterProgression(const Key& key, double start, double end, double barBeats) {
    static constexpr std::array<int, 4> kMajor{0, 4, 5, 3};  // I V vi IV
    static constexpr std::array<int, 4> kMinor{0, 5, 2, 6};  // i VI III VII
    const auto& degrees = key.minor ? kMinor : kMajor;
    std::vector<ChordSpan> spans;
    if (barBeats <= 0.0) barBeats = 4.0;
    size_t bar = 0;
    for (double at = start; at < end - kEps; ++bar) {
        const double next = std::min(end, (std::floor(at / barBeats + kEps) + 1.0) * barBeats);
        spans.push_back({at, next, key.triad(degrees[bar % degrees.size()])});
        at = next;
    }
    return spans;
}

}  // namespace sub::intelligence::harmony
