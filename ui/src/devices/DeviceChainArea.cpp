#include "devices/DeviceChainArea.h"

#include "browser/BrowserMime.h"
#include "controls/KnobItem.h"
#include "controls/ValueBoxItem.h"
#include "input/Modifiers.h"
#include "model/Device.h"
#include "model/Project.h"
#include "session/DeviceSelection.h"

#include <QCursor>
#include <QDrag>
#include <QDropEvent>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QMouseEvent>
#include <QPixmap>
#include <QQuickWindow>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>

namespace sub::ui {

namespace {

const QString kDeviceRole = QStringLiteral("device");
const QString kChainRole = QStringLiteral("chain");
const QString kChainRowRole = QStringLiteral("chainRow");

// The topmost item at a scene point under `item` (in paint order), not looking into clipped parts.
QQuickItem* topItemAt(QQuickItem* item, const QPointF& scenePos) {
    QList<QQuickItem*> children = item->childItems();
    std::stable_sort(children.begin(), children.end(), [](QQuickItem* a, QQuickItem* b) { return a->z() < b->z(); });
    for (auto it = children.crbegin(); it != children.crend(); ++it) {
        QQuickItem* child = *it;
        if (!child->isVisible())
            continue;
        const QPointF local = child->mapFromScene(scenePos);
        if (child->clip() && !child->contains(local))
            continue;
        if (QQuickItem* found = topItemAt(child, scenePos))
            return found;
        if (child->contains(local))
            return child;
    }
    return nullptr;
}

}  // namespace

DeviceChainArea::DeviceChainArea(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemAcceptsDrops, true);
    autoscroll_.setInterval(kAutoscrollInterval);
    connect(&autoscroll_, &QTimer::timeout, this, &DeviceChainArea::autoScroll);
}

DeviceChainArea::~DeviceChainArea() {
    if (filtered_)
        filtered_->removeEventFilter(this);
    if (pan_)
        QGuiApplication::restoreOverrideCursor();
}

void DeviceChainArea::setSession(sub::app::Session* session) {
    if (session == session_)
        return;
    for (const QMetaObject::Connection& connection : std::as_const(connections_))
        disconnect(connection);
    connections_.clear();
    session_ = session;
    if (session_) {
        connections_ << connect(session_->project(), &sub::app::Project::devicesChanged, this,
                                &DeviceChainArea::onDevicesChanged);
        connections_ << connect(session_->deviceSelection(), &sub::app::DeviceSelection::changed, this,
                                &DeviceChainArea::onSelectionChanged);
    }
    onSelectionChanged();
    Q_EMIT sessionChanged();
}

void DeviceChainArea::setContent(QQuickItem* content) {
    if (content == content_)
        return;
    if (content_)
        disconnect(content_, nullptr, this, nullptr);
    content_ = content;
    if (content_)
        connect(content_, &QQuickItem::widthChanged, this, [this] { setContentX(contentX_); });
    setContentX(contentX_);
    Q_EMIT contentChanged();
}

qreal DeviceChainArea::maxContentX() const { return content_ ? std::max(0.0, content_->width() - width()) : 0.0; }

void DeviceChainArea::setContentX(qreal x) {
    const qreal clamped = std::clamp(x, 0.0, maxContentX());
    if (clamped == contentX_) {
        Q_EMIT contentXChanged();  // (the range may have changed)
        return;
    }
    contentX_ = clamped;
    Q_EMIT contentXChanged();
}

void DeviceChainArea::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.width() != oldGeometry.width())
        setContentX(contentX_);
}

// --- What the chain shows ---------------------------------------------------------------------

void DeviceChainArea::collect(QQuickItem* item, const QString& role, QList<Part>& into) const {
    for (QQuickItem* child : item->childItems()) {
        if (!child->isVisible())
            continue;
        if (child->property("panelRole").toString() == role) {
            into.append({child, child->property("deviceId").toString(), child->property("chainId").toString(),
                         child->property("rackId").toString()});
        }
        collect(child, role, into);
    }
}

QList<DeviceChainArea::Part> DeviceChainArea::parts(const QString& role) const {
    QList<Part> found;
    if (content_)
        collect(content_, role, found);
    return found;
}

QRectF DeviceChainArea::rectIn(QQuickItem* item) const {
    return item->mapRectToItem(this, QRectF(0, 0, item->width(), item->height()));
}

QRectF DeviceChainArea::contentRect(QQuickItem* item) const {
    return content_ ? item->mapRectToItem(content_, QRectF(0, 0, item->width(), item->height())) : QRectF();
}

bool DeviceChainArea::showsAt(QQuickItem* item, const QPointF& at) const {
    if (!rectIn(item).contains(at))
        return false;
    for (QQuickItem* above = item->parentItem(); above && above != this; above = above->parentItem()) {
        if (above->clip() && !rectIn(above).contains(at))
            return false;  // scrolled out of view
    }
    return true;
}

int DeviceChainArea::chainViewDepth(QQuickItem* item) const {
    int depth = 0;
    for (QQuickItem* above = item->parentItem(); above && above != this; above = above->parentItem())
        depth += above->property("panelRole").toString() == kChainRole ? 1 : 0;
    return depth;
}

QQuickItem* DeviceChainArea::frameOf(const QString& deviceId) const {
    for (const Part& part : parts(kDeviceRole)) {
        if (part.deviceId == deviceId)
            return part.item;
    }
    return nullptr;
}

QQuickItem* DeviceChainArea::chainRowOf(const QString& rackId, const QString& chainId) const {
    for (const Part& part : parts(kChainRowRole)) {
        if (part.chainId == chainId && (rackId.isEmpty() || part.rackId == rackId))
            return part.item;
    }
    return nullptr;
}

QVariantMap DeviceChainArea::dropTarget(qreal x, qreal y) const {
    const QPointF at(x, y);
    auto target = [](const QString& chain, int index) {
        return QVariantMap{{QStringLiteral("chain"), chain}, {QStringLiteral("index"), index}};
    };
    // On a rack's chain list (its rack not folded): into the chain of the row there, last.
    for (const Part& row : parts(kChainRowRole)) {
        if (row.item && showsAt(row.item, at) && session_)
            return target(row.chainId, int(session_->deviceSelection()->chainDevices(row.chainId).size()));
    }
    // Else the innermost rack chain shown there, else the track's own.
    QString chain;
    int deepest = -1;
    for (const Part& view : parts(kChainRole)) {
        if (!view.item || !showsAt(view.item, at))
            continue;
        const int depth = chainViewDepth(view.item);
        if (depth > deepest) {
            chain = view.chainId;
            deepest = depth;
        }
    }
    int index = 0;
    for (const Part& device : parts(kDeviceRole)) {
        if (device.item && device.chainId == chain && rectIn(device.item).center().x() < x)
            ++index;
    }
    return target(chain, index);
}

QString DeviceChainArea::presetTarget(qreal x, qreal y, const QStringList& paths) const {
    if (paths.size() != 1 || !session_)
        return {};
    const QPointF at(x, y);
    for (const Part& device : parts(kDeviceRole)) {
        if (!device.item || !rectIn(device.item).contains(at))
            continue;
        for (const Part& row : parts(kChainRowRole)) {  // a rack's chain list takes it into the chain
            if (row.rackId == device.deviceId && row.item && showsAt(row.item, at))
                return {};
        }
        return session_->deviceSelection()->presetLoadsInto(paths.front(), device.deviceId) ? device.deviceId
                                                                                          : QString();
    }
    return {};
}

// --- Scrolling --------------------------------------------------------------------------------

void DeviceChainArea::layOut() {
    std::function<void(QQuickItem*)> polish = [&polish](QQuickItem* item) {
        for (QQuickItem* child : item->childItems())
            polish(child);
        if (item->inherits("QQuickBasePositioner"))
            item->ensurePolished();
    };
    if (content_)
        polish(content_);
}

void DeviceChainArea::scrollTo(const QString& deviceId) {
    layOut();
    QQuickItem* frame = frameOf(deviceId);
    if (!frame)
        return;
    const QRectF r = contentRect(frame);
    const qreal view = width();
    qreal value = contentX_;
    if (r.width() > view)
        value = r.center().x() - view / 2;
    else if (r.right() > contentX_ + view)
        value = r.right() - view;
    else if (r.left() < contentX_)
        value = r.left();
    setContentX(value);
}

QQuickItem* DeviceChainArea::itemAt(const QPointF& scenePos) const {
    return window() ? topItemAt(window()->contentItem(), scenePos) : nullptr;
}

bool DeviceChainArea::overChain(const QPointF& scenePos) const {
    if (!isVisible() || !contains(mapFromScene(scenePos)))
        return false;
    QQuickItem* top = itemAt(scenePos);
    return top == this || (top && isAncestorOf(top));
}

bool DeviceChainArea::wheel(QWheelEvent* event) {
    if (!overChain(event->scenePosition()))
        return false;
    const QPoint angle = event->angleDelta();
    if (event->modifiers() & Qt::ShiftModifier) {  // Shift+wheel scrolls the chain, even over a knob
        const int delta = angle.y() ? angle.y() : angle.x();
        setContentX(contentX_ - std::round(delta * double(kWheelScroll) / 120.0));
        return true;
    }
    // The wheel never turns a knob (or a value box) here.
    for (QQuickItem* item = itemAt(event->scenePosition()); item && item != this; item = item->parentItem()) {
        if (qobject_cast<KnobItem*>(item) || qobject_cast<ValueBoxItem*>(item))
            return true;
    }
    if (std::abs(angle.x()) > std::abs(angle.y())) {  // sideways (a trackpad): the chain scrolls
        setContentX(contentX_ - std::round(angle.x() * double(kWheelScroll) / 120.0));
        return true;
    }
    return false;
}

void DeviceChainArea::installFilter(QQuickWindow* window) {
    if (filtered_ == window)
        return;
    if (filtered_)
        filtered_->removeEventFilter(this);
    filtered_ = window;
    if (filtered_)
        filtered_->installEventFilter(this);
}

void DeviceChainArea::itemChange(ItemChange change, const ItemChangeData& data) {
    QQuickItem::itemChange(change, data);
    if (change == ItemSceneChange)
        installFilter(data.window);
}

bool DeviceChainArea::eventFilter(QObject* watched, QEvent* event) {
    if (watched != filtered_)
        return false;
    switch (event->type()) {
        case QEvent::Wheel:
            return wheel(static_cast<QWheelEvent*>(event));
        case QEvent::MouseButtonPress: {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (!pan_ && mouse->button() == Qt::LeftButton && isPanModifier(mouse->modifiers()) &&
                overChain(mouse->scenePosition())) {
                pan_ = Pan{mouse->globalPosition().x(), contentX_};
                QGuiApplication::setOverrideCursor(QCursor(Qt::ClosedHandCursor));
                Q_EMIT panningChanged();
                return true;
            }
            return false;
        }
        case QEvent::MouseMove:
        case QEvent::MouseButtonRelease: {
            if (!pan_)
                return false;
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (event->type() == QEvent::MouseMove && (mouse->buttons() & Qt::LeftButton)) {
                setContentX(pan_->contentX - std::round(mouse->globalPosition().x() - pan_->pressX));
            } else {  // released (even if the release went elsewhere)
                pan_.reset();
                QGuiApplication::restoreOverrideCursor();
                Q_EMIT panningChanged();
            }
            return true;
        }
        default:
            return false;
    }
}

// --- Dragging and dropping --------------------------------------------------------------------

QStringList DeviceChainArea::moving(const QMimeData* mime) const {
    const auto moved = sub::app::movedDevices(mime);
    if (!moved || !session_ || moved->trackId != session_->deviceSelection()->trackId())
        return {};
    return moved->deviceIds;
}

bool DeviceChainArea::acceptsDrag(const QMimeData* mime) const {
    if (!session_ || session_->deviceSelection()->trackId().isEmpty() || mime == nullptr)
        return false;
    return !sub::app::deviceKinds(mime).isEmpty() || mime->hasFormat(QString::fromLatin1(sub::app::kPluginMime)) ||
           !sub::app::presetPaths(mime).isEmpty() || !moving(mime).isEmpty();
}

void DeviceChainArea::dragEnterEvent(QDragEnterEvent* event) {
    if (!acceptsDrag(event->mimeData())) {
        event->ignore();
        return;
    }
    draggedPresets_ = sub::app::presetPaths(event->mimeData());
    event->acceptProposedAction();
    dragAt(event->position());
}

void DeviceChainArea::dragMoveEvent(QDragMoveEvent* event) {
    event->acceptProposedAction();
    dragAt(event->position());
}

void DeviceChainArea::dragLeaveEvent(QDragLeaveEvent*) { dragEnded(); }

void DeviceChainArea::dropEvent(QDropEvent* event) {
    dragEnded();
    event->acceptProposedAction();
    if (!session_ || session_->deviceSelection()->trackId().isEmpty())
        return;
    sub::app::DeviceSelection* devices = session_->deviceSelection();
    const QMimeData* mime = event->mimeData();
    const QPointF at = event->position();
    const QVariantMap target = dropTarget(at.x(), at.y());
    const QString chain = target.value(QStringLiteral("chain")).toString();
    const int index = target.value(QStringLiteral("index")).toInt();
    if (const QStringList ids = moving(mime); !ids.isEmpty()) {
        devices->dropMoved(ids, chain, index);
        return;
    }
    // Devices added by a drop are in view already: no scrolling to them.
    dropping_ = true;
    if (const QStringList presets = sub::app::presetPaths(mime); !presets.isEmpty()) {
        devices->dropPresets(presets, chain, index, presetTarget(at.x(), at.y(), presets));
    } else {
        QVariantList plugins;
        for (const sub::app::PluginInfo& plugin : sub::app::pluginRefs(mime))
            plugins << plugin.toRef();
        devices->dropDevices(sub::app::deviceKinds(mime), plugins, chain, index);
    }
    dropping_ = false;
}

// A drag is over `pos`: show where it would drop, and scroll while it is near an edge of the chain.
void DeviceChainArea::dragAt(const QPointF& pos) {
    dragPos_ = pos;
    const int x = int(std::floor(pos.x()));
    const int view = int(width());
    if (x < kAutoscrollEdge)
        scrollStep_ = -std::max(2, (kAutoscrollEdge - x) / 2);
    else if (x > view - kAutoscrollEdge)
        scrollStep_ = std::max(2, (x - view + kAutoscrollEdge) / 2);
    else
        scrollStep_ = 0;
    if (scrollStep_)
        autoscroll_.start();
    else
        autoscroll_.stop();
    showDropMarkers(pos);
}

void DeviceChainArea::autoScroll() {
    setContentX(contentX_ + scrollStep_);
    showDropMarkers(dragPos_);
}

// Where a drag at `pos` would go: between devices, or (a preset) into the device outlined.
void DeviceChainArea::showDropMarkers(const QPointF& pos) {
    const QString into = presetTarget(pos.x(), pos.y(), draggedPresets_);
    if (into.isEmpty()) {
        const QVariantMap target = dropTarget(pos.x(), pos.y());
        showDropMarker(target.value(QStringLiteral("chain")).toString(),
                       target.value(QStringLiteral("index")).toInt());
        return;
    }
    QQuickItem* frame = frameOf(into);
    setMarkers(QRectF(), frame ? contentRect(frame) : QRectF());
}

void DeviceChainArea::showDropMarker(const QString& chain, int index) {
    QList<QRectF> rects;
    for (const Part& device : parts(kDeviceRole)) {
        if (device.item && device.chainId == chain)
            rects << contentRect(device.item);
    }
    std::sort(rects.begin(), rects.end(), [](const QRectF& a, const QRectF& b) { return a.x() < b.x(); });
    constexpr int gap = kSpacing;
    qreal x = 0, top = 0, bottom = 0;
    if (rects.isEmpty()) {  // an empty rack chain: inside its bracket
        QQuickItem* view = nullptr;
        for (const Part& part : parts(kChainRole)) {
            if (part.chainId == chain && !chain.isEmpty())
                view = part.item;
        }
        if (!view) {
            setMarkers(QRectF(), QRectF());
            return;
        }
        const QRectF r = contentRect(view);
        x = r.left() + 3;
        top = r.top();
        bottom = r.top() + r.height() - 1;
    } else {
        // (As QRect's right(): the last pixel in it.)
        x = index < rects.size() ? rects[index].left() - gap / 2 - 1 : rects.last().left() + rects.last().width() - 1 + gap / 2;
        top = rects.front().top();
        bottom = rects.front().top() + rects.front().height() - 1;
        for (const QRectF& r : std::as_const(rects)) {
            top = std::min(top, r.top());
            bottom = std::max(bottom, r.top() + r.height() - 1);
        }
    }
    setMarkers(QRectF(x, top, 2, bottom - top + 1), QRectF());
}

void DeviceChainArea::setMarkers(const QRectF& drop, const QRectF& load) {
    if (drop == dropMarker_ && load == loadMarker_)
        return;
    dropMarker_ = drop;
    loadMarker_ = load;
    Q_EMIT markersChanged();
}

void DeviceChainArea::dragEnded() {
    autoscroll_.stop();
    setMarkers(QRectF(), QRectF());
    draggedPresets_.clear();
    if (session_)
        session_->deviceSelection()->dragEnded();
}

void DeviceChainArea::startDrag(const QString& deviceId, QQuickItem* frame) {
    if (!session_)
        return;
    const QString trackId = session_->deviceSelection()->trackId();
    const QStringList ids = session_->deviceSelection()->dragDevices(deviceId);
    if (trackId.isEmpty() || ids.isEmpty())
        return;
    Q_EMIT dragStarting(trackId, ids);
    // After the frame's handler has returned: the drop may make the frames again, that one too.
    QPointer<QQuickItem> source(frame);
    QTimer::singleShot(0, this, [this, trackId, ids, source] {
        QImage picture;
        if (source && window()) {
            const QImage shot = window()->grabWindow();
            const qreal dpr = shot.devicePixelRatio();
            const QRectF r = source->mapRectToScene(QRectF(0, 0, source->width(), source->height()));
            const QRect part(QPoint(int(r.x() * dpr), int(r.y() * dpr)), QSize(int(r.width() * dpr), int(r.height() * dpr)));
            picture = shot.copy(part).scaledToHeight(int(kDragPictureHeight * dpr), Qt::SmoothTransformation);
            picture.setDevicePixelRatio(dpr);
        }
        runDrag(trackId, ids, picture);
    });
}

void DeviceChainArea::runDrag(const QString& trackId, const QStringList& deviceIds, const QImage& picture) {
    // (Not if the button went up meanwhile: the drag would wait for a release that came already.)
    if (!session_ || session_->deviceSelection()->trackId() != trackId ||
        !(QGuiApplication::mouseButtons() & Qt::LeftButton))
        return;
    auto* mime = new QMimeData;
    mime->setData(QString::fromLatin1(sub::app::kDeviceMoveMime), sub::app::movedDevicesData(trackId, deviceIds));
    QPointer<QDrag> drag = new QDrag(this);
    drag->setMimeData(mime);
    if (!picture.isNull())
        drag->setPixmap(QPixmap::fromImage(picture));
    drag->exec(Qt::MoveAction);
    if (drag)
        drag->deleteLater();
    dragEnded();
}

bool DeviceChainArea::hasChain(const QString& rackId, const QString& chainId) const {
    const sub::app::Device* rack = session_ ? session_->project()->findDevice(shownTrack_, rackId) : nullptr;
    return rack != nullptr && std::any_of(rack->chains.begin(), rack->chains.end(),
                                          [&](const sub::app::Chain& chain) { return chain.id == chainId; });
}

QUrl DeviceChainArea::fileUrl(const QString& path) { return path.isEmpty() ? QUrl() : QUrl::fromLocalFile(path); }

QString DeviceChainArea::localPath(const QUrl& url) { return url.isLocalFile() ? url.toLocalFile() : url.toString(); }

// --- Following the model ----------------------------------------------------------------------

QStringList DeviceChainArea::allDevices() const {
    QStringList ids;
    const sub::app::Track* track = session_ ? session_->project()->findTrack(shownTrack_) : nullptr;
    if (track != nullptr) {
        for (const sub::app::Device* device : sub::app::iterDevices(track->devices))
            ids << device->id;
    }
    return ids;
}

void DeviceChainArea::onSelectionChanged() {
    const QString trackId = session_ ? session_->deviceSelection()->trackId() : QString();
    if (trackId == shownTrack_)
        return;
    shownTrack_ = trackId;
    const QStringList ids = allDevices();
    known_ = QSet<QString>(ids.begin(), ids.end());
}

void DeviceChainArea::onDevicesChanged(const QString& trackId) {
    if (trackId != shownTrack_)
        return;
    const QStringList ids = allDevices();
    QStringList added;
    for (const QString& id : ids) {
        if (!known_.contains(id))
            added << id;
    }
    known_ = QSet<QString>(ids.begin(), ids.end());
    if (added.isEmpty() || dropping_)
        return;
    // After the chain is laid out, so the new device has its place.
    const QString last = added.last();
    QTimer::singleShot(0, this, [this, last] { scrollTo(last); });
}

}  // namespace sub::ui
