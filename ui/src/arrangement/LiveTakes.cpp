#include "arrangement/LiveTakes.h"

#include "arrangement/ArrangementLanes.h"
#include "audio/EngineBridge.h"
#include "model/Project.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"
#include "timeline/Timeline.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace sub::ui {

using namespace arrangement;

LiveTakes::LiveTakes(QQuickItem* parent) : ArrangementItem(parent) {}

void LiveTakes::setTakes(std::optional<QMap<QString, app::LiveTake>> takes) {
    takes_ = std::move(takes);
    update();
}

const QMap<QString, app::LiveTake>& LiveTakes::takes() const {
    static const QMap<QString, app::LiveTake> kNone;
    if (takes_) return *takes_;
    return session() ? session()->bridge()->liveTakes() : kNone;
}

void LiveTakes::connectSession(app::Session* session) {
    app::EngineBridge* bridge = session->bridge();
    connect(bridge, &app::EngineBridge::recordingUpdated, this, &QQuickItem::update);
    connect(bridge, &app::EngineBridge::recordingChanged, this, &QQuickItem::update);
    connect(session->project(), &app::Project::trackChanged, this, &QQuickItem::update);
}

void LiveTakes::connectArrangement(Arrangement* arrangement) {
    // The takes grow up to the playhead: while there are some, it moves them.
    connect(arrangement, &Arrangement::playheadChanged, this, [this] {
        if (!takes().isEmpty()) update();
    });
    connect(arrangement, &Arrangement::vscrollChanged, this, &QQuickItem::update);
    connect(arrangement, &Arrangement::layoutChanged, this, &QQuickItem::update);
}

void LiveTakes::paint(SgPainter& p) {
    if (!ready()) return;
    const QMap<QString, app::LiveTake>& live = takes();
    if (live.isEmpty()) return;
    const QRectF visible = p.rect();
    const Arrangement& a = *arrangement();
    const app::Project& project = *session()->project();
    const int scroll = a.scrollY();
    for (int index : a.layout().visibleRows(scroll + visible.top(), scroll + visible.bottom() + 1)) {
        const Row& row = a.layout().rows()[static_cast<size_t>(index)];
        const auto take = live.constFind(row.trackId);
        const app::Track* track = project.findTrack(row.trackId);
        if (take == live.constEnd() || !take->started || !track) continue;
        drawTake(p, QColor(track->color), *take, row.top - scroll, row.mainHeight, row.bars, visible);
    }
}

void LiveTakes::drawTake(SgPainter& p, const QColor& trackColor, const app::LiveTake& take, double rowTop,
                         int rowHeight, bool folded, const QRectF& visible) const {
    const double rate = std::max(1.0, session()->bridge()->sampleRate());
    const double tempo = session()->project()->tempo();
    const timeline::Timeline& view = arrangement()->view();
    const double start = static_cast<double>(take.startSample) / rate * tempo / 60.0;
    double end = static_cast<double>(take.startSample + take.frames) / rate * tempo / 60.0;
    if (const auto playhead = arrangement()->playhead()) end = std::max(start, *playhead);
    const double x0 = view.beatToX(std::max(0.0, start)), x1 = view.beatToX(end);
    const QRectF rect(x0, rowTop + 1, std::max(1.0, x1 - x0), rowHeight - 3);
    if (rect.right() < visible.left() || rect.left() > visible.right()) return;
    p.save();
    p.setClipRect(rect.intersected(visible).adjusted(-1, -1, 1, 1));
    const double titleHeight = clipTitleHeight(rect.height(), folded);  // (as a clip's)
    p.fillRect(rect, trackColor.darker(160));
    if (titleHeight > 0) p.fillRect(QRectF(rect.left(), rect.top(), rect.width(), titleHeight), Theme::kRecordOn);
    const QRectF body = rect.adjusted(0, titleHeight + 1, 0, -1);
    const qint64 peaks = take.peakCount();
    if (take.midi) {
        drawNotes(p, take, end, body, visible);
    } else if (peaks > 0 && body.height() > 2) {
        // One column per pixel: the extremes of the peaks it covers.
        const double fpp = view.framesPerPixel(rate);
        const double takeX = view.beatToX(start);
        const int first = static_cast<int>(std::max(rect.left(), visible.left()));
        const int last = static_cast<int>(std::min(rect.right(), visible.right()));
        const auto indexAt = [&](double column) {
            return static_cast<qint64>((column - takeX) * fpp / app::LiveTake::kPeakFrames);
        };
        // The columns where a new peak starts (the first of each), and the peak each starts at.
        std::vector<std::pair<int, qint64>> starts;
        int lastColumn = -1;
        for (int column = first; column <= last; ++column) {
            const qint64 index = indexAt(column);
            if (index < 0 || index >= peaks) continue;
            lastColumn = column;
            if (starts.empty() || starts.back().second != index) starts.emplace_back(column, index);
        }
        if (!starts.empty()) {
            // The last column ends where the next pixel would start, not at the take's end.
            const qint64 stop = std::min(peaks, std::max(indexAt(lastColumn + 1.0), starts.back().second + 1));
            const double mid = body.center().y(), half = body.height() / 2;
            for (size_t i = 0; i < starts.size(); ++i) {
                const qint64 from = starts[i].second;
                const qint64 to = i + 1 < starts.size() ? starts[i + 1].second : stop;
                float low = take.peakMin(from), high = take.peakMax(from);
                for (qint64 k = from + 1; k < to; ++k) {
                    low = std::min(low, take.peakMin(k));
                    high = std::max(high, take.peakMax(k));
                }
                const double x = starts[i].first;
                p.drawLine(QPointF(x, mid - high * half), QPointF(x, mid - low * half), Theme::kWaveform);
            }
        }
    }
    p.drawRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), Theme::kRecordOn, 1);
    p.restore();
}

void LiveTakes::drawNotes(SgPainter& p, const app::LiveTake& take, double takeEnd, const QRectF& area,
                          const QRectF& visible) const {
    // A MIDI take's notes while it records, laid out as a clip's (held ones reach its end).
    if (take.notes.empty() || area.height() < 3) return;
    const double rate = std::max(1.0, session()->bridge()->sampleRate());
    const double beatsPerSample = session()->project()->tempo() / 60.0 / rate;
    int low = take.notes.front().key, high = low;
    for (const app::LiveNote& note : take.notes) {
        low = std::min(low, note.key);
        high = std::max(high, note.key);
    }
    const double row = std::min(area.height() / (high - low + 1), std::max(2.0, area.height() / 12));
    const double top = area.top() + (area.height() - row * (high - low + 1)) / 2;
    const double gap = row > 3 ? 1.0 : 0.0;
    const timeline::Timeline& view = arrangement()->view();
    for (const app::LiveNote& note : take.notes) {
        const double x0 = view.beatToX(static_cast<double>(note.start) * beatsPerSample);
        const double x1 = view.beatToX(note.end >= 0 ? static_cast<double>(note.end) * beatsPerSample : takeEnd);
        if (x1 >= visible.left() && x0 <= visible.right())
            p.fillRect(QRectF(x0, top + (high - note.key) * row, std::max(1.0, x1 - x0 - gap), std::max(1.0, row - gap)),
                       Theme::kWaveform);
    }
}

}  // namespace sub::ui
