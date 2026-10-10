#pragma once

// The Amp's transfer curve: what comes out (up) for each level going in
// (across, -1..1: 0 dBFS at the edges) with the settings as they are: for a
// 1 kHz tone of peak x, the output's highest value at x and its lowest at -x
// (sub::app::AmpTransferCurve: the engine's own stages, filters and voicing,
// the tone played until it has settled, so the curve is the sound: a tone's
// peaks land within 0.1 dB of it at the defaults, 0.6 dB with Gain, Presence
// and Volume at 10).
// Up is scaled to the curve's own reach (its largest output with no sag), since
// the models are level-matched far below full scale: the shape shows how hard
// and how lopsided it clips. A faint line is the clean gain (the curve's slope
// at 0) carried on, so the bend away from it is the distortion.
//
// The input's peaks ride on it: two dots at +-the peak (display input, with a
// meter's ballistics: up at once, falling 18 dB/s) and a short trail of where
// they were, over the part of the curve the signal uses, which glows. As the
// supply sags (display sag), the power stage's drive drops and the curve's
// shoulder breathes down with it (made again from the preamp's part kept, a
// fraction of the work). A display read counts what came since the last tick
// (at least its last 50 ms, AmpDisplays.h): a backlog (what came while the
// editor wasn't showing, seconds of it) is history. The curve is made again
// only when what it is made from changes (the settings, the rate, the width),
// not at every sync. No mouse.

#include "audio/AmpResponse.h"
#include "devices/DeviceCanvas.h"
#include "devices/EditorPaint.h"

#include <QList>

#include <QElapsedTimer>
#include <QPointF>
#include <QtQml/qqmlregistration.h>

#include <array>
#include <vector>

namespace sub::ui {

class AmpDriveGraph : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(double inputLevel READ inputLevel NOTIFY levelsChanged)  // dB, as drawn
    Q_PROPERTY(double sagDb READ sagDb NOTIFY levelsChanged)            // as the curve is drawn

public:
    static constexpr int kWidth = 140;
    static constexpr int kMinimumHeight = 40;
    static constexpr double kFloorDb = -90.0;
    static constexpr double kQuietSeconds = 0.3;  // with no display values this long, the dots fall
    static constexpr int kTrail = 8;              // ticks the trail remembers
    static constexpr double kReach = 0.86;        // the curve's largest output, of the half height

    explicit AmpDriveGraph(QQuickItem* parent = nullptr);

    double inputLevel() const { return input_.level; }
    double sagDb() const { return curveSag_; }
    // The dot on the right (pixels): at the input's peak, on the curve.
    QPointF dot() const;
    // The curve as drawn, in values: (input, output), input rising over -1..1.
    const std::vector<QPointF>& curve() const { return curve_; }
    // The transfer at input `x` with the settings and the sag as drawn.
    Q_INVOKABLE double outputAt(double x) const;
    // What the plot's top and bottom stand for (+-, in output values).
    double outputRange() const { return range_; }

    QRectF plot() const;
    double xOf(double v) const;
    double yOf(double v) const;

Q_SIGNALS:
    void curveChanged();
    void levelsChanged();

protected:
    void sync() override;
    void refreshDisplays() override;
    void paint(SgPainter& painter) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    void updateCurve();              // for the settings (the preamp's part too), if they changed
    void shapeCurve();               // for the sag as drawn
    double curveAt(double x) const;  // interpolated from curve_

    // What the curve is made from: the settings, the columns and the rate.
    struct CurveKey {
        int model = -1, columns = 0;
        double rate = 0.0;
        std::array<double, 6> dials = {};  // Gain, Bass, Middle, Treble, Presence, Volume
        bool operator==(const CurveKey&) const = default;
    };

    int model_ = 0;
    double gain_ = 5.0, bass_ = 5.0, middle_ = 5.0, treble_ = 5.0, presence_ = 5.0, volume_ = 5.0;
    CurveKey made_;  // what the curve was last made from (model -1: not yet)
    sub::app::AmpTransferCurve transfer_;
    QList<double> xs_;  // the curve's inputs
    std::vector<QPointF> curve_;
    double range_ = 1.0;   // output at the plot's top
    double slope_ = 1.0;   // the curve's slope at 0
    double curveSag_ = 0.0;  // the sag the curve was made with
    QMetaObject::Connection bridgeConnection_;

    double inputRead_ = kFloorDb;  // the latest input peak (held through ticks that read none)
    double sagRead_ = 0.0;
    QElapsedTimer clock_;
    QElapsedTimer lastRead_;
    MeterBallistics input_;
    Eased sag_;
    std::array<double, kTrail> trail_{};  // the last ticks' peaks (linear), newest first
};

}  // namespace sub::ui
