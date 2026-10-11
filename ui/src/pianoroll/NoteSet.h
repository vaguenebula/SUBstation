#pragma once

// Sets of notes, as the old piano roll kept them (Python sets of frozen
// Notes): a sorted vector with no two notes alike. Notes are values, so a
// moved note is a new note, and two identical notes on the same key and time
// are one.
//
// The piano roll shows one clip or several at once: a ClipNote is a note of
// one of them (its index among the roll's clips) in that clip's own content
// beats, and sets of them are kept the same way, by clip, then note.

#include "model/Clip.h"
#include "model/Notes.h"

#include <algorithm>
#include <tuple>
#include <vector>

namespace sub::ui {

// A note of one of the clips the piano roll shows: which (its index in the
// roll's clips(), 0 the lead) and the note, in that clip's content beats.
struct ClipNote {
    int clip = 0;
    app::Note note;

    friend bool operator==(const ClipNote&, const ClipNote&) = default;
};

}  // namespace sub::ui

namespace sub::ui::roll {

// A total order on notes: start, pitch, length, velocity, deactivated last,
// then their bends (the model's, app::notes::lessFull).
inline bool noteLess(const app::Note& a, const app::Note& b) { return app::notes::lessFull(a, b); }

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

// --- Notes of several clips ---

// By clip, then as noteLess.
inline bool clipNoteLess(const ClipNote& a, const ClipNote& b) {
    if (a.clip != b.clip) return a.clip < b.clip;
    return noteLess(a.note, b.note);
}

inline std::vector<ClipNote> clipNoteSet(std::vector<ClipNote> notes) {
    std::sort(notes.begin(), notes.end(), clipNoteLess);
    notes.erase(std::unique(notes.begin(), notes.end()), notes.end());
    return notes;
}

inline bool contains(const std::vector<ClipNote>& set, const ClipNote& note) {
    return std::binary_search(set.begin(), set.end(), note, clipNoteLess);
}

inline std::vector<ClipNote> united(std::vector<ClipNote> a, const std::vector<ClipNote>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return clipNoteSet(std::move(a));
}

inline std::vector<ClipNote> without(std::vector<ClipNote> set, const ClipNote& note) {
    set.erase(std::remove(set.begin(), set.end(), note), set.end());
    return set;
}

// The notes of clip `clip` among them, by time.
inline std::vector<app::Note> notesOf(const std::vector<ClipNote>& notes, int clip) {
    std::vector<app::Note> result;
    for (const ClipNote& n : notes) {
        if (n.clip == clip) result.push_back(n.note);
    }
    return byTime(std::move(result));
}

// Notes of clip `clip`, as ClipNotes.
inline std::vector<ClipNote> tagged(const std::vector<app::Note>& notes, int clip) {
    std::vector<ClipNote> result;
    result.reserve(notes.size());
    for (const app::Note& n : notes) result.push_back({clip, n});
    return result;
}

// The clips they are notes of, in order, each once.
inline std::vector<int> clipsOf(const std::vector<ClipNote>& notes) {
    std::vector<int> clips;
    for (const ClipNote& n : notes) clips.push_back(n.clip);
    std::sort(clips.begin(), clips.end());
    clips.erase(std::unique(clips.begin(), clips.end()), clips.end());
    return clips;
}

}  // namespace sub::ui::roll
