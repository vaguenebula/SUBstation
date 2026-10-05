#pragma once

// What the clip view shows and does (clip_view.py's ClipView without its
// widgets; ClipView.qml lays it out). It is given clips (`trackId`,
// `clipIds`, `leadClipId`) and shows them top track first, then by time. If
// the lead clip (the one double-clicked; by default the first) is a MIDI clip,
// it alone opens, in the piano roll; otherwise every audio clip among them
// opens together: their settings on the left, their waveforms on the right.
// Deleted clips (or tracks) are dropped; when none are left, or the project
// is reset, it asks to be closed (closeRequested). It shows them only while
// `active` (the view shown): becoming active opens them afresh (the piano
// roll fitted to the clip again), and going inactive forgets them, as the old
// view's open_clips and close_view did.
//
// For audio it edits every open clip at once, through the editor's
// updateClips. Knobs move all clips by the same amount (transposing up 2
// semitones transposes each clip by 2, whatever it was at), measured from where
// the gesture started, so a clip held at a limit keeps its offset to the others
// when the knob comes back; the knob sits on the first clip's value, and its
// readout shows the range when the clips differ. Switches, the warp mode and
// the segment BPM set the same value on all of them.
//
// Warp locks a clip to the beat grid: its audio is taken to be at the segment
// BPM and is stretched to follow the project tempo. Transpose and detune shift
// the pitch without changing the speed, warped or not (except in Re-Pitch
// mode, where speed and pitch move together like a turntable and transposing
// does nothing).

#include "editor/ClipRef.h"
#include "model/Clip.h"
#include "pianoroll/PianoRoll.h"
#include "session/Session.h"

#include <QColor>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace sub::app {
class Project;
}

namespace sub::ui {


class ClipViewController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(sub::app::Session* session READ session WRITE setSession NOTIFY sessionChanged)
    // The piano roll a MIDI clip opens in.
    Q_PROPERTY(sub::ui::PianoRoll* pianoRoll READ pianoRoll WRITE setPianoRoll NOTIFY pianoRollChanged)
    // The clips asked for: ids of clips on `trackId`, or {trackId, clipId}
    // maps (clips on several tracks); `leadClipId` (optional) leads.
    Q_PROPERTY(QString trackId READ trackId WRITE setTrackId NOTIFY requestChanged)
    Q_PROPERTY(QVariantList clipIds READ clipIds WRITE setClipIds NOTIFY requestChanged)
    Q_PROPERTY(QString leadClipId READ leadClipId WRITE setLeadClipId NOTIFY requestChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    // What shows: a MIDI clip in the piano roll, or audio clips (none: count 0).
    Q_PROPERTY(bool midi READ midi NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(QStringList shownTrackIds READ shownTrackIds NOTIFY changed)
    Q_PROPERTY(QStringList shownClipIds READ shownClipIds NOTIFY changed)
    // The header: a name, a line about the clips, the first one's track colour.
    Q_PROPERTY(QString name READ name NOTIFY changed)
    Q_PROPERTY(QString info READ info NOTIFY changed)
    Q_PROPERTY(QColor color READ color NOTIFY changed)
    // The audio clips' settings: Warp on for all, the warp mode (-1: they
    // differ, "Mixed"), the first clip's segment BPM, every clip Re-Pitch
    // (Transpose and Detune do nothing then).
    Q_PROPERTY(bool warp READ warp NOTIFY changed)
    Q_PROPERTY(int warpModeIndex READ warpModeIndex NOTIFY changed)
    Q_PROPERTY(double segmentBpm READ segmentBpm NOTIFY changed)
    Q_PROPERTY(bool repitch READ repitch NOTIFY changed)
    // The knobs, by name ("transpose", "detune", "gain", "pan"): each a map of
    // caption, from, to, defaultValue, bipolar, tooltip, and the clips' value
    // (the first's), text (its readout: the range when they differ) and differs.
    Q_PROPERTY(QVariantMap knobs READ knobs NOTIFY changed)
    Q_PROPERTY(QStringList warpModes READ warpModes CONSTANT)
    Q_PROPERTY(QStringList warpModeTips READ warpModeTips CONSTANT)

public:
    static constexpr int kControlsWidth = 260;
    static constexpr int kHeaderHeight = 30;
    static constexpr double kMinBpm = 20.0;
    static constexpr double kMaxBpm = 999.0;

    explicit ClipViewController(QObject* parent = nullptr);
    ~ClipViewController() override;

    app::Session* session() const { return session_; }
    void setSession(app::Session* session);
    PianoRoll* pianoRoll() const { return pianoRoll_; }
    void setPianoRoll(PianoRoll* roll);
    QString trackId() const { return trackId_; }
    void setTrackId(const QString& trackId);
    QVariantList clipIds() const { return clipIds_; }
    void setClipIds(const QVariantList& clipIds);
    QString leadClipId() const { return leadClipId_; }
    void setLeadClipId(const QString& clipId);
    bool active() const { return active_; }
    void setActive(bool active);

    // Show these clips (the first leads the knobs; `lead` decides MIDI or audio).
    void open(const app::ClipRefs& refs, const std::optional<app::ClipRef>& lead = std::nullopt);
    // The clips shown, in order.
    const app::ClipRefs& refs() const { return refs_; }

    bool midi() const { return midi_; }
    int count() const { return static_cast<int>(refs_.size()); }
    QStringList shownTrackIds() const;
    QStringList shownClipIds() const;
    QString name() const { return name_; }
    QString info() const { return info_; }
    QColor color() const { return color_; }
    bool warp() const { return warp_; }
    int warpModeIndex() const { return warpModeIndex_; }
    double segmentBpm() const { return segmentBpm_; }
    bool repitch() const { return repitch_; }
    QVariantMap knobs() const { return knobs_; }
    static QStringList warpModes();
    static QStringList warpModeTips();

    // Each open audio clip with its track's colour, for their waveforms.
    const std::vector<std::pair<app::Clip, QColor>>& audioClips() const { return audioClips_; }

    // Warping a clip whose segment BPM was never set takes the project tempo,
    // so the clip keeps its length and speed until the tempo changes.
    Q_INVOKABLE void setWarp(bool on);
    Q_INVOKABLE void setWarpMode(int index);
    Q_INVOKABLE void setSegmentBpm(double bpm, const QString& gestureKey = {});
    // :2 (0.5) and ×2 (2.0): each clip's own segment BPM.
    Q_INVOKABLE void scaleBpm(double factor);
    // A knob turned to `value`: every clip moves by the lead's change (one undo
    // step per gesture key).
    Q_INVOKABLE void nudge(const QString& knob, double value, const QString& gestureKey);
    // A knob's value as its readout and tooltip show it.
    Q_INVOKABLE QString format(const QString& knob, double value) const;

Q_SIGNALS:
    void sessionChanged();
    void pianoRollChanged();
    void requestChanged();
    void activeChanged();
    void changed();
    // Clips were opened (the view gives the notes, or itself, the keyboard).
    void opened();
    // Nothing is left to show (the clips were deleted, the project reset).
    void closeRequested();
    // An edit couldn't be made, and why.
    void statusMessage(const QString& message);

private:
    struct Shown {
        QString trackId;
        const app::Clip* clip;
    };

    app::Project* project() const;
    // Opens the clips asked for (while active).
    void reopen();
    // Shows nothing (without asking to be closed).
    void forget();
    // (track id, clip) for each clip of `refs` that still exists.
    std::vector<Shown> existing(const app::ClipRefs& refs) const;
    void close();
    void dropMissing();
    void refresh();
    void edit(const std::function<void()>& change);
    void update(const std::function<app::Clip(const app::Clip&)>& change, const QString& text,
                const QString& mergeKey = {});

    QPointer<app::Session> session_;
    QPointer<PianoRoll> pianoRoll_;
    QString trackId_;
    QVariantList clipIds_;
    QString leadClipId_;
    bool active_ = true;
    app::ClipRefs refs_;  // the first one leads: knobs show its values
    bool midi_ = false;
    QString name_;
    QString info_;
    QColor color_;
    bool warp_ = false;
    int warpModeIndex_ = -1;
    double segmentBpm_ = 120.0;
    bool repitch_ = false;
    QVariantMap knobs_;
    std::vector<std::pair<app::Clip, QColor>> audioClips_;
    // (gesture key, knob, {clip id: value when the gesture started})
    struct Baseline {
        QString key;
        QString knob;
        QMap<QString, double> values;
    };
    std::optional<Baseline> baseline_;
};

}  // namespace sub::ui
