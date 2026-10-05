#pragma once

// Sets of notes, as the old piano roll kept them (Python sets of frozen
// Notes): a sorted vector with no two notes alike. Notes are values, so a
// moved note is a new note, and two identical notes on the same key and time
// are one.

#include "model/Clip.h"
#include "model/Notes.h"

#include <algorithm>
#include <tuple>
#include <vector>

namespace sub::ui::roll {

// A total order on notes: start, pitch, length, velocity.
inline bool noteLess(const app::Note& a, const app::Note& b) {
    return std::tie(a.start, a.pitch, a.length, a.velocity) < std::tie(b.start, b.pitch, b.length, b.velocity);
}

inline std::vector<app::Note> noteSet(std::vector<app::Note> notes) {
    std::sort(notes.begin(), notes.end(), noteLess);
    notes.erase(std::unique(notes.begin(), notes.end()), notes.end());
    return notes;
}

inline bool contains(const std::vector<app::Note>& set, const app::Note& note) {
    return std::binary_search(set.begin(), set.end(), note, noteLess);
}

// a | b
inline std::vector<app::Note> united(std::vector<app::Note> a, const std::vector<app::Note>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return noteSet(std::move(a));
}

// set - {note}
inline std::vector<app::Note> without(std::vector<app::Note> set, const app::Note& note) {
    set.erase(std::remove(set.begin(), set.end(), note), set.end());
    return set;
}

// sorted(notes, key=by_time)
inline std::vector<app::Note> byTime(std::vector<app::Note> notes) {
    std::stable_sort(notes.begin(), notes.end(), app::notes::byTime);
    return notes;
}

}  // namespace sub::ui::roll
