#pragma once

// The piano roll's note area (piano_roll/note_grid.py): a row per key, the
// grid, the notes, and editing them.
//
// Mouse: double-click to add a note (one grid step long) or to delete one;
// drag a note to move it (Ctrl copies, Alt bypasses the grid), drag either end
// to resize it, drag in empty space to select. Selected notes move and resize
// together. Ctrl+Alt drag scrolls the view. Wheel: scroll (Shift: sideways),
// Ctrl: zoom time, Alt: make the keys' rows taller or shorter.
// Keys: Delete, Ctrl+A, Ctrl+D (duplicate), Ctrl+U (quantize), Up/Down
// (Shift: an octave), Left/Right (a grid step; Shift: a bar). The main window
// has shortcuts for Delete, Ctrl+A and Ctrl+D too (for clips): the grid
// accepts their ShortcutOverride, so they come to it as key presses instead
// of firing the window's actions while it has the focus.

#include "model/Clip.h"
#include "pianoroll/RollItem.h"

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

    // Where a note was hit: its ends resize it, its body moves it.
    enum class Zone { Start, End, Body };
    struct Hit {
        app::Note note;
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
    // Ctrl+Alt: drag to scroll, as in the arrangement (and Ableton).
    static bool isPanModifier(Qt::KeyboardModifiers modifiers);

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
                  const QFont& font, bool selected, bool playing) const;
    void updateCursor(const QPointF& pos, Qt::KeyboardModifiers modifiers);
    // Show the hand as soon as Ctrl+Alt is held, without moving the mouse.
    void onModifiers(Qt::KeyboardModifiers modifiers);

    std::unique_ptr<Gesture> gesture_;
    // What a click selects when the mouse comes up without dragging.
    std::optional<std::vector<app::Note>> selectOnClick_;
    std::optional<QPointF> hover_;  // where the mouse is over the grid
};

}  // namespace sub::ui
