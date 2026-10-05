#include "arrangement/ClipGestures.h"

#include "arrangement/Arrangement.h"
#include "audio/EngineBridge.h"
#include "editor/ProjectEditor.h"
#include "model/Edits.h"
#include "model/Errors.h"
#include "model/Project.h"
#include "session/Selection.h"
#include "session/Session.h"

#include <QCursor>

#include <algorithm>
#include <cmath>

namespace sub::ui::arrangement {

namespace {

bool farEnough(const QPointF& pos, const QPointF& press) { return (pos - press).manhattanLength() >= kDragThreshold; }

}  // namespace

bool isPanModifier(Qt::KeyboardModifiers modifiers) {
    return (modifiers & Qt::ControlModifier) && (modifiers & Qt::AltModifier);
}

// --- MoveRangeGesture --------------------------------------------------------------------------

MoveRangeGesture::MoveRangeGesture(LanesHost& host, const QPointF& press, std::function<void()> onClick)
    : host_(host), press_(press), onClick_(std::move(onClick)) {
    app::Session& session = *host.hostSession();
    const app::Project& project = *session.project();
    const auto& range = session.selection()->timeRange();
    if (range) {
        start_ = range->start;
        end_ = range->end;
        trackIds_ = range->trackIds;
    }
    const double tempo = project.tempo();
    const timeline::Timeline& view = host.hostArrangement()->view();
    originBeat_ = view.xToBeat(press.x());
    originRow_ = host.rowAt(press.y(), true).value_or(0);
    for (const QString& id : trackIds_) {
        const app::Track* track = project.findTrack(id);
        if (!track || !project.hasTrack(id)) continue;
        const int row = project.trackIndex(id);
        if (project.isFrozen(id) && track->hasClips()) {
            frozen_.push_back({row, QColor(track->color), track->clips});
            for (const app::Clip& c : track->clips) frozenIds_.insert(c.id);
        }
        std::vector<app::Clip> inside;
        for (const app::Clip& c : track->clips) {
            if (c.startBeat < end_ && c.endBeat(tempo) > start_) inside.push_back(c);
        }
        for (const app::Clip& c : inside) touched_.insert(c.id);
        for (const app::Clip& c : app::edits::removeRange(inside, start_, end_, tempo))
            remnants_.push_back({row, QColor(track->color), c});
        for (const app::Clip& c : app::edits::sliceRange(inside, start_, end_, tempo)) pieces_.emplace_back(row, c);
    }
}

void MoveRangeGesture::move(const QPointF& pos, Qt::KeyboardModifiers modifiers) {
    if (!active_) {
        if (!farEnough(pos, press_)) return;
        active_ = true;
    }
    const timeline::Timeline& view = host_.hostArrangement()->view();
    const bool bypass = modifiers & Qt::AltModifier;
    const double raw = view.xToBeat(pos.x()) - originBeat_;
    delta_ = std::max(0.0, view.snapBeat(start_ + raw, bypass)) - start_;
    const int row = host_.rowAt(pos.y(), true).value_or(originRow_);
    app::ClipRefs refs;
    for (const QString& id : trackIds_) refs.append({id, QString()});
    trackDelta_ = host_.hostSession()->editor()->clampTrackDelta(refs, row - originRow_);
    copy_ = modifiers & Qt::ControlModifier;
    // Frozen tracks' clips where they stay: on a frozen track the moved stretch
    // replaces everything where it lands.
    frozenKept_.clear();
    const double tempo = host_.hostSession()->project()->tempo();
    for (const FrozenRow& frozen : frozen_) {
        std::vector<app::Clip> clips =
            copy_ ? frozen.clips : app::edits::removeRange(frozen.clips, start_, end_, tempo);
        if (trackDelta_ == 0) clips = app::edits::removeRange(clips, start_ + delta_, end_ + delta_, tempo);
        for (const app::Clip& c : clips) frozenKept_.push_back({frozen.row, frozen.color, c});
    }
    preview();
}

void MoveRangeGesture::preview() {
    // The engine plays the clips where they would land (only when that changes).
    const auto state = std::make_tuple(delta_, trackDelta_, copy_);
    if (previewed_ == state) return;
    previewed_ = state;
    app::Session& session = *host_.hostSession();
    const app::MovedRange after = session.editor()->movedRange(start_, end_, trackIds_, delta_, trackDelta_, copy_);
    session.bridge()->previewClips(after.clips, after.frozen);
}

QSet<QString> MoveRangeGesture::hiddenIds() const {
    if (!active_) return {};
    return copy_ ? frozenIds_ : touched_ + frozenIds_;
}

std::vector<GestureClip> MoveRangeGesture::ghosts() const {
    if (!active_) return {};
    const auto& tracks = host_.hostSession()->project()->tracks();
    std::vector<GestureClip> ghosts;
    for (const auto& [row, clip] : pieces_) {
        const int to = row + trackDelta_;
        if (to < 0 || to >= static_cast<int>(tracks.size())) continue;
        app::Clip moved = clip;
        moved.startBeat = clip.startBeat + delta_;
        ghosts.push_back({to, QColor(tracks[static_cast<size_t>(to)].color), moved});
    }
    return ghosts;
}

std::vector<GestureClip> MoveRangeGesture::kept() const {
    if (!active_) return {};
    std::vector<GestureClip> kept = frozenKept_;
    if (copy_) return kept;
    const auto frozenRow = [&](int row) {
        return std::any_of(frozen_.begin(), frozen_.end(), [&](const FrozenRow& f) { return f.row == row; });
    };
    for (const GestureClip& remnant : remnants_) {
        if (!frozenRow(remnant.row)) kept.push_back(remnant);
    }
    return kept;
}

std::optional<app::TimeRange> MoveRangeGesture::timeRange() const {
    if (!active_) return std::nullopt;
    const app::Project& project = *host_.hostSession()->project();
    QStringList ids;
    for (const QString& id : trackIds_) {
        if (!project.hasTrack(id)) continue;
        const int to = project.trackIndex(id) + trackDelta_;
        if (to >= 0 && to < static_cast<int>(project.tracks().size())) ids << project.tracks()[static_cast<size_t>(to)].id;
    }
    return app::TimeRange{start_ + delta_, end_ + delta_, ids};
}

void MoveRangeGesture::finish() {
    if (!active_) {
        if (onClick_) onClick_();
        return;
    }
    app::Session& session = *host_.hostSession();
    std::pair<double, QStringList> landed{start_, trackIds_};
    try {
        landed = session.editor()->moveRange(start_, end_, trackIds_, delta_, trackDelta_, copy_);
    } catch (const app::EditError& error) {
        Q_EMIT host_.hostArrangement()->statusMessage(error.message());
    }
    session.bridge()->endClipPreview();  // (the model's clips: where they went, or back if refused)
    const auto& [start, ids] = landed;
    const double end = start + end_ - start_;
    app::Selection& selection = *session.selection();
    selection.setTimeRange(start, end, ids, session.editor()->clipsInRange(start, end, ids));
    selection.setInsert(start);
}

void MoveRangeGesture::cancel() {
    if (previewed_) host_.hostSession()->bridge()->endClipPreview();
}

// --- TrimGesture ----------------------------------------------------------------------------------

TrimGesture::TrimGesture(LanesHost& host, const QString& trackId, const app::Clip& clip, bool left)
    : host_(host), trackId_(trackId), clip_(clip), left_(left) {
    const app::Project& project = *host.hostSession()->project();
    row_ = project.trackIndex(trackId);
    color_ = QColor(project.track(trackId).color);
}

void TrimGesture::move(const QPointF& pos, Qt::KeyboardModifiers modifiers) {
    app::Session& session = *host_.hostSession();
    const app::Project& project = *session.project();
    const app::Track* track = project.findTrack(trackId_);
    if (!track) return;
    const timeline::Timeline& view = host_.hostArrangement()->view();
    const double tempo = project.tempo();
    const double beat = view.snapBeat(view.xToBeat(pos.x()), modifiers & Qt::AltModifier);
    const std::optional<app::Clip> previous = result_;
    result_ = left_ ? app::edits::trimStart(clip_, beat, tempo) : app::edits::trimEnd(clip_, beat, tempo);
    if (result_ != previous) {
        std::vector<app::Clip> clips;
        for (const app::Clip& c : track->clips) clips.push_back(c.id == clip_.id ? *result_ : c);
        QMap<QString, std::vector<app::Clip>> preview;
        preview.insert(trackId_, app::edits::resolveOverlaps(clips, {clip_.id}, tempo));
        session.bridge()->previewClips(preview);
    }
}

QSet<QString> TrimGesture::hiddenIds() const { return result_ ? QSet<QString>{clip_.id} : QSet<QString>(); }

std::vector<GestureClip> TrimGesture::ghosts() const {
    if (!result_) return {};
    return {{row_, color_, *result_}};
}

std::optional<app::TimeRange> TrimGesture::timeRange() const {
    if (!result_) return std::nullopt;
    const double tempo = host_.hostSession()->project()->tempo();
    return app::TimeRange{result_->startBeat, result_->endBeat(tempo), {trackId_}};
}

void TrimGesture::finish() {
    app::Session& session = *host_.hostSession();
    if (result_ && *result_ != clip_ && session.project()->findClip(trackId_, clip_.id)) {
        try {
            session.editor()->replaceClip(trackId_, *result_, QStringLiteral("Trim Clip"));
            // The selection (and where playback starts) follows the clip's new edges.
            app::Selection& selection = *session.selection();
            selection.selectClips(*session.editor(), {{trackId_, clip_.id}});
            if (selection.timeRange()) selection.setInsert(selection.timeRange()->start);
        } catch (const app::EditError& error) {
            Q_EMIT host_.hostArrangement()->statusMessage(error.message());
        }
    }
    session.bridge()->endClipPreview();  // (the model's clips: trimmed, or as they were)
}

void TrimGesture::cancel() {
    if (result_) host_.hostSession()->bridge()->endClipPreview();
}

// --- TimeSelectGesture ---------------------------------------------------------------------------

TimeSelectGesture::TimeSelectGesture(LanesHost& host, const QPointF& press, bool bypassSnap)
    : host_(host), press_(press) {
    const timeline::Timeline& view = host.hostArrangement()->view();
    anchor_ = std::max(0.0, view.snapBeat(view.xToBeat(press.x()), bypassSnap));
    anchorRow_ = host.rowAt(press.y(), true).value_or(0);
}

void TimeSelectGesture::move(const QPointF& pos, Qt::KeyboardModifiers modifiers) {
    if (!active_) {
        if (!farEnough(pos, press_)) return;
        active_ = true;
    }
    Arrangement& arrangement = *host_.hostArrangement();
    app::Session& session = *host_.hostSession();
    const timeline::Timeline& view = arrangement.view();
    const double beat = std::max(0.0, view.snapBeat(view.xToBeat(pos.x()), modifiers & Qt::AltModifier));
    const double start = std::min(anchor_, beat), end = std::max(anchor_, beat);
    const auto& rows = arrangement.layout().rows();
    if (rows.empty()) return;
    const int row = host_.rowAt(pos.y(), true).value_or(anchorRow_);
    const int first = std::clamp(std::min(anchorRow_, row), 0, static_cast<int>(rows.size()) - 1);
    const int last = std::clamp(std::max(anchorRow_, row), 0, static_cast<int>(rows.size()) - 1);
    QStringList ids;
    for (int i = first; i <= last; ++i) ids << rows[static_cast<size_t>(i)].trackId;
    const QStringList trackIds = session.project()->withContents(ids);
    session.selection()->setTimeRange(start, end, trackIds, session.editor()->clipsInRange(start, end, trackIds));
    host_.setHostCursor(QCursor(Qt::IBeamCursor));
    session.selection()->setInsert(start);
}

// --- PanGesture ------------------------------------------------------------------------------------

PanGesture::PanGesture(LanesHost& host, const QPointF& press) : host_(host), press_(press) {
    scrollBeats_ = host.hostArrangement()->scrollBeats();
    scrollY_ = host.hostArrangement()->scrollY();
}

void PanGesture::move(const QPointF& pos, Qt::KeyboardModifiers) {
    Arrangement& arrangement = *host_.hostArrangement();
    const QPointF delta = pos - press_;
    if (delta.x() != 0.0) arrangement.scrollByHand(scrollBeats_ - delta.x() / arrangement.pxPerBeat());
    arrangement.setScrollY(scrollY_ - delta.y());
}

}  // namespace sub::ui::arrangement
