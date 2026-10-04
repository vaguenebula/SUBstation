#pragma once

// The oscilloscope of the master output, as in FL Studio's tool bar
// (oscilloscope.py); Oscilloscope.qml wraps it with its tooltip.
//
// Every kUpdateMs its timer reads the feed's `scopeWritten` (a counter); only
// when it moved does it fetch scopeSamples(2 * kWindow) (neither may block).
// trigger() finds the last rising zero crossing that still leaves a full
// kWindow after it, so steady tones stand still; with none, it shows the newest
// window. trace() turns the samples into one column per pixel (the column's
// highest then lowest sample), built when the samples change, not on every
// paint. When no new audio comes, the trace shrinks by kFadePerUpdate each
// update (0.64 per 33 ms) and stops repainting once flat. It skips its work
// while hidden.
//
// kWindow is a fixed number of samples (about 21 ms at 48 kHz) because the
// engine's sample rate is behind its edit lock, which the scope shouldn't take
// 60 times a second. The glow is drawn without antialiasing (it is soft
// anyway); only the thin line has it.
//
// The feed is any QObject with
//   Q_PROPERTY(quint64 scopeWritten ...)                  samples written so far
//   Q_INVOKABLE QList<float> scopeSamples(int frames)     the newest `frames` (or fewer)
// (the app layer's engine bridge provides it).

#include "sg/SgCanvas.h"

#include <QList>
#include <QPointF>
#include <QPointer>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <vector>

namespace sub::ui {

class OscilloscopeItem : public SgCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject* feed READ feed WRITE setFeed NOTIFY feedChanged)

public:
    static constexpr int kWindow = 1024;  // samples shown; twice as many are read to find a trigger in
    static constexpr int kUpdateMs = 16;  // ~60 fps
    static constexpr double kFadePerUpdate = 0.8;  // while no audio comes (as fast as 0.64 per 33 ms)
    static constexpr double kSilent = 1e-4;        // below this the trace counts as flat and stops repainting

    explicit OscilloscopeItem(QQuickItem* parent = nullptr);

    QObject* feed() const { return feed_; }
    void setFeed(QObject* feed);

    // Where the shown window starts: the last rising zero crossing that leaves a full window after it.
    static int trigger(const float* samples, int count, int window);
    // One column per pixel from `left`: a line through each column's highest and
    // then lowest sample (the lowest only where the column spans a pixel or more).
    static std::vector<QPointF> trace(const float* samples, int count, double left, int columns, double mid,
                                      double half);

    // What it shows now (for tests).
    const std::vector<float>& samples() const { return samples_; }
    const std::vector<QPointF>& tracePoints() const { return trace_; }

    // One update (the timer's): reads the feed if it moved, else fades.
    Q_INVOKABLE void poll();

Q_SIGNALS:
    void feedChanged();

protected:
    void paint(SgPainter& painter) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    void rebuild();

    QPointer<QObject> feed_;
    QTimer timer_;
    std::vector<float> samples_;
    std::vector<QPointF> trace_;
    quint64 written_ = 0;
    bool hasWritten_ = false;
};

}  // namespace sub::ui
