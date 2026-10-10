#pragma once

// The Erosion's display, after Live 12.4's: an X-Y field on a 20 Hz..20 kHz log
// axis whose dot is the modulation, across for its Frequency, up for its Amount.
// Behind it the input's spectrum (filled, grey) and the output's (a line over
// it: the fizz Erosion adds at the top); on it the noise's band, peaking at the
// dot, as wide as Filter Width, worked out at the engine's rate from the
// engine's own filter (sub::app::erosionBandMagnitude), so the band drawn is the
// one that plays; and the sine as a spike at the dot. Noise Blend crossfades the
// two. While audio is eroded (the display `erosion`) the band's fill shimmers
// like heat haze, in two layers that part as Stereo widens, the dot gets a halo
// and the sine's spike trembles; in silence it all comes to rest and stops
// repainting.
//
// Drag the dot (it jumps to the press) for the Frequency and the Amount; Shift
// starts from where the dot is and moves it finely; Alt (Option) starts from the
// dot too and drags up and down for the Filter Width (doubling every
// kWidthPixels); the wheel sets the Filter Width, Ctrl+wheel finely (Shift+wheel
// is the device chain's: it scrolls the chain). One undo step per drag, and per
// burst of wheel notches.

#include "analysis/Spectrum.h"
#include "audio/ErosionResponse.h"
#include "devices/DeviceCanvas.h"
#include "devices/EditorPaint.h"
#include "input/GestureKey.h"

#include <QtQml/qqmlregistration.h>

#include <random>
#include <utility>
#include <vector>

namespace sub::ui {

class ErosionGraph : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool dragging READ dragging NOTIFY draggingChanged)  // from a press to its release (the tooltip waits)
    // Noise Blend's weights as the engine has them (sub::app::erosionBlendWeights): the editor's glyphs and
    // Width's dimming follow them.
    Q_PROPERTY(double sineWeight READ sineWeight NOTIFY weightsChanged)
    Q_PROPERTY(double noiseWeight READ noiseWeight NOTIFY weightsChanged)

public:
    static constexpr int kWidth = 300;
    static constexpr int kMinimumHeight = 100;
    static constexpr double kLow = 20.0;  // Hz across the graph
    static constexpr double kHigh = 20000.0;
    static constexpr int kColumn = 2;                  // px per column of the shimmer and the spectra
    static constexpr double kWidthPixels = 40.0;       // Alt-dragged up this far, the Filter Width doubles
    static constexpr double kWheelOctaves = 0.25;      // a wheel notch multiplies it by 2^0.25 (Ctrl: 2^(1/16))
    static constexpr double kFine = 0.15;              // Shift-dragged, the dot moves this share of the mouse's way
    static constexpr double kTopStrip = 14.0;          // over the plot: the source and the readout
    static constexpr double kDotRadius = 5.0;          // the dot's ring (2 px wide),
    static constexpr double kHaloGrowth = 6.0;         // and how far its halo reaches past it, eroding hard
    // The activity (the graph's and the scope's): the `erosion` display from kActivityFloorDb (0) over
    // kActivitySpanDb (1), eased towards that with these time constants: up in a few ticks, down over half a
    // second.
    static constexpr double kActivityFloorDb = -60.0;
    static constexpr double kActivitySpanDb = 48.0;
    static constexpr double kActivityRiseSeconds = 0.037;
    static constexpr double kActivityFallSeconds = 0.19;
    // What a refresh counts of the `erosion` display (the graph's and the scope's): the newest values, those
    // covering this long (about two refreshes; DeviceCanvas::readRecent), not a backlog's.
    static constexpr double kRecentSeconds = 0.035;

    // `activity` moved `dtSeconds` on towards what `erosionDb` says; whether it moved.
    static bool easeActivity(Eased& activity, double erosionDb, double dtSeconds);

    explicit ErosionGraph(QQuickItem* parent = nullptr);

    QRectF plot() const;
    // Where the dot's centre goes up and down: the plot less the dot's reach, its halo's at the top (clear of
    // the strip's texts) and its ring's at the bottom (inside the well).
    QRectF travel() const;
    LogAxis frequencyAxis() const;
    double xOf(double freq) const;
    double freqAt(double x) const;
    double yOfAmount(double amount) const;
    double amountAt(double y) const;  // held to 0..100
    // Where the dot is: the frequency the modulator plays (held to the axis), and the Amount.
    QPointF dot() const;

    // The band's outline: frequencies rising, one per pixel across the plot with the tuned one among
    // them (a band narrower than a pixel still peaks at the dot), and the band's magnitude at each (0..1,
    // the engine's filter).
    const std::vector<double>& frequencies() const { return frequencies_; }
    const std::vector<double>& magnitudes() const { return magnitudes_; }
    // The kColumn-wide columns (the shimmer, the spectra): each one's centre, and the band's magnitude
    // over it (the most of its edges and centre; 1 in the column the tuned frequency falls in).
    const std::vector<double>& columnFrequencies() const { return columnFrequencies_; }
    const std::vector<double>& columnMagnitudes() const { return columnMagnitudes_; }
    double tunedFrequency() const { return tuned_; }
    double excursionMs() const { return excursionMs_; }
    double sineWeight() const { return sineWeight_; }
    double noiseWeight() const { return noiseWeight_; }
    // How much the sound is being eroded (0..1, eased): what the shimmer, the halo and the spike's trembling follow.
    double activity() const { return activity_.value; }
    // How much the last refresh says the device erodes now (dB; sub::app::kErosionFloorDb without any): the
    // most of the newest `erosion` values it read (kRecentSeconds of them, sub::app::erosionPeakDb).
    double erosionDb() const { return erosionDb_; }
    // Whether the input's spectrum shows anything above its floor.
    bool spectrumLive() const { return !inColumns_.empty(); }
    bool dragging() const { return drag_ != Drag::None; }
    // Whether the last refresh moved anything (and so repainted).
    bool animating() const { return animating_; }

Q_SIGNALS:
    void draggingChanged();
    void weightsChanged();

protected:
    void sync() override;
    void refreshDisplays() override;
    void paint(SgPainter& painter) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void wheelEvent(QWheelEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    enum class Drag { None, Point, Width };

    void updateCurve();  // the outline, the columns, the edges, at the engine's rate
    void apply();        // the drag's parameters, from where it has the dot
    void endDrag();
    void setAltHeld(bool held);
    void shimmerStep(double dtSeconds);  // the band's fill on a tick: its knots glide on, the columns follow

    sub::app::analysis::EqAnalyzer analyzer_;  // the input's and the output's spectra
    std::vector<double> frequencies_, magnitudes_, columnFrequencies_, columnMagnitudes_;
    std::vector<double> inColumns_, outColumns_;  // dB per column (empty while not live)
    struct Knot {
        float value = 0.5f, target = 0.5f;
    };
    std::vector<Knot> knotsL_, knotsR_;           // the shimmer's, gliding to random heights (0..1)
    std::vector<float> shimmerL_, shimmerR_;      // per column, between the knots: what is drawn
    std::minstd_rand random_{1};                  // (seeded: screenshots repeat)
    Eased activity_;
    double erosionDb_ = sub::app::kErosionFloorDb;
    double sinePhase_ = 0.0;  // the sine spike's wave (radians)
    double freq_ = 1000.0, width_ = 2.5, amount_ = 25.0, blend_ = 100.0, stereo_ = 0.0;
    double tuned_ = 1000.0, excursionMs_ = 0.0, sineWeight_ = 0.0, noiseWeight_ = 1.0;
    std::pair<double, double> edges_{0.0, 0.0};
    bool altHeld_ = false, animating_ = false;
    Drag drag_ = Drag::None;
    QString gesture_;            // the drag's merge key
    WheelGesture wheelGesture_;  // the wheel's: one per burst of notches
    // Where the drag has the dot (not held to the plot: dragged out and back, it comes back under the
    // mouse), and where the mouse was.
    QPointF virtual_, last_;
    // The press: where the drag had the dot, the values then, and whether it started from the dot (Shift,
    // Alt) rather than jumping to the mouse.
    QPointF pressedAt_;
    double pressedFreq_ = 1000.0, pressedAmount_ = 25.0;
    bool fromDot_ = false;
    double pressedWidth_ = 2.5, pressedY_ = 0.0;  // an Alt drag's start (a wheel notch in it starts it again)
    QMetaObject::Connection bridgeConnection_;    // the bridge's deviceChanged: a new sample rate
};

}  // namespace sub::ui
