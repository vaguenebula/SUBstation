#pragma once

// Mouse gestures on the track lanes: move or copy a time selection (a selected
// clip is one too), trim edges, select time, and scroll by hand.
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
#include <optional>
#include <tuple>
#include <vector>

namespace sub::ui::arrangement {

// Drag inside a clip range: move (Ctrl: copy) the selected stretch of clips,
// split at the range's edges (and the automation under it). A click without
// dragging calls `onClick`. While it drags, the clips play where they would land.
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

    LanesHost& host_;
    QPointF press_;
    std::function<void()> onClick_;
    double start_ = 0.0;
    double end_ = 0.0;
    QStringList trackIds_;
    double originBeat_ = 0.0;
    int originRow_ = 0;
    QSet<QString> touched_;
    std::vector<GestureClip> remnants_;
    std::vector<std::pair<int, app::Clip>> pieces_;
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
// across the tracks it crosses: everything on them in that range (the clips it
// touches, and the automation), and on every track in a group it crosses, as
// in Ableton. Delete, Cut, Copy, Paste and Duplicate act on all of it.
class TimeSelectGesture : public Gesture {
public:
    TimeSelectGesture(LanesHost& host, const QPointF& press, bool bypassSnap);

    void move(const QPointF& pos, Qt::KeyboardModifiers modifiers) override;

    double anchor() const { return anchor_; }
    void setAnchorRow(int row) { anchorRow_ = row; }
    int anchorRow() const { return anchorRow_; }

private:
    LanesHost& host_;
    QPointF press_;
    double anchor_ = 0.0;
    int anchorRow_ = 0;
    bool active_ = false;
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

}  // namespace sub::ui::arrangement
