#pragma once
// Clip content copied from a time selection (Ctrl+C / Ctrl+X): what
// ProjectEditor::copyRange gives and paste takes, with the frozen audio
// copied along from frozen tracks.

#include "model/Automation.h"
#include "model/Clip.h"

#include <QString>

#include <utility>
#include <vector>

namespace sub::app {

// One track's part of copied clip content: its clips (starts from the copied
// range's start), and the automation under them (key: points from beat 0).
struct CopiedTrack {
    QString trackId;
    QString kind;  // the track's (audio, MIDI, group): pasted onto tracks of its kind
    int row = 0;  // tracks below the topmost copied one
    std::vector<Clip> clips;
    std::vector<std::pair<QString, Envelope>> automation;  // in the track's order of its envelopes

    friend bool operator==(const CopiedTrack&, const CopiedTrack&) = default;
};

// Frozen audio copied with clip content: what of a frozen track's (or
// group's) render played in the copied range (Freeze::segments, from beat 0),
// copied from a time selection taking in all of what it holds. It pastes only
// back into that frozen track, the same render (`path`), with the clips copied
// from it onto the tracks they came from.
struct CopiedFreeze {
    QString trackId;
    QString path;
    std::vector<Clip> segments;

    friend bool operator==(const CopiedFreeze&, const CopiedFreeze&) = default;
};

// Clip content copied from a time selection, `length` beats long.
struct ClipboardContent {
    double length = 0.0;
    std::vector<CopiedTrack> tracks;  // the topmost first
    std::vector<CopiedFreeze> frozen = {};  // in the arrangement's order

    // The frozen audio copied from this frozen track (null: none).
    const CopiedFreeze* frozenOf(const QString& trackId) const {
        for (const CopiedFreeze& f : frozen) {
            if (f.trackId == trackId) return &f;
        }
        return nullptr;
    }

    friend bool operator==(const ClipboardContent&, const ClipboardContent&) = default;
};

}  // namespace sub::app
