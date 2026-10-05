#include "pianoroll/RollPlayhead.h"

#include "model/Numbers.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QPolygonF>

namespace sub::ui {

RollPlayhead::RollPlayhead(QQuickItem* parent) : RollItem(parent) {}

void RollPlayhead::setRuler(bool ruler) {
    if (ruler == ruler_) return;
    ruler_ = ruler;
    Q_EMIT rulerChanged();
    update();
}

void RollPlayhead::rollConnected(PianoRoll* roll) {
    connect(roll, &PianoRoll::playheadChanged, this, &QQuickItem::update);
    connect(roll, &PianoRoll::viewChanged, this, &QQuickItem::update);
}

void RollPlayhead::paint(SgPainter& p) {
    PianoRoll* roll = this->roll();
    const auto playhead = roll ? roll->playhead() : std::nullopt;
    if (!playhead) return;
    const double x = app::roundHalfEven(roll->view().beatToX(*playhead));
    const double h = height();
    if (ruler_) {
        p.fillPolygon(QPolygonF({QPointF(x - 5, h - 8), QPointF(x + 6, h - 8), QPointF(x + 0.5, h - 1)}),
                      Theme::kPlayhead);
    } else {
        p.fillRect(QRectF(x, 0, 1, h), Theme::kPlayhead);
    }
}

}  // namespace sub::ui
