#include "pianoroll/RollTimeline.h"

#include "model/Numbers.h"
#include "model/Project.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QRectF>

#include <algorithm>
#include <array>
#include <cmath>

namespace sub::ui::roll {

double gridMinPixels(int level) {
    static constexpr std::array<double, 5> kPixels{6.0, 11.0, 20.0, 40.0, 80.0};
    return kPixels[static_cast<size_t>(std::clamp(level, -2, 2) + 2)];
}

app::TimeSignature Timeline::timeSignature() const {
    return project_ ? project_->timeSignature() : app::TimeSignature();
}

bool Timeline::setScrollBeats(double beats) {
    beats = std::max(0.0, beats);
    if (beats == scrollBeats_) return false;
    scrollBeats_ = beats;
    return true;
}

bool Timeline::setScrollY(double y) {
    const int value = std::max(0, std::min(maxScrollY_, static_cast<int>(y)));
    if (value == scrollY_) return false;
    scrollY_ = value;
    return true;
}

bool Timeline::zoomAt(double x, double factor) {
    const double anchor = xToBeat(x);
    const double ppb = std::clamp(pxPerBeat_ * factor, kMinPxPerBeat, kMaxPxPerBeat);
    if (ppb == pxPerBeat_) return false;
    pxPerBeat_ = ppb;
    scrollBeats_ = std::max(0.0, anchor - x / ppb);
    return true;
}

bool Timeline::zoomToFit(double start, double end, double width) {
    if (end <= start || width <= 0) return false;
    pxPerBeat_ = std::clamp(width / (end - start), kMinPxPerBeat, kMaxPxPerBeat);
    scrollBeats_ = std::max(0.0, start);
    return true;
}

double Timeline::gridStep() const {
    const double bar = timeSignature().beatsPerBar();
    std::vector<double> steps;
    for (double s : {1.0 / 32, 1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 1.0, 2.0}) {
        const double ratio = bar / s;
        // math.isclose(bar / s, round(bar / s)): a whole number of them in a bar.
        if (s < bar && std::abs(ratio - std::round(ratio)) <= 1e-9 * std::max(std::abs(ratio), std::round(ratio)))
            steps.push_back(s);
    }
    for (int m : {1, 2, 4, 8, 16, 32, 64, 128, 256}) steps.push_back(bar * m);
    const double minPx = gridMinPixels(gridLevel_);
    for (double step : steps) {
        if (step * pxPerBeat_ >= minPx) return step;
    }
    return steps.back();
}

double Timeline::snapBeat(double beat, bool bypass) const {
    if (!snap_ || bypass) return beat;
    const double step = gridStep();
    return app::roundHalfEven(beat / step) * step;
}

std::vector<GridLine> gridLines(const Timeline& view, double x0, double x1, double step) {
    if (step <= 0.0) step = view.gridStep();
    const app::TimeSignature ts = view.timeSignature();
    const auto first = static_cast<long long>(std::max(0.0, std::floor(view.xToBeat(x0) / step)));
    const auto last = static_cast<long long>(std::ceil(view.xToBeat(x1) / step));
    std::vector<GridLine> lines;
    for (long long k = first; k <= last; ++k) {
        const double beat = static_cast<double>(k) * step;
        LineKind kind = LineKind::Sub;
        if (app::isMultiple(beat, ts.beatsPerBar()))
            kind = LineKind::Bar;
        else if (app::isMultiple(beat, ts.beatLength()))
            kind = LineKind::Beat;
        lines.push_back({view.beatToX(beat), beat, kind});
    }
    return lines;
}

double labelStep(const Timeline& view, double step) {
    const app::TimeSignature ts = view.timeSignature();
    const double bar = ts.beatsPerBar();
    std::vector<double> candidates{step, ts.beatLength(), bar};
    for (int m : {2, 4, 8, 16, 32, 64, 128}) candidates.push_back(bar * m);
    std::vector<double> coarser;
    for (double c : candidates) {
        if (c >= step) coarser.push_back(c);
    }
    std::sort(coarser.begin(), coarser.end());
    for (double candidate : coarser) {
        if (candidate * view.pxPerBeat() >= 44) return candidate;
    }
    return candidates.back();
}

void drawGrid(SgPainter& painter, const Timeline& view, double x0, double x1, double top, double bottom) {
    if (bottom <= top) return;
    for (const GridLine& line : gridLines(view, x0 - 1, x1 + 1)) {
        const QColor color = line.kind == LineKind::Bar    ? Theme::kGridBar
                             : line.kind == LineKind::Beat ? Theme::kGridBeat
                                                           : Theme::kGridSub;
        painter.fillRect(QRectF(app::roundHalfEven(line.x), top, 1, bottom - top), color);
    }
}

}  // namespace sub::ui::roll
