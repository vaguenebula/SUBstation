#pragma once

// The piano roll's note area: a row per key, the grid, the notes, and editing
// them.
//
// The notes of every clip the roll shows, each in its track's colour (with
// several, a line at each clip's ends), and the stretch a rubber band selected.
//
// Mouse: double-click to add a note (one grid step long; into the clip that
// plays there) or to delete one; drag a note to move it (Ctrl copies, Alt
// bypasses the grid), drag either end to resize it, drag in empty space to
// select (each note it touches sounds for a moment as it is caught); click
// empty space to place the paste marker (where Ctrl+V pastes; dashed, apart
// from the start marker: playback doesn't start there). Selected notes
// move and resize together, each staying in its clip. Ctrl+Alt drag scrolls
// the view. Wheel: scroll (Shift: sideways), Ctrl: zoom time, Alt: make the
// keys' rows taller or shorter.
// Keys: Delete, Ctrl+A, Ctrl+D (duplicate), Ctrl+C / Ctrl+X / Ctrl+V (copy,
// cut, paste), Ctrl+U (quantize), 0 (deactivate the selected notes, or
// activate them if they all are: they show grey and aren't heard), Up/Down
// (Shift: an octave), Left/Right (a grid step; Shift: a bar). The main window
// has shortcuts for most of them too (for clips): the grid accepts their
// ShortcutOverride, so they come to it as key presses instead of firing the
// window's actions while it has the focus.
//
// Bends (NoteGridBends.cpp; B, or the bend button, turns bend mode on and off):
// every note shows its bend as a curve (a semitone a row) to edit as an
// automation envelope is: click on the line to add a point (press and drag to
// place it), click a point to delete it, drag points (Shift- or Ctrl-click
// selects several) in time and pitch (on the grid and whole semitones; Alt:
// freely), Alt-drag a segment to bend it, drag in empty space to select points,
// double-click to put one at the pitch clicked. Delete deletes the selected
// points; Ctrl+A selects every point. The vibrato tool (V) draws vibrato: drag
// across a note for the stretch it covers (up deepens it), click a note for
// vibrato from there to its end, click a vibrato to take it away. Out of bend
// mode, bent notes show their curves faintly.

#include "model/Clip.h"
#include "pianoroll/NoteSet.h"
#include "pianoroll/RollItem.h"

#include <QColor>
#include <QFont>
#include <QPointF>
#include <QRectF>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <optional>
#include <vector>

class QKeyEvent;

namespace sub::ui {

class NoteGrid : public RollItem {
    Q_OBJECT
    QML_ELEMENT

public:
    static constexpr double kEdgeGrab = 5.0;  // pixels inside each end of a note that resize it
    static constexpr double kDragThreshold = 3.0;
    static constexpr double kPasteDash = 4.0;  // the paste marker's dashes (and gaps), in pixels
    // Bends, as automation's envelopes.
    static constexpr double kPointRadius = 3.0;
    static constexpr double kPointGrab = 6.0;     // pixels around a bend point that grab it
    static constexpr double kLineGrab = 5.0;      // pixels around a curve that count as on it
    static constexpr double kSegmentGrab = 14.0;  // pixels around a segment that Alt-grab it
    static constexpr double kCurvePixels = 150.0;  // an Alt-drag this far bends a segment from straight to its most

    // Where a note was hit: its ends resize it, its body moves it.
    enum class Zone { Start, End, Body };
    struct Hit {
        ClipNote note;
        Zone zone;
    };

    explicit NoteGrid(QQuickItem* parent = nullptr);
    ~NoteGrid() override;

    // The note under `pos` (the one drawn on top) and where.
    std::optional<Hit> noteAt(const QPointF& pos) const;
    // Whether a drag is under way (the note tools stay out of its way).
    bool dragging() const;
    // The rubber band being drawn, if one is.
    std::optional<QRectF> rubberBand() const;
    // The keys the grid takes, before the window's shortcuts.
    static bool handles(const QKeyEvent* event);

    // In bend mode, what is under the mouse: a note's bend point (`point`), its
    // curve (a click adds a point at `beat`, `semitones`: on the grid, on the
    // curve drawn by hand), or with Alt a segment between two points (`point`:
    // the first).
    struct BendHit {
        enum class Kind { Point, Line, Segment };
        Kind kind = Kind::Line;
        ClipNote note;
        int point = -1;
        double beat = 0.0;  // roll beats
        double semitones = 0.0;
    };
    std::optional<BendHit> bendHitAt(const QPointF& pos, Qt::KeyboardModifiers modifiers) const;
    // The note whose curve passes nearest `pos` where it is (of the notes
    // sounding at its x), within `grab` pixels of it (< 0: however far).
    std::optional<ClipNote> curveNear(const QPointF& pos, double grab) const;

    class Gesture;

protected:
    void paint(SgPainter& painter) override;
    bool event(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void hoverEnterEvent(QHoverEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    void rollConnected(PianoRoll* roll) override;

private:
    void drawNote(SgPainter& painter, const app::Note& note, const QRectF& rect, const QColor& color,
                  const QFont& font, bool selected, bool playing, bool outOfKey, bool dim = false) const;
    // Bends (NoteGridBends.cpp): the curves (to edit in bend mode, faint
    // otherwise), presses and double-clicks in bend mode, and its cursor.
    void paintBends(SgPainter& painter, const QRectF& visible) const;
    std::unique_ptr<Gesture> bendPress(const QPointF& pos, Qt::KeyboardModifiers modifiers);
    void bendDoubleClick(const QPointF& pos, Qt::KeyboardModifiers modifiers);
    Qt::CursorShape bendCursor(const QPointF& pos, Qt::KeyboardModifiers modifiers);
    // Bend-mode keys: B, V, and in bend mode Delete and Ctrl+A. True if taken.
    bool bendKey(QKeyEvent* event);
    void updateCursor(const QPointF& pos, Qt::KeyboardModifiers modifiers);
    // Show the hand as soon as Ctrl+Alt is held, without moving the mouse.
    void onModifiers(Qt::KeyboardModifiers modifiers);

    std::unique_ptr<Gesture> gesture_;
    // What a click selects when the mouse comes up without dragging.
    std::optional<std::vector<ClipNote>> selectOnClick_;
    std::optional<QPointF> hover_;  // where the mouse is over the grid
    std::optional<BendHit> bendHover_;  // in bend mode, what is under the mouse (a ghost point, a hovered one)
};

}  // namespace sub::ui
