#pragma once

// Mouse gestures on the track lanes: move or copy a time selection (a selected
// clip is one too), trim edges, select time (and extend a selection with
// Shift), and scroll by hand.
//
// Moving and trimming clips are heard as they go: the engine plays what the
// drag would make of the clips (EngineBridge::previewClips) while the model
// waits for the drag to end, which makes one undo step.

#include "arrangement/Envelopes.h"
#include "arrangement/Gesture.h"
#include "model/Clip.h"

#include <QPointF>
#include <QSet>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

namespace sub::ui::arrangement {

// Drag inside a clip range: move (Ctrl: copy) the selected stretch of clips,
// split at the range's edges (and the automation under it). A click without
// dragging calls `onClick`. While it drags, the clips play where they would
// land (frozen tracks: their frozen audio, which moves with them). On a frozen
// track the stretch replaces everything where it lands, as its audio does: the
// clips there are drawn cut away.
class MoveRangeGesture : public Gesture {
public:
    MoveRangeGesture(LanesHost& host, const QPointF& press, std::function<void()> onClick = {});

    void move(const QPointF& pos, Qt::KeyboardModifiers modifiers) override;
    void finish() override;
    void cancel() override;
    QSet<QString> hiddenIds() const override;
    std::vector<GestureClip> ghosts() const override;
    std::vector<GestureClip> kept() const override;
    std::optional<app::TimeRange> timeRange() const override;

private:
    void preview();

    // A frozen track's clips: all of them are drawn by the gesture (`kept`).
    struct FrozenRow {
        int row = 0;
        QColor color;
        std::vector<app::Clip> clips;
    };

    LanesHost& host_;
    QPointF press_;
    std::function<void()> onClick_;
    double start_ = 0.0;
    double end_ = 0.0;
    QStringList trackIds_;
    QStringList rows_;  // the rows the selection covers (Selection::rangeRows)
    double originBeat_ = 0.0;
    int originRow_ = 0;
    QSet<QString> touched_;
    std::vector<GestureClip> remnants_;
    std::vector<std::pair<int, app::Clip>> pieces_;
    std::vector<FrozenRow> frozen_;
    QSet<QString> frozenIds_;
    std::vector<GestureClip> frozenKept_;  // what stays of frozen tracks' clips where they are
    double delta_ = 0.0;
    int trackDelta_ = 0;
    bool copy_ = false;
    bool active_ = false;
    std::optional<std::tuple<double, int, bool>> previewed_;  // (delta, track delta, copy) the engine plays
};

// Drag a clip's edge: trim it. While it drags, the clip plays trimmed.
class TrimGesture : public Gesture {
public:
    TrimGesture(LanesHost& host, const QString& trackId, const app::Clip& clip, bool left);

    void move(const QPointF& pos, Qt::KeyboardModifiers modifiers) override;
    void finish() override;
    void cancel() override;
    QSet<QString> hiddenIds() const override;
    std::vector<GestureClip> ghosts() const override;
    std::optional<app::TimeRange> timeRange() const override;

private:
    LanesHost& host_;
    QString trackId_;
    app::Clip clip_;
    bool left_;
    int row_ = 0;
    QColor color_;
    std::optional<app::Clip> result_;
};

// Click places the insert marker; drag selects a time range on the grid,
// across the tracks it crosses (folded and frozen ones too): everything on them
// in that range (the clips it touches, and the automation), and on every track
// in a group it crosses, as in Ableton: a clip range, wherever in the lanes it
// goes. Delete, Cut, Copy, Paste and Duplicate act on all of it; it shows over
// the rows it crosses.
class TimeSelectGesture : public Gesture {
public:
    TimeSelectGesture(LanesHost& host, const QPointF& press, bool bypassSnap);

    void move(const QPointF& pos, Qt::KeyboardModifiers modifiers) override;

    double anchor() const { return anchor_; }

private:
    LanesHost& host_;
    QPointF press_;
    double anchor_ = 0.0;
    int anchorRow_ = 0;
    bool active_ = false;
};

// Shift-click (and a drag on from it): the time selection extended to where the
// mouse is, over the time between and everything in that direction, as the
// drag that made it would have gone on: where it was made decides what it
// selects. A clip range takes in every track from its rows to the one there
// (folded, frozen, groups and what is in them); a lane range every automation
// lane showing from its lanes to the one there (or the nearest), of every
// track. With no time selection it extends from the insert marker on the
// selected track (a clip range).
class ExtendGesture : public Gesture {
public:
    // None if there is nothing to extend: no time selection whose rows or lanes
    // show here, nor a selected track of the arrangement.
    static std::unique_ptr<ExtendGesture> start(LanesHost& host, const QPointF& press, Qt::KeyboardModifiers modifiers);

    void move(const QPointF& pos, Qt::KeyboardModifiers modifiers) override;

private:
    explicit ExtendGesture(LanesHost& host) : host_(host) {}

    LanesHost& host_;
    double start_ = 0.0;
    double end_ = 0.0;
    bool lanes_ = false;  // a lane range (else a clip range)
    int first_ = 0;  // the rows (or lanes: indices in envelopeAreas) it covered at the press
    int last_ = 0;
};

// Ctrl+Alt drag: scroll the arrangement in both directions (Ableton's hand).
class PanGesture : public Gesture {
public:
    PanGesture(LanesHost& host, const QPointF& press);

    void move(const QPointF& pos, Qt::KeyboardModifiers modifiers) override;

private:
    LanesHost& host_;
    QPointF press_;
    double scrollBeats_ = 0.0;
    int scrollY_ = 0;
};

// Ctrl+Alt: drag to scroll the arrangement, as in Ableton.
bool isPanModifier(Qt::KeyboardModifiers modifiers);

// Select the time from `start` to `end` on the rows from `firstRow` to
// `lastRow` (indices into the layout's rows, either way round): a clip range
// over their tracks and what is in the groups among them, shown over those
// rows. The insert marker goes to its start.
void selectRows(LanesHost& host, double start, double end, int firstRow, int lastRow);

}  // namespace sub::ui::arrangement
