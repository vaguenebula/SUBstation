#pragma once

// The time axis the arrangement and the piano roll share (the old
// arrangement/view_state.py's ViewState and arrangement/grid.py): zoom,
// horizontal scroll and the adaptive grid, in beats (the arrangement's, or a
// clip's content beats in the piano roll), and the vertical scroll in pixels;
// the grid lines the rulers, the lanes, the notes and the velocities draw, and
// the loop region.
//
// Time is in beats everywhere in the UI; seconds only come in through the
// tempo. beatToX(beat) = (beat - scrollBeats) * pxPerBeat. Vertical positions
// are content coordinates (0 at the top of the content); an item's y is
// content y - scrollY.
//
// The setters say whether anything changed; the view owning a Timeline emits
// its own signals (ViewState's changed: zoom or horizontal scroll;
// vscroll_changed; grid_changed).

#include "model/Timebase.h"

#include <vector>

namespace sub::app {
class Project;
}

namespace sub::ui {

class SgPainter;

namespace timeline {

// The narrowest a grid step may be on screen, in pixels, at a grid level
// (GRID_MIN_PIXELS: -2 narrowest .. 2 widest).
double gridMinPixels(int level);
inline constexpr int kMinGridLevel = -2;
inline constexpr int kMaxGridLevel = 2;

class Timeline {
public:
    static constexpr double kMinPxPerBeat = 0.25;
    static constexpr double kMaxPxPerBeat = 4000.0;
    static constexpr double kDefaultPxPerBeat = 24.0;

    // The project gives the time signature (the grid's bars and beats) and the tempo.
    void setProject(const app::Project* project) { project_ = project; }
    const app::Project* project() const { return project_; }
    app::TimeSignature timeSignature() const;
    double tempo() const;

    double pxPerBeat() const { return pxPerBeat_; }
    double scrollBeats() const { return scrollBeats_; }
    int scrollY() const { return scrollY_; }
    int maxScrollY() const { return maxScrollY_; }
    void setMaxScrollY(int max) { maxScrollY_ = max; }
    int gridLevel() const { return gridLevel_; }
    bool snap() const { return snap_; }

    double beatToX(double beat) const { return (beat - scrollBeats_) * pxPerBeat_; }
    double xToBeat(double x) const { return scrollBeats_ + x / pxPerBeat_; }
    // Source frames per pixel: a waveform's. `sourceTempo` is the tempo the
    // audio maps onto beats at (a warped clip's segment BPM); 0: the project's.
    double framesPerPixel(double sampleRate, double sourceTempo = 0.0) const;

    bool setScrollBeats(double beats);
    bool setScrollY(double y);
    // The zoom as it is (held to kMinPxPerBeat..kMaxPxPerBeat), scroll unchanged.
    bool setPxPerBeat(double pxPerBeat);
    // Zoom keeping the beat under `x` in place.
    bool zoomAt(double x, double factor);
    // Zoom so `start`..`end` fills `width` (Zoom to Arrangement).
    bool zoomToFit(double start, double end, double width);
    // Held to kMinGridLevel..kMaxGridLevel.
    bool setGridLevel(int level);
    bool setSnap(bool snap);

    // The adaptive grid in beats, like Ableton's zoom-dependent grid: the
    // smallest musical subdivision of the bar (1/32 to 2 beats, those that
    // divide it) or multiple of the bar at least gridMinPixels(level) wide.
    double gridStep() const;
    // `beat` on the grid, unless snapping is off or `bypass` (Alt held).
    double snapBeat(double beat, bool bypass = false) const;

private:
    const app::Project* project_ = nullptr;
    double pxPerBeat_ = kDefaultPxPerBeat;
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
// The grid lines between x0 and x1, from `top` to `bottom`. `overClip` draws
// them as faint dark lines, so the grid shows through a clip of any colour.
void drawGrid(SgPainter& painter, const Timeline& view, double x0, double x1, double top, double bottom,
              bool overClip = false);
// The loop region (while the loop is on) between x0 and x1, from `top` to `bottom`.
void drawLoopRegion(SgPainter& painter, const Timeline& view, double x0, double x1, double top, double bottom);

}  // namespace timeline
}  // namespace sub::ui
