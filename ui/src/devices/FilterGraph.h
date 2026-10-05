#pragma once

// The Delay's filter: the band-pass's response on a 20 Hz..20 kHz log axis, its
// dot dragged across for the frequency and up and down for the width (one undo
// step per drag), over a spectrum of the device's display "input" (a 4096-point
// Hann FFT of the latest samples, falling 1 dB a refresh).

#include "devices/DeviceCanvas.h"

#include "analysis/Spectrum.h"

#include <QtQml/qqmlregistration.h>

namespace sub::ui {

class FilterGraph : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT

public:
    static constexpr int kWidth = 220;
    static constexpr int kMinimumHeight = 60;
    static constexpr double kLow = 20.0;  // Hz across the graph
    static constexpr double kHigh = 20000.0;
    static constexpr double kFloorDb = -30.0;  // the curve's bottom
    static constexpr double kWidthMin = 0.5;   // octaves
    static constexpr double kWidthMax = 9.0;

    explicit FilterGraph(QQuickItem* parent = nullptr);

    // The filter's gain in dB at `freq`: a 12 dB/octave high-pass and low-pass,
    // `width` octaves apart around `center`, as the engine has them.
    static double response(double freq, double center, double width);

    // The input's samples since the last refresh (the spectrum falls back without any).
    void addSamples(const std::vector<float>& samples, double sampleRate);
    const sub::app::analysis::FallingSpectrum& spectrum() const { return spectrum_; }

    QRectF plot() const;
    double xOf(double freq) const;
    double freqAt(double x) const;

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
    void updateColumns();
    double dotY(double width) const;
    double widthAt(double y) const;
    void dragTo(const QPointF& pos);

    sub::app::analysis::FallingSpectrum spectrum_;
    std::vector<double> columns_;  // the spectrum under each column, worked out for paint()
    bool on_ = true;
    double center_ = 1000.0;
    double width_ = 8.0;
    QString gesture_;  // the drag's merge key ("": none)
};

}  // namespace sub::ui
