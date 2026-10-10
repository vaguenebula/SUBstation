#include "devices/AmpPanel.h"

#include "audio/AmpResponse.h"
#include "devices/AmpDisplays.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sub::ui {

namespace {

constexpr double kRiseSeconds = 0.03;   // a tube flares this quickly
constexpr double kCoolSeconds = 0.25;   // and cools this slowly
constexpr double kSagSeconds = 0.06;    // the power tube's blue follows the sag
constexpr double kLampSeconds = 0.05;   // the pilot lamp
constexpr double kColorSeconds = 0.08;  // a new model's colour
constexpr double kMeterFallDbPerSecond = 24.0;
constexpr double kMeterHoldSeconds = 1.0;

}  // namespace

AmpPanel::AmpPanel(QQuickItem* parent)
    : DeviceCanvas(parent), floorDb_(ampDisplays::floorDb()), names_(sub::app::ampModelNames()) {
    setAcceptedMouseButtons(Qt::NoButton);
    colorFrom_ = colorTo_ = ampModelColor(0);
    colorMix_.snap(1.0);
    driveDb_.fill(floorDb_);
    outputDb_ = floorDb_;
    for (Eased& g : glow_) g.snap(kIdleGlow);
    sag_.snap(0.0);
    lamp_.snap(lampFor(floorDb_, 0.0));
    meter_.reset(kMeterFloorDb);
}

QFont AmpPanel::logoFont() {
    QFont font = uiFont(10, true);
    font.setItalic(true);
    return font;
}

double AmpPanel::logoWidth() const {
    double widest = 0.0;
    for (const QString& name : names_) widest = std::max(widest, SgPainter::textWidth(name, logoFont()));
    return widest;
}

void AmpPanel::setTubeRect(const QRectF& rect) {
    if (rect == tubeRect_)
        return;
    tubeRect_ = rect;
    Q_EMIT rectsChanged();
    update();
}

void AmpPanel::setJewelRect(const QRectF& rect) {
    if (rect == jewelRect_)
        return;
    jewelRect_ = rect;
    Q_EMIT rectsChanged();
    update();
}

void AmpPanel::setMeterRect(const QRectF& rect) {
    if (rect == meterRect_)
        return;
    meterRect_ = rect;
    Q_EMIT rectsChanged();
    update();
}

QColor AmpPanel::modelColor() const { return mixColor(colorFrom_, colorTo_, colorMix_.value); }

double AmpPanel::glow(int tube) const { return tube >= 0 && tube < kTubes ? glow_[size_t(tube)].value : 0.0; }

double AmpPanel::glowTarget(int tube) const { return tube >= 0 && tube < kTubes ? glow_[size_t(tube)].target : 0.0; }

double AmpPanel::glowFor(double db) {
    // An idle filament's glow, full at 6 dB over the clipping point, from 24 dB under it.
    return kIdleGlow + (1.0 - kIdleGlow) * smoothstep((db + 24.0) / 30.0);
}

double AmpPanel::lampFor(double outputDb, double sagDb) {
    // Brighter with the level, dimmer as the supply sags, as a real pilot lamp.
    const double level = std::clamp((outputDb + 36.0) / 36.0, 0.0, 1.0);
    return (0.45 + 0.55 * level) * (1.0 - 0.5 * std::clamp(sagDb / 8.0, 0.0, 1.0));
}

void AmpPanel::sync() {
    if (device() == nullptr) {  // (not yet, as it is being made: the first sync with it snaps)
        update();
        return;
    }
    const int model =
        std::clamp(static_cast<int>(std::lround(value(QStringLiteral("type")))), 0, int(names_.size()) - 1);
    if (!synced_ || model != model_) {
        if (synced_) {  // the colour turns from what is drawn to the new model's
            colorFrom_ = modelColor();
            colorMix_.value = 0.0;
            colorMix_.target = 1.0;
        } else {
            colorFrom_ = ampModelColor(model);
            colorMix_.snap(1.0);
        }
        colorTo_ = ampModelColor(model);
        const bool changed = model != model_;
        model_ = model;
        synced_ = true;
        if (changed)
            Q_EMIT modelChanged();
        Q_EMIT modelColorChanged();
    }
    update();
}

void AmpPanel::refreshDisplays() {
    const double dt = tickSeconds();

    // The latest values; a display that read nothing keeps its last.
    bool read = false;
    static const QString kDrives[kTubes] = {QStringLiteral("drive1"), QStringLiteral("drive2"),
                                            QStringLiteral("drive3"), QStringLiteral("power")};
    const double recent = ampDisplays::recentSeconds(dt);
    for (int i = 0; i < kTubes; ++i)
        read = ampDisplays::loudest(readRecent(kDrives[i], recent), floorDb_, driveDb_[size_t(i)]) || read;
    read = ampDisplays::latest(readRecent(QStringLiteral("sag"), recent), sagRead_) || read;
    read = ampDisplays::loudest(readRecent(QStringLiteral("output"), recent), floorDb_, outputDb_) || read;
    if (read) {
        lastRead_.restart();
    } else if (!lastRead_.isValid() || lastRead_.elapsed() > kQuietSeconds * 1000.0) {
        driveDb_.fill(floorDb_);  // nothing coming: everything cools
        sagRead_ = 0.0;
        outputDb_ = floorDb_;
    }

    bool moving = false;
    for (int i = 0; i < kTubes; ++i) {
        Eased& g = glow_[size_t(i)];
        g.target = glowFor(driveDb_[size_t(i)]);
        moving = g.step(easeFraction(dt, g.target > g.value ? kRiseSeconds : kCoolSeconds)) || moving;
    }
    sag_.target = std::max(0.0, sagRead_);
    moving = sag_.step(easeFraction(dt, kSagSeconds), 1e-3) || moving;
    lamp_.target = lampFor(outputDb_, sagRead_);
    moving = lamp_.step(easeFraction(dt, kLampSeconds)) || moving;
    const double level = meter_.level, peak = meter_.peak;
    meter_.update(outputDb_, dt, kMeterFallDbPerSecond, kMeterHoldSeconds, kMeterFloorDb);
    moving = moving || meter_.level != level || meter_.peak != peak;
    if (colorMix_.step(easeFraction(dt, kColorSeconds))) {
        Q_EMIT modelColorChanged();
        moving = true;
    }
    if (moving) {
        Q_EMIT levelsChanged();
        update();
    }
}

// --- Painting ---------------------------------------------------------------------------

void AmpPanel::paint(SgPainter& p) {
    p.setAntialiasing(true);
    if (!tubeRect_.isEmpty())
        paintTubes(p);
    if (!jewelRect_.isEmpty())
        paintJewel(p);
    if (!meterRect_.isEmpty())
        drawLevelMeter(p, meterRect_, meter_.level, meter_.peak, kMeterFloorDb, 0.0);
}

void AmpPanel::paintTubes(SgPainter& p) const {
    const QRectF r = tubeRect_;
    p.fillRoundedRect(r, 4, 4, Theme::kMeterBg);
    const double rail = r.bottom() - 11.0;  // the sockets' rail, the labels under it
    const double room = std::max(10.0, rail - r.top() - 4.0);
    constexpr double kPreampWidth = 22.0, kPowerWidth = 30.0, kGap = 8.0;
    const double total = 3 * kPreampWidth + kPowerWidth + 3 * kGap;
    const double left = r.left() + std::round((r.width() - total) / 2.0);
    const double blue = std::clamp(sag_.value / 6.0, 0.0, 1.0);
    auto bodyOf = [&](int i) {
        const bool power = i == kTubes - 1;
        const double h = std::round((power ? 0.9 : 0.68) * room);
        return QRectF(left + i * (kPreampWidth + kGap), rail - h, power ? kPowerWidth : kPreampWidth, h);
    };
    // The glow the tubes throw around them first, so a tube's glass stays over its neighbour's.
    p.save();
    p.setClipRect(r.adjusted(1, 1, -1, -1));
    for (int i = 0; i < kTubes; ++i) paintBloom(p, bodyOf(i), glow_[size_t(i)].value);
    p.restore();
    p.drawLine(QPointF(r.left() + 6, rail + 0.5), QPointF(r.right() - 6, rail + 0.5), withAlpha(Theme::kGridBar, 160));
    static const QString kLabels[kTubes] = {QStringLiteral("V1"), QStringLiteral("V2"), QStringLiteral("V3"),
                                            QStringLiteral("P")};
    const QFont font = uiFont(7);
    for (int i = 0; i < kTubes; ++i) {
        const QRectF body = bodyOf(i);
        paintTube(p, body, glow_[size_t(i)].value, i == kTubes - 1 ? blue : 0.0);
        p.drawText(QRectF(body.left() - 4, rail + 1, body.width() + 8, r.bottom() - rail - 1), Qt::AlignCenter,
                   kLabels[i], Theme::kTextDim, font);
    }
}

void AmpPanel::paintBloom(SgPainter& p, const QRectF& body, double g) const {
    if (g <= 0.0)
        return;
    // Layers fading outwards, wider the harder it glows: a soft halo round the heater.
    const double w = body.width(), h = body.height();
    const QPointF heater(body.center().x(), body.top() + 0.62 * h);
    const QColor bloom(0xff, 0x96, 0x38);
    const double spread = 0.75 + 0.35 * g;
    const double strength = g * g;  // a tube driven hard stands out from one merely warm
    p.fillEllipse(heater, 1.05 * w * spread, 0.46 * h * spread, withAlpha(bloom, int(12 * strength)));
    p.fillEllipse(heater, 0.8 * w * spread, 0.33 * h * spread, withAlpha(bloom, int(20 * strength)));
    p.fillEllipse(heater, 0.58 * w * spread, 0.22 * h * spread, withAlpha(bloom, int(32 * strength)));
}

void AmpPanel::paintTube(SgPainter& p, const QRectF& body, double g, double blue) const {
    const double w = body.width(), h = body.height();
    const double round = w / 2.0;
    const QColor cold(0xff, 0x6a, 0x00), hot(0xff, 0xd2, 0x7a);
    const QColor heat = mixColor(cold, hot, g);  // orange when idle, nearly white when driven hard
    // The glass, warmed from inside (and hazed blue, the power tube's, as the supply sags).
    p.fillRoundedRect(body, round, round, withAlpha(Theme::kSurface, 64));
    p.fillRoundedRect(body, round, round, withAlpha(QColor(0xff, 0x8a, 0x2a), int(36 * g * g)));
    if (blue > 0.0)
        p.fillRoundedRect(body, round, round, withAlpha(QColor(0x6f, 0x7f, 0xff), int(64 * blue)));
    // The getter's silvering at the top.
    p.fillEllipse(QPointF(body.center().x(), body.top() + 0.42 * round + 1.0), 0.3 * w, 0.14 * round + 0.8,
                  withAlpha(QColor(0xc8, 0xc8, 0xd2), 46));
    // The plate (anode), the cathode glowing through its slot.
    const QRectF plate(body.center().x() - 0.3 * w, body.top() + 0.26 * h, 0.6 * w, 0.36 * h);
    p.fillRect(plate, QColor(0x2f, 0x2f, 0x2f));
    p.fillRect(QRectF(plate.left(), plate.top(), 1.0, plate.height()), QColor(0x46, 0x46, 0x46));
    if (blue > 0.0)
        p.fillRect(plate, withAlpha(QColor(0x7a, 0x8c, 0xff), int(110 * blue)));
    p.fillRect(QRectF(body.center().x() - 1.0, plate.top() + 2, 2.0, plate.height() - 4),
               withAlpha(heat, int(50 + 180 * g)));
    // The heater under it.
    p.fillEllipse(QPointF(body.center().x(), body.top() + 0.7 * h), 0.27 * w, 1.6, withAlpha(heat, int(120 + 135 * g)));
    // The glass's edge and a reflection down its left side.
    p.drawRoundedRect(body.adjusted(0.5, 0.5, -0.5, -0.5), round, round, withAlpha(Qt::white, 40));
    p.drawLine(QPointF(body.left() + 3.5, body.top() + round), QPointF(body.left() + 3.5, body.bottom() - 9),
               withAlpha(Qt::white, 26), 1.0);
    // The base.
    p.fillRect(QRectF(body.left(), body.bottom() - 6, w, 6), QColor(0x1a, 0x1a, 0x1a));
    p.fillRect(QRectF(body.left(), body.bottom() - 6, w, 1), QColor(0x2c, 0x2c, 0x2c));
}

void AmpPanel::paintJewel(SgPainter& p) const {
    const QRectF r = jewelRect_;
    p.fillRoundedRect(r, 4, 4, Theme::kMeterBg);
    const double b = lamp_.value;
    const QPointF centre(r.center().x(), r.top() + 16.0);
    const QColor red(0xff, 0x3b, 0x2f);
    p.save();
    p.setClipRect(r);
    p.fillEllipse(centre, 14, 14, withAlpha(red, int(16 * b)));  // the halo
    p.fillEllipse(centre, 10, 10, withAlpha(red, int(40 * b)));
    p.restore();
    p.fillEllipse(centre, 6, 6, withAlpha(red, int(90 + 165 * b)));
    p.drawEllipse(QRectF(centre.x() - 7, centre.y() - 7, 14, 14), Theme::kGridBar, 1.5);  // the bezel
    p.fillEllipse(QPointF(centre.x() - 2.0, centre.y() - 2.5), 1.5, 1.0, withAlpha(Qt::white, 110));  // a facet
    // The model's name as the amp's logo.
    const double top = centre.y() + 9.0;
    p.drawText(QRectF(r.left(), top, r.width(), r.bottom() - top), Qt::AlignCenter, names_.value(model_),
               modelColor(), logoFont());
}

}  // namespace sub::ui
