#include "devices/SampleView.h"

#include "audio/AudioFiles.h"
#include "audio/EngineBridge.h"
#include "controls/KnobItem.h"
#include "editor/ProjectEditor.h"
#include "model/Device.h"
#include "model/DeviceState.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QDir>
#include <QFileInfo>
#include <QMimeData>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>

namespace sub::ui {

std::pair<std::vector<float>, std::vector<float>> waveformColumns(const sub::app::Waveform& waveform, int width) {
    width = std::max(1, width);
    std::vector<float> lows(std::size_t(width), 0.0f), highs(std::size_t(width), 0.0f);
    if (waveform.isNull())
        return {lows, highs};
    int level = 0;
    const double perColumn = double(waveform.frames()) / width;
    while (level + 1 < waveform.peakLevels() && sub::app::Waveform::samplesPerPeak(level + 1) <= perColumn)
        ++level;
    const qint64 count = waveform.peakCount(level);
    const float* peaks = waveform.peaks(level);
    const int channels = waveform.channels();
    if (count <= 0 || peaks == nullptr || channels <= 0)
        return {lows, highs};
    // Each peak's lowest and highest over the channels.
    std::vector<float> lo(static_cast<std::size_t>(count)), hi(static_cast<std::size_t>(count));
    for (qint64 i = 0; i < count; ++i) {
        float a = peaks[(i * channels) * 2], b = peaks[(i * channels) * 2 + 1];
        for (int c = 1; c < channels; ++c) {
            a = std::min(a, peaks[(i * channels + c) * 2]);
            b = std::max(b, peaks[(i * channels + c) * 2 + 1]);
        }
        lo[std::size_t(i)] = a;
        hi[std::size_t(i)] = b;
    }
    // numpy's minimum/maximum.reduceat over each column's first peak (never going back): to the
    // next column's first, the last to the end; where the next starts no later, just its first.
    std::vector<qint64> starts(static_cast<std::size_t>(width));
    for (int i = 0; i < width; ++i) {
        const qint64 edge = std::min(static_cast<qint64>(i * (double(count) / width)), count - 1);
        starts[std::size_t(i)] = i > 0 ? std::max(edge, starts[std::size_t(i) - 1]) : edge;
    }
    for (int i = 0; i < width; ++i) {
        const qint64 from = starts[std::size_t(i)];
        const qint64 to = i + 1 < width ? starts[std::size_t(i) + 1] : count;
        float a = lo[std::size_t(from)], b = hi[std::size_t(from)];
        for (qint64 k = from + 1; k < to; ++k) {
            a = std::min(a, lo[std::size_t(k)]);
            b = std::max(b, hi[std::size_t(k)]);
        }
        lows[std::size_t(i)] = a;
        highs[std::size_t(i)] = b;
    }
    return {lows, highs};
}

SampleView::SampleView(QQuickItem* parent) : DeviceCanvas(parent) {
    setImplicitSize(kWidth, kMinimumHeight);
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
    setFlag(ItemAcceptsDrops, true);
}

QString SampleView::samplePath() const { return path_; }

QUrl SampleView::sampleFolder() const {
    return path_.isEmpty() ? QUrl() : QUrl::fromLocalFile(QFileInfo(path_).absolutePath());
}

void SampleView::setPlayhead(double where) {
    if (where == playhead_)
        return;
    playhead_ = where;
    update();
    Q_EMIT playheadChanged();
}

QRectF SampleView::plot() const { return QRectF(0, 0, width(), height()).adjusted(1, 14, -1, -1); }

double SampleView::xOf(double percent) const {
    const QRectF r = plot();
    return r.left() + percent / 100.0 * r.width();
}

QString SampleView::markerAt(double x) const {
    if (waveform_.isNull())
        return {};
    const double start = std::abs(xOf(start_) - x), end = std::abs(xOf(end_) - x);
    const bool isStart = start <= end;  // (the first on a tie)
    return (isStart ? start : end) <= kMarkerGrab ? (isStart ? QStringLiteral("start") : QStringLiteral("end"))
                                                  : QString();
}

// --- The sample ------------------------------------------------------------------------

void SampleView::readSample() {
    const sub::app::Device* found = device();
    const QString path =
        found ? sub::app::deviceState::fromModel(found->state).value(QStringLiteral("sample")) : QString();
    sub::app::Waveform waveform;
    QString error;
    if (!path.isEmpty() && session()) {
        sub::app::EngineBridge* bridge = session()->bridge();
        waveform = bridge->waveform(path);
        error = bridge->loadError(path);
        // The engine decoded it for the sampler already: this is quick.
        if (waveform.isNull() && error.isEmpty() && !bridge->isLoading(path)) {
            const QPointer<SampleView> self(this);
            bridge->requestSource(path, [self, path] {
                if (self && self->path_ == path)
                    self->readSample();
            });
            waveform = bridge->waveform(path);  // (it may have been at once)
            error = bridge->loadError(path);
        }
    }
    const bool changed = path != path_ || !(waveform == waveform_) || error != error_;
    path_ = path;
    waveform_ = waveform;
    error_ = error;
    updateColumns();
    update();
    if (changed)
        Q_EMIT sampleChanged();
}

void SampleView::loadSampleUrl(const QUrl& url) { loadSample(url.isLocalFile() ? url.toLocalFile() : url.toString()); }

void SampleView::loadSample(const QString& path) {
    const sub::app::Device* found = device();
    if (found == nullptr)
        return;
    sub::app::deviceState::Values values;
    if (!path.isEmpty())
        values.insert(QStringLiteral("sample"), QDir::cleanPath(path));
    const std::optional<QString> state = sub::app::deviceState::toModel(values);
    if (state == found->state)
        return;
    const std::optional<QString> old = found->state;
    session()->editor()->setDeviceState(trackId(), deviceId(), old, state,
                                        path.isEmpty() ? QStringLiteral("Clear Sample") : QStringLiteral("Load Sample"));
}

void SampleView::sync() {
    start_ = value(QStringLiteral("start"));
    end_ = value(QStringLiteral("end"));
    looping_ = value(QStringLiteral("loop")) >= 0.5;
    if (session() && !connectedSources_) {
        connectedSources_ = true;
        auto changed = [this](const QString& path) {
            if (path == path_)
                readSample();
        };
        connect(session()->bridge(), &sub::app::EngineBridge::sourceReady, this, changed);
        connect(session()->bridge(), &sub::app::EngineBridge::sourceFailed, this,
                [changed](const QString& path, const QString&) { changed(path); });
    }
    readSample();
}

void SampleView::stateChanged() { sync(); }

void SampleView::refreshDisplays() {
    const std::vector<float> positions = readDisplay(QStringLiteral("position"));
    if (!positions.empty())
        setPlayhead(positions.back());
}

void SampleView::updateColumns() {
    const int columns = std::max(1, int(plot().width()));
    const QString key = path_ + QLatin1Char('|') + QString::number(waveform_.frames()) + QLatin1Char('|') +
                        QString::number(columns) + QLatin1Char('|') + QString::number(plot().height());
    if (waveform_.isNull()) {
        top_.clear();
        bottom_.clear();
        columnsKey_.clear();
        return;
    }
    if (key == columnsKey_)
        return;
    columnsKey_ = key;
    const auto [lo, hi] = waveformColumns(waveform_, columns);
    const QRectF r = plot();
    const double middle = r.center().y(), half = r.height() / 2;
    top_.resize(lo.size());
    bottom_.resize(lo.size());
    for (std::size_t x = 0; x < lo.size(); ++x) {
        const double top = middle - std::min(1.0, double(hi[x])) * half;
        const double bottom = middle - std::max(-1.0, double(lo[x])) * half;
        top_[x] = float(top);
        bottom_[x] = float(std::max(bottom, top + 1));
    }
}

void SampleView::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    DeviceCanvas::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        updateColumns();
}

// --- Markers -----------------------------------------------------------------------------

void SampleView::setDrag(const QString& marker) {
    if (marker == drag_)
        return;
    drag_ = marker;
    Q_EMIT markerChanged();
}

void SampleView::mousePressEvent(QMouseEvent* event) {
    const QString marker = event->button() == Qt::LeftButton ? markerAt(event->position().x()) : QString();
    if (marker.isEmpty()) {
        event->ignore();  // (to the device: selecting it)
        return;
    }
    gesture_ = newGestureKey();
    setDrag(marker);
    touch(marker);
}

void SampleView::mouseMoveEvent(QMouseEvent* event) {
    if (drag_.isEmpty())
        return;
    const QRectF r = plot();
    double percent = std::clamp((event->position().x() - r.left()) / r.width() * 100.0, 0.0, 100.0);
    if (drag_ == QLatin1String("start"))
        percent = std::min(percent, value(QStringLiteral("end")));
    else
        percent = std::max(percent, value(QStringLiteral("start")));
    setParam(drag_, percent, gesture_);
}

void SampleView::mouseReleaseEvent(QMouseEvent*) { setDrag(QString()); }

void SampleView::mouseUngrabEvent() { setDrag(QString()); }

void SampleView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton)
        Q_EMIT browseRequested();
}

void SampleView::hoverMoveEvent(QHoverEvent* event) {
    if (drag_.isEmpty())
        setCursor(markerAt(event->position().x()).isEmpty() ? Qt::ArrowCursor : Qt::SizeHorCursor);
}

// --- Dropping files --------------------------------------------------------------------------

QString SampleView::droppedFile(const QDropEvent* event) {
    const QMimeData* mime = event->mimeData();
    if (mime == nullptr)
        return {};
    for (const QUrl& url : mime->urls()) {
        const QString path = url.toLocalFile();
        if (!path.isEmpty() && sub::app::isAudioFile(path))
            return path;
    }
    return {};
}

void SampleView::dragEnterEvent(QDragEnterEvent* event) {
    if (!droppedFile(event).isEmpty())
        event->acceptProposedAction();
    else
        event->ignore();
}

void SampleView::dragMoveEvent(QDragMoveEvent* event) {
    if (!droppedFile(event).isEmpty())
        event->acceptProposedAction();
    else
        event->ignore();
}

void SampleView::dropEvent(QDropEvent* event) {
    const QString path = droppedFile(event);
    if (path.isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    loadSample(path);
}

// --- Drawing -------------------------------------------------------------------------------

void SampleView::paint(SgPainter& p) {
    const QRectF all(0, 0, width(), height());
    p.fillRect(all, Theme::kMeterBg);
    const QRectF r = plot();
    const QFont font = uiFont(8);
    if (path_.isEmpty()) {
        p.drawText(all, Qt::AlignCenter, QStringLiteral("Drop a sample here\nor double-click to browse"),
                   Theme::kTextDim, font);
        return;
    }
    const QString name = QFileInfo(path_).fileName();
    const QRectF title(4, 0, width() - 8, r.top());
    p.drawText(title, Qt::AlignVCenter | Qt::AlignLeft,
               SgPainter::elidedText(error_.isEmpty() ? name : QStringLiteral("Missing: ") + name, font,
                                     title.width(), Qt::ElideMiddle),
               error_.isEmpty() ? Theme::kTextDim : Theme::kRecordOn, font);
    if (waveform_.isNull()) {
        if (error_.isEmpty())
            p.drawText(r, Qt::AlignCenter, QStringLiteral("Loading…"), Theme::kTextDim, font);
        return;
    }
    if (!top_.empty())
        p.fillColumns(r.left(), 1.0, top_.data(), bottom_.data(), int(top_.size()), Theme::kAccent, 1.0);

    const double start = xOf(start_), end = xOf(end_);
    p.fillRect(QRectF(r.left(), r.top(), start - r.left(), r.height()), Theme::kOutsideClip);
    p.fillRect(QRectF(end, r.top(), r.right() - end, r.height()), Theme::kOutsideClip);
    const QColor marker = looping_ ? Theme::kText : Theme::kAccent;
    for (double x : {start, end})
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()), marker, 1.5);
    if (looping_)  // a bracket over what loops
        p.drawLine(QPointF(start, r.top() + 1), QPointF(end, r.top() + 1), marker, 1.5);
    if (playhead_ >= 0) {
        const double x = r.left() + playhead_ * r.width();
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()), Theme::kPlayhead, 1);
    }
}

}  // namespace sub::ui
