#pragma once

// The Erosion's modulation, left against right, as a goniometer in a small
// round well (mid up, side across): the latest kTrailSeconds of the modulators
// the device publishes every sample (the displays `mod_l` and `mod_r`), drawn as
// a trace fading with age, over a soft cloud as wide and tall as the
// modulation's side and mid (two RMS out, eased). Mono (Stereo 0 %) it is an
// upright line; as Stereo widens it opens into an ellipse, then a round cloud
// (noise) or a circle (the sine, its sides a quarter cycle apart). Its colour goes from the sine's blue
// to the noise's orange with Noise Blend; it is dim at Amount 0 (the modulators
// run, but nothing is applied) and brightens while the sound is eroded (the
// display `erosion`, eased as the graph's).
//
// It repaints only while the sound is eroded, or for a moment after a change
// (the trace catching up with it): in silence it holds its last trace, still.

#include "devices/DeviceCanvas.h"
#include "devices/EditorPaint.h"

#include <QtQml/qqmlregistration.h>

#include <utility>
#include <vector>

namespace sub::ui {

class ErosionScope : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT

public:
    static constexpr int kSize = 46;                // its implicit size (the editor may give it less, to fit)
    static constexpr int kRing = 4096;              // modulator pairs kept (85 ms at 48 kHz): spread() is over these
    static constexpr double kTrailSeconds = 0.005;  // the latest this long are traced
    // A pair whose (mid, side) is d from the centre is drawn tanh(kGain d) of the way to the rim.
    static constexpr double kGain = 1.1;
    static constexpr int kSettleTicks = 20;         // refreshes it repaints for after a change (a third of a second)

    explicit ErosionScope(QQuickItem* parent = nullptr);

    // The pairs held (up to kRing).
    int pointCount() const { return count_; }
    // How wide the modulation is: the side's RMS over the mid's, over the pairs held (0: a line, about
    // 1: a round cloud or a circle).
    double spread() const;
    // 0..1, eased: the trace's brightness.
    double activity() const { return activity_.value; }

protected:
    void sync() override;
    void refreshDisplays() override;
    void paint(SgPainter& painter) override;

private:
    // Values of one modulator not yet paired: the two are read one after the other, so the second read may
    // hold values published between them, whose partners come with the next refresh.
    struct Unpaired {
        qint64 at = 0;  // the absolute index of the first
        std::vector<float> values;
    };
    void take(Unpaired& unpaired, std::pair<qint64, std::vector<float>> read);

    std::vector<float> left_, right_;  // rings of kRing
    int head_ = 0;                     // where the next pair goes
    int count_ = 0;
    Unpaired unpairedLeft_, unpairedRight_;
    int trail_ = 2;  // pairs traced: kTrailSeconds at the engine's rate (worked out in refreshDisplays())
    double amount_ = 25.0, noiseWeight_ = 1.0;
    Eased activity_;
    Eased midRms_, sideRms_;  // the cloud's size
    int settle_ = 0;  // refreshes left to repaint for after a change
};

}  // namespace sub::ui
