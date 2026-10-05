#pragma once
// A stretch of the arrangement's grid: a beat range on some adjacent tracks
// (what a time selection selects, where clips were pasted, the area clips
// cover), and what moving one would make of the clips.

#include "model/Clip.h"

#include <QMap>
#include <QString>
#include <QStringList>

#include <vector>

namespace sub::app {

struct TimeRange {
    double start = 0.0;
    double end = 0.0;
    QStringList trackIds;  // from the top one down

    friend bool operator==(const TimeRange&, const TimeRange&) = default;
};

// What ProjectEditor::moveRange would make, without making it (a drag's
// preview): the clips of the tracks it changes, and the time and track deltas
// it would move by (within the timeline, and onto tracks of their kind).
struct MovedRange {
    QMap<QString, std::vector<Clip>> clips;  // track id -> its clips after the move
    double deltaBeats = 0.0;
    int trackDelta = 0;
    // Frozen track id -> what of its frozen audio plays after the move
    // (Freeze::segments; the frozen audio moves with the clips).
    QMap<QString, std::vector<Clip>> frozen = {};

    friend bool operator==(const MovedRange&, const MovedRange&) = default;
};

}  // namespace sub::app
