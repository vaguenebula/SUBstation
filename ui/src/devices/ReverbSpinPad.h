#pragma once

// The Reverb's early reflections: Spin's X-Y pad (across for its rate, 0.07 to
// 1.3 Hz on a log axis; up and down for its amount; one undo step per drag,
// Shift finely) over the reflections themselves, drawn as particles: one per
// reflection the Density plays (sub::app::reverbEarlyTaps), placed across by
// where it sits in the stereo field (times Stereo's width) and down by when it comes
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
// fades: nothing while nothing plays. (The swing's easing runs, painted, until
// no particle is 0.2 px from where it ends, and then ends there: it never rests
// with a way still to go, creeping on below a repaint to paint again later.)

#include "devices/DeviceCanvas.h"
#include "devices/EditorPaint.h"

#include "audio/ReverbResponse.h"

#include <QFont>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QString>
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
    // At least, and as wide as its captions need (the editor gives it the width of the boxes under it).
    static constexpr int kMinimumWidth = 96;
    static constexpr int kMinimumHeight = 56;
    static constexpr double kCaptionInset = 3.0;  // px: the captions in from the plot's sides,
    static constexpr double kCaptionGap = 8.0;    // and at least this far apart
    static constexpr double kRateMin = sub::app::kReverbMinSpinRate;  // Hz: ER Spin Rate's range, across
    static constexpr double kRateMax = sub::app::kReverbMaxSpinRate;
    static constexpr int kTaps = sub::app::kReverbTaps;
    static constexpr int kTrail = 6;  // positions kept per particle

    explicit ReverbSpinPad(QQuickItem* parent = nullptr);

    double phase() const { return phase_; }
    double flash() const { return flash_.value; }
    bool animating() const { return animating_; }
    // Where each reflection's particle is now (those the Density doesn't play are not drawn), and its
    // trail (its last positions, the latest first).
    QList<QPointF> particles() const;
    QList<QPointF> trail(int k) const;
    // Whether reflection k is drawn (the Density plays it).
    bool particleShown(int k) const { return k >= 0 && k < kTaps && radius_[std::size_t(k)] > 0.0; }
    // How far reflection k's particle can reach up or down from where it sits at rest: its radius with its
    // glow lit up, and Spin's bob at its most (0 for one not drawn).
    double particleReach(int k) const;
    // Spin's swing as drawn (0..1): its amount, eased as Spin is switched, while the reflections sound.
    double amountShown() const { return amount_.value * presence_.value; }

    // The captions over the plot, in captionFont(): "Early" at the left, the tail's onset after the input at the
    // right ("tail +53 ms"), each rect as wide as its text (the onset's cut short kCaptionGap after "Early", should
    // a pad ever be narrower than its implicit width: it is drawn clipped there). The implicit width is what the
    // captions need with the widest onset (three of the font's widest figures: it stays under a second).
    static QFont captionFont();
    QString onsetText() const { return onsetText_; }
    QRectF earlyRect() const;
    QRectF onsetRect() const;

    // Pad coordinates: the plot (the item less 1 px), the handle's area in it (6 px less all round, and
    // under the captions' 13 px strip, so the handle never covers them).
    QRectF plot() const;
    QRectF inner() const;
    Q_INVOKABLE double xOfRate(double hz) const;
    Q_INVOKABLE double rateAt(double x) const;
    Q_INVOKABLE double yOfAmount(double percent) const;
    Q_INVOKABLE double amountAt(double y) const;
    Q_INVOKABLE QPointF handle() const;

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
    // One tick of animation, `seconds` after the last.
    void advance(double seconds);
    // Where reflection k's particle is drawn, Spin's swing at `swing` (its amount as drawn, 0..1).
    QPointF positionOf(int k, double swing) const;
    void place();
    // Whether every particle drawn is within kMoved (0.2 px) of where the swing's easing leaves it.
    bool swingSettled() const;
    void dragTo(const QPointF& pos, Qt::KeyboardModifiers modifiers);

    // The parameters, as sync() read them.
    bool spin_ = true;
    double rate_ = 0.3, amountPercent_ = 25.0;
    double width_ = 100.0 / 120.0;  // Stereo's width on the wet (sub::app::reverbStereoWidth)
    bool synced_ = false;
    // The reflections at rest: each one's row (0 the first's time .. 1 the last's), radius (0: not played)
    // and loudness (0..1, of the loudest). (Their pans are the engine's: reverbSpinPan.)
    std::array<double, kTaps> row_{}, radius_{}, loudness_{};
    QString onsetText_;

    // The motion.
    double phase_ = -1.0;      // the latest published (-1: still)
    double drawPhase_ = 0.0;   // the one drawn (the last while still; run on through ticks without one)
    double stale_ = 0.0;       // s since a tick last brought a value
    Eased amount_;             // Spin's amount 0..1 as drawn (0 while off)
    Eased presence_;           // 1 while the reflections sound (the swing drawn), 0 at rest in silence
    double linger_ = 0.0;      // s the swing still shows after Spin's settings changed
    Eased flash_;
    std::array<QPointF, kTaps> at_{}, painted_{};  // now, and as last painted
    std::array<std::array<QPointF, kTrail>, kTaps> trail_{};  // the latest first
    bool animating_ = false;

    // The drag (as the filter pad's).
    QString gesture_;
    bool fine_ = false;
    QPointF anchor_, anchorHandle_;
};

}  // namespace sub::ui
