#include "arrangement/ArrangementPlayhead.h"

#include "model/Numbers.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QPolygonF>

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
    const double h = height();
    if (x < -8 || x > width() + 8) return;
    if (ruler_) {
        p.fillPolygon(QPolygonF({QPointF(x - 5, h - 8), QPointF(x + 6, h - 8), QPointF(x + 0.5, h - 1)}),
                      Theme::kPlayhead);
    } else {
        p.fillRect(QRectF(x, 0, 1, h), Theme::kPlayhead);
    }
}

}  // namespace sub::ui
