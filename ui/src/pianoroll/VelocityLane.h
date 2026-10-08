#pragma once

// The velocity editor under the piano roll: a stem per note at its start (of
// every clip shown, in its track's colour), as tall as its velocity (1 to 127
// over the lane's height), white for selected notes. Drag a stem up or down; with several notes selected, dragging one of
// theirs changes them all by the same amount, committed live as one undo step.
// The selected notes' values show while dragging.

#include "model/Clip.h"
#include "pianoroll/NoteSet.h"
#include "pianoroll/RollItem.h"

#include <QString>
#include <QtQml/qqmlregistration.h>

#include <map>
#include <optional>
#include <vector>

namespace sub::ui {

class VelocityLane : public RollItem {
    Q_OBJECT
    QML_ELEMENT

public:
    static constexpr int kHeight = 72;
    static constexpr double kStemGrab = 6.0;  // pixels either side of a stem
    static constexpr double kMarginTop = 8.0;
    static constexpr double kMarginBottom = 3.0;

    explicit VelocityLane(QQuickItem* parent = nullptr);

    // The height velocities span, and where a velocity's stem tops out.
    double span() const { return height() - kMarginTop - kMarginBottom; }
    double velocityY(int velocity) const { return height() - kMarginBottom - span() * velocity / 127; }
    // The note whose stem is nearest `x` (selected notes first), if within reach.
    std::optional<ClipNote> stemAt(double x) const;

protected:
    void paint(SgPainter& painter) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void hoverEnterEvent(QHoverEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;

private:
    struct Drag {
        double y;
        std::map<int, std::vector<app::Note>> base;  // the notes at the press of the clips changing
        QString key;                                 // the gesture's undo merge key
        std::vector<ClipNote> targets;               // the selected notes
    };
    std::optional<Drag> drag_;
};

}  // namespace sub::ui
