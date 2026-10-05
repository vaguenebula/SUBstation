#pragma once

// The arrangement's vertical layout (the old arrangement/view_state.py's
// TrackLayout, Row, LaneRow and automation_rows): a row per track of the
// arrangement, in content coordinates (0 at the top of the first track, before
// scrolling), and the rows of the returns and the master.
//
// Rows stay one per track, in order, hidden or not, so a row's index is the
// track's index in Project::tracks(). A track in a folded group has a row too,
// but hidden, with no height: rowIndexAt() bisects the rows' tops, and a hidden
// row has the same top as the row after it, so the bisect lands on the shown
// one. A row's mainHeight isn't the track's height: it is taller while the
// track's automation shows (room in the header for the choosers) and fixed
// while folded.

#include "model/Automation.h"

#include <QString>

#include <optional>
#include <vector>

namespace sub::app {
class Project;
}

namespace sub::ui::arrangement {

inline constexpr int kAutomationLaneHeight = 44;  // a lane shown below a track (or a return, the master)
inline constexpr int kFoldedHeight = 22;  // a folded track: its name row (its clips are bars, as in Ableton)
inline constexpr int kFoldedGroupHeight = 24;  // a folded group: a little taller, so it stands out
// A track's own lane while its automation shows: room in its header for the choosers.
inline constexpr int kMinAutomationRow = 76;
// The send knobs' row in a track's header, while there are return tracks
// (below volume and pan): the choosers go below it.
inline constexpr int kSendsRow = 24;
inline constexpr int kHeaderWidth = 252;  // the headers' column
inline constexpr int kDropZone = 120;  // empty space below the last track for dropping files
inline constexpr int kMasterHeight = 40;
inline constexpr int kReturnHeight = 52;  // a return track's row: its name, then volume, pan and its sends
inline constexpr int kChooserHeight = 18;  // an automation chooser

// How tall a track's own lane is at least while its automation shows.
int minAutomationRow(const app::Project& project);

// An automation lane shown below its owner's own lane.
struct LaneRow {
    int index = 0;  // in the owner's AutomationView::lanes
    QString key;
    int top = 0;
    int height = kAutomationLaneHeight;

    friend bool operator==(const LaneRow&, const LaneRow&) = default;
};

// A track: its own lane (clips; its automation too while that shows), then the
// automation lanes shown below it. A folded track's row is kFoldedHeight high
// (a folded group's kFoldedGroupHeight); neither shows automation. A folded
// track (not a group) shows its clips as `bars`, as Ableton does: they are
// clicked and dragged, but the row isn't a grid to select time on.
struct Row {
    QString trackId;
    int top = 0;
    int mainHeight = 0;  // the track's own lane
    std::vector<LaneRow> lanes;
    bool automation = false;  // its automation shows
    bool hidden = false;      // in a folded group
    bool folded = false;      // the track itself is folded
    int depth = 0;            // how many groups it is in
    bool bars = false;        // folded, and not a group: its clips are bars

    int height() const;
    int bottom() const { return top + height(); }

    friend bool operator==(const Row&, const Row&) = default;
};

// An owner's own lane with its automation view (at least `minRow` high while
// it shows) and the lanes below it, from `top`.
struct AutomationRows {
    int mainHeight = 0;
    std::vector<LaneRow> lanes;

    int height() const;
};
AutomationRows automationRows(const app::AutomationView& view, int top, int mainHeight,
                              int minRow = kMinAutomationRow);
// A return's own row (taller while its automation shows, for the choosers) and its lanes, from 0.
AutomationRows returnRows(const app::Project& project, const QString& returnId);
// The master's.
AutomationRows masterRows(const app::Project& project);

class TrackLayout {
public:
    void rebuild(const app::Project& project);

    const std::vector<Row>& rows() const { return rows_; }
    int totalHeight() const { return totalHeight_; }
    // The row at a content height (never a hidden one); none outside the tracks.
    std::optional<int> rowIndexAt(double contentY) const;
    std::optional<int> lastShownIndex() const;
    const Row* rowFor(const QString& trackId) const;
    std::optional<int> indexOf(const QString& trackId) const;
    // The rows shown between two content heights, with their indices.
    std::vector<int> visibleRows(double top, double bottom) const;

private:
    std::vector<Row> rows_;
    std::vector<int> tops_;
    int totalHeight_ = 0;
};

}  // namespace sub::ui::arrangement
