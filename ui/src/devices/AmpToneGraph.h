#pragma once

// The Amp's tone section as it sounds: the model's tone stack (its make-up
// included) and Presence, in dB on a 30 Hz..16 kHz log axis, worked out at the
// engine's rate by the engine's own design (sub::app::ampToneResponseDb), so the
// curve is the sound. The stack's controls interact as a real amp's do: turning
// one moves the curve where the others act too.
//
// A handle per tone control sits on the curve at the frequency it acts most on
// (B 100 Hz, M 700 Hz, T 3 kHz, P 6 kHz): drag it up and down to set its dial
// (kPixelsPerStep a step; Shift finely), one undo step per drag; double-click
// it to put it back to 5. A new model's curve morphs from the old one's; dials
// and drags move it at once.

#include "devices/DeviceCanvas.h"
#include "devices/EditorPaint.h"

#include <QElapsedTimer>
#include <QPointF>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <array>
#include <vector>

namespace sub::ui {

class AmpToneGraph : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool morphing READ morphing NOTIFY curveChanged)
    Q_PROPERTY(int hovered READ hovered NOTIFY handlesChanged)    // the handle under the mouse (-1: none)
    Q_PROPERTY(int dragging READ dragging NOTIFY handlesChanged)  // the handle being dragged (-1: none)

public:
    static constexpr int kWidth = 368;
    static constexpr int kMinimumHeight = 40;
    static constexpr double kLow = 30.0;  // Hz across the graph
    static constexpr double kHigh = 16000.0;
    // Its bottom and top: every model's curve with its dials anywhere but near 0 (the stacks cut up to
    // 40 dB there, along the bottom then), Presence's +9 dB with Treble's.
    static constexpr double kFloorDb = -24.0;
    static constexpr double kCeilingDb = 12.0;
    static constexpr double kGutter = 20.0;  // at the right, the dB figures
    static constexpr double kPixelsPerStep = 8.0;  // dragged up this far, a dial step of 1
    static constexpr double kFine = 0.2;           // with Shift
    static constexpr double kHitPixels = 14.0;     // a press this near a handle (across) takes it
    static constexpr double kRadius = 4.5;         // a handle's
    static constexpr double kHoverRadius = 6.5;    // under the mouse or dragged
    static constexpr int kHandles = 4;

    struct Handle {
        const char* id;
        double frequency;
        const char* letter;
        const char* name;
    };
    static constexpr std::array<Handle, kHandles> kHandleList = {{{"bass", 100.0, "B", "Bass"},
                                                                  {"middle", 700.0, "M", "Middle"},
                                                                  {"treble", 3000.0, "T", "Treble"},
                                                                  {"presence", 6000.0, "P", "Presence"}}};

    explicit AmpToneGraph(QQuickItem* parent = nullptr);

    // The curve: frequencies (Hz, rising: one per column, and the handles'), what it is heading
    // for, and as drawn (dB; the same but while a new model's morphs in).
    const std::vector<double>& frequencies() const { return frequencies_; }
    const std::vector<double>& targetDb() const { return target_; }
    const std::vector<double>& shownDb() const { return shown_; }
    bool morphing() const { return morph_.value < 1.0; }
    int hovered() const { return hovered_; }
    int dragging() const { return dragging_; }
    // A handle's place (pixels): at its frequency, on the curve as drawn.
    Q_INVOKABLE QPointF handlePos(int i) const;
    // The handle a press at `pos` takes (-1: none).
    Q_INVOKABLE int handleAt(QPointF pos) const;
    // A handle's radius as drawn.
    Q_INVOKABLE double handleRadius(int i) const;

    QRectF plot() const;
    LogAxis frequencyAxis() const;
    double xOf(double hz) const;
    double yOf(double db) const;

Q_SIGNALS:
    void curveChanged();
    void handlesChanged();

protected:
    void sync() override;
    void refreshDisplays() override;
    void paint(SgPainter& painter) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    void updateCurve();
    void setHovered(int handle);
    void endDrag();
    double shownAt(double hz) const;  // the curve as drawn at a handle's frequency
    bool stepHandles(double dt);

    int model_ = -1;  // -1: not synced yet (the first curve shows at once)
    double dials_[kHandles] = {5.0, 5.0, 5.0, 5.0};
    std::vector<double> frequencies_;
    std::vector<double> target_;
    std::vector<double> from_;
    std::vector<double> shown_;
    Eased morph_;  // 0..1 from from_ to target_
    std::array<Eased, kHandles> radius_;
    int hovered_ = -1;
    int dragging_ = -1;
    QString gesture_;  // the drag's merge key ("": none)
    double pressedY_ = 0.0;
    double pressedValue_ = 5.0;
    QElapsedTimer clock_;
    QMetaObject::Connection bridgeConnection_;
};

}  // namespace sub::ui
