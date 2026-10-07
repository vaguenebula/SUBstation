#pragma once

// The device view's chain area: the chain scrolled sideways, without a scroll
// bar; drops; the drags its devices start; scrolling to a device added.
//
// - Scrolling: `contentX` (QML moves `content` by it). Shift+wheel anywhere
//   over the chain scrolls it (kWheelScroll px a notch), and the wheel never
//   turns a knob or a value box here (it goes to nothing); Ctrl+Alt-drag
//   anywhere on the chain scrolls it by hand, as in the arrangement. The press
//   usually lands on a device or a knob, so the area watches its window's
//   mouse events before they reach them (an event filter, as the panel's was).
// - Drops: devices and plug-ins from the browser, presets (into the device of
//   their kind they are dropped onto, else as new devices), devices of the
//   track dragged along its chain or into a rack's: they go to the chain
//   under the mouse (a rack chain's row: last in that chain; else the
//   innermost rack chain shown there; else the track's own), before the first
//   device whose middle is right of the mouse (dropTarget()); the view acts
//   through Session.deviceSelection. While a drag is over the chain, a line
//   shows where it would go (dropMarker), or the device a preset would load
//   into is outlined (loadMarker); held within kAutoscrollEdge px of either
//   side, the chain scrolls, faster nearer the edge.
// - Drags: startDrag() (a device's frame dragged past the start distance)
//   drags the selected devices (if it is one of them; else it), instruments
//   left out, under kDeviceMoveMime, with a picture of the device: along the
//   chain, into a rack's, or onto another track in the arrangement.
// - A device added (not by a drop on the chain, where it is in view already)
//   is scrolled to, once laid out.
// - View state kept by id across the frames' rebuilds: each device's page.
//
// QML lays the chain out inside `content` and says what its items are with a
// `panelRole` property: "device" (a device's frame: `deviceId`, and `chainId`
// the chain it is in, "" for the track's own), "chain" (a rack's chain shown
// beside the rack, in its bracket: `chainId`), "chainRow" (a row of a rack's
// chain list: `chainId`, `rackId`).

#include <QHash>
#include <QPointer>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

#include <optional>

#include "session/Session.h"

class QMimeData;

namespace sub::ui {

class DeviceChainArea : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(sub::app::Session* session READ session WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(QQuickItem* content READ content WRITE setContent NOTIFY contentChanged)
    Q_PROPERTY(qreal contentX READ contentX WRITE setContentX NOTIFY contentXChanged)
    Q_PROPERTY(qreal maxContentX READ maxContentX NOTIFY contentXChanged)
    Q_PROPERTY(QRectF dropMarker READ dropMarker NOTIFY markersChanged)  // in `content`; empty: none
    Q_PROPERTY(QRectF loadMarker READ loadMarker NOTIFY markersChanged)  // in `content`; empty: none
    Q_PROPERTY(bool panning READ panning NOTIFY panningChanged)  // Ctrl+Alt-dragging the chain

public:
    static constexpr int kSpacing = 8;            // between the chain's devices (a grip in the middle)
    static constexpr int kWheelScroll = 80;       // px Shift+wheel scrolls the chain by a notch
    static constexpr int kAutoscrollEdge = 40;    // px from the chain's edge where a drag scrolls it
    static constexpr int kAutoscrollInterval = 16;  // ms
    static constexpr int kDragPictureHeight = 48;  // px: the picture of the device dragged

    explicit DeviceChainArea(QQuickItem* parent = nullptr);
    ~DeviceChainArea() override;

    sub::app::Session* session() const { return session_; }
    void setSession(sub::app::Session* session);
    QQuickItem* content() const { return content_; }
    void setContent(QQuickItem* content);
    qreal contentX() const { return contentX_; }
    void setContentX(qreal x);
    qreal maxContentX() const;
    QRectF dropMarker() const { return dropMarker_; }
    QRectF loadMarker() const { return loadMarker_; }
    bool panning() const { return pan_.has_value(); }

    // Where a drop at (x, y) (this item's coordinates) goes: {chain ("": the
    // track's own), index (in that chain)}.
    Q_INVOKABLE QVariantMap dropTarget(qreal x, qreal y) const;
    // The device one preset dropped at (x, y) loads into: the one there, if
    // it is of the preset's kind (not on a rack's chain list, where it goes
    // into the chain). "": it goes in as a new device.
    Q_INVOKABLE QString presetTarget(qreal x, qreal y, const QStringList& paths) const;
    // Scrolls so the device's frame is in view (as QScrollArea::ensureWidgetVisible).
    Q_INVOKABLE void scrollTo(const QString& deviceId);
    // Lays the chain out now (its rows lay out their items before the next
    // frame, otherwise): a device just added has its place.
    Q_INVOKABLE void layOut();
    // A device's frame was dragged: drags the devices it moves (DeviceSelection::dragDevices).
    Q_INVOKABLE void startDrag(const QString& deviceId, QQuickItem* frame);
    // The frame of a device shown, a row of a rack's chain list (null: none).
    Q_INVOKABLE QQuickItem* frameOf(const QString& deviceId) const;
    Q_INVOKABLE QQuickItem* chainRowOf(const QString& rackId, const QString& chainId) const;
    // The parameter page each device shows (view state, kept by id).
    Q_INVOKABLE int pageOf(const QString& deviceId) const { return pages_.value(deviceId, 0); }
    Q_INVOKABLE void setPage(const QString& deviceId, int page) { pages_.insert(deviceId, page); }
    // Whether a rack of the track shown still has this chain (after a chain's menu).
    Q_INVOKABLE bool hasChain(const QString& rackId, const QString& chainId) const;
    // For the file dialogs: a path as a URL, a URL chosen as a path.
    Q_INVOKABLE static QUrl fileUrl(const QString& path);
    Q_INVOKABLE static QString localPath(const QUrl& url);

Q_SIGNALS:
    void sessionChanged();
    void contentChanged();
    void contentXChanged();
    void markersChanged();
    void panningChanged();
    // A drag of devices starts (the drag itself runs after it): from this track, these devices.
    void dragStarting(const QString& trackId, const QStringList& deviceIds);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void itemChange(ItemChange change, const ItemChangeData& data) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Part {
        QPointer<QQuickItem> item;
        QString deviceId;
        QString chainId;
        QString rackId;
    };
    // What the chain shows, from its items' roles.
    QList<Part> parts(const QString& role) const;
    void collect(QQuickItem* item, const QString& role, QList<Part>& into) const;
    // An item's rectangle in this item's coordinates, and whether a point there is in view (not clipped).
    QRectF rectIn(QQuickItem* item) const;
    QRectF contentRect(QQuickItem* item) const;
    bool showsAt(QQuickItem* item, const QPointF& at) const;
    int chainViewDepth(QQuickItem* item) const;
    // The topmost item of the window at a scene point; whether it is in the chain.
    QQuickItem* itemAt(const QPointF& scenePos) const;
    bool overChain(const QPointF& scenePos) const;
    // The devices a drag moves, if it moves this track's.
    QStringList moving(const QMimeData* mime) const;
    bool acceptsDrag(const QMimeData* mime) const;
    void dragAt(const QPointF& pos);
    void showDropMarkers(const QPointF& pos);
    void showDropMarker(const QString& chain, int index);
    void setMarkers(const QRectF& drop, const QRectF& load);
    void dragEnded();
    void autoScroll();
    void runDrag(const QString& trackId, const QStringList& deviceIds, const QImage& picture);
    void onDevicesChanged(const QString& trackId);
    void onSelectionChanged();
    QStringList allDevices() const;  // every device on the track shown, depth first
    bool wheel(QWheelEvent* event);
    void installFilter(QQuickWindow* window);

    QPointer<sub::app::Session> session_;
    QPointer<QQuickItem> content_;
    qreal contentX_ = 0.0;
    QRectF dropMarker_;
    QRectF loadMarker_;
    QHash<QString, int> pages_;
    QStringList draggedPresets_;  // the files of the presets dragged over the chain
    QPointF dragPos_;
    int scrollStep_ = 0;
    QTimer autoscroll_;
    bool dropping_ = false;  // devices added by a drop on the chain are in view already
    QString shownTrack_;
    QSet<QString> known_;  // the devices of the track shown, as last seen
    struct Pan {
        qreal pressX;  // on the screen
        qreal contentX;
    };
    std::optional<Pan> pan_;
    QPointer<QQuickWindow> filtered_;
    QList<QMetaObject::Connection> connections_;
};

}  // namespace sub::ui
