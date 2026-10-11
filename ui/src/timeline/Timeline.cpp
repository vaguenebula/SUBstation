#include "timeline/Timeline.h"

#include "model/Numbers.h"
#include "model/Project.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QPolygonF>
#include <QRectF>

#include <algorithm>
#include <array>
#include <cmath>

namespace sub::ui::timeline {

double gridMinPixels(int level) {
    static constexpr std::array<double, 5> kPixels{6.0, 11.0, 20.0, 40.0, 80.0};
    return kPixels[static_cast<size_t>(std::clamp(level, kMinGridLevel, kMaxGridLevel) - kMinGridLevel)];
}

app::TimeSignature Timeline::timeSignature() const {
    return project_ ? project_->timeSignature() : app::TimeSignature();
}

double Timeline::tempo() const { return project_ ? project_->tempo() : 120.0; }

double Timeline::framesPerPixel(double sampleRate, double sourceTempo) const {
    const double tempo = sourceTempo > 0.0 ? sourceTempo : this->tempo();
    return sampleRate * 60.0 / (tempo * pxPerBeat_);
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

bool Timeline::setPxPerBeat(double pxPerBeat) {
    pxPerBeat = std::clamp(pxPerBeat, kMinPxPerBeat, kMaxPxPerBeat);
    if (pxPerBeat == pxPerBeat_) return false;
    pxPerBeat_ = pxPerBeat;
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

bool Timeline::setGridLevel(int level) {
    level = std::clamp(level, kMinGridLevel, kMaxGridLevel);
    if (level == gridLevel_) return false;
    gridLevel_ = level;
    return true;
}

bool Timeline::setSnap(bool snap) {
    if (snap == snap_) return false;
    snap_ = snap;
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

void drawGrid(SgPainter& painter, const Timeline& view, double x0, double x1, double top, double bottom,
              bool overClip) {
    if (bottom <= top) return;
    // Over clips: faint dark lines, so the grid shows through whatever colour a clip has.
    static constexpr QColor kOverClipBar{0, 0, 0, 70};
    static constexpr QColor kOverClipBeat{0, 0, 0, 42};
    static constexpr QColor kOverClipSub{0, 0, 0, 24};
    for (const GridLine& line : gridLines(view, x0 - 1, x1 + 1)) {
        QColor color;
        switch (line.kind) {
            case LineKind::Bar: color = overClip ? kOverClipBar : Theme::gridBar(); break;
            case LineKind::Beat: color = overClip ? kOverClipBeat : Theme::gridBeat(); break;
            case LineKind::Sub: color = overClip ? kOverClipSub : Theme::gridSub(); break;
        }
        painter.fillRect(QRectF(app::roundHalfEven(line.x), top, 1, bottom - top), color);
    }
}

void drawLoopRegion(SgPainter& painter, const Timeline& view, double x0, double x1, double top, double bottom) {
    const app::Project* project = view.project();
    if (!project || !project->loopEnabled() || bottom <= top) return;
    const double left = std::max(x0, view.beatToX(project->loopStart()));
    const double right = std::min(x1, view.beatToX(project->loopEnd()));
    if (right > left) painter.fillRect(QRectF(left, top, right - left, bottom - top), Theme::loopRegion());
}

void drawPlayhead(SgPainter& painter, const Timeline& view, double beat, double height, bool ruler) {
    const double x = app::roundHalfEven(view.beatToX(beat));
    if (ruler) {
        painter.fillPolygon(
            QPolygonF({QPointF(x - 5, height - 8), QPointF(x + 6, height - 8), QPointF(x + 0.5, height - 1)}),
            Theme::playhead());
    } else {
        painter.fillRect(QRectF(x, 0, 1, height), Theme::playhead());
    }
}

}  // namespace sub::ui::timeline
