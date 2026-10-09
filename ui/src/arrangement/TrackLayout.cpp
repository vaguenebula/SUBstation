#include "arrangement/TrackLayout.h"

#include "model/Project.h"

#include <QSet>

#include <algorithm>

namespace sub::ui::arrangement {

int minAutomationRow(const app::Track& track) { return track.isGroup() ? kMinGroupAutomationRow : kMinAutomationRow; }

int Row::height() const {
    int h = mainHeight;
    for (const LaneRow& lane : lanes) h += lane.height;
    return h;
}

int AutomationRows::height() const {
    int h = mainHeight;
    for (const LaneRow& lane : lanes) h += lane.height;
    return h;
}

AutomationRows automationRows(const app::AutomationView& view, int top, int mainHeight, int minRow) {
    if (!view.shown) return {mainHeight, {}};
    AutomationRows rows;
    rows.mainHeight = std::max(mainHeight, minRow);
    for (int i = 0; i < view.lanes.size(); ++i)
        rows.lanes.push_back({i, view.lanes[i], top + rows.mainHeight + i * kAutomationLaneHeight, kAutomationLaneHeight});
    return rows;
}

AutomationRows returnRows(const app::Project& project, const QString& returnId) {
    return automationRows(project.automationView(returnId), 0, kReturnHeight);
}

AutomationRows masterRows(const app::Project& project) {
    return automationRows(project.master().automationView, 0, kMasterHeight);
}

void TrackLayout::rebuild(const app::Project& project) {
    rows_.clear();
    int y = 0;
    QSet<QString> folded;  // folded groups, and the groups in them
    for (const app::Track& track : project.tracks()) {
        const bool hidden = track.parent && folded.contains(*track.parent);
        if (track.isGroup() && (hidden || track.folded)) folded.insert(track.id);
        int depth = 0;
        if (track.parent) {
            for (auto it = rows_.rbegin(); it != rows_.rend(); ++it) {
                if (it->trackId == *track.parent) {
                    depth = it->depth + 1;
                    break;
                }
            }
        }
        Row row;
        row.trackId = track.id;
        row.top = y;
        row.depth = depth;
        if (hidden) {
            row.hidden = true;
        } else if (track.folded) {  // just its name row, no automation
            row.mainHeight = kFoldedHeight;
            row.folded = true;
            row.bars = !track.isGroup();
        } else {
            AutomationRows lanes = automationRows(track.automationView, y, track.height, minAutomationRow(track));
            row.mainHeight = lanes.mainHeight;
            row.lanes = std::move(lanes.lanes);
            row.automation = track.automationView.shown;
        }
        y += row.height();
        rows_.push_back(std::move(row));
    }
    tops_.clear();
    for (const Row& row : rows_) tops_.push_back(row.top);
    totalHeight_ = y;
}

std::optional<int> TrackLayout::rowIndexAt(double contentY) const {
    if (contentY < 0 || contentY >= totalHeight_) return std::nullopt;
    const auto it = std::upper_bound(tops_.begin(), tops_.end(), contentY);
    return static_cast<int>(it - tops_.begin()) - 1;
}

std::optional<int> TrackLayout::lastShownIndex() const {
    for (int i = static_cast<int>(rows_.size()) - 1; i >= 0; --i) {
        if (!rows_[static_cast<size_t>(i)].hidden) return i;
    }
    return std::nullopt;
}

const Row* TrackLayout::rowFor(const QString& trackId) const {
    for (const Row& row : rows_) {
        if (row.trackId == trackId) return &row;
    }
    return nullptr;
}

std::optional<int> TrackLayout::indexOf(const QString& trackId) const {
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].trackId == trackId) return static_cast<int>(i);
    }
    return std::nullopt;
}

std::vector<int> TrackLayout::visibleRows(double top, double bottom) const {
    std::vector<int> result;
    const auto it = std::upper_bound(tops_.begin(), tops_.end(), top);
    const int start = std::max(0, static_cast<int>(it - tops_.begin()) - 1);
    for (int index = start; index < static_cast<int>(rows_.size()); ++index) {
        const Row& row = rows_[static_cast<size_t>(index)];
        if (row.top >= bottom) break;
        if (!row.hidden) result.push_back(index);
    }
    return result;
}

}  // namespace sub::ui::arrangement
