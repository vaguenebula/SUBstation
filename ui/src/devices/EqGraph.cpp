#include "devices/EqGraph.h"

#include "audio/EqResponse.h"
#include "controls/KnobItem.h"
#include "devices/EditorPaint.h"
#include "devices/EqView.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace sub::ui {

namespace {

using Values = sub::app::OrderedMap<QString, double>;

const QStringList kTypes{QStringLiteral("Bell"),     QStringLiteral("Low Shelf"), QStringLiteral("Low Cut"),
                         QStringLiteral("High Shelf"), QStringLiteral("High Cut"), QStringLiteral("Notch"),
                         QStringLiteral("Band Pass"), QStringLiteral("Tilt Shelf")};
const QStringList kPlaces{QStringLiteral("Stereo"), QStringLiteral("Left"), QStringLiteral("Right"),
                          QStringLiteral("Mid"), QStringLiteral("Side")};
const QStringList kPlaceLetters{QString(), QStringLiteral("L"), QStringLiteral("R"), QStringLiteral("M"),
                                QStringLiteral("S")};
// Where a click on the curve adds which type: up to this fraction of the way across.
constexpr std::pair<double, int> kZones[] = {{0.09, EqGraph::LowCut},
                                             {0.24, EqGraph::LowShelf},
                                             {0.76, EqGraph::Bell},
                                             {0.91, EqGraph::HighShelf},
                                             {2.0, EqGraph::HighCut}};

// A new band's Q (others: Butterworth's) and slope (cuts 24 dB/octave; shelves 12).
double defaultQ(int type) {
    switch (type) {
    case EqGraph::Bell: return 1.0;
    case EqGraph::Notch: return 4.0;
    case EqGraph::BandPass: return 1.0;
    default: return 0.71;
    }
}

std::optional<int> defaultSlope(int type) {
    return type == EqGraph::LowCut || type == EqGraph::HighCut ? std::optional<int>(3) : std::nullopt;
}

const QColor kCurveColor(0xff, 0xd6, 0x8a);
const QColor kBackgroundTop(0x1d, 0x20, 0x27), kBackgroundBottom(0x10, 0x12, 0x16);
const QColor kGridMajor(255, 255, 255, 30), kGridMinor(255, 255, 255, 11);
const QColor kLabelColor(255, 255, 255, 90);
const QColor kPostColor(110, 160, 235);
const QColor kPreColor(200, 210, 230);

double roundTo(double value, int places) {
    const double scale = std::pow(10.0, places);
    return std::nearbyint(value * scale) / scale;
}

// f"{value:+g}"
QString signedGeneral(double value) {
    const QString text = pythonGeneral(value);
    return value >= 0 ? QLatin1Char('+') + text : text;
}

}  // namespace

EqGraph::EqGraph(QQuickItem* parent) : DeviceCanvas(parent) {
    setImplicitSize(200, 60);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setAcceptHoverEvents(true);
    setActiveFocusOnTab(false);
    animation_.setInterval(16);
    connect(&animation_, &QTimer::timeout, this, &EqGraph::animate);
    connect(&displayTimer_, &QTimer::timeout, this, [this] {
        if (alive() && isVisible())
            feed();
    });
    connect(EqView::instance(), &EqView::changed, this, [this] {
        refreshCurves();
        update();
    });
}

EqGraph::~EqGraph() = default;

QString EqGraph::param(int band, const QString& name) {
    return QLatin1Char('b') + QString::number(band + 1) + QLatin1Char('_') + name;
}

QColor EqGraph::bandColor(int band) {
    return QColor::fromHsvF(static_cast<float>(std::fmod(0.02 + band * 0.137, 1.0)), 0.58f, 1.0f);
}

int EqGraph::typeAt(double fraction) {
    for (const auto& [limit, type] : kZones)
        if (fraction < limit)
            return type;
    return HighCut;
}

bool EqGraph::hasGain(int type) { return type == Bell || type == LowShelf || type == HighShelf || type == TiltShelf; }

bool EqGraph::hasSlope(int type) {
    return type == LowShelf || type == HighShelf || type == LowCut || type == HighCut || type == TiltShelf;
}

QString EqGraph::formatFreq(double freq) {
    if (freq >= 1000)
        return pythonFixed(freq / 1000, 2) + QStringLiteral(" kHz");
    return freq < 100 ? pythonFixed(freq, 1) + QStringLiteral(" Hz") : pythonFixed(freq, 0) + QStringLiteral(" Hz");
}

QStringList EqGraph::types() const { return kTypes; }

QStringList EqGraph::slopes() const {
    QStringList names;
    for (int slope : kSlopes)
        names << QStringLiteral("%1 dB/oct").arg(slope);
    return names;
}

QStringList EqGraph::places() const { return kPlaces; }

void EqGraph::setRightInset(qreal inset) {
    if (inset == rightInset_)
        return;
    rightInset_ = inset;
    update();
    Q_EMIT rightInsetChanged();
}

void EqGraph::setDisplayInterval(int ms) {
    if (ms == displayInterval_)
        return;
    displayInterval_ = ms;
    if (ms > 0)
        displayTimer_.start(ms);
    else
        displayTimer_.stop();
    Q_EMIT displayIntervalChanged();
}

// --- The model ---------------------------------------------------------------------------

void EqGraph::sync() {
    std::array<std::optional<Band>, kBands> bands;
    if (alive()) {
        for (int i = 0; i < kBands; ++i) {
            if (value(param(i, QStringLiteral("used"))) < 0.5)
                continue;
            Band band;
            band.index = i;
            band.on = value(param(i, QStringLiteral("on"))) >= 0.5;
            band.type = int(std::nearbyint(value(param(i, QStringLiteral("type")))));
            band.freq = value(param(i, QStringLiteral("freq")));
            band.gain = value(param(i, QStringLiteral("gain")));
            band.q = value(param(i, QStringLiteral("q")));
            band.slope = int(std::nearbyint(value(param(i, QStringLiteral("slope")))));
            band.place = int(std::nearbyint(value(param(i, QStringLiteral("place")))));
            bands[std::size_t(i)] = band;
        }
    }
    bands_ = bands;
    scale_ = alive() ? value(QStringLiteral("scale")) / 100.0 : 1.0;
    if (selected_ >= 0 && !bands_[std::size_t(selected_)]) {
        selected_ = -1;
        Q_EMIT selectedChanged();
    }
    refreshCurves();
    update();
    Q_EMIT bandsChanged();
}

QVariantMap EqGraph::band(int index) const {
    if (index < 0 || index >= kBands || !bands_[std::size_t(index)])
        return {};
    const Band& b = *bands_[std::size_t(index)];
    return {{QStringLiteral("index"), b.index}, {QStringLiteral("on"), b.on},
            {QStringLiteral("type"), b.type},   {QStringLiteral("freq"), b.freq},
            {QStringLiteral("gain"), b.gain},   {QStringLiteral("q"), b.q},
            {QStringLiteral("slope"), b.slope}, {QStringLiteral("place"), b.place},
            {QStringLiteral("color"), bandColor(b.index)}};
}

bool EqGraph::hasBands() const {
    return std::any_of(bands_.begin(), bands_.end(), [](const auto& b) { return b.has_value(); });
}

int EqGraph::freeBand() const {
    for (int i = 0; i < kBands; ++i)
        if (!bands_[std::size_t(i)])
            return i;
    return -1;
}

void EqGraph::select(int index) {
    if (index >= 0 && (index >= kBands || !bands_[std::size_t(index)]))
        index = -1;
    if (index == selected_)
        return;
    selected_ = index;
    update();
    Q_EMIT selectedChanged();
    Q_EMIT bandsChanged();  // (selectedBand)
}

void EqGraph::set(const Values& values, const QString& mergeKey, const QString& text) {
    setParams(values, mergeKey, text);
}

void EqGraph::setParamValue(const QString& paramId, double value, const QString& mergeKey, const QString& text) {
    set({{paramId, value}}, mergeKey, text);
}

void EqGraph::setBandParam(int index, const QString& name, double value, const QString& mergeKey,
                           const QString& text) {
    if (index >= 0 && index < kBands && bands_[std::size_t(index)])
        set({{param(index, name), value}}, mergeKey, text);
}

void EqGraph::setType(int index, int type) {
    if (index < 0 || index >= kBands || !bands_[std::size_t(index)])
        return;
    const Band band = *bands_[std::size_t(index)];
    if (band.type == type)
        return;
    Values values{{param(index, QStringLiteral("type")), double(type)}};
    if (defaultSlope(type) && !hasSlope(band.type))
        values.insert(param(index, QStringLiteral("slope")), double(*defaultSlope(type)));
    set(values, QString(), QStringLiteral("Change EQ Band Type"));
}

void EqGraph::toggleBand(int index) {
    if (index >= 0 && index < kBands && bands_[std::size_t(index)])
        set({{param(index, QStringLiteral("on")), bands_[std::size_t(index)]->on ? 0.0 : 1.0}}, QString(),
            QStringLiteral("Switch EQ Band"));
}

void EqGraph::removeBand(int index) {
    if (index < 0 || index >= kBands || !bands_[std::size_t(index)])
        return;
    set({{param(index, QStringLiteral("used")), 0.0}}, QString(), QStringLiteral("Delete EQ Band"));
    if (selected_ == index)
        select(-1);
}

void EqGraph::removeAll() {
    Values values;
    for (const auto& b : bands_)
        if (b)
            values.insert(param(b->index, QStringLiteral("used")), 0.0);
    if (!values.isEmpty())
        set(values, QString(), QStringLiteral("Delete EQ Bands"));
}

void EqGraph::touchBand(int index, const QString& name) {
    if (index >= 0)
        touch(param(index, name));
}

// --- Geometry ------------------------------------------------------------------------------

QRectF EqGraph::plot() const { return QRectF(0, 0, width(), height()).adjusted(1, 1, -1, -1); }

double EqGraph::xOf(double freq) const {
    const QRectF r = plot();
    return r.left() + std::log(freq / kFreqMin) / std::log(kFreqMax / kFreqMin) * r.width();
}

double EqGraph::freqAt(double x) const {
    const QRectF r = plot();
    const double fraction = std::clamp((x - r.left()) / r.width(), 0.0, 1.0);
    return kFreqMin * std::pow(kFreqMax / kFreqMin, fraction);
}

double EqGraph::half() const { return std::max(10.0, plot().height() / 2 - kMargin); }

double EqGraph::yOf(double db) const { return plot().center().y() - db / EqView::instance()->range() * half(); }

double EqGraph::dbAt(double y) const { return (plot().center().y() - y) / half() * EqView::instance()->range(); }

std::vector<double> EqGraph::columnFreqs() const {
    const int count = std::max(2, int(plot().width() / 2) + 1);
    std::vector<double> freqs(std::size_t(count), 0.0);
    for (int i = 0; i < count; ++i)
        freqs[std::size_t(i)] = kFreqMin * std::pow(kFreqMax / kFreqMin, double(i) / (count - 1));
    return freqs;
}

std::vector<double> EqGraph::response(const Band& band, const std::vector<double>& freqs, double gain) const {
    const QList<double> db = sub::app::eqResponseDb(band.type, band.freq, gain, band.q, band.slope, sampleRate(),
                                                    QList<double>(freqs.begin(), freqs.end()));
    return std::vector<double>(db.begin(), db.end());
}

void EqGraph::refreshCurves() {
    freqs_ = columnFreqs();
    total_.assign(freqs_.size(), 0.0);
    const double rate = sampleRate();
    for (int i = 0; i < kBands; ++i) {
        Curve& curve = curves_[std::size_t(i)];
        const auto& band = bands_[std::size_t(i)];
        if (!band) {
            curve = Curve();
            continue;
        }
        const double gain = band->gain * scale_;  // (as it plays: its gain scaled)
        if (!(curve.band == band) || curve.gain != gain || curve.sampleRate != rate ||
            curve.db.size() != freqs_.size() || curve.lastFreq != freqs_.back()) {
            curve.db = response(*band, freqs_, gain);
            curve.dotDb = hasGain(band->type) ? gain : response(*band, {band->freq}, 0.0).front();
            curve.band = band;
            curve.gain = gain;
            curve.sampleRate = rate;
            curve.lastFreq = freqs_.back();
        }
        if (band->on)
            for (std::size_t k = 0; k < total_.size(); ++k)
                total_[k] += curve.db[k];
    }
    refreshAnalyzer();
}

QPointF EqGraph::dot(const Band& band) const {
    return dotAt(band, hasGain(band.type) ? band.gain * scale_ : response(band, {band.freq}, 0.0).front());
}

QPointF EqGraph::dotAt(const Band& band, double db) const {
    const double range = EqView::instance()->range();
    const double limit = range + kMargin * 0.6 * range / half();
    return QPointF(xOf(band.freq), yOf(std::clamp(db, -limit, limit)));
}

int EqGraph::bandAt(const QPointF& pos) const {
    // The selected one first, then the last drawn.
    int found = -1;
    double nearest = kHitRadius;
    for (const auto& band : bands_) {
        if (!band)
            continue;
        const QPointF at = dot(*band);
        const double distance = std::hypot(pos.x() - at.x(), pos.y() - at.y());
        if (distance < nearest || (band->index == selected_ && distance < kHitRadius)) {
            found = band->index;
            nearest = distance;
            if (band->index == selected_)
                break;
        }
    }
    return found;
}

double EqGraph::curveY(double x) const {
    const double freq = freqAt(x), range = EqView::instance()->range();
    double total = 0.0;
    for (const auto& band : bands_)
        if (band && band->on)
            total += response(*band, {freq}, band->gain * scale_).front();
    return yOf(std::clamp(total, -range * 1.2, range * 1.2));
}

QRectF EqGraph::overlayRect(bool analyzerLabel) const {
    const QRectF r = plot();
    const EqView* view = EqView::instance();
    const QString text = analyzerLabel ? view->analyzerModes().at(view->analyzer())
                                       : QStringLiteral("± %1 dB").arg(pythonGeneral(view->range()));
    const double w = SgPainter::textWidth(text, uiFont(7)) + 12;
    const double right = r.right() - w - 4 - rightInset_;
    return QRectF(analyzerLabel ? r.left() + 4 : right, r.top() + 5, w, 14);
}

QString EqGraph::overlayAt(const QPointF& pos) const {
    if (overlayRect(true).contains(pos))
        return QStringLiteral("analyzer");
    if (overlayRect(false).contains(pos))
        return QStringLiteral("range");
    return {};
}

void EqGraph::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    DeviceCanvas::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        refreshCurves();
}

// --- The analyzer --------------------------------------------------------------------------

void EqGraph::refreshDisplays() {
    if (displayInterval_ <= 0)
        feed();
}

void EqGraph::feed() {
    const int mode = EqView::instance()->analyzer();
    if (mode == 0 || !alive())
        return;
    const double rate = sampleRate();
    if (mode == 1 || mode == 3) {
        const std::vector<float> samples = readDisplay(QStringLiteral("input"));
        analyzer_.feed(sub::app::analysis::EqAnalyzer::Input, samples.data(), samples.size(), rate);
    }
    if (mode == 2 || mode == 3) {
        const std::vector<float> samples = readDisplay(QStringLiteral("output"));
        analyzer_.feed(sub::app::analysis::EqAnalyzer::Output, samples.data(), samples.size(), rate);
    }
    refreshAnalyzer();
    update();
}

void EqGraph::refreshAnalyzer() {
    using sub::app::analysis::EqAnalyzer;
    outputLive_ = analyzer_.live(EqAnalyzer::Output);
    inputLive_ = analyzer_.live(EqAnalyzer::Input);
    outputColumns_ = outputLive_ ? analyzer_.columns(EqAnalyzer::Output, freqs_) : std::vector<double>();
    inputColumns_ = inputLive_ ? analyzer_.columns(EqAnalyzer::Input, freqs_) : std::vector<double>();
}

// --- Mouse ------------------------------------------------------------------------------------

void EqGraph::mousePressEvent(QMouseEvent* event) {
    const QPointF pos = event->position();
    if (secondPressOfDoubleClick(event) && event->button() == Qt::LeftButton)
        return;  // (the double-click follows: as a widget, it has it instead)
    addedOnPress_ = false;
    if (event->button() == Qt::RightButton) {
        const int index = bandAt(pos);
        if (index >= 0) {
            select(index);
            Q_EMIT bandMenuRequested(index, pos);
        } else {
            Q_EMIT viewMenuRequested(pos);
        }
        return;
    }
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    forceActiveFocus(Qt::MouseFocusReason);  // (Delete removes the selected band)
    const QString overlay = overlayAt(pos);
    if (overlay == QLatin1String("range")) {
        EqView::instance()->nextRange();
        return;
    }
    if (overlay == QLatin1String("analyzer")) {
        EqView::instance()->nextAnalyzer();
        return;
    }
    const int index = bandAt(pos);
    if (index >= 0) {
        if (event->modifiers() & Qt::AltModifier) {
            removeBand(index);
            return;
        }
        select(index);
        startDrag(index, pos);
        return;
    }
    if (ghost_) {
        add(pos, ghost_->second);
        return;
    }
    select(-1);
}

void EqGraph::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || addedOnPress_ || !overlayAt(event->position()).isEmpty())
        return;
    const int index = bandAt(event->position());
    if (index >= 0) {
        drag_.reset();
        toggleBand(index);
    } else {  // anywhere, not only on the curve
        const QRectF r = plot();
        add(event->position(), typeAt((event->position().x() - r.left()) / r.width()));
    }
}

void EqGraph::add(const QPointF& pos, int type) {
    const int index = freeBand();
    if (index < 0)
        return;
    const double gain = std::clamp(dbAt(pos.y()) / std::max(scale_, 0.01), -kGainMax, kGainMax);
    const Values values{
        {param(index, QStringLiteral("used")), 1.0},
        {param(index, QStringLiteral("on")), 1.0},
        {param(index, QStringLiteral("type")), double(type)},
        {param(index, QStringLiteral("freq")), roundTo(freqAt(pos.x()), 2)},
        {param(index, QStringLiteral("gain")), hasGain(type) ? roundTo(gain, 2) : 0.0},
        {param(index, QStringLiteral("q")), defaultQ(type)},
        {param(index, QStringLiteral("slope")), double(defaultSlope(type).value_or(1))},
        {param(index, QStringLiteral("place")), 0.0},
    };
    const QString gesture = newGestureKey();
    set(values, gesture, QStringLiteral("Add EQ Band"));
    select(index);
    addedOnPress_ = true;
    setHover(hover_, std::nullopt);
    startDrag(index, pos, gesture, values);
}

void EqGraph::startDrag(int index, const QPointF& pos, const QString& gesture, const Values& base) {
    if (index < 0 || !bands_[std::size_t(index)])
        return;
    const Band band = *bands_[std::size_t(index)];
    touch(param(index, QStringLiteral("freq")));
    drag_ = Drag{index, pos, band.freq, band.gain, band.q, gesture.isEmpty() ? newGestureKey() : gesture, base};
    setCursor(Qt::ClosedHandCursor);
    animateSoon();
}

void EqGraph::mouseMoveEvent(QMouseEvent* event) {
    if (drag_)
        dragTo(event->position(), event->modifiers());
    else
        hoverAt(event->position());
}

void EqGraph::hoverMoveEvent(QHoverEvent* event) {
    if (!drag_)
        hoverAt(event->position());
}

void EqGraph::setHover(int hover, const std::optional<std::pair<QPointF, int>>& ghost) {
    if (hover == hover_ && ghost == ghost_)
        return;
    hover_ = hover;
    ghost_ = ghost;
    if (ghost_) {
        lastGhost_ = *ghost_;
        hasLastGhost_ = true;
    }
    animateSoon();
    update();
    Q_EMIT hoverChanged();
}

void EqGraph::hoverAt(const QPointF& pos) {
    const int hover = bandAt(pos);
    std::optional<std::pair<QPointF, int>> ghost;
    const bool onOverlay = !overlayAt(pos).isEmpty();
    if (hover < 0 && !onOverlay && freeBand() >= 0) {
        const double y = curveY(pos.x());
        if (std::abs(y - pos.y()) <= kCurveHit) {
            const QRectF r = plot();
            ghost = std::pair{QPointF(pos.x(), y), typeAt((pos.x() - r.left()) / r.width())};
        }
    }
    if (hover != hover_ || ghost != ghost_) {
        if (hover >= 0)
            setCursor(Qt::OpenHandCursor);
        else if (ghost)
            setCursor(Qt::CrossCursor);
        else if (onOverlay)
            setCursor(Qt::PointingHandCursor);
        else
            unsetCursor();
        setHover(hover, ghost);
    }
}

void EqGraph::dragTo(const QPointF& pos, Qt::KeyboardModifiers modifiers) {
    const Drag& drag = *drag_;
    const int index = drag.band;
    if (!bands_[std::size_t(index)])
        return;
    const Band band = *bands_[std::size_t(index)];
    const double fine = modifiers & Qt::ShiftModifier ? 0.15 : 1.0;
    const double dx = (pos.x() - drag.origin.x()) * fine, dy = (pos.y() - drag.origin.y()) * fine;
    Values values = drag.base;
    const double freq = drag.freq * std::pow(kFreqMax / kFreqMin, dx / plot().width());
    values.insert(param(index, QStringLiteral("freq")), roundTo(std::clamp(freq, kFreqMin, kFreqMax), 2));
    if ((modifiers & Qt::ControlModifier) || !hasGain(band.type)) {
        const double q = drag.q * std::pow(2.0, -dy / 45.0);
        values.insert(param(index, QStringLiteral("q")), roundTo(std::clamp(q, kQMin, kQMax), 3));
    } else {
        const double gain = drag.gain - dy / half() * EqView::instance()->range() / std::max(scale_, 0.01);
        values.insert(param(index, QStringLiteral("gain")), roundTo(std::clamp(gain, -kGainMax, kGainMax), 2));
    }
    set(values, drag.gesture, drag.base.isEmpty() ? QStringLiteral("Move EQ Band") : QStringLiteral("Add EQ Band"));
}

void EqGraph::mouseReleaseEvent(QMouseEvent* event) {
    if (drag_) {
        drag_.reset();
        hoverAt(event->position());
        update();
    }
}

void EqGraph::mouseUngrabEvent() {
    if (drag_) {
        drag_.reset();
        update();
    }
}

void EqGraph::hoverLeaveEvent(QHoverEvent*) {
    if (!drag_) {
        unsetCursor();
        setHover(-1, std::nullopt);
    }
}

void EqGraph::wheelEvent(QWheelEvent* event) {
    int index = drag_ ? drag_->band : bandAt(event->position());
    if (index < 0)
        index = selected_;
    const int delta = event->angleDelta().y() ? event->angleDelta().y() : event->angleDelta().x();  // (Alt turns it sideways)
    if (index < 0 || !bands_[std::size_t(index)] || delta == 0) {
        event->ignore();
        return;
    }
    const Band band = *bands_[std::size_t(index)];
    event->accept();
    // Notches closer than 400 ms are one gesture.
    if (wheelGesture_.isEmpty() || !wheelClock_.isValid() || wheelClock_.elapsed() > 400)
        wheelGesture_ = newGestureKey();
    wheelClock_.start();
    const double notches = delta / 120.0;
    const Qt::KeyboardModifiers modifiers = event->modifiers();
    const bool draggingCut = drag_ && (band.type == LowCut || band.type == HighCut);
    if (draggingCut || (modifiers & Qt::AltModifier)) {
        if (hasSlope(band.type)) {
            const double slope = std::clamp(band.slope + (notches > 0 ? 1 : -1), 0, int(std::size(kSlopes)) - 1);
            wheelSet(index, {{param(index, QStringLiteral("slope")), slope}}, QString(),
                     QStringLiteral("Change EQ Band Slope"));
        }
        return;
    }
    const double step = modifiers & Qt::ShiftModifier ? 1.03 : 1.15;
    const double q = roundTo(std::clamp(band.q * std::pow(step, notches), kQMin, kQMax), 3);
    if (drag_ && drag_->band == index) {
        // The drag goes on from the new Q: where the mouse is now gives it (see dragTo).
        const double fine = modifiers & Qt::ShiftModifier ? 0.15 : 1.0;
        const double dy = (event->position().y() - drag_->origin.y()) * fine;
        const bool setsQ = (modifiers & Qt::ControlModifier) || !hasGain(band.type);
        drag_->q = setsQ ? q * std::pow(2.0, dy / 45.0) : q;
    }
    wheelSet(index, {{param(index, QStringLiteral("q")), q}}, wheelGesture_, QStringLiteral("Change EQ Band Q"));
}

void EqGraph::wheelSet(int index, const Values& changes, const QString& gesture, const QString& text) {
    // During the drag that added the band, every move sets all its parameters (one undo step),
    // so they take the change too, or the next move would undo it.
    if (drag_ && drag_->band == index && !drag_->base.isEmpty() && bands_[std::size_t(index)]) {
        const Band band = *bands_[std::size_t(index)];
        for (const auto& [key, value] : changes)
            drag_->base.insert(key, value);
        Values values = drag_->base;
        values.insert(param(index, QStringLiteral("freq")), band.freq);
        values.insert(param(index, QStringLiteral("gain")), band.gain);
        for (const auto& [key, value] : changes)
            values.insert(key, value);
        set(values, drag_->gesture, QStringLiteral("Add EQ Band"));
    } else {
        set(changes, gesture, text);
    }
}

bool EqGraph::event(QEvent* event) {
    // Delete removes the selected band, not what the app's Delete would (the selected devices).
    if (event->type() == QEvent::ShortcutOverride && selected_ >= 0) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Delete || key == Qt::Key_Backspace) {
            event->accept();
            return true;
        }
    }
    return DeviceCanvas::event(event);
}

void EqGraph::keyPressEvent(QKeyEvent* event) {
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && selected_ >= 0) {
        removeBand(selected_);
        event->accept();
        return;
    }
    event->ignore();
}

// --- Animation -------------------------------------------------------------------------------

double EqGraph::targetRadius(int index) const {
    const bool active = index == hover_ || (drag_ && drag_->band == index);
    return active ? kDotHoverRadius : kDotRadius;
}

void EqGraph::animateSoon() {
    if (!animation_.isActive())
        animation_.start();
}

void EqGraph::animate() {
    bool settled = true;
    for (const auto& band : bands_) {
        if (!band)
            continue;
        double& current = radius_[std::size_t(band->index)];
        const double target = targetRadius(band->index);
        current += (target - current) * 0.35;
        if (std::abs(target - current) < 0.05)
            current = target;
        else
            settled = false;
    }
    const double target = ghost_ ? 1.0 : 0.0;
    ghostAlpha_ += (target - ghostAlpha_) * 0.3;
    if (std::abs(target - ghostAlpha_) < 0.02)
        ghostAlpha_ = target;
    else
        settled = false;
    update();
    if (settled)
        animation_.stop();
}

// --- Painting ----------------------------------------------------------------------------------

void EqGraph::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const QRectF r = plot();
    const EqView* view = EqView::instance();
    const double range = view->range();
    QLinearGradient background(r.topLeft(), r.bottomLeft());
    background.setColorAt(0.0, kBackgroundTop);
    background.setColorAt(1.0, kBackgroundBottom);
    p.fillRect(QRectF(0, 0, width(), height()), background);
    p.save();
    p.setClipRect(r);
    const QFont small = uiFont(7);

    // The grid.
    for (double decade : {10.0, 100.0, 1000.0, 10000.0}) {
        for (int multiple = 1; multiple < 10; ++multiple) {
            const double freq = decade * multiple;
            if (!(kFreqMin < freq && freq < kFreqMax))
                continue;
            const double x = xOf(freq);
            const bool major = multiple == 1 || multiple == 2 || multiple == 5;
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()),
                       multiple == 1 ? kGridMajor : !major ? kGridMinor : QColor(255, 255, 255, 18));
            if (major && freq >= 20) {
                const QString label =
                    freq >= 1000 ? pythonGeneral(freq / 1000) + QLatin1Char('k') : pythonGeneral(freq);
                p.drawText(QRectF(x + 3, r.bottom() - 13, 40, 12), Qt::AlignLeft, label, kLabelColor, small);
            }
        }
    }
    const double step = range / (range <= 3 ? 2 : range <= 12 ? 4 : 3);
    for (double db = -range; db <= range + 1e-6; db += step) {
        const double y = yOf(db);
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y),
                   std::abs(db) < 1e-6 ? QColor(255, 255, 255, 46) : kGridMinor);
        if (std::abs(db) > 1e-6 && r.top() + 8 < y && y < r.bottom() - 16)
            p.drawText(QRectF(r.right() - 34, y - 6, 31, 12), Qt::AlignRight | Qt::AlignVCenter,
                       signedGeneral(std::nearbyint(db * 1e6) / 1e6), kLabelColor, small);
    }

    // The analyzer: the output filled, the input a line.
    const std::size_t columns = freqs_.size();
    std::vector<double> xs(columns);
    for (std::size_t i = 0; i < columns; ++i)
        xs[i] = columns > 1 ? r.left() + (r.right() - r.left()) * double(i) / double(columns - 1) : r.left();
    auto spectrumY = [&](double db) {
        const double fraction = (db - sub::app::analysis::EqAnalyzer::kFloorDb) /
                                (sub::app::analysis::EqAnalyzer::kCeilDb - sub::app::analysis::EqAnalyzer::kFloorDb);
        return r.bottom() - std::clamp(fraction, 0.0, 1.05) * r.height();
    };
    const int mode = view->analyzer();
    if ((mode == 2 || mode == 3) && outputLive_ && outputColumns_.size() == columns) {
        std::vector<QPointF> shape;
        shape.reserve(columns + 2);
        shape.emplace_back(xs.front(), r.bottom());
        for (std::size_t i = 0; i < columns; ++i)
            shape.emplace_back(xs[i], spectrumY(outputColumns_[i]));
        shape.emplace_back(xs.back(), r.bottom());
        QLinearGradient fill(r.topLeft(), r.bottomLeft());
        fill.setColorAt(0.0, withAlpha(kPostColor, 120));
        fill.setColorAt(1.0, withAlpha(kPostColor, 18));
        p.fillToBaseline(shape.data(), int(shape.size()), r.bottom(), fill);
        p.drawPolygon(shape.data(), int(shape.size()), withAlpha(kPostColor.lighter(130), 120), 1.0);
    }
    if ((mode == 1 || mode == 3) && inputLive_ && inputColumns_.size() == columns) {
        std::vector<QPointF> line(columns);
        for (std::size_t i = 0; i < columns; ++i)
            line[i] = QPointF(xs[i], spectrumY(inputColumns_[i]));
        p.drawPolyline(line.data(), int(line.size()), withAlpha(kPreColor, mode == 3 ? 70 : 110), 1.0);
    }

    // The bands, then their total.
    const double limit = range * 3 + 20;
    auto curvePoints = [&](const std::vector<double>& db) {
        std::vector<QPointF> points(db.size());
        for (std::size_t i = 0; i < db.size() && i < xs.size(); ++i)
            points[i] = QPointF(xs[i], yOf(std::clamp(db[i], -limit, limit)));
        return points;
    };
    const double zero = yOf(0.0);
    for (const auto& band : bands_) {
        if (!band || !band->on || curves_[std::size_t(band->index)].db.size() != columns)
            continue;
        const std::vector<QPointF> curve = curvePoints(curves_[std::size_t(band->index)].db);
        const QColor color = bandColor(band->index);
        if (band->index == selected_ || band->index == hover_) {
            const double strength = band->index == selected_ ? 1.0 : 0.6;
            QLinearGradient fill(QPointF(0, yOf(range)), QPointF(0, yOf(-range)));
            fill.setColorAt(0.0, withAlpha(color, int(95 * strength)));
            fill.setColorAt(0.5, withAlpha(color, int(25 * strength)));
            fill.setColorAt(1.0, withAlpha(color, int(95 * strength)));
            p.fillToBaseline(curve.data(), int(curve.size()), zero, fill);
            p.drawPolyline(curve.data(), int(curve.size()), withAlpha(color, int(120 * strength)), 1.0);
        } else {
            p.drawPolyline(curve.data(), int(curve.size()), withAlpha(color, 55), 1.0);
        }
    }
    if (total_.size() == columns) {
        const std::vector<QPointF> total = curvePoints(total_);
        p.drawPolyline(total.data(), int(total.size()), withAlpha(kCurveColor, 38), 7.0, Qt::RoundCap);
        p.drawPolyline(total.data(), int(total.size()), withAlpha(kCurveColor, 70), 3.5, Qt::RoundCap);
        p.drawPolyline(total.data(), int(total.size()), kCurveColor, 1.8, Qt::RoundCap);
    }
    if (!hasBands())
        p.drawText(r.adjusted(0, 0, 0, -r.height() * 0.45), Qt::AlignCenter, QStringLiteral("Click the curve to add a band"),
                   QColor(255, 255, 255, 70), uiFont(8));

    // The ghost band, fading in and out.
    if (ghostAlpha_ > 0.01 && hasLastGhost_) {
        const auto& [center, type] = lastGhost_;
        const double alpha = ghostAlpha_;
        const QColor color = bandColor(std::max(freeBand(), 0));
        p.fillEllipse(center, kDotRadius + 1, kDotRadius + 1, withAlpha(color, int(45 * alpha)));
        p.drawEllipse(QRectF(center.x() - kDotRadius - 1, center.y() - kDotRadius - 1, 2 * kDotRadius + 2,
                             2 * kDotRadius + 2),
                      withAlpha(color, int(220 * alpha)), 1.5);
        const QColor white = withAlpha(Qt::white, int(230 * alpha));
        p.drawLine(QPointF(center.x() - 3, center.y()), QPointF(center.x() + 3, center.y()), white, 1.4);
        p.drawLine(QPointF(center.x(), center.y() - 3), QPointF(center.x(), center.y() + 3), white, 1.4);
        const QString label = kTypes.at(type);
        const double w = SgPainter::textWidth(label, small) + 10;
        const bool above = center.y() - 26 > r.top();
        QRectF rect(center.x() - w / 2, center.y() + (above ? -24 : 12), w, 14);
        rect.moveLeft(std::max(r.left() + 2, std::min(r.right() - w - 2, rect.left())));
        p.fillRoundedRect(rect, 7, 7, QColor(12, 13, 16, int(200 * alpha)));
        p.drawText(rect, Qt::AlignCenter, label, withAlpha(color, int(255 * alpha)), small);
    }

    // The dots, the focus on top.
    std::vector<const Band*> order;
    for (const auto& band : bands_)
        if (band)
            order.push_back(&*band);
    std::stable_sort(order.begin(), order.end(), [this](const Band* a, const Band* b) {
        const auto rank = [this](const Band* band) {
            return std::pair{band->index == selected_, band->index == hover_};
        };
        return rank(a) < rank(b);
    });
    const QFont numberFont = uiFont(7, true);
    for (const Band* band : order) {
        const QPointF center = dotAt(*band, curves_[std::size_t(band->index)].dotDb);
        const double stored = radius_[std::size_t(band->index)];
        const double radius = stored != 0.0 ? stored : kDotRadius;
        const QColor color = band->on ? bandColor(band->index) : QColor(110, 112, 118);
        if (band->index == selected_)
            p.drawEllipse(QRectF(center.x() - radius - 3, center.y() - radius - 3, 2 * radius + 6, 2 * radius + 6),
                          QColor(255, 255, 255, 230), 1.6);
        p.fillEllipse(center, radius + 2, radius + 2, withAlpha(color, band->index == hover_ ? 60 : 35));
        const QRectF disc(center.x() - radius, center.y() - radius, 2 * radius, 2 * radius);
        p.fillEllipse(disc, band->on ? color : QColor(40, 42, 48));
        p.drawEllipse(disc, band->on ? QColor(10, 10, 12, 200) : color, 1.2);
        p.drawText(disc, Qt::AlignCenter, QString::number(band->index + 1), band->on ? QColor(15, 15, 18) : color,
                   numberFont);
        const QString letter = kPlaceLetters.value(band->place);
        if (!letter.isEmpty())
            p.drawText(QRectF(center.x() + radius + 1, center.y() - radius - 9, 12, 10), Qt::AlignLeft, letter,
                       withAlpha(color, 230), numberFont);
    }

    // The badge of the band dragged or hovered.
    const int badgeIndex = drag_ ? drag_->band : hover_;
    if (badgeIndex >= 0 && bands_[std::size_t(badgeIndex)]) {
        const Band& band = *bands_[std::size_t(badgeIndex)];
        QStringList parts{kTypes.at(band.type), formatFreq(band.freq)};
        if (hasGain(band.type))
            parts << pythonSigned(band.gain * scale_, 1) + QStringLiteral(" dB");
        parts << QStringLiteral("Q ") + pythonFixed(band.q, 2);
        if (hasSlope(band.type))
            parts << QStringLiteral("%1 dB/oct").arg(kSlopes[std::clamp(band.slope, 0, 8)]);
        const QString text = parts.join(QStringLiteral("   "));
        const double w = SgPainter::textWidth(text, small) + 14;
        const QPointF center = dotAt(band, curves_[std::size_t(band.index)].dotDb);
        const bool above = center.y() - 30 > r.top();
        QRectF rect(center.x() - w / 2, center.y() + (above ? -30 : 16), w, 16);
        rect.moveLeft(std::max(r.left() + 2, std::min(r.right() - w - 2, rect.left())));
        p.fillRoundedRect(rect, 8, 8, QColor(12, 13, 16, 225));
        p.drawRoundedRect(rect, 8, 8, withAlpha(bandColor(band.index), 170), 1);
        p.drawText(rect, Qt::AlignCenter, text, QColor(235, 235, 240), small);
    }

    // The clickable labels: the analyzer's mode, the range.
    for (bool analyzerLabel : {true, false}) {
        const QRectF rect = overlayRect(analyzerLabel);
        const QString text = analyzerLabel ? view->analyzerModes().at(mode)
                                           : QStringLiteral("± %1 dB").arg(pythonGeneral(range));
        p.fillRoundedRect(rect, 7, 7, QColor(255, 255, 255, 16));
        p.drawText(rect, Qt::AlignCenter, text, QColor(255, 255, 255, 120), small);
    }
    p.restore();
}

}  // namespace sub::ui
