#include "devices/ErosionGraph.h"

#include "audio/EngineBridge.h"
#include "audio/ErosionResponse.h"
#include "input/GestureKey.h"
#include "model/ParamSpec.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QHoverEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace sub::ui {

using sub::app::analysis::EqAnalyzer;

namespace {

constexpr double kActivityFloorDb = -60.0;  // the `erosion` display: activity 0 here,
constexpr double kActivitySpanDb = 48.0;    // and 1 this far above
constexpr double kActivityRise = 0.35;      // of the way per refresh: up in a few,
constexpr double kActivityFall = 0.08;      // down over half a second
constexpr double kShimmerDepth = 0.4;       // at full activity the band's fill dips up to this share of its height
constexpr int kKnotColumns = 8;             // the shimmer's knots: one every this many columns (16 px)
constexpr float kKnotGlide = 0.12f;         // per refresh, of the way to a knot's target,
constexpr float kRetarget = 0.08f;          // and the chance a knot takes a fresh one (about every 0.2 s)
constexpr double kSpikePerTick = 0.3;       // the sine's spike: its wave's phase, radians a refresh at full activity,
constexpr double kSpikeSway = 2.0;          // how far it sways (px) then,
constexpr double kSpikeWavelength = 24.0;   // and its wavelength (px)

double rounded(double value, double step) { return std::round(value / step) * step; }

bool audible(const std::vector<float>& samples) {
    return std::any_of(samples.begin(), samples.end(), [](float s) { return s != 0.0f; });
}

}  // namespace

ErosionGraph::ErosionGraph(QQuickItem* parent) : DeviceCanvas(parent) {
    setImplicitSize(kWidth, kMinimumHeight);
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
    setCursor(Qt::SizeAllCursor);
    // The band is the engine's at its sample rate: worked out again when the audio device changes.
    connect(this, &DeviceCanvas::deviceChanged, this, [this] {
        disconnect(bridgeConnection_);
        if (session())
            bridgeConnection_ = connect(session()->bridge(), &sub::app::EngineBridge::deviceChanged, this, [this] {
                updateCurve();
                update();
            });
    });
}

QRectF ErosionGraph::plot() const { return QRectF(0, 0, width(), height()).adjusted(1, kTopStrip, -1, -1); }

LogAxis ErosionGraph::frequencyAxis() const {
    const QRectF r = plot();
    return {kLow, kHigh, r.left(), r.width()};
}

double ErosionGraph::xOf(double freq) const { return frequencyAxis().position(freq); }

double ErosionGraph::freqAt(double x) const { return frequencyAxis().valueAt(x); }

double ErosionGraph::yOfAmount(double amount) const {
    const QRectF r = plot();
    return r.bottom() - std::clamp(amount, 0.0, 100.0) / 100.0 * r.height();
}

double ErosionGraph::amountAt(double y) const {
    const QRectF r = plot();
    return r.height() > 0.0 ? std::clamp((r.bottom() - y) / r.height() * 100.0, 0.0, 100.0) : 0.0;
}

QPointF ErosionGraph::dot() const { return QPointF(xOf(std::clamp(tuned_, kLow, kHigh)), yOfAmount(amount_)); }

void ErosionGraph::sync() {
    freq_ = value(QStringLiteral("freq"));
    width_ = value(QStringLiteral("width"));
    amount_ = value(QStringLiteral("amount"));
    blend_ = value(QStringLiteral("blend"));
    stereo_ = value(QStringLiteral("stereo"));
    const auto [sine, noise] = sub::app::erosionBlendWeights(blend_);
    sineWeight_ = sine;
    noiseWeight_ = noise;
    updateCurve();
    update();
}

void ErosionGraph::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    DeviceCanvas::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        updateCurve();
}

void ErosionGraph::updateCurve() {
    const double rate = sampleRate();
    const QRectF r = plot();
    tuned_ = sub::app::erosionModFrequency(freq_, rate);
    edges_ = sub::app::erosionBandEdges(freq_, width_, rate);
    excursionMs_ = sub::app::erosionExcursionMs(amount_, rate);

    // The outline: a point per pixel, and where it is tuned (a band narrower than a pixel peaks there).
    const int pixels = std::max(2, int(r.width()));
    QList<double> outline;
    outline.reserve(pixels + 2);
    for (int i = 0; i <= pixels; ++i)
        outline.append(kLow * std::pow(kHigh / kLow, double(i) / pixels));
    if (kLow < tuned_ && tuned_ < kHigh) {
        const auto at = std::lower_bound(outline.begin(), outline.end(), tuned_);
        if (at == outline.end() || *at != tuned_)
            outline.insert(at, tuned_);
    }
    const QList<double> magnitudes = sub::app::erosionBandMagnitude(freq_, width_, rate, outline);
    frequencies_.assign(outline.begin(), outline.end());
    magnitudes_.assign(magnitudes.begin(), magnitudes.end());

    // The columns: each one's band is the most of its edges and its centre (a narrow band between them
    // still shows), all of it in the one the tuned frequency falls in.
    const int columns = std::max(1, int(std::ceil(r.width() / kColumn)));
    QList<double> probes;
    probes.reserve(3 * columns);
    for (int c = 0; c < columns; ++c) {
        const double left = r.left() + c * kColumn;
        probes << freqAt(left) << freqAt(left + 0.5 * kColumn) << freqAt(left + kColumn);
    }
    const QList<double> probed = sub::app::erosionBandMagnitude(freq_, width_, rate, probes);
    columnFrequencies_.resize(size_t(columns));
    columnMagnitudes_.resize(size_t(columns));
    for (int c = 0; c < columns; ++c) {
        const qsizetype i = 3 * c;
        columnFrequencies_[size_t(c)] = probes[i + 1];
        const bool tuned = probes[i] <= tuned_ && tuned_ < probes[i + 2];
        columnMagnitudes_[size_t(c)] = tuned ? 1.0 : std::max({probed[i], probed[i + 1], probed[i + 2]});
    }
    knotsL_.resize(size_t(columns / kKnotColumns + 2));
    knotsR_.resize(knotsL_.size());
    shimmerL_.resize(size_t(columns), 0.5f);
    shimmerR_.resize(size_t(columns), 0.5f);
    if (!inColumns_.empty() || !outColumns_.empty()) {  // (the spectra at the new columns)
        inColumns_ = analyzer_.live(EqAnalyzer::Input) ? analyzer_.columns(EqAnalyzer::Input, columnFrequencies_)
                                                       : std::vector<double>();
        outColumns_ = analyzer_.live(EqAnalyzer::Output) ? analyzer_.columns(EqAnalyzer::Output, columnFrequencies_)
                                                         : std::vector<double>();
    }
}

// --- Displays and animation -------------------------------------------------------------------

void ErosionGraph::refreshDisplays() {
    const double rate = sampleRate();
    const std::vector<float> input = readDisplay(QStringLiteral("input"));
    const std::vector<float> output = readDisplay(QStringLiteral("output"));
    const std::vector<float> erosion = readDisplay(QStringLiteral("erosion"));

    // The spectra, while sound comes or they still show some (falling back): silence costs no FFT.
    const bool wasLive = !inColumns_.empty() || !outColumns_.empty();
    if (wasLive || audible(input) || audible(output)) {
        analyzer_.feed(EqAnalyzer::Input, input.data(), input.size(), rate);
        analyzer_.feed(EqAnalyzer::Output, output.data(), output.size(), rate);
        inColumns_ = analyzer_.live(EqAnalyzer::Input) ? analyzer_.columns(EqAnalyzer::Input, columnFrequencies_)
                                                       : std::vector<double>();
        outColumns_ = analyzer_.live(EqAnalyzer::Output) ? analyzer_.columns(EqAnalyzer::Output, columnFrequencies_)
                                                         : std::vector<double>();
    }
    const bool live = !inColumns_.empty() || !outColumns_.empty();

    // How much is being eroded: rising quickly, falling back over half a second.
    erosionDb_ = -90.0;
    for (const float db : erosion) {
        if (std::isfinite(db))
            erosionDb_ = std::max(erosionDb_, double(db));
    }
    activity_.target = std::clamp((erosionDb_ - kActivityFloorDb) / kActivitySpanDb, 0.0, 1.0);
    const bool moved = activity_.step(activity_.target > activity_.value ? kActivityRise : kActivityFall);
    if (activity_.value > 0.0) {
        shimmerStep();
        sinePhase_ = std::fmod(sinePhase_ + kSpikePerTick * activity_.value, 2.0 * std::numbers::pi);
    }

    // Still in silence: nothing to repaint.
    animating_ = live || wasLive || moved || activity_.value > 0.005;
    if (animating_)
        update();
}

void ErosionGraph::shimmerStep() {
    // Heat haze: knots every kKnotColumns glide towards random heights, now and then taking fresh ones,
    // and the columns between follow them smoothly. (Fresh heights for every column every tick read as
    // TV static.) The right layer's knots take targets that part from the left's as Stereo widens: one
    // edge in mono, two shimmering on their own at 100 %.
    std::uniform_real_distribution<float> uniform(0.0f, 1.0f);
    const float s = float(std::clamp(stereo_ / 100.0, 0.0, 1.0));
    for (size_t k = 0; k < knotsL_.size(); ++k) {
        if (uniform(random_) < kRetarget) {
            const float t = uniform(random_);
            knotsL_[k].target = t;
            knotsR_[k].target = (1.0f - s) * t + s * uniform(random_);
        }
        knotsL_[k].value += kKnotGlide * (knotsL_[k].target - knotsL_[k].value);
        knotsR_[k].value += kKnotGlide * (knotsR_[k].target - knotsR_[k].value);
    }
    for (size_t c = 0; c < shimmerL_.size(); ++c) {
        const size_t k = c / kKnotColumns;
        const float f = float(c % kKnotColumns) / kKnotColumns;
        const float eased = f * f * (3.0f - 2.0f * f);
        shimmerL_[c] = knotsL_[k].value + (knotsL_[k + 1].value - knotsL_[k].value) * eased;
        shimmerR_[c] = knotsR_[k].value + (knotsR_[k + 1].value - knotsR_[k].value) * eased;
    }
}

// --- Dragging ---------------------------------------------------------------------------------

void ErosionGraph::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();  // (the frame's menu)
        return;
    }
    const Qt::KeyboardModifiers modifiers = event->modifiers();
    gesture_ = newGestureKey();
    last_ = event->position();
    pressedFreq_ = freq_;
    pressedAmount_ = amount_;
    if (modifiers & Qt::AltModifier) {
        // Up and down for the Filter Width, from where the dot is: nothing changes until the mouse moves.
        drag_ = Drag::Width;
        fromDot_ = true;
        touch(QStringLiteral("width"));
        virtual_ = pressedAt_ = dot();
        pressedWidth_ = width_;
        pressedY_ = virtual_.y();
    } else if (modifiers & Qt::ShiftModifier) {
        // Finely, from where the dot is.
        drag_ = Drag::Point;
        fromDot_ = true;
        touch(QStringLiteral("freq"));
        virtual_ = pressedAt_ = dot();
    } else {
        // The dot jumps to the press, as in Live's X-Y field.
        drag_ = Drag::Point;
        fromDot_ = false;
        touch(QStringLiteral("freq"));
        virtual_ = pressedAt_ = event->position();
        apply();
    }
    Q_EMIT draggingChanged();
    update();  // (the band's edges brighten in a Width drag)
}

void ErosionGraph::mouseMoveEvent(QMouseEvent* event) {
    if (drag_ == Drag::None)
        return;
    const QPointF pos = event->position();
    virtual_ += (pos - last_) * ((event->modifiers() & Qt::ShiftModifier) ? kFine : 1.0);
    last_ = pos;
    apply();
}

void ErosionGraph::mouseReleaseEvent(QMouseEvent*) { endDrag(); }

void ErosionGraph::mouseUngrabEvent() { endDrag(); }

void ErosionGraph::endDrag() {
    if (drag_ == Drag::None)
        return;
    drag_ = Drag::None;
    gesture_.clear();
    Q_EMIT draggingChanged();
    update();
}

void ErosionGraph::apply() {
    // A drag from the dot that hasn't moved it across (or up and down) leaves that value exactly as it was.
    const QRectF r = plot();
    const double freq = fromDot_ && virtual_.x() == pressedAt_.x()
                            ? pressedFreq_
                            : std::clamp(rounded(freqAt(std::clamp(virtual_.x(), r.left(), r.right())), 0.01),
                                         sub::app::kErosionMinFrequency, sub::app::kErosionMaxFrequency);
    if (drag_ == Drag::Width) {
        const double width = pressedWidth_ * std::exp2((pressedY_ - virtual_.y()) / kWidthPixels);
        setParams({{QStringLiteral("width"),
                    rounded(std::clamp(width, sub::app::kErosionMinWidth, sub::app::kErosionMaxWidth), 0.001)},
                   {QStringLiteral("freq"), freq}},
                  gesture_, tr("Change Erosion Width"));
    } else if (drag_ == Drag::Point) {
        const double amount =
            fromDot_ && virtual_.y() == pressedAt_.y() ? pressedAmount_ : rounded(amountAt(virtual_.y()), 0.01);
        setParams({{QStringLiteral("freq"), freq}, {QStringLiteral("amount"), amount}}, gesture_,
                  tr("Change Erosion Frequency and Amount"));
    }
}

void ErosionGraph::wheelEvent(QWheelEvent* event) {
    const QPoint angle = event->angleDelta();
    const int delta = angle.y() != 0 ? angle.y() : angle.x();  // (Alt turns it sideways on some systems)
    if (delta == 0) {
        event->ignore();
        return;
    }
    // Notches in a quick burst are one undo step.
    if (wheelGesture_.isEmpty() || !wheelClock_.isValid() || wheelClock_.elapsed() > kWheelGestureMs)
        wheelGesture_ = newGestureKey();
    wheelClock_.start();
    const double octaves = delta / 120.0 * ((event->modifiers() & Qt::ShiftModifier) ? 1.0 / 16.0 : kWheelOctaves);
    const double width = rounded(
        std::clamp(width_ * std::exp2(octaves), sub::app::kErosionMinWidth, sub::app::kErosionMaxWidth), 0.001);
    touch(QStringLiteral("width"));
    if (drag_ == Drag::Width) {
        // In an Alt drag the drag goes on from the new width (and it is part of the drag's step).
        pressedWidth_ = width;
        pressedY_ = virtual_.y();
        apply();
    } else {
        setParams({{QStringLiteral("width"), width}}, wheelGesture_, tr("Change Erosion Width"));
    }
    event->accept();
}

void ErosionGraph::hoverMoveEvent(QHoverEvent* event) {
    setAltHeld(event->modifiers().testFlag(Qt::AltModifier));
    DeviceCanvas::hoverMoveEvent(event);
}

void ErosionGraph::hoverLeaveEvent(QHoverEvent* event) {
    setAltHeld(false);
    DeviceCanvas::hoverLeaveEvent(event);
}

void ErosionGraph::setAltHeld(bool held) {
    if (altHeld_ == held)
        return;
    altHeld_ = held;
    update();
}

// --- Painting ---------------------------------------------------------------------------------

void ErosionGraph::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const QRectF r = plot();
    const QFont font = uiFont(7);
    p.fillRect(QRectF(0, 0, width(), height()), Theme::kMeterBg);

    // The grid: a line per decade across, every quarter of the Amount up.
    drawDecadeGrid(p, r, frequencyAxis());
    for (const double amount : {25.0, 50.0, 75.0}) {
        const double y = yOfAmount(amount);
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y), withAlpha(Theme::kGridBeat, 110));
    }
    const std::pair<double, const char*> figures[] = {{100.0, "100"}, {1000.0, "1k"}, {10000.0, "10k"}};
    for (const auto& [freq, text] : figures)
        p.drawText(QRectF(xOf(freq) + 3, r.bottom() - 12, 30, 11), Qt::AlignLeft | Qt::AlignVCenter,
                   QString::fromLatin1(text), Theme::kTextDim, font);

    p.save();
    p.setClipRect(r);
    const size_t columns = columnFrequencies_.size();
    std::vector<double> xs(columns);
    for (size_t c = 0; c < columns; ++c)
        xs[c] = r.left() + (double(c) + 0.5) * kColumn;
    auto spectrumY = [&](double db) {
        const double fraction = (db - EqAnalyzer::kFloorDb) / (EqAnalyzer::kCeilDb - EqAnalyzer::kFloorDb);
        return r.bottom() - std::clamp(fraction, 0.0, 1.0) * r.height();
    };
    auto spectrumLine = [&](const std::vector<double>& db) {
        std::vector<QPointF> line;
        line.reserve(columns + 2);
        line.emplace_back(r.left(), spectrumY(db.front()));
        for (size_t c = 0; c < columns; ++c)
            line.emplace_back(xs[c], spectrumY(db[c]));
        line.emplace_back(r.right(), spectrumY(db.back()));
        return line;
    };

    // The input's spectrum, filled.
    if (columns > 0 && inColumns_.size() == columns) {
        const std::vector<QPointF> line = spectrumLine(inColumns_);
        QLinearGradient fill(r.topLeft(), r.bottomLeft());
        fill.setColorAt(0.0, withAlpha(Theme::kText, 64));
        fill.setColorAt(1.0, withAlpha(Theme::kText, 12));
        p.fillToBaseline(line.data(), int(line.size()), r.bottom(), fill);
        p.drawPolyline(line.data(), int(line.size()), withAlpha(Theme::kTextDim, 120), 1.0);
    }

    // The noise's band, filled and shimmering while it erodes: in Stereo a second layer parts from the first.
    const QPointF at = dot();
    const double h = r.bottom() - at.y();
    const double activity = activity_.value;
    if (noiseWeight_ > 0.01 && h >= 1.0 && columns > 0) {
        std::vector<float> tops(columns), bottoms(columns, float(r.bottom()));
        auto layer = [&](const std::vector<float>& shimmer, const QColor& color, double alpha) {
            for (size_t c = 0; c < columns; ++c)
                tops[c] = float(r.bottom() - columnMagnitudes_[c] * h * (1.0 - kShimmerDepth * activity * shimmer[c]));
            p.fillBand(r.left(), kColumn, tops.data(), bottoms.data(), int(columns),
                       withAlpha(color, int(std::lround(alpha))));
        };
        const double alpha = (24.0 + 44.0 * activity) * noiseWeight_;
        layer(shimmerL_, Theme::kAccent, alpha);
        if (stereo_ > 0.5)
            layer(shimmerR_, Theme::kScopeLine, alpha * stereo_ / 100.0);
    }

    // The output's spectrum, a line over it all: the fizz Erosion adds shows at the top.
    if (columns > 0 && outColumns_.size() == columns) {
        const std::vector<QPointF> line = spectrumLine(outColumns_);
        p.drawPolyline(line.data(), int(line.size()), withAlpha(Theme::kText, 150), 1.0);
    }

    // The band's outline, the engine's filter, with its -3 dB edges.
    if (noiseWeight_ > 0.01 && frequencies_.size() >= 2) {
        std::vector<QPointF> outline(frequencies_.size());
        for (size_t i = 0; i < frequencies_.size(); ++i)
            outline[i] = QPointF(xOf(frequencies_[i]), r.bottom() - magnitudes_[i] * h);
        drawGlowPolyline(p, outline, withAlpha(Theme::kAccent, int(std::lround(255 * noiseWeight_))), 1.5);
    }
    if (noiseWeight_ > 0.05 && h >= 1.0) {
        const QColor tick = altHeld_ || drag_ == Drag::Width ? Theme::kText : Theme::kTextDim;
        const double y = r.bottom() - 0.70710678 * h;
        for (const double edge : {edges_.first, edges_.second}) {
            if (kLow <= edge && edge <= kHigh) {
                const double x = xOf(edge);
                p.drawLine(QPointF(x, y - 4), QPointF(x, y + 4), withAlpha(tick, int(std::lround(255 * noiseWeight_))),
                           1.0, Qt::FlatCap);
            }
        }
    }

    // The sine: a spike from the floor to the dot, glowing as it erodes, and while it does trembling like a
    // plucked string (a wave running up it, held still at both ends).
    if (sineWeight_ > 0.01 && h >= 1.0) {
        std::vector<QPointF> spike;
        const double sway = kSpikeSway * activity;
        if (sway > 0.05) {
            const int steps = std::max(2, int(std::ceil(h / 2.0)));  // a point every 2 px or so
            spike.reserve(size_t(steps) + 1);
            for (int i = 0; i <= steps; ++i) {
                const double t = double(i) / steps;
                const double wave = std::sin(2.0 * std::numbers::pi * t * h / kSpikeWavelength - sinePhase_);
                spike.emplace_back(at.x() + sway * std::sin(std::numbers::pi * t) * wave, r.bottom() - t * h);
            }
        } else {
            spike = {QPointF(at.x(), r.bottom()), at};
        }
        const int count = int(spike.size());
        p.drawPolyline(spike.data(), count,
                       withAlpha(Theme::kSoloOn, int(std::lround((25 + 35 * activity) * sineWeight_))), 7.0,
                       Qt::RoundCap);
        p.drawPolyline(spike.data(), count, withAlpha(Theme::kSoloOn, int(std::lround(60 * sineWeight_))), 3.0,
                       Qt::RoundCap);
        p.drawPolyline(spike.data(), count, withAlpha(Theme::kSoloOn, int(std::lround(255 * sineWeight_))), 1.5,
                       Qt::FlatCap);
    }
    p.restore();

    // The dot: a halo while it erodes, dim at Amount 0.
    if (activity > 0.0)
        p.fillEllipse(at, 5 + 6 * activity, 5 + 6 * activity,
                      withAlpha(Theme::kAccent, int(std::lround(60 * activity))));
    p.fillEllipse(at, 4.5, 4.5, Theme::kPanelAlt);
    p.drawEllipse(QRectF(at.x() - 5, at.y() - 5, 10, 10), amount_ > 0.0 ? Theme::kAccent : Theme::kTextDim, 2);

    // What modulates, and where and how far.
    QString source;
    if (blend_ <= 0.0)
        source = tr("Sine");
    else if (blend_ >= 100.0)
        source = tr("Noise");
    else
        source = tr("Noise %1").arg(sub::app::formatValue(blend_, QStringLiteral("%")));
    source += stereo_ <= 0.0 ? tr(" · Mono")
                             : tr(" · Stereo %1").arg(sub::app::formatValue(stereo_, QStringLiteral("%")));
    p.drawText(QRectF(r.left() + 3, 0, r.width() / 2, kTopStrip), Qt::AlignLeft | Qt::AlignVCenter, source,
               Theme::kTextDim, font);
    const QString readout = sub::app::formatValue(tuned_, QStringLiteral("Hz")) + QStringLiteral(" · ") +
                            sub::app::erosionExcursionText(excursionMs_);
    p.drawText(QRectF(r.left(), 0, r.width() - 3, kTopStrip), Qt::AlignRight | Qt::AlignVCenter, readout,
               amount_ > 0.0 ? Theme::kText : Theme::kTextDim, font);
}

}  // namespace sub::ui
