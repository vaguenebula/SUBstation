#include "pianoroll/PianoRuler.h"

#include "model/Numbers.h"
#include "model/Timebase.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QMouseEvent>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

namespace sub::ui {

PianoRuler::PianoRuler(QQuickItem* parent) : RollItem(parent) { setAcceptedMouseButtons(Qt::LeftButton); }

void PianoRuler::paint(SgPainter& p) {
    const QRectF rect = p.rect();
    const double h = height();
    p.fillRect(rect, Theme::kPanel);
    PianoRoll* roll = this->roll();
    if (!roll) return;
    const timeline::Timeline& view = roll->view();
    if (const app::Clip* clip = roll->clip()) {
        const double x0 = view.beatToX(clip->offsetBeats), x1 = view.beatToX(clip->windowEnd());
        p.fillRect(QRectF(x0, h - 5, x1 - x0, 4), roll->trackColor());
    }
    const double step = view.gridStep();
    const double every = timeline::labelStep(view, step);
    const QFont font = uiFont(8);
    const app::TimeSignature ts = view.timeSignature();
    for (const timeline::GridLine& line : timeline::gridLines(view, rect.left() - 60, rect.right() + 1, step)) {
        const double tick = line.kind == timeline::LineKind::Bar ? 10 : line.kind == timeline::LineKind::Beat ? 6 : 3;
        const double x = app::roundHalfEven(line.x);
        p.fillRect(QRectF(x, h - tick, 1, tick), line.kind == timeline::LineKind::Bar ? Theme::kTextDim : Theme::kGridBar);
        if (std::abs(line.beat / every - std::round(line.beat / every)) < 1e-6)
            p.drawText(QPointF(x + 3, 12), app::formatBarLabel(line.beat, ts), Theme::kText, font);
    }
    p.fillRect(QRectF(rect.left(), h - 1, rect.width(), 1), Theme::kBorder);
    if (const auto start = roll->startBeat()) {
        const double sx = view.beatToX(*start);
        p.fillPolygon(QPolygonF({QPointF(sx - 5, 1), QPointF(sx + 5, 1), QPointF(sx, 8)}), Theme::kInsertMarker);
    }
}

void PianoRuler::mousePressEvent(QMouseEvent* event) {
    PianoRoll* roll = this->roll();
    if (!roll) return;
    roll->focusGrid();
    const QPointF pos = event->position();
    drag_ = Drag{pos.x(), pos.y(), pos.y(), roll->scrollBeats(), roll->view().xToBeat(pos.x())};
}

void PianoRuler::mouseMoveEvent(QMouseEvent* event) {
    PianoRoll* roll = this->roll();
    if (!drag_ || !roll) return;
    Drag& d = *drag_;
    const QPointF pos = event->position();
    const double dx = pos.x() - d.x, dy = pos.y() - d.y;
    if (!d.moved && std::max(std::abs(dx), std::abs(dy)) > 3) {
        d.moved = true;
        d.pan = std::abs(dx) > std::abs(dy);
    }
    if (d.pan) {
        roll->setScrollBeats(d.scroll - dx / roll->pxPerBeat());
    } else if (d.moved) {
        roll->zoomAt(d.x, std::pow(1.012, pos.y() - d.lastY));
        d.lastY = pos.y();
    }
}

void PianoRuler::mouseReleaseEvent(QMouseEvent* event) {
    const std::optional<Drag> d = drag_;
    drag_.reset();
    PianoRoll* roll = this->roll();
    if (d && !d->moved && roll && roll->clip()) {
        const bool bypass = event->modifiers() & Qt::AltModifier;
        roll->requestLocate(roll->view().snapBeat(d->beat, bypass));
    }
}

void PianoRuler::mouseUngrabEvent() { drag_.reset(); }

}  // namespace sub::ui
