#include "pianoroll/NoteGrid.h"

#include "input/GestureKey.h"
#include "input/Modifiers.h"
#include "model/Notes.h"
#include "model/Numbers.h"
#include "pianoroll/NoteSet.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace sub::ui {

using app::Note;
namespace notes = app::notes;

// --- Gestures ------------------------------------------------------------------------

// A mouse drag in the note grid. Moves and resizes edit the clips live, as one
// undo step (the gesture's merge key).
class NoteGrid::Gesture {
public:
    Gesture(NoteGrid* grid, const QPointF& press)
        : grid(grid), roll(grid->roll()), press(press), key(newGestureKey()) {}
    virtual ~Gesture() = default;

    // Past the drag threshold yet (from then on, it is active).
    bool started(const QPointF& pos) {
        if (!active && (pos - press).manhattanLength() >= kDragThreshold) active = true;
        return active;
    }
    virtual void move(const QPointF& pos, Qt::KeyboardModifiers modifiers) = 0;
    virtual std::optional<QRectF> rubberBand() const { return std::nullopt; }
    virtual void finish() {}
    // The mouse came up without dragging.
    virtual void clicked(Qt::KeyboardModifiers) {}

    NoteGrid* grid;
    PianoRoll* roll;
    QPointF press;
    bool active = false;
    QString key;
};

namespace {

// Each clip's notes now, for the clips of `notes`.
std::map<int, std::vector<Note>> clipNotes(const PianoRoll* roll, const std::vector<ClipNote>& notes) {
    std::map<int, std::vector<Note>> result;
    for (const int index : roll::clipsOf(notes)) {
        if (const app::Clip* clip = roll->clipAt(index)) result[index] = clip->notes;
    }
    return result;
}

// How far `notes` can move together (none before its clip's content start, nor off the keyboard).
std::pair<double, int> clampMove(const std::vector<ClipNote>& notes, double deltaBeats, int deltaPitch) {
    for (const int index : roll::clipsOf(notes))
        std::tie(deltaBeats, deltaPitch) = notes::clampMove(roll::notesOf(notes, index), deltaBeats, deltaPitch);
    return {deltaBeats, deltaPitch};
}

// Each clip's notes among `notes`, by time.
std::map<int, std::vector<Note>> byClip(const std::vector<ClipNote>& notes) {
    std::map<int, std::vector<Note>> result;
    for (const int index : roll::clipsOf(notes)) result[index] = roll::notesOf(notes, index);
    return result;
}

class MoveNotesGesture : public NoteGrid::Gesture {
public:
    MoveNotesGesture(NoteGrid* grid, const QPointF& press, const ClipNote& grabbed, std::vector<ClipNote> moving)
        : Gesture(grid, press),
          grabbed_(grabbed),
          moving_(std::move(moving)),
          mine_(byClip(moving_)),
          base_(clipNotes(roll, moving_)),
          originBeat_(roll->view().xToBeat(press.x())),
          originPitch_(roll->pitchAt(press.y())) {}

    void move(const QPointF& pos, Qt::KeyboardModifiers modifiers) override {
        if (!started(pos)) return;
        const timeline::Timeline& view = roll->view();
        const bool bypass = modifiers & Qt::AltModifier;
        const double raw = view.xToBeat(pos.x()) - originBeat_;
        const double grabbedAt = roll->rollStart(grabbed_);
        const double deltaBeats = view.snapBeat(grabbedAt + raw, bypass) - grabbedAt;
        const auto [beats, pitch] = clampMove(moving_, deltaBeats, roll->pitchAt(pos.y()) - originPitch_);
        const bool copy = modifiers & Qt::ControlModifier;
        // From the notes as they were at the press, or a drag would compound.
        std::map<int, std::vector<Note>> changes;
        std::vector<ClipNote> moved;
        for (const auto& [index, base] : base_) {
            const std::vector<Note>& mine = mine_[index];
            const std::vector<Note> shifted = notes::shifted(mine, beats, pitch);
            changes[index] = notes::place(base, copy ? std::vector<Note>() : mine, shifted);
            for (const Note& note : shifted) moved.push_back({index, note});
        }
        roll->commitNotes(changes, copy ? QStringLiteral("Copy Notes") : QStringLiteral("Move Notes"), key, moved);
        roll->audition(grabbed_.note.pitch + pitch, grabbed_.note.velocity, grabbed_.clip);
    }

private:
    ClipNote grabbed_;
    std::vector<ClipNote> moving_;
    std::map<int, std::vector<Note>> mine_;  // each clip's moving notes
    std::map<int, std::vector<Note>> base_;
    double originBeat_;
    int originPitch_;
};

class ResizeNotesGesture : public NoteGrid::Gesture {
public:
    ResizeNotesGesture(NoteGrid* grid, const QPointF& press, const ClipNote& grabbed, std::vector<ClipNote> moving,
                       notes::Edge edge)
        : Gesture(grid, press),
          grabbed_(grabbed),
          moving_(std::move(moving)),
          edge_(edge),
          mine_(byClip(moving_)),
          base_(clipNotes(roll, moving_)),
          originBeat_(roll->view().xToBeat(press.x())) {}

    void move(const QPointF& pos, Qt::KeyboardModifiers modifiers) override {
        if (!started(pos)) return;
        const timeline::Timeline& view = roll->view();
        const bool bypass = modifiers & Qt::AltModifier;
        const double start = roll->rollStart(grabbed_);
        const double edgeBeat = edge_ == notes::Edge::End ? start + grabbed_.note.length : start;
        const double delta = view.snapBeat(edgeBeat + view.xToBeat(pos.x()) - originBeat_, bypass) - edgeBeat;
        const double minLength = view.snap() && !bypass ? view.gridStep() : notes::kMinNoteBeats;
        std::map<int, std::vector<Note>> changes;
        std::vector<ClipNote> resized;
        for (const auto& [index, base] : base_) {
            const std::vector<Note>& mine = mine_[index];
            const std::vector<Note> changed = notes::resized(mine, edge_, delta, minLength);
            changes[index] = notes::place(base, mine, changed);
            for (const Note& note : changed) resized.push_back({index, note});
        }
        roll->commitNotes(changes, QStringLiteral("Resize Notes"), key, resized);
    }

private:
    ClipNote grabbed_;
    std::vector<ClipNote> moving_;
    notes::Edge edge_;
    std::map<int, std::vector<Note>> mine_;  // each clip's resized notes
    std::map<int, std::vector<Note>> base_;
    double originBeat_;
};

// Rubber band: selects the notes it touches (added to the selection with
// Ctrl/Shift), each sounding for a moment as it is caught, and on release the
// stretch of time it covers. A click instead places the paste marker.
class SelectNotesGesture : public NoteGrid::Gesture {
public:
    SelectNotesGesture(NoteGrid* grid, const QPointF& press, bool additive)
        : Gesture(grid, press), base_(additive ? roll->selectedNotes() : std::vector<ClipNote>()) {}

    void move(const QPointF& pos, Qt::KeyboardModifiers) override {
        if (!started(pos)) return;
        rect_ = QRectF(press, pos).normalized();
        std::vector<ClipNote> hits;
        roll->forEachNote([&](int clip, const Note& note) {
            if (roll->noteRect(ClipNote{clip, note}).intersects(*rect_)) hits.push_back({clip, note});
        });
        hits = roll::clipNoteSet(std::move(hits));
        std::vector<ClipNote> caught;  // (just now: they sound)
        for (const ClipNote& note : hits) {
            if (!roll::contains(hits_, note)) caught.push_back(note);
        }
        hits_ = std::move(hits);
        roll->selectNotes(roll::united(base_, hits_));
        if (!caught.empty()) roll->previewNotes(caught);
    }

    std::optional<QRectF> rubberBand() const override { return rect_; }

    void finish() override {
        if (!active || !rect_) return;
        roll->selectNotes(roll->selectedNotes(), true);  // a group chosen by dragging: the note tools come up
        if (hits_.empty()) return;
        // The stretch it covered, out to the grid lines around it.
        const timeline::Timeline& view = roll->view();
        double start = std::max(0.0, view.xToBeat(rect_->left())), end = view.xToBeat(rect_->right());
        if (view.snap()) {
            const double step = view.gridStep();
            start = std::floor(start / step + 1e-9) * step;
            end = std::ceil(end / step - 1e-9) * step;
        }
        roll->setSelectedSpan(PianoRoll::Span{start, end});
    }

    void clicked(Qt::KeyboardModifiers modifiers) override {
        roll->setPasteBeat(roll->view().snapBeat(roll->view().xToBeat(press.x()), modifiers & Qt::AltModifier));
    }

private:
    std::vector<ClipNote> base_;
    std::vector<ClipNote> hits_;  // the notes it touches (a set)
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


// Each part of [from, to] outside every span.
std::vector<PianoRoll::Span> outside(std::vector<PianoRoll::Span> spans, double from, double to) {
    std::sort(spans.begin(), spans.end());
    std::vector<PianoRoll::Span> gaps;
    double at = from;
    for (const auto& [start, end] : spans) {
        if (start > at) gaps.emplace_back(at, std::min(start, to));
        at = std::max(at, end);
        if (at >= to) break;
    }
    if (at < to) gaps.emplace_back(at, to);
    return gaps;
}

}  // namespace

// --- NoteGrid ------------------------------------------------------------------------

NoteGrid::NoteGrid(QQuickItem* parent) : RollItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
    setActiveFocusOnTab(true);
}

NoteGrid::~NoteGrid() = default;

void NoteGrid::rollConnected(PianoRoll* roll) {
    connect(roll, &PianoRoll::vscrollChanged, this, &QQuickItem::update);
    roll->setGrid(this);
}

std::optional<NoteGrid::Hit> NoteGrid::noteAt(const QPointF& pos) const {
    PianoRoll* roll = this->roll();
    if (!roll || !roll->hasClip()) return std::nullopt;
    // The one drawn on top first: the lead's notes, then the other clips' from the last (each from its last note).
    for (int i = 0; i < roll->clipCount(); ++i) {
        const int index = i == 0 ? 0 : roll->clipCount() - i;
        const app::Clip* clip = roll->clipAt(index);
        if (!clip) continue;
        for (auto it = clip->notes.rbegin(); it != clip->notes.rend(); ++it) {
            const ClipNote note{index, *it};
            const QRectF rect = roll->noteRect(note);
            if (rect.left() <= pos.x() && pos.x() <= rect.right() && rect.top() <= pos.y() && pos.y() < rect.bottom()) {
                const double grab = std::min(kEdgeGrab, rect.width() / 4);
                if (pos.x() >= rect.right() - grab) return Hit{note, Zone::End};
                if (pos.x() <= rect.left() + grab) return Hit{note, Zone::Start};
                return Hit{note, Zone::Body};
            }
        }
    }
    return std::nullopt;
}

bool NoteGrid::dragging() const { return gesture_ && gesture_->active; }

std::optional<QRectF> NoteGrid::rubberBand() const { return gesture_ ? gesture_->rubberBand() : std::nullopt; }

// --- Painting ----------------------------------------------------------------------------

void NoteGrid::paint(SgPainter& p) {
    const QRectF visible = p.rect();
    p.fillRect(visible, Theme::lane());
    PianoRoll* roll = this->roll();
    if (!roll) return;
    const timeline::Timeline& view = roll->view();
    const int height = roll->rowHeight();
    for (int pitch = roll->pitchAt(visible.bottom()); pitch <= roll->pitchAt(visible.top()); ++pitch) {
        const double top = roll->pitchTop(pitch);
        if (notes::isBlackKey(pitch))
            p.fillRect(QRectF(visible.left(), top, visible.width(), height), Theme::blackKeyRow());
        if (pitch % 12 == 0 || pitch % 12 == 5) {  // octave (B|C) and E|F lines
            p.fillRect(QRectF(visible.left(), top + height - 1, visible.width(), 1),
                       pitch % 12 == 0 ? Theme::gridBar() : Theme::gridSub());
        }
    }
    const double bottom = std::min(visible.bottom(), roll->pitchTop(0) + height);
    timeline::drawGrid(p, view, visible.left(), visible.right(), visible.top(), bottom);
    if (bottom < visible.bottom())
        p.fillRect(QRectF(visible.left(), bottom, visible.width(), visible.bottom() - bottom), Theme::emptyArea());
    if (roll->hasClip()) {
        // Dimmed outside the parts the clips play; with several, a line at each one's ends.
        const std::vector<PianoRoll::Span> lit = roll->windows();
        for (const auto& [from, to] : outside(lit, view.xToBeat(visible.left()), view.xToBeat(visible.right() + 1))) {
            const double x0 = view.beatToX(from), x1 = view.beatToX(to);
            p.fillRect(QRectF(x0, visible.top(), x1 - x0, visible.height()), Theme::outsideClip());
        }
        if (roll->clipCount() > 1) {
            for (int i = 0; i < roll->clipCount(); ++i) {
                const app::Clip* clip = roll->clipAt(i);
                if (!clip) continue;
                QColor edge = roll->colorOf(i);
                edge.setAlphaF(0.6f);
                for (const double beat : {clip->offsetBeats + roll->shift(i), clip->windowEnd() + roll->shift(i)})
                    p.fillRect(QRectF(app::roundHalfEven(view.beatToX(beat)), visible.top(), 1, visible.height()), edge);
            }
        }
        // The stretch of time a rubber band selected, under the notes.
        if (const auto& span = roll->selectedSpan()) {
            const double x0 = view.beatToX(span->first), x1 = view.beatToX(span->second);
            p.fillRect(QRectF(x0, visible.top(), x1 - x0, bottom - visible.top()), Theme::selection());
        }
        std::vector<QColor> colors;
        std::vector<std::pair<double, double>> played;  // each clip's window, in its content beats
        for (int i = 0; i < roll->clipCount(); ++i) {
            colors.push_back(roll->colorOf(i));
            const app::Clip* clip = roll->clipAt(i);
            played.emplace_back(clip ? clip->offsetBeats : 0.0, clip ? clip->windowEnd() : 0.0);
        }
        const QFont font = uiFont(7);
        roll->forEachNote([&](int clip, const Note& n) {
            const ClipNote note{clip, n};
            const QRectF rect = roll->noteRect(note);
            if (!rect.intersects(visible)) return;
            const auto& [from, to] = played[static_cast<size_t>(clip)];
            const bool playing = from <= n.start && n.start < to;
            drawNote(p, n, rect, colors[static_cast<size_t>(clip)], font, roll->isSelected(note), playing,
                     roll->outOfKey(n.pitch));
        });
    }

    if (const auto band = rubberBand()) {
        p.fillRect(*band, Theme::rubberBand());
        p.drawRect(*band, Theme::accent(), 1);
    }
    if (const auto start = roll->startBeat())
        p.fillRect(QRectF(app::roundHalfEven(view.beatToX(*start)), 0, 1, this->height()), Theme::insertMarker());
    // Where Ctrl+V pastes: dashed, unlike the start marker and the playhead.
    if (const auto paste = roll->pasteBeat()) {
        const double x = app::roundHalfEven(view.beatToX(*paste));
        for (double y = visible.top() - std::fmod(visible.top(), kPasteDash * 2); y < visible.bottom(); y += kPasteDash * 2)
            p.fillRect(QRectF(x, y, 1, kPasteDash), Theme::pasteMarker());
    }
}

void NoteGrid::drawNote(SgPainter& p, const Note& note, const QRectF& rect, const QColor& trackColor,
                        const QFont& font, bool selected, bool playing, bool outOfKey) const {
    // Out of the song's key: halfway to red. Deactivated: grey.
    const QColor color = note.muted ? Theme::deactivatedClip()
                         : outOfKey ? QColor((trackColor.red() + Theme::outOfKey().red()) / 2,
                                             (trackColor.green() + Theme::outOfKey().green()) / 2,
                                             (trackColor.blue() + Theme::outOfKey().blue()) / 2)
                                    : trackColor;
    // Brighter for louder notes, as in Ableton; faint outside the part the clip plays.
    QColor fill = selected ? color.lighter(135) : color;
    fill.setAlphaF(static_cast<float>((0.35 + 0.65 * note.velocity / 127) * (playing ? 1.0 : 0.5)));
    const QRectF box = rect.adjusted(0.5, 0.5, -0.5, -0.5);
    p.fillRect(box, fill);
    p.drawRect(box, selected ? Theme::selectionOutline() : color.darker(220), 1);
    if (rect.width() >= 30 && rect.height() >= 10) {
        p.drawText(box.adjusted(3, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft, notes::noteName(note.pitch),
                   Theme::accentText(), font);
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
        if (!additive) roll->selectNotes({});
        gesture_ = std::make_unique<SelectNotesGesture>(this, pos, additive);
        return;
    }
    const ClipNote note = hit->note;
    // Clicking one of several selected notes selects just it, and Ctrl-clicking
    // a selected note deselects it, but only once the mouse comes up without
    // dragging: dragging moves (Ctrl: copies) the whole selection instead.
    selectOnClick_.reset();
    if (roll->isSelected(note)) {
        if (mods & Qt::ShiftModifier) {
        } else if (mods & Qt::ControlModifier) {
            selectOnClick_ = roll::without(roll->selectedNotes(), note);
        } else if (roll->selectedNotes().size() > 1) {
            selectOnClick_ = std::vector<ClipNote>{note};
        }
    } else {
        roll->selectNotes(roll::united(additive ? roll->selectedNotes() : std::vector<ClipNote>(),
                                       std::vector<ClipNote>{note}));
    }
    roll->audition(note.note.pitch, note.note.velocity, note.clip);
    std::vector<ClipNote> moving = roll->selectedNotes();
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
    if (selectOnClick_ && gesture && !gesture->active) roll->selectNotes(*selectOnClick_);
    selectOnClick_.reset();
    if (gesture) {
        if (gesture->active)
            gesture->finish();
        else
            gesture->clicked(event->modifiers());
    }
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
    if (event->button() != Qt::LeftButton || !roll || !roll->hasClip() || isPanModifier(event->modifiers())) return;
    forceActiveFocus(Qt::MouseFocusReason);
    const QPointF pos = event->position();
    if (const auto hit = noteAt(pos)) {
        const app::Clip* clip = roll->clipAt(hit->note.clip);
        roll->setToolsWanted(false);
        roll->commitNotes({{hit->note.clip, notes::place(clip->notes, {hit->note.note}, {})}},
                          QStringLiteral("Delete Note"), {}, roll::without(roll->selectedNotes(), hit->note));
        return;
    }
    const timeline::Timeline& view = roll->view();
    const double step = view.gridStep();
    double beat = std::max(0.0, view.xToBeat(pos.x()));
    if (view.snap() && !(event->modifiers() & Qt::AltModifier))
        beat = std::floor(beat / step + 1e-9) * step;  // the grid cell that was clicked
    // Into the clip that plays there (the lead, if it does).
    const int index = roll->clipFor(beat).value_or(0);
    const app::Clip* clip = roll->clipAt(index);
    if (!clip) return;
    Note note;
    note.pitch = roll->pitchAt(pos.y());
    note.start = std::max(0.0, beat - roll->shift(index));
    note.length = step;
    roll->setToolsWanted(false);
    roll->commitNotes({{index, notes::place(clip->notes, {}, {note})}}, QStringLiteral("Add Note"), {},
                      std::vector<ClipNote>{{index, note}});
    roll->audition(note.pitch, note.velocity, index);
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
    if (event->modifiers() & Qt::ControlModifier) {
        return key == Qt::Key_A || key == Qt::Key_D || key == Qt::Key_U || key == Qt::Key_C || key == Qt::Key_X ||
               key == Qt::Key_V;
    }
    return key == Qt::Key_Delete || key == Qt::Key_Backspace || key == Qt::Key_Up || key == Qt::Key_Down ||
           key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_0;
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
    if (!roll || !roll->hasClip() || !handles(event)) {
        event->ignore();
        return;
    }
    const int key = event->key();
    const bool ctrl = event->modifiers() & Qt::ControlModifier;
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    const std::vector<ClipNote> selected = roll->selectedNotes();
    if (key == Qt::Key_0) {
        roll->toggleSelectedActive();  // Ableton's: the selected notes deactivated, or activated if they all are
    } else if (ctrl && key == Qt::Key_A) {
        roll->selectNotes(roll->allNotes(), true);
    } else if (ctrl && key == Qt::Key_U) {
        roll->quantize();  // the selected notes, or all
    } else if (ctrl && key == Qt::Key_C) {
        roll->copySelected();
    } else if (ctrl && key == Qt::Key_X) {
        roll->cutSelected();
    } else if (ctrl && key == Qt::Key_V) {
        roll->paste();
    } else if (selected.empty()) {
    } else if (key == Qt::Key_Delete || key == Qt::Key_Backspace) {
        std::map<int, std::vector<Note>> changes;
        for (const auto& [index, base] : clipNotes(roll, selected))
            changes[index] = notes::place(base, roll::notesOf(selected, index), {});
        roll->commitNotes(changes, QStringLiteral("Delete Notes"), {}, std::vector<ClipNote>());
    } else if (ctrl && key == Qt::Key_D) {
        roll->duplicateSelected();
    } else {
        double deltaBeats = 0.0;
        int deltaPitch = 0;
        if (key == Qt::Key_Up || key == Qt::Key_Down) {
            deltaPitch = (shift ? 12 : 1) * (key == Qt::Key_Up ? 1 : -1);
        } else {
            const double step = shift ? roll->view().timeSignature().beatsPerBar() : roll->view().gridStep();
            deltaBeats = step * (key == Qt::Key_Right ? 1 : -1);
        }
        const auto [beats, pitch] = clampMove(selected, deltaBeats, deltaPitch);
        if (beats != 0.0 || pitch != 0) {
            std::map<int, std::vector<Note>> changes;
            std::vector<ClipNote> moved;
            for (const auto& [index, base] : clipNotes(roll, selected)) {
                const std::vector<Note> mine = roll::notesOf(selected, index);
                const std::vector<Note> shifted = notes::shifted(mine, beats, pitch);
                changes[index] = notes::place(base, mine, shifted);
                for (const Note& note : shifted) moved.push_back({index, note});
            }
            roll->commitNotes(changes, pitch ? QStringLiteral("Transpose Notes") : QStringLiteral("Move Notes"), {},
                              moved);
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
