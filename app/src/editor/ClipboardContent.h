#pragma once
// Clip content copied from a time selection (Ctrl+C / Ctrl+X): what
// ProjectEditor::copyRange gives and paste takes.

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

// Clip content copied from a time selection, `length` beats long.
struct ClipboardContent {
    double length = 0.0;
    std::vector<CopiedTrack> tracks;  // the topmost first

    friend bool operator==(const ClipboardContent&, const ClipboardContent&) = default;
};

}  // namespace sub::app
