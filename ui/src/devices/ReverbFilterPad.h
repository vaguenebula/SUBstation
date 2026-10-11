#pragma once

// The Reverb's input filter: an X-Y pad over the spectrum of what goes into the
// reverb (the display "signal", the input summed to mono, falling 1 dB a
// refresh), with the band's response on a 20 Hz..20 kHz log axis worked out
// from the engine's own filter (sub::app::reverbInputFilterDb, so the curve is
// what the reverb hears). Its dot is the band: across for its centre (In Filter
// Freq), up and down for its width (0.5 octaves at the bottom, 9 at the top),
// one undo step per drag, Shift finely.
//
// It moves in advance() (from refreshDisplays(), about 60 times a second): the
// spectrum falls, the passband and the dot's halo glow with the input's level
// (rising at once, fading over a quarter of a second), and a switch of Lo Cut or
// Hi Cut eases the curve to its new shape (about 120 ms) instead of jumping.
// Dragged values never animate. It repaints only while something moves.

#include "devices/DeviceCanvas.h"
#include "devices/EditorPaint.h"

#include "analysis/Spectrum.h"
#include "audio/ReverbResponse.h"

#include <QtQml/qqmlregistration.h>

#include <vector>

namespace sub::ui {

class ReverbFilterPad : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(double inputLevel READ inputLevel NOTIFY levelsChanged)  // dB: the newest "input" (floor: none lately)
    Q_PROPERTY(double glow READ glow NOTIFY levelsChanged)              // 0..1, eased
    Q_PROPERTY(bool animating READ animating NOTIFY levelsChanged)      // the last tick asked for a repaint

public:
    static constexpr int kMinimumWidth = 108;  // (the editor gives it the width of the boxes under it)
    static constexpr int kMinimumHeight = 56;
    static constexpr double kLow = 20.0;  // Hz across the pad
    static constexpr double kHigh = 20000.0;
    static constexpr double kTopDb = 6.0;  // the response's axis
    static constexpr double kFloorDb = -30.0;
    static constexpr double kWidthMin = sub::app::kReverbMinInWidth;  // octaves, the pad's bottom
    static constexpr double kWidthMax = sub::app::kReverbMaxInWidth;  // and its top
    static constexpr double kFreqMin = sub::app::kReverbMinInFreq;    // In Filter Freq's range
    static constexpr double kFreqMax = sub::app::kReverbMaxInFreq;

    explicit ReverbFilterPad(QQuickItem* parent = nullptr);

    double inputLevel() const { return inputLevel_; }
    double glow() const { return glow_.value; }
    bool animating() const { return animating_; }

    // The response as computed (one frequency per column, its gain in dB) and as drawn now (while a
    // switch's change eases).
    const std::vector<double>& curveFrequencies() const { return frequencies_; }
    const std::vector<double>& curveDb() const { return db_; }
    const std::vector<double>& curveShown() const { return shown_; }
    const sub::app::analysis::FallingSpectrum& spectrum() const { return spectrum_; }

    // Pad coordinates: the plot (the item less 1 px), the dot's area in it (the width's axis: under the
    // caption's 13 px strip and 6 px more, and 6 px above the bottom, so the dot never covers the caption),
    // the axes.
    QRectF plot() const;
    QRectF inner() const;
    Q_INVOKABLE double xOf(double freq) const;
    Q_INVOKABLE double freqAt(double x) const;
    Q_INVOKABLE double yOfWidth(double octaves) const;
    Q_INVOKABLE double widthAt(double y) const;
    double yOfDb(double db) const;
    Q_INVOKABLE QPointF dot() const;

Q_SIGNALS:
    void levelsChanged();

protected:
    void sync() override;
    void refreshDisplays() override;
    void paint(SgPainter& painter) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    // One tick of animation, `seconds` after the last (refreshDisplays calls it with tickSeconds()).
    void advance(double seconds);
    void updateCurve();
    void updateShown();
    void updateColumns();
    void dragTo(const QPointF& pos, Qt::KeyboardModifiers modifiers);

    // The parameters, as sync() read them.
    bool loCut_ = true, hiCut_ = true;
    double freq_ = 830.0, width_ = 7.5;
    bool synced_ = false;

    // The response: computed (db_), drawn (shown_), and where a switch's easing started (from_).
    std::vector<double> frequencies_, db_, shown_, from_;
    Eased switch_;  // 0 -> 1 from from_ to db_ (1: settled)

    // The input.
    sub::app::analysis::FallingSpectrum spectrum_;
    std::vector<double> columns_;  // the spectrum under each column
    bool spectrumChanged_ = false;
    double inputLevel_ = sub::app::kReverbMeterFloorDb;
    // The loudest of the last tick's newest "input" values (kept a moment through ticks without any), and the
    // seconds since a tick last brought one.
    double loudest_ = sub::app::kReverbMeterFloorDb;
    double stale_ = 0.0;
    Eased glow_;
    bool animating_ = false;
    QMetaObject::Connection bridgeConnection_;  // the bridge's deviceChanged: a new sample rate

    // The drag: its merge key ("": none), and where it is measured from (Shift moves a quarter as far from
    // where it was pressed: rebased when Shift goes down or up).
    QString gesture_;
    bool fine_ = false;
    QPointF anchor_, anchorDot_;
};

}  // namespace sub::ui
