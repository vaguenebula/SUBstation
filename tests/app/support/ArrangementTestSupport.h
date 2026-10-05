#pragma once
// What the arrangement's tests share (header only: it uses the UI): the
// arrangement view in a window on a UI session (UiTestSupport.h), its items
// found by name, points in its lanes and headers (in the window), the menus
// its items work out, and audio files to put in it.

#include "UiTestSupport.h"
#include "arrangement/Arrangement.h"
#include "arrangement/ArrangementLanes.h"
#include "arrangement/ArrangementRuler.h"
#include "arrangement/BusLane.h"
#include "arrangement/LiveTakes.h"
#include "arrangement/MenuEntries.h"
#include "arrangement/TrackHeaderItem.h"
#include "audio/EngineBridge.h"
#include "editor/ProjectEditor.h"
#include "model/Project.h"
#include "session/Selection.h"

#include <QDir>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>

#include <cmath>
#include <vector>

namespace sub::app::test {

// The window the arrangement's tests show (as the main window has it, a little smaller).
inline const char* const kArrangementWindow =
    "import QtQuick\n"
    "import SUBstation\n"
    "Window {\n"
    "    width: 1300\n"
    "    height: 720\n"
    "    color: Theme.window\n"
    "    ArrangementView { objectName: \"arrangementView\"; anchors.fill: parent }\n"
    "}\n";

inline constexpr double kPi = 3.14159265358979323846;

// Two seconds of a tone a channel (0.5 and 0.3 of full scale), as the Python tests' tone().
inline std::vector<float> tone(double seconds, double freq, int channels = 2) {
    const int frames = static_cast<int>(seconds * kSampleRate);
    std::vector<float> samples(static_cast<size_t>(frames * channels));
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        samples[static_cast<size_t>(i * channels)] = static_cast<float>(0.5 * std::sin(2 * kPi * freq * t));
        if (channels > 1)
            samples[static_cast<size_t>(i * channels + 1)] = static_cast<float>(0.3 * std::sin(2 * kPi * freq * 1.5 * t));
    }
    return samples;
}

// A constant level in every channel.
inline std::vector<float> constant(double seconds, float level, int channels = 2) {
    return std::vector<float>(static_cast<size_t>(seconds * kSampleRate) * static_cast<size_t>(channels), level);
}

// An item under `root` (itself too) by its objectName, in the item tree (a
// Repeater's items have no QObject parent: findChild misses them).
template <typename T>
T findIn(QQuickItem* root, const QString& name) {
    if (!root) return nullptr;
    if (root->objectName() == name) {
        if (auto* found = qobject_cast<T>(root)) return found;
    }
    for (QQuickItem* child : root->childItems()) {
        if (T found = findIn<T>(child, name)) return found;
    }
    return nullptr;
}

// Every item under `root` with this objectName, in the item tree's order (a
// Repeater's: by index).
template <typename T>
void findAllIn(QQuickItem* root, const QString& name, std::vector<T>& found) {
    if (!root) return;
    if (root->objectName() == name) {
        if (auto* item = qobject_cast<T>(root)) found.push_back(item);
    }
    for (QQuickItem* child : root->childItems()) findAllIn<T>(child, name, found);
}
template <typename T>
std::vector<T> findAllIn(QQuickItem* root, const QString& name) {
    std::vector<T> found;
    findAllIn<T>(root, name, found);
    return found;
}

class ArrangementHarness {
public:
    // An item of the window by its objectName.
    template <typename T>
    T find(const QString& name) const {
        return window_ ? findIn<T>(window_->contentItem(), name) : nullptr;
    }

    explicit ArrangementHarness(UiSession& ui) : ui_(ui) {}

    // Shows the window; false if it didn't load.
    bool show() {
        window_ = ui_.show(kArrangementWindow);
        if (!window_) return false;
        view_ = find<QQuickItem*>(QStringLiteral("arrangementView"));
        if (!view_) return false;
        arrangement_ = view_->property("arrangement").value<sub::ui::Arrangement*>();
        lanes_ = find<sub::ui::ArrangementLanes*>(QStringLiteral("lanes"));
        ruler_ = find<sub::ui::ArrangementRuler*>(QStringLiteral("ruler"));
        masterLane_ = find<sub::ui::BusLane*>(QStringLiteral("masterLane"));
        return arrangement_ && lanes_ && ruler_ && masterLane_;
    }

    QQuickWindow* window() const { return window_; }
    QQuickItem* view() const { return view_; }
    sub::ui::Arrangement* arrangement() const { return arrangement_; }
    sub::ui::ArrangementLanes* lanes() const { return lanes_; }
    sub::ui::ArrangementRuler* ruler() const { return ruler_; }
    sub::ui::BusLane* masterLane() const { return masterLane_; }
    sub::ui::LiveTakes* liveTakes() const { return find<sub::ui::LiveTakes*>(QStringLiteral("liveTakes")); }
    Session& session() const { return ui_.session(); }
    Project& project() const { return *ui_.session().project(); }
    ProjectEditor& editor() const { return *ui_.session().editor(); }
    Selection& selection() const { return *ui_.session().selection(); }
    EngineBridge& bridge() const { return *ui_.session().bridge(); }
    const sub::ui::timeline::Timeline& timeline() const { return arrangement_->view(); }

    // A track's header (a group's), a return's, the master's; null if there is none (yet).
    sub::ui::TrackHeaderItem* header(const QString& trackId) const {
        return find<sub::ui::TrackHeaderItem*>(QStringLiteral("header:") + trackId);
    }
    sub::ui::TrackHeaderItem* returnHeader(const QString& returnId) const {
        return find<sub::ui::TrackHeaderItem*>(QStringLiteral("returnHeader:") + returnId);
    }
    sub::ui::BusLane* returnLane(const QString& returnId) const {
        return find<sub::ui::BusLane*>(QStringLiteral("returnLane:") + returnId);
    }
    sub::ui::TrackHeaderItem* masterHeader() const {
        return find<sub::ui::TrackHeaderItem*>(QStringLiteral("masterHeader"));
    }
    // A control of a header (its objectName: "solo", "volume", "pan", "input"...).
    QQuickItem* control(QQuickItem* header, const QString& name) const {
        return header ? findIn<QQuickItem*>(header, name) : nullptr;
    }

    const sub::ui::arrangement::Row& row(int index) const {
        return arrangement_->layout().rows()[static_cast<size_t>(index)];
    }
    const sub::ui::arrangement::Row& rowOf(const QString& trackId) const { return *arrangement_->layout().rowFor(trackId); }

    // The x of a beat in the lanes.
    int x(double beat) const { return static_cast<int>(timeline().beatToX(beat)); }
    // A point of a track's row (by index), in the lanes: its title bar ("title", 6 px down),
    // or the middle of its row ("body").
    QPointF lanePoint(int index, double beat, bool title = false) const {
        const auto& r = row(index);
        const double top = r.top - arrangement_->scrollY();
        return QPointF(x(beat), top + (title ? 6 : r.height() / 2));
    }
    // A point of the lanes, in the window.
    QPoint at(const QPointF& lanesPoint) const { return sub::app::test::at(lanes_, lanesPoint); }
    QPoint lane(int index, double beat, bool title = false) const { return at(lanePoint(index, beat, title)); }

    // The zoom as it is (pixels a beat).
    void setZoom(double pxPerBeat) {
        arrangement_->setPxPerBeat(pxPerBeat);
        QCoreApplication::processEvents();
    }

    // Until the lanes have drawn (a frame after a change).
    void settle() {
        QCoreApplication::processEvents();
        QTest::qWait(30);
    }

private:
    UiSession& ui_;
    QQuickWindow* window_ = nullptr;
    QQuickItem* view_ = nullptr;
    sub::ui::Arrangement* arrangement_ = nullptr;
    sub::ui::ArrangementLanes* lanes_ = nullptr;
    sub::ui::ArrangementRuler* ruler_ = nullptr;
    sub::ui::BusLane* masterLane_ = nullptr;
};

// Mouse moves with the left button held and modifiers, without a press
// (UiTestSupport's moveTo, at points of the window).
inline void dragWith(QQuickWindow* window, QPoint start, QPoint end, Qt::KeyboardModifiers modifiers) {
    drag(window, start, end, modifiers);
}

}  // namespace sub::app::test
