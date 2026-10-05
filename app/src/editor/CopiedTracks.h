#pragma once
// Tracks copied (Ctrl+C / Ctrl+X on tracks): what ProjectEditor::copyTracks
// gives and pasteTracks takes.

#include "model/Track.h"

#include <QSet>
#include <QString>
#include <QStringList>

#include <vector>

namespace sub::app {

// Tracks as they were when copied: each of `roots` (in track order) followed by
// what is in it, if a group. `folded`: their devices that were folded.
struct CopiedTracks {
    QStringList roots;
    std::vector<Track> tracks;
    QSet<QString> folded;

    friend bool operator==(const CopiedTracks&, const CopiedTracks&) = default;
};

}  // namespace sub::app
