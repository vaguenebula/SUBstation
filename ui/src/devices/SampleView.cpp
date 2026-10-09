#include "devices/SampleView.h"

#include "audio/AudioFiles.h"
#include "audio/EngineBridge.h"
#include "controls/KnobItem.h"
#include "editor/ProjectEditor.h"
#include "model/Device.h"
#include "model/DeviceState.h"
#include "model/Project.h"
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

namespace {

// The sampler's modes (its "mode" list).
constexpr int kClassic = 0;
constexpr int kOneShot = 1;
constexpr int kSlice = 2;

// A marker's flag: a small triangle at the top of its line, pointing into what plays.
QPolygonF flag(double x, double top, bool pointsRight) {
    const double w = pointsRight ? 6.0 : -6.0;
    return QPolygonF({QPointF(x, top), QPointF(x + w, top), QPointF(x, top + 7.0)});
}

// "m:ss:mmm", as Simpler's ruler counts.
QString timeText(double seconds) {
    const auto ms = static_cast<qint64>(std::llround(seconds * 1000.0));
    return QStringLiteral("%1:%2:%3")
        .arg(ms / 60000)
        .arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

}  // namespace

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

QList<qreal> SampleView::slices() const {
    QList<qreal> result;
    const qint64 frames = waveform_.isNull() ? 0 : waveform_.frames();
    if (mode_ != kSlice || frames <= 0)
        return result;
    for (const qint64 start : slices_)
        result.append(double(start) / double(frames));
    return result;
}

int SampleView::playingSlice() const {
    if (mode_ != kSlice || playhead_ < 0 || waveform_.isNull())
        return -1;
    const double frame = playhead_ * double(waveform_.frames());
    if (frame < double(startFrame_) || frame >= double(endFrame_))
        return -1;
    const auto after = std::upper_bound(slices_.begin(), slices_.end(), qint64(frame));
    return after == slices_.begin() ? -1 : int(after - slices_.begin()) - 1;
}

void SampleView::setPlayhead(double where) {
    if (where == playhead_)
        return;
    playhead_ = where;
    update();
    Q_EMIT playheadChanged();
}

QRectF SampleView::plot() const { return QRectF(0, 0, width(), height()).adjusted(1, 1, -1, -1 - kRulerHeight); }

double SampleView::xOf(double percent) const {
    const QRectF r = plot();
    return r.left() + percent / 100.0 * r.width();
}

double SampleView::xOfFrame(double frame) const {
    const qint64 frames = waveform_.isNull() ? 0 : waveform_.frames();
    const QRectF r = plot();
    return frames > 0 ? r.left() + frame / double(frames) * r.width() : r.left();
}

qint64 SampleView::frameOf(double percent) const {
    const qint64 frames = waveform_.frames();
    const auto frame = static_cast<qint64>(percent / 100.0 * double(frames));
    return snap_ ? sub::app::sampleSlices::nearestZeroCrossing(waveform_, reverse_, frame) : frame;
}

QString SampleView::markerAt(double x, double y) const {
    if (waveform_.isNull())
        return {};
    const bool loops = mode_ == kClassic && looping_;
    const double loopX = xOfFrame(double(loopFrame_));
    if (loops && y >= plot().bottom() - kLoopHandle && std::abs(loopX - x) <= kMarkerGrab)
        return QStringLiteral("loop");  // its handle (where it is on Start, the only way to it)
    QString nearest;
    double distance = kMarkerGrab;
    const auto consider = [&](const QString& marker, double at) {
        const double d = std::abs(at - x);
        if (d <= distance && (nearest.isEmpty() || d < distance)) {  // (the first on a tie)
            nearest = marker;
            distance = d;
        }
    };
    consider(QStringLiteral("start"), xOfFrame(double(startFrame_)));
    consider(QStringLiteral("end"), xOfFrame(double(endFrame_)));
    if (loops)
        consider(QStringLiteral("loop"), loopX);
    return nearest;
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
    if (!(waveform == waveform_))
        transientsFound_ = {};
    path_ = path;
    waveform_ = waveform;
    error_ = error;
    updateColumns();
    updateLayout();
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
    mode_ = std::clamp(int(std::lround(value(QStringLiteral("mode")))), kClassic, kSlice);
    start_ = value(QStringLiteral("start"));
    end_ = value(QStringLiteral("end"));
    looping_ = value(QStringLiteral("loop")) >= 0.5;
    loopStart_ = value(QStringLiteral("loop_start"));
    loopFade_ = value(QStringLiteral("loop_fade"));
    reverse_ = value(QStringLiteral("reverse")) >= 0.5;
    snap_ = value(QStringLiteral("snap")) >= 0.5;
    fadeIn_ = value(QStringLiteral("fade_in"));
    fadeOut_ = value(QStringLiteral("fade_out"));
    sliceSettings_.by = static_cast<sub::app::sampleSlices::SliceBy>(
        std::clamp(int(std::lround(value(QStringLiteral("slice_by")))), 0, 2));
    sliceSettings_.sensitivity = value(QStringLiteral("sensitivity")) / 100.0;
    sliceSettings_.divisionBeats =
        sub::app::sampleSlices::divisionBeats(int(std::lround(value(QStringLiteral("slice_beat")))));
    sliceSettings_.regions = int(std::lround(value(QStringLiteral("regions"))));
    const double beats = std::max(1.0, std::round(value(QStringLiteral("warp_beats"))));
    sliceSettings_.regionBeats = beats;  // (of the whole sample: updateLayout() takes Start..End's share)
    warp_ = value(QStringLiteral("warp")) >= 0.5;
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

// Where Start, End, the loop and the slices are, as the sampler places them.
void SampleView::updateLayout() {
    const QList<qreal> before = slices();
    slices_.clear();
    startFrame_ = endFrame_ = loopFrame_ = fadeFrames_ = 0;
    const qint64 frames = waveform_.isNull() ? 0 : waveform_.frames();
    // Warped, the whole sample lasts its beats at the song's tempo.
    rate_ = 1.0;
    if (warp_ && frames > 0 && session()) {
        const double tempo = std::max(1.0, session()->project()->tempo());
        rate_ = waveform_.duration() / (sliceSettings_.regionBeats * 60.0 / tempo);
    }
    if (frames > 0) {
        startFrame_ = std::clamp<qint64>(frameOf(start_), 0, frames - 1);
        endFrame_ = std::clamp<qint64>(frameOf(end_), 0, frames);
        if (endFrame_ > startFrame_) {
            const qint64 loopAt = static_cast<qint64>(loopStart_ / 100.0 * double(frames));
            loopFrame_ = std::clamp<qint64>(frameOf(100.0 * double(std::max(loopAt, startFrame_)) / double(frames)),
                                            startFrame_, endFrame_ - 1);
            fadeFrames_ = std::min<qint64>(std::llround(loopFade_ / 100.0 * double(endFrame_ - loopFrame_)), loopFrame_);
        }
        if (mode_ == kSlice && endFrame_ > startFrame_) {
            const int way = reverse_ ? 1 : 0;
            if (sliceSettings_.by == sub::app::sampleSlices::SliceBy::Transient && !transientsFound_[way]) {
                transients_[way] = sub::app::sampleSlices::transients(waveform_, reverse_);
                transientsFound_[way] = true;
            }
            sub::app::sampleSlices::Settings settings = sliceSettings_;
            settings.regionBeats = sliceSettings_.regionBeats * double(endFrame_ - startFrame_) / double(frames);
            slices_ = sub::app::sampleSlices::sliceStarts(settings, transients_[way], startFrame_, endFrame_,
                                                          waveform_.sampleRate());
            if (snap_) {
                for (std::size_t i = 1; i < slices_.size(); ++i)
                    slices_[i] = sub::app::sampleSlices::nearestZeroCrossing(waveform_, reverse_, slices_[i]);
            }
        }
    }
    Q_EMIT layoutChanged();
    if (slices() != before)
        Q_EMIT playheadChanged();  // (the slice playing may be another)
}

void SampleView::updateColumns() {
    const int columns = std::max(1, int(plot().width()));
    const QString key = path_ + QLatin1Char('|') + QString::number(waveform_.frames()) + QLatin1Char('|') +
                        QString::number(columns) + QLatin1Char('|') + QString::number(plot().height()) +
                        QLatin1Char('|') + QString::number(reverse_);
    if (waveform_.isNull()) {
        top_.clear();
        bottom_.clear();
        columnsKey_.clear();
        return;
    }
    if (key == columnsKey_)
        return;
    columnsKey_ = key;
    auto [lo, hi] = waveformColumns(waveform_, columns);
    if (reverse_) {  // drawn as it plays
        std::reverse(lo.begin(), lo.end());
        std::reverse(hi.begin(), hi.end());
    }
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
    const QString marker =
        event->button() == Qt::LeftButton ? markerAt(event->position().x(), event->position().y()) : QString();
    if (marker.isEmpty()) {
        event->ignore();  // (to the device: selecting it)
        return;
    }
    gesture_ = newGestureKey();
    setDrag(marker);
    touch(marker == QLatin1String("loop") ? QStringLiteral("loop_start") : marker);
}

void SampleView::mouseMoveEvent(QMouseEvent* event) {
    if (drag_.isEmpty())
        return;
    const QRectF r = plot();
    double percent = std::clamp((event->position().x() - r.left()) / r.width() * 100.0, 0.0, 100.0);
    const double start = value(QStringLiteral("start")), end = value(QStringLiteral("end"));
    if (drag_ == QLatin1String("start")) {
        setParam(drag_, std::min(percent, end), gesture_);
    } else if (drag_ == QLatin1String("end")) {
        setParam(drag_, std::max(percent, start), gesture_);
    } else {
        setParam(QStringLiteral("loop_start"), std::clamp(percent, start, end), gesture_);
    }
}

void SampleView::mouseReleaseEvent(QMouseEvent*) { setDrag(QString()); }

void SampleView::mouseUngrabEvent() { setDrag(QString()); }

void SampleView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton)
        Q_EMIT browseRequested();
}

void SampleView::hoverMoveEvent(QHoverEvent* event) {
    if (drag_.isEmpty())
        setCursor(markerAt(event->position().x(), event->position().y()).isEmpty() ? Qt::ArrowCursor
                                                                                    : Qt::SizeHorCursor);
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

void SampleView::paintRuler(SgPainter& p, const QRectF& area) const {
    const double seconds = waveform_.duration();
    if (!(seconds > 0.0) || area.width() < 2)
        return;
    const QFont font = uiFont(7);
    // The finest step whose labels don't run into each other.
    const double perSecond = area.width() / seconds;
    const double labelWidth = SgPainter::textWidth(QStringLiteral("0:00:000"), font) + 12;
    double step = 600.0;
    for (const double candidate : {0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0,
                                   60.0, 120.0, 300.0}) {
        if (candidate * perSecond >= labelWidth) {
            step = candidate;
            break;
        }
    }
    const QColor tick(Theme::kTextDim.red(), Theme::kTextDim.green(), Theme::kTextDim.blue(), 140);
    for (int i = 1; i * step < seconds; ++i) {
        const double x = area.left() + i * step * perSecond;
        p.drawLine(QPointF(x, area.top()), QPointF(x, area.top() + 3), tick, 1);
        if (x + labelWidth - 10 < area.right())
            p.drawText(QRectF(x + 2, area.top(), labelWidth, area.height()), Qt::AlignVCenter | Qt::AlignLeft,
                       timeText(i * step), Theme::kTextDim, font);
    }
}

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
    if (waveform_.isNull()) {
        paintName(p, r, font);
        if (error_.isEmpty())
            p.drawText(r, Qt::AlignCenter, QStringLiteral("Loading…"), Theme::kTextDim, font);
        return;
    }
    const double middle = r.center().y();
    p.drawLine(QPointF(r.left(), middle), QPointF(r.right(), middle), Theme::kGridBar, 1);
    if (!top_.empty())
        p.fillColumns(r.left(), 1.0, top_.data(), bottom_.data(), int(top_.size()), Theme::kAccent, 1.0);
    paintRuler(p, QRectF(r.left(), r.bottom() + 1, r.width(), kRulerHeight));

    const double start = xOfFrame(double(startFrame_)), end = xOfFrame(double(endFrame_));
    const QColor dimLine(Theme::kText.red(), Theme::kText.green(), Theme::kText.blue(), 110);
    p.save();
    p.setClipRect(r);
    p.setAntialiasing(true);
    if (mode_ == kSlice) {
        // The slice playing lit, a line where each begins, numbered while there is room.
        const int playing = playingSlice();
        const QFont small = uiFont(7);
        for (std::size_t i = 0; i < slices_.size(); ++i) {
            const double from = xOfFrame(double(slices_[i]));
            const double to = i + 1 < slices_.size() ? xOfFrame(double(slices_[i + 1])) : end;
            if (int(i) == playing)
                p.fillRect(QRectF(from, r.top(), to - from, r.height()), QColor(255, 255, 255, 34));
            if (i > 0)
                p.drawLine(QPointF(from, r.top()), QPointF(from, r.bottom()), dimLine, 1);
            if (to - from >= 14)
                p.drawText(QRectF(from + 2, r.bottom() - 11, to - from - 2, 10), Qt::AlignLeft | Qt::AlignBottom,
                           QString::number(i + 1), Theme::kText, small);
        }
    } else if (mode_ == kClassic && looping_ && endFrame_ > loopFrame_) {
        // The loop: bracketed over the top, its crossfade shaded (its end, and what it fades from).
        const double loop = xOfFrame(double(loopFrame_));
        if (fadeFrames_ > 0) {
            const double fade = xOfFrame(double(endFrame_ - fadeFrames_)), from = xOfFrame(double(loopFrame_ - fadeFrames_));
            const QColor shade(255, 255, 255, 40);
            p.fillPolygon(QPolygonF({QPointF(fade, r.bottom()), QPointF(end, r.top()), QPointF(end, r.bottom())}), shade);
            p.fillPolygon(QPolygonF({QPointF(from, r.top()), QPointF(loop, r.bottom()), QPointF(from, r.bottom())}),
                          shade);
        }
        p.fillRect(QRectF(loop, r.top(), end - loop, 3), Theme::kLoopOn);
        p.drawLine(QPointF(loop, r.top()), QPointF(loop, r.bottom()), Theme::kLoopOn, 1.5);
        // Its handle at the bottom, pointing into the loop.
        p.fillPolygon(QPolygonF({QPointF(loop, r.bottom() - kLoopHandle), QPointF(loop + 7, r.bottom() - kLoopHandle / 2),
                                 QPointF(loop, r.bottom())}),
                      Theme::kLoopOn);
    } else if (mode_ == kOneShot) {
        // Its fades: in from Start, out before End.
        const double perMs = waveform_.sampleRate() / 1000.0 * rate_;
        const double in = std::min(end, xOfFrame(double(startFrame_) + fadeIn_ * perMs));
        const double out = std::max(in, xOfFrame(double(endFrame_) - fadeOut_ * perMs));
        const QPolygonF shape({QPointF(start, r.bottom()), QPointF(in, r.top() + 1), QPointF(out, r.top() + 1),
                               QPointF(end, r.bottom())});
        p.drawPolyline(shape, dimLine, 1);
    }
    // Start and End: lines flagged at the top, pointing into what plays.
    p.drawLine(QPointF(start, r.top()), QPointF(start, r.bottom()), Theme::kAccent, 1.5);
    p.drawLine(QPointF(end, r.top()), QPointF(end, r.bottom()), Theme::kAccent, 1.5);
    p.fillPolygon(flag(start, r.top(), true), Theme::kAccent);
    p.fillPolygon(flag(end, r.top(), false), Theme::kAccent);
    p.setAntialiasing(false);
    p.restore();
    // What doesn't play, dimmed.
    p.fillRect(QRectF(r.left(), r.top(), start - r.left(), r.height()), Theme::kOutsideClip);
    p.fillRect(QRectF(end, r.top(), r.right() - end, r.height()), Theme::kOutsideClip);
    if (playhead_ >= 0) {
        const double x = r.left() + playhead_ * r.width();
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()), Theme::kPlayhead, 1);
    }
    paintName(p, r, font);
}

// The sample's name over the waveform's top left corner (in red, missing).
void SampleView::paintName(SgPainter& p, const QRectF& r, const QFont& font) const {
    const QString name = QFileInfo(path_).fileName();
    const QString text = SgPainter::elidedText(error_.isEmpty() ? name : QStringLiteral("Missing: ") + name, font,
                                               r.width() * 0.6, Qt::ElideMiddle);
    const QRectF box(r.left() + 9, r.top() + 1, SgPainter::textWidth(text, font) + 6, 13);
    p.fillRect(box, QColor(Theme::kMeterBg.red(), Theme::kMeterBg.green(), Theme::kMeterBg.blue(), 190));
    p.drawText(box, Qt::AlignCenter, text, error_.isEmpty() ? Theme::kTextDim : Theme::kRecordOn, font);
}

}  // namespace sub::ui
