#pragma once
// What the device view's tests share (test_ui_device_panel*.cpp): a session
// in a window showing the DevicePanel (as the main window places it), its
// parts found by object name or by the device they show, its menu read and
// chosen from, drags from the browser (or along the chain) dropped on it as
// the platform delivers them, and what the session and the panel said.
// Header only, as UiTestSupport.h (the support library doesn't link Qt Quick).

#include "UiTestSupport.h"
#include "browser/BrowserMime.h"
#include "devices/DeviceChainArea.h"
#include "session/Session.h"

#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>

#include <memory>

namespace sub::app::test {

// The window the tests show: the device view at its bottom, as tall as it
// wants, below room for its menus (where the arrangement is in the main window).
inline constexpr int kAboveThePanel = 420;
inline constexpr const char* kDevicePanelWindow =
    "import QtQuick\n"
    "import SUBstation\n"
    "Window {\n"
    "    width: 1200\n"
    "    height: 420 + panel.implicitHeight\n"
    "    color: Theme.emptyArea\n"
    "    property alias panel: panel\n"
    "    property var said: []\n"
    "    DevicePanel {\n"
    "        id: panel\n"
    "        objectName: \"devicePanel\"\n"
    "        y: 420\n"
    "        width: parent.width\n"
    "        height: implicitHeight\n"
    "        onStatusMessage: message => said.push(message)\n"
    "    }\n"
    "}\n";

// Every item under `root` (in the item tree, depth first) whose object name is `name`.
inline QList<QQuickItem*> itemsNamed(QQuickItem* root, const QString& name) {
    QList<QQuickItem*> found;
    if (!root) return found;
    for (QQuickItem* child : root->childItems()) {
        if (child->objectName() == name) found << child;
        found += itemsNamed(child, name);
    }
    return found;
}

// The first visible item under `root` named `name` (null: none).
inline QQuickItem* itemNamed(QQuickItem* root, const QString& name) {
    for (QQuickItem* item : itemsNamed(root, name)) {
        if (item->isVisible()) return item;
    }
    return nullptr;
}

// An item a QML item holds in a property (an alias to one of its parts).
inline QQuickItem* part(QObject* item, const char* name) {
    return item ? qvariant_cast<QQuickItem*>(item->property(name)) : nullptr;
}

// A drag carrying `mime` entering the window at `pos`, moved to `pos`, and dropped there.
inline bool dragEnter(QQuickWindow* window, QPoint pos, QMimeData* mime) {
    QDragEnterEvent enter(pos, Qt::CopyAction | Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
    QGuiApplication::sendEvent(window, &enter);
    return enter.isAccepted();
}
inline void dragMove(QQuickWindow* window, QPoint pos, QMimeData* mime) {
    QDragMoveEvent move(pos, Qt::CopyAction | Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
    QGuiApplication::sendEvent(window, &move);
}
inline void dragLeave(QQuickWindow* window) {
    QDragLeaveEvent leave;
    QGuiApplication::sendEvent(window, &leave);
}
inline void dropAt(QQuickWindow* window, QPoint pos, QMimeData* mime) {
    QDropEvent drop(QPointF(pos), Qt::CopyAction | Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
    QGuiApplication::sendEvent(window, &drop);
}
inline void dragAndDrop(QQuickWindow* window, QPoint pos, QMimeData* mime) {
    dragEnter(window, pos, mime);
    dragMove(window, pos, mime);
    dropAt(window, pos, mime);
}

// What the browser's drags carry: built-in devices, plug-ins (PluginRef fields), presets; and devices moved.
inline std::unique_ptr<QMimeData> deviceKindsMime(const QStringList& kinds) {
    auto mime = std::make_unique<QMimeData>();
    mime->setData(QString::fromLatin1(kDeviceMime),
                  QJsonDocument(QJsonArray::fromStringList(kinds)).toJson(QJsonDocument::Compact));
    return mime;
}
inline std::unique_ptr<QMimeData> pluginsMime(const QVariantList& refs) {
    auto mime = std::make_unique<QMimeData>();
    QJsonArray array;
    for (const QVariant& ref : refs) array.append(QJsonObject::fromVariantMap(ref.toMap()));
    mime->setData(QString::fromLatin1(kPluginMime), QJsonDocument(array).toJson(QJsonDocument::Compact));
    return mime;
}
inline std::unique_ptr<QMimeData> presetsMime(const QStringList& paths) {
    auto mime = std::make_unique<QMimeData>();
    mime->setData(QString::fromLatin1(kPresetMime),
                  QJsonDocument(QJsonArray::fromStringList(paths)).toJson(QJsonDocument::Compact));
    return mime;
}
inline std::unique_ptr<QMimeData> movedMime(const QString& trackId, const QStringList& deviceIds) {
    auto mime = std::make_unique<QMimeData>();
    mime->setData(QString::fromLatin1(kDeviceMoveMime), movedDevicesData(trackId, deviceIds));
    return mime;
}

// The device view's menu (PanelMenu) or a parameter's (ParamMenu): its entries' texts ("" for a separator).
inline QStringList menuTexts(QObject* menu) {
    QStringList texts;
    for (int i = 0; menu && i < menu->property("count").toInt(); ++i) {
        QQuickItem* item = nullptr;
        QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, i));
        texts << (item ? item->property("text").toString() : QString());
    }
    return texts;
}
inline QQuickItem* menuItem(QObject* menu, const QString& text) {
    for (int i = 0; menu && i < menu->property("count").toInt(); ++i) {
        QQuickItem* item = nullptr;
        QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, i));
        if (item && item->property("text").toString() == text) return item;
    }
    return nullptr;
}
// An entry's state: whether it is enabled, checked; its shortcut tip.
inline bool menuEnabled(QObject* menu, const QString& text) {
    QQuickItem* item = menuItem(menu, text);
    return item && item->isEnabled();
}
inline bool menuChecked(QObject* menu, const QString& text) {
    QQuickItem* item = menuItem(menu, text);
    return item && item->property("checked").toBool();
}

// A button of a dialog's button box by its text ("OK", "Yes": without its mnemonic mark).
inline QQuickItem* dialogButton(QObject* dialog, const QString& text) {
    auto* footer = dialog ? qvariant_cast<QQuickItem*>(dialog->property("footer")) : nullptr;
    QList<QQuickItem*> items{footer};
    while (!items.isEmpty()) {
        QQuickItem* item = items.takeFirst();
        if (!item) continue;
        if (item->property("text").toString().remove(u'&') == text && item->inherits("QQuickAbstractButton"))
            return item;
        items += item->childItems();
    }
    return nullptr;
}

// A session in a window showing the device view. One per test program.
class PanelWindow {
public:
    // Shows it (false: it didn't load).
    bool open() {
        ui_ = std::make_unique<UiSession>();
        QObject::connect(&ui_->session(), &Session::statusMessage, [this](const QString& text) { messages << text; });
        window_ = ui_->show(kDevicePanelWindow);
        if (!window_) return false;
        panel_ = qvariant_cast<QQuickItem*>(window_->property("panel"));
        area_ = panel_ ? qobject_cast<sub::ui::DeviceChainArea*>(part(panel_, "area")) : nullptr;
        return panel_ && area_;
    }
    void close() { ui_.reset(); }

    Session& session() { return ui_->session(); }
    sub::Engine& engine() { return ui_->engine(); }
    QQuickWindow* window() const { return window_; }
    QQuickItem* panel() const { return panel_; }
    sub::ui::DeviceChainArea* area() const { return area_; }
    QObject* menu() const { return panel_->property("menu").value<QObject*>(); }
    // What the panel itself said (statusMessage), in order.
    QStringList panelSaid() const { return window_->property("said").toStringList(); }
    QString lastMessage() const { return messages.isEmpty() ? QString() : messages.back(); }

    // A device's frame, once laid out (null: not shown).
    QQuickItem* frame(const QString& deviceId) {
        polish();
        return area_->frameOf(deviceId);
    }
    // The chain's layout done now (as before a frame is drawn).
    void polish() {
        QTest::qWait(0);
        area_->layOut();
    }
    // A point of an item, in the window.
    QPoint at(QQuickItem* item, qreal x, qreal y) const { return item->mapToScene(QPointF(x, y)).toPoint(); }
    QPoint center(QQuickItem* item) const { return centerOf(item); }
    // A point of the chain's area (as the old tests' panel coordinates were the chain's).
    QPoint inArea(qreal x, qreal y) const { return area_->mapToScene(QPointF(x, y)).toPoint(); }
    // A point of the panel beside the devices, `fromRight` px from its right edge.
    QPoint beside(int fromRight = 5) const { return at(panel_, panel_->width() - fromRight, panel_->height() / 2); }

    // A screenshot of the panel (the whole window with `withMenus`: they open above it).
    void screenshot(const QString& name, bool withMenus = false) {
        polish();
        QTest::qWait(30);  // (drawn)
        const QRect part = withMenus ? QRect() : QRect(0, kAboveThePanel, window_->width(), int(panel_->height()));
        sub::app::test::screenshot(window_, QStringLiteral("device-panel-") + name, part);
    }

    // The right button clicked there (a menu opens on its press).
    void rightClick(QPoint pos) { QTest::mouseClick(window_, Qt::RightButton, Qt::NoModifier, pos); }
    // Until the panel's menu has opened (false: it didn't).
    bool menuOpened() {
        QObject* m = menu();
        return QTest::qWaitFor([m] { return m->property("opened").toBool(); }, 3000);
    }
    // Chooses an entry of the panel's open menu with a click (false: no such
    // entry enabled, or the menu didn't close).
    bool choose(const QString& text) {
        if (!menuOpened()) return false;
        QObject* m = menu();
        QQuickItem* item = menuItem(m, text);
        if (!item || !item->isEnabled()) return false;
        QTest::qWait(50);  // (placed where it fits in the window: on the next frame)
        moveTo(window_, centerOf(item), {}, Qt::NoButton);
        click(window_, centerOf(item));
        return QTest::qWaitFor([m] { return !m->property("visible").toBool(); }, 3000);
    }
    bool closeMenu() {
        QObject* m = menu();
        QMetaObject::invokeMethod(m, "close");
        return QTest::qWaitFor([m] { return !m->property("visible").toBool(); }, 3000);
    }

    QStringList messages;  // the session's statusMessage, in order

private:
    std::unique_ptr<UiSession> ui_;
    QQuickWindow* window_ = nullptr;
    QQuickItem* panel_ = nullptr;
    sub::ui::DeviceChainArea* area_ = nullptr;
};

}  // namespace sub::app::test
