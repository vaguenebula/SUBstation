#include "arrangement/Envelopes.h"

#include "arrangement/Arrangement.h"
#include "arrangement/ClipGestures.h"
#include "arrangement/Cursors.h"
#include "audio/EngineBridge.h"
#include "editor/ProjectEditor.h"
#include "model/Project.h"
#include "session/Selection.h"
#include "session/Session.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"
#include "timeline/Timeline.h"

#include <QFontMetricsF>
#include <QLineF>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>

namespace sub::ui::arrangement {

double EnvelopeArea::y(double value) const {
    const QRectF v = values();
    return v.bottom() - value * v.height();
}

double EnvelopeArea::value(double y) const {
    const QRectF v = values();
    return std::min(1.0, std::max(0.0, (v.bottom() - y) / std::max(1.0, v.height())));
}

namespace envelopes {

namespace {

using app::AutomationPoint;
using app::Envelope;

double fine(Qt::KeyboardModifiers modifiers) { return modifiers & Qt::ShiftModifier ? 0.1 : 1.0; }
bool alt(Qt::KeyboardModifiers modifiers) { return modifiers & Qt::AltModifier; }
bool farEnough(const QPointF& pos, const QPointF& press) { return (pos - press).manhattanLength() >= kDragThreshold; }

const timeline::Timeline& viewOf(LanesHost& host) { return host.hostArrangement()->view(); }
app::Project& projectOf(LanesHost& host) { return *host.hostSession()->project(); }

// bisect_left / bisect_right on the points' beats.
int bisectLeft(const Envelope& points, double beat) {
    return static_cast<int>(std::lower_bound(points.begin(), points.end(), beat,
                                             [](const AutomationPoint& p, double b) { return p.beat < b; }) -
                            points.begin());
}
int bisectRight(const Envelope& points, double beat) {
    return static_cast<int>(std::upper_bound(points.begin(), points.end(), beat,
                                             [](double b, const AutomationPoint& p) { return b < p.beat; }) -
                            points.begin());
}

// From `pos` to the segment a-b.
double distance(const QPointF& pos, const QPointF& a, const QPointF& b) {
    const QPointF d = b - a;
    const double length = QPointF::dotProduct(d, d);
    const double t = length == 0 ? 0.0 : std::min(1.0, std::max(0.0, QPointF::dotProduct(pos - a, d) / length));
    return QLineF(a + d * t, pos).length();
}

double polylineDistance(const QPointF& pos, const QPolygonF& line) {
    if (line.size() == 1) return QLineF(line[0], pos).length();
    double best = std::numeric_limits<double>::infinity();
    for (qsizetype i = 0; i + 1 < line.size(); ++i) best = std::min(best, distance(pos, line[i], line[i + 1]));
    return best;
}

std::optional<app::ParamSpec> specOf(LanesHost& host, const EnvelopeArea& area) {
    return host.hostSession()->bridge()->paramSpec(area.owner, area.key);
}

// Where a target without an envelope draws its line: its own value.
std::optional<double> unautomatedValue(LanesHost& host, const EnvelopeArea& area) {
    app::EngineBridge& bridge = *host.hostSession()->bridge();
    const auto value = bridge.ownValue(area.owner, area.key);
    const auto spec = bridge.paramSpec(area.owner, area.key);
    if (!value || !spec) return std::nullopt;
    return spec->toNormalized(*value);
}

// Whether `pos` is within `grab` pixels of the envelope's line, as drawn.
bool onLine(LanesHost& host, const EnvelopeArea& area, const Envelope& points, const QPointF& pos,
            double grab = kLineGrab) {
    if (points.empty()) {
        const auto value = unautomatedValue(host, area);
        return value && std::abs(area.y(*value) - pos.y()) <= grab;
    }
    const auto spec = specOf(host, area);
    const QPolygonF line = trace(viewOf(host), area, points, pos.x() - grab, pos.x() + grab,
                                 spec && spec->discrete() ? &*spec : nullptr);
    if (line.size() == 1) return QLineF(line[0], pos).length() <= grab;
    for (qsizetype i = 0; i + 1 < line.size(); ++i) {
        if (distance(pos, line[i], line[i + 1]) <= grab) return true;
    }
    return false;
}

// The segment from breakpoint i to the next as drawn (between x0 and x1).
QPolygonF segmentLine(const timeline::Timeline& view, const EnvelopeArea& area, const Envelope& points, int i,
                      const app::ParamSpec* quantize, double x0 = -std::numeric_limits<double>::infinity(),
                      double x1 = std::numeric_limits<double>::infinity()) {
    const AutomationPoint& a = points[static_cast<size_t>(i)];
    const AutomationPoint& b = points[static_cast<size_t>(i) + 1];
    const double xa = view.beatToX(a.beat), xb = view.beatToX(b.beat);
    if (xb - xa < 1.0)  // a step: straight up or down
        return QPolygonF({QPointF(xa, area.y(a.value)), QPointF(xb, area.y(b.value))});
    const Envelope pair{a, b};
    return trace(view, area, pair, std::max(xa, x0), std::min(xb, x1), quantize);
}

// The segment (its first breakpoint) whose line `pos` is nearest to, within
// kSegmentGrab pixels. Steps count too.
std::optional<int> segmentAt(LanesHost& host, const EnvelopeArea& area, const Envelope& points, const QPointF& pos) {
    if (points.size() < 2) return std::nullopt;
    const timeline::Timeline& view = viewOf(host);
    const auto spec = specOf(host, area);
    const app::ParamSpec* quantize = spec && spec->discrete() ? &*spec : nullptr;
    const int first = std::max(0, bisectLeft(points, view.xToBeat(pos.x() - kSegmentGrab)) - 1);
    const int last = std::min(static_cast<int>(points.size()) - 1, bisectRight(points, view.xToBeat(pos.x() + kSegmentGrab)));
    std::optional<int> best;
    double bestDistance = kSegmentGrab;
    for (int i = first; i < last; ++i) {
        const QPolygonF line = segmentLine(view, area, points, i, quantize, pos.x() - kSegmentGrab, pos.x() + kSegmentGrab);
        if (line.isEmpty()) continue;
        const double d = polylineDistance(pos, line);
        if (d <= bestDistance) {
            best = i;
            bestDistance = d;
        }
    }
    return best;
}

// The first or last breakpoint when `pos` is within kSegmentGrab pixels of the
// flat line running out to the left or right of it.
std::optional<int> endAt(LanesHost& host, const EnvelopeArea& area, const Envelope& points, const QPointF& pos) {
    if (points.empty()) return std::nullopt;
    const timeline::Timeline& view = viewOf(host);
    int index = 0;
    if (pos.x() < view.beatToX(points.front().beat))
        index = 0;
    else if (pos.x() > view.beatToX(points.back().beat))
        index = static_cast<int>(points.size()) - 1;
    else
        return std::nullopt;
    const auto spec = specOf(host, area);
    double value = points[static_cast<size_t>(index)].value;
    if (spec && spec->discrete()) value = spec->quantize(value);
    if (std::abs(area.y(value) - pos.y()) <= kSegmentGrab) return index;
    return std::nullopt;
}

bool isStep(const Envelope& points, int segment) {
    return points[static_cast<size_t>(segment)].beat == points[static_cast<size_t>(segment) + 1].beat;
}

// What a press off the breakpoints takes: Add (beat, value) on the line, or
// Segment (index) near it. On a step there is nothing to add: it takes the step.
struct Grab {
    bool add = false;
    std::pair<double, double> target;
    int segment = 0;
};
std::optional<Grab> grabAt(LanesHost& host, const EnvelopeArea& area, const Envelope& points, const QPointF& pos,
                           Qt::KeyboardModifiers modifiers) {
    const auto segment = segmentAt(host, area, points, pos);
    if (segment && isStep(points, *segment)) return Grab{false, {}, *segment};
    if (const auto target = addTarget(host, area, pos, modifiers)) return Grab{true, *target, 0};
    if (segment) return Grab{false, {}, *segment};
    return std::nullopt;
}

QString newKey() { return QUuid::createUuid().toString(); }

// --- Gestures -------------------------------------------------------------------------------------

// Drag breakpoints: the one pressed, and the others selected with it. A click
// without dragging deletes it (Shift/Ctrl: selects it instead). With `added`,
// the breakpoint was just added by this press: a click keeps it, and adding
// and dragging it are one undo step. With `segment`, it and the next one are
// dragged (a segment); a click selects them.
class PointGesture : public Gesture {
public:
    PointGesture(LanesHost& host, const EnvelopeArea& area, int index, const QPointF& press,
                 Qt::KeyboardModifiers modifiers, bool added = false, bool segment = false, QString mergeKey = {})
        : host_(host), area_(area), index_(index), press_(press), added_(added || segment),
          mergeKey_(mergeKey.isEmpty() ? newKey() : std::move(mergeKey)) {
        app::Selection& selection = *host.hostSession()->selection();
        original_ = projectOf(host).envelope(area.owner, area.key);
        QSet<int> selected = added_ ? QSet<int>() : selection.selectedPoints(area.owner, area.key);
        selecting_ = !added_ && (modifiers & (Qt::ShiftModifier | Qt::ControlModifier));
        if (segment) {
            selected = {index, index + 1};
        } else if (selecting_) {
            if (selected.contains(index))
                selected.remove(index);
            else
                selected.insert(index);
        } else if (!selected.contains(index)) {
            selected = {index};
        }
        selection.selectPoints(area.owner, area.key, selected);
        indices_ = selected.contains(index) ? selected : QSet<int>{index};
        current_ = index;
    }

    void move(const QPointF& pos, Qt::KeyboardModifiers modifiers) override {
        if (!active_) {
            if (!farEnough(pos, press_)) return;
            active_ = true;
        }
        if (index_ < 0 || index_ >= static_cast<int>(original_.size())) return;
        app::Session& session = *host_.hostSession();
        const timeline::Timeline& view = viewOf(host_);
        const AutomationPoint& anchor = original_[static_cast<size_t>(index_)];
        double deltaBeats = 0.0;
        if (std::abs(pos.x() - press_.x()) >= kDragThreshold) {  // a vertical drag leaves the time alone
            const double beat = anchor.beat + view.xToBeat(pos.x()) - view.xToBeat(press_.x());
            deltaBeats = std::max(0.0, view.snapBeat(beat, alt(modifiers))) - anchor.beat;
        }
        const double deltaValue = (press_.y() - pos.y()) / std::max(1.0, area_.values().height()) * fine(modifiers);
        QList<int> indices(indices_.begin(), indices_.end());
        std::sort(indices.begin(), indices.end());
        const QMap<int, int> where = session.editor()->moveAutomationPoints(area_.owner, area_.key, original_, indices,
                                                                            deltaBeats, deltaValue, mergeKey_);
        // Several points override what they land on, so the points after them move up the list.
        current_ = where.value(index_, current_);
        QSet<int> moved;
        for (int i : where) moved.insert(i);
        session.selection()->selectPoints(area_.owner, area_.key, moved);
        updateReadout();
        host_.repaint();
    }

    std::optional<Readout> readout() const override { return readout_; }

    void finish() override {
        if (active_ || selecting_ || added_) return;
        app::Session& session = *host_.hostSession();
        session.editor()->deleteAutomationPoints(area_.owner, area_.key, {current_});
        session.selection()->selectPoints(area_.owner, area_.key, QSet<int>());
    }

private:
    void updateReadout() {
        readout_.reset();
        const Envelope points = projectOf(host_).envelope(area_.owner, area_.key);
        const auto spec = specOf(host_, area_);
        if (!active_ || !spec || current_ >= static_cast<int>(points.size()) || current_ < 0) return;
        const AutomationPoint& point = points[static_cast<size_t>(current_)];
        readout_ = Readout{QPointF(viewOf(host_).beatToX(point.beat), area_.y(point.value)),
                           spec->formatNormalized(spec->quantize(point.value))};
    }

    LanesHost& host_;
    EnvelopeArea area_;
    int index_;
    QPointF press_;
    bool added_;
    QString mergeKey_;
    Envelope original_;
    bool selecting_ = false;
    QSet<int> indices_;
    int current_ = 0;  // where the pressed point is now
    bool active_ = false;
    std::optional<Readout> readout_;
};

// Alt-drag between two breakpoints: bend the segment.
class CurveGesture : public Gesture {
public:
    CurveGesture(LanesHost& host, const EnvelopeArea& area, int index, const QPointF& press)
        : host_(host), area_(area), index_(index), press_(press), key_(newKey()) {
        original_ = projectOf(host).envelope(area.owner, area.key);
        host.hostSession()->selection()->selectPoints(area.owner, area.key, QSet<int>());
    }

    void move(const QPointF& pos, Qt::KeyboardModifiers modifiers) override {
        if (index_ < 0 || index_ >= static_cast<int>(original_.size())) return;
        const double bend = (press_.y() - pos.y()) / kCurvePixels * fine(modifiers);
        host_.hostSession()->editor()->setAutomationCurve(area_.owner, area_.key, original_, index_,
                                                          original_[static_cast<size_t>(index_)].curve + bend, key_);
        host_.repaint();
    }

private:
    LanesHost& host_;
    EnvelopeArea area_;
    int index_;
    QPointF press_;
    QString key_;
    Envelope original_;
};

// A press on a lane off its breakpoints and its line: a click sets the
// insert marker; a drag selects a time range on the lanes it crosses (on the
// clips when it goes up into their title band or above the track).
class LaneGesture : public Gesture {
public:
    LaneGesture(LanesHost& host, const EnvelopeArea& area, const QPointF& press, Qt::KeyboardModifiers modifiers)
        : host_(host), area_(area), press_(press) {
        const timeline::Timeline& view = viewOf(host);
        const bool bypass = alt(modifiers);
        anchor_ = std::max(0.0, view.snapBeat(view.xToBeat(press.x()), bypass));
        // A track's lanes are in the lanes canvas, among the clips: a drag can reach them.
        if (area.owner != app::kMaster && host.isTrackLanes()) {
            if (const auto row = host.hostArrangement()->layout().indexOf(area.owner)) {
                clips_ = std::make_unique<TimeSelectGesture>(host, press, bypass);
                clips_->setAnchorRow(*row);
            }
        }
        host.hostSession()->selection()->clear(area.owner == app::kMaster ? QString() : area.owner);
    }

    void move(const QPointF& pos, Qt::KeyboardModifiers modifiers) override {
        if (!active_) {
            if (!farEnough(pos, press_)) return;
            active_ = true;
        }
        if (overClips(pos)) {
            clips_->move(pos, modifiers);
            return;
        }
        app::Session& session = *host_.hostSession();
        const timeline::Timeline& view = viewOf(host_);
        const double beat = std::max(0.0, view.snapBeat(view.xToBeat(pos.x()), alt(modifiers)));
        const double start = std::min(anchor_, beat), end = std::max(anchor_, beat);
        const std::vector<EnvelopeArea> areas = host_.envelopeAreas();
        std::optional<int> first;
        for (size_t i = 0; i < areas.size(); ++i) {
            if (areas[i].sameLane(area_)) {
                first = static_cast<int>(i);
                break;
            }
        }
        const auto here = nearestArea(areas, pos.y());
        std::vector<EnvelopeArea> covered;
        if (!first || !here)
            covered = {area_};
        else
            covered.assign(areas.begin() + std::min(*first, *here), areas.begin() + std::max(*first, *here) + 1);
        QList<app::LaneRef> lanes;
        QStringList trackIds;
        for (const EnvelopeArea& a : covered) {
            const app::LaneRef lane{a.owner, a.key};
            if (!lanes.contains(lane)) lanes.append(lane);
            if (session.project()->hasTrack(a.owner) && !trackIds.contains(a.owner)) trackIds << a.owner;
        }
        session.selection()->setTimeRange(start, end, trackIds, std::nullopt, lanes);
        session.selection()->setInsert(start);
        host_.setHostCursor(QCursor(Qt::IBeamCursor));
    }

    void finish() override {
        if (!active_) host_.hostSession()->selection()->setInsert(anchor_);
    }

private:
    // Whether the drag went up into the clips' title band or above its track.
    bool overClips(const QPointF& pos) const {
        if (!clips_) return false;
        const Arrangement& arrangement = *host_.hostArrangement();
        const auto& rows = arrangement.layout().rows();
        if (clips_->anchorRow() < 0 || clips_->anchorRow() >= static_cast<int>(rows.size())) return false;
        const Row& row = rows[static_cast<size_t>(clips_->anchorRow())];
        return host_.inClipBand(pos) || pos.y() + arrangement.scrollY() < row.top;
    }

    LanesHost& host_;
    EnvelopeArea area_;
    QPointF press_;
    double anchor_ = 0.0;
    bool active_ = false;
    std::unique_ptr<TimeSelectGesture> clips_;
};

// A press inside the selected time range on one of its lanes: a drag moves
// the automation in the range, on all its lanes, in time and value (the range
// goes along). (Its breakpoints and segments, and the line, are taken as
// outside it.) A click sets the insert marker, as outside the range.
class RangeGesture : public Gesture {
public:
    RangeGesture(LanesHost& host, const EnvelopeArea& area, const QPointF& press, Qt::KeyboardModifiers modifiers)
        : host_(host), area_(area), press_(press), modifiers_(modifiers), key_(newKey()) {
        const app::Selection& selection = *host.hostSession()->selection();
        if (const auto& range = selection.timeRange()) {
            start_ = range->start;
            end_ = range->end;
            trackIds_ = range->trackIds;
        }
        lanes_ = selection.lanes();
        for (const app::LaneRef& lane : lanes_) originals_.insert(lane, projectOf(host).envelope(lane.first, lane.second));
    }

    void move(const QPointF& pos, Qt::KeyboardModifiers modifiers) override {
        if (!active_) {
            if (!farEnough(pos, press_)) return;
            active_ = true;
        }
        app::Session& session = *host_.hostSession();
        const timeline::Timeline& view = viewOf(host_);
        double deltaBeats = 0.0;
        if (std::abs(pos.x() - press_.x()) >= kDragThreshold) {  // a vertical drag leaves the time alone
            const double beat = start_ + view.xToBeat(pos.x()) - view.xToBeat(press_.x());
            deltaBeats = std::max(0.0, view.snapBeat(beat, alt(modifiers))) - start_;
        }
        const double deltaValue = (press_.y() - pos.y()) / std::max(1.0, area_.values().height()) * fine(modifiers);
        session.editor()->moveAutomationRange(start_, end_, originals_, deltaBeats, deltaValue, key_);
        session.selection()->setTimeRange(start_ + deltaBeats, end_ + deltaBeats, trackIds_, std::nullopt, lanes_);
        session.selection()->setInsert(start_ + deltaBeats);
        host_.repaint();
    }

    void finish() override {
        if (!active_) LaneGesture(host_, area_, press_, modifiers_).finish();
    }

private:
    LanesHost& host_;
    EnvelopeArea area_;
    QPointF press_;
    Qt::KeyboardModifiers modifiers_;
    QString key_;
    double start_ = 0.0;
    double end_ = 0.0;
    QStringList trackIds_;
    QList<app::LaneRef> lanes_;
    QMap<app::LaneRef, Envelope> originals_;
    bool active_ = false;
};

// A press on the line: a breakpoint there at once, and a drag places it.
std::unique_ptr<Gesture> addAndDrag(LanesHost& host, const EnvelopeArea& area, const QPointF& pos,
                                    Qt::KeyboardModifiers modifiers, const std::pair<double, double>& target) {
    app::Session& session = *host.hostSession();
    const QString key = newKey();  // ties the breakpoint's adding to the drag that follows
    const int index = session.editor()->addAutomationPoint(area.owner, area.key, target.first, target.second, key);
    auto gesture = std::make_unique<PointGesture>(host, area, index, pos, modifiers, true, false, key);
    session.selection()->setInsert(target.first);
    return gesture;
}

void dashedLine(SgPainter& p, double x0, double x1, double y, const QColor& color, double width) {
    // Qt's DashLine: dashes of 4 pen widths, gaps of 2.
    const double dash = 4 * width, gap = 2 * width;
    for (double x = x0; x < x1; x += dash + gap) p.drawLine(QPointF(x, y), QPointF(std::min(x + dash, x1), y), color, width, Qt::FlatCap);
}

void drawGhost(SgPainter& p, const timeline::Timeline& view, const EnvelopeArea& area,
               const std::optional<Hover>& hover, bool gestureActive) {
    // Where a click would add a breakpoint (not while a gesture is under way).
    if (!hover || !hover->on(area) || hover->kind != Hover::Kind::Add || gestureActive) return;
    const double radius = kPointRadius + 1.0;
    const QPointF at(view.beatToX(hover->beat), area.y(hover->value));
    p.fillEllipse(at, radius, radius, QColor(kGhost.red(), kGhost.green(), kGhost.blue(), 70));
    p.drawEllipse(QRectF(at.x() - radius, at.y() - radius, 2 * radius, 2 * radius), kGhost, 1.4);
}

}  // namespace

EnvelopeLook lookOf(app::Session& session, const QString& owner, const QString& key) {
    app::EngineBridge& bridge = *session.bridge();
    EnvelopeLook look;
    look.spec = bridge.paramSpec(owner, key);
    look.overridden = bridge.isOverridden(owner, key);
    if (session.project()->envelope(owner, key).empty()) {
        const auto value = bridge.ownValue(owner, key);
        if (value && look.spec) look.unautomated = look.spec->toNormalized(*value);
    }
    return look;
}

std::optional<EnvelopeArea> areaAt(const std::vector<EnvelopeArea>& areas, const QPointF& pos) {
    for (const EnvelopeArea& area : areas) {
        if (area.rect.contains(pos)) return area;
    }
    return std::nullopt;
}

std::optional<int> nearestArea(const std::vector<EnvelopeArea>& areas, double y) {
    std::optional<int> best;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < areas.size(); ++i) {
        const double d = std::max({0.0, areas[i].rect.top() - y, y - areas[i].rect.bottom()});
        if (d < bestDistance) {
            best = static_cast<int>(i);
            bestDistance = d;
        }
    }
    return best;
}

std::optional<int> pointAt(const timeline::Timeline& view, const EnvelopeArea& area, const Envelope& points,
                           const QPointF& pos) {
    if (points.empty()) return std::nullopt;
    const int first = bisectLeft(points, view.xToBeat(pos.x() - kPointGrab));
    const int last = bisectRight(points, view.xToBeat(pos.x() + kPointGrab));
    std::optional<int> best;
    double bestDistance = kPointGrab;
    for (int i = first; i < last; ++i) {
        const AutomationPoint& point = points[static_cast<size_t>(i)];
        const double d = std::max(std::abs(view.beatToX(point.beat) - pos.x()), std::abs(area.y(point.value) - pos.y()));
        if (d <= bestDistance) {
            best = i;
            bestDistance = d;
        }
    }
    return best;
}

QPolygonF trace(const timeline::Timeline& view, const EnvelopeArea& area, const Envelope& points, double x0, double x1,
                const app::ParamSpec* quantize) {
    QPolygonF polygon;
    if (points.empty()) return polygon;
    const double b0 = view.xToBeat(x0), b1 = view.xToBeat(x1);
    const int count = static_cast<int>(points.size());
    const int first = std::max(0, bisectRight(points, b0) - 1);
    const int last = std::min(count - 1, bisectLeft(points, b1));
    std::vector<std::pair<double, double>> line;
    if (b0 < points.front().beat) line.emplace_back(x0, points.front().value);
    for (int j = first; j <= last; ++j) {
        const AutomationPoint& a = points[static_cast<size_t>(j)];
        const double xa = view.beatToX(a.beat);
        line.emplace_back(xa, a.value);
        if (j + 1 < count) {
            const AutomationPoint& b = points[static_cast<size_t>(j) + 1];
            const double xb = view.beatToX(b.beat);
            if (xb > xa && (quantize != nullptr || (a.curve != 0.0 && a.value != b.value))) {
                const double end = std::min(xb, x1);
                for (double x = std::max(xa, x0) + kSamplePixels; x < end; x += kSamplePixels)
                    line.emplace_back(x, app::automation::segmentValue(a, b, view.xToBeat(x)));
            }
        }
    }
    if (b1 > points.back().beat) line.emplace_back(x1, points.back().value);
    if (quantize != nullptr) {
        std::vector<std::pair<double, double>> steps;
        for (const auto& [x, raw] : line) {
            const double value = quantize->quantize(raw);
            if (!steps.empty() && steps.back().second != value) steps.emplace_back(x, steps.back().second);
            steps.emplace_back(x, value);
        }
        line = std::move(steps);
    }
    polygon.reserve(static_cast<qsizetype>(line.size()));
    for (const auto& [x, value] : line) polygon.append(QPointF(x, area.y(value)));
    return polygon;
}

bool inRange(LanesHost& host, const EnvelopeArea& area, const QPointF& pos) {
    const app::Selection& selection = *host.hostSession()->selection();
    const auto& range = selection.timeRange();
    if (!range || !selection.lanes().contains(app::LaneRef{area.owner, area.key})) return false;
    const double beat = viewOf(host).xToBeat(pos.x());
    return range->start <= beat && beat <= range->end;
}

std::optional<std::pair<double, double>> addTarget(LanesHost& host, const EnvelopeArea& area, const QPointF& pos,
                                                   Qt::KeyboardModifiers modifiers) {
    const Envelope points = projectOf(host).envelope(area.owner, area.key);
    if (!onLine(host, area, points, pos)) return std::nullopt;
    const timeline::Timeline& view = viewOf(host);
    const double beat = std::max(0.0, view.snapBeat(view.xToBeat(pos.x()), alt(modifiers)));
    const std::optional<double> value =
        points.empty() ? unautomatedValue(host, area) : app::automation::valueAt(points, beat);
    if (!value) return std::nullopt;
    const auto spec = specOf(host, area);
    return std::make_pair(beat, spec && spec->discrete() ? spec->quantize(*value) : *value);
}

std::unique_ptr<Gesture> press(LanesHost& host, const EnvelopeArea& area, const QPointF& pos,
                               Qt::KeyboardModifiers modifiers) {
    const Envelope points = projectOf(host).envelope(area.owner, area.key);
    const timeline::Timeline& view = viewOf(host);
    if (const auto index = pointAt(view, area, points, pos))
        return std::make_unique<PointGesture>(host, area, *index, pos, modifiers);
    if (alt(modifiers)) {
        if (const auto segment = app::automation::segmentIndex(points, view.xToBeat(pos.x())))
            return std::make_unique<CurveGesture>(host, area, *segment, pos);
    }
    const auto grab = grabAt(host, area, points, pos, modifiers);
    if (!grab && inRange(host, area, pos)) return std::make_unique<RangeGesture>(host, area, pos, modifiers);
    const auto end = endAt(host, area, points, pos);
    if (end && (!grab || !grab->add))  // near the line, not on it
        return std::make_unique<PointGesture>(host, area, *end, pos, modifiers, true);
    if (grab && grab->add) return addAndDrag(host, area, pos, modifiers, grab->target);
    if (grab) return std::make_unique<PointGesture>(host, area, grab->segment, pos, modifiers, false, true);
    return std::make_unique<LaneGesture>(host, area, pos, modifiers);
}

std::pair<std::optional<Hover>, QCursor> hover(LanesHost& host, const std::optional<EnvelopeArea>& area,
                                               const QPointF& pos, Qt::KeyboardModifiers modifiers) {
    if (!area) return {std::nullopt, QCursor(Qt::ArrowCursor)};
    const Envelope points = projectOf(host).envelope(area->owner, area->key);
    const timeline::Timeline& view = viewOf(host);
    const auto make = [&](Hover::Kind kind, std::optional<int> index = std::nullopt, double beat = 0.0,
                          double value = 0.0) {
        return Hover{area->owner, area->key, area->lane, kind, index, beat, value};
    };
    if (const auto index = pointAt(view, *area, points, pos))
        return {make(Hover::Kind::Point, index), QCursor(Qt::PointingHandCursor)};
    if (alt(modifiers) && app::automation::segmentIndex(points, view.xToBeat(pos.x())))
        return {std::nullopt, QCursor(Qt::SizeVerCursor)};
    const auto grab = grabAt(host, *area, points, pos, modifiers);
    if (!grab && inRange(host, *area, pos)) return {make(Hover::Kind::Range), QCursor(Qt::ArrowCursor)};
    const auto end = endAt(host, *area, points, pos);
    if (end && (!grab || !grab->add))
        return {make(Hover::Kind::End, end, view.xToBeat(pos.x())), QCursor(Qt::ArrowCursor)};
    if (grab && grab->add)
        return {make(Hover::Kind::Add, std::nullopt, grab->target.first, grab->target.second), addCursor()};
    if (grab) return {make(Hover::Kind::Segment, grab->segment), QCursor(Qt::ArrowCursor)};
    return {std::nullopt, QCursor(Qt::ArrowCursor)};
}

void addMenuEntries(LanesHost& host, const EnvelopeArea& area, const QPointF& pos, MenuEntries& menu) {
    app::Session* session = host.hostSession();
    const QString owner = area.owner, key = area.key;
    const Envelope points = projectOf(host).envelope(owner, key);
    if (const auto index = pointAt(viewOf(host), area, points, pos)) {
        const int at = *index;
        menu.add(QStringLiteral("Delete Breakpoint"), [session, owner, key, at] {
            session->editor()->deleteAutomationPoints(owner, key, {at});
            session->selection()->selectPoints(owner, key, QSet<int>());
        });
    }
    const QSet<int> selected = session->selection()->selectedPoints(owner, key);
    if (selected.size() > 1) {
        QList<int> indices(selected.begin(), selected.end());
        std::sort(indices.begin(), indices.end());
        menu.add(QStringLiteral("Delete Selected Breakpoints"), [session, owner, key, indices] {
            session->editor()->deleteAutomationPoints(owner, key, indices);
            session->selection()->selectPoints(owner, key, QSet<int>());
        });
    }
    menu.add(QStringLiteral("Delete Envelope"), [session, owner, key] { session->editor()->clearEnvelope(owner, key); })
        .enabled = !points.empty();
    if (session->bridge()->isOverridden(owner, key))
        menu.add(QStringLiteral("Re-Enable Automation"), [session, owner] { session->bridge()->reEnableAutomation(owner); });
    menu.addSeparator();
    if (area.lane >= 0) {
        const int lane = area.lane;
        menu.add(QStringLiteral("Remove Lane"),
                 [session, owner, lane] { session->editor()->removeAutomationLane(owner, lane); });
    }
    menu.add(QStringLiteral("Show Automation in New Lane"),
             [session, owner] { session->editor()->addAutomationLane(owner); });
    menu.add(QStringLiteral("Hide Automation"), [session, owner] { session->editor()->hideAutomation(owner); });
}

// --- Drawing --------------------------------------------------------------------------------------

void drawArea(SgPainter& p, const timeline::Timeline& view, const app::Selection& selection, const EnvelopeArea& area,
              const Envelope& points, const EnvelopeLook& look, const QRectF& visible, const std::optional<Hover>& hover,
              bool shade, bool gestureActive) {
    const QRectF clip = area.rect.intersected(visible);
    if (clip.isEmpty()) return;
    p.save();
    p.setClipRect(clip.adjusted(0, -kPointRadius - 1, 0, kPointRadius + 1));
    if (shade) p.fillRect(clip, kLaneBackground);
    p.setAntialiasing(true);
    const double x0 = clip.left() - 2, x1 = clip.right() + 2;
    if (points.empty()) {
        if (look.unautomated) dashedLine(p, x0, x1, area.y(*look.unautomated), kUnautomated, 1.5);
        drawGhost(p, view, area, hover, gestureActive);
        p.restore();
        return;
    }
    const QColor color = look.overridden ? kOverridden : kEnvelope;
    const app::ParamSpec* quantize = look.quantizer();
    const bool here = hover && hover->on(area);
    if (here && hover->kind == Hover::Kind::Segment && hover->index && *hover->index + 1 < static_cast<int>(points.size()))
        p.drawPolyline(segmentLine(view, area, points, *hover->index, quantize, x0, x1), color, 3.2);
    if (here && hover->kind == Hover::Kind::End && hover->index && *hover->index < static_cast<int>(points.size())) {
        const AutomationPoint& end = points[static_cast<size_t>(*hover->index)];
        const double y = area.y(quantize ? quantize->quantize(end.value) : end.value);
        p.drawLine(QPointF(view.beatToX(end.beat), y), QPointF(hover->beat < end.beat ? x0 : x1, y), color, 3.2);
    }
    p.drawPolyline(trace(view, area, points, x0, x1, quantize), color, 1.6);
    const QSet<int> selected = selection.selectedPoints(area.owner, area.key);
    for (int i = 0; i < static_cast<int>(points.size()); ++i) {
        const AutomationPoint& point = points[static_cast<size_t>(i)];
        const double x = view.beatToX(point.beat);
        if (x < x0 - kPointRadius || x > x1 + kPointRadius) continue;
        bool hovered = false;
        if (here && hover->index) {
            if (hover->kind == Hover::Kind::Point || hover->kind == Hover::Kind::End)
                hovered = *hover->index == i;
            else if (hover->kind == Hover::Kind::Segment)
                hovered = i - *hover->index == 0 || i - *hover->index == 1;
        }
        const double radius = kPointRadius + (hovered ? 1.0 : 0.0);
        const QColor fill = selected.contains(i) ? Theme::kSelectionOutline : (hovered ? color : Theme::kLane);
        const QPointF at(x, area.y(point.value));
        p.fillEllipse(at, radius, radius, fill);
        p.drawEllipse(QRectF(at.x() - radius, at.y() - radius, 2 * radius, 2 * radius), color, 1.4);
    }
    drawGhost(p, view, area, hover, gestureActive);
    p.restore();
}

void drawRange(SgPainter& p, const timeline::Timeline& view, const app::Selection& selection,
               const std::vector<EnvelopeArea>& areas, const QColor& tint) {
    const auto& range = selection.timeRange();
    if (!range || selection.lanes().isEmpty()) return;
    const double x0 = view.beatToX(range->start), x1 = view.beatToX(range->end);
    for (const EnvelopeArea& area : areas) {
        if (selection.lanes().contains(app::LaneRef{area.owner, area.key}))
            p.fillRect(QRectF(x0, area.rect.top(), x1 - x0, area.rect.height()), tint);
    }
}

void drawReadout(SgPainter& p, double width, const std::optional<Readout>& readout) {
    if (!readout) return;
    const QFont font = uiFont(8);
    const QFontMetricsF metrics(font);
    const QPointF at = readout->at;
    QRectF box(at.x() + 8, at.y() - metrics.height() - 6, metrics.horizontalAdvance(readout->text) + 10,
               metrics.height() + 4);
    if (box.top() < 0) box.moveTop(at.y() + 6);
    if (box.right() > width) box.moveRight(at.x() - 8);
    p.save();
    p.setAntialiasing(true);
    p.fillRoundedRect(box, 3, 3, Theme::kPanelAlt);
    p.drawRoundedRect(box, 3, 3, Theme::kBorder);
    p.restore();
    p.drawText(box, Qt::AlignCenter, readout->text, Theme::kText, font);
}

}  // namespace envelopes
}  // namespace sub::ui::arrangement
