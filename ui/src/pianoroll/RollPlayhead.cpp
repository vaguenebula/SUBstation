#include "pianoroll/RollPlayhead.h"

#include "timeline/Timeline.h"

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
    timeline::drawPlayhead(p, roll->view(), *playhead, height(), ruler_);
}

}  // namespace sub::ui
