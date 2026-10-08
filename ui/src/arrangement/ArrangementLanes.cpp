#include "arrangement/ArrangementLanes.h"

#include "arrangement/ClipGestures.h"
#include "arrangement/Cursors.h"
#include "audio/AudioFiles.h"
#include "audio/EngineBridge.h"
#include "browser/BrowserController.h"
#include "browser/BrowserMime.h"
#include "editor/ProjectEditor.h"
#include "model/Devices.h"
#include "model/Edits.h"
#include "model/Errors.h"
#include "model/Numbers.h"
#include "model/Project.h"
#include "model/Timebase.h"
#include "session/ArrangementActions.h"
#include "session/ComputerKeyboard.h"
#include "session/Selection.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"
#include "timeline/Timeline.h"

#include <QCoreApplication>
#include <QCursor>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QLineF>
#include <QMimeData>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QUrl>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace sub::ui {

using namespace arrangement;

double arrangement::clipTitleHeight(double clipHeight, bool folded) {
    if (folded) return std::min(clipHeight, kTitleHeight);  // (the bar a folded track's row is)
    return clipHeight >= kMinTitleRow ? kTitleHeight : kShortTitleHeight;
}

FadeHandles arrangement::fadeHandles(const app::Clip& clip, const QRectF& rect, const QRectF& body, double pxPerBeat,
                                     double tempo) {
    const double top = body.top() + 1, bottom = body.bottom() - 1;
    const double inWidth = clip.fadeInBeats(tempo) * pxPerBeat, outWidth = clip.fadeOutBeats(tempo) * pxPerBeat;
    // The squares stay inside the clip (at its corners while it has no fades).
    const double lo = rect.left() + kFadeHandle / 2, hi = std::max(lo, rect.right() - kFadeHandle / 2);
    const auto square = [&](double x) {
        return QRectF(std::clamp(x, lo, hi) - kFadeHandle / 2, top, kFadeHandle, kFadeHandle);
    };
    const auto dot = [&](double x, double curve) {
        return QPointF(x, bottom - app::Clip::fadeGain(0.5, curve) * (bottom - top));
    };
    FadeHandles handles{square(rect.left() + inWidth), square(rect.right() - outWidth), std::nullopt, std::nullopt};
    if (inWidth >= kMinCurveFade) handles.inCurve = dot(rect.left() + inWidth / 2, clip.fadeInCurve);
    if (outWidth >= kMinCurveFade) handles.outCurve = dot(rect.right() - outWidth / 2, clip.fadeOutCurve);
    return handles;
}

namespace {

constexpr QColor kDropFill{255, 166, 43, 70};
constexpr QColor kFadeVeil{0, 0, 0, 80};          // over what a fade takes away
constexpr QColor kFadeLine{255, 255, 255, 190};   // its curve
constexpr QColor kFadeHandleFill{235, 235, 235};
constexpr QColor kFadeHandleEdge{20, 20, 20, 200};

// Ctrl+Shift (without Alt): drag a clip's body to slide its content.
bool isSlipModifier(Qt::KeyboardModifiers modifiers) {
    return (modifiers & Qt::ControlModifier) && (modifiers & Qt::ShiftModifier) && !(modifiers & Qt::AltModifier);
}

// Alt (without Ctrl): drag a clip's edge to stretch it.
bool isStretchModifier(Qt::KeyboardModifiers modifiers) {
    return (modifiers & Qt::AltModifier) && !(modifiers & Qt::ControlModifier);
}

// The modifiers held once a key event is through: a modifier key's own press
// or release isn't in its event's modifiers on every platform.
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

// Audio files dragged in: local URLs with an audio extension.
QStringList audioPaths(const QMimeData* mime) {
    QStringList paths;
    if (!mime || !mime->hasUrls()) return paths;
    for (const QUrl& url : mime->urls()) {
        if (url.isLocalFile() && app::isAudioFile(url.toLocalFile())) paths << url.toLocalFile();
    }
    return paths;
}

// Devices dragged from the browser: built-in kinds, and plug-ins (PluginRef fields).
struct DroppedDevices {
    QStringList kinds;
    QVariantList plugins;
    bool anyInstrument = false;
    bool empty() const { return kinds.isEmpty() && plugins.isEmpty(); }
};
DroppedDevices droppedDevices(const QMimeData* mime) {
    DroppedDevices devices;
    for (const QString& kind : app::deviceKinds(mime)) {
        if (app::builtinDevice(kind) == nullptr) continue;
        devices.kinds << kind;
        devices.anyInstrument = devices.anyInstrument || app::isInstrument(kind);
    }
    for (const app::PluginInfo& ref : app::pluginRefs(mime)) {
        devices.plugins.append(ref.toRef());
        devices.anyInstrument = devices.anyInstrument || ref.instrument;
    }
    return devices;
}

// A rectangle's outline in Qt's DashLine (4 px dashes, 2 px gaps), as a 1 px pen draws it.
void dashedRect(SgPainter& p, const QRectF& rect, const QColor& color) {
    const auto dashes = [&](double x0, double y0, double x1, double y1) {
        const double length = std::hypot(x1 - x0, y1 - y0);
        if (length <= 0) return;
        const double dx = (x1 - x0) / length, dy = (y1 - y0) / length;
        for (double t = 0; t < length; t += 6.0) {
            const double e = std::min(t + 4.0, length);
            p.fillRect(QRectF(std::min(x0 + dx * t, x0 + dx * e), std::min(y0 + dy * t, y0 + dy * e),
                              std::max(1.0, std::abs(dx) * (e - t)), std::max(1.0, std::abs(dy) * (e - t))),
                       color);
        }
    };
    const double l = std::floor(rect.left()), t = std::floor(rect.top());
    const double r = std::floor(rect.right()), b = std::floor(rect.bottom());
    dashes(l, t, r, t);
    dashes(r, t, r, b);
    dashes(l, b, r, b);
    dashes(l, t, l, b);
}

}  // namespace

ArrangementLanes::ArrangementLanes(QQuickItem* parent) : ArrangementItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setAcceptHoverEvents(true);
    setFlag(ItemAcceptsDrops, true);
    setFlag(ItemIsFocusScope, false);
    setActiveFocusOnTab(true);
    if (QCoreApplication* app = QCoreApplication::instance()) app->installEventFilter(this);
}

ArrangementLanes::~ArrangementLanes() {
    if (QCoreApplication* app = QCoreApplication::instance()) app->removeEventFilter(this);
}

void ArrangementLanes::connectSession(app::Session* session) {
    const auto repaint = [this] { this->repaint(); };
    app::Project* p = session->project();
    connect(session->selection(), &app::Selection::changed, this, repaint);
    connect(session->selection(), &app::Selection::insertChanged, this, repaint);
    for (auto signal : {&app::Project::trackChanged, &app::Project::clipsChanged, &app::Project::devicesChanged,
                        &app::Project::freezeChanged, &app::Project::automationViewChanged})
        connect(p, signal, this, repaint);
    connect(p, &app::Project::settingsChanged, this, repaint);
    connect(p, &app::Project::automationChanged, this, repaint);
    connect(p, &app::Project::reset, this, [this] {
        waveforms_.clear();
        this->repaint();
    });
    app::EngineBridge* bridge = session->bridge();
    connect(bridge, &app::EngineBridge::sourceReady, this, repaint);
    connect(bridge, &app::EngineBridge::sourceFailed, this, repaint);
    connect(bridge, &app::EngineBridge::automationStateChanged, this, repaint);
    // A parameter set by hand: lanes without an envelope draw its value.
    connect(p, &app::Project::deviceParamChanged, this, &ArrangementLanes::updateIfShown);
    connect(p, &app::Project::deviceStateChanged, this, &ArrangementLanes::updateIfShown);
    connect(bridge, &app::EngineBridge::pluginParamEdited, this, &ArrangementLanes::updateIfShown);
    connect(bridge, &app::EngineBridge::pluginParamsChanged, this, &ArrangementLanes::updateIfShown);
    connect(bridge, &app::EngineBridge::pluginParamsRebuilt, this, &ArrangementLanes::updateIfShown);
    connect(bridge, &app::EngineBridge::devicesLoaded, this, &ArrangementLanes::updateIfShown);
}

void ArrangementLanes::connectArrangement(Arrangement* arrangement) {
    const auto repaint = [this] { this->repaint(); };
    connect(arrangement, &Arrangement::vscrollChanged, this, repaint);
    connect(arrangement, &Arrangement::layoutChanged, this, repaint);
    arrangement->setLanesWidth(width());
    arrangement->setLanesHeight(height());
}

void ArrangementLanes::updateIfShown(const QString& trackId) {
    const app::Project* p = session() ? session()->project() : nullptr;
    const app::Track* track = p && p->hasTrack(trackId) ? p->findTrack(trackId) : nullptr;
    if (track && track->automationView.shown) repaint();
}

void ArrangementLanes::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    ArrangementItem::geometryChange(newGeometry, oldGeometry);
    if (Arrangement* a = arrangement()) {
        a->setLanesWidth(newGeometry.width());
        a->setLanesHeight(newGeometry.height());
    }
    repaint();
}

// --- Geometry ----------------------------------------------------------------------------------

std::optional<int> ArrangementLanes::rowIndexAt(double y, bool clamp) const {
    const Arrangement* a = arrangement();
    if (!a) return std::nullopt;
    const double contentY = y + a->scrollY();
    std::optional<int> index = a->layout().rowIndexAt(contentY);
    if (!index && clamp && !a->layout().rows().empty())
        return contentY < 0 ? std::optional<int>(0) : a->layout().lastShownIndex();
    return index;
}

QRectF ArrangementLanes::clipRect(const app::Clip& clip, double rowTop, int rowHeight) const {
    const timeline::Timeline& view = arrangement()->view();
    const double x0 = view.beatToX(clip.startBeat);
    const double x1 = view.beatToX(clip.endBeat(session()->project()->tempo()));
    return QRectF(x0, rowTop + 1, std::max(2.0, x1 - x0), rowHeight - 3);
}

bool ArrangementLanes::inClipBand(const QPointF& pos) const {
    const auto index = rowIndexAt(pos.y(), true);
    if (!index) return false;
    const Row& row = arrangement()->layout().rows()[static_cast<size_t>(*index)];
    const double y = pos.y() + arrangement()->scrollY() - row.top;
    return y < row.mainHeight && (row.mainHeight < kMinTitleRow || (0 <= y && y < kTitleHeight + 1));
}

std::vector<EnvelopeArea> ArrangementLanes::envelopeAreas() const {
    std::vector<EnvelopeArea> areas;
    if (!ready()) return areas;
    const app::Project& project = *session()->project();
    const double w = width(), scroll = arrangement()->scrollY();
    for (const Row& row : arrangement()->layout().rows()) {
        if (!row.automation || row.bottom() - scroll < 0 || row.top - scroll > height()) continue;
        const app::Track* track = project.findTrack(row.trackId);
        if (!track) continue;
        const double top = row.top - scroll;
        if (const auto& key = track->automationView.key; key && !key->isEmpty()) {
            areas.push_back({row.trackId, *key, -1,
                             QRectF(0, top + kTitleHeight + 1, w, row.mainHeight - kTitleHeight - 2)});
        }
        for (const LaneRow& lane : row.lanes)
            areas.push_back({row.trackId, lane.key, lane.index, QRectF(0, lane.top - scroll, w, lane.height - 1)});
    }
    return areas;
}

std::optional<EnvelopeArea> ArrangementLanes::envelopeAreaAt(const QPointF& pos) const {
    return envelopes::areaAt(envelopeAreas(), pos);
}

std::optional<ArrangementLanes::Hit> ArrangementLanes::hitClip(const QPointF& pos) const {
    const auto index = rowIndexAt(pos.y());
    if (!index) return std::nullopt;
    const Row& row = arrangement()->layout().rows()[static_cast<size_t>(*index)];
    const double top = row.top - arrangement()->scrollY();
    if (pos.y() >= top + row.mainHeight) return std::nullopt;  // in an automation lane below the track
    const app::Track* track = session()->project()->findTrack(row.trackId);
    if (!track) return std::nullopt;
    for (auto it = track->clips.rbegin(); it != track->clips.rend(); ++it) {
        const QRectF rect = clipRect(*it, top, row.mainHeight);
        // Only inside the clip: next to it, or on a neighbour's side of a
        // shared boundary, you are not trimming this clip.
        if (rect.left() <= pos.x() && pos.x() <= rect.right()) {
            if (!row.bars && pos.y() >= rect.top() + clipTitleHeight(rect.height())) return Hit{row.trackId, *it, Zone::Body};
            const double grab = std::min(kEdgeGrab, rect.width() / 3);
            Zone zone = Zone::Title;
            if (pos.x() <= rect.left() + grab)
                zone = Zone::Left;
            else if (pos.x() >= rect.right() - grab)
                zone = Zone::Right;
            return Hit{row.trackId, *it, zone};
        }
    }
    return std::nullopt;
}

std::optional<ArrangementLanes::FadeHit> ArrangementLanes::hitFade(const QPointF& pos) const {
    if (!ready()) return std::nullopt;
    const auto index = rowIndexAt(pos.y());
    if (!index) return std::nullopt;
    const Row& row = arrangement()->layout().rows()[static_cast<size_t>(*index)];
    const double top = row.top - arrangement()->scrollY();
    if (row.bars || pos.y() >= top + row.mainHeight) return std::nullopt;  // (a folded track's bars have no body)
    const app::Track* track = session()->project()->findTrack(row.trackId);
    if (!track) return std::nullopt;
    const double tempo = session()->project()->tempo(), px = arrangement()->pxPerBeat();
    for (auto it = track->clips.rbegin(); it != track->clips.rend(); ++it) {
        if (!it->isAudio()) continue;
        const QRectF rect = clipRect(*it, top, row.mainHeight);
        if (pos.x() < rect.left() - kFadeGrab || pos.x() > rect.right() + kFadeGrab) continue;
        const QRectF body = rect.adjusted(0, clipTitleHeight(rect.height()), 0, 0);
        if (body.height() < kMinFadeBody) continue;
        const FadeHandles handles = fadeHandles(*it, rect, body, px, tempo);
        // The dots first (inside the fades, they are clear of the squares); of two squares, the nearer.
        for (const bool out : {false, true}) {
            const auto& dot = out ? handles.outCurve : handles.inCurve;
            if (dot && QLineF(*dot, pos).length() <= kFadeDot + kFadeGrab) return FadeHit{row.trackId, *it, out, true};
        }
        std::optional<FadeHit> nearest;
        double distance = std::numeric_limits<double>::infinity();
        for (const bool out : {false, true}) {
            const QRectF square = out ? handles.out : handles.in;
            if (!square.adjusted(-kFadeGrab, -kFadeGrab, kFadeGrab, kFadeGrab).contains(pos)) continue;
            const double d = QLineF(square.center(), pos).length();
            if (d < distance) {
                distance = d;
                nearest = FadeHit{row.trackId, *it, out, false};
            }
        }
        if (nearest) return nearest;
    }
    return std::nullopt;
}

bool ArrangementLanes::inClipRange(const QPointF& pos) const { return inClipBand(pos) && inSelection(pos); }

bool ArrangementLanes::inSelection(const QPointF& pos) const {
    const app::Selection& selection = *session()->selection();
    const auto index = rowIndexAt(pos.y());
    if (!selection.clipRange() || !index) return false;
    // (On the rows it shows over: a group's tracks it doesn't reach aren't shown selected.)
    const app::TimeRange& range = *selection.timeRange();
    const double beat = arrangement()->view().xToBeat(pos.x());
    return selection.rangeRows().contains(arrangement()->layout().rows()[static_cast<size_t>(*index)].trackId) &&
           range.start <= beat && beat <= range.end;
}

// --- Painting -------------------------------------------------------------------------------------

void ArrangementLanes::updatePolish() {
    areas_.clear();
    looks_.clear();
    if (!ready()) return;
    areas_ = envelopeAreas();
    for (const EnvelopeArea& area : areas_) looks_.push_back(envelopes::lookOf(*session(), area.owner, area.key));
}

QHash<QString, QRectF> ArrangementLanes::selectedAreas(const app::TimeRange& range) const {
    // Where a time selection over tracks is tinted, by track: its stretch of
    // the lane of each track whose row it covers (`range.trackIds`: not a
    // group's tracks below the rows it reaches, which it acts on all the same),
    // and of the automation lanes below it (unless automation is locked: then
    // the selection leaves it where it is).
    const timeline::Timeline& view = arrangement()->view();
    const double x0 = view.beatToX(range.start), x1 = view.beatToX(range.end);
    const bool locked = session()->project()->automationLocked();
    QHash<QString, QRectF> areas;
    for (const QString& trackId : range.trackIds) {
        const Row* row = arrangement()->layout().rowFor(trackId);
        if (!row || row->hidden) continue;
        const int height = locked ? row->mainHeight : row->height();
        areas.insert(trackId, QRectF(x0, row->top - arrangement()->scrollY(), x1 - x0, height - 1));
    }
    return areas;
}

void ArrangementLanes::paint(SgPainter& p) {
    const QRectF visible = p.rect();
    p.fillRect(visible, Theme::kEmptyArea);
    if (!ready()) return;
    const Arrangement& a = *arrangement();
    const timeline::Timeline& view = a.view();
    const app::Project& project = *session()->project();
    const app::Selection& selection = *session()->selection();
    const TrackLayout& layout = a.layout();
    const int scroll = a.scrollY();

    const std::vector<int> rows = layout.visibleRows(scroll + visible.top(), scroll + visible.bottom() + 1);
    const QStringList selectedTracks = selection.trackIds();
    for (int index : rows) {
        const Row& row = layout.rows()[static_cast<size_t>(index)];
        const QColor color = selectedTracks.contains(row.trackId) ? Theme::kLaneSelected : Theme::kLane;
        p.fillRect(QRectF(visible.left(), row.top - scroll, visible.width(), row.height()), color);
    }
    // The grid goes all the way down: below the tracks too, where selecting works on it as well.
    timeline::drawGrid(p, view, visible.left(), visible.right(), visible.top(), visible.bottom() + 1);
    timeline::drawLoopRegion(p, view, visible.left(), visible.right(), 0.0, height());

    const Gesture* gesture = gesture_.get();
    const std::optional<app::TimeRange> gestureRange = gesture ? gesture->timeRange() : std::nullopt;
    std::optional<app::TimeRange> timeRange = gestureRange ? gestureRange : selection.timeRange();
    if (timeRange && !gestureRange) timeRange->trackIds = selection.rangeRows();  // (the rows it shows over)
    const bool onLanes = timeRange && !selection.lanes().isEmpty() && !gestureRange;
    QHash<QString, QRectF> tinted = !timeRange || onLanes ? QHash<QString, QRectF>() : selectedAreas(*timeRange);
    std::vector<Frame> frames;  // the clips drawn

    const QSet<QString> hidden = gesture ? gesture->hiddenIds() : QSet<QString>();
    const QSet<app::ClipRef>& selectedClips = selection.clips();
    for (int index : rows) {
        const Row& row = layout.rows()[static_cast<size_t>(index)];
        const app::Track* track = project.findTrack(row.trackId);
        if (!track) continue;
        const double y = row.top - scroll;
        const bool bars = row.bars;
        if (bars && tinted.contains(track->id))  // under its clips' bars, which stay as they are
            p.fillRect(tinted.take(track->id), Theme::kSelection);
        if (track->isGroup()) drawGroupSummary(p, track->id, y, row.mainHeight, track->folded, visible);
        const QColor trackColor(track->color);
        for (const app::Clip& clip : track->clips) {
            if (hidden.contains(clip.id)) continue;
            const QRectF rect = clipRect(clip, y, row.mainHeight);
            if (rect.left() > visible.right()) break;
            if (rect.right() < visible.left()) continue;
            // The selected area is tinted; a folded track's bars show they are selected by their outline.
            const bool selected = bars && selectedClips.contains(app::ClipRef{track->id, clip.id});
            drawClip(p, trackColor, clip, rect, visible, selected, false, bars);
            frames.push_back({track->id, trackColor, clip, rect, selected, false, bars});
        }
        if (project.isFrozen(track->id))  // its clips play as frozen: tinted, as in Ableton
            p.fillRect(QRectF(visible.left(), y, visible.width(), row.mainHeight - 1), Theme::kFrozenTint);
        for (const LaneRow& lane : row.lanes)  // automation lanes below the track
            p.fillRect(QRectF(visible.left(), lane.top - scroll - 1, visible.width(), 1), Theme::kGridBar);
        p.fillRect(QRectF(visible.left(), y + row.height() - 1, visible.width(), 1), Theme::kBorder);
    }

    if (gesture) {
        const auto& allRows = layout.rows();
        for (const GestureClip& kept : gesture->kept()) {
            if (kept.row < 0 || kept.row >= static_cast<int>(allRows.size())) continue;
            const Row& row = allRows[static_cast<size_t>(kept.row)];
            if (row.hidden) continue;
            const QRectF rect = clipRect(kept.clip, row.top - scroll, row.mainHeight);
            drawClip(p, kept.color, kept.clip, rect, visible, false, false, row.bars);
            if (project.isFrozen(row.trackId)) p.fillRect(rect, Theme::kFrozenTint);  // (as its row's clips are)
            frames.push_back({row.trackId, kept.color, kept.clip, rect, false, false, row.bars});
        }
        for (const GestureClip& ghost : gesture->ghosts()) {
            if (ghost.row < 0 || ghost.row >= static_cast<int>(allRows.size())) continue;
            const Row& row = allRows[static_cast<size_t>(ghost.row)];
            if (row.hidden) continue;
            const QRectF rect = clipRect(ghost.clip, row.top - scroll, row.mainHeight);
            drawClip(p, ghost.color, ghost.clip, rect, visible, true, true, row.bars);
            if (project.isFrozen(row.trackId)) p.fillRect(rect, Theme::kFrozenTint);
            frames.push_back({row.trackId, ghost.color, ghost.clip, rect, true, true, row.bars});
        }
    }
    for (size_t i = 0; i < areas_.size() && i < looks_.size(); ++i) {
        const EnvelopeArea& area = areas_[i];
        envelopes::drawArea(p, view, selection, area, project.envelope(area.owner, area.key), looks_[i], visible,
                            hoverPoint_, area.lane < 0, gesture != nullptr);
    }
    drawDropPreview(p);

    if (project.tracks().empty() && !dropPreview_) {
        p.drawText(visible, Qt::AlignCenter,
                   QStringLiteral("Drag audio files here from the browser\nor press Ctrl+T to create an audio "
                                  "track, Ctrl+Shift+T for a MIDI track"),
                   Theme::kTextDim, uiFont(10));
    }

    if (onLanes) envelopes::drawRange(p, view, selection, areas_, Theme::kSelection);  // on the automation lanes it covers
    for (const QRectF& area : tinted) p.fillRect(area, Theme::kSelection);
    // Over the tint, the clips' title bars (and outlines) as they were: selecting doesn't light them up.
    for (const Frame& frame : frames) {
        const auto area = tinted.constFind(frame.trackId);
        if (area != tinted.constEnd() && area->intersects(frame.rect)) {
            p.save();
            p.setClipRect(area->intersected(visible));
            drawClipFrame(p, frame.color, frame.clip, frame.rect, frame.selected, frame.ghost, frame.folded);
            p.restore();
        }
    }

    // The insert marker on the selected track (Ableton's blinking cursor, minus the blink).
    if (!selection.trackId().isEmpty() && selection.clips().isEmpty() && !timeRange) {
        if (const Row* row = layout.rowFor(selection.trackId())) {
            const double x = app::roundHalfEven(view.beatToX(selection.insertBeat()));
            if (row->height() > 1)
                p.fillRect(QRectF(x, row->top - scroll, 1, row->height() - 1), Theme::kInsertMarker);
        }
    }
    envelopes::drawReadout(p, width(), gesture ? gesture->readout() : std::nullopt);
}

void ArrangementLanes::drawClip(SgPainter& p, const QColor& trackColor, const app::Clip& clip, const QRectF& rect,
                                const QRectF& visible, bool selected, bool ghost, bool folded) {
    // A clip: its body (the waveform or notes), then its title bar and outline
    // (drawClipFrame). A folded track's clip is all title bar.
    const double titleHeight = clipTitleHeight(rect.height(), folded);
    const QRectF body = rect.adjusted(0, titleHeight, 0, 0);
    p.save();
    p.setClipRect(rect.intersected(visible).adjusted(-1, -1, 1, 1));
    if (body.height() > 0) {
        QColor bodyColor = trackColor;
        bodyColor.setHsvF(trackColor.hsvHueF(), trackColor.hsvSaturationF() * 0.6f,
                          std::min(1.0f, trackColor.valueF() * 0.78f));
        if (ghost) bodyColor.setAlphaF(0.75f);
        p.fillRect(body, bodyColor);
        // The grid shows through the body, faintly (under the notes and the waveform);
        // the title bar, where the clip is grabbed, stays solid.
        timeline::drawGrid(p, arrangement()->view(), std::max(rect.left() + 1, visible.left()),
                           std::min(rect.right() - 1, visible.right()), body.top(), body.bottom(), true);
        drawContent(p, clip, rect, body, visible);
        drawFades(p, clip, rect, body, visible, showsFadeHandles());
    }
    drawClipFrame(p, trackColor, clip, rect, selected, ghost, folded);
    p.restore();
}

void ArrangementLanes::drawFades(SgPainter& p, const app::Clip& clip, const QRectF& rect, const QRectF& body,
                                 const QRectF& visible, bool handles) const {
    // An audio clip's fades: each a curve from silence (the body's bottom) to
    // full level (its top), every few pixels, with a veil over what it takes
    // away; with F held, the handles that drag them.
    if (!clip.isAudio() || body.height() < 4) return;
    const double tempo = session()->project()->tempo(), px = arrangement()->pxPerBeat();
    const double top = body.top() + 1, bottom = body.bottom() - 1;
    p.save();
    p.setAntialiasing(true);
    for (const bool out : {false, true}) {
        const double width = (out ? clip.fadeOutBeats(tempo) : clip.fadeInBeats(tempo)) * px;
        if (width < 1) continue;
        const double x0 = out ? rect.right() - width : rect.left();
        const double from = std::max(x0, visible.left() - 2), to = std::min(x0 + width, visible.right() + 2);
        if (to <= from) continue;
        const double curve = out ? clip.fadeOutCurve : clip.fadeInCurve;
        const int steps = std::max(2, static_cast<int>(std::ceil((to - from) / kSamplePixels)));
        QPolygonF line;
        for (int i = 0; i <= steps; ++i) {
            const double x = from + (to - from) * i / steps;
            const double along = (x - x0) / width;  // from the fade's start; a fade out's gain counts from the end
            const double gain = app::Clip::fadeGain(out ? 1.0 - along : along, curve);
            line << QPointF(x, bottom - gain * (bottom - top));
        }
        p.fillToBaseline(line.constData(), static_cast<int>(line.size()), body.top(), kFadeVeil);
        p.drawPolyline(line, kFadeLine, 1.0);
    }
    if (handles && body.height() >= kMinFadeBody) {
        const FadeHandles at = fadeHandles(clip, rect, body, px, tempo);
        const auto lit = [&](bool out, bool curve) {
            return hoverFade_ && std::get<0>(*hoverFade_) == clip.id && std::get<1>(*hoverFade_) == out &&
                   std::get<2>(*hoverFade_) == curve;
        };
        for (const bool out : {false, true}) {
            if (const auto& dot = out ? at.outCurve : at.inCurve) {
                p.fillEllipse(*dot, kFadeDot + 1, kFadeDot + 1, kFadeHandleEdge);
                p.fillEllipse(*dot, kFadeDot, kFadeDot, lit(out, true) ? Theme::kAccent : kFadeHandleFill);
            }
            const QRectF square = out ? at.out : at.in;
            p.fillRect(square, lit(out, false) ? Theme::kAccent : kFadeHandleFill);
            p.drawRect(square.adjusted(0.5, 0.5, -0.5, -0.5), kFadeHandleEdge, 1);
        }
    }
    p.restore();
}

void ArrangementLanes::drawContent(SgPainter& p, const app::Clip& clip, const QRectF& rect, const QRectF& body,
                                   const QRectF& visible) {
    // A MIDI clip's notes, or an audio clip's waveform (as loud as its gain makes it).
    if (clip.isMidi()) {
        drawNotes(p, clip, body.adjusted(0, 2, 0, -2), visible);
        return;
    }
    const app::EngineBridge& bridge = *session()->bridge();
    const app::Waveform source = bridge.waveform(clip.path);
    if (!source.isNull()) {
        const QRectF wave = body.adjusted(0, 1, 0, -1);
        p.save();
        p.setClipRect(wave.intersected(visible));
        const double tempo = session()->project()->tempo();
        waveforms_.draw(p, source, wave, rect.left(), clip.offsetSec,
                        arrangement()->view().framesPerPixel(source.sampleRate(), clip.sourceTempo(tempo)),
                        Theme::kWaveform, wave.height() >= 44, visible, app::dbToGain(clip.gainDb));
        p.restore();
    } else if (body.height() > 10 && rect.width() > 40) {
        const QString error = bridge.loadError(clip.path);
        if (!error.isEmpty()) p.fillRect(body, QColor(200, 60, 60, 110));
        p.drawText(body.adjusted(4, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft,
                   error.isEmpty() ? QStringLiteral("Loading…") : QStringLiteral("Missing file"), Theme::kAccentText,
                   uiFont(7.5));
    }
}

void ArrangementLanes::drawClipFrame(SgPainter& p, const QColor& trackColor, const app::Clip& clip, const QRectF& rect,
                                     bool selected, bool ghost, bool folded) const {
    // A clip's title bar (its name, if there is room) and its outline: white
    // if `selected`, and the trim handle under the mouse.
    const double titleHeight = clipTitleHeight(rect.height(), folded);
    const QRectF title(rect.left(), rect.top(), rect.width(), titleHeight);
    if (titleHeight > 0) p.fillRect(title, trackColor);
    if ((titleHeight >= kTitleHeight || folded) && rect.width() > 16) {  // (no name in a thin bar)
        const QFont font = uiFont(7.5);
        const QRectF textRect = title.adjusted(4, 0, -3, 0);
        p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                   SgPainter::elidedText(clip.name, font, std::floor(textRect.width())), Theme::kAccentText, font);
    }
    const QColor outline = selected ? Theme::kSelectionOutline : trackColor.darker(170);
    p.drawRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), outline, 1);
    if (hoverEdge_ && hoverEdge_->first == clip.id && !ghost) {  // (Alt: a stretch, in the accent colour)
        const double x = hoverEdge_->second ? rect.left() : rect.right() - 2;
        p.fillRect(QRectF(x, rect.top(), 2, rect.height()), hoverStretch_ ? Theme::kAccent : Theme::kSelectionOutline);
    }
}

void ArrangementLanes::drawGroupSummary(SgPainter& p, const QString& groupId, double rowTop, int rowHeight,
                                        bool folded, const QRectF& visible) const {
    // What is in a group, as Ableton's group lanes show it: a thin row per track
    // in it (those in groups in it too), in order, with that track's clips as
    // bars, so the group's structure shows: in each track's colour while folded
    // (its tracks are hidden), barely there while open (its tracks show below it).
    const QRectF area(visible.left(), rowTop + 2, visible.width(), rowHeight - 5);
    if (area.height() < 1) return;
    const app::Project& project = *session()->project();
    std::vector<const app::Track*> tracks;
    for (const app::Track* track : project.descendants(groupId)) {
        if (track->hasClips()) tracks.push_back(track);
    }
    if (tracks.empty()) return;
    const timeline::Timeline& view = arrangement()->view();
    const double tempo = project.tempo();
    const double share = area.height() / static_cast<double>(tracks.size());
    const double gap = share >= 3.0 ? 1.0 : 0.0;  // a line between the rows, while they have room for one
    p.save();
    p.setClipRect(area);
    for (size_t i = 0; i < tracks.size(); ++i) {
        // Whole pixels, so the rows (and the lines between them) are even.
        const double top = area.top() + std::floor(static_cast<double>(i) * share);
        const double bottom = area.top() + std::floor(static_cast<double>(i + 1) * share) - gap;
        if (bottom <= top) continue;
        const QColor color = folded ? QColor(tracks[i]->color) : Theme::kGroupOutline;
        for (const app::Clip& clip : tracks[i]->clips) {
            const double x0 = view.beatToX(clip.startBeat), x1 = view.beatToX(clip.endBeat(tempo));
            if (x1 < visible.left()) continue;
            if (x0 > visible.right()) break;
            p.fillRect(QRectF(x0, top, std::max(1.0, x1 - x0), bottom - top), color);
        }
    }
    p.restore();
}

void ArrangementLanes::drawNotes(SgPainter& p, const app::Clip& clip, const QRectF& area, const QRectF& visible) const {
    // The notes a MIDI clip plays, fitted to the clip's height (as in Ableton).
    const std::vector<app::PlayedNote> played = clip.playedNotes();
    if (played.empty() || area.height() < 3) return;
    int low = played.front().note.pitch, high = low;
    for (const app::PlayedNote& n : played) {
        low = std::min(low, n.note.pitch);
        high = std::max(high, n.note.pitch);
    }
    const double row = std::min(area.height() / (high - low + 1), std::max(2.0, area.height() / 12));
    const double top = area.top() + (area.height() - row * (high - low + 1)) / 2;
    const double gap = row > 3 ? 1.0 : 0.0;
    const timeline::Timeline& view = arrangement()->view();
    for (const app::PlayedNote& n : played) {
        const double x0 = view.beatToX(n.start), x1 = view.beatToX(n.end);
        if (x1 >= visible.left() && x0 <= visible.right()) {
            p.fillRect(QRectF(x0, top + (high - n.note.pitch) * row, std::max(1.0, x1 - x0 - gap), std::max(1.0, row - gap)),
                       Theme::kWaveform);
        }
    }
}

void ArrangementLanes::drawDropPreview(SgPainter& p) const {
    if (!dropPreview_) return;
    const Arrangement& a = *arrangement();
    double top = 0.0;
    int height = app::kDefaultTrackHeight;
    if (!dropPreview_->row || *dropPreview_->row >= static_cast<int>(a.layout().rows().size())) {
        top = a.layout().totalHeight() - a.scrollY();
    } else {
        const Row& row = a.layout().rows()[static_cast<size_t>(*dropPreview_->row)];
        top = row.top - a.scrollY();
        height = row.mainHeight;
    }
    double x = a.view().beatToX(dropPreview_->beat);
    const double tempo = session()->project()->tempo();
    for (const DropPreview::Source& source : dropPreview_->sources) {
        const double width = source.duration * tempo / 60.0 * a.pxPerBeat();
        const QRectF rect(x, top + 1, std::max(2.0, width), height - 3);
        p.fillRect(rect, kDropFill);
        dashedRect(p, rect, Theme::kAccent);
        p.drawText(rect.adjusted(4, 2, -2, 0), Qt::AlignTop | Qt::AlignLeft, source.name, Theme::kText, uiFont());
        x += width;
    }
}

// --- Mouse -----------------------------------------------------------------------------------------

void ArrangementLanes::mousePressEvent(QMouseEvent* event) {
    // The press a double-click starts with: the double-click stands for it, as
    // with widgets (which never see it).
    if (event->flags() & Qt::MouseEventCreatedDoubleClick) return;
    if (!ready()) return;
    const QPointF pos = event->position();
    if (event->button() == Qt::RightButton) {
        showMenu(pos);
        return;
    }
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    forceActiveFocus(Qt::MouseFocusReason);
    if (gesture_) {  // (one whose release never came: what it previewed goes)
        gesture_.reset();
        session()->bridge()->endClipPreview();
    }
    const Qt::KeyboardModifiers mods = event->modifiers();
    if (isPanModifier(mods)) {
        gesture_ = std::make_unique<PanGesture>(*this, pos);
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    app::Selection& selection = *session()->selection();
    // A clip edited by itself (a fade, an edge, its content): selected first, as a click on its body would.
    const auto selectClip = [&](const QString& trackId, const QString& clipId) {
        selection.selectClips(*session()->editor(), {{trackId, clipId}});
        if (selection.timeRange()) selection.setInsert(selection.timeRange()->start);
    };
    if (fadeKey_) {
        if (const auto fade = hitFade(pos)) {
            selectClip(fade->trackId, fade->clip.id);
            if (fade->curve)
                gesture_ = std::make_unique<FadeCurveGesture>(*this, fade->trackId, fade->clip, fade->out, pos);
            else
                gesture_ = std::make_unique<FadeGesture>(*this, fade->trackId, fade->clip, fade->out, pos);
            repaint();
            return;
        }
    }
    if (const auto area = envelopeAreaAt(pos)) {
        gesture_ = envelopes::press(*this, *area, pos, mods);
        repaint();
        return;
    }
    const auto hit = hitClip(pos);
    if (hit && (hit->zone == Zone::Left || hit->zone == Zone::Right)) {
        selectClip(hit->trackId, hit->clip.id);
        const bool left = hit->zone == Zone::Left;
        if (isStretchModifier(mods))
            gesture_ = std::make_unique<StretchGesture>(*this, hit->trackId, hit->clip, left);
        else
            gesture_ = std::make_unique<TrimGesture>(*this, hit->trackId, hit->clip, left);
        return;
    }
    if (hit && hit->zone == Zone::Body && isSlipModifier(mods)) {
        selectClip(hit->trackId, hit->clip.id);
        gesture_ = std::make_unique<SlipGesture>(*this, hit->trackId, hit->clip, pos);
        setCursor(Qt::SizeHorCursor);
        return;
    }
    if (inClipRange(pos) && !(mods & Qt::ShiftModifier)) {
        // Dragging the selected stretch moves it (Ctrl: copies); a click selects as usual.
        gesture_ = std::make_unique<MoveRangeGesture>(*this, pos, [this, pos, mods, hit] { click(pos, mods, hit); });
        return;
    }
    click(pos, mods, hit);
}

void ArrangementLanes::click(const QPointF& pos, Qt::KeyboardModifiers mods, const std::optional<Hit>& hit) {
    // A press that is not a trim or a drag of the time selection.
    app::Selection& selection = *session()->selection();
    app::ProjectEditor& editor = *session()->editor();
    if (hit && hit->zone != Zone::Body) {
        // Selecting a clip selects the area it covers on the grid; Shift-clicking
        // another selects the area that fully contains both (and the tracks between).
        // (A folded track's clips are all title: its bars.)
        const app::ClipRef ref{hit->trackId, hit->clip.id};
        if ((mods & Qt::ShiftModifier) && clipAnchor_) {
            selection.selectClips(editor, {*clipAnchor_, ref}, hit->trackId);
        } else {
            clipAnchor_ = ref;
            selection.selectClips(editor, {ref});
        }
        // Playback will start from the selection, as with Ableton's start marker.
        if (selection.timeRange()) selection.setInsert(selection.timeRange()->start);
        gesture_ = std::make_unique<MoveRangeGesture>(*this, pos);  // dragging moves it (Ctrl: copies)
        return;
    }
    const auto& rows = arrangement()->layout().rows();
    if (rows.empty()) return;
    // Shift-click: the selection extended to here (and on, while dragging).
    if (mods & Qt::ShiftModifier) {
        if (auto extend = ExtendGesture::start(*this, pos, mods)) {
            gesture_ = std::move(extend);
            return;
        }
    }
    // Clip body, empty lane (a folded track's too, between its bars) or below
    // the tracks: a click sets the insert marker, a drag selects time on the
    // grid (from below the tracks, starting at the last one).
    const auto index = rowIndexAt(pos.y());
    auto gesture = std::make_unique<TimeSelectGesture>(*this, pos, mods & Qt::AltModifier);
    selection.clear(index ? rows[static_cast<size_t>(*index)].trackId : QString());
    selection.setInsert(gesture->anchor());
    gesture_ = std::move(gesture);
}

void ArrangementLanes::mouseMoveEvent(QMouseEvent* event) {
    hoverPos_ = event->position();
    if (gesture_) {
        gesture_->move(event->position(), event->modifiers());
        repaint();
        return;
    }
    updateHover(event->position(), event->modifiers());
}

void ArrangementLanes::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    std::unique_ptr<Gesture> gesture = std::move(gesture_);
    if (gesture) gesture->finish();
    gesture_.reset();  // (a click in a clip range selects as elsewhere: no gesture follows it)
    if (ready()) updateHover(event->position(), event->modifiers());
    repaint();
}

void ArrangementLanes::mouseUngrabEvent() {
    // The mouse was taken away mid-gesture (a popup): it ends where it is.
    if (!gesture_) return;
    std::unique_ptr<Gesture> gesture = std::move(gesture_);
    gesture->cancel();
    repaint();
}

void ArrangementLanes::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !ready()) return;
    const QPointF pos = event->position();
    if (fadeKey_ && !isPanModifier(event->modifiers())) {
        // On a fade's dot: a straight line again; on its square: no fade.
        if (const auto fade = hitFade(pos)) {
            const double tempo = session()->project()->tempo();
            const app::Clip changed = fade->curve ? app::edits::curveFade(fade->clip, fade->out, 0.0)
                                                  : app::edits::fadeClip(fade->clip, fade->out, 0.0, tempo);
            if (changed != fade->clip) {
                const QString text = fade->curve ? QStringLiteral("Fade Curve")
                                     : fade->out ? QStringLiteral("Fade Out")
                                                 : QStringLiteral("Fade In");
                try {
                    session()->editor()->replaceClip(fade->trackId, changed, text);
                } catch (const app::EditError& error) {
                    Q_EMIT arrangement()->statusMessage(error.message());
                }
            }
            return;
        }
    }
    const auto area = envelopeAreaAt(pos);
    if (area && !isPanModifier(event->modifiers())) {
        gesture_ = envelopes::press(*this, *area, pos, event->modifiers());
        repaint();
        return;
    }
    const auto hit = hitClip(pos);
    if (!hit) return;
    // Double-clicking one of several selected clips opens them all.
    app::Selection& selection = *session()->selection();
    const app::ClipRef ref{hit->trackId, hit->clip.id};
    if (!selection.clips().contains(ref)) selection.selectClips(*session()->editor(), {ref});
    arrangement()->openClips(hit->trackId, hit->clip.id);
}

void ArrangementLanes::hoverEnterEvent(QHoverEvent* event) {
    hoverPos_ = event->position();
    if (!gesture_ && ready()) updateHover(event->position(), event->modifiers());
}

void ArrangementLanes::hoverMoveEvent(QHoverEvent* event) {
    hoverPos_ = event->position();
    if (!gesture_ && ready()) updateHover(event->position(), event->modifiers());
}

void ArrangementLanes::hoverLeaveEvent(QHoverEvent*) {
    hoverPos_.reset();
    if (gesture_) return;
    setHoverEdge(std::nullopt);
    if (hoverFade_) {
        hoverFade_.reset();
        repaint();
    }
    if (hoverPoint_) {
        hoverPoint_.reset();
        repaint();
    }
}

void ArrangementLanes::updateHover(const QPointF& pos, Qt::KeyboardModifiers mods) {
    if (!ready()) return;
    const bool pan = isPanModifier(mods);
    const std::optional<FadeHit> fade = pan || !fadeKey_ ? std::nullopt : hitFade(pos);
    std::optional<std::tuple<QString, bool, bool>> fadeHover;
    if (fade) fadeHover = std::make_tuple(fade->clip.id, fade->out, fade->curve);
    if (fadeHover != hoverFade_) {
        hoverFade_ = fadeHover;
        repaint();
    }
    const std::optional<EnvelopeArea> area = pan || fade ? std::nullopt : envelopeAreaAt(pos);
    auto [point, cursor] = envelopes::hover(*this, area, pos, mods);
    if (point != hoverPoint_) {
        hoverPoint_ = point;
        repaint();
    }
    if (fade) {
        setHoverEdge(std::nullopt);
        setCursor(fade->curve ? Qt::SizeVerCursor : Qt::SizeHorCursor);
        return;
    }
    if (area) {
        setHoverEdge(std::nullopt);
        setCursor(cursor);
        return;
    }
    const auto& rows = arrangement()->layout().rows();
    Qt::CursorShape shape = Qt::ArrowCursor;
    if (pan) {
        shape = Qt::OpenHandCursor;
    } else if (!rows.empty()) {
        const auto hit = hitClip(pos);
        const Zone zone = hit ? hit->zone : Zone::Body;
        if (zone == Zone::Left || zone == Zone::Right) {
            const bool left = zone == Zone::Left, stretch = isStretchModifier(mods);
            setHoverEdge(std::make_pair(hit->clip.id, left), stretch);
            setCursor(stretch ? stretchCursor(left) : trimCursor(left));
            return;
        }
        const bool grab = zone == Zone::Title || inClipRange(pos);
        shape = grab ? Qt::PointingHandCursor : Qt::IBeamCursor;
        if (hit && zone == Zone::Body && isSlipModifier(mods)) shape = Qt::SizeHorCursor;
    }
    setHoverEdge(std::nullopt);
    setCursor(shape);
}

void ArrangementLanes::setHoverEdge(const std::optional<std::pair<QString, bool>>& edge, bool stretch) {
    stretch = stretch && edge.has_value();
    if (edge == hoverEdge_ && stretch == hoverStretch_) return;
    hoverEdge_ = edge;
    hoverStretch_ = stretch;
    repaint();
}

void ArrangementLanes::wheelEvent(QWheelEvent* event) {
    if (!ready()) return;
    Arrangement& a = *arrangement();
    const QPoint delta = event->angleDelta();
    const Qt::KeyboardModifiers mods = event->modifiers();
    if ((mods & Qt::AltModifier) && !(mods & Qt::ControlModifier)) {
        // Alt+wheel resizes the track under the mouse, folding (or unfolding) it at its smallest.
        // (Qt may report Alt+wheel as horizontal scrolling, so either axis counts.)
        if (const auto index = rowIndexAt(event->position().y()))
            a.wheelResize(a.layout().rows()[static_cast<size_t>(*index)].trackId, delta.y() ? delta.y() : delta.x());
    } else if (mods & Qt::ControlModifier) {
        a.zoomAt(event->position().x(), std::pow(1.2, delta.y() / 120.0));
    } else if ((mods & Qt::ShiftModifier) || delta.x()) {
        const double pixels = -(delta.x() ? delta.x() : delta.y()) / 120.0 * 80.0;
        a.scrollByHand(a.scrollBeats() + pixels / a.pxPerBeat());
    } else {
        a.setScrollY(a.scrollY() - delta.y() / 120.0 * 48);
    }
    event->accept();
}

void ArrangementLanes::keyPressEvent(QKeyEvent* event) {
    onModifiers(heldModifiers(event));
    event->ignore();
}

void ArrangementLanes::keyReleaseEvent(QKeyEvent* event) {
    onModifiers(heldModifiers(event));
    event->ignore();
}

void ArrangementLanes::onModifiers(Qt::KeyboardModifiers mods) {
    // Show the hand cursor as soon as Ctrl+Alt is held (Alt on an edge: the
    // stretch cursor), without moving the mouse.
    if (!gesture_ && hoverPos_) updateHover(*hoverPos_, mods);
}

bool ArrangementLanes::eventFilter(QObject* watched, QEvent* event) {
    // (A key event reaches the filter for each object it is delivered to: taken
    // where the window gets it, before the item with the focus.)
    if (watched == nullptr || watched != window()) return false;
    switch (event->type()) {
        case QEvent::KeyPress:
        case QEvent::KeyRelease: {
            const auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_F && !key->isAutoRepeat()) {
                bool held = false;
                if (event->type() == QEvent::KeyPress) {
                    // Not while typing, nor with shortcut modifiers, nor while the computer keyboard plays F.
                    const Qt::KeyboardModifiers mods = key->modifiers();
                    const app::ComputerKeyboard* keyboard = ready() ? session()->computerKeyboard() : nullptr;
                    const bool plays = keyboard && keyboard->takesKey(Qt::Key_F) && !(mods & Qt::ShiftModifier);
                    held = !(mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) && !plays &&
                           !app::ComputerKeyboard::focusTakesText();
                }
                setFadeKey(held);
            }
            onModifiers(heldModifiers(key));
            break;
        }
        case QEvent::FocusOut:
        case QEvent::WindowDeactivate:
            setFadeKey(false);  // (its release goes elsewhere)
            break;
        default:
            break;
    }
    return false;
}

void ArrangementLanes::setFadeKey(bool held) {
    if (held == fadeKey_) return;
    fadeKey_ = held;
    if (!gesture_ && hoverPos_) updateHover(*hoverPos_, QGuiApplication::keyboardModifiers());
    if (!held && hoverFade_) hoverFade_.reset();
    repaint();
}

bool ArrangementLanes::showsFadeHandles() const {
    const Gesture* gesture = gesture_.get();
    return fadeKey_ || dynamic_cast<const FadeGesture*>(gesture) || dynamic_cast<const FadeCurveGesture*>(gesture);
}

// --- Menus -------------------------------------------------------------------------------------------

void ArrangementLanes::showMenu(const QPointF& pos) {
    menu_ = contextMenu(pos);
    if (!menu_.isEmpty()) Q_EMIT menuRequested(menu_.toVariant(), pos);
}

void ArrangementLanes::triggerMenu(int id) {
    const MenuEntries menu = menu_;  // (what it does may show another)
    menu.trigger(id);
}

MenuEntries ArrangementLanes::contextMenu(const QPointF& pos) {
    MenuEntries menu;
    if (!ready()) return menu;
    app::Session* s = session();
    app::ArrangementActions* actions = s->arrangement();
    app::Selection& selection = *s->selection();
    if (const auto area = envelopeAreaAt(pos)) {
        const bool inRange = envelopes::inRange(*this, *area, pos);
        MenuEntry& cut = menu.add(QStringLiteral("Cut"), [actions] { actions->cutAutomation(); });
        cut.shortcut = QStringLiteral("Ctrl+X");
        cut.enabled = inRange;
        MenuEntry& copy = menu.add(QStringLiteral("Copy"), [actions] { actions->copyAutomation(); });
        copy.shortcut = QStringLiteral("Ctrl+C");
        copy.enabled = inRange;
        // At the insert marker: onto the selected lanes if this is one of them, else onto this one.
        const QString owner = area->owner, key = area->key;
        MenuEntry& paste = menu.add(QStringLiteral("Paste"), [actions, owner, key] { actions->pasteAutomationAt(owner, key); });
        paste.shortcut = QStringLiteral("Ctrl+V");
        paste.enabled = actions->clipboardKind() == QStringLiteral("automation");
        menu.addSeparator();
        envelopes::addMenuEntries(*this, *area, pos, menu);
        return menu;
    }
    const auto hit = hitClip(pos);
    if (hit || inSelection(pos)) {
        // On a clip (selected first, unless it is), or anywhere in the selected area: what acts on it.
        if (hit && !selection.clips().contains(app::ClipRef{hit->trackId, hit->clip.id}))
            selection.selectClips(*s->editor(), {{hit->trackId, hit->clip.id}});
        menu.add(QStringLiteral("Cut"), [actions] { actions->cutArea(); }).shortcut = QStringLiteral("Ctrl+X");
        menu.add(QStringLiteral("Copy"), [actions] { actions->copyArea(); }).shortcut = QStringLiteral("Ctrl+C");
        MenuEntry& paste = menu.add(QStringLiteral("Paste"), [actions] { actions->paste(); });
        paste.shortcut = QStringLiteral("Ctrl+V");
        paste.enabled = actions->hasClipboard();
        menu.addSeparator();
        if (hit) {
            const double at = arrangement()->view().snapBeat(arrangement()->view().xToBeat(pos.x()));
            menu.add(QStringLiteral("Split Here"), [actions, at] { actions->splitAt(at); });
        }
        menu.add(QStringLiteral("Duplicate"), [actions] { actions->duplicateArea(); }).shortcut = QStringLiteral("Ctrl+D");
        MenuEntry& consolidate = menu.add(QStringLiteral("Consolidate"), [actions] { actions->consolidate(); });
        consolidate.shortcut = QStringLiteral("Ctrl+J");
        consolidate.enabled = actions->canConsolidate();
        // (What R reverses: the audio clips in the selected area, whichever are selected as clips.)
        MenuEntry& reverse = menu.add(QStringLiteral("Reverse"), [actions] { actions->reverseSelection(); });
        reverse.shortcut = QStringLiteral("R");
        reverse.enabled = actions->canReverse();
        if (hit && hit->clip.isAudio()) {
            // The browser lists the sounds most like the part of its file the clip plays.
            app::BrowserController* browser = s->browser();
            const app::Clip& clip = hit->clip;
            const bool whole = clip.offsetSec <= 1e-6 &&
                               (clip.sourceDurationSec <= 0.0 || clip.durationSec >= clip.sourceDurationSec - 1e-3);
            const QString path = clip.path;
            const double start = whole ? 0.0 : clip.offsetSec;
            const double length = whole ? -1.0 : clip.durationSec;
            menu.addSeparator();
            MenuEntry& similar = menu.add(QStringLiteral("Find Similar Sounds"),
                                          [browser, path, start, length] { browser->findSimilar(path, start, length); });
            similar.enabled = browser->canFindSimilar() && !path.isEmpty();
        }
        menu.addSeparator();
        menu.add(QStringLiteral("Delete"), [actions] { actions->deleteArea(); }).shortcut = QStringLiteral("Del");
        return menu;
    }
    const auto index = rowIndexAt(pos.y());
    const QString trackId = index ? arrangement()->layout().rows()[static_cast<size_t>(*index)].trackId : QString();
    if (!trackId.isEmpty()) {
        const double beat = std::max(0.0, arrangement()->view().snapBeat(arrangement()->view().xToBeat(pos.x())));
        MenuEntry& paste = menu.add(QStringLiteral("Paste"), [actions, beat, trackId] { actions->paste(beat, trackId); });
        paste.shortcut = QStringLiteral("Ctrl+V");
        paste.enabled = actions->hasClipboard();
        menu.addSeparator();
        const app::Track* track = s->project()->findTrack(trackId);
        if (track && track->isMidi()) {
            const double x = pos.x();
            menu.add(QStringLiteral("Insert MIDI Clip"), [this, trackId, x] { insertMidiClip(trackId, x); });
            menu.addSeparator();
        }
    }
    menu.add(QStringLiteral("Insert Audio Track"), [actions, trackId] { actions->insertTrackAfter(trackId, false); });
    menu.add(QStringLiteral("Insert MIDI Track"), [actions, trackId] { actions->insertTrackAfter(trackId, true); });
    if (!trackId.isEmpty()) {
        app::ProjectEditor* editor = s->editor();
        menu.add(QStringLiteral("Delete Track"), [editor, trackId] { editor->deleteTracks({trackId}); });
    }
    return menu;
}

std::optional<app::ClipRef> ArrangementLanes::insertMidiClip(const QString& trackId, double x) {
    if (!ready()) return std::nullopt;
    const timeline::Timeline& view = arrangement()->view();
    const QVariantMap ref = session()->arrangement()->insertMidiClip(trackId, view.xToBeat(x),
                                                                     view.snap() ? view.gridStep() : 0.0);
    if (ref.isEmpty()) return std::nullopt;
    return app::ClipRef{ref.value(QStringLiteral("trackId")).toString(), ref.value(QStringLiteral("clipId")).toString()};
}

// --- Drops --------------------------------------------------------------------------------------------

bool ArrangementLanes::dragOver(const QMimeData* mime, const QPointF& pos) {
    if (!ready()) return false;
    const auto& rows = arrangement()->layout().rows();
    if (const auto moved = app::movedDevices(mime)) {
        // Devices from a track's chain move to another track's, the one under the mouse.
        const auto index = rowIndexAt(pos.y());
        return index && rows[static_cast<size_t>(*index)].trackId != moved->trackId;
    }
    const QStringList paths = audioPaths(mime);
    if (paths.isEmpty()) {
        // Devices (and presets) drop onto the track under the mouse; an
        // instrument below the tracks makes a new MIDI track.
        const DroppedDevices devices = droppedDevices(mime);
        const QStringList presets = app::presetPaths(mime);
        const bool onTrack = (!devices.empty() || !presets.isEmpty()) && rowIndexAt(pos.y()).has_value();
        return onTrack || !presets.isEmpty() || devices.anyInstrument;
    }
    std::optional<int> index = rowIndexAt(pos.y());
    if (index) {
        const app::Track* track = session()->project()->findTrack(rows[static_cast<size_t>(*index)].trackId);
        if (!track || !track->isAudio()) index.reset();  // audio goes only on an audio track: otherwise it gets a new track
    }
    const timeline::Timeline& view = arrangement()->view();
    DropPreview preview;
    preview.row = index;
    preview.beat = std::max(0.0, view.snapBeat(view.xToBeat(pos.x())));
    if (paths != dropPaths_ || !dropPreview_) {
        dropPaths_ = paths;
        for (const QVariant& item : session()->arrangement()->dropSources(paths)) {
            const QVariantMap source = item.toMap();
            preview.sources.push_back({source.value(QStringLiteral("path")).toString(),
                                       source.value(QStringLiteral("name")).toString(),
                                       source.value(QStringLiteral("duration")).toDouble()});
        }
    } else {
        preview.sources = dropPreview_->sources;
    }
    dropPreview_ = preview;
    repaint();
    return true;
}

void ArrangementLanes::dragLeft() {
    dropPreview_.reset();
    dropPaths_.clear();
    repaint();
}

bool ArrangementLanes::drop(const QMimeData* mime, const QPointF& pos) {
    if (!ready()) return false;
    const std::optional<DropPreview> preview = dropPreview_;
    const QStringList previewPaths = dropPaths_;
    dragLeft();
    app::ArrangementActions& actions = *session()->arrangement();
    const auto& rows = arrangement()->layout().rows();
    const auto index = rowIndexAt(pos.y());
    const QString trackId = index ? rows[static_cast<size_t>(*index)].trackId : QString();
    if (const auto moved = app::movedDevices(mime)) {
        return !trackId.isEmpty() && actions.dropMovedDevices(moved->trackId, moved->deviceIds, trackId);
    }
    const DroppedDevices devices = droppedDevices(mime);
    if (!devices.empty()) return actions.dropDevices(devices.kinds, devices.plugins, trackId);
    const QStringList presets = app::presetPaths(mime);
    if (!presets.isEmpty()) return actions.dropPresets(presets, trackId);
    const QStringList paths = audioPaths(mime);
    if (paths.isEmpty()) return false;
    // Where the preview showed them (worked out again if no move came first).
    std::optional<DropPreview> at = preview;
    if (!at || previewPaths != paths) {
        dragOver(mime, pos);
        at = dropPreview_;
        dragLeft();
    }
    if (!at || at->sources.empty()) return false;
    const QString onto = at->row && *at->row < static_cast<int>(rows.size()) ? rows[static_cast<size_t>(*at->row)].trackId
                                                                              : QString();
    QStringList sources;
    for (const DropPreview::Source& source : at->sources) sources << source.path;
    actions.dropFiles(sources, onto, at->beat);
    forceActiveFocus(Qt::OtherFocusReason);
    return true;
}

void ArrangementLanes::dragEnterEvent(QDragEnterEvent* event) {
    const QMimeData* mime = event->mimeData();
    const bool wanted = !audioPaths(mime).isEmpty() || !droppedDevices(mime).empty() ||
                        !app::presetPaths(mime).isEmpty() || app::movedDevices(mime).has_value() ||
                        (mime && mime->hasFormat(QString::fromLatin1(app::kPluginMime)));
    if (!wanted) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    if (!dragOver(mime, event->position())) event->setAccepted(true);  // (it may move to where it can drop)
}

void ArrangementLanes::dragMoveEvent(QDragMoveEvent* event) {
    if (dragOver(event->mimeData(), event->position()))
        event->acceptProposedAction();
    else
        event->ignore();
}

void ArrangementLanes::dragLeaveEvent(QDragLeaveEvent*) { dragLeft(); }

void ArrangementLanes::dropEvent(QDropEvent* event) {
    if (drop(event->mimeData(), event->position()))
        event->acceptProposedAction();
    else
        event->ignore();
}

}  // namespace sub::ui
