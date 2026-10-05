#include "devices/CurveGraph.h"

#include "audio/EngineBridge.h"
#include "controls/KnobItem.h"
#include "devices/EditorPaint.h"
#include "model/Automation.h"
#include "model/Device.h"
#include "model/Project.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace sub::ui {

namespace sf = sub::app::sidechainFit;
using Values = sub::app::OrderedMap<QString, double>;

namespace {

const QStringList kRates{QStringLiteral("1/32"), QStringLiteral("1/16"), QStringLiteral("1/8"), QStringLiteral("3/16"),
                         QStringLiteral("1/4"),  QStringLiteral("3/8"),  QStringLiteral("1/2"), QStringLiteral("1 Bar")};
constexpr double kRateBeats[] = {0.125, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, -1.0};  // -1: a bar

const QColor kBackgroundTop(0x1d, 0x20, 0x27), kBackgroundBottom(0x10, 0x12, 0x16);
const QColor kGridMajor(255, 255, 255, 30), kGridMinor(255, 255, 255, 11);
const QColor kLabelColor(255, 255, 255, 90);
const QColor kCurveColor(0x8f, 0xe3, 0xff);
const QColor kDuckTop(120, 140, 255, 25), kDuckBottom(110, 220, 255, 95);
const QColor kKickColor(0xff, 0x9f, 0x5a);

// Automation's shape, as the editor had it (straight below |a| = 1e-4).
double shape(double x, double bend) {
    const double a = -bend * sub::app::automation::kCurvature;
    if (std::abs(a) < 1e-4)
        return x;
    return std::expm1(a * x) / std::expm1(a);
}

QString hintText() { return QStringLiteral("Choose the kick to listen to (the sidechain)…"); }

}  // namespace

CurveGraph::CurveGraph(QQuickItem* parent) : DeviceCanvas(parent), capture_(48000.0) {
    setImplicitSize(kMinimumWidth, kMinimumHeight);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setAcceptHoverEvents(true);
    setCursor(Qt::CrossCursor);
}

QString CurveGraph::pointParam(int index, const QString& name) {
    return QLatin1Char('p') + QString::number(index + 1) + QLatin1Char('_') + name;
}

double CurveGraph::curveValue(const std::vector<Point>& points, double x) {
    if (points.empty())
        return 1.0;
    if (x <= points.front().x)
        return points.front().y;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const Point& a = points[i];
        const Point& b = points[i + 1];
        if (x < b.x) {
            const double span = b.x - a.x;
            if (span <= 0)
                return b.y;
            const double bend = b.y >= a.y ? a.curve : -a.curve;
            return a.y + (b.y - a.y) * shape((x - a.x) / span, bend);
        }
    }
    return points.back().y;
}

Values CurveGraph::pointsValues(const std::vector<Point>& points) {
    Values values;
    for (int i = 0; i < kPoints; ++i) {
        const bool used = i < int(points.size());
        const Point p = used ? points[std::size_t(i)] : Point{1.0, 1.0, 0.0};
        values.insert(pointParam(i, QStringLiteral("used")), used ? 1.0 : 0.0);
        values.insert(pointParam(i, QStringLiteral("x")), p.x);
        values.insert(pointParam(i, QStringLiteral("y")), p.y);
        values.insert(pointParam(i, QStringLiteral("curve")), used ? p.curve : 0.0);
    }
    return values;
}

QString CurveGraph::formatMs(double ms) {
    return ms >= 1000 ? pythonFixed(ms / 1000, 2) + QStringLiteral(" s") : pythonFixed(ms, 0) + QStringLiteral(" ms");
}

const std::vector<std::pair<QString, std::vector<CurveGraph::Point>>>& CurveGraph::shapeList() {
    static const std::vector<std::pair<QString, std::vector<Point>>> shapes{
        {QStringLiteral("Pump"), {{0.0, 0.0, 0.0}, {0.1, 0.0, -0.45}, {1.0, 1.0, 0.0}}},
        {QStringLiteral("Smooth"), {{0.0, 0.0, -0.6}, {1.0, 1.0, 0.0}}},
        {QStringLiteral("Snappy"), {{0.0, 0.0, 0.0}, {0.06, 0.0, 0.55}, {0.45, 1.0, 0.0}, {1.0, 1.0, 0.0}}},
        {QStringLiteral("Linear"), {{0.0, 0.0, 0.0}, {1.0, 1.0, 0.0}}},
        {QStringLiteral("Hold"), {{0.0, 0.0, 0.0}, {0.4, 0.0, 0.0}, {0.42, 1.0, 0.0}, {1.0, 1.0, 0.0}}},
        {QStringLiteral("Gentle"), {{0.0, 0.45, 0.0}, {0.12, 0.45, -0.4}, {1.0, 1.0, 0.0}}},
        {QStringLiteral("Bounce"),
         {{0.0, 0.0, 0.0}, {0.12, 0.0, 0.5}, {0.3, 0.85, 0.0}, {0.36, 0.45, 0.5}, {0.7, 1.0, 0.0}, {1.0, 1.0, 0.0}}},
        {QStringLiteral("Stutter"),
         {{0.0, 0.0, 0.0},
          {0.2, 0.0, 0.0},
          {0.21, 1.0, 0.0},
          {0.4, 1.0, 0.0},
          {0.41, 0.0, 0.0},
          {0.6, 0.0, 0.0},
          {0.61, 1.0, 0.0},
          {1.0, 1.0, 0.0}}},
    };
    return shapes;
}

QStringList CurveGraph::shapes() const {
    QStringList names;
    for (const auto& [name, points] : shapeList())
        names << name;
    return names;
}

// --- The model --------------------------------------------------------------------------------

void CurveGraph::sync() {
    std::vector<Point> points;
    if (alive()) {
        for (int i = 0; i < kPoints; ++i) {
            if (value(pointParam(i, QStringLiteral("used"))) >= 0.5)
                points.push_back({value(pointParam(i, QStringLiteral("x"))), value(pointParam(i, QStringLiteral("y"))),
                                  value(pointParam(i, QStringLiteral("curve")))});
        }
        std::stable_sort(points.begin(), points.end(), [](const Point& a, const Point& b) { return a.x < b.x; });
    }
    points_ = points;
    depth_ = value(QStringLiteral("depth")) / 100.0;
    threshold_ = value(QStringLiteral("threshold"));
    const bool synced = value(QStringLiteral("sync")) >= 0.5;
    const int rate = std::clamp(int(std::nearbyint(value(QStringLiteral("rate")))), 0, int(kRates.size()) - 1);
    if (synced) {
        double beats = kRateBeats[rate];
        const sub::app::Project* project = session() ? session()->project() : nullptr;
        if (beats < 0)
            beats = project ? project->timeSignature().beatsPerBar() : 4.0;  // a bar, as the engine has it
        const double tempo = project && project->tempo() > 0 ? project->tempo() : 120.0;
        lengthMs_ = beats * 60000.0 / tempo;
        lengthText_ = kRates.at(rate) + QStringLiteral("  ·  ") + formatMs(lengthMs_);
    } else {
        lengthMs_ = std::max(1.0, value(QStringLiteral("length")));
        lengthText_ = formatMs(lengthMs_);
    }
    const sub::app::Device* found = device();
    hint_ = found && std::nearbyint(value(QStringLiteral("trigger"))) == 0 && !found->sidechain ? hintText() : QString();
    if (value(QStringLiteral("autofit")) < 0.5)
        autoGesture_.clear();
    if (selected_ >= int(points_.size()))
        selected_ = -1;
    update();
    Q_EMIT curveChanged();
}

void CurveGraph::set(const Values& values, const QString& mergeKey, const QString& text) {
    setParams(values, mergeKey, text);
}

void CurveGraph::setPoints(const std::vector<Point>& points, const QString& mergeKey, const QString& text,
                           const Values& extra) {
    Values values = pointsValues(points);
    for (const auto& [key, value] : extra)
        values.insert(key, value);
    set(values, mergeKey, text);
}

void CurveGraph::removePoint(int index) {
    std::vector<Point> points = points_;
    if (!(0 < index && index < int(points.size()) - 1))
        return;  // (the ends stay)
    points.erase(points.begin() + index);
    selected_ = -1;
    hover_ = {QString(), -1};
    setPoints(points, QString(), QStringLiteral("Delete Sidechain Point"));
    update();
}

void CurveGraph::applyShape(int index) {
    if (index < 0 || index >= int(shapeList().size()))
        return;
    const auto& [name, points] = shapeList()[std::size_t(index)];
    setPoints(points, QString(), QStringLiteral("Sidechain Shape: ") + name);
}

void CurveGraph::flip() {
    // Up for down: ducking becomes swelling (and the other way round).
    std::vector<Point> points = points_;
    for (Point& p : points)
        p.y = 1.0 - p.y;
    setPoints(points, QString(), QStringLiteral("Flip Sidechain Curve"));
}

void CurveGraph::resetCurve() { setPoints(shapeList().front().second, QString(), QStringLiteral("Reset Sidechain Curve")); }

// --- Fitting ----------------------------------------------------------------------------------

Values CurveGraph::fitValues(const sf::Fit& fit) const {
    const double crossover = std::clamp(std::nearbyint(fit.spectra.high * 1.5), 30.0, 1000.0);
    Values values = pointsValues(fit.points);
    values.insert(QStringLiteral("length"), fit.length);
    values.insert(QStringLiteral("sync"), 0.0);
    values.insert(QStringLiteral("crossover"), crossover);
    return values;
}

void CurveGraph::fitNow() {
    if (!fit_) {
        if (session())
            Q_EMIT session()->bridge()->statusMessage(
                QStringLiteral("Sidechain: play the kick (and the bass) first, to fit to it"));
        return;
    }
    set(fitValues(*fit_), QString(), QStringLiteral("Fit Sidechain to Kick"));
}

void CurveGraph::setAuto(bool on) {
    autoGesture_ = on ? newGestureKey() : QString();
    set({{QStringLiteral("autofit"), on ? 1.0 : 0.0}}, QString(),
        on ? QStringLiteral("Auto Fit") : QStringLiteral("Stop Auto Fit"));
    if (on) {
        if (autoGesture_.isEmpty())
            autoGesture_ = newGestureKey();  // (the sync after setting it may have cleared it)
        if (fit_)
            applyAuto();
    }
}

void CurveGraph::applyAuto() {
    if (!fit_ || drag_)
        return;
    if (autoGesture_.isEmpty())
        autoGesture_ = newGestureKey();
    set(fitValues(*fit_), autoGesture_, QStringLiteral("Auto Fit Sidechain"));
}

void CurveGraph::setCharacter(int index) {
    set({{QStringLiteral("character"), double(index)}}, QString(), QStringLiteral("Change Fit Character"));
    analyze();
    if (value(QStringLiteral("autofit")) >= 0.5)
        applyAuto();
}

void CurveGraph::analyze() {
    if (kicks_.empty())
        return;
    const int pre = int(sf::kPreSeconds * capture_.sampleRate());
    const auto fit = sf::analyze(kicks_, bass_, capture_.sampleRate(),
                                 int(std::nearbyint(value(QStringLiteral("character")))), pre);
    if (fit)
        fit_ = fit;
    update();
    Q_EMIT fitChanged();
}

// --- Displays ----------------------------------------------------------------------------------

void CurveGraph::refreshDisplays() {
    const double rate = sampleRate();
    if (capture_.sampleRate() != rate)
        capture_ = sf::Capture(rate);
    for (const auto& [name, stream] : {std::pair{QStringLiteral("key"), sf::Capture::Stream::Key},
                                       std::pair{QStringLiteral("input"), sf::Capture::Stream::Input},
                                       std::pair{QStringLiteral("phase"), sf::Capture::Stream::Phase}}) {
        const auto [start, values] = readDisplayAt(name);
        capture_.feed(stream, start, values);
    }
    const bool hit = capture_.hitCount() != hitsSeen_;
    hitsSeen_ = capture_.hitCount();
    const double phase = capture_.lastPhase();
    const double length = lengthMs_ * capture_.sampleRate() / 1000.0;
    tick(phase >= 0 ? std::optional<double>(phase / length) : std::nullopt, capture_.takeKeyPeak(), hit);
    const std::vector<sf::Capture::Hit> done = capture_.hits();
    if (!done.empty()) {
        for (const auto& [kick, input] : done)
            kicks_.emplace_back(kick.begin(), kick.end());
        if (kicks_.size() > std::size_t(kKeepKicks))
            kicks_.erase(kicks_.begin(), kicks_.end() - kKeepKicks);
        bass_.assign(done.back().second.begin(), done.back().second.end());
        analyze();
        if (value(QStringLiteral("autofit")) >= 0.5)
            applyAuto();
    }
}

void CurveGraph::tick(std::optional<double> phase, double keyPeak, bool hit) {
    if (phase)
        trail_.push_back(*phase);
    else if (!trail_.empty())
        trail_.push_back(-1.0);
    if (trail_.size() > std::size_t(kTrail))
        trail_.erase(trail_.begin(), trail_.end() - kTrail);
    if (!trail_.empty() && std::all_of(trail_.begin(), trail_.end(), [](double x) { return x < 0; }))
        trail_.clear();
    const double level = keyPeak > 1e-6 ? 20 * std::log10(keyPeak) : kMeterFloor;
    level_ = std::max(level, level_ - 1.5);
    flash_ = hit ? 1.0 : flash_ * 0.82;
    update();
    Q_EMIT ticked();
}

// --- Geometry -------------------------------------------------------------------------------------

QRectF CurveGraph::plot() const {
    return QRectF(0, 0, width(), height()).adjusted(6, 16, -(kMeterWidth + 12), -14);
}

QPointF CurveGraph::toScreen(double x, double y) const {
    const QRectF r = plot();
    return QPointF(r.left() + x * r.width(), r.bottom() - y * r.height());
}

QPointF CurveGraph::fromScreen(const QPointF& pos) const {
    const QRectF r = plot();
    return QPointF((pos.x() - r.left()) / std::max(r.width(), 1.0), (r.bottom() - pos.y()) / std::max(r.height(), 1.0));
}

std::pair<QString, int> CurveGraph::hit(const QPointF& pos) const {
    int best = -1;
    double bestDistance = kHitRadius;
    for (std::size_t i = 0; i < points_.size(); ++i) {
        const QPointF at = toScreen(points_[i].x, points_[i].y);
        const double d = std::hypot(pos.x() - at.x(), pos.y() - at.y());
        if (d < bestDistance) {
            best = int(i);
            bestDistance = d;
        }
    }
    if (best >= 0)
        return {QStringLiteral("point"), best};
    const double x = fromScreen(pos).x();
    if (!(0.0 <= x && x <= 1.0) || points_.size() < 2)
        return {QString(), -1};
    if (std::abs(toScreen(x, curveValue(points_, x)).y() - pos.y()) > kCurveHit)
        return {QString(), -1};
    const int before = int(std::count_if(points_.begin(), points_.end(), [x](const Point& p) { return p.x <= x; }));
    return {QStringLiteral("segment"), std::clamp(before - 1, 0, int(points_.size()) - 2)};
}

QRectF CurveGraph::hintRect() const {
    if (hint_.isEmpty())
        return QRectF();
    const QRectF r = plot();
    const double w = SgPainter::textWidth(hint_, uiFont(8)) + 20;
    return QRectF(r.center().x() - w / 2, r.center().y() - 11, w, 22);
}

QString CurveGraph::readoutText(const Point& point) const {
    const double gain = 1.0 - depth_ * (1.0 - point.y);
    const QString level =
        gain > 1e-4 ? pythonFixed(20 * std::log10(gain), 1) + QStringLiteral(" dB") : QStringLiteral("-∞ dB");
    return formatMs(point.x * lengthMs_) + QStringLiteral("  ·  ") + level;
}

// --- Editing ----------------------------------------------------------------------------------------

void CurveGraph::mousePressEvent(QMouseEvent* event) {
    const QPointF pos = event->position();
    const bool second = secondPressOfDoubleClick(event);
    if (event->button() == Qt::RightButton) {
        const auto found = hit(pos);
        const bool inner = found.first == QLatin1String("point") && 0 < found.second &&
                           found.second < int(points_.size()) - 1;
        Q_EMIT contextMenuRequested(pos, inner ? found.second : -1);
        return;
    }
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    if (second)
        return;  // (the double-click follows: as a widget, it has it instead)
    forceActiveFocus(Qt::MouseFocusReason);  // (Delete removes the selected point)
    if (hintRect().contains(pos)) {
        Q_EMIT sidechainMenuRequested(plot().center());
        return;
    }
    std::vector<Point> points = points_;
    const auto found = hit(pos);
    const bool onPoint = found.first == QLatin1String("point");
    if (onPoint && (event->modifiers() & Qt::AltModifier)) {
        removePoint(found.second);
        return;
    }
    if (!found.first.isEmpty())
        added_ = -1;
    if (onPoint) {
        selected_ = found.second;
        drag_ = Drag{QStringLiteral("point"), found.second, pos, points, newGestureKey()};
    } else if (!found.first.isEmpty()) {
        drag_ = Drag{QStringLiteral("bend"), found.second, pos, points, newGestureKey()};
    } else if (int(points.size()) < kPoints && plot().adjusted(-4, -4, 4, 4).contains(pos)) {
        const QPointF at = fromScreen(pos);
        const double x = std::clamp(at.x(), 0.0, 1.0), y = std::clamp(at.y(), 0.0, 1.0);
        if (!points.empty() && !(0.0 < x && x < 1.0))
            return;
        const int index = int(std::count_if(points.begin(), points.end(), [x](const Point& p) { return p.x <= x; }));
        const Point point{x, y, index > 0 ? points[std::size_t(index) - 1].curve : 0.0};
        points.insert(points.begin() + index, point);
        const QString gesture = newGestureKey();
        setPoints(points, gesture, QStringLiteral("Add Sidechain Point"));
        added_ = index;
        addedClock_.start();
        selected_ = index;
        drag_ = Drag{QStringLiteral("point"), index, pos, points, gesture, QStringLiteral("Add Sidechain Point")};
    }
    update();
    Q_EMIT curveChanged();
}

void CurveGraph::mouseMoveEvent(QMouseEvent* event) {
    if (!drag_) {
        hoverAt(event->position());
        return;
    }
    const QPointF pos = event->position();
    const Drag& drag = *drag_;
    const double fine = event->modifiers() & Qt::ShiftModifier ? 0.2 : 1.0;
    std::vector<Point> points = drag.points;
    const int index = drag.index;
    if (index < 0 || index >= int(points.size()))
        return;
    const QRectF r = plot();
    const double dx = (pos.x() - drag.origin.x()) * fine / std::max(r.width(), 1.0);
    const double dy = (pos.y() - drag.origin.y()) * fine;
    if (drag.kind == QLatin1String("point")) {
        const Point old = points[std::size_t(index)];
        double x = old.x;  // the ends stay at the ends
        if (index != 0 && index != int(points.size()) - 1)
            x = std::clamp(old.x + dx, points[std::size_t(index) - 1].x, points[std::size_t(index) + 1].x);
        const double y = std::clamp(old.y - dy / std::max(r.height(), 1.0), 0.0, 1.0);
        points[std::size_t(index)] = {x, y, old.curve};
        readout_ = std::pair{toScreen(x, y), readoutText(points[std::size_t(index)])};
    } else {
        const Point old = points[std::size_t(index)];
        const double curve = std::clamp(old.curve - dy / 60.0, -1.0, 1.0);
        points[std::size_t(index)] = {old.x, old.y, curve};
        readout_ = std::pair{pos, QStringLiteral("Bend ") + pythonSigned(curve, 2)};
    }
    setPoints(points, drag.gesture, drag.text);
    update();
}

void CurveGraph::endDrag() {
    drag_.reset();
    readout_.reset();
    update();
}

void CurveGraph::mouseReleaseEvent(QMouseEvent*) { endDrag(); }

void CurveGraph::mouseUngrabEvent() { endDrag(); }

void CurveGraph::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton)
        return;
    const auto found = hit(event->position());
    if (found.first.isEmpty() ||
        (found.first == QLatin1String("point") && found.second == added_ && addedClock_.isValid() &&
         addedClock_.elapsed() < 500))
        return;
    endDrag();
    if (found.first == QLatin1String("point")) {
        removePoint(found.second);
    } else {
        std::vector<Point> points = points_;
        points[std::size_t(found.second)].curve = 0.0;
        setPoints(points, QString(), QStringLiteral("Straighten Sidechain Curve"));
    }
}

void CurveGraph::hoverMoveEvent(QHoverEvent* event) {
    if (!drag_)
        hoverAt(event->position());
}

void CurveGraph::hoverAt(const QPointF& pos) {
    const auto found = hit(pos);
    if (found != hover_) {
        hover_ = found;
        update();
    }
    if (hintRect().contains(pos))
        setCursor(Qt::PointingHandCursor);
    else if (found.first == QLatin1String("point"))
        setCursor(Qt::SizeAllCursor);
    else if (!found.first.isEmpty())
        setCursor(Qt::SizeVerCursor);
    else
        setCursor(Qt::CrossCursor);
}

void CurveGraph::hoverLeaveEvent(QHoverEvent*) {
    if (!drag_) {
        hover_ = {QString(), -1};
        update();
    }
}

void CurveGraph::wheelEvent(QWheelEvent* event) {
    const auto found = hit(event->position());
    const double notches = event->angleDelta().y() / 120.0;
    if (found.first.isEmpty() || notches == 0.0) {
        event->ignore();
        return;
    }
    std::vector<Point> points = points_;
    const int index =
        found.first == QLatin1String("segment") ? found.second : std::min(found.second, int(points.size()) - 2);
    if (index < 0)
        return;
    const double fine = event->modifiers() & Qt::ShiftModifier ? 0.25 : 1.0;
    // Notches closer than 0.6 s are one gesture.
    if (wheelGesture_.isEmpty() || !wheelClock_.isValid() || wheelClock_.elapsed() > 600)
        wheelGesture_ = newGestureKey();
    wheelClock_.start();
    Point& a = points[std::size_t(index)];
    a.curve = std::clamp(a.curve + 0.08 * notches * fine, -1.0, 1.0);
    setPoints(points, wheelGesture_, QStringLiteral("Bend Sidechain Curve"));
    event->accept();
}

bool CurveGraph::event(QEvent* event) {
    // Delete removes the selected point, not whatever the app has Delete for.
    if (event->type() == QEvent::ShortcutOverride && selected_ >= 0) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Delete || key == Qt::Key_Backspace) {
            event->accept();
            return true;
        }
    }
    return DeviceCanvas::event(event);
}

void CurveGraph::keyPressEvent(QKeyEvent* event) {
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && selected_ >= 0) {
        removePoint(selected_);
        return;
    }
    event->ignore();
}

// --- Painting -------------------------------------------------------------------------------------

void CurveGraph::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const QRectF rect(0, 0, width(), height());
    QLinearGradient background(rect.topLeft(), rect.bottomLeft());
    background.setColorAt(0, kBackgroundTop);
    background.setColorAt(1, kBackgroundBottom);
    p.fillRect(rect, background);
    const QRectF r = plot();
    const double length = lengthMs_;
    const QFont small = uiFont(7);

    // The grid: time across (in steps of at most 8), the level the depth makes of each quarter up.
    double step = 1000;
    for (double s : {1.0, 2.0, 5.0, 10.0, 20.0, 25.0, 50.0, 100.0, 200.0, 250.0, 500.0, 1000.0}) {
        if (length / s <= 8) {
            step = s;
            break;
        }
    }
    for (double ms = 0.0; ms <= length + 1e-6; ms += step) {
        const double x = r.left() + ms / length * r.width();
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()), ms == 0 ? kGridMajor : kGridMinor);
        if (0 < ms && ms < length - step / 2)
            p.drawText(QRectF(x - 24, r.bottom() + 1, 48, 12), Qt::AlignCenter, formatMs(ms), kLabelColor, small);
    }
    for (double y : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        const double at = r.bottom() - y * r.height();
        p.drawLine(QPointF(r.left(), at), QPointF(r.right(), at), y == 0.0 || y == 1.0 ? kGridMajor : kGridMinor);
        if (0.0 < y && y < 1.0) {
            const double gain = 1.0 - depth_ * (1.0 - y);
            const QString text = gain > 1e-3 ? pythonFixed(20 * std::log10(gain), 0) : QStringLiteral("-∞");
            p.drawText(QRectF(r.right() - 30, at - 12, 28, 11), Qt::AlignRight | Qt::AlignVCenter, text, kLabelColor,
                       small);
        }
    }

    // The kick's envelope where it clashes, and the curve the fit calls for (dashed).
    if (fit_ && fit_->times.size() >= 2) {
        const sf::Fit& fit = *fit_;
        std::size_t shown = 0;
        while (shown < fit.times.size() && fit.times[shown] <= length)
            ++shown;
        if (shown >= 2) {
            const std::size_t every = std::max<std::size_t>(1, shown / std::size_t(std::max(1, int(r.width()))));
            std::vector<QPointF> envelope, target;
            for (std::size_t i = 0; i < shown; i += every) {
                const double x = fit.times[i] / length;
                envelope.push_back(toScreen(x, 0.92 * fit.envelope[i]));
                target.push_back(toScreen(x, fit.target[i]));
            }
            QLinearGradient gradient(r.topLeft(), r.bottomLeft());
            gradient.setColorAt(0, withAlpha(kKickColor, 70));
            gradient.setColorAt(1, withAlpha(kKickColor, 12));
            p.fillToBaseline(envelope.data(), int(envelope.size()), r.bottom(), gradient);
            drawDashedPolyline(p, target, withAlpha(kKickColor, 150), 1.2);
        }
    }

    // The curve, and what it takes away (from it up to untouched).
    const int steps = std::max(2, int(r.width()));
    std::vector<QPointF> curve;
    curve.reserve(std::size_t(steps) + 1);
    for (int i = 0; i <= steps; ++i) {
        const double x = double(i) / steps;
        curve.push_back(toScreen(x, curveValue(points_, x)));
    }
    QLinearGradient duck(r.topLeft(), r.bottomLeft());
    duck.setColorAt(0, kDuckTop);
    duck.setColorAt(1, kDuckBottom);
    p.fillToBaseline(curve.data(), int(curve.size()), r.top(), duck);
    p.drawPolyline(curve.data(), int(curve.size()), withAlpha(kCurveColor, 45), 6, Qt::RoundCap);
    p.drawPolyline(curve.data(), int(curve.size()), kCurveColor, 1.8, Qt::RoundCap);
    const bool bending = drag_ && drag_->kind == QLatin1String("bend");
    const bool hovering = !drag_ && hover_.first == QLatin1String("segment");
    if (bending || hovering) {
        const int index = bending ? drag_->index : hover_.second;
        if (index >= 0 && index + 1 < int(points_.size())) {
            const Point a = points_[std::size_t(index)], b = points_[std::size_t(index) + 1];
            const int count = std::max(2, int((b.x - a.x) * r.width()));
            std::vector<QPointF> segment;
            for (int i = 0; i <= count; ++i) {
                const double x = a.x + (b.x - a.x) * i / count;
                segment.push_back(toScreen(x, i < count ? curveValue(points_, std::min(x, b.x - 1e-9)) : b.y));
            }
            p.drawPolyline(segment.data(), int(segment.size()), QColor(255, 255, 255, 200), 2.6, Qt::RoundCap);
        }
    }

    // The playhead and its trail.
    for (std::size_t age = 0; age < trail_.size(); ++age) {
        const double x = trail_[trail_.size() - 1 - age];
        if (!(0.0 <= x && x <= 1.0))
            continue;
        const QPointF at = toScreen(x, curveValue(points_, x));
        const double fade = 1.0 - double(age) / kTrail;
        if (age == 0) {
            QLinearGradient line(QPointF(at.x(), r.top()), QPointF(at.x(), r.bottom()));
            line.setColorAt(0, QColor(255, 255, 255, 0));
            line.setColorAt(1, QColor(255, 255, 255, 70));
            p.fillRect(QRectF(at.x() - 0.5, r.top(), 1, r.height()), line);
            p.fillEllipse(at, 9, 9, withAlpha(kCurveColor, 60));
        }
        p.fillEllipse(at, 3.5 * fade + 1, 3.5 * fade + 1, withAlpha(Qt::white, int(230 * fade)));
    }

    // The points.
    for (std::size_t i = 0; i < points_.size(); ++i) {
        const QPointF at = toScreen(points_[i].x, points_[i].y);
        const bool hovered = (hover_.first == QLatin1String("point") && hover_.second == int(i)) ||
                             (drag_ && drag_->index == int(i) && drag_->kind == QLatin1String("point"));
        const double radius = hovered ? kPointHoverRadius : kPointRadius;
        if (hovered)
            p.fillEllipse(at, radius + 4, radius + 4, withAlpha(kCurveColor, 70));
        const QRectF disc(at.x() - radius, at.y() - radius, 2 * radius, 2 * radius);
        p.fillEllipse(disc, hovered || int(i) == selected_ ? kCurveColor : QColor(0x15, 0x18, 0x1e));
        p.drawEllipse(disc, QColor(255, 255, 255, 230), 1.4);
    }

    // The key's level against the threshold, at the right; a hit lights it.
    const QRectF meter(width() - kMeterWidth - 5, r.top(), kMeterWidth, r.height());
    auto meterY = [&](double db) {
        return meter.bottom() - (std::min(0.0, std::max(kMeterFloor, db)) - kMeterFloor) / -kMeterFloor * meter.height();
    };
    p.fillRoundedRect(meter, 2, 2, QColor(255, 255, 255, 14));
    const double top = meterY(level_);
    if (level_ > kMeterFloor)
        p.fillRoundedRect(QRectF(meter.left(), top, meter.width(), meter.bottom() - top), 2, 2,
                          level_ >= threshold_ ? withAlpha(kKickColor, 210) : QColor(255, 255, 255, 90));
    if (flash_ > 0.02)
        p.fillRoundedRect(meter.adjusted(-2, -2, 2, 2), 3, 3, withAlpha(kKickColor, int(120 * flash_)));
    const double at = meterY(threshold_);
    p.drawLine(QPointF(meter.left() - 3, at), QPointF(meter.right() + 1, at), QColor(255, 255, 255, 220), 1.5);

    // The labels: the clash band, the length, the hint.
    if (fit_)
        p.drawText(QRectF(r.left() + 2, 2, 160, 12), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("Kick %1–%2 Hz").arg(pythonFixed(fit_->spectra.low, 0), pythonFixed(fit_->spectra.high, 0)),
                   withAlpha(kKickColor, 200), small);
    p.drawText(QRectF(r.right() - 160, 2, 158, 12), Qt::AlignRight | Qt::AlignVCenter, lengthText_, kLabelColor, small);
    if (!hint_.isEmpty()) {
        const QRectF box = hintRect();
        p.fillRoundedRect(box, 11, 11, QColor(10, 11, 14, 210));
        p.drawRoundedRect(box, 11, 11, QColor(255, 255, 255, 40), 1);
        p.drawText(box, Qt::AlignCenter, hint_, QColor(255, 255, 255, 200), uiFont(8));
    }

    // While dragging: where the point is, or the bend.
    if (readout_) {
        const auto& [point, text] = *readout_;
        const double w = SgPainter::textWidth(text, small) + 10;
        QRectF badge(point.x() + 10, point.y() - 22, w, 15);
        if (badge.right() > r.right())
            badge.moveRight(point.x() - 10);
        if (badge.top() < 0)
            badge.moveTop(point.y() + 8);
        p.fillRoundedRect(badge, 4, 4, QColor(10, 11, 14, 220));
        p.drawText(badge, Qt::AlignCenter, text, QColor(255, 255, 255, 220), small);
    }
}

}  // namespace sub::ui
