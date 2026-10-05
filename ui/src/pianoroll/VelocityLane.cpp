#include "pianoroll/VelocityLane.h"

#include "model/Notes.h"
#include "model/Numbers.h"
#include "pianoroll/NoteSet.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QUuid>

#include <cmath>
#include <tuple>

namespace sub::ui {

using app::Note;
namespace notes = app::notes;

VelocityLane::VelocityLane(QQuickItem* parent) : RollItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
}

std::optional<Note> VelocityLane::stemAt(double x) const {
    PianoRoll* roll = this->roll();
    const app::Clip* clip = roll ? roll->clip() : nullptr;
    if (!clip) return std::nullopt;
    // The nearest (distance, unselected first), as min() over the tuples did.
    std::optional<std::tuple<double, bool, Note>> best;
    for (const Note& note : clip->notes) {
        const double distance = std::abs(roll->view().beatToX(note.start) - x);
        if (distance > kStemGrab) continue;
        const bool unselected = !roll->isSelected(note);
        if (!best || std::tie(distance, unselected) < std::tie(std::get<0>(*best), std::get<1>(*best)))
            best = std::make_tuple(distance, unselected, note);
    }
    if (!best) return std::nullopt;
    return std::get<2>(*best);
}

void VelocityLane::paint(SgPainter& p) {
    const QRectF visible = p.rect();
    const double h = height();
    p.fillRect(visible, Theme::kLane);
    PianoRoll* roll = this->roll();
    if (!roll) return;
    const timeline::Timeline& view = roll->view();
    timeline::drawGrid(p, view, visible.left(), visible.right(), 1, h);
    if (const app::Clip* clip = roll->clip()) {
        const double x0 = view.beatToX(clip->offsetBeats), x1 = view.beatToX(clip->windowEnd());
        p.fillRect(QRectF(visible.left(), 0, std::max(0.0, x0 - visible.left()), h), Theme::kOutsideClip);
        p.fillRect(QRectF(x1, 0, std::max(0.0, visible.right() - x1 + 1), h), Theme::kOutsideClip);
        const QColor color = roll->trackColor();
        const double bottom = h - kMarginBottom;
        const QFont font = uiFont(7);
        for (const Note& note : clip->notes) {
            const double x = app::roundHalfEven(view.beatToX(note.start));
            if (x < visible.left() - 3 || x > visible.right() + 3) continue;
            const bool selected = roll->isSelected(note);
            const QColor stem = selected ? Theme::kSelectionOutline : color;
            const double y = velocityY(note.velocity);
            p.fillRect(QRectF(x, y, 1, bottom - y), stem);
            p.fillRect(QRectF(x - 2, y - 2, 5, 5), stem);
            if (selected && drag_)
                p.drawText(QRectF(x + 5, y - 7, 30, 12), Qt::AlignLeft, QString::number(note.velocity), Theme::kText,
                           font);
        }
    }
    p.fillRect(QRectF(visible.left(), 0, visible.width(), 1), Theme::kBorder);
    if (const auto start = roll->startBeat())
        p.fillRect(QRectF(app::roundHalfEven(view.beatToX(*start)), 0, 1, h), Theme::kInsertMarker);
}

void VelocityLane::mousePressEvent(QMouseEvent* event) {
    PianoRoll* roll = this->roll();
    if (!roll) return;
    roll->focusGrid();
    const app::Clip* clip = roll->clip();
    const auto note = stemAt(event->position().x());
    if (!clip || !note) return;
    if (!roll->isSelected(*note)) roll->setSelection({*note});
    drag_ = Drag{event->position().y(), roll->clip()->notes, QUuid::createUuid().toString(),
                 roll::byTime(roll->selected())};
    update();
}

void VelocityLane::mouseMoveEvent(QMouseEvent* event) {
    PianoRoll* roll = this->roll();
    if (!drag_ || !roll) return;
    const double delta = (drag_->y - event->position().y()) / span() * 127;
    const std::vector<Note> changed = notes::withVelocity(drag_->targets, delta);
    roll->commit(notes::place(drag_->base, drag_->targets, changed), QStringLiteral("Change Velocity"), drag_->key,
                 changed);
}

void VelocityLane::mouseReleaseEvent(QMouseEvent*) {
    drag_.reset();
    update();
}

void VelocityLane::mouseUngrabEvent() {
    drag_.reset();
    update();
}

void VelocityLane::hoverMoveEvent(QHoverEvent* event) {
    if (drag_) return;
    setCursor(stemAt(event->position().x()) ? Qt::SizeVerCursor : Qt::ArrowCursor);
}

}  // namespace sub::ui
