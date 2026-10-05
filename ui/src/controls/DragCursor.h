#pragma once

// Hides the mouse cursor while a knob or value box is dragged (from the first
// move: a click leaves it be), then puts it back where the drag started.
// Meanwhile, at the top or bottom of the screen it jumps to the middle, so a
// drag never runs out of room. A popup opening mid-drag (a right-click menu)
// ends the drag: the release goes to the popup. (In Qt Quick the item also
// calls release() when it loses the mouse grab.)

#include <QObject>
#include <QPoint>
#include <QPointF>

#include <optional>

class QQuickItem;

namespace sub::ui {

class DragCursor : public QObject {
public:
    explicit DragCursor(QObject* parent = nullptr);
    ~DragCursor() override;

    bool dragging() const { return start_.has_value(); }
    bool hidden() const { return hidden_; }

    void press(const QPointF& globalPosition);
    // The mouse moved while dragging `item`: the y (in the item) to measure the
    // next move from: where it is, or where it jumped to (unless not `jump`).
    qreal moved(QQuickItem* item, const QPointF& position, const QPointF& globalPosition, bool jump = true);
    void release();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    std::optional<QPoint> start_;
    bool hidden_ = false;
};

}  // namespace sub::ui
