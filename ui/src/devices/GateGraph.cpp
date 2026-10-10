#include "devices/GateGraph.h"

#include "audio/GateResponse.h"
#include "input/GestureKey.h"
#include "model/Device.h"
#include "model/ParamSpec.h"
#include "model/Project.h"
#include "model/Track.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QHoverEvent>
#include <QLinearGradient>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace sub::ui {

namespace {

constexpr const char* kStreamIds[GateGraph::kStreams] = {"input", "output", "key", "open"};
constexpr qint64 kMask = GateGraph::kCapacity - 1;
constexpr double kLagSeconds = 0.03;     // the drawing stays this far behind the newest value
constexpr double kMaxLagSeconds = 0.1;   // and jumps on when it falls further behind
constexpr double kQuietSeconds = 0.25;   // no values for this long: the engine stopped
constexpr double kRecentSeconds = 0.1;   // a tick's levels are of this much at most (after a backlog)
constexpr double kPingSeconds = 0.25;    // the ring as the gate opens
constexpr double kPulseSeconds = 1.2;    // the listening label's pulse
constexpr double kShadeAlpha = 36.0;     // the blue where the gate is open, over the levels
constexpr double kKeyFall = 300.0;       // dB/s the key dot falls (24 dB in 80 ms: it moves, it doesn't lag)
constexpr double kPi = 3.14159265358979323846;

// What a stream holds where it has no values: the bottom of the axis, closed.
float clearValue(int stream) { return stream == GateGraph::Open ? 0.f : float(GateGraph::kFloorDb); }

qint64 floorDiv(qint64 a, qint64 b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// The meters, in wells as the Compressor's (panel grey on the display's black), on the plot's own dB
// axis: `rect` spans 0 dB at its top to the axis' floor at its bottom, a line across every 12 dB
// where the plot has its grid (the In meter is the house's: drawLevelMeter).
double meterY(double db, const QRectF& rect) { return dbToY(db, rect, GateGraph::kFloorDb, 0.0); }

// How far the gate turns down what comes in: down from the top by as many dB (to the floor's depth
// when shut, the whole well at a silent floor), so it reads against the figures as the In meter does.
void drawGateMeter(SgPainter& p, const QRectF& rect, double reductionDb) {
    p.fillRect(rect, Theme::kPanel);
    const double bottom = meterY(-std::max(0.0, reductionDb), rect);
    if (bottom > rect.top() + 0.01) {
        QLinearGradient gradient(rect.topLeft(), rect.bottomLeft());
        gradient.setColorAt(0, withAlpha(Theme::kAccent, 150));
        gradient.setColorAt(1, Theme::kAccent);
        p.fillRect(QRectF(rect.left(), rect.top(), rect.width(), bottom - rect.top()), gradient);
    }
    for (int db = -12; db > GateGraph::kFloorDb; db -= 12)
        p.drawLine(QPointF(rect.left(), meterY(db, rect)), QPointF(rect.right(), meterY(db, rect)),
                   withAlpha(Theme::kMeterBg, 145));
}

}  // namespace

GateGraph::GateGraph(QQuickItem* parent) : DeviceCanvas(parent) {
    setImplicitSize(kWidth, kMinimumHeight);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);  // (right-click off the lines: the frame's menu)
    setAcceptHoverEvents(true);
    for (int s = 0; s < kStreams; ++s) {
        rings_[std::size_t(s)].assign(kCapacity, clearValue(s));
        last_[std::size_t(s)] = clearValue(s);
    }
    ends_.fill(-1);
    inMeter_.reset(kFloorDb);
    keyMeter_.reset(kFloorDb);
    threshold_.snap(thresholdDb_);
    return_.snap(thresholdDb_ - returnDb_);
    // The source's name: renaming it changes its own track, not this device's.
    connect(this, &DeviceCanvas::deviceChanged, this, [this] {
        disconnect(projectConnection_);
        if (session())
            projectConnection_ = connect(session()->project(), &sub::app::Project::trackChanged, this,
                                         [this](const QString& trackId) {
                                             if (!sidechainTrack_.isEmpty() && trackId == sidechainTrack_)
                                                 readSidechain();
                                         });
    });
}

// --- Geometry ------------------------------------------------------------------------------

QRectF GateGraph::plot() const {
    return QRectF(5, 5, std::max(1.0, width() - 5 - kRightStrip), std::max(1.0, height() - 10));
}

double GateGraph::yOf(double db) const {
    const QRectF r = plot();
    if (!std::isfinite(db))
        db = db > 0 ? kCeilingDb : kFloorDb;
    return r.top() + (kCeilingDb - std::clamp(db, kFloorDb, kCeilingDb)) / (kCeilingDb - kFloorDb) * r.height();
}

QRectF GateGraph::inMeter() const {
    const double top = yOf(0.0);
    return QRectF(width() - 36, top, 6, yOf(kFloorDb) - top);
}

QRectF GateGraph::gateMeter() const { return inMeter().translated(20, 0); }  // (its caption clear of the In's)

bool GateGraph::floorIsSilent(double floorDb) const { return sub::app::gateFloorIsSilent(floorDb); }

double GateGraph::dbAt(double y) const {
    const QRectF r = plot();
    return std::clamp(kCeilingDb - (y - r.top()) / r.height() * (kCeilingDb - kFloorDb), kFloorDb, kCeilingDb);
}

int GateGraph::bucketSize() const {
    const double px = plot().width() / std::max(1.0, kHistorySeconds * rate_);
    return px >= 1.0 ? 1 : int(std::ceil(1.0 / px - 1e-9));
}

qint64 GateGraph::historyBegin() const {
    qint64 latest = newest_;
    for (const qint64 end : ends_)
        latest = std::max(latest, end);
    return std::max(begin_, latest - kCapacity);  // (a stream a value ahead has written over its oldest)
}

int GateGraph::historySize() const { return started_ ? int(std::max<qint64>(0, newest_ - historyBegin())) : 0; }

float GateGraph::historyAt(int stream, qint64 index) const {
    if (stream < 0 || stream >= kStreams || !started_ || index < historyBegin() || index >= ends_[std::size_t(stream)])
        return stream >= 0 && stream < kStreams ? clearValue(stream) : 0.f;
    return rings_[std::size_t(stream)][std::size_t(index & kMask)];
}

// --- The parameters ------------------------------------------------------------------------

void GateGraph::sync() {
    thresholdDb_ = value(QStringLiteral("threshold"));
    returnDb_ = value(QStringLiteral("return"));
    floorDb_ = value(QStringLiteral("floor"));
    listening_ = value(QStringLiteral("sc_listen")) >= 0.5;
    const bool eqOn = value(QStringLiteral("sc_eq")) >= 0.5;
    readSidechain();
    showKey_ = keyed_ || eqOn;
    threshold_.target = thresholdDb_;
    return_.target = sub::app::gateCloseDb(thresholdDb_, returnDb_);
    if (!synced_ || drag_ != Line::None) {  // the lines start where they are, and follow a drag at once
        threshold_.snap(threshold_.target);
        return_.snap(return_.target);
    }
    synced_ = true;
    drawnKeyAbove_ = keyAbove();  // (the repaint below draws the dot's colour for the threshold now)
    update();
}

void GateGraph::readSidechain() {
    const sub::app::Device* found = device();
    const bool keyed = found && found->sidechain && !found->sidechain->trackId.isEmpty();
    sidechainTrack_ = keyed ? found->sidechain->trackId : QString();
    const sub::app::Track* track = keyed && session() ? session()->project()->findTrack(sidechainTrack_) : nullptr;
    const QString name = track ? track->name : QString();
    if (keyed == keyed_ && name == sidechainName_)
        return;
    keyed_ = keyed;
    sidechainName_ = name;
    Q_EMIT sidechainChanged();
}

// --- The displays --------------------------------------------------------------------------

void GateGraph::write(int stream, qint64 index, float value) {
    if (!std::isfinite(value))
        value = clearValue(stream);
    rings_[std::size_t(stream)][std::size_t(index & kMask)] = value;
    // Whether the picture changes here: levels as the axis draws them, how open to a step's worth.
    const float before = last_[std::size_t(stream)];
    const bool differs =
        stream == Open ? std::abs(value - before) > 1e-3f
                       : std::abs(std::clamp(value, float(kFloorDb), float(kCeilingDb)) -
                                  std::clamp(before, float(kFloorDb), float(kCeilingDb))) > 0.01f;
    if (differs)
        lastVaried_ = std::max(lastVaried_, index);
    last_[std::size_t(stream)] = value;
}

bool GateGraph::readStreams(bool& restarted) {
    std::array<std::pair<qint64, std::vector<float>>, kStreams> reads;
    bool any = false;
    for (int s = 0; s < kStreams; ++s) {
        reads[std::size_t(s)] = readDisplayAt(QString::fromLatin1(kStreamIds[s]));
        any = any || !reads[std::size_t(s)].second.empty();
    }
    if (!any)
        return false;
    // A stream that doesn't go on from where it was: the device's processor was made again (its
    // indices start again), or nobody read the stream for so long that it dropped values. The history
    // starts afresh.
    restarted = !started_;
    for (int s = 0; s < kStreams; ++s) {
        const auto& [first, values] = reads[std::size_t(s)];
        if (!values.empty() && ends_[std::size_t(s)] >= 0 && first != ends_[std::size_t(s)])
            restarted = true;
    }
    if (restarted) {
        for (int s = 0; s < kStreams; ++s) {
            std::fill(rings_[std::size_t(s)].begin(), rings_[std::size_t(s)].end(), clearValue(s));
            last_[std::size_t(s)] = clearValue(s);
        }
        ends_.fill(-1);
        begin_ = std::numeric_limits<qint64>::min();
        lastVaried_ = std::numeric_limits<qint64>::min();
        previousOpen_ = 0.f;
        started_ = true;
    }
    const qint64 recent = std::max<qint64>(1, qint64(std::ceil(kRecentSeconds * rate_)));
    for (int s = 0; s < kStreams; ++s) {
        const auto& [first, values] = reads[std::size_t(s)];
        if (values.empty())
            continue;
        const qint64 end = first + qint64(values.size());
        if (ends_[std::size_t(s)] < 0)  // (a stream starting: the history begins where all have values)
            begin_ = std::max(begin_, first);
        for (qint64 i = std::max(first, end - kCapacity); i < end; ++i)
            write(s, i, values[std::size_t(i - first)]);
        ends_[std::size_t(s)] = end;

        // The tick's level: the loudest of what came (so a short burst is not missed), of the latest
        // kRecentSeconds after a backlog.
        const qint64 from = std::max(first, end - recent);
        if (s == Open) {
            for (qint64 i = first; i < end; ++i) {
                const float open = values[std::size_t(i - first)];
                if (i >= from && previousOpen_ < 0.5f && open >= 0.5f)
                    ping_ = 0.0;  // the gate opened
                previousOpen_ = open;
            }
            passing_ = std::clamp(double(values.back()), 0.0, 1.0);
            continue;
        }
        double loudest = kFloorDb;
        for (qint64 i = from; i < end; ++i) {
            const double v = values[std::size_t(i - first)];
            if (std::isfinite(v))
                loudest = std::max(loudest, v);
        }
        (s == Input ? levelIn_ : s == Output ? levelOut_ : levelKey_) = loudest;
    }
    newest_ = std::numeric_limits<qint64>::max();
    for (const qint64 end : ends_)
        if (end >= 0)
            newest_ = std::min(newest_, end);
    newest_ = std::max(newest_, begin_);
    lastVaried_ = std::max(lastVaried_, begin_);  // (where the history starts is an edge till it scrolls out of sight)
    return true;
}

void GateGraph::refreshDisplays() {
    const double dt = tickSeconds();
    rate_ = std::max(1.0, sampleRate() / sub::app::gateDisplaySamples());

    bool restarted = false;
    const bool came = readStreams(restarted);
    const bool wasIdle = idle_;
    bool redraw = restarted;
    if (came) {
        sinceValues_ = 0.0;
        idle_ = false;
    } else {
        sinceValues_ += dt;
        if (sinceValues_ > kQuietSeconds) {  // the engine stopped, or the device is off: nothing passes
            levelIn_ = levelOut_ = levelKey_ = kFloorDb;  // (the meters fall)
            passing_ = 0.0;                               // (the LED goes out)
            idle_ = true;
        }
    }
    redraw = redraw || idle_ != wasIdle;
    // levelsChanged once a level has moved a hundredth of a dB from what it last told of (how open, a
    // thousandth, or across open and closed): a steady tone's levels jitter by far less tick to tick.
    bool levels = idle_ != emittedIdle_ || (passing_ >= 0.5) != (emitted_[3] >= 0.5) ||
                  std::abs(passing_ - emitted_[3]) > 1e-3;
    const std::array<double, 3> now = {levelIn_, levelOut_, levelKey_};
    for (std::size_t i = 0; i < now.size(); ++i)
        levels = levels || std::abs(now[i] - emitted_[i]) > 0.01;
    if (levels) {
        emitted_ = {levelIn_, levelOut_, levelKey_, passing_};
        emittedIdle_ = idle_;
    }

    // Scrolling: on by the time since the last tick, eased towards a steady lag behind the newest
    // value; never past it.
    const double before = scroll_;
    if (restarted) {
        scroll_ = std::max(double(historyBegin()), double(newest_) - kLagSeconds * rate_);
    } else if (started_) {
        scroll_ += rate_ * dt;
        const double lag = double(newest_) - scroll_;
        if (lag > kMaxLagSeconds * rate_)
            scroll_ = double(newest_) - kMaxLagSeconds * rate_;  // fell behind: jump
        else if (lag < 0.0)
            scroll_ = double(newest_);  // no more values: it stops there
        else
            scroll_ += 0.1 * (lag - kLagSeconds * rate_);
    }
    scroll_ = std::min(scroll_, double(newest_));
    if (scroll_ != before) {
        // Scrolling changes the picture only while something that differs is in sight.
        const double shown = kHistorySeconds * rate_ + 2.0 * bucketSize() + 2.0;
        redraw = redraw || double(lastVaried_) >= before - shown;
    }

    // The meters, the LED, the lines. The meters and the key dot repaint once they are a twentieth
    // of a pixel from where they were last drawn (a steady tone's peaks jitter by about 1e-4 dB), the
    // dot also as its colour changes (the key's level now crossing the threshold).
    inMeter_.update(levelIn_, dt, 24.0, 1.0, kFloorDb);
    keyMeter_.update(levelKey_, dt, kKeyFall, 0.0, kFloorDb);
    const auto moved = [this](double db, double drawn) { return std::abs(yOf(db) - drawn) > 0.05; };
    redraw = redraw || moved(inMeter_.level, drawnIn_) || moved(inMeter_.peak, drawnPeak_) ||
             moved(keyMeter_.level, drawnKey_) || keyAbove() != drawnKeyAbove_;
    led_.target = passing_;
    redraw = led_.step(easeFraction(dt, 0.03), 1e-3) || redraw;
    // How far the gate turns down what comes in, in dB on the axis (with nothing coming in, nothing).
    reduction_.target =
        levelIn_ > kFloorDb ? std::clamp(-sub::app::gateGainDb(passing_, floorDb_), 0.0, -kFloorDb) : 0.0;
    redraw = reduction_.step(easeFraction(dt, 0.03), 1e-3) || redraw;
    if (drag_ == Line::None) {
        redraw = threshold_.step(easeFraction(dt, 0.05), 1e-3) || redraw;
        redraw = return_.step(easeFraction(dt, 0.05), 1e-3) || redraw;
    }
    hoverThreshold_.target = hover_ == Line::Threshold || drag_ == Line::Threshold ? 1.0 : 0.0;
    hoverReturn_.target = hover_ == Line::Return || drag_ == Line::Return ? 1.0 : 0.0;
    redraw = hoverThreshold_.step(easeFraction(dt, 0.08), 1e-3) || redraw;
    redraw = hoverReturn_.step(easeFraction(dt, 0.08), 1e-3) || redraw;
    if (ping_ >= 0.0) {
        ping_ += dt;
        if (ping_ > kPingSeconds)
            ping_ = -1.0;
        redraw = true;
    }
    if (listening_ && !idle_) {  // (idle, nothing is heard of the key: the label holds still, dimmed)
        pulse_ = std::fmod(pulse_ + dt, kPulseSeconds);
        redraw = true;
    }
    // Not at rest yet besides: a meter or the dot above where the values put it, falling (whether or
    // not this tick took it far enough to repaint), or the In meter's peak held above its level.
    const auto above = [this](double db, double target) { return yOf(db) < yOf(target) - 0.05; };
    const bool falling = above(inMeter_.level, levelIn_) || above(inMeter_.peak, inMeter_.level) ||
                         above(keyMeter_.level, levelKey_);
    animating_ = redraw || falling;
    if (redraw) {
        drawnIn_ = yOf(inMeter_.level);
        drawnPeak_ = yOf(inMeter_.peak);
        drawnKey_ = yOf(keyMeter_.level);
        drawnKeyAbove_ = keyAbove();
        update();
    }
    if (levels)
        Q_EMIT levelsChanged();
}

// --- The mouse -----------------------------------------------------------------------------

GateGraph::Line GateGraph::lineAt(const QPointF& pos) const {
    const QRectF r = plot();
    if (pos.x() < r.left() - 2 || pos.x() > r.right() + 8 || pos.y() < r.top() - kLineGrab ||
        pos.y() > r.bottom() + kLineGrab)
        return Line::None;
    const double thresholdY = this->thresholdY(), returnY = this->returnY();
    const double toThreshold = std::abs(pos.y() - thresholdY), toReturn = std::abs(pos.y() - returnY);
    // On top of each other (Return 0, or within float rounding of it): the threshold from above,
    // Return from on and below the line, so either can still be taken.
    if (toReturn <= kLineGrab && std::abs(returnY - thresholdY) < 0.5)
        return pos.y() >= returnY ? Line::Return : Line::Threshold;
    if (toReturn <= kLineGrab && toReturn < toThreshold)
        return Line::Return;
    if (toThreshold <= kLineGrab)
        return Line::Threshold;
    return Line::None;
}

void GateGraph::hoverAt(const QPointF& pos) {
    hover_ = lineAt(pos);
    setCursor(hover_ != Line::None ? Qt::SizeVerCursor : Qt::ArrowCursor);
}

void GateGraph::mousePressEvent(QMouseEvent* event) {
    const QPointF pos = event->position();
    if (event->button() == Qt::RightButton) {
        // On a line: its parameter's menu (automation, a macro); elsewhere, the device's (the frame's).
        const Line line = lineAt(pos);
        if (line == Line::None || device() == nullptr) {
            event->ignore();
            return;
        }
        Q_EMIT paramMenuRequested(line == Line::Threshold ? QStringLiteral("threshold") : QStringLiteral("return"));
        return;
    }
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    if (secondPressOfDoubleClick(event) || device() == nullptr)
        return;  // (the double-click follows)
    // On a line: that one; anywhere else in the plot: the threshold (the figures and the meters are
    // not a control). Relative to where it was: nothing jumps.
    Line line = lineAt(pos);
    if (line == Line::None) {
        if (!plot().contains(pos)) {
            event->ignore();
            return;
        }
        line = Line::Threshold;
    }
    drag_ = line;
    hover_ = line;
    gesture_ = newGestureKey();
    dragValue_ = line == Line::Threshold ? thresholdDb_ : returnDb_;
    lastY_ = pos.y();
    touch(line == Line::Threshold ? QStringLiteral("threshold") : QStringLiteral("return"));
    threshold_.snap(threshold_.target);
    return_.snap(return_.target);
    setCursor(Qt::SizeVerCursor);
    Q_EMIT draggingChanged();
    update();
}

void GateGraph::mouseMoveEvent(QMouseEvent* event) {
    if (drag_ == Line::None)
        return;
    const double y = event->position().y();
    const double fine = event->modifiers() & Qt::ShiftModifier ? 0.1 : 1.0;
    const double db = (lastY_ - y) / plot().height() * (kCeilingDb - kFloorDb) * fine;  // up: more
    lastY_ = y;
    if (drag_ == Line::Threshold) {
        dragValue_ += db;
        setParams({{QStringLiteral("threshold"), sub::app::gateThresholdRange().clamp(dragValue_)}}, gesture_,
                  QStringLiteral("Change Gate Threshold"));
    } else {
        dragValue_ -= db;  // the return line dragged down: it closes lower
        setParams({{QStringLiteral("return"), sub::app::gateReturnRange().clamp(dragValue_)}}, gesture_,
                  QStringLiteral("Change Gate Return"));
    }
}

void GateGraph::endDrag() {
    if (drag_ == Line::None)
        return;
    drag_ = Line::None;
    gesture_.clear();
    Q_EMIT draggingChanged();
    update();
}

void GateGraph::mouseReleaseEvent(QMouseEvent* event) {
    endDrag();
    hoverAt(event->position());
}

void GateGraph::mouseUngrabEvent() {
    endDrag();
    hover_ = Line::None;
}

void GateGraph::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton)
        return;
    const Line line = lineAt(event->position());
    endDrag();
    if (line == Line::Threshold)
        setParams({{QStringLiteral("threshold"), defaultValue(QStringLiteral("threshold"))}}, QString(),
                  QStringLiteral("Change Gate Threshold"));
    else if (line == Line::Return)
        setParams({{QStringLiteral("return"), defaultValue(QStringLiteral("return"))}}, QString(),
                  QStringLiteral("Change Gate Return"));
}

void GateGraph::hoverMoveEvent(QHoverEvent* event) {
    if (drag_ == Line::None)
        hoverAt(event->position());
}

void GateGraph::hoverLeaveEvent(QHoverEvent*) {
    if (drag_ == Line::None)
        hover_ = Line::None;
}

// --- Painting ------------------------------------------------------------------------------

void GateGraph::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const double w = width(), h = height();
    p.fillRoundedRect(QRectF(0, 0, w, h), 4, 4, Theme::kMeterBg);
    p.drawRoundedRect(QRectF(0.5, 0.5, w - 1, h - 1), 4, 4, withAlpha(Theme::kGridBeat, 150), 1);
    const QRectF r = plot();
    const QFont small = uiFont(7);

    // The level axis: a line every 12 dB, its figure in the strip on the right (clear of the key
    // dot's halo on the plot's edge).
    for (int db = 0; db >= -60; db -= 12) {
        const double y = yOf(db);
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y), withAlpha(Theme::kGridBar, db == 0 ? 150 : 70));
        p.drawText(QRectF(r.right() + 6, y - 6, 17, 12), Qt::AlignRight | Qt::AlignVCenter, QString::number(db),
                   Theme::kTextDim, small);
    }

    // The history, newest at the right edge, in columns of whole buckets of values (aligned to the
    // values' absolute index, so nothing shimmers as it scrolls), gliding by fractions of a pixel.
    const int k = bucketSize();
    const double px = r.width() / std::max(1.0, kHistorySeconds * rate_);
    const qint64 begin = historyBegin();
    qint64 first = floorDiv(qint64(std::floor(scroll_ - kHistorySeconds * rate_)), k) - 1;
    first = std::max(first, floorDiv(begin + k - 1, k));  // (only buckets the history wholly has)
    const qint64 last = std::min(floorDiv(newest_, k), floorDiv(qint64(std::ceil(scroll_)), k) + 1);
    const int count = started_ ? int(std::max<qint64>(0, last - first)) : 0;
    if (count >= 2) {
        for (auto* v : {&inTops_, &outTops_, &keyTops_, &bottoms_, &shadeTops_, &opens_})
            v->resize(std::size_t(count));
        outline_.resize(std::size_t(count));
        keyPoints_.resize(std::size_t(count));
        const double dx = k * px;
        const double x0 = r.right() - (scroll_ - double(first * k)) * px;
        for (int i = 0; i < count; ++i) {
            const qint64 from = (first + i) * k;
            float in = -1e9f, out = -1e9f, key = -1e9f, open = 0.f;
            for (qint64 v = from; v < from + k; ++v) {
                const std::size_t slot = std::size_t(v & kMask);
                in = std::max(in, rings_[Input][slot]);
                out = std::max(out, rings_[Output][slot]);
                key = std::max(key, rings_[Key][slot]);
                open += rings_[Open][slot];
            }
            const std::size_t at = std::size_t(i);
            inTops_[at] = float(yOf(in));
            outTops_[at] = float(yOf(out));
            keyTops_[at] = float(yOf(key));
            bottoms_[at] = float(r.bottom());
            shadeTops_[at] = float(r.top());
            opens_[at] = open / float(k);
            const double x = x0 + (i + 0.5) * dx;
            outline_[at] = QPointF(x, outTops_[at]);
            keyPoints_[at] = QPointF(x, keyTops_[at]);
        }
        p.save();
        p.setClipRect(r);
        // The input (light), the output over it (darker, outlined): the light part is what the gate took.
        p.fillBand(x0, dx, inTops_.data(), bottoms_.data(), count, QColor(0x9a, 0x9a, 0x9a, 150));
        p.fillBand(x0, dx, outTops_.data(), bottoms_.data(), count, QColor(0x2e, 0x2e, 0x2e, 235));
        // Where the gate let sound through: a faint blue over the whole height, the levels too (under
        // them it would show only above the loudest), in eight steps, each run of one step at once.
        for (int i = 0; i < count;) {
            const int step = int(std::lround(std::clamp(double(opens_[std::size_t(i)]), 0.0, 1.0) * 8.0));
            int j = i + 1;
            while (j < count && int(std::lround(std::clamp(double(opens_[std::size_t(j)]), 0.0, 1.0) * 8.0)) == step)
                ++j;
            if (step > 0)
                p.fillBand(x0 + i * dx, dx, shadeTops_.data() + i, bottoms_.data() + i, j - i,
                           withAlpha(Theme::kSoloOn, int(std::lround(kShadeAlpha * step / 8.0))));
            i = j;
        }
        drawGlowPolyline(p, outline_, QColor(255, 255, 255, 200), 1.1);
        if (showKey_)
            p.drawPolyline(keyPoints_.data(), count, withAlpha(Theme::kPlayOn, 170), 1.0);
        p.restore();
    }

    // Between the threshold and where the gate closes again (Return): a tint, and the return line.
    const double thresholdY = this->thresholdY(), returnY = this->returnY();
    const double hoverReturn = hoverReturn_.value, hoverThreshold = hoverThreshold_.value;
    if (returnY > thresholdY + 0.5)
        p.fillRect(QRectF(r.left(), thresholdY, r.width(), returnY - thresholdY),
                   withAlpha(Theme::kAccent, int(std::lround(12 + 12 * hoverReturn))));
    drawDashedPolyline(p, {QPointF(r.left(), returnY), QPointF(r.right(), returnY)}, Theme::kAccent, 1.0 + hoverReturn);
    p.fillRect(QRectF(r.left(), returnY - 3.5, 4, 7), Theme::kAccent);

    // The threshold: a glowing blue line, its tab at the right.
    drawGlowPolyline(p, {QPointF(r.left(), thresholdY), QPointF(r.right(), thresholdY)}, Theme::kSoloOn,
                     1.5 + hoverThreshold);
    const QPointF tab[3] = {{r.right(), thresholdY - 4.5}, {r.right(), thresholdY + 4.5}, {r.right() - 6, thresholdY}};
    p.fillPolygon(tab, 3, Theme::kSoloOn);

    // The newest key level: a dot on the right edge, falling quickly rather than jumping, blue while the
    // key's level now (not the falling dot's) is at or above the threshold, its halo as open as the
    // gate is; a ring ripples out from it as the gate opens.
    const double ledEase = std::clamp(led_.value, 0.0, 1.0);
    if (keyMeter_.level > kFloorDb + 0.5) {
        const QPointF dot(r.right(), yOf(keyMeter_.level));
        p.fillEllipse(dot, 6, 6, withAlpha(Theme::kSoloOn, int(std::lround(50 * ledEase))));
        p.fillEllipse(dot, 2.5, 2.5, keyAbove() ? Theme::kSoloOn : Theme::kTextDim);
    }
    if (ping_ >= 0.0) {  // (inside the plot: it would run over the figures)
        const double t = std::clamp(ping_ / kPingSeconds, 0.0, 1.0), eased = 1.0 - (1.0 - t) * (1.0 - t);
        const double radius = 3.0 + 11.0 * eased;
        const QPointF at(r.right(), yOf(std::max(keyMeter_.level, thresholdDb_)));
        p.save();
        p.setClipRect(r);
        p.drawEllipse(QRectF(at.x() - radius, at.y() - radius, 2 * radius, 2 * radius),
                      withAlpha(Theme::kSoloOn, int(std::lround(180 * (1.0 - eased)))), 1.2);
        p.restore();
    }

    // The state, top left: an LED (glowing as it opens) and its word, on a dark backing so the history
    // under it doesn't get in the way. While no values come (the device off, the engine stopped): "Idle".
    const bool open = !idle_ && led_.value >= 0.5;
    const QString state = idle_ ? QStringLiteral("Idle") : open ? QStringLiteral("Open") : QStringLiteral("Closed");
    const QPointF led = r.topLeft() + QPointF(9, 9);
    p.fillRoundedRect(QRectF(r.left() + 2, r.top() + 2, 20 + SgPainter::textWidth(state, small), 14), 3, 3,
                      withAlpha(Theme::kMeterBg, 170));
    if (ledEase > 0.01)
        p.fillEllipse(led, 3.5 + 4 * ledEase, 3.5 + 4 * ledEase,
                      withAlpha(Theme::kSoloOn, int(std::lround(70 * ledEase))));
    p.fillEllipse(led, 3.5, 3.5, mixColor(Theme::kTextDisabled, Theme::kSoloOn, ledEase));
    p.drawText(QRectF(led.x() + 7, led.y() - 7, 50, 14), Qt::AlignLeft | Qt::AlignVCenter, state,
               idle_ ? Theme::kTextDisabled : open ? Theme::kText : Theme::kTextDim, small);
    if (listening_) {  // (pulsing while sound comes, on a backing of its own: the 0 dB line runs under it)
        const QString listening = QStringLiteral("Listening to the key");
        const double alpha = 0.55 + 0.45 * std::sin(2 * kPi * pulse_ / kPulseSeconds);
        const double width = SgPainter::textWidth(listening, small) + 12;
        const QRectF box(r.center().x() - width / 2, r.top() + 2, width, 14);
        p.fillRoundedRect(box, 3, 3, withAlpha(Theme::kMeterBg, 200));
        p.drawText(box, Qt::AlignCenter, listening,
                   idle_ ? Theme::kTextDisabled : withAlpha(Theme::kAccent, int(std::lround(255 * alpha))), small);
    }

    // The lines' values while hovered or dragged, over everything else.
    auto tag = [&](double y, double opacity, bool right, const QString& text, const QColor& color) {
        if (opacity <= 0.01)
            return;
        const double width = SgPainter::textWidth(text, small) + 8;
        const double top = y - 15 >= r.top() ? y - 15 : y + 3;
        const QRectF box(right ? r.right() - 10 - width : r.left() + 8, top, width, 12);
        p.save();
        p.setOpacity(opacity);
        p.fillRoundedRect(box, 2, 2, withAlpha(Theme::kMeterBg, 220));
        p.drawText(box, Qt::AlignCenter, text, color, small);
        p.restore();
    };
    tag(thresholdY, hoverThreshold, true, sub::app::formatValue(thresholdDb_, QStringLiteral("dB")), Theme::kSoloOn);
    tag(returnY, hoverReturn, false,
        QStringLiteral("Return %1").arg(sub::app::formatValue(returnDb_, QStringLiteral("dB"))), Theme::kAccent);

    // The meters, captioned above: the input's level (red above 0 dB, over the well's top), and how
    // far the gate turns it down.
    const QRectF inRect = inMeter(), gateRect = gateMeter();
    for (const auto& [rect, caption] :
         {std::pair{inRect, QStringLiteral("In")}, std::pair{gateRect, QStringLiteral("Gate")}}) {
        const double width = SgPainter::textWidth(caption, small) + 2;
        const double left = std::min(rect.center().x() - width / 2, w - 2 - width);
        p.drawText(QRectF(left, std::max(1.0, rect.top() - 13), width, 12), Qt::AlignCenter, caption,
                   Theme::kTextDim, small);
    }
    drawLevelMeter(p, inRect, inMeter_.level, inMeter_.peak, kFloorDb, 0.0, MeterWell::Panel);
    if (inMeter_.level > 0.0) {
        const double top = yOf(std::min(inMeter_.level, kCeilingDb));
        p.fillRect(QRectF(inRect.left(), top, inRect.width(), inRect.top() - top), Theme::kMeterHigh);
    }
    drawGateMeter(p, gateRect, reduction_.value);
}

}  // namespace sub::ui
