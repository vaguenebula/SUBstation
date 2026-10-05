#pragma once

// Automation lanes in the arrangement: envelopes drawn over the timeline and
// edited with the mouse. Tracks show theirs in their own lanes (over the clips,
// below the clips' title band) and in lanes below them (ArrangementLanes); the
// returns and the master in their lanes (BusLane). Both hosts hand their lanes
// to the functions here as EnvelopeAreas.
//
// In a lane (as in Ableton):
// - A press on the envelope's line adds a breakpoint on it (on the grid when
//   snapping is on; Alt-click where there is no segment to bend: off the grid),
//   and dragging on places it. Where it would go shows while the mouse is over
//   the line. A click off the line adds nothing.
// - A click on a breakpoint deletes it; Shift- or Ctrl-click selects it instead
//   (with the others selected), and Delete deletes the selected ones.
// - Near the line between two breakpoints (not on it), the segment lights up:
//   dragging it moves both breakpoints, in time and value. A step (two
//   breakpoints at the same time) is a segment too, grabbed anywhere along it.
// - Dragging a breakpoint moves it, and the others selected with it, in time and
//   value. Alt: off the grid; Shift while dragging: finer values. One breakpoint
//   can't pass its neighbours; several override the breakpoints they land on.
// - Alt-dragging between two breakpoints bends the segment: up bulges it upward.
// - Dragging from off the breakpoints selects a time range on the lanes it
//   crosses, up or down, of every track (into the clips too: where a selection
//   starts decides what it selects); Delete clears their automation there,
//   Ctrl+D duplicates it. Shift-click extends a selection (ExtendGesture).
//   Dragging inside the selected range (off its breakpoints and line) moves the
//   automation in it, on all its lanes, up, down, left and right, over what is
//   where it lands: breakpoints at the range's edges keep the envelope outside
//   it as it was.
//
// Envelopes are drawn red while they play, grey when overridden (the target was
// changed by hand: Re-Enable Automation brings them back); a target without an
// envelope shows its own value as a faint line.
//
// Values are normalized 0..1 (top 1, bottom 0, kValuePad px padding). What is
// under the mouse is tested on the line as drawn (trace), so what you click is
// what you see.

#include "arrangement/Gesture.h"
#include "arrangement/MenuEntries.h"
#include "model/Automation.h"
#include "model/ParamSpec.h"

#include <QColor>
#include <QCursor>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QString>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace sub::app {
class Session;
class Selection;
}  // namespace sub::app

namespace sub::ui {
class Arrangement;
class SgPainter;
namespace timeline {
class Timeline;
}
}  // namespace sub::ui

namespace sub::ui::arrangement {

class LanesHost;

inline constexpr QColor kEnvelope{0xff, 0x4a, 0x3d};
inline constexpr QColor kOverridden{0x8c, 0x8c, 0x8c};
inline constexpr QColor kUnautomated{255, 74, 61, 110};
inline constexpr QColor kGhost{255, 74, 61, 170};       // where a click on the line would add a breakpoint
inline constexpr QColor kLaneBackground{0, 0, 0, 55};  // over the clips of a lane showing automation
inline constexpr double kPointRadius = 3.0;
inline constexpr double kPointGrab = 6.0;     // pixels around a breakpoint that grab it
inline constexpr double kLineGrab = 4.0;      // pixels around the envelope's line that count as on it
inline constexpr double kSegmentGrab = 14.0;  // pixels around it that grab the segment (beyond kLineGrab)
inline constexpr double kCurvePixels = 150.0;  // an Alt-drag this far bends a segment from straight to its most
inline constexpr double kValuePad = 4.0;      // between a lane's edges and its values' range
inline constexpr double kSamplePixels = 3.0;  // between the points a curved segment is drawn through

// A lane showing an envelope, in its host's coordinates.
struct EnvelopeArea {
    QString owner;  // track id or kMaster
    QString key;
    int lane = -1;  // -1: the owner's own lane; else its index in the lanes below it
    QRectF rect;    // where it takes the mouse

    bool sameLane(const EnvelopeArea& other) const {
        return owner == other.owner && key == other.key && lane == other.lane;
    }
    // Where values go: 1 at the top, 0 at the bottom.
    QRectF values() const { return rect.adjusted(0, kValuePad, 0, -kValuePad); }
    double y(double value) const;
    double value(double y) const;

    friend bool operator==(const EnvelopeArea&, const EnvelopeArea&) = default;
};

// What is under the mouse in a lane: a breakpoint (Point, `index`), a place on
// the line where a click adds one (Add, `beat`, `value`), a segment to drag
// (Segment, `index`: its first breakpoint), the flat line out from the first
// or last breakpoint (End, `index`, `beat`: under the mouse), or the selected
// range (Range).
struct Hover {
    enum class Kind { Point, Add, Segment, End, Range };
    QString owner;
    QString key;
    int lane = -1;
    Kind kind = Kind::Point;
    std::optional<int> index;
    double beat = 0.0;
    double value = 0.0;

    bool on(const EnvelopeArea& area) const { return owner == area.owner && key == area.key && lane == area.lane; }
    friend bool operator==(const Hover&, const Hover&) = default;
};

// How a lane draws, worked out on the GUI thread (the bridge's state): the
// parameter's spec (a discrete one steps), whether its automation is
// overridden, and the target's own value (normalized) for a lane without an envelope.
struct EnvelopeLook {
    std::optional<app::ParamSpec> spec;
    bool overridden = false;
    std::optional<double> unautomated;

    const app::ParamSpec* quantizer() const { return spec && spec->discrete() ? &*spec : nullptr; }
};

// What the hosts (the track lanes and the bus lanes) give the gestures.
class LanesHost {
public:
    virtual ~LanesHost() = default;
    virtual app::Session* hostSession() const = 0;
    virtual Arrangement* hostArrangement() const = 0;
    // The automation lanes showing, top to bottom, in the host's coordinates.
    virtual std::vector<EnvelopeArea> envelopeAreas() const = 0;
    virtual double hostWidth() const = 0;
    virtual void setHostCursor(const QCursor& cursor) = 0;
    virtual void repaint() = 0;
    // The track lanes: a drag can go up from a track's lanes into its clips.
    virtual bool isTrackLanes() const { return false; }
    // Whether `pos` is in the clips' band of a track lane (isTrackLanes()).
    virtual bool inClipBand(const QPointF& pos) const {
        Q_UNUSED(pos);
        return false;
    }
    // The row index under a host y (clamped above and below the tracks; isTrackLanes()).
    virtual std::optional<int> rowAt(double y, bool clamp) const {
        Q_UNUSED(y);
        Q_UNUSED(clamp);
        return std::nullopt;
    }
};

namespace envelopes {

EnvelopeLook lookOf(app::Session& session, const QString& owner, const QString& key);

std::optional<EnvelopeArea> areaAt(const std::vector<EnvelopeArea>& areas, const QPointF& pos);
// The index of the area closest to `y` vertically.
std::optional<int> nearestArea(const std::vector<EnvelopeArea>& areas, double y);
// Select the time from `start` to `end` on these lanes (a lane range over
// their tracks), the insert marker at its start.
void selectLaneRange(LanesHost& host, double start, double end, const std::vector<EnvelopeArea>& lanes);
// The breakpoint under `pos` (the one on top where several are).
std::optional<int> pointAt(const timeline::Timeline& view, const EnvelopeArea& area, const app::Envelope& points,
                           const QPointF& pos);
// The envelope's line from x0 to x1: through its breakpoints, and along curved
// segments every few pixels. A discrete target (`quantize`) steps.
QPolygonF trace(const timeline::Timeline& view, const EnvelopeArea& area, const app::Envelope& points, double x0,
                double x1, const app::ParamSpec* quantize);
// Whether `pos` is inside the selected time range, on a lane it covers.
bool inRange(LanesHost& host, const EnvelopeArea& area, const QPointF& pos);
// Where a click at `pos` adds a breakpoint, (beat, value): on the line, at the
// grid line nearest the mouse. None off the line.
std::optional<std::pair<double, double>> addTarget(LanesHost& host, const EnvelopeArea& area, const QPointF& pos,
                                                   Qt::KeyboardModifiers modifiers);

// The gesture a press on an automation lane starts. (Hosts start one on a
// double-click too: each click of it counts.)
std::unique_ptr<Gesture> press(LanesHost& host, const EnvelopeArea& area, const QPointF& pos,
                               Qt::KeyboardModifiers modifiers);
// (What is under the mouse, the cursor) over a lane (none: no lane).
std::pair<std::optional<Hover>, QCursor> hover(LanesHost& host, const std::optional<EnvelopeArea>& area,
                                               const QPointF& pos, Qt::KeyboardModifiers modifiers);
// A lane's right-click menu.
void addMenuEntries(LanesHost& host, const EnvelopeArea& area, const QPointF& pos, MenuEntries& menu);

// --- Drawing (render thread: reads only) ---

// A lane's envelope (and, with `shade`, a veil over what is under it).
void drawArea(SgPainter& painter, const timeline::Timeline& view, const app::Selection& selection,
              const EnvelopeArea& area, const app::Envelope& points, const EnvelopeLook& look, const QRectF& visible,
              const std::optional<Hover>& hover, bool shade, bool gestureActive);
// The selected time range, over the automation lanes it covers.
void drawRange(SgPainter& painter, const timeline::Timeline& view, const app::Selection& selection,
               const std::vector<EnvelopeArea>& areas, const QColor& tint);
// What a breakpoint being dragged is set to, next to it.
void drawReadout(SgPainter& painter, double width, const std::optional<Readout>& readout);

}  // namespace envelopes
}  // namespace sub::ui::arrangement
