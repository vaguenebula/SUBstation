#include "WindowFrame.h"

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QMetaObject>
#include <QPlatformSurfaceEvent>
#include <QString>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>
#endif

namespace sub::ui {

namespace {

// Whether a popup shows in the window's overlay: one that takes room (a
// tooltip said in the info view takes none).
bool popupShown(const QQuickItem* overlay) {
    if (!overlay || !overlay->isVisible()) return false;
    for (const QQuickItem* item : overlay->childItems())
        if (item->isVisible() && item->width() > 0 && item->height() > 0) return true;
    return false;
}

// Whether an item takes a press at `at` (in its coordinates). A Text accepts
// the left button for its links only (Qt Quick's Text accepts it always, and
// lets a press elsewhere go by): the title and the status line drag the window.
bool takesMouse(QQuickItem* item, const QPointF& at) {
    if (item->inherits("QQuickText")) {
        QString link;
        QMetaObject::invokeMethod(item, "linkAt", Q_RETURN_ARG(QString, link), Q_ARG(qreal, at.x()),
                                  Q_ARG(qreal, at.y()));
        return !link.isEmpty();
    }
    return item->acceptedMouseButtons() != Qt::NoButton || item->acceptHoverEvents();
}

#ifdef Q_OS_WIN
// How far the window's resizing edge reaches in (physical pixels): the frame
// the system gives a window, and how far a maximized one reaches past the screen.
int resizeBorder(HWND hwnd) {
    const UINT dpi = GetDpiForWindow(hwnd);
    return GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

// A maximized window covering an auto-hiding taskbar's edge would keep it from
// coming up: its client area leaves that edge a line free.
void leaveAutoHideTaskbar(HWND hwnd, RECT& client) {
    APPBARDATA state{};
    state.cbSize = sizeof(state);
    if (!(SHAppBarMessage(ABM_GETSTATE, &state) & ABS_AUTOHIDE)) return;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    const auto hasBar = [&](UINT edge) {
        APPBARDATA bar{};
        bar.cbSize = sizeof(bar);
        bar.uEdge = edge;
        bar.rc = monitor.rcMonitor;
        return SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &bar) != 0;
    };
    constexpr LONG kLine = 2;
    if (hasBar(ABE_TOP))
        client.top += kLine;
    else if (hasBar(ABE_BOTTOM))
        client.bottom -= kLine;
    else if (hasBar(ABE_LEFT))
        client.left += kLine;
    else if (hasBar(ABE_RIGHT))
        client.right -= kLine;
}
#endif

}  // namespace

WindowFrame::WindowFrame(QObject* parent) : QObject(parent) {}

WindowFrame::~WindowFrame() {
    if (installed_ && QCoreApplication::instance()) QCoreApplication::instance()->removeNativeEventFilter(this);
}

void WindowFrame::setWindow(QQuickWindow* window) {
    if (window == window_) return;
    if (window_) window_->removeEventFilter(this);
    detach();
    window_ = window;
    if (window_) {
        window_->installEventFilter(this);
        if (window_->handle()) attach();  // (made already: else when it is)
    }
    Q_EMIT windowChanged();
}

void WindowFrame::setTitleBar(QQuickItem* titleBar) {
    if (titleBar == titleBar_) return;
    titleBar_ = titleBar;
    Q_EMIT titleBarChanged();
}

void WindowFrame::setMaximizeButton(QQuickItem* button) {
    if (button == maximizeButton_) return;
    maximizeButton_ = button;
    Q_EMIT maximizeButtonChanged();
}

void WindowFrame::setOverlay(QQuickItem* overlay) {
    if (overlay == overlay_) return;
    overlay_ = overlay;
    Q_EMIT overlayChanged();
}

WindowFrame::Hit WindowFrame::hitTest(const QPointF& point) const {
    if (!titleBar_ || !titleBar_->isVisible() || !titleBar_->contains(titleBar_->mapFromScene(point))) return Client;
    if (popupShown(overlay_)) return Client;  // (a press closes it)
    if (maximizeButton_ && maximizeButton_->isVisible() &&
        maximizeButton_->contains(maximizeButton_->mapFromScene(point)))
        return MaximizeButton;
    // The topmost item there, from the title bar down: the window's if one on
    // the way takes the mouse (a button, the menus: a Control takes it all).
    QQuickItem* item = titleBar_;
    for (;;) {
        const QPointF at = item->mapFromScene(point);
        QQuickItem* child = item->childAt(at.x(), at.y());
        if (!child) return Caption;
        if (takesMouse(child, child->mapFromScene(point))) return Client;
        item = child;
    }
}

void WindowFrame::toggleMaximized() {
    if (!window_) return;
    if (window_->windowStates() & Qt::WindowMaximized)
        window_->showNormal();
    else
        window_->showMaximized();
}

bool WindowFrame::eventFilter(QObject* watched, QEvent* event) {
    if (watched == window_ && event->type() == QEvent::PlatformSurface) {
        const auto type = static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType();
        if (type == QPlatformSurfaceEvent::SurfaceCreated)
            attach();
        else if (type == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
            detach();
    }
    return QObject::eventFilter(watched, event);
}

void WindowFrame::attach() {
#ifdef Q_OS_WIN
    if (hwnd_ || !window_ || QGuiApplication::platformName() != QLatin1String("windows")) return;
    if (window_->flags() & Qt::FramelessWindowHint) return;  // (no caption to take the place of)
    const HWND hwnd = reinterpret_cast<HWND>(window_->winId());
    hwnd_ = hwnd;
    if (!installed_) {
        QCoreApplication::instance()->installNativeEventFilter(this);
        installed_ = true;
    }
    // The frame changes (WM_NCCALCSIZE below): the window's top comes down to
    // its client area's, which stays where it is.
    RECT frame{};
    GetWindowRect(hwnd, &frame);
    POINT client{0, 0};
    ClientToScreen(hwnd, &client);
    const UINT flags = SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE;
    if (IsZoomed(hwnd) || IsIconic(hwnd))
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, flags | SWP_NOMOVE | SWP_NOSIZE);
    else
        SetWindowPos(hwnd, nullptr, frame.left, client.y, frame.right - frame.left, frame.bottom - client.y, flags);
    Q_EMIT activeChanged();
#endif
}

void WindowFrame::detach() {
    if (!hwnd_) return;
    hwnd_ = nullptr;
    setMaximizeState(false, false);
    Q_EMIT activeChanged();
}

void WindowFrame::setMaximizeState(bool hovered, bool pressed) {
    if (hovered == maximizeHovered_ && pressed == maximizePressed_) return;
    maximizeHovered_ = hovered;
    maximizePressed_ = pressed;
    Q_EMIT maximizeStateChanged();
}

bool WindowFrame::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) {
#ifdef Q_OS_WIN
    if (!hwnd_ || eventType != "windows_generic_MSG") return false;
    MSG* msg = static_cast<MSG*>(message);
    if (msg->hwnd != static_cast<HWND>(hwnd_)) return false;
    const HWND hwnd = msg->hwnd;
    // (Input messages come here from the event loop, with no result to give.)
    const auto answer = [result](LRESULT value) {
        if (result) *result = value;
        return true;
    };
    switch (msg->message) {
    case WM_NCCALCSIZE: {
        if (!msg->wParam) return false;
        auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam);
        const LONG top = params->rgrc[0].top;
        // The system's frame, then its caption's place given to the client area.
        const LRESULT standard = DefWindowProcW(hwnd, WM_NCCALCSIZE, msg->wParam, msg->lParam);
        if (standard != 0) return answer(standard);
        RECT& client = params->rgrc[0];
        client.top = top;
        if (IsZoomed(hwnd)) {
            client.top += resizeBorder(hwnd);  // (the part past the screen's edge)
            leaveAutoHideTaskbar(hwnd, client);
        }
        return answer(0);
    }
    case WM_NCHITTEST: {
        const LRESULT standard = DefWindowProcW(hwnd, WM_NCHITTEST, msg->wParam, msg->lParam);
        if (standard != HTCLIENT) return answer(standard);  // the system's borders
        POINT at{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
        ScreenToClient(hwnd, &at);
        if (!IsZoomed(hwnd)) {  // the top edge, where the system's caption had it
            const int border = resizeBorder(hwnd);
            // (Only the visible frame's thickness: the menus and the buttons start right under it.)
            if (at.y < GetSystemMetricsForDpi(SM_CYSIZEFRAME, GetDpiForWindow(hwnd))) {
                RECT client{};
                GetClientRect(hwnd, &client);
                return answer(at.x < border ? HTTOPLEFT : at.x >= client.right - border ? HTTOPRIGHT : HTTOP);
            }
        }
        const qreal dpr = window_ ? window_->devicePixelRatio() : 1.0;
        switch (hitTest(QPointF(at.x / dpr, at.y / dpr))) {
        case Caption:
            return answer(HTCAPTION);
        case MaximizeButton:
            return answer(HTMAXBUTTON);
        case Client:
            break;
        }
        return answer(HTCLIENT);
    }
    // The maximize button, the system's: Windows' own drawing of it (behind
    // the title bar) is never asked for.
    case WM_NCMOUSEMOVE:
        if (msg->wParam != HTMAXBUTTON) {
            setMaximizeState(false, false);
            return false;
        }
        if (!maximizeHovered_) {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE | TME_NONCLIENT;
            track.hwndTrack = hwnd;
            TrackMouseEvent(&track);
        }
        setMaximizeState(true, maximizePressed_);
        return answer(0);
    case WM_NCMOUSELEAVE:
    case WM_MOUSEMOVE:
        setMaximizeState(false, false);
        return false;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (msg->wParam != HTMAXBUTTON) return false;
        setMaximizeState(true, true);
        return answer(0);
    case WM_NCLBUTTONUP: {
        if (msg->wParam != HTMAXBUTTON) return false;
        const bool clicked = maximizePressed_;
        setMaximizeState(false, false);  // (the button moves: hovered again on the next move over it)
        if (clicked) toggleMaximized();
        return answer(0);
    }
    default:
        return false;
    }
#else
    Q_UNUSED(eventType)
    Q_UNUSED(message)
    Q_UNUSED(result)
    return false;
#endif
}

}  // namespace sub::ui
