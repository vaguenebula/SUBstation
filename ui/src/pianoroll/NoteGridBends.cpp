// The note grid's bends (MIDI 2.0's per-note pitch bend): each note's bend
// drawn as a curve over the rows (a semitone a row, from the middle of the
// note's own), and in bend mode edited as an automation envelope is, and the
// vibrato tool. See NoteGrid.h.

#include "pianoroll/NoteGrid.h"

#include "input/GestureKey.h"
#include "model/Notes.h"
#include "model/Numbers.h"
#include "model/Project.h"
#include "pianoroll/NoteGridGesture.h"
#include "pianoroll/NoteSet.h"
#include "pianoroll/PianoRoll.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QKeyEvent>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

namespace sub::ui {

using app::Note;
namespace notes = app::notes;

namespace {

constexpr QColor kCurve{255, 255, 255, 235};        // a curve in bend mode
constexpr QColor kCurveShadow{0, 0, 0, 150};        // under it, so it shows over any colour
constexpr QColor kHandCurve{255, 255, 255, 90};     // the curve drawn by hand, under a vibrato
constexpr QColor kFaintCurve{255, 255, 255, 120};   // a bent note's curve out of bend mode
constexpr QColor kGhost{255, 255, 255, 170};        // where a click would add a point
constexpr QColor kLabelBack{20, 20, 20, 220};
constexpr double kSamplePixels = 2.0;  // between the points a curve is drawn through
constexpr int kMaxSamples = 6000;      // per note

double tempoOf(const PianoRoll* roll) { return roll->project() ? roll->project()->tempo() : 120.0; }

// Each note shown and where it is on the roll, as forEachNote gives them.
template <typename Visit>
void forEachShown(const PianoRoll* roll, Visit&& visit) {
    roll->forEachNote([&](int clip, const Note& note) { visit(ClipNote{clip, note}); });
}

// "+2.00 st"
QString semitoneText(double semitones) {
    const QString number = app::formatFixed(std::abs(semitones), 2);
    return (semitones > 0.0 ? QStringLiteral("+") : semitones < 0.0 ? QStringLiteral("−") : QString()) + number +
           QStringLiteral(" st");
}

// Drags a note's selected bend points (and those of other notes selected with
// them) in time and pitch: the grabbed one onto the grid and a whole semitone
// (Alt: anywhere), the others by as much. A click without a drag on a point
// deletes it (unless it was Ctrl-clicked, or was just added).
class MoveBendPointsGesture : public NoteGrid::Gesture {
public:
    MoveBendPointsGesture(NoteGrid* grid, const QPointF& press, const PianoRoll::BendRef& grabbed, bool removeOnClick,
                          const QString& mergeKey = {})
        : Gesture(grid, press), removeOnClick_(removeOnClick) {
        if (!mergeKey.isEmpty()) key = mergeKey;
        for (const PianoRoll::BendRef& ref : roll->selectedBends()) {
            if (groups_.empty() || !(groups_.back().base == ref.note)) groups_.push_back({ref.note, {}, ref.note});
            groups_.back().points.push_back(ref.point);
            if (ref == grabbed) {
                grabbed_ = static_cast<int>(groups_.size()) - 1;
                point_ = ref.point;
            }
        }
        if (grabbed_ < 0) {  // (not selected: it moves alone)
            groups_ = {{grabbed.note, {grabbed.point}, grabbed.note}};
            grabbed_ = 0;
            point_ = grabbed.point;
        }
    }

    void move(const QPointF& pos, Qt::KeyboardModifiers modifiers) override {
        if (!started(pos)) return;
        const Group& mine = groups_[static_cast<size_t>(grabbed_)];
        const app::BendPoint& from = mine.base.note.bend[static_cast<size_t>(point_)];
        const bool free = modifiers & Qt::AltModifier;
        const timeline::Timeline& view = roll->view();
        // The grabbed point onto the grid and a whole semitone; the rest by as much.
        const double startAt = roll->rollStart(mine.base);
        const double at = view.snapBeat(startAt + from.time + view.xToBeat(pos.x()) - view.xToBeat(press.x()), free);
        double value = from.semitones + (press.y() - pos.y()) / roll->rowHeight();
        if (!free) value = std::round(value);
        const double deltaTime = at - startAt - from.time, deltaSemitones = value - from.semitones;
        std::vector<std::pair<ClipNote, Note>> changes;
        std::vector<PianoRoll::BendRef> points;
        for (Group& group : groups_) {
            const Note moved = notes::withBendPointsMoved(group.base.note, group.points, deltaTime, deltaSemitones);
            changes.emplace_back(group.current, moved);
            group.current = {group.base.clip, moved};
            for (const int point : group.points) points.push_back({group.current, point});
        }
        roll->commitBends(changes, QStringLiteral("Move Bend Points"), key, points);
    }

    void clicked(Qt::KeyboardModifiers modifiers) override {
        if (!removeOnClick_ || (modifiers & Qt::ControlModifier)) return;
        const Group& mine = groups_[static_cast<size_t>(grabbed_)];
        roll->commitBend(mine.current, notes::withoutBendPoints(mine.current.note, {point_}),
                         QStringLiteral("Delete Bend Point"), {}, std::vector<int>());
    }

    std::optional<std::pair<QPointF, QString>> label() const override {
        if (!active) return std::nullopt;
        const ClipNote& note = groups_[static_cast<size_t>(grabbed_)].current;
        if (point_ >= static_cast<int>(note.note.bend.size())) return std::nullopt;
        const app::BendPoint& point = note.note.bend[static_cast<size_t>(point_)];
        const QPointF at(roll->view().beatToX(roll->rollStart(note) + point.time), roll->bendY(note, point.semitones));
        return std::make_pair(at, semitoneText(point.semitones));
    }

private:
    struct Group {
        ClipNote base;            // the note as it was at the press
        std::vector<int> points;  // its points moving
        ClipNote current;         // as last committed
    };
    std::vector<Group> groups_;
    int grabbed_ = -1;  // the group of the point grabbed,
    int point_ = 0;     // and its place in the note's bend
    bool removeOnClick_;
};

// Alt-drag between two points: bends the segment from the first (up bulges it upward).
class CurveBendGesture : public NoteGrid::Gesture {
public:
    CurveBendGesture(NoteGrid* grid, const QPointF& press, const ClipNote& note, int point)
        : Gesture(grid, press), base_(note), current_(note), point_(point) {}

    void move(const QPointF& pos, Qt::KeyboardModifiers) override {
        if (!started(pos)) return;
        const double curve = base_.note.bend[static_cast<size_t>(point_)].curve + (press.y() - pos.y()) / NoteGrid::kCurvePixels;
        current_ = roll->commitBend(current_, notes::withBendCurve(base_.note, point_, curve), QStringLiteral("Bend Segment"), key);
    }

private:
    ClipNote base_;
    ClipNote current_;
    int point_;
};

// A rubber band selecting bend points (added to those selected with Ctrl); a
// click in empty space lets them go.
class SelectBendsGesture : public NoteGrid::Gesture {
public:
    SelectBendsGesture(NoteGrid* grid, const QPointF& press, bool additive)
        : Gesture(grid, press), additive_(additive), base_(additive ? roll->selectedBends() : std::vector<PianoRoll::BendRef>()) {}

    void move(const QPointF& pos, Qt::KeyboardModifiers) override {
        if (!started(pos)) return;
        rect_ = QRectF(press, pos).normalized();
        std::vector<PianoRoll::BendRef> chosen = base_;
        forEachShown(roll, [&](const ClipNote& note) {
            for (int i = 0; i < static_cast<int>(note.note.bend.size()); ++i) {
                const app::BendPoint& point = note.note.bend[static_cast<size_t>(i)];
                if (point.time < 0.0 || point.time > note.note.length) continue;  // (not drawn)
                const QPointF at(roll->view().beatToX(roll->rollStart(note) + point.time), roll->bendY(note, point.semitones));
                if (rect_->contains(at)) chosen.push_back({note, i});
            }
        });
        roll->selectBends(std::move(chosen));
    }

    std::optional<QRectF> rubberBand() const override { return rect_; }

    void clicked(Qt::KeyboardModifiers) override {
        if (!additive_) roll->selectBends({});
    }

private:
    bool additive_;
    std::vector<PianoRoll::BendRef> base_;
    std::optional<QRectF> rect_;
};

// The vibrato tool on a note: a drag across it draws vibrato over the stretch
// dragged across (on the grid; Ctrl: anywhere), dragging up deepens it from the
// bend bar's depth. While Shift is held, dragging sideways makes it faster
// (right) or slower (left) instead, and while Alt is, its ramp longer or
// shorter (how much of it it takes to reach its depth): the stretch and the
// depth stay as they are meanwhile, and go on from there once it is let go. A
// click adds vibrato from there to the note's end, or, on a vibrato, takes it
// away. The bend bar keeps its settings for the next one.
class VibratoGesture : public NoteGrid::Gesture {
public:
    VibratoGesture(NoteGrid* grid, const QPointF& press, const ClipNote& note, bool free)
        : Gesture(grid, press), base_(note), current_(note), last_(press), rate_(roll->vibratoRate()),
          fade_(roll->vibratoFade() / 100.0), depth_(roll->vibratoDepth()) {
        const double start = roll->rollStart(note);
        from_ = std::clamp(roll->view().snapBeat(roll->view().xToBeat(press.x()), free), start, start + note.note.length);
    }

    void move(const QPointF& pos, Qt::KeyboardModifiers modifiers) override {
        if (!started(pos)) return;
        const bool speed = modifiers & Qt::ShiftModifier, ramp = modifiers & Qt::AltModifier;
        const QPointF step = pos - last_;
        last_ = pos;
        if (speed || ramp) {
            held_ += step;  // (taken from the stretch and the depth)
            if (speed) {
                rate_ = std::clamp(rate_ * std::exp2(step.x() / NoteGrid::kVibratoRatePixels), notes::kMinVibratoRate,
                                   notes::kMaxVibratoRate);
            }
            if (ramp) fade_ = std::clamp(fade_ + step.x() / NoteGrid::kVibratoRampPixels, 0.0, 1.0);
        }
        grid->setCursor(QCursor(speed || ramp ? Qt::SizeHorCursor : Qt::CrossCursor));
        // Where the mouse would be but for what Shift and Alt took.
        const QPointF at = pos - held_;
        const double start = roll->rollStart(base_);
        const double to = std::clamp(roll->view().snapBeat(roll->view().xToBeat(at.x()), modifiers & Qt::ControlModifier),
                                     start, start + base_.note.length);
        depth_ = std::clamp(roll->vibratoDepth() + (press.y() - at.y()) / roll->rowHeight(), 0.05, 12.0);
        const double a = std::min(from_, to), b = std::max(from_, to);
        if (b - a < notes::kMinVibratoBeats) return;
        drawn_ = {a, b};
        draw(a - start, b - a);
    }

    void clicked(Qt::KeyboardModifiers) override {
        const double at = roll->view().xToBeat(press.x()) - roll->rollStart(base_);
        const auto& vibratos = base_.note.vibrato;
        for (int i = 0; i < static_cast<int>(vibratos.size()); ++i) {
            const app::Vibrato& v = vibratos[static_cast<size_t>(i)];
            if (v.start <= at && at < v.start + v.length) {
                roll->commitBend(base_, notes::withoutVibrato(base_.note, i), QStringLiteral("Remove Vibrato"));
                return;
            }
        }
        const double from = from_ - roll->rollStart(base_);
        if (base_.note.length - from >= notes::kMinVibratoBeats) draw(from, base_.note.length - from);
    }

    std::optional<std::pair<QPointF, QString>> label() const override {
        if (!active) return std::nullopt;
        return std::make_pair(QPointF(press.x(), press.y() - 14),
                              semitoneText(depth_) + QStringLiteral(" · ") + app::formatFixed(rate(), 1) +
                                  QStringLiteral(" Hz · ramp ") + app::formatFixed(fade() * 100.0, 0) + QStringLiteral(" %"));
    }

    std::optional<QRectF> area() const override {
        if (!active || drawn_.second <= drawn_.first) return std::nullopt;
        const double top = roll->pitchTop(base_.note.pitch) - depth_ * roll->rowHeight();
        return QRectF(QPointF(roll->view().beatToX(drawn_.first), top),
                      QPointF(roll->view().beatToX(drawn_.second), top + roll->rowHeight() * (1.0 + 2.0 * depth_)));
    }

private:
    // What it is drawn with: the rate to a tenth of a hertz, the ramp to a percent.
    double rate() const { return std::round(rate_ * 10.0) / 10.0; }
    double fade() const { return std::round(fade_ * 100.0) / 100.0; }

    void draw(double start, double length) {
        const Note drawn = notes::withVibrato(base_.note, app::Vibrato{start, length, depth_, rate(), fade()});
        current_ = roll->commitBend(current_, drawn, QStringLiteral("Draw Vibrato"), key);
    }

    ClipNote base_;
    ClipNote current_;
    double from_ = 0.0;  // roll beats
    QPointF last_;       // the mouse at the last move
    QPointF held_;       // how far it moved while Shift or Alt was held
    double rate_;        // as dragged, before rounding
    double fade_;
    double depth_;
    std::pair<double, double> drawn_;  // the stretch drawn over (roll beats)
};

// The glide tool on a note: a slide (a glissando) into the note after it
// (notes::nextNote), from where the press is (on the grid; Ctrl: anywhere) over
// the stretch dragged across, or with a click to the note's end. While Alt is
// held, dragging sideways bends it instead (right: it arrives later, easing
// in; left: sooner, easing out), the stretch staying where it is meanwhile;
// the curve set so is what new slides start with (PianoRoll::glideCurve). It
// is made of two bend points (notes::withSlide), to edit as any others.
class GlideGesture : public NoteGrid::Gesture {
public:
    GlideGesture(NoteGrid* grid, const QPointF& press, const ClipNote& note, int target, bool free)
        : Gesture(grid, press), base_(note), current_(note), last_(press), target_(target),
          semitones_(std::clamp(static_cast<double>(target - note.note.pitch), -notes::kMaxBendSemitones,
                                notes::kMaxBendSemitones)),
          amount_(roll->glideCurve() / 100.0) {
        const double start = roll->rollStart(note);
        from_ = std::clamp(roll->view().snapBeat(roll->view().xToBeat(press.x()), free), start, start + note.note.length);
    }

    void move(const QPointF& pos, Qt::KeyboardModifiers modifiers) override {
        if (!started(pos)) return;
        const bool bending = modifiers & Qt::AltModifier;
        const QPointF step = pos - last_;
        last_ = pos;
        if (bending) {
            held_ += step;  // (taken from the stretch)
            amount_ = std::clamp(amount_ + step.x() / NoteGrid::kGlideCurvePixels, -1.0, 1.0);
            roll->setGlideCurve(curvePercent());  // (the next slide's too)
        }
        grid->setCursor(QCursor(bending ? Qt::SizeHorCursor : Qt::CrossCursor));
        const double start = roll->rollStart(base_);
        const double to = std::clamp(
            roll->view().snapBeat(roll->view().xToBeat((pos - held_).x()), modifiers & Qt::ControlModifier), start,
            start + base_.note.length);
        if (to - from_ < notes::kMinNoteBeats) return;  // (it goes on from where it starts)
        to_ = to;
        draw();
    }

    void clicked(Qt::KeyboardModifiers) override {
        to_ = roll->rollStart(base_) + base_.note.length;
        if (to_ - from_ >= notes::kMinNoteBeats) draw();
    }

    std::optional<std::pair<QPointF, QString>> label() const override {
        if (!active) return std::nullopt;
        return std::make_pair(QPointF(press.x(), press.y() - 14),
                              QStringLiteral("→ ") + notes::noteName(target_) + QStringLiteral(" · ") +
                                  semitoneText(semitones_) + QStringLiteral(" · curve ") +
                                  (curvePercent() > 0.0 ? QStringLiteral("+") : QString()) +
                                  app::formatFixed(curvePercent(), 0) + QStringLiteral(" %"));
    }

    std::optional<QRectF> area() const override {
        if (!active || to_ <= from_) return std::nullopt;
        const double from = roll->bendY(base_, base_.note.curveAt(from_ - roll->rollStart(base_)));
        const double to = roll->bendY(base_, semitones_);
        const double half = roll->rowHeight() / 2.0;
        return QRectF(QPointF(roll->view().beatToX(from_), std::min(from, to) - half),
                      QPointF(roll->view().beatToX(to_), std::max(from, to) + half));
    }

private:
    // Its curve as drawn: to a percent.
    double curvePercent() const { return std::round(amount_ * 100.0); }

    void draw() {
        const double start = roll->rollStart(base_);
        // Right (positive) arrives later: the curve bulges away from where it goes.
        const double from = base_.note.curveAt(from_ - start);
        const double amount = curvePercent() / 100.0;
        const double curve = semitones_ >= from ? -amount : amount;
        const Note slid = notes::withSlide(base_.note, from_ - start, to_ - start, semitones_, curve);
        current_ = roll->commitBend(current_, slid, QStringLiteral("Add Slide"), key);
    }

    ClipNote base_;
    ClipNote current_;
    QPointF last_;
    QPointF held_;     // how far it moved while Alt was held
    int target_;       // the pitch it slides to
    double semitones_;
    double amount_;      // its curve as dragged: -1..1, positive arriving later
    double from_ = 0.0;  // roll beats
    double to_ = 0.0;
};

}  // namespace

// --- Hit-testing -------------------------------------------------------------------------

std::optional<ClipNote> NoteGrid::curveNear(const QPointF& pos, double grab) const {
    PianoRoll* roll = this->roll();
    if (!roll || !roll->hasClip()) return std::nullopt;
    const double beat = roll->view().xToBeat(pos.x());
    std::optional<ClipNote> nearest;
    double best = 0.0;
    forEachShown(roll, [&](const ClipNote& note) {
        const double start = roll->rollStart(note);
        if (beat < start || beat > start + note.note.length) return;
        const double distance = std::abs(pos.y() - roll->bendY(note, roll->bendAt(note, beat)));
        if (grab >= 0.0 && distance > grab) return;
        if (!nearest || distance <= best) {  // (the one drawn on top, of two as near)
            nearest = note;
            best = distance;
        }
    });
    return nearest;
}

std::optional<ClipNote> NoteGrid::toolNote(const QPointF& pos) const {
    if (auto note = curveNear(pos, kLineGrab * 2)) return note;
    if (const auto hit = noteAt(pos)) return hit->note;
    return std::nullopt;
}

std::optional<app::Note> NoteGrid::slideTarget(const ClipNote& note) const {
    const app::Clip* clip = roll()->clipAt(note.clip);
    return clip ? notes::nextNote(clip->notes, note.note) : std::nullopt;
}

std::optional<NoteGrid::BendHit> NoteGrid::bendHitAt(const QPointF& pos, Qt::KeyboardModifiers modifiers) const {
    PianoRoll* roll = this->roll();
    if (!roll || !roll->hasClip()) return std::nullopt;
    const timeline::Timeline& view = roll->view();
    // A point (the nearest, the one drawn on top of two as near).
    std::optional<BendHit> point;
    double best = 0.0;
    forEachShown(roll, [&](const ClipNote& note) {
        for (int i = 0; i < static_cast<int>(note.note.bend.size()); ++i) {
            const app::BendPoint& p = note.note.bend[static_cast<size_t>(i)];
            if (p.time < 0.0 || p.time > note.note.length) continue;
            const double beat = roll->rollStart(note) + p.time;
            const QPointF at(view.beatToX(beat), roll->bendY(note, p.semitones));
            const double distance = std::hypot(at.x() - pos.x(), at.y() - pos.y());
            if (distance > kPointGrab || (point && distance > best)) continue;
            point = BendHit{BendHit::Kind::Point, note, i, beat, p.semitones};
            best = distance;
        }
    });
    if (point) return point;
    const double beat = view.xToBeat(pos.x());
    // With Alt, a segment between two points.
    if (modifiers & Qt::AltModifier) {
        if (const auto note = curveNear(pos, kSegmentGrab)) {
            const double t = beat - roll->rollStart(*note);
            const auto& bend = note->note.bend;
            for (int i = 0; i + 1 < static_cast<int>(bend.size()); ++i) {
                if (bend[static_cast<size_t>(i)].time <= t && t < bend[static_cast<size_t>(i) + 1].time)
                    return BendHit{BendHit::Kind::Segment, *note, i, beat, roll->bendAt(*note, beat)};
            }
        }
    }
    // On the line: a point would go on the grid, on the curve drawn by hand (a vibrato swings around it).
    const auto note = curveNear(pos, kLineGrab);
    if (!note) return std::nullopt;
    const double start = roll->rollStart(*note);
    const double at = std::clamp(view.snapBeat(beat, modifiers & Qt::AltModifier), start, start + note->note.length);
    return BendHit{BendHit::Kind::Line, *note, -1, at, note->note.curveAt(at - start)};
}

// --- Painting --------------------------------------------------------------------------------

void NoteGrid::paintBends(SgPainter& p, const QRectF& visible) const {
    PianoRoll* roll = this->roll();
    if (!roll || !roll->hasClip()) return;
    const bool editing = roll->bendView();
    const timeline::Timeline& view = roll->view();
    const double tempo = tempoOf(roll);
    p.save();
    p.setAntialiasing(true);
    const auto hovered = [&](const ClipNote& note, int point) {
        return bendHover_ && bendHover_->kind == BendHit::Kind::Point && bendHover_->note == note && bendHover_->point == point;
    };
    forEachShown(roll, [&](const ClipNote& note) {
        if (!editing && !note.note.bent()) return;
        const double start = roll->rollStart(note);
        const double x0 = view.beatToX(start), x1 = view.beatToX(start + note.note.length);
        if (x1 < visible.left() - kPointRadius || x0 > visible.right() + kPointRadius) return;
        // Through the visible part, finely enough for its fastest vibrato.
        double step = kSamplePixels;
        for (const app::Vibrato& v : note.note.vibrato) {
            const double pixelsPerCycle = view.pxPerBeat() * tempo / 60.0 / std::max(0.1, v.rate);
            step = std::min(step, std::max(0.25, pixelsPerCycle / 10.0));
        }
        const double from = std::max(x0, visible.left() - 2), to = std::min(x1, visible.right() + 2);
        step = std::max(step, (to - from) / kMaxSamples);
        QPolygonF line, hand;
        for (double x = from;; x += step) {
            x = std::min(x, to);
            const double t = view.xToBeat(x) - start;
            line.append(QPointF(x, roll->bendY(note, note.note.bendAt(t, tempo))));
            if (!note.note.vibrato.empty()) hand.append(QPointF(x, roll->bendY(note, note.note.curveAt(t))));
            if (x >= to) break;
        }
        if (!editing) {
            p.drawPolyline(line, kFaintCurve, 1.2);
            return;
        }
        if (!hand.isEmpty()) p.drawPolyline(hand, kHandCurve, 1.0);
        const bool segment = bendHover_ && bendHover_->kind == BendHit::Kind::Segment && bendHover_->note == note;
        p.drawPolyline(line, kCurveShadow, segment ? 4.4 : 3.4);
        p.drawPolyline(line, kCurve, segment ? 2.6 : 1.6);
        // Its vibratos: a bar along the bottom of its row.
        const double bottom = roll->pitchTop(note.note.pitch) + roll->rowHeight() - 2;
        for (const app::Vibrato& v : note.note.vibrato) {
            const double vx0 = view.beatToX(start + std::max(0.0, v.start));
            const double vx1 = view.beatToX(start + std::min(note.note.length, v.start + v.length));
            if (vx1 > vx0) p.fillRect(QRectF(vx0, bottom, vx1 - vx0, 2), Theme::kAccent);
        }
        for (int i = 0; i < static_cast<int>(note.note.bend.size()); ++i) {
            const app::BendPoint& point = note.note.bend[static_cast<size_t>(i)];
            if (point.time < 0.0 || point.time > note.note.length) continue;
            const QPointF at(view.beatToX(start + point.time), roll->bendY(note, point.semitones));
            const double radius = kPointRadius + (hovered(note, i) ? 1.0 : 0.0);
            p.fillEllipse(at, radius, radius, roll->isBendSelected(note, i) ? Theme::kSelectionOutline : Theme::kLane);
            p.drawEllipse(QRectF(at.x() - radius, at.y() - radius, 2 * radius, 2 * radius), kCurve, 1.4);
        }
    });
    // Where a click on a curve would add a point (not while a gesture is under way).
    if (editing && bendHover_ && bendHover_->kind == BendHit::Kind::Line && !gesture_ &&
        roll->bendTool() == QLatin1String(PianoRoll::kDrawTool)) {
        const double radius = kPointRadius + 1.0;
        const QPointF at(view.beatToX(bendHover_->beat), roll->bendY(bendHover_->note, bendHover_->semitones));
        p.fillEllipse(at, radius, radius, QColor(255, 255, 255, 60));
        p.drawEllipse(QRectF(at.x() - radius, at.y() - radius, 2 * radius, 2 * radius), kGhost, 1.4);
    }
    // The stretch a vibrato or a slide is being drawn over.
    if (gesture_) {
        if (const auto area = gesture_->area()) {
            QColor tint = Theme::kAccent;
            tint.setAlpha(40);
            p.fillRect(*area, tint);
        }
    }
    // A dragged point's value (or a vibrato's depth), by it.
    if (gesture_) {
        if (const auto label = gesture_->label()) {
            const QFont font = uiFont(8);
            const QRectF box(label->first.x() + 8, label->first.y() - 20, 6.5 * label->second.size() + 8, 15);
            p.fillRoundedRect(box, 3, 3, kLabelBack);
            p.drawText(box, Qt::AlignCenter, label->second, Theme::kText, font);
        }
    }
    p.restore();
}

// --- Mouse ---------------------------------------------------------------------------------

std::unique_ptr<NoteGrid::Gesture> NoteGrid::bendPress(const QPointF& pos, Qt::KeyboardModifiers modifiers) {
    PianoRoll* roll = this->roll();
    const bool additive = modifiers & Qt::ControlModifier;
    if (roll->bendTool() == QLatin1String(PianoRoll::kVibratoTool)) {
        const auto note = toolNote(pos);
        if (!note) return nullptr;
        return std::make_unique<VibratoGesture>(this, pos, *note, modifiers & Qt::ControlModifier);
    }
    if (roll->bendTool() == QLatin1String(PianoRoll::kGlideTool)) {
        const auto note = toolNote(pos);
        const auto next = note ? slideTarget(*note) : std::nullopt;
        if (!next) return nullptr;  // (nothing after it to slide to)
        return std::make_unique<GlideGesture>(this, pos, *note, next->pitch, modifiers & Qt::ControlModifier);
    }
    const auto hit = bendHitAt(pos, modifiers);
    if (!hit) return std::make_unique<SelectBendsGesture>(this, pos, additive);
    switch (hit->kind) {
        case BendHit::Kind::Point: {
            const PianoRoll::BendRef ref{hit->note, hit->point};
            if (additive) {  // Ctrl-click: in or out of the selection
                std::vector<PianoRoll::BendRef> chosen = roll->selectedBends();
                const auto found = std::find(chosen.begin(), chosen.end(), ref);
                if (found != chosen.end()) {
                    chosen.erase(found);
                    roll->selectBends(std::move(chosen));
                    return nullptr;
                }
                chosen.push_back(ref);
                roll->selectBends(std::move(chosen));
            } else if (!roll->isBendSelected(hit->note, hit->point)) {
                roll->selectBends({ref});
            }
            return std::make_unique<MoveBendPointsGesture>(this, pos, ref, !additive);
        }
        case BendHit::Kind::Segment:
            return std::make_unique<CurveBendGesture>(this, pos, hit->note, hit->point);
        case BendHit::Kind::Line: {
            // A point on the line, at once, then dragged where the mouse goes (one undo step).
            const QString key = newGestureKey();
            int index = 0;
            const Note added = notes::withBendPoint(
                hit->note.note, {hit->beat - roll->rollStart(hit->note), hit->semitones, 0.0}, &index);
            roll->selectBends({});
            const ClipNote note = roll->commitBend(hit->note, added, QStringLiteral("Add Bend Point"), key, std::vector<int>{index});
            const PianoRoll::BendRef ref{note, index};
            return std::make_unique<MoveBendPointsGesture>(this, pos, ref, false, key);
        }
    }
    return nullptr;
}

void NoteGrid::bendDoubleClick(const QPointF& pos, Qt::KeyboardModifiers modifiers) {
    PianoRoll* roll = this->roll();
    if (roll->bendTool() != QLatin1String(PianoRoll::kDrawTool)) return;
    // A point at the pitch clicked (a whole semitone; Alt: anywhere), on the
    // note whose curve is nearest there; where the click before put one, it goes there.
    const auto note = curveNear(pos, -1.0);
    if (!note) return;
    const bool free = modifiers & Qt::AltModifier;
    const double start = roll->rollStart(*note);
    const double beat = std::clamp(roll->view().snapBeat(roll->view().xToBeat(pos.x()), free), start, start + note->note.length);
    double semitones = roll->bendSemitones(*note, pos.y());
    if (!free) semitones = std::round(semitones);
    const double t = beat - start;
    Note changed = note->note;
    int index = -1;
    for (int i = 0; i < static_cast<int>(changed.bend.size()); ++i) {
        if (std::abs(roll->view().beatToX(start + changed.bend[static_cast<size_t>(i)].time) - roll->view().beatToX(beat)) <= 1.0)
            index = i;
    }
    if (index >= 0) {
        changed.bend[static_cast<size_t>(index)].semitones = std::clamp(semitones, -notes::kMaxBendSemitones, notes::kMaxBendSemitones);
    } else {
        changed = notes::withBendPoint(changed, {t, semitones, 0.0}, &index);
    }
    roll->commitBend(*note, changed, QStringLiteral("Add Bend Point"), {}, std::vector<int>{index});
}

Qt::CursorShape NoteGrid::bendCursor(const QPointF& pos, Qt::KeyboardModifiers modifiers) {
    PianoRoll* roll = this->roll();
    if (roll->bendTool() == QLatin1String(PianoRoll::kVibratoTool)) {
        bendHover_.reset();
        return toolNote(pos) ? Qt::CrossCursor : Qt::ArrowCursor;
    }
    if (roll->bendTool() == QLatin1String(PianoRoll::kGlideTool)) {
        bendHover_.reset();
        const auto note = toolNote(pos);
        if (!note) return Qt::ArrowCursor;
        return slideTarget(*note) ? Qt::CrossCursor : Qt::ForbiddenCursor;  // (nothing after it)
    }
    bendHover_ = bendHitAt(pos, modifiers);
    if (!bendHover_) return Qt::ArrowCursor;
    switch (bendHover_->kind) {
        case BendHit::Kind::Point: return Qt::PointingHandCursor;
        case BendHit::Kind::Segment: return Qt::SizeVerCursor;
        case BendHit::Kind::Line: return Qt::CrossCursor;
    }
    return Qt::ArrowCursor;
}

// --- Keys ------------------------------------------------------------------------------------

bool NoteGrid::bendKey(QKeyEvent* event) {
    PianoRoll* roll = this->roll();
    const int key = event->key();
    const bool plain = !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
    if (plain && key == Qt::Key_B) {
        roll->toggleBendMode();
        return true;
    }
    // The vibrato and glide tools (into bend mode, if out of it); again, back to Draw.
    if (plain && (key == Qt::Key_V || key == Qt::Key_G)) {
        const QString tool = QString::fromLatin1(key == Qt::Key_V ? PianoRoll::kVibratoTool : PianoRoll::kGlideTool);
        const bool again = roll->bendMode() && roll->bendTool() == tool;
        roll->setBendMode(true);
        roll->setBendTool(again ? QString::fromLatin1(PianoRoll::kDrawTool) : tool);
        return true;
    }
    if (!roll->bendView()) return false;  // (Shift held: the notes' keys)
    if (key == Qt::Key_Delete || key == Qt::Key_Backspace) {
        roll->deleteSelectedBends();  // (in bend mode Delete takes points, never notes)
        return true;
    }
    if ((event->modifiers() & Qt::ControlModifier) && key == Qt::Key_A) {
        std::vector<PianoRoll::BendRef> all;
        forEachShown(roll, [&](const ClipNote& note) {
            for (int i = 0; i < static_cast<int>(note.note.bend.size()); ++i) all.push_back({note, i});
        });
        roll->selectBends(std::move(all));
        return true;
    }
    return false;
}

}  // namespace sub::ui
