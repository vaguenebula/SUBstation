#include "labels/NoteProfile.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <set>

namespace sub::intelligence::labels {

namespace {

constexpr double kTogether = 1.0 / 32.0;  // starts this close (beats) are struck together

}  // namespace

NoteProfile profile(const std::vector<NoteFact>& notes) {
    NoteProfile p;
    p.count = static_cast<int>(notes.size());
    if (notes.empty()) return p;

    std::vector<int> pitches;
    std::vector<double> lengths;
    std::set<int> distinct;
    for (const NoteFact& note : notes) {
        pitches.push_back(note.pitch);
        lengths.push_back(std::max(0.0, note.end - note.start));
        distinct.insert(note.pitch);
    }
    std::sort(pitches.begin(), pitches.end());
    const auto at = [&](double share) {
        return pitches[std::min(pitches.size() - 1, static_cast<size_t>(share * static_cast<double>(pitches.size())))];
    };
    p.lowest = at(0.05);
    p.highest = at(0.95);
    p.distinctPitches = static_cast<int>(distinct.size());
    std::vector<double> sortedLengths = lengths;
    std::sort(sortedLengths.begin(), sortedLengths.end());
    p.medianLength = sortedLengths[sortedLengths.size() / 2];

    // The median pitch by time: half of all the notes' time is below it.
    std::vector<std::pair<int, double>> timed;
    double total = 0.0;
    for (size_t i = 0; i < notes.size(); ++i) {
        const double weight = std::max(lengths[i], 1e-3);
        timed.emplace_back(notes[i].pitch, weight);
        total += weight;
    }
    std::sort(timed.begin(), timed.end());
    double below = 0.0;
    for (const auto& [pitch, weight] : timed) {
        below += weight;
        if (below >= total / 2.0) {
            p.medianPitch = pitch;
            break;
        }
    }

    // Onsets (starts struck together), how many notes sound at each, and how
    // long anything sounds (the union of the notes).
    std::vector<const NoteFact*> byStart;
    for (const NoteFact& note : notes) byStart.push_back(&note);
    std::sort(byStart.begin(), byStart.end(), [](const NoteFact* a, const NoteFact* b) { return a->start < b->start; });
    std::priority_queue<double, std::vector<double>, std::greater<>> ends;  // of the notes sounding
    int onsets = 0, chords = 0;
    double voices = 0.0, sounding = 0.0, coveredUntil = -1e300;
    size_t i = 0;
    while (i < byStart.size()) {
        const double t = byStart[i]->start;
        int started = 0;
        while (i < byStart.size() && byStart[i]->start <= t + kTogether) {
            const NoteFact& note = *byStart[i];
            ends.push(note.end);
            // (the union: what of this note isn't covered yet)
            if (note.end > coveredUntil) {
                sounding += note.end - std::max(note.start, coveredUntil);
                coveredUntil = note.end;
            }
            ++started;
            ++i;
        }
        while (!ends.empty() && ends.top() <= t + kTogether) ends.pop();
        ++onsets;
        voices += static_cast<double>(ends.size());
        if (started >= 3) ++chords;
    }
    p.voices = voices / onsets;
    p.chordShare = static_cast<double>(chords) / onsets;
    p.notesPerBeat = onsets / std::max(1.0, sounding);
    return p;
}

NoteReading readNotes(const NoteProfile& p) {
    NoteReading reading;
    if (p.count < 3) return reading;
    const double range = p.highest - p.lowest;
    const bool fast = p.medianLength <= 0.26 && p.notesPerBeat >= 1.8;
    if (p.voices >= 2.6 || p.chordShare >= 0.45) {
        if (p.medianLength >= 2.0) {
            reading = {"Chords", "Pad", 0.55};
        } else if (p.medianLength <= 0.375 && p.chordShare >= 0.5) {
            reading = {"", "Stab", 0.5};
        } else {
            reading = {"Chords", "", 0.6};
        }
    } else if (p.voices <= 1.6) {
        if (p.medianPitch < 40.0) {
            reading = {"", "Sub Bass", 0.65};
        } else if (p.medianPitch < 53.0) {
            reading = {"", "Bass", 0.6};
        } else if (fast && range >= 7.0) {
            reading = {"Arp", "", 0.6};
        } else if (p.medianPitch >= 60.0) {
            reading = {"Line", "Lead", 0.5};
        } else {
            reading = {"Line", "", 0.35};
        }
    } else if (p.medianPitch < 50.0) {
        reading = {"", "Bass", 0.45};
    } else if (fast) {
        reading = {"Arp", "", 0.4};
    } else {
        reading = {"Chords", "", 0.35};
    }
    return reading;
}

std::string noteName(int pitch) {
    static const char* const kNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int octave = static_cast<int>(std::floor(pitch / 12.0)) - 2;
    return std::string(kNames[((pitch % 12) + 12) % 12]) + std::to_string(octave);
}

std::string describe(const NoteProfile& p) {
    if (p.count == 0) return {};
    std::string text = std::to_string(p.count) + (p.count == 1 ? " note" : " notes");
    text += p.voices >= 2.6 || p.chordShare >= 0.45 ? ", chords" : p.voices <= 1.6 ? ", one at a time" : "";
    text += ", " + noteName(p.lowest) + (p.highest != p.lowest ? "-" + noteName(p.highest) : "");
    // The usual length as a note value (in 4/4: a beat is a quarter note).
    if (p.medianLength >= 4.0) {
        text += ", held for bars";
    } else if (p.medianLength > 0.0) {
        const int denominator = std::clamp(
            static_cast<int>(std::lround(std::pow(2.0, std::round(std::log2(4.0 / p.medianLength))))), 1, 64);
        text += ", mostly 1/" + std::to_string(denominator) + " notes";
    }
    return text;
}

}  // namespace sub::intelligence::labels
