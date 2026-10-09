#include "arrangement/ArrangementPlayhead.h"

#include "model/Numbers.h"
#include "timeline/Timeline.h"

namespace sub::ui {

ArrangementPlayhead::ArrangementPlayhead(QQuickItem* parent) : ArrangementItem(parent) {}

void ArrangementPlayhead::setRuler(bool ruler) {
    if (ruler == ruler_) return;
    ruler_ = ruler;
    Q_EMIT rulerChanged();
    update();
}

void ArrangementPlayhead::connectArrangement(Arrangement* arrangement) {
    connect(arrangement, &Arrangement::playheadChanged, this, &QQuickItem::update);
}

void ArrangementPlayhead::paint(SgPainter& p) {
    const Arrangement* a = arrangement();
    if (!a || !a->playhead()) return;
    const double x = app::roundHalfEven(a->view().beatToX(*a->playhead()));
    if (x < -8 || x > width() + 8) return;
    timeline::drawPlayhead(p, a->view(), *a->playhead(), height(), ruler_);
}

}  // namespace sub::ui
