#pragma once
// What is selected in the arrangement.
//
// The selected track (or tracks, a return track, or the master), the insert
// (start) marker, and a time selection: a beat range spanning one or more
// adjacent tracks. Selecting is always on the grid: selecting a clip selects
// the area it covers.
//
// A time selection made in the clips' band (or by clicking clips) is a clip
// range: `clips` holds the clips it touches (for the clip view) and Delete cuts
// out just that range. Made lower in the lanes it is a lane range; made in
// automation lanes, `lanes` holds them (owner, target key), and Delete clears
// their automation in the range.
//
// Automation breakpoints can be selected too (`points`: owner, key, indices);
// Delete deletes them.
//
// Several tracks can be selected (Ctrl-click toggles one, Shift-click selects
// the tracks from the last one clicked): trackIds(). trackId() is the one the
// device view shows, the last one clicked.

#include "editor/ClipRef.h"
#include "editor/TimeRange.h"
#include "model/Project.h"  // LaneRef

#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <optional>

namespace sub::app {

class ProjectEditor;

// Selected breakpoints of one envelope.
struct SelectedPoints {
    QString owner;
    QString key;
    QSet<int> indices;

    friend bool operator==(const SelectedPoints&, const SelectedPoints&) = default;
};

class Selection : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString trackId READ trackId NOTIFY changed)
    Q_PROPERTY(QStringList trackIds READ trackIds NOTIFY changed)
    Q_PROPERTY(double insertBeat READ insertBeat NOTIFY insertChanged)
    Q_PROPERTY(bool hasTimeRange READ hasTimeRange NOTIFY changed)
    Q_PROPERTY(double rangeStart READ rangeStart NOTIFY changed)
    Q_PROPERTY(double rangeEnd READ rangeEnd NOTIFY changed)
    Q_PROPERTY(QStringList rangeTrackIds READ rangeTrackIds NOTIFY changed)
    Q_PROPERTY(bool clipRange READ clipRange NOTIFY changed)
    Q_PROPERTY(Focus focus READ focus NOTIFY changed)

public:
    // What Delete (Cut, Copy) acts on.
    enum class Focus {
        Clips,       // the time selection (or the clips in it)
        Track,       // the selected tracks
        Devices,     // the device view's selected devices
        Automation,  // the selected breakpoints
    };
    Q_ENUM(Focus)

    // How selectTrack selects.
    enum class Mode {
        Replace,  // just this track
        Toggle,   // Ctrl-click: adds it to the selected tracks or takes it out
        Range,    // Shift-click: the tracks from the last one clicked to it
    };
    Q_ENUM(Mode)

    explicit Selection(QObject* parent = nullptr);

    const QSet<ClipRef>& clips() const { return clips_; }
    // The track the device view shows ("": none; kMaster: the master).
    QString trackId() const { return trackId_; }
    // The selected tracks, in the order they were selected (trackId among them).
    QStringList trackIds() const;
    double insertBeat() const { return insertBeat_; }
    const std::optional<TimeRange>& timeRange() const { return timeRange_; }
    bool hasTimeRange() const { return timeRange_.has_value(); }
    double rangeStart() const { return timeRange_ ? timeRange_->start : 0.0; }
    double rangeEnd() const { return timeRange_ ? timeRange_->end : 0.0; }
    QStringList rangeTrackIds() const { return timeRange_ ? timeRange_->trackIds : QStringList(); }
    // Whether the time selection selects clip content (not automation). Derived,
    // so clearing the time selection can never leave it stale.
    bool clipRange() const { return timeRange_.has_value() && rangeSelectsClips_; }
    // The automation lanes a lane range covers.
    const QList<LaneRef>& lanes() const { return lanes_; }
    // The selected automation breakpoints (none: none).
    const std::optional<SelectedPoints>& points() const { return points_; }
    Focus focus() const { return focus_; }

    // Select nothing (on `trackId`, if given: where the insert marker shows).
    Q_INVOKABLE void clear(const QString& trackId = {});
    // Select breakpoints of one envelope (none: select nothing).
    void selectPoints(const QString& owner, const QString& key, const QSet<int>& indices);
    Q_INVOKABLE void selectPoints(const QString& owner, const QString& key, const QList<int>& indices);
    QSet<int> selectedPoints(const QString& owner, const QString& key) const;
    // Select the grid area that fully contains these clips: from the earliest
    // start to the latest end, on every track from the first clip's to the
    // last's (on `trackId` if it is one of them).
    void selectClips(const ProjectEditor& editor, const ClipRefs& refs, const QString& trackId = {});
    // Select a track ("": none). `mode` Toggle (Ctrl-click) adds it to the
    // selected tracks or takes it out; Range (Shift-click) selects the tracks
    // from the last one clicked to it, in `order` (the tracks top to bottom).
    // With `focusTrack`, the tracks are what Cut, Copy and Delete act on.
    Q_INVOKABLE void selectTrack(const QString& trackId, bool focusTrack = false, Mode mode = Mode::Replace,
                                 const QStringList& order = {});
    // The selected tracks (as they are) are what Cut, Copy and Delete act on now.
    Q_INVOKABLE void focusTracks();
    // Devices were selected in the device view: Delete acts on them now.
    Q_INVOKABLE void focusDevices();
    // Select a beat range across tracks. With `clips` (the clips it touches,
    // possibly none) it is a clip range, otherwise a lane range: of automation
    // `lanes` (owner, key), if given (the master's have no track).
    void setTimeRange(double start, double end, const QStringList& trackIds,
                      const std::optional<QSet<ClipRef>>& clips = std::nullopt, const QList<LaneRef>& lanes = {});
    // A lane range over these tracks (no clips, no automation lanes).
    Q_INVOKABLE void setLaneRange(double start, double end, const QStringList& trackIds);
    Q_INVOKABLE void setInsert(double beat);
    // Drop references to clips, tracks and breakpoints that no longer exist.
    void prune(const Project& project);

Q_SIGNALS:
    void changed();
    void insertChanged();

private:
    void focusOnTracks();

    QSet<ClipRef> clips_;
    QString trackId_;
    QStringList tracks_;  // several selected tracks; only while trackId_ is one of them
    QString anchor_;  // where a Shift-click selects tracks from
    double insertBeat_ = 0.0;
    std::optional<TimeRange> timeRange_;
    bool rangeSelectsClips_ = false;
    QList<LaneRef> lanes_;
    std::optional<SelectedPoints> points_;
    Focus focus_ = Focus::Clips;
};

}  // namespace sub::app
