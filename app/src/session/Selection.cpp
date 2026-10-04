#include "session/Selection.h"

#include "editor/ProjectEditor.h"

#include <algorithm>

namespace sub::app {

Selection::Selection(QObject* parent) : QObject(parent) {}

QStringList Selection::trackIds() const {
    if (!trackId_.isEmpty() && tracks_.contains(trackId_)) return tracks_;
    return trackId_.isEmpty() ? QStringList() : QStringList{trackId_};
}

void Selection::clear(const QString& trackId) {
    clips_.clear();
    timeRange_.reset();
    lanes_.clear();
    points_.reset();
    tracks_.clear();
    if (!trackId.isEmpty()) trackId_ = trackId;
    focus_ = Focus::Clips;
    Q_EMIT changed();
}

void Selection::selectPoints(const QString& owner, const QString& key, const QSet<int>& indices) {
    clips_.clear();
    timeRange_.reset();
    lanes_.clear();
    points_ = indices.isEmpty() ? std::nullopt : std::optional<SelectedPoints>(SelectedPoints{owner, key, indices});
    tracks_.clear();
    if (owner != kMaster) trackId_ = owner;
    focus_ = indices.isEmpty() ? Focus::Clips : Focus::Automation;
    Q_EMIT changed();
}

void Selection::selectPoints(const QString& owner, const QString& key, const QList<int>& indices) {
    selectPoints(owner, key, QSet<int>(indices.begin(), indices.end()));
}

QSet<int> Selection::selectedPoints(const QString& owner, const QString& key) const {
    if (points_ && points_->owner == owner && points_->key == key) return points_->indices;
    return {};
}

void Selection::selectClips(const ProjectEditor& editor, const ClipRefs& refs, const QString& trackId) {
    const auto area = editor.clipsArea(refs);
    if (!area) {
        clear(trackId);
        return;
    }
    setTimeRange(area->start, area->end, area->trackIds, editor.clipsInRange(area->start, area->end, area->trackIds));
    if (!trackId.isEmpty() && area->trackIds.contains(trackId) && trackId != trackId_) {
        trackId_ = trackId;
        Q_EMIT changed();
    }
}

void Selection::selectTrack(const QString& trackId, bool focusTrack, Mode mode, const QStringList& order) {
    QString selected = trackId;
    if (mode == Mode::Toggle && !selected.isEmpty()) {
        QStringList tracks = trackIds();
        if (tracks.contains(selected)) {
            tracks.removeAll(selected);
            selected = tracks.isEmpty() ? QString() : tracks.back();
        } else {
            tracks.append(selected);
        }
        tracks_ = tracks;
        anchor_ = selected;
    } else if (mode == Mode::Range && !selected.isEmpty() && !order.isEmpty() && order.contains(anchor_) &&
               order.contains(selected)) {
        const auto a = order.indexOf(anchor_);
        const auto b = order.indexOf(selected);
        tracks_ = order.mid(std::min(a, b), std::abs(b - a) + 1);
    } else {
        tracks_.clear();
        anchor_ = selected;
    }
    trackId_ = selected;
    if (focusTrack) focusOnTracks();
    Q_EMIT changed();
}

void Selection::focusTracks() {
    if (focus_ != Focus::Track || timeRange_ || !clips_.isEmpty() || points_) {
        focusOnTracks();
        Q_EMIT changed();
    }
}

void Selection::focusOnTracks() {
    focus_ = Focus::Track;
    clips_.clear();
    timeRange_.reset();
    lanes_.clear();
    points_.reset();
}

void Selection::focusDevices() {
    if (focus_ != Focus::Devices) {
        focus_ = Focus::Devices;
        Q_EMIT changed();
    }
}

void Selection::setTimeRange(double start, double end, const QStringList& trackIds,
                             const std::optional<QSet<ClipRef>>& clips, const QList<LaneRef>& lanes) {
    const QList<LaneRef> rangeLanes = clips ? QList<LaneRef>() : lanes;
    if (end > start && (!trackIds.isEmpty() || !rangeLanes.isEmpty())) {
        timeRange_ = TimeRange{start, end, trackIds};
    } else {
        timeRange_.reset();
    }
    rangeSelectsClips_ = clips.has_value();
    clips_ = timeRange_ && clips ? *clips : QSet<ClipRef>();
    lanes_ = timeRange_ ? rangeLanes : QList<LaneRef>();
    points_.reset();
    tracks_.clear();
    if (!trackIds.isEmpty()) trackId_ = trackIds.front();
    focus_ = Focus::Clips;
    Q_EMIT changed();
}

void Selection::setLaneRange(double start, double end, const QStringList& trackIds) {
    setTimeRange(start, end, trackIds);
}

void Selection::setInsert(double beat) {
    insertBeat_ = std::max(0.0, beat);
    Q_EMIT insertChanged();
}

void Selection::prune(const Project& project) {
    QSet<QString> validTracks;
    for (const Track& t : project.tracks()) validTracks.insert(t.id);
    QSet<QString> selectable = validTracks;
    for (const Track& t : project.returns()) selectable.insert(t.id);
    QSet<ClipRef> clips;
    for (const ClipRef& ref : clips_) {
        const Track* track = validTracks.contains(ref.trackId) ? project.findTrack(ref.trackId) : nullptr;
        if (track != nullptr && project.findClip(ref.trackId, ref.clipId) != nullptr) clips.insert(ref);
    }
    QStringList tracks;
    for (const QString& id : tracks_) {
        if (selectable.contains(id)) tracks.append(id);
    }
    QString trackId = trackId_;
    if (!selectable.contains(trackId) && trackId != kMaster) trackId = tracks.isEmpty() ? QString() : tracks.back();
    QList<LaneRef> lanes;
    for (const LaneRef& lane : lanes_) {
        if (project.hasOwner(lane.first)) lanes.append(lane);
    }
    std::optional<TimeRange> timeRange = timeRange_;
    if (timeRange) {
        QStringList kept;
        for (const QString& id : timeRange->trackIds) {
            if (validTracks.contains(id)) kept.append(id);
        }
        if (!kept.isEmpty() || !lanes.isEmpty()) {
            timeRange->trackIds = kept;
        } else {
            timeRange.reset();
        }
    }
    std::optional<SelectedPoints> points = points_;
    if (points) {
        const auto count = project.hasOwner(points->owner)
                               ? static_cast<int>(project.envelope(points->owner, points->key).size())
                               : 0;
        QSet<int> indices;
        for (int i : points->indices) {
            if (i < count) indices.insert(i);
        }
        if (indices.isEmpty()) {
            points.reset();
        } else {
            points->indices = indices;
        }
    }
    if (clips != clips_ || trackId != trackId_ || timeRange != timeRange_ || lanes != lanes_ || points != points_ ||
        tracks != tracks_) {
        clips_ = clips;
        trackId_ = trackId;
        tracks_ = tracks;
        timeRange_ = timeRange;
        lanes_ = timeRange ? lanes : QList<LaneRef>();
        points_ = points;
        if (!points_ && focus_ == Focus::Automation) focus_ = Focus::Clips;
        Q_EMIT changed();
    }
}

}  // namespace sub::app
