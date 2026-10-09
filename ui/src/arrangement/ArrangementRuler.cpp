#include "arrangement/ArrangementRuler.h"

#include "editor/ProjectEditor.h"
#include "input/GestureKey.h"
#include "model/Numbers.h"
#include "model/Project.h"
#include "model/Timebase.h"
#include "session/Selection.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"
#include "timeline/Timeline.h"

#include <QCursor>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

namespace sub::ui {

ArrangementRuler::ArrangementRuler(QQuickItem* parent) : ArrangementItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
    setImplicitHeight(kHeight);
}

void ArrangementRuler::connectSession(app::Session* session) {
    const auto repaint = [this] { this->repaint(); };
    connect(session->project(), &app::Project::settingsChanged, this, repaint);
    connect(session->project(), &app::Project::reset, this, repaint);
    connect(session->selection(), &app::Selection::insertChanged, this, repaint);
}

void ArrangementRuler::paint(SgPainter& p) {
    const QRectF rect = p.rect();
    const double w = width(), h = height();
    p.fillRect(rect, Theme::kPanel);
    p.fillRect(QRectF(rect.left(), 0, rect.width(), kLoopStrip), Theme::kPanelAlt);
    if (!ready()) return;
    const app::Project& project = *session()->project();
    const timeline::Timeline& view = arrangement()->view();

    // The loop brace.
    const double lx0 = view.beatToX(project.loopStart()), lx1 = view.beatToX(project.loopEnd());
    if (lx1 > 0 && lx0 < w) {
        const QColor color = project.loopEnabled() ? Theme::kLoopOn : Theme::kLoopOff;
        p.fillRect(QRectF(lx0, 2, lx1 - lx0, kLoopStrip - 4), color);
        p.fillRect(QRectF(lx0, 2, 2, kLoopStrip - 4), color.darker(140));
        p.fillRect(QRectF(lx1 - 2, 2, 2, kLoopStrip - 4), color.darker(140));
    }

    // Ticks and labels.
    const double scaleTop = kLoopStrip;
    const double scaleHeight = h - kLoopStrip;
    const double step = view.gridStep();
    const double every = timeline::labelStep(view, step);
    const QFont font = uiFont(8);
    const app::TimeSignature ts = project.timeSignature();
    for (const timeline::GridLine& line : timeline::gridLines(view, rect.left() - 60, rect.right() + 1, step)) {
        const double tick = line.kind == timeline::LineKind::Bar    ? scaleHeight
                            : line.kind == timeline::LineKind::Beat ? scaleHeight * 0.45
                                                                    : scaleHeight * 0.25;
        const double x = app::roundHalfEven(line.x);
        p.fillRect(QRectF(x, h - tick, 1, tick), line.kind == timeline::LineKind::Bar ? Theme::kTextDim : Theme::kGridBar);
        if (std::abs(line.beat / every - std::round(line.beat / every)) < 1e-6)
            p.drawText(QPointF(x + 3, scaleTop + 12), app::formatBarLabel(line.beat, ts), Theme::kText, font);
    }
    p.fillRect(QRectF(rect.left(), h - 1, rect.width(), 1), Theme::kBorder);

    // The start marker (insert position).
    const double sx = view.beatToX(session()->selection()->insertBeat());
    p.fillPolygon(QPolygonF({QPointF(sx - 5, scaleTop + 1), QPointF(sx + 5, scaleTop + 1), QPointF(sx, scaleTop + 8)}),
                  Theme::kInsertMarker);
}

ArrangementRuler::Zone ArrangementRuler::loopZone(double x) const {
    const app::Project& project = *session()->project();
    const timeline::Timeline& view = arrangement()->view();
    const double x0 = view.beatToX(project.loopStart()), x1 = view.beatToX(project.loopEnd());
    if (std::abs(x - x0) <= kEdgeGrab) return Zone::Start;
    if (std::abs(x - x1) <= kEdgeGrab) return Zone::End;
    if (x0 < x && x < x1) return Zone::Body;
    return Zone::None;
}

void ArrangementRuler::mousePressEvent(QMouseEvent* event) {
    if (event->flags() & Qt::MouseEventCreatedDoubleClick) return;  // (the double-click stands for it)
    if (event->button() != Qt::LeftButton || !ready()) return;
    const QPointF pos = event->position();
    const double beat = arrangement()->view().xToBeat(pos.x());
    const app::Project& project = *session()->project();
    Drag drag;
    drag.beat = beat;
    if (pos.y() < kLoopStrip) {
        switch (loopZone(pos.x())) {
            case Zone::Start: drag.mode = Mode::LoopStart; break;
            case Zone::End: drag.mode = Mode::LoopEnd; break;
            case Zone::Body: drag.mode = Mode::LoopBody; break;
            case Zone::None: drag.mode = Mode::LoopNew; break;
        }
        drag.start = project.loopStart();
        drag.end = project.loopEnd();
        drag.key = newGestureKey();
    } else {
        drag.mode = Mode::Scrub;
        drag.x = pos.x();
        drag.y = drag.lastY = pos.y();
        drag.scroll = arrangement()->scrollBeats();
    }
    drag_ = drag;
}

void ArrangementRuler::mouseMoveEvent(QMouseEvent* event) {
    if (!drag_ || !ready()) return;
    Drag& d = *drag_;
    const QPointF pos = event->position();
    const bool bypass = event->modifiers() & Qt::AltModifier;
    Arrangement& a = *arrangement();
    const timeline::Timeline& view = a.view();
    const double beat = view.xToBeat(pos.x());
    const auto snap = [&](double b) { return view.snapBeat(b, bypass); };
    app::ProjectEditor& editor = *session()->editor();
    const bool enabled = session()->project()->loopEnabled();
    switch (d.mode) {
        case Mode::LoopStart: editor.setLoop(enabled, std::min(snap(beat), d.end - 0.25), d.end, d.key); break;
        case Mode::LoopEnd: editor.setLoop(enabled, d.start, std::max(snap(beat), d.start + 0.25), d.key); break;
        case Mode::LoopBody: {
            const double start = std::max(0.0, snap(d.start + beat - d.beat));
            editor.setLoop(enabled, start, start + d.end - d.start, d.key);
            break;
        }
        case Mode::LoopNew: {
            const double a0 = snap(d.beat), b0 = snap(beat);
            const double lo = std::min(a0, b0), hi = std::max(a0, b0);
            if (hi - lo >= 0.25) editor.setLoop(true, lo, hi, d.key);
            break;
        }
        case Mode::Scrub: {
            const double dx = pos.x() - d.x, dy = pos.y() - d.y;
            if (!d.moved && std::max(std::abs(dx), std::abs(dy)) > 3) {
                d.moved = true;
                d.pan = std::abs(dx) > std::abs(dy);
            }
            if (d.moved) {
                if (d.pan) {
                    a.scrollByHand(d.scroll - dx / a.pxPerBeat());
                } else {
                    a.zoomAt(d.x, std::pow(1.012, pos.y() - d.lastY));
                    d.lastY = pos.y();
                }
            }
            break;
        }
    }
}

void ArrangementRuler::mouseReleaseEvent(QMouseEvent* event) {
    const std::optional<Drag> d = drag_;
    drag_.reset();
    if (d && d->mode == Mode::Scrub && !d->moved && ready()) {
        const bool bypass = event->modifiers() & Qt::AltModifier;
        session()->locate(std::max(0.0, arrangement()->view().snapBeat(d->beat, bypass)));
    }
}

void ArrangementRuler::mouseUngrabEvent() { drag_.reset(); }

void ArrangementRuler::mouseDoubleClickEvent(QMouseEvent* event) {
    if (!ready() || event->button() != Qt::LeftButton) return;
    if (event->position().y() < kLoopStrip && loopZone(event->position().x()) != Zone::None)
        session()->editor()->setLoopEnabled(!session()->project()->loopEnabled());
}

void ArrangementRuler::hoverMoveEvent(QHoverEvent* event) { updateCursor(event->position()); }

void ArrangementRuler::updateCursor(const QPointF& pos) {
    if (!ready()) return;
    Qt::CursorShape shape = Qt::ArrowCursor;
    if (pos.y() < kLoopStrip) {
        switch (loopZone(pos.x())) {
            case Zone::Start:
            case Zone::End: shape = Qt::SizeHorCursor; break;
            case Zone::Body: shape = Qt::OpenHandCursor; break;
            case Zone::None: break;
        }
    }
    setCursor(shape);
}

}  // namespace sub::ui
