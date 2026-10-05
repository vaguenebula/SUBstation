#pragma once

// The beat-time ruler with the loop brace.
//
// Top strip (kLoopStrip): drag the loop brace (its body moves it, its edges
// resize it: an edge can't come within a quarter beat of the other; dragging
// in empty space draws a new loop and turns it on); double-click toggles it.
// One undo step per drag. Lower area, like Ableton's scrub area: a click (no
// movement past 3 px) plays from the snapped beat (Session::locate); a drag
// decides once, on its first 3 px, whether it pans (mostly horizontal: scroll)
// or zooms (mostly vertical: 1.012 per pixel, around where it was pressed).
// It draws the insert marker (a triangle); the playhead is an
// ArrangementPlayhead over it.

#include "arrangement/ArrangementItem.h"

#include <QPointF>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <optional>

namespace sub::ui {

class ArrangementRuler : public ArrangementItem {
    Q_OBJECT
    QML_ELEMENT

public:
    static constexpr int kLoopStrip = 14;
    static constexpr int kHeight = 40;
    static constexpr double kEdgeGrab = 6;

    explicit ArrangementRuler(QQuickItem* parent = nullptr);

protected:
    void paint(SgPainter& painter) override;
    void connectSession(app::Session* session) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void hoverEnterEvent(QHoverEvent* event) override { hoverMoveEvent(event); }
    void hoverMoveEvent(QHoverEvent* event) override;

private:
    enum class Zone { None, Start, End, Body };
    enum class Mode { LoopStart, LoopEnd, LoopBody, LoopNew, Scrub };
    struct Drag {
        Mode mode = Mode::Scrub;
        double beat = 0.0;  // where it was pressed
        double start = 0.0;  // the loop as it was
        double end = 0.0;
        QString key;  // the drag's undo merge key
        double x = 0.0, y = 0.0, lastY = 0.0;
        double scroll = 0.0;
        bool moved = false;
        bool pan = false;
    };

    Zone loopZone(double x) const;
    void updateCursor(const QPointF& pos);

    std::optional<Drag> drag_;
};

}  // namespace sub::ui
