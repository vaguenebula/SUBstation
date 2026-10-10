#pragma once

// NoteGrid::Gesture, for the files making the grid's gestures (NoteGrid.cpp:
// notes; NoteGridBends.cpp: bends): a mouse drag in the note grid. Gestures
// edit the clips live, as one undo step (the gesture's merge key).

#include "input/GestureKey.h"
#include "pianoroll/NoteGrid.h"
#include "pianoroll/PianoRoll.h"

#include <QPointF>
#include <QRectF>
#include <QString>

#include <optional>
#include <utility>

namespace sub::ui {

class NoteGrid::Gesture {
public:
    Gesture(NoteGrid* grid, const QPointF& press)
        : grid(grid), roll(grid->roll()), press(press), key(newGestureKey()) {}
    virtual ~Gesture() = default;

    // Past the drag threshold yet (from then on, it is active).
    bool started(const QPointF& pos) {
        if (!active && (pos - press).manhattanLength() >= kDragThreshold) active = true;
        return active;
    }
    virtual void move(const QPointF& pos, Qt::KeyboardModifiers modifiers) = 0;
    virtual std::optional<QRectF> rubberBand() const { return std::nullopt; }
    // What it shows by the mouse while it drags (a bend point's value), and where.
    virtual std::optional<std::pair<QPointF, QString>> label() const { return std::nullopt; }
    virtual void finish() {}
    // The mouse came up without dragging.
    virtual void clicked(Qt::KeyboardModifiers) {}

    NoteGrid* grid;
    PianoRoll* roll;
    QPointF press;
    bool active = false;
    QString key;
};

}  // namespace sub::ui
