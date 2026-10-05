#pragma once

// The piano roll's time axis: zoom, horizontal scroll and the adaptive grid in
// the clip's content beats, and its vertical scroll in pixels, as the
// arrangement's ViewState had them (arrangement/view_state.py, which the old
// piano roll used for itself); and the grid lines the ruler, the notes and the
// velocity lane draw (arrangement/grid.py).

#include "model/Timebase.h"

#include <vector>

namespace sub::app {
class Project;
}

namespace sub::ui {

class SgPainter;

namespace roll {

// The narrowest a grid step may be on screen, in pixels, at a grid level
// (GRID_MIN_PIXELS: -2 narrowest .. 2 widest).
double gridMinPixels(int level);

class Timeline {
public:
    static constexpr double kMinPxPerBeat = 0.25;
    static constexpr double kMaxPxPerBeat = 4000.0;

    // The project gives the time signature (the grid's bars and beats).
    void setProject(const app::Project* project) { project_ = project; }
    app::TimeSignature timeSignature() const;

    double pxPerBeat() const { return pxPerBeat_; }
    double scrollBeats() const { return scrollBeats_; }
    int scrollY() const { return scrollY_; }
    int maxScrollY() const { return maxScrollY_; }
    void setMaxScrollY(int max) { maxScrollY_ = max; }
    int gridLevel() const { return gridLevel_; }
    void setGridLevel(int level) { gridLevel_ = level; }
    bool snap() const { return snap_; }

    double beatToX(double beat) const { return (beat - scrollBeats_) * pxPerBeat_; }
    double xToBeat(double x) const { return scrollBeats_ + x / pxPerBeat_; }

    // These say whether anything changed (ViewState's changed and
    // vscroll_changed signals).
    bool setScrollBeats(double beats);
    bool setScrollY(double y);
    // Zoom keeping the beat under `x` in place.
    bool zoomAt(double x, double factor);
    bool zoomToFit(double start, double end, double width);

    // The adaptive grid in beats, like Ableton's zoom-dependent grid.
    double gridStep() const;
    double snapBeat(double beat, bool bypass = false) const;

private:
    const app::Project* project_ = nullptr;
    double pxPerBeat_ = 24.0;
    double scrollBeats_ = 0.0;
    int scrollY_ = 0;
    int maxScrollY_ = 0;
    int gridLevel_ = 0;
    bool snap_ = true;
};

enum class LineKind { Bar, Beat, Sub };

struct GridLine {
    double x;
    double beat;
    LineKind kind;
};

// The grid lines between x0 and x1 (every `step` beats; 0: the grid's step).
std::vector<GridLine> gridLines(const Timeline& view, double x0, double x1, double step = 0.0);
// Beats between ruler labels: the grid step or a coarser musical unit, so
// labels are at least 44 px apart.
double labelStep(const Timeline& view, double step);
// The grid lines between x0 and x1, from `top` to `bottom`.
void drawGrid(SgPainter& painter, const Timeline& view, double x0, double x1, double top, double bottom);

}  // namespace roll
}  // namespace sub::ui
