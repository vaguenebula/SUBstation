#pragma once

// A mouse gesture on the arrangement's lanes: what a left press starts, kept by
// the item from press to release. It gets move() and finish(), and the item
// asks it what to draw: hiddenIds() (clips not drawn in place), kept() (what
// stays of moved clips), ghosts() (where they go, translucent), timeRange()
// (where the selection is drawn while it moves) and readout() (a breakpoint's
// value while dragged). The clip gestures (ClipGestures.h) preview with pure
// clip maths and make one editor call in finish(), so a drag is one undo step
// and the model isn't touched while dragging; the automation gestures
// (Envelopes.h) edit the model as they go, with a merge key per drag.
//
// paint() reads a gesture (the GUI thread is blocked meanwhile): what it
// draws is worked out in move().

#include "editor/TimeRange.h"
#include "model/Clip.h"

#include <QColor>
#include <QPointF>
#include <QSet>
#include <QString>

#include <optional>
#include <vector>

namespace sub::ui::arrangement {

inline constexpr double kDragThreshold = 4.0;  // pixels (manhattan) before a press is a drag

// A clip to draw for a gesture: on the row of the track at `row` (an index
// into Project::tracks()), in its track's colour.
struct GestureClip {
    int row = 0;
    QColor color;
    app::Clip clip;
};

// A value to show next to what is being dragged, and where.
struct Readout {
    QPointF at;
    QString text;
};

class Gesture {
public:
    virtual ~Gesture() = default;

    virtual void move(const QPointF& pos, Qt::KeyboardModifiers modifiers) = 0;
    virtual void finish() {}
    // The mouse was taken away mid-gesture (a popup): it ends where it is,
    // what it previewed goes.
    virtual void cancel() {}

    virtual QSet<QString> hiddenIds() const { return {}; }
    virtual std::vector<GestureClip> ghosts() const { return {}; }
    virtual std::vector<GestureClip> kept() const { return {}; }
    virtual std::optional<app::TimeRange> timeRange() const { return std::nullopt; }
    virtual std::optional<Readout> readout() const { return std::nullopt; }
};

}  // namespace sub::ui::arrangement
