#pragma once

// The Reverb's early reflections: Spin's X-Y pad (across for its rate, 0.07 to
// 1.3 Hz on a log axis; up and down for its amount; one undo step per drag,
// Shift finely) over the reflections themselves, drawn as particles: one per
// reflection the Density plays (sub::app::reverbEarlyTaps), placed across by
// where it sits in the stereo field (times Stereo) and down by when it comes
// (the earliest at the top), as big as it is loud (Shape). The diffuse tail's
// onset after the input (Predelay, Shape, Size) is read out at the top right.
//
// It moves with the audio, in advance() (from refreshDisplays(), about 60
// times a second): while the reflections sound (and for a moment after Spin
// is changed) the particles orbit as Spin swings them (their pans from the
// engine's own law, sub::app::reverbSpinPan, at the phase the device last
// published), trailing their last positions, and they light up, fading as the
// reflections do; in silence they settle at rest. Spin's amount eases in and
// out as it is switched. It repaints only while a particle moves or the light
// fades: nothing while nothing plays.

#include "devices/DeviceCanvas.h"
#include "devices/EditorPaint.h"

#include <QElapsedTimer>
#include <QList>
#include <QPointF>
#include <QtQml/qqmlregistration.h>

#include <array>

namespace sub::ui {

class ReverbSpinPad : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(double phase READ phase NOTIFY levelsChanged)  // Spin's latest phase (0..1), -1 while still
    Q_PROPERTY(double flash READ flash NOTIFY levelsChanged)  // 0..1: the reflections' light, eased
    Q_PROPERTY(bool animating READ animating NOTIFY levelsChanged)

public:
    static constexpr int kWidth = 96;
    static constexpr int kMinimumHeight = 56;
    static constexpr double kRateMin = 0.07;  // Hz: ER Spin Rate's range, across
    static constexpr double kRateMax = 1.3;
    static constexpr int kTaps = 12;
    static constexpr int kTrail = 6;  // positions kept per particle

    explicit ReverbSpinPad(QQuickItem* parent = nullptr);

    double phase() const { return phase_; }
    double flash() const { return flash_.value; }
    bool animating() const { return animating_; }
    // Where each reflection's particle is now (those the Density doesn't play are not drawn).
    QList<QPointF> particles() const;
    // Whether reflection k is drawn (the Density plays it).
    bool particleShown(int k) const { return k >= 0 && k < kTaps && radius_[std::size_t(k)] > 0.0; }
    // Spin's swing as drawn (0..1): its amount, eased as Spin is switched, while the reflections sound.
    double amountShown() const { return amount_.value * presence_.value; }

    // Pad coordinates: the plot (the item less 1 px), the handle's area in it (6 px less all round).
    QRectF plot() const;
    QRectF inner() const;
    Q_INVOKABLE double xOfRate(double hz) const;
    Q_INVOKABLE double rateAt(double x) const;
    Q_INVOKABLE double yOfAmount(double percent) const;
    Q_INVOKABLE double amountAt(double y) const;
    Q_INVOKABLE QPointF handle() const;

    // One tick of animation, `seconds` after the last.
    Q_INVOKABLE void advance(double seconds);

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
    QPointF positionOf(int k) const;
    void place();
    void dragTo(const QPointF& pos, Qt::KeyboardModifiers modifiers);

    // The parameters, as sync() read them.
    bool spin_ = true;
    double rate_ = 0.3, amountPercent_ = 25.0, stereo_ = 100.0;
    bool synced_ = false;
    // The reflections at rest: each one's row (0 the first's time .. 1 the last's), radius (0: not played)
    // and loudness (0..1, of the loudest). (Their pans are the engine's: reverbSpinPan.)
    std::array<double, kTaps> row_{}, radius_{}, loudness_{};
    QString onsetText_;

    // The motion.
    double phase_ = -1.0;      // the latest published (-1: still)
    double drawPhase_ = 0.0;   // the one drawn (the last while still)
    Eased amount_;             // Spin's amount 0..1 as drawn (0 while off)
    Eased presence_;           // 1 while the reflections sound (the swing drawn), 0 at rest in silence
    double linger_ = 0.0;      // s the swing still shows after Spin's settings changed
    Eased flash_;
    std::array<QPointF, kTaps> at_{}, painted_{};  // now, and as last painted
    std::array<std::array<QPointF, kTrail>, kTaps> trail_{};  // the latest first
    QElapsedTimer clock_;
    bool animating_ = false;

    // The drag (as the filter pad's).
    QString gesture_;
    bool fine_ = false;
    QPointF anchor_, anchorHandle_;
};

}  // namespace sub::ui
