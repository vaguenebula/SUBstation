#include "pianoroll/PianoKeys.h"

#include "model/Notes.h"
#include "pianoroll/NoteSet.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QMouseEvent>
#include <QWheelEvent>

namespace sub::ui {

using app::Note;
namespace notes = app::notes;

PianoKeys::PianoKeys(QQuickItem* parent) : RollItem(parent) { setAcceptedMouseButtons(Qt::LeftButton); }

void PianoKeys::rollConnected(PianoRoll* roll) {
    connect(roll, &PianoRoll::vscrollChanged, this, &QQuickItem::update);
    connect(roll, &PianoRoll::auditionChanged, this, &QQuickItem::update);
}

void PianoKeys::paint(SgPainter& p) {
    const QRectF rect = p.rect();
    p.fillRect(rect, Theme::kEmptyArea);
    PianoRoll* roll = this->roll();
    if (!roll) return;
    const int height = roll->rowHeight();
    const QFont font = uiFont(7);
    const double blackWidth = width() * 0.6;
    const auto auditioned = roll->auditioned();
    for (int pitch = roll->pitchAt(rect.bottom()); pitch <= roll->pitchAt(rect.top()); ++pitch) {
        const QRectF row(0, roll->pitchTop(pitch), width() - 1, height);
        const bool black = notes::isBlackKey(pitch);
        p.fillRect(row, Theme::kKeyWhite);
        if (black) p.fillRect(QRectF(0, row.top(), blackWidth, height), Theme::kKeyBlack);
        if (auditioned == pitch)
            p.fillRect(QRectF(0, row.top(), black ? blackWidth : row.width(), height), Theme::kAccent);
        if (pitch % 12 == 0 || pitch % 12 == 5)  // the gap between two white keys (B|C, E|F)
            p.fillRect(QRectF(0, row.bottom() - 1, row.width(), 1), Theme::kTextDim);
        if (pitch % 12 == 0) {
            p.drawText(row.adjusted(0, 0, -4, 0), Qt::AlignRight | Qt::AlignVCenter, notes::noteName(pitch),
                       Theme::kKeyLabel, font);
        }
    }
    p.fillRect(QRectF(width() - 1, rect.top(), 1, rect.height()), Theme::kBorder);
}

void PianoKeys::mousePressEvent(QMouseEvent* event) {
    PianoRoll* roll = this->roll();
    if (!roll) return;
    roll->focusGrid();
    const int pitch = roll->pitchAt(event->position().y());
    if (const app::Clip* clip = roll->clip()) {
        std::vector<Note> chosen = event->modifiers() & Qt::ShiftModifier ? roll->selected() : std::vector<Note>();
        for (const Note& note : clip->notes) {
            if (note.pitch == pitch) chosen.push_back(note);
        }
        roll->setSelection(chosen);
    }
    pressed_ = true;
    roll->audition(pitch);
}

void PianoKeys::mouseMoveEvent(QMouseEvent* event) {
    if (pressed_ && roll()) roll()->audition(roll()->pitchAt(event->position().y()));
}

void PianoKeys::mouseReleaseEvent(QMouseEvent*) {
    pressed_ = false;
    if (roll()) roll()->releaseAudition();
}

void PianoKeys::mouseUngrabEvent() {
    if (!pressed_) return;
    pressed_ = false;
    if (roll()) roll()->releaseAudition();
}

void PianoKeys::wheelEvent(QWheelEvent* event) {
    if (roll()) roll()->wheel(event->position(), event->angleDelta(), event->modifiers());
    event->accept();
}

}  // namespace sub::ui
