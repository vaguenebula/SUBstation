#pragma once

// The piano roll's ruler: bar numbers in the clip's own time, a bar in the
// track's colour under the part the clip plays, and the start marker (the
// playhead is a RollPlayhead above it). Click to play from there (the snapped
// beat, converted to the timeline: the roll's locateRequested); drag
// horizontally to scroll, vertically to zoom, as the arrangement's ruler does
// (decided on its first 3 px).

#include "pianoroll/RollItem.h"

#include <QtQml/qqmlregistration.h>

#include <optional>

namespace sub::ui {

class PianoRuler : public RollItem {
    Q_OBJECT
    QML_ELEMENT

public:
    explicit PianoRuler(QQuickItem* parent = nullptr);

protected:
    void paint(SgPainter& painter) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;

private:
    struct Drag {
        double x;
        double y;
        double lastY;
        double scroll;  // scrollBeats at the press
        double beat;    // the content beat pressed
        bool moved = false;
        bool pan = false;
    };
    std::optional<Drag> drag_;
};

}  // namespace sub::ui
