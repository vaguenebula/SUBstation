#pragma once

// The Amp's face behind its controls: what is drawn that isn't a graph, in
// three dark wells the editor lays out (tubeRect, jewelRect, meterRect).
//
// - The tube window: the three preamp tubes (V1, V2, V3) and the power tube
//   (P), each glowing as hard as its stage is driven (the displays drive1..3 and
//   power: its peak against its clipping point), rising quickly and cooling
//   slowly as a filament does; the power tube's plate turns blue as the supply
//   sags (the display sag), as hard-driven power tubes do.
// - The pilot lamp, brightening with the output and dimming as the supply sags,
//   over the model's name in its colour (the amp's logo).
// - The output meter (the display output).
//
// A display tick that reads nothing (the engine hands its values over at its
// own pace) keeps the last values, so nothing flickers with the block size;
// after kQuietSeconds with nothing at all (the device off, the engine stopped)
// everything cools. It takes no mouse: clicks go on to the controls over it and
// to the frame.

#include "devices/DeviceCanvas.h"
#include "devices/EditorPaint.h"

#include <QColor>
#include <QElapsedTimer>
#include <QRectF>
#include <QtQml/qqmlregistration.h>

#include <algorithm>
#include <array>

namespace sub::ui {

// The models' colours (Clean, Boost, Blues, Rock, Lead, Heavy, Bass): the logo
// and the editor's selector underline, the one accent of its own the face has.
inline QColor ampModelColor(int model) {
    static constexpr QColor kColors[] = {
        QColor(0xe6, 0xd3, 0xa3),  // Clean: fawn
        QColor(0xf0, 0xa2, 0x4a),  // Boost: copper
        QColor(0x8f, 0xc1, 0xd4),  // Blues: silverface blue
        QColor(0xe8, 0xc2, 0x4a),  // Rock: gold
        QColor(0xff, 0x5a, 0x4a),  // Lead: red
        QColor(0xb0, 0x7c, 0xff),  // Heavy: violet
        QColor(0x6f, 0xd0, 0x8c),  // Bass: green
    };
    return kColors[std::clamp(model, 0, 6)];
}

class AmpPanel : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QRectF tubeRect READ tubeRect WRITE setTubeRect NOTIFY rectsChanged)
    Q_PROPERTY(QRectF jewelRect READ jewelRect WRITE setJewelRect NOTIFY rectsChanged)
    Q_PROPERTY(QRectF meterRect READ meterRect WRITE setMeterRect NOTIFY rectsChanged)
    // The model's colour as drawn (it turns to a new model's over a moment).
    Q_PROPERTY(QColor modelColor READ modelColor NOTIFY modelColorChanged)
    Q_PROPERTY(int model READ model NOTIFY modelChanged)
    // As drawn: the meter's level (dB), the sag (dB) and the lamp's brightness (0..1).
    Q_PROPERTY(double outputLevel READ outputLevel NOTIFY levelsChanged)
    Q_PROPERTY(double sagDb READ sagDb NOTIFY levelsChanged)
    Q_PROPERTY(double lamp READ lamp NOTIFY levelsChanged)

public:
    static constexpr int kTubes = 4;  // V1, V2, V3, P
    static constexpr double kIdleGlow = 0.12;  // a filament's glow with nothing through it
    static constexpr double kQuietSeconds = 0.3;  // with no display values this long, everything cools
    static constexpr double kFloorDb = -90.0;
    static constexpr double kMeterFloorDb = -60.0;

    explicit AmpPanel(QQuickItem* parent = nullptr);

    QRectF tubeRect() const { return tubeRect_; }
    void setTubeRect(const QRectF& rect);
    QRectF jewelRect() const { return jewelRect_; }
    void setJewelRect(const QRectF& rect);
    QRectF meterRect() const { return meterRect_; }
    void setMeterRect(const QRectF& rect);

    QColor modelColor() const;
    int model() const { return model_; }
    double outputLevel() const { return meter_.level; }
    double sagDb() const { return sag_.value; }
    double lamp() const { return lamp_.value; }
    // A tube's glow as drawn (0..1) and where it is heading: 0..2 the preamp's V1..V3, 3 the power tube.
    Q_INVOKABLE double glow(int tube) const;
    Q_INVOKABLE double glowTarget(int tube) const;

    // How brightly a stage driven to `db` (its peak against its clipping point) glows.
    static double glowFor(double db);
    // The pilot lamp's brightness for an output at `outputDb` with the supply sagged by `sagDb`.
    static double lampFor(double outputDb, double sagDb);

Q_SIGNALS:
    void rectsChanged();
    void modelColorChanged();
    void modelChanged();
    void levelsChanged();

protected:
    void sync() override;
    void refreshDisplays() override;
    void paint(SgPainter& painter) override;

private:
    void paintTubes(SgPainter& p) const;
    void paintBloom(SgPainter& p, const QRectF& body, double glow) const;
    void paintTube(SgPainter& p, const QRectF& body, double glow, double blue) const;
    void paintJewel(SgPainter& p) const;

    QRectF tubeRect_, jewelRect_, meterRect_;
    int model_ = 0;
    bool synced_ = false;  // the first sync snaps to the model's colour
    QColor colorFrom_, colorTo_;
    Eased colorMix_;  // 0..1 from colorFrom_ to colorTo_

    // The latest display values (held through ticks that read none).
    std::array<double, kTubes> driveDb_ = {kFloorDb, kFloorDb, kFloorDb, kFloorDb};
    double sagRead_ = 0.0;
    double outputDb_ = kFloorDb;
    QElapsedTimer clock_;     // since the last tick
    QElapsedTimer lastRead_;  // since display values last came

    std::array<Eased, kTubes> glow_;
    Eased sag_;   // dB
    Eased lamp_;  // 0..1
    MeterBallistics meter_;
};

}  // namespace sub::ui
