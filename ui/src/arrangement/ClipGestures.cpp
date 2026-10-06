#include "arrangement/ClipGestures.h"

#include "arrangement/Arrangement.h"
#include "audio/EngineBridge.h"
#include "editor/ProjectEditor.h"
#include "model/Edits.h"
#include "model/Errors.h"
#include "model/Project.h"
#include "model/Timebase.h"
#include "session/Selection.h"
#include "session/Session.h"

#include <QCursor>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

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
        rows_ = session.selection()->rangeRows();
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
    // (Drawn over the rows the selection covers, where they go.)
    if (!active_) return std::nullopt;
    const app::Project& project = *host_.hostSession()->project();
    QStringList ids;
    for (const QString& id : rows_) {
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
    // The rows it covered, where they went (as far as its tracks went: none if refused).
    const app::Project& project = *session.project();
    const bool known = !ids.isEmpty() && !trackIds_.isEmpty() && project.hasTrack(ids.front()) &&
                       project.hasTrack(trackIds_.front());
    const int moved = known ? project.trackIndex(ids.front()) - project.trackIndex(trackIds_.front()) : 0;
    const auto& tracks = project.tracks();
    QStringList rows;
    for (const QString& id : rows_) {
        if (!project.hasTrack(id)) continue;
        const int to = project.trackIndex(id) + moved;
        if (to >= 0 && to < static_cast<int>(tracks.size())) rows << tracks[static_cast<size_t>(to)].id;
    }
    app::Selection& selection = *session.selection();
    selection.setTimeRange(start, end, ids, session.editor()->clipsInRange(start, end, ids), {}, rows);
    selection.setInsert(start);
}

void MoveRangeGesture::cancel() {
    if (previewed_) host_.hostSession()->bridge()->endClipPreview();
}

// --- ClipEditGesture ------------------------------------------------------------------------------

ClipEditGesture::ClipEditGesture(LanesHost& host, const QString& trackId, const app::Clip& clip, QString text)
    : host_(host), trackId_(trackId), clip_(clip), text_(std::move(text)) {
    const app::Project& project = *host.hostSession()->project();
    row_ = project.trackIndex(trackId);
    color_ = QColor(project.track(trackId).color);
}

const app::Track* ClipEditGesture::track() const { return host_.hostSession()->project()->findTrack(trackId_); }

double ClipEditGesture::tempo() const { return host_.hostSession()->project()->tempo(); }

void ClipEditGesture::update(const app::Clip& result) {
    const app::Track* track = this->track();
    if (!track) return;
    const std::optional<app::Clip> previous = result_;
    result_ = result;
    if (result_ == previous) return;
    std::vector<app::Clip> clips;
    for (const app::Clip& c : track->clips) clips.push_back(c.id == clip_.id ? *result_ : c);
    QMap<QString, std::vector<app::Clip>> preview;
    preview.insert(trackId_, app::edits::resolveOverlaps(clips, {clip_.id}, tempo()));
    host_.hostSession()->bridge()->previewClips(preview);
}

QSet<QString> ClipEditGesture::hiddenIds() const { return result_ ? QSet<QString>{clip_.id} : QSet<QString>(); }

std::vector<GestureClip> ClipEditGesture::ghosts() const {
    if (!result_) return {};
    return {{row_, color_, *result_}};
}

std::optional<app::TimeRange> ClipEditGesture::timeRange() const {
    if (!result_) return std::nullopt;
    return app::TimeRange{result_->startBeat, result_->endBeat(tempo()), {trackId_}};
}

void ClipEditGesture::finish() {
    app::Session& session = *host_.hostSession();
    if (result_ && *result_ != clip_ && session.project()->findClip(trackId_, clip_.id)) {
        try {
            session.editor()->replaceClip(trackId_, *result_, text_);
            // The selection (and where playback starts) follows the clip's new edges.
            app::Selection& selection = *session.selection();
            selection.selectClips(*session.editor(), {{trackId_, clip_.id}});
            if (selection.timeRange()) selection.setInsert(selection.timeRange()->start);
        } catch (const app::EditError& error) {
            Q_EMIT host_.hostArrangement()->statusMessage(error.message());
        }
    }
    session.bridge()->endClipPreview();  // (the model's clips: edited, or as they were)
}

void ClipEditGesture::cancel() {
    if (result_) host_.hostSession()->bridge()->endClipPreview();
}

// --- TrimGesture ----------------------------------------------------------------------------------

TrimGesture::TrimGesture(LanesHost& host, const QString& trackId, const app::Clip& clip, bool left)
    : ClipEditGesture(host, trackId, clip, QStringLiteral("Trim Clip")), left_(left) {}

void TrimGesture::move(const QPointF& pos, Qt::KeyboardModifiers modifiers) {
    const timeline::Timeline& view = host_.hostArrangement()->view();
    const double beat = view.snapBeat(view.xToBeat(pos.x()), modifiers & Qt::AltModifier);
    update(left_ ? app::edits::trimStart(clip_, beat, tempo()) : app::edits::trimEnd(clip_, beat, tempo()));
}

// --- StretchGesture -------------------------------------------------------------------------------

StretchGesture::StretchGesture(LanesHost& host, const QString& trackId, const app::Clip& clip, bool left)
    : ClipEditGesture(host, trackId, clip, QStringLiteral("Stretch Clip")), left_(left) {}

void StretchGesture::move(const QPointF& pos, Qt::KeyboardModifiers modifiers) {
    // (Alt started it: Shift takes it off the grid instead.)
    const timeline::Timeline& view = host_.hostArrangement()->view();
    const double beat = view.snapBeat(view.xToBeat(pos.x()), modifiers & Qt::ShiftModifier);
    update(app::edits::stretchClip(clip_, beat, left_, tempo()));
}

// --- SlipGesture ----------------------------------------------------------------------------------

SlipGesture::SlipGesture(LanesHost& host, const QString& trackId, const app::Clip& clip, const QPointF& press)
    : ClipEditGesture(host, trackId, clip, QStringLiteral("Move Clip Content")), press_(press) {}

void SlipGesture::move(const QPointF& pos, Qt::KeyboardModifiers modifiers) {
    if (!active_) {
        if (!farEnough(pos, press_)) return;
        active_ = true;
    }
    const timeline::Timeline& view = host_.hostArrangement()->view();
    double delta = view.xToBeat(pos.x()) - view.xToBeat(press_.x());
    // By whole grid steps, so content on the grid stays on it (Alt: freely).
    if (view.snap() && !(modifiers & Qt::AltModifier)) {
        const double step = view.gridStep();
        if (step > 0) delta = std::round(delta / step) * step;
    }
    update(app::edits::slipClip(clip_, delta, tempo()));
}

// --- FadeGesture ----------------------------------------------------------------------------------

QString fadeText(double seconds) {
    if (seconds < 1.0) {
        const double ms = seconds * 1000.0;
        return ms < 10.0 ? QStringLiteral("%1 ms").arg(ms, 0, 'f', 1) : QStringLiteral("%1 ms").arg(std::round(ms));
    }
    return QStringLiteral("%1 s").arg(seconds, 0, 'f', 2);
}

FadeGesture::FadeGesture(LanesHost& host, const QString& trackId, const app::Clip& clip, bool out,
                         const QPointF& press)
    : ClipEditGesture(host, trackId, clip, out ? QStringLiteral("Fade Out") : QStringLiteral("Fade In")), out_(out),
      press_(press) {}

void FadeGesture::move(const QPointF& pos, Qt::KeyboardModifiers) {
    const timeline::Timeline& view = host_.hostArrangement()->view();
    const double moved = view.xToBeat(pos.x()) - view.xToBeat(press_.x());
    const double tempo = this->tempo();
    const double from = out_ ? clip_.fadeOutBeats(tempo) : clip_.fadeInBeats(tempo);
    const app::Clip faded = app::edits::fadeClip(clip_, out_, from + (out_ ? -moved : moved), tempo);
    update(faded);
    const double beats = out_ ? faded.fadeOutBeats(tempo) : faded.fadeInBeats(tempo);
    readout_ = Readout{pos, (out_ ? QStringLiteral("Fade Out ") : QStringLiteral("Fade In ")) +
                                fadeText(app::beatsToSeconds(beats, tempo))};
}

// --- FadeCurveGesture -----------------------------------------------------------------------------

FadeCurveGesture::FadeCurveGesture(LanesHost& host, const QString& trackId, const app::Clip& clip, bool out,
                                   const QPointF& press)
    : ClipEditGesture(host, trackId, clip, QStringLiteral("Fade Curve")), out_(out), press_(press) {}

void FadeCurveGesture::move(const QPointF& pos, Qt::KeyboardModifiers modifiers) {
    const double fine = modifiers & Qt::ShiftModifier ? 0.1 : 1.0;
    const double from = out_ ? clip_.fadeOutCurve : clip_.fadeInCurve;
    update(app::edits::curveFade(clip_, out_, from + (press_.y() - pos.y()) / kCurvePixels * fine));
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
    const timeline::Timeline& view = arrangement.view();
    const double beat = std::max(0.0, view.snapBeat(view.xToBeat(pos.x()), modifiers & Qt::AltModifier));
    const double start = std::min(anchor_, beat), end = std::max(anchor_, beat);
    if (arrangement.layout().rows().empty()) return;
    const int row = host_.rowAt(pos.y(), true).value_or(anchorRow_);
    selectRows(host_, start, end, anchorRow_, row);
    host_.setHostCursor(QCursor(Qt::IBeamCursor));
}

// --- ExtendGesture -------------------------------------------------------------------------------

std::unique_ptr<ExtendGesture> ExtendGesture::start(LanesHost& host, const QPointF& press,
                                                    Qt::KeyboardModifiers modifiers) {
    if (!host.isTrackLanes()) return nullptr;
    const app::Selection& selection = *host.hostSession()->selection();
    const TrackLayout& layout = host.hostArrangement()->layout();
    std::unique_ptr<ExtendGesture> gesture(new ExtendGesture(host));
    const auto& range = selection.timeRange();
    if (range && !selection.lanes().isEmpty()) {  // a lane range: its lanes that show here
        const std::vector<EnvelopeArea> areas = host.envelopeAreas();
        std::optional<int> first, last;
        for (int i = 0; i < static_cast<int>(areas.size()); ++i) {
            const auto& area = areas[static_cast<size_t>(i)];
            if (!selection.lanes().contains(app::LaneRef{area.owner, area.key})) continue;
            if (!first) first = i;
            last = i;
        }
        if (!first) return nullptr;
        for (const app::LaneRef& lane : selection.lanes()) {
            const auto index = layout.indexOf(lane.first);
            if (!index) continue;
            gesture->laneRowsFirst_ = gesture->laneRowsFirst_ ? std::min(*gesture->laneRowsFirst_, *index) : *index;
            gesture->laneRowsLast_ = gesture->laneRowsLast_ ? std::max(*gesture->laneRowsLast_, *index) : *index;
        }
        gesture->lanes_ = true;
        gesture->first_ = *first;
        gesture->last_ = *last;
        gesture->start_ = range->start;
        gesture->end_ = range->end;
    } else if (range) {  // a clip range: the rows it covers
        std::optional<int> first, last;
        for (const QString& id : selection.rangeRows()) {
            const auto index = layout.indexOf(id);
            if (!index) continue;
            first = first ? std::min(*first, *index) : *index;
            last = last ? std::max(*last, *index) : *index;
        }
        if (!first) return nullptr;
        gesture->first_ = *first;
        gesture->last_ = *last;
        gesture->start_ = range->start;
        gesture->end_ = range->end;
    } else {  // from the insert marker on the selected track
        const auto index = layout.indexOf(selection.trackId());
        if (!index) return nullptr;
        gesture->first_ = gesture->last_ = *index;
        gesture->start_ = gesture->end_ = selection.insertBeat();
    }
    gesture->move(press, modifiers);
    return gesture;
}

void ExtendGesture::move(const QPointF& pos, Qt::KeyboardModifiers modifiers) {
    const timeline::Timeline& view = host_.hostArrangement()->view();
    const double beat = std::max(0.0, view.snapBeat(view.xToBeat(pos.x()), modifiers & Qt::AltModifier));
    const double start = std::min(start_, beat), end = std::max(end_, beat);
    if (end <= start) return;  // (nothing to select yet)
    if (lanes_ && host_.inClipBand(pos)) {  // into the clips: the clips of the tracks from its lanes' to there
        const auto row = host_.rowAt(pos.y(), true);
        if (!row) return;
        selectRows(host_, start, end, std::min(laneRowsFirst_.value_or(*row), *row),
                   std::max(laneRowsLast_.value_or(*row), *row));
    } else if (lanes_) {
        const std::vector<EnvelopeArea> areas = host_.envelopeAreas();
        std::optional<int> here;
        for (int i = 0; i < static_cast<int>(areas.size()); ++i) {
            if (areas[static_cast<size_t>(i)].rect.contains(pos)) here = i;
        }
        if (!here) here = envelopes::nearestArea(areas, pos.y());
        if (!here || first_ >= static_cast<int>(areas.size()) || last_ >= static_cast<int>(areas.size())) return;
        const int first = std::min(first_, *here), last = std::max(last_, *here);
        envelopes::selectLaneRange(host_, start, end,
                                   std::vector<EnvelopeArea>(areas.begin() + first, areas.begin() + last + 1));
    } else {
        const auto row = host_.rowAt(pos.y(), true);
        if (!row) return;
        selectRows(host_, start, end, std::min(first_, *row), std::max(last_, *row));
    }
    host_.setHostCursor(QCursor(Qt::IBeamCursor));
}

void selectRows(LanesHost& host, double start, double end, int firstRow, int lastRow) {
    app::Session& session = *host.hostSession();
    const auto& rows = host.hostArrangement()->layout().rows();
    if (rows.empty()) return;
    const int first = std::clamp(std::min(firstRow, lastRow), 0, static_cast<int>(rows.size()) - 1);
    const int last = std::clamp(std::max(firstRow, lastRow), 0, static_cast<int>(rows.size()) - 1);
    QStringList ids;
    for (int i = first; i <= last; ++i) ids << rows[static_cast<size_t>(i)].trackId;
    const QStringList trackIds = session.project()->withContents(ids);
    session.selection()->setTimeRange(start, end, trackIds, session.editor()->clipsInRange(start, end, trackIds), {},
                                      ids);
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
