#pragma once

// The main window's title bar is its own (Windows): the system's caption goes
// and the window's title bar item (TitleBar.qml: the menus, the title, the
// window's buttons) takes its place, while the window keeps its native frame:
// its shadow, its resizing edges, Aero Snap (dragging to a screen's edge,
// Win+arrows), the snap layouts over the maximize button, the system menu
// (Alt+Space, a right-click on the title bar), a double-click maximizing it,
// minimizing it from the taskbar.
//
// A native event filter on the window's messages does it:
// - WM_NCCALCSIZE: the client area takes the caption's place; the left, right
//   and bottom resizing borders stay the system's. Maximized, the window
//   reaches past the screen's edges by its frame, so the client area starts
//   that far down (and leaves a line free along an auto-hiding taskbar, which
//   then still comes up).
// - WM_NCHITTEST: the top edge resizes (not maximized). Over the title bar,
//   what handles no mouse of its own (its background, the title, the status
//   line) is the caption: it drags the window. The maximize button is the
//   system's (HTMAXBUTTON, for the snap layouts): the item draws it from
//   maximizeHovered and maximizePressed, and its click is handled here. The
//   rest (the menus, the other buttons) is the window's, as anywhere in it.
//   While a popup shows (a menu open), all of it is the window's: a press
//   there closes the popup, as anywhere else.
//
// Qt keeps a window's geometry as its client area's. The frame changes when
// the window's native window is made (before it shows) with its client area
// staying where it is, and Qt reads the new frame's margins from
// WM_NCCALCSIZE, so the geometry the window had (and WindowState saves) stays
// what it was.
//
// Elsewhere (Linux, and Qt's offscreen platform) nothing changes: `active`
// stays false, and the system draws the window's title bar. hitTest() is
// plain code: the tests check it anywhere.

#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtQml/qqmlregistration.h>

namespace sub::ui {

class WindowFrame : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickWindow* window READ window WRITE setWindow NOTIFY windowChanged)
    // The window's title bar (an item along its top).
    Q_PROPERTY(QQuickItem* titleBar READ titleBar WRITE setTitleBar NOTIFY titleBarChanged)
    // The title bar's maximize button: drawn by the item, handled here.
    Q_PROPERTY(QQuickItem* maximizeButton READ maximizeButton WRITE setMaximizeButton NOTIFY maximizeButtonChanged)
    // The window's popups' layer (Overlay.overlay): a popup in it that takes room is shown.
    Q_PROPERTY(QQuickItem* overlay READ overlay WRITE setOverlay NOTIFY overlayChanged)
    // Whether the system's caption is gone: the title bar has the window's buttons.
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(bool maximizeHovered READ maximizeHovered NOTIFY maximizeStateChanged)
    Q_PROPERTY(bool maximizePressed READ maximizePressed NOTIFY maximizeStateChanged)

public:
    // What a point of the window is.
    enum Hit {
        Client,          // the window's own (the menus, the buttons, everything below the title bar)
        Caption,         // drags the window
        MaximizeButton,  // the system's maximize button
    };
    Q_ENUM(Hit)

    explicit WindowFrame(QObject* parent = nullptr);
    ~WindowFrame() override;

    QQuickWindow* window() const { return window_; }
    void setWindow(QQuickWindow* window);
    QQuickItem* titleBar() const { return titleBar_; }
    void setTitleBar(QQuickItem* titleBar);
    QQuickItem* maximizeButton() const { return maximizeButton_; }
    void setMaximizeButton(QQuickItem* button);
    QQuickItem* overlay() const { return overlay_; }
    void setOverlay(QQuickItem* overlay);
    bool active() const { return hwnd_ != nullptr; }
    bool maximizeHovered() const { return maximizeHovered_; }
    bool maximizePressed() const { return maximizePressed_; }

    // What the point (in the window's coordinates) is: outside the title bar,
    // or while a popup shows, the window's; on it, the maximize button, the
    // window's where an item (or one it is in) takes the mouse, else the caption.
    Q_INVOKABLE sub::ui::WindowFrame::Hit hitTest(const QPointF& point) const;
    // Maximizes the window, or restores it from maximized.
    Q_INVOKABLE void toggleMaximized();

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

Q_SIGNALS:
    void windowChanged();
    void titleBarChanged();
    void maximizeButtonChanged();
    void overlayChanged();
    void activeChanged();
    void maximizeStateChanged();

protected:
    // The window's native window made or about to go (QPlatformSurfaceEvent).
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void attach();
    void detach();
    void setMaximizeState(bool hovered, bool pressed);

    QPointer<QQuickWindow> window_;
    QPointer<QQuickItem> titleBar_;
    QPointer<QQuickItem> maximizeButton_;
    QPointer<QQuickItem> overlay_;
    void* hwnd_ = nullptr;  // the window's HWND while its caption is the title bar
    bool installed_ = false;
    bool maximizeHovered_ = false;
    bool maximizePressed_ = false;
};

}  // namespace sub::ui
