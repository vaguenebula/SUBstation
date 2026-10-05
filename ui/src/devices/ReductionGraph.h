#pragma once

// The Compressor's graph: the gain reduction over the last kHistory display
// values (about 1.3 s at 48 kHz), growing downward to 24 dB and filled with a
// gradient; an In meter of what keys it, the threshold marked by an accent
// notch; an Out meter (both -60 to 0 dBFS); and the current reduction and the
// threshold in figures. It reads the device's displays "reduction", "input" and
// "output" as the meters update.

#include "devices/DeviceCanvas.h"

#include <QtQml/qqmlregistration.h>

#include <vector>

namespace sub::ui {

class ReductionGraph : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(double reduction READ reduction NOTIFY levelsChanged)  // dB, the latest
    Q_PROPERTY(double levelIn READ levelIn NOTIFY levelsChanged)      // dBFS
    Q_PROPERTY(double levelOut READ levelOut NOTIFY levelsChanged)

public:
    static constexpr int kWidth = 232;
    static constexpr int kMinimumHeight = 108;
    static constexpr int kHistory = 240;  // 256 samples each: about 1.3 s at 48 kHz
    static constexpr double kReductionRangeDb = 24.0;
    static constexpr double kMeterFloorDb = -60.0;
    static constexpr double kMeterWidth = 8.0;

    explicit ReductionGraph(QQuickItem* parent = nullptr);

    double reduction() const { return history_.back(); }
    double levelIn() const { return levelIn_; }
    double levelOut() const { return levelOut_; }
    const std::vector<float>& history() const { return history_; }

    // What the displays reported since: the reduction history moves on, the meters show the latest.
    void add(const std::vector<float>& reduction, const std::vector<float>& levelIn,
             const std::vector<float>& levelOut);

Q_SIGNALS:
    void levelsChanged();

protected:
    void sync() override;
    void refreshDisplays() override;
    void paint(SgPainter& painter) override;

private:
    std::vector<float> history_;
    double levelIn_ = kMeterFloorDb;
    double levelOut_ = kMeterFloorDb;
    double threshold_ = -18.0;
};

}  // namespace sub::ui
