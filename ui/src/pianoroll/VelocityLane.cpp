#include "pianoroll/VelocityLane.h"

#include "input/GestureKey.h"
#include "model/Notes.h"
#include "model/Numbers.h"
#include "pianoroll/NoteSet.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QHoverEvent>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace sub::ui {

using app::Note;
namespace notes = app::notes;

VelocityLane::VelocityLane(QQuickItem* parent) : RollItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
}

std::optional<ClipNote> VelocityLane::stemAt(double x) const {
    PianoRoll* roll = this->roll();
    if (!roll || !roll->hasClip()) return std::nullopt;
    // The nearest (distance, unselected first), as min() over the tuples did.
    std::optional<std::tuple<double, bool, ClipNote>> best;
    roll->forEachNote([&](int clip, const Note& n) {
        const ClipNote note{clip, n};
        const double distance = std::abs(roll->view().beatToX(roll->rollStart(note)) - x);
        if (distance > kStemGrab) return;
        const bool unselected = !roll->isSelected(note);
        if (!best || std::tie(distance, unselected) < std::tie(std::get<0>(*best), std::get<1>(*best)))
            best = std::make_tuple(distance, unselected, note);
    });
    if (!best) return std::nullopt;
    return std::get<2>(*best);
}

void VelocityLane::paint(SgPainter& p) {
    const QRectF visible = p.rect();
    const double h = height();
    p.fillRect(visible, Theme::lane());
    PianoRoll* roll = this->roll();
    if (!roll) return;
    const timeline::Timeline& view = roll->view();
    timeline::drawGrid(p, view, visible.left(), visible.right(), 1, h);
    if (roll->hasClip()) {
        // Dimmed outside the parts the clips play.
        std::vector<PianoRoll::Span> lit = roll->windows();
        std::sort(lit.begin(), lit.end());
        double from = visible.left();
        for (const auto& [start, end] : lit) {
            const double x0 = view.beatToX(start), x1 = view.beatToX(end);
            if (x0 > from) p.fillRect(QRectF(from, 0, x0 - from, h), Theme::outsideClip());
            from = std::max(from, x1);
        }
        if (from < visible.right() + 1) p.fillRect(QRectF(from, 0, visible.right() + 1 - from, h), Theme::outsideClip());
        std::vector<QColor> colors;
        for (int i = 0; i < roll->clipCount(); ++i) colors.push_back(roll->colorOf(i));
        const double bottom = h - kMarginBottom;
        const QFont font = uiFont(7);
        roll->forEachNote([&](int clip, const Note& note) {
            const ClipNote clipNote{clip, note};
            const double x = app::roundHalfEven(view.beatToX(roll->rollStart(clipNote)));
            if (x < visible.left() - 3 || x > visible.right() + 3) return;
            const bool selected = roll->isSelected(clipNote);
            const QColor stem = selected     ? Theme::selectionOutline()
                                : note.muted ? Theme::deactivatedClip()
                                             : colors[static_cast<size_t>(clipNote.clip)];
            const double y = velocityY(note.velocity);
            p.fillRect(QRectF(x, y, 1, bottom - y), stem);
            p.fillRect(QRectF(x - 2, y - 2, 5, 5), stem);
            if (selected && drag_)
                p.drawText(QRectF(x + 5, y - 7, 30, 12), Qt::AlignLeft, QString::number(note.velocity), Theme::text(),
                           font);
        });
    }
    p.fillRect(QRectF(visible.left(), 0, visible.width(), 1), Theme::border());
    if (const auto start = roll->startBeat())
        p.fillRect(QRectF(app::roundHalfEven(view.beatToX(*start)), 0, 1, h), Theme::insertMarker());
}

void VelocityLane::mousePressEvent(QMouseEvent* event) {
    PianoRoll* roll = this->roll();
    if (!roll) return;
    roll->focusGrid();
    const auto note = stemAt(event->position().x());
    if (!note) return;
    if (!roll->isSelected(*note)) roll->selectNotes({*note});
    std::map<int, std::vector<Note>> base;
    for (const int index : roll::clipsOf(roll->selectedNotes())) {
        if (const app::Clip* clip = roll->clipAt(index)) base[index] = clip->notes;
    }
    drag_ = Drag{event->position().y(), std::move(base), newGestureKey(), roll->selectedNotes()};
    update();
}

void VelocityLane::mouseMoveEvent(QMouseEvent* event) {
    PianoRoll* roll = this->roll();
    if (!drag_ || !roll) return;
    const double delta = (drag_->y - event->position().y()) / span() * 127;
    std::map<int, std::vector<Note>> changes;
    std::vector<ClipNote> changed;
    for (const auto& [index, base] : drag_->base) {
        const std::vector<Note> mine = roll::notesOf(drag_->targets, index);
        const std::vector<Note> louder = notes::withVelocity(mine, delta);
        changes[index] = notes::place(base, mine, louder);
        for (const Note& note : louder) changed.push_back({index, note});
    }
    roll->commitNotes(changes, QStringLiteral("Change Velocity"), drag_->key, changed);
}

void VelocityLane::mouseReleaseEvent(QMouseEvent*) {
    drag_.reset();
    update();
}

void VelocityLane::mouseUngrabEvent() {
    drag_.reset();
    update();
}

void VelocityLane::hoverEnterEvent(QHoverEvent* event) { hoverMoveEvent(event); }  // (entering right by a stem)

void VelocityLane::hoverMoveEvent(QHoverEvent* event) {
    if (drag_) return;
    setCursor(stemAt(event->position().x()) ? Qt::SizeVerCursor : Qt::ArrowCursor);
}

}  // namespace sub::ui
