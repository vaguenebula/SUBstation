#include "arrangement/BusLane.h"

#include "audio/EngineBridge.h"
#include "editor/ProjectEditor.h"
#include "model/Project.h"
#include "session/Selection.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"
#include "timeline/Timeline.h"

#include <QCursor>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QMouseEvent>

namespace sub::ui {

using namespace arrangement;

BusLane::BusLane(QQuickItem* parent) : ArrangementItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setAcceptHoverEvents(true);
}

BusLane::~BusLane() = default;

void BusLane::setOwner(const QString& owner) {
    if (owner == owner_) return;
    owner_ = owner;
    Q_EMIT ownerChanged();
    repaint();
}

void BusLane::connectSession(app::Session* session) {
    const auto repaint = [this] { this->repaint(); };
    app::Project* p = session->project();
    connect(p, &app::Project::settingsChanged, this, repaint);
    connect(p, &app::Project::automationChanged, this, repaint);
    connect(p, &app::Project::automationViewChanged, this, repaint);
    connect(p, &app::Project::devicesChanged, this, repaint);
    connect(p, &app::Project::trackChanged, this, repaint);
    connect(session->selection(), &app::Selection::changed, this, repaint);
    connect(session->bridge(), &app::EngineBridge::automationStateChanged, this, repaint);
}

void BusLane::connectArrangement(Arrangement* arrangement) {
    const auto repaint = [this] { this->repaint(); };
    connect(arrangement, &Arrangement::masterRowsChanged, this, repaint);
    connect(arrangement, &Arrangement::returnRowsChanged, this, repaint);
}

AutomationRows BusLane::rows() const {
    const Arrangement* a = arrangement();
    if (!a) return {};
    if (owner_ == app::kMaster) return a->masterRows();
    return a->returnRows(owner_).value_or(AutomationRows{kReturnHeight, {}});
}

std::vector<EnvelopeArea> BusLane::envelopeAreas() const {
    std::vector<EnvelopeArea> areas;
    if (!ready() || !session()->project()->hasOwner(owner_)) return areas;
    const app::AutomationView& view = session()->project()->automationView(owner_);
    if (!view.shown) return areas;
    const AutomationRows r = rows();
    const double w = width();
    if (view.key && !view.key->isEmpty()) areas.push_back({owner_, *view.key, -1, QRectF(0, 2, w, r.mainHeight - 3)});
    for (const LaneRow& lane : r.lanes)
        areas.push_back({owner_, lane.key, lane.index, QRectF(0, lane.top, w, lane.height - 1)});
    return areas;
}

void BusLane::updatePolish() {
    areas_.clear();
    looks_.clear();
    rows_ = rows();
    if (!ready()) return;
    areas_ = envelopeAreas();
    for (const EnvelopeArea& area : areas_) looks_.push_back(envelopes::lookOf(*session(), area.owner, area.key));
}

void BusLane::paint(SgPainter& p) {
    const QRectF visible = p.rect();
    p.fillRect(visible, Theme::lane());
    if (!ready()) return;
    const timeline::Timeline& view = arrangement()->view();
    timeline::drawGrid(p, view, visible.left(), visible.right(), 1, height());
    timeline::drawLoopRegion(p, view, visible.left(), visible.right(), 1, height());
    p.fillRect(QRectF(visible.left(), 0, visible.width(), 1), Theme::border());
    for (const LaneRow& lane : rows_.lanes)
        p.fillRect(QRectF(visible.left(), lane.top - 1, visible.width(), 1), Theme::gridBar());
    const app::Project& project = *session()->project();
    const app::Selection& selection = *session()->selection();
    for (size_t i = 0; i < areas_.size() && i < looks_.size(); ++i) {
        const EnvelopeArea& area = areas_[i];
        envelopes::drawArea(p, view, selection, area, project.envelope(area.owner, area.key), looks_[i], visible,
                            hoverPoint_, false, gesture_ != nullptr);
    }
    envelopes::drawRange(p, view, selection, areas_, Theme::selection());
    envelopes::drawReadout(p, width(), gesture_ ? gesture_->readout() : std::nullopt);
}

// --- Mouse: automation -------------------------------------------------------------------------

void BusLane::press(const QPointF& pos, Qt::KeyboardModifiers modifiers) {
    if (const auto area = envelopes::areaAt(envelopeAreas(), pos)) {
        gesture_ = envelopes::press(*this, *area, pos, modifiers);
        repaint();
    }
}

void BusLane::mousePressEvent(QMouseEvent* event) {
    if (event->flags() & Qt::MouseEventCreatedDoubleClick) return;  // (the double-click stands for it)
    if (!ready()) return;
    if (event->button() == Qt::RightButton) {
        menu_ = contextMenu(event->position());
        if (!menu_.isEmpty()) Q_EMIT menuRequested(menu_.toVariant(), event->position());
        return;
    }
    if (event->button() != Qt::LeftButton) return;
    gesture_.reset();
    press(event->position(), event->modifiers());
}

void BusLane::mouseMoveEvent(QMouseEvent* event) {
    hoverPos_ = event->position();
    if (gesture_) {
        gesture_->move(event->position(), event->modifiers());
        repaint();
        return;
    }
    updateHover(event->position(), event->modifiers());
}

void BusLane::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    std::unique_ptr<Gesture> gesture = std::move(gesture_);
    if (gesture) gesture->finish();
    if (ready()) updateHover(event->position(), event->modifiers());
    repaint();
}

void BusLane::mouseDoubleClickEvent(QMouseEvent* event) {
    // Each click of a double-click counts on its own.
    if (event->button() != Qt::LeftButton || !ready()) return;
    gesture_.reset();
    press(event->position(), event->modifiers());
}

void BusLane::mouseUngrabEvent() {
    if (!gesture_) return;
    gesture_->cancel();
    gesture_.reset();
    repaint();
}

void BusLane::hoverMoveEvent(QHoverEvent* event) {
    hoverPos_ = event->position();
    if (!gesture_ && ready()) updateHover(event->position(), event->modifiers());
}

void BusLane::hoverLeaveEvent(QHoverEvent*) {
    hoverPos_.reset();
    if (hoverPoint_) {
        hoverPoint_.reset();
        repaint();
    }
}

void BusLane::keyPressEvent(QKeyEvent* event) {
    if (hoverPos_ && !gesture_) updateHover(*hoverPos_, event->modifiers());
    event->ignore();
}

void BusLane::keyReleaseEvent(QKeyEvent* event) {
    if (hoverPos_ && !gesture_) updateHover(*hoverPos_, event->modifiers());
    event->ignore();
}

void BusLane::updateHover(const QPointF& pos, Qt::KeyboardModifiers modifiers) {
    if (!ready()) return;
    const auto area = envelopes::areaAt(envelopeAreas(), pos);
    auto [point, cursor] = envelopes::hover(*this, area, pos, modifiers);
    if (point != hoverPoint_) {
        hoverPoint_ = point;
        repaint();
    }
    setCursor(cursor);
}

MenuEntries BusLane::contextMenu(const QPointF& pos) {
    MenuEntries menu;
    if (!ready() || !session()->project()->hasOwner(owner_)) return menu;
    app::ProjectEditor* editor = session()->editor();
    const QString owner = owner_;
    if (const auto area = envelopes::areaAt(envelopeAreas(), pos)) {
        envelopes::addMenuEntries(*this, *area, pos, menu);
    } else if (session()->project()->automationView(owner).shown) {
        menu.add(QStringLiteral("Hide Automation"), [editor, owner] { editor->hideAutomation(owner); });
    } else {
        menu.add(QStringLiteral("Show Automation"), [editor, owner] { editor->showAutomation(owner); });
    }
    return menu;
}

void BusLane::triggerMenu(int id) {
    const MenuEntries menu = menu_;
    menu.trigger(id);
}

}  // namespace sub::ui
