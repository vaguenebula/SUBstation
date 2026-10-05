#include "pianoroll/NoteGrid.h"

#include "model/Notes.h"
#include "model/Numbers.h"
#include "pianoroll/NoteSet.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QUuid>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace sub::ui {

using app::Note;
namespace notes = app::notes;

// --- Gestures ------------------------------------------------------------------------

// A mouse drag in the note grid. Moves and resizes edit the clip live, as one
// undo step (the gesture's merge key).
class NoteGrid::Gesture {
public:
    Gesture(NoteGrid* grid, const QPointF& press)
        : grid(grid), roll(grid->roll()), press(press), key(QUuid::createUuid().toString()) {}
    virtual ~Gesture() = default;

    // Past the drag threshold yet (from then on, it is active).
    bool started(const QPointF& pos) {
        if (!active && (pos - press).manhattanLength() >= kDragThreshold) active = true;
        return active;
    }
    virtual void move(const QPointF& pos, Qt::KeyboardModifiers modifiers) = 0;
    virtual std::optional<QRectF> rubberBand() const { return std::nullopt; }
    virtual void finish() {}

    NoteGrid* grid;
    PianoRoll* roll;
    QPointF press;
    bool active = false;
    QString key;
};

namespace {

class MoveNotesGesture : public NoteGrid::Gesture {
public:
    MoveNotesGesture(NoteGrid* grid, const QPointF& press, const Note& grabbed, std::vector<Note> moving)
        : Gesture(grid, press),
          grabbed_(grabbed),
          moving_(std::move(moving)),
          base_(roll->clip()->notes),
          originBeat_(roll->view().xToBeat(press.x())),
          originPitch_(roll->pitchAt(press.y())) {}

    void move(const QPointF& pos, Qt::KeyboardModifiers modifiers) override {
        if (!started(pos)) return;
        const roll::Timeline& view = roll->view();
        const bool bypass = modifiers & Qt::AltModifier;
        const double raw = view.xToBeat(pos.x()) - originBeat_;
        const double deltaBeats = view.snapBeat(grabbed_.start + raw, bypass) - grabbed_.start;
        const auto [beats, pitch] = notes::clampMove(moving_, deltaBeats, roll->pitchAt(pos.y()) - originPitch_);
        const bool copy = modifiers & Qt::ControlModifier;
        const std::vector<Note> moved = notes::shifted(moving_, beats, pitch);
        // From the notes as they were at the press, or a drag would compound.
        roll->commit(notes::place(base_, copy ? std::vector<Note>() : moving_, moved),
                     copy ? QStringLiteral("Copy Notes") : QStringLiteral("Move Notes"), key, moved);
        roll->audition(grabbed_.pitch + pitch, grabbed_.velocity);
    }

private:
    Note grabbed_;
    std::vector<Note> moving_;
    std::vector<Note> base_;
    double originBeat_;
    int originPitch_;
};

class ResizeNotesGesture : public NoteGrid::Gesture {
public:
    ResizeNotesGesture(NoteGrid* grid, const QPointF& press, const Note& grabbed, std::vector<Note> moving,
                       notes::Edge edge)
        : Gesture(grid, press),
          grabbed_(grabbed),
          moving_(std::move(moving)),
          edge_(edge),
          base_(roll->clip()->notes),
          originBeat_(roll->view().xToBeat(press.x())) {}

    void move(const QPointF& pos, Qt::KeyboardModifiers modifiers) override {
        if (!started(pos)) return;
        const roll::Timeline& view = roll->view();
        const bool bypass = modifiers & Qt::AltModifier;
        const double edgeBeat = edge_ == notes::Edge::End ? grabbed_.end() : grabbed_.start;
        const double delta = view.snapBeat(edgeBeat + view.xToBeat(pos.x()) - originBeat_, bypass) - edgeBeat;
        const double minLength = view.snap() && !bypass ? view.gridStep() : notes::kMinNoteBeats;
        const std::vector<Note> resized = notes::resized(moving_, edge_, delta, minLength);
        roll->commit(notes::place(base_, moving_, resized), QStringLiteral("Resize Notes"), key, resized);
    }

private:
    Note grabbed_;
    std::vector<Note> moving_;
    notes::Edge edge_;
    std::vector<Note> base_;
    double originBeat_;
};

// Rubber band: selects the notes it touches (added to the selection with Ctrl/Shift).
class SelectNotesGesture : public NoteGrid::Gesture {
public:
    SelectNotesGesture(NoteGrid* grid, const QPointF& press, bool additive)
        : Gesture(grid, press), base_(additive ? roll->selected() : std::vector<Note>()) {}

    void move(const QPointF& pos, Qt::KeyboardModifiers) override {
        if (!started(pos)) return;
        rect_ = QRectF(press, pos).normalized();
        std::vector<Note> hits;
        if (const app::Clip* clip = roll->clip()) {
            for (const Note& note : clip->notes) {
                if (roll->noteRect(note).intersects(*rect_)) hits.push_back(note);
            }
        }
        roll->setSelection(roll::united(base_, hits));
    }

    std::optional<QRectF> rubberBand() const override { return rect_; }

    void finish() override {
        if (active) roll->setSelection(roll->selected(), true);  // a group chosen by dragging: the note tools come up
    }

private:
    std::vector<Note> base_;
    std::optional<QRectF> rect_;
};

// Ctrl+Alt drag: scroll the view in both directions, as in the arrangement.
class PanGesture : public NoteGrid::Gesture {
public:
    PanGesture(NoteGrid* grid, const QPointF& press)
        : Gesture(grid, press), scrollBeats_(roll->scrollBeats()), scrollY_(roll->scrollY()) {}

    void move(const QPointF& pos, Qt::KeyboardModifiers) override {
        const QPointF delta = pos - press;
        roll->setScrollBeats(scrollBeats_ - delta.x() / roll->pxPerBeat());
        roll->setScrollY(scrollY_ - delta.y());
    }

private:
    double scrollBeats_;
    int scrollY_;
};

// The modifiers held once a key event is through: a modifier key's own press
// or release isn't in its event's modifiers on every platform (X11 reports the
// state before it).
Qt::KeyboardModifiers heldModifiers(const QKeyEvent* event) {
    Qt::KeyboardModifiers modifiers = event->modifiers();
    Qt::KeyboardModifier own = Qt::NoModifier;
    switch (event->key()) {
    case Qt::Key_Control: own = Qt::ControlModifier; break;
    case Qt::Key_Alt: own = Qt::AltModifier; break;
    case Qt::Key_Shift: own = Qt::ShiftModifier; break;
    case Qt::Key_Meta: own = Qt::MetaModifier; break;
    default: break;
    }
    if (own != Qt::NoModifier) modifiers.setFlag(own, event->type() == QEvent::KeyPress);
    return modifiers;
}

}  // namespace

// --- NoteGrid ------------------------------------------------------------------------

NoteGrid::NoteGrid(QQuickItem* parent) : RollItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
    setActiveFocusOnTab(true);
}

NoteGrid::~NoteGrid() = default;

bool NoteGrid::isPanModifier(Qt::KeyboardModifiers modifiers) {
    return (modifiers & Qt::ControlModifier) && (modifiers & Qt::AltModifier);
}

void NoteGrid::rollConnected(PianoRoll* roll) {
    connect(roll, &PianoRoll::vscrollChanged, this, &QQuickItem::update);
    roll->setGrid(this);
}

std::optional<NoteGrid::Hit> NoteGrid::noteAt(const QPointF& pos) const {
    const app::Clip* clip = roll() ? roll()->clip() : nullptr;
    if (!clip) return std::nullopt;
    for (auto it = clip->notes.rbegin(); it != clip->notes.rend(); ++it) {
        const QRectF rect = roll()->noteRect(*it);
        if (rect.left() <= pos.x() && pos.x() <= rect.right() && rect.top() <= pos.y() && pos.y() < rect.bottom()) {
            const double grab = std::min(kEdgeGrab, rect.width() / 4);
            if (pos.x() >= rect.right() - grab) return Hit{*it, Zone::End};
            if (pos.x() <= rect.left() + grab) return Hit{*it, Zone::Start};
            return Hit{*it, Zone::Body};
        }
    }
    return std::nullopt;
}

bool NoteGrid::dragging() const { return gesture_ && gesture_->active; }

std::optional<QRectF> NoteGrid::rubberBand() const { return gesture_ ? gesture_->rubberBand() : std::nullopt; }

// --- Painting ----------------------------------------------------------------------------

void NoteGrid::paint(SgPainter& p) {
    const QRectF visible = p.rect();
    p.fillRect(visible, Theme::kLane);
    PianoRoll* roll = this->roll();
    if (!roll) return;
    const roll::Timeline& view = roll->view();
    const int height = roll->rowHeight();
    for (int pitch = roll->pitchAt(visible.bottom()); pitch <= roll->pitchAt(visible.top()); ++pitch) {
        const double top = roll->pitchTop(pitch);
        if (notes::isBlackKey(pitch))
            p.fillRect(QRectF(visible.left(), top, visible.width(), height), Theme::kBlackKeyRow);
        if (pitch % 12 == 0 || pitch % 12 == 5) {  // octave (B|C) and E|F lines
            p.fillRect(QRectF(visible.left(), top + height - 1, visible.width(), 1),
                       pitch % 12 == 0 ? Theme::kGridBar : Theme::kGridSub);
        }
    }
    const double bottom = std::min(visible.bottom(), roll->pitchTop(0) + height);
    roll::drawGrid(p, view, visible.left(), visible.right(), visible.top(), bottom);
    if (bottom < visible.bottom())
        p.fillRect(QRectF(visible.left(), bottom, visible.width(), visible.bottom() - bottom), Theme::kEmptyArea);

    if (const app::Clip* clip = roll->clip()) {
        const double x0 = view.beatToX(clip->offsetBeats), x1 = view.beatToX(clip->windowEnd());
        if (x0 > visible.left())
            p.fillRect(QRectF(visible.left(), visible.top(), x0 - visible.left(), visible.height()),
                       Theme::kOutsideClip);
        if (x1 < visible.right())
            p.fillRect(QRectF(x1, visible.top(), visible.right() - x1 + 1, visible.height()), Theme::kOutsideClip);
        const QColor color = roll->trackColor();
        const QFont font = uiFont(7);
        for (const Note& note : clip->notes) {
            const QRectF rect = roll->noteRect(note);
            if (rect.intersects(visible)) {
                const bool playing = clip->offsetBeats <= note.start && note.start < clip->windowEnd();
                drawNote(p, note, rect, color, font, roll->isSelected(note), playing);
            }
        }
    }

    if (const auto band = rubberBand()) {
        p.fillRect(*band, Theme::kRubberBand);
        p.drawRect(*band, Theme::kAccent, 1);
    }
    if (const auto start = roll->startBeat())
        p.fillRect(QRectF(app::roundHalfEven(view.beatToX(*start)), 0, 1, this->height()), Theme::kInsertMarker);
}

void NoteGrid::drawNote(SgPainter& p, const Note& note, const QRectF& rect, const QColor& color, const QFont& font,
                        bool selected, bool playing) const {
    // Brighter for louder notes, as in Ableton; faint outside the part the clip plays.
    QColor fill = selected ? color.lighter(135) : color;
    fill.setAlphaF(static_cast<float>((0.35 + 0.65 * note.velocity / 127) * (playing ? 1.0 : 0.5)));
    const QRectF box = rect.adjusted(0.5, 0.5, -0.5, -0.5);
    p.fillRect(box, fill);
    p.drawRect(box, selected ? Theme::kSelectionOutline : color.darker(220), 1);
    if (rect.width() >= 30 && rect.height() >= 10) {
        p.drawText(box.adjusted(3, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft, notes::noteName(note.pitch),
                   Theme::kAccentText, font);
    }
}

// --- Mouse -------------------------------------------------------------------------------

void NoteGrid::mousePressEvent(QMouseEvent* event) {
    // The press a double-click starts with: the double-click stands for it, as
    // with widgets (which never see it).
    if (event->flags() & Qt::MouseEventCreatedDoubleClick) return;
    forceActiveFocus(Qt::MouseFocusReason);
    PianoRoll* roll = this->roll();
    if (!roll) return;
    const QPointF pos = event->position();
    const Qt::KeyboardModifiers mods = event->modifiers();
    if (isPanModifier(mods)) {
        gesture_ = std::make_unique<PanGesture>(this, pos);
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    const bool additive = mods & (Qt::ControlModifier | Qt::ShiftModifier);
    const auto hit = noteAt(pos);
    if (!hit) {
        if (!additive) roll->setSelection({});
        gesture_ = std::make_unique<SelectNotesGesture>(this, pos, additive);
        return;
    }
    const Note note = hit->note;
    // Clicking one of several selected notes selects just it, and Ctrl-clicking
    // a selected note deselects it, but only once the mouse comes up without
    // dragging: dragging moves (Ctrl: copies) the whole selection instead.
    selectOnClick_.reset();
    if (roll->isSelected(note)) {
        if (mods & Qt::ShiftModifier) {
        } else if (mods & Qt::ControlModifier) {
            selectOnClick_ = roll::without(roll->selected(), note);
        } else if (roll->selected().size() > 1) {
            selectOnClick_ = std::vector<Note>{note};
        }
    } else {
        roll->setSelection(roll::united(additive ? roll->selected() : std::vector<Note>(), {note}));
    }
    roll->audition(note.pitch, note.velocity);
    std::vector<Note> moving = roll::byTime(roll->selected());
    if (hit->zone == Zone::Body) {
        gesture_ = std::make_unique<MoveNotesGesture>(this, pos, note, std::move(moving));
    } else {
        gesture_ = std::make_unique<ResizeNotesGesture>(
            this, pos, note, std::move(moving), hit->zone == Zone::End ? notes::Edge::End : notes::Edge::Start);
    }
}

void NoteGrid::mouseMoveEvent(QMouseEvent* event) {
    if (gesture_) {
        gesture_->move(event->position(), event->modifiers());
        update();
        return;
    }
    updateCursor(event->position(), event->modifiers());
}

void NoteGrid::mouseReleaseEvent(QMouseEvent* event) {
    const std::unique_ptr<Gesture> gesture = std::move(gesture_);
    PianoRoll* roll = this->roll();
    if (!roll) return;
    if (selectOnClick_ && gesture && !gesture->active) roll->setSelection(*selectOnClick_);
    selectOnClick_.reset();
    if (gesture) gesture->finish();
    roll->releaseAudition();
    updateCursor(event->position(), event->modifiers());
    roll->placeTools();
    update();
}

void NoteGrid::mouseUngrabEvent() {
    // The mouse was taken away mid-gesture (a popup): it ends where it is.
    if (!gesture_ && !selectOnClick_) return;
    gesture_.reset();
    selectOnClick_.reset();
    if (PianoRoll* roll = this->roll()) {
        roll->releaseAudition();
        roll->placeTools();
    }
    update();
}

void NoteGrid::mouseDoubleClickEvent(QMouseEvent* event) {
    gesture_.reset();  // (where the platform didn't mark the press the double-click started with)
    selectOnClick_.reset();
    PianoRoll* roll = this->roll();
    const app::Clip* clip = roll ? roll->clip() : nullptr;
    if (event->button() != Qt::LeftButton || !clip || isPanModifier(event->modifiers())) return;
    forceActiveFocus(Qt::MouseFocusReason);
    const QPointF pos = event->position();
    if (const auto hit = noteAt(pos)) {
        roll->setToolsWanted(false);
        roll->commit(notes::place(clip->notes, {hit->note}, {}), QStringLiteral("Delete Note"), {},
                     roll::without(roll->selected(), hit->note));
        return;
    }
    const roll::Timeline& view = roll->view();
    const double step = view.gridStep();
    double beat = std::max(0.0, view.xToBeat(pos.x()));
    if (view.snap() && !(event->modifiers() & Qt::AltModifier))
        beat = std::floor(beat / step + 1e-9) * step;  // the grid cell that was clicked
    Note note;
    note.pitch = roll->pitchAt(pos.y());
    note.start = beat;
    note.length = step;
    roll->setToolsWanted(false);
    roll->commit(notes::place(clip->notes, {}, {note}), QStringLiteral("Add Note"), {}, std::vector<Note>{note});
    roll->audition(note.pitch, note.velocity);
}

void NoteGrid::hoverEnterEvent(QHoverEvent* event) {
    hover_ = event->position();
    if (!gesture_) updateCursor(event->position(), event->modifiers());
}

void NoteGrid::hoverMoveEvent(QHoverEvent* event) {
    hover_ = event->position();
    if (!gesture_) updateCursor(event->position(), event->modifiers());
}

void NoteGrid::hoverLeaveEvent(QHoverEvent*) { hover_.reset(); }

void NoteGrid::updateCursor(const QPointF& pos, Qt::KeyboardModifiers mods) {
    const auto hit = noteAt(pos);
    if (isPanModifier(mods))
        setCursor(Qt::OpenHandCursor);
    else if (!hit)
        setCursor(Qt::ArrowCursor);
    else if (hit->zone == Zone::Body)
        setCursor(Qt::PointingHandCursor);
    else
        setCursor(Qt::SizeHorCursor);
}

void NoteGrid::onModifiers(Qt::KeyboardModifiers mods) {
    if (!gesture_ && hover_) updateCursor(*hover_, mods);
}

void NoteGrid::wheelEvent(QWheelEvent* event) {
    if (PianoRoll* roll = this->roll()) roll->wheel(event->position(), event->angleDelta(), event->modifiers());
    event->accept();
}

// --- Keys -------------------------------------------------------------------------------

bool NoteGrid::handles(const QKeyEvent* event) {
    const int key = event->key();
    if (event->modifiers() & Qt::ControlModifier && (key == Qt::Key_A || key == Qt::Key_D || key == Qt::Key_U))
        return true;
    return key == Qt::Key_Delete || key == Qt::Key_Backspace || key == Qt::Key_Up || key == Qt::Key_Down ||
           key == Qt::Key_Left || key == Qt::Key_Right;
}

bool NoteGrid::event(QEvent* event) {
    // Take Delete, Ctrl+A, Ctrl+D (and the rest) from the main window's shortcuts.
    if (event->type() == QEvent::ShortcutOverride && handles(static_cast<QKeyEvent*>(event))) {
        event->accept();
        return true;
    }
    return RollItem::event(event);
}

void NoteGrid::keyPressEvent(QKeyEvent* event) {
    onModifiers(heldModifiers(event));
    PianoRoll* roll = this->roll();
    const app::Clip* clip = roll ? roll->clip() : nullptr;
    if (!clip || !handles(event)) {
        event->ignore();
        return;
    }
    const int key = event->key();
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    const std::vector<Note> selected = roll::byTime(roll->selected());
    if (key == Qt::Key_A) {
        roll->setSelection(clip->notes, true);
    } else if (key == Qt::Key_U) {
        roll->quantize();  // the selected notes, or all
    } else if (selected.empty()) {
    } else if (key == Qt::Key_Delete || key == Qt::Key_Backspace) {
        roll->commit(notes::place(clip->notes, selected, {}), QStringLiteral("Delete Notes"), {}, std::vector<Note>());
    } else if (key == Qt::Key_D) {
        const auto [start, end] = notes::span(selected);
        const std::vector<Note> copies = notes::shifted(selected, end - start, 0);
        roll->commit(notes::place(clip->notes, {}, copies), QStringLiteral("Duplicate Notes"), {}, copies);
    } else {
        double deltaBeats = 0.0;
        int deltaPitch = 0;
        if (key == Qt::Key_Up || key == Qt::Key_Down) {
            deltaPitch = (shift ? 12 : 1) * (key == Qt::Key_Up ? 1 : -1);
        } else {
            const double step = shift ? roll->view().timeSignature().beatsPerBar() : roll->view().gridStep();
            deltaBeats = step * (key == Qt::Key_Right ? 1 : -1);
        }
        const auto [beats, pitch] = notes::clampMove(selected, deltaBeats, deltaPitch);
        if (beats != 0.0 || pitch != 0) {
            const std::vector<Note> moved = notes::shifted(selected, beats, pitch);
            roll->commit(notes::place(clip->notes, selected, moved),
                         pitch ? QStringLiteral("Transpose Notes") : QStringLiteral("Move Notes"), {}, moved);
        }
    }
    event->accept();
}

void NoteGrid::keyReleaseEvent(QKeyEvent* event) {
    onModifiers(heldModifiers(event));
    event->ignore();
}

// --- Size and visibility --------------------------------------------------------------------

void NoteGrid::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    RollItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size() && roll()) roll()->gridGeometryChanged();
}

void NoteGrid::itemChange(ItemChange change, const ItemChangeData& value) {
    RollItem::itemChange(change, value);
    if (change != ItemVisibleHasChanged) return;
    // Hidden notes don't keep the keyboard (as a hidden widget doesn't): the
    // window's shortcuts for Delete, Ctrl+A and Ctrl+D work again.
    if (!value.boolValue && hasFocus()) setFocus(false);
    if (roll()) roll()->gridVisibilityChanged(value.boolValue);
}

}  // namespace sub::ui
