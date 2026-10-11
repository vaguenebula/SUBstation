#include "devices/ReverbFilterPad.h"

#include "audio/EngineBridge.h"
#include "audio/ReverbResponse.h"
#include "devices/EditorPaint.h"
#include "input/GestureKey.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>

namespace sub::ui {

namespace {

constexpr double kSwitchSeconds = 0.04;  // a switch's change eases over about 120 ms (three of these)
constexpr double kGlowSeconds = 0.25;    // the glow fades
constexpr double kGlowRangeDb = 48.0;    // the input's level from -48 dB (none) to 0 (all)
// A long audio block: a tick reads the input's newest values back this far (not a backlog's), and one without
// values keeps the last this long.
constexpr double kStaleSeconds = 0.1;
constexpr double kCaption = 13.0;       // px: the caption's strip at the top, clear of the dot

}  // namespace

ReverbFilterPad::ReverbFilterPad(QQuickItem* parent) : DeviceCanvas(parent) {
    setImplicitSize(kMinimumWidth, kMinimumHeight);
    setAcceptedMouseButtons(Qt::LeftButton);
    setCursor(Qt::SizeAllCursor);
    // The curve is the engine's at its sample rate: worked out again when the audio device changes.
    connect(this, &DeviceCanvas::deviceChanged, this, [this] {
        disconnect(bridgeConnection_);
        if (session())
            bridgeConnection_ = connect(session()->bridge(), &sub::app::EngineBridge::deviceChanged, this, [this] {
                updateCurve();
                switch_.snap(1.0);
                updateShown();
                update();
            });
    });
}

QRectF ReverbFilterPad::plot() const { return QRectF(0, 0, width(), height()).adjusted(1, 1, -1, -1); }

QRectF ReverbFilterPad::inner() const { return plot().adjusted(0, kCaption + 6, 0, -6); }

double ReverbFilterPad::xOf(double freq) const {
    const QRectF r = plot();
    return LogAxis{kLow, kHigh, r.left(), r.width()}.position(freq);
}

double ReverbFilterPad::freqAt(double x) const {
    const QRectF r = plot();
    return LogAxis{kLow, kHigh, r.left(), r.width()}.valueAt(x);
}

double ReverbFilterPad::yOfWidth(double octaves) const {
    const QRectF r = inner();
    return r.bottom() - (std::clamp(octaves, kWidthMin, kWidthMax) - kWidthMin) / (kWidthMax - kWidthMin) * r.height();
}

double ReverbFilterPad::widthAt(double y) const {
    const QRectF r = inner();
    const double fraction = std::clamp((r.bottom() - y) / r.height(), 0.0, 1.0);
    return kWidthMin + fraction * (kWidthMax - kWidthMin);
}

double ReverbFilterPad::yOfDb(double db) const {
    const QRectF r = plot();
    const double fraction = (kTopDb - std::clamp(db, kFloorDb, kTopDb)) / (kTopDb - kFloorDb);
    return r.top() + fraction * r.height();
}

QPointF ReverbFilterPad::dot() const { return QPointF(xOf(freq_), yOfWidth(width_)); }

void ReverbFilterPad::sync() {
    const bool loCut = value(QStringLiteral("lo_cut")) >= 0.5, hiCut = value(QStringLiteral("hi_cut")) >= 0.5;
    const bool switched = synced_ && (loCut != loCut_ || hiCut != hiCut_);
    loCut_ = loCut;
    hiCut_ = hiCut;
    freq_ = value(QStringLiteral("in_freq"));
    width_ = value(QStringLiteral("in_width"));
    if (switched && shown_.size() == db_.size()) {
        // A switch eases the curve from where it is drawn now to its new shape.
        from_ = shown_;
        switch_.value = 0.0;
        switch_.target = 1.0;
    }
    updateCurve();
    if (!synced_)
        switch_.snap(1.0);
    synced_ = true;
    updateShown();
    update();
}

void ReverbFilterPad::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    DeviceCanvas::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        updateCurve();
        switch_.snap(1.0);  // (a different number of columns: nothing to ease from)
        updateShown();
        updateColumns();
    }
}

void ReverbFilterPad::updateCurve() {
    const int columns = std::max(2, int(plot().width()));
    QList<double> frequencies;
    frequencies.reserve(columns + 1);
    for (int i = 0; i <= columns; ++i) frequencies.append(kLow * std::pow(kHigh / kLow, double(i) / columns));
    const QList<double> db =
        sub::app::reverbInputFilterDb(freq_, width_, loCut_, hiCut_, sampleRate(), frequencies);
    frequencies_.assign(frequencies.begin(), frequencies.end());
    db_.assign(db.begin(), db.end());
}

void ReverbFilterPad::updateShown() {
    if (from_.size() != db_.size() || switch_.value >= 1.0) {
        shown_ = db_;
        return;
    }
    shown_.resize(db_.size());
    const double t = switch_.value;
    for (std::size_t i = 0; i < db_.size(); ++i) {
        // Eased in the drawn domain (clamped to the axis), so a cut's -300 dB never races past the bottom.
        const double from = std::clamp(from_[i], kFloorDb - 1.0, kTopDb);
        const double to = std::clamp(db_[i], kFloorDb - 1.0, kTopDb);
        shown_[i] = from + (to - from) * t;
    }
}

void ReverbFilterPad::updateColumns() { columns_ = spectrum_.columns(std::max(2, int(plot().width())), kLow, kHigh); }

// --- Displays and animation -------------------------------------------------------------

void ReverbFilterPad::refreshDisplays() {
    const double seconds = tickSeconds();
    const std::vector<float> signal = readDisplay(QStringLiteral("signal"));
    if (spectrum_.add(signal.data(), signal.size(), sampleRate())) {
        updateColumns();
        spectrumChanged_ = true;
    }
    // The newest value and the loudest of the tick's newest (shown again after the sound stopped, a read holds
    // the loud past: DeviceCanvas::readRecent). A tick that brings none (the audio's blocks longer than a tick)
    // keeps the last for a moment: only a while without any is silence.
    const std::vector<float> input = readRecent(QStringLiteral("input"), kStaleSeconds);
    double newest = inputLevel_;
    if (!input.empty()) {
        newest = double(input.back());
        loudest_ = sub::app::kReverbMeterFloorDb;
        for (const float v : input) loudest_ = std::max(loudest_, double(v));
        stale_ = 0.0;
    } else if ((stale_ += seconds) > kStaleSeconds) {
        newest = loudest_ = sub::app::kReverbMeterFloorDb;
    }
    if (newest != inputLevel_) {
        inputLevel_ = newest;
        Q_EMIT levelsChanged();
    }
    glow_.target = std::clamp((loudest_ + kGlowRangeDb) / kGlowRangeDb, 0.0, 1.0);
    advance(seconds);
}

void ReverbFilterPad::advance(double seconds) {
    bool moving = spectrumChanged_;
    spectrumChanged_ = false;
    // The glow rises at once and fades.
    if (glow_.target > glow_.value) {
        glow_.value = glow_.target;
        moving = true;
    } else {
        moving = glow_.step(easeFraction(seconds, kGlowSeconds), 1e-3) || moving;
    }
    if (switch_.step(easeFraction(seconds, kSwitchSeconds), 1e-3)) {
        updateShown();
        moving = true;
    }
    if (moving != animating_ || moving) {
        animating_ = moving;
        Q_EMIT levelsChanged();
    }
    if (moving)
        update();
}

// --- Dragging -------------------------------------------------------------------------

void ReverbFilterPad::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    gesture_ = newGestureKey();
    fine_ = event->modifiers().testFlag(Qt::ShiftModifier);
    anchor_ = anchorDot_ = event->position();  // the dot comes to the mouse
    touch(QStringLiteral("in_freq"));
    dragTo(event->position(), event->modifiers());
}

void ReverbFilterPad::mouseMoveEvent(QMouseEvent* event) {
    if (!gesture_.isEmpty())
        dragTo(event->position(), event->modifiers());
}

void ReverbFilterPad::mouseReleaseEvent(QMouseEvent*) { gesture_.clear(); }

void ReverbFilterPad::mouseUngrabEvent() { gesture_.clear(); }

void ReverbFilterPad::dragTo(const QPointF& pos, Qt::KeyboardModifiers modifiers) {
    const bool fine = modifiers.testFlag(Qt::ShiftModifier);
    if (fine != fine_) {  // Shift went down or up: measure from here
        fine_ = fine;
        anchor_ = pos;
        anchorDot_ = dot();
    }
    const QPointF at = anchorDot_ + (pos - anchor_) * (fine_ ? 0.25 : 1.0);
    const double octaves = std::clamp(std::nearbyint(widthAt(at.y()) * 100.0) / 100.0, kWidthMin, kWidthMax);
    setParams({{QStringLiteral("in_freq"), std::clamp(freqAt(at.x()), kFreqMin, kFreqMax)},
               {QStringLiteral("in_width"), octaves}},
              gesture_, QStringLiteral("Change Reverb Input Filter"));
}

// --- Painting ---------------------------------------------------------------------------

void ReverbFilterPad::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const QRectF r = plot();
    p.fillRect(QRectF(0, 0, width(), height()), Theme::meterBg());
    drawDecadeGrid(p, r, LogAxis{kLow, kHigh, r.left(), r.width()});

    // What goes into the reverb, behind the curve.
    const double floor = sub::app::analysis::FallingSpectrum::kFloorDb;
    const int columns = int(columns_.size());
    if (columns >= 2 && *std::max_element(columns_.begin(), columns_.end()) > floor) {
        std::vector<QPointF> shape;
        shape.reserve(std::size_t(columns));
        for (int i = 0; i < columns; ++i) {
            const double h = std::clamp((columns_[std::size_t(i)] - floor) / -floor, 0.0, 1.0) * r.height();
            shape.emplace_back(r.left() + (i + 0.5) * r.width() / columns, r.bottom() - h);
        }
        p.fillToBaseline(shape.data(), int(shape.size()), r.bottom(), withAlpha(Theme::textDim(), 46));
        p.drawPolyline(shape.data(), int(shape.size()), withAlpha(Theme::textDim(), 90), 1.0);
    }

    // The band: filled while it glows with the input, its line glowing.
    const bool on = loCut_ || hiCut_;
    const double glow = glow_.value;
    std::vector<QPointF> curve;
    curve.reserve(shown_.size());
    for (std::size_t i = 0; i < shown_.size() && i < frequencies_.size(); ++i)
        curve.emplace_back(xOf(frequencies_[i]), yOfDb(shown_[i]));
    if (curve.size() >= 2) {
        p.save();
        p.setClipRect(r);
        if (on) {
            p.fillToBaseline(curve.data(), int(curve.size()), r.bottom(),
                             withAlpha(Theme::scopeLine(), int(18 + 30 * glow)));
            drawGlowPolyline(p, curve, Theme::scopeLine(), 1.5);
        } else {
            p.drawPolyline(curve.data(), int(curve.size()), Theme::textDisabled(), 1.5);
        }
        p.restore();
    }

    p.drawText(QRectF(r.left() + 3, r.top() + 1, r.width() - 6, 12), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("Input"), Theme::textDim(), uiFont(7));

    // The dot: faint crosshairs through it, a halo breathing with the input, the ring (in the pad).
    p.save();
    p.setClipRect(QRectF(0, 0, width(), height()));
    const QPointF at = dot();
    p.drawLine(QPointF(r.left(), at.y()), QPointF(r.right(), at.y()), withAlpha(Theme::text(), 30));
    p.drawLine(QPointF(at.x(), r.top()), QPointF(at.x(), r.bottom()), withAlpha(Theme::text(), 30));
    const QColor ring = on ? Theme::accent() : Theme::textDim();
    if (on) {
        const double halo = 6.0 + 6.0 * glow;
        p.fillEllipse(at, halo, halo, withAlpha(Theme::accent(), int(40 + 120 * glow)));
    }
    p.fillEllipse(at, 4.0, 4.0, Theme::meterBg());
    p.drawEllipse(QRectF(at.x() - 5, at.y() - 5, 10, 10), ring, 2);
    p.restore();
}

}  // namespace sub::ui
