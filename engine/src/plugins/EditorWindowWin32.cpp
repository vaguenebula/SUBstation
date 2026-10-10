// Plug-in editor windows on Windows (see EditorWindow.h): a plain Win32 window
// owned by the main window, holding the view as an HWND child.

#include "plugins/EditorWindow.h"

#include <windows.h>

#include <algorithm>

#include "platform/Unicode.h"

namespace sub::vst3 {

using Steinberg::kResultTrue;
using Steinberg::ViewRect;

// The window, and the size the view asked for during a DPI change (between
// WM_GETDPISCALEDSIZE and WM_DPICHANGED).
struct EditorWindow::Native {
    HWND hwnd = nullptr;
    bool inDpiChange = false;
    int dpiChangeWidth = 0;
    int dpiChangeHeight = 0;

    static void registerClass();
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    // The window procedure's work, for its window.
    static LRESULT handleMessage(EditorWindow& window, HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
};

namespace {

constexpr wchar_t kWindowClass[] = L"SUBstationPluginEditor";

// The module this code is in (the program, or a DLL linking the engine).
HINSTANCE thisModule() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       kWindowClass, &module);
    return module;
}

int width(const ViewRect& rect) { return rect.right - rect.left; }
int height(const ViewRect& rect) { return rect.bottom - rect.top; }

// The frame around the client area, as a rect with negative left/top.
RECT frameInsets(HWND hwnd) {
    RECT frame{0, 0, 0, 0};
    AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
                             static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), GetDpiForWindow(hwnd));
    return frame;
}

float dpiScale(HWND hwnd) { return static_cast<float>(GetDpiForWindow(hwnd)) / USER_DEFAULT_SCREEN_DPI; }

void setClientSize(HWND hwnd, int clientWidth, int clientHeight) {
    const RECT frame = frameInsets(hwnd);
    SetWindowPos(hwnd, nullptr, 0, 0, clientWidth + frame.right - frame.left, clientHeight + frame.bottom - frame.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// `x`, `y` moved so that the window (of its present size) is inside the work
// area of the screen `monitor` is on, then the window put there.
void moveInside(HWND hwnd, HMONITOR monitor, int x, int y) {
    RECT window{};
    GetWindowRect(hwnd, &window);
    const int w = window.right - window.left;
    const int h = window.bottom - window.top;
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);
    const RECT area = info.rcWork;
    x = std::max<int>(area.left, std::min<int>(x, area.right - w));
    y = std::max<int>(area.top, std::min<int>(y, area.bottom - h));
    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void placeOverOwner(HWND hwnd, HWND owner) {
    static int cascade = 0;  // successive editors don't hide each other
    const int offset = (cascade++ % 6) * 28;
    RECT window{};
    GetWindowRect(hwnd, &window);
    const int w = window.right - window.left;
    const int h = window.bottom - window.top;
    const HMONITOR monitor = MonitorFromWindow(owner ? owner : hwnd, MONITOR_DEFAULTTONEAREST);
    RECT anchor{};
    if (owner) {
        GetWindowRect(owner, &anchor);
    } else {
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(monitor, &info);
        anchor = info.rcWork;
    }
    moveInside(hwnd, monitor, anchor.left + (anchor.right - anchor.left - w) / 2 + offset,
               anchor.top + (anchor.bottom - anchor.top - h) / 3 + offset);
}

// A screen may have gone, or the window grown, since it was at `position`.
void placeAt(HWND hwnd, EditorWindow::Position position) {
    moveInside(hwnd, MonitorFromPoint({position.x, position.y}, MONITOR_DEFAULTTONEAREST), position.x, position.y);
}

// Before this window goes away or hides while it is the active one: activate the
// owner. Otherwise Windows picks the next window in the z-order, which after an
// Alt+Tab can be another application's (it only prefers the owner for WS_POPUP).
void yieldActivation(HWND hwnd) {
    if (!hwnd || GetForegroundWindow() != hwnd) return;
    HWND owner = GetWindow(hwnd, GW_OWNER);
    if (owner && IsWindowVisible(owner) && IsWindowEnabled(owner)) SetForegroundWindow(owner);
}

}  // namespace

void EditorWindow::Native::registerClass() {
    static bool registered = false;  // main thread only
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = thisModule();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // the plug-in paints everything
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    registered = true;
}

bool EditorWindow::canHold(Steinberg::IPlugView& view) {
    return view.isPlatformTypeSupported(Steinberg::kPlatformTypeHWND) == kResultTrue;
}

EditorWindow::EditorWindow(Steinberg::IPtr<Steinberg::IPlugView> view, void* ownerWindow, const std::string& title,
                           const Position* position)
    : view_(std::move(view)), native_(std::make_unique<Native>()) {
    Native::registerClass();
    auto owner = static_cast<HWND>(ownerWindow);
    if (owner && !IsWindow(owner)) owner = nullptr;
    resizable_ = view_->canResize() == kResultTrue;
    // Close only: no minimize or maximize.
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    if (resizable_) style |= WS_THICKFRAME;
    // Created where it will be (if known), so it has that screen's scale from the start.
    HWND hwnd = CreateWindowExW(0, kWindowClass, platform::toWide(title).c_str(), style,
                                position ? position->x : CW_USEDEFAULT, position ? position->y : CW_USEDEFAULT, 400,
                                300, owner, nullptr, thisModule(), this);
    native_->hwnd = hwnd;
    if (!hwnd) {
        view_ = nullptr;
        return;
    }
    setContentScale(dpiScale(hwnd));  // before attaching: the plug-in may size itself for it
    view_->setFrame(this);
    ViewRect rect{};
    if (view_->getSize(&rect) == kResultTrue && width(rect) > 0 && height(rect) > 0) {
        setClientSize(hwnd, width(rect), height(rect));
    }
    if (position) {
        placeAt(hwnd, *position);
    } else {
        placeOverOwner(hwnd, owner);
    }
    if (view_->attached(hwnd, Steinberg::kPlatformTypeHWND) != kResultTrue) {
        view_->setFrame(nullptr);
        view_ = nullptr;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        DestroyWindow(hwnd);
        native_->hwnd = nullptr;
        return;
    }
    // Some plug-ins only know their size once attached.
    if (view_->getSize(&rect) == kResultTrue && width(rect) > 0 && height(rect) > 0) {
        setClientSize(hwnd, width(rect), height(rect));
    }
    ShowWindow(hwnd, SW_SHOW);
    position_ = this->position();
}

EditorWindow::~EditorWindow() {
    detachView();
    if (HWND hwnd = native_->hwnd) {
        position_ = position();
        yieldActivation(hwnd);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        DestroyWindow(hwnd);
        native_->hwnd = nullptr;
    }
}

bool EditorWindow::isOpen() const noexcept { return native_->hwnd != nullptr; }

void EditorWindow::setTitle(const std::string& title) {
    if (native_->hwnd) SetWindowTextW(native_->hwnd, platform::toWide(title).c_str());
}

void EditorWindow::bringToFront() {
    HWND hwnd = native_->hwnd;
    if (!hwnd) return;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
}

void EditorWindow::setVisible(bool visible) {
    HWND hwnd = native_->hwnd;
    if (!hwnd) return;
    if (!visible) yieldActivation(hwnd);
    ShowWindow(hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

bool EditorWindow::isVisible() const { return native_->hwnd && IsWindowVisible(native_->hwnd); }

EditorWindow::Position EditorWindow::position() const {
    RECT window{};
    if (native_->hwnd && !IsIconic(native_->hwnd) && GetWindowRect(native_->hwnd, &window)) return {window.left, window.top};
    return position_;
}

tresult PLUGIN_API EditorWindow::resizeView(Steinberg::IPlugView* view, ViewRect* newSize) {
    if (!view_ || view != view_.get() || !newSize || width(*newSize) <= 0 || height(*newSize) <= 0) {
        return Steinberg::kInvalidArgument;
    }
    if (native_->inDpiChange) {  // WM_GETDPISCALEDSIZE asked the view; WM_DPICHANGED applies the size
        native_->dpiChangeWidth = width(*newSize);
        native_->dpiChangeHeight = height(*newSize);
        return kResultTrue;
    }
    if (resizing_) return Steinberg::kResultFalse;
    resizing_ = true;
    setClientSize(native_->hwnd, width(*newSize), height(*newSize));
    resizing_ = false;
    sizeView(*newSize);
    return kResultTrue;
}

LRESULT CALLBACK EditorWindow::Native::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (auto* window = reinterpret_cast<EditorWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
        return handleMessage(*window, hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT EditorWindow::Native::handleMessage(EditorWindow& w, HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    Native& native = *w.native_;
    switch (message) {
        case WM_ERASEBKGND:
            return 1;  // the plug-in paints

        case WM_SIZE:
            if (!w.resizing_ && w.view_ && wParam != SIZE_MINIMIZED) {
                RECT client{};
                GetClientRect(hwnd, &client);
                ViewRect wanted(0, 0, client.right, client.bottom);
                w.sizeView(wanted);
            }
            return 0;

        case WM_SIZING:
            if (w.view_ && w.resizable_) {
                auto* rect = reinterpret_cast<RECT*>(lParam);
                const RECT frame = frameInsets(hwnd);
                const int frameWidth = frame.right - frame.left;
                const int frameHeight = frame.bottom - frame.top;
                ViewRect wanted(0, 0, rect->right - rect->left - frameWidth, rect->bottom - rect->top - frameHeight);
                if (w.view_->checkSizeConstraint(&wanted) == kResultTrue) {
                    const int newWidth = width(wanted) + frameWidth;
                    const int newHeight = height(wanted) + frameHeight;
                    if (wParam == WMSZ_LEFT || wParam == WMSZ_TOPLEFT || wParam == WMSZ_BOTTOMLEFT) {
                        rect->left = rect->right - newWidth;
                    } else {
                        rect->right = rect->left + newWidth;
                    }
                    if (wParam == WMSZ_TOP || wParam == WMSZ_TOPLEFT || wParam == WMSZ_TOPRIGHT) {
                        rect->top = rect->bottom - newHeight;
                    } else {
                        rect->bottom = rect->top + newHeight;
                    }
                }
            }
            return TRUE;

        case WM_GETDPISCALEDSIZE: {
            // The window moves to a screen with another scale: tell the view first,
            // and size the window to what it asks for.
            native.inDpiChange = true;
            native.dpiChangeWidth = 0;
            native.dpiChangeHeight = 0;
            w.setContentScale(static_cast<float>(wParam) / USER_DEFAULT_SCREEN_DPI);
            if (native.dpiChangeWidth > 0 && native.dpiChangeHeight > 0) {
                RECT frame{0, 0, native.dpiChangeWidth, native.dpiChangeHeight};
                AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
                                         static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)),
                                         static_cast<UINT>(wParam));
                auto* proposed = reinterpret_cast<SIZE*>(lParam);
                proposed->cx = frame.right - frame.left;
                proposed->cy = frame.bottom - frame.top;
                return TRUE;
            }
            return FALSE;
        }

        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            const bool asked = native.inDpiChange;
            native.inDpiChange = false;
            if (!asked) w.setContentScale(dpiScale(hwnd));
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_SETFOCUS:
            if (HWND child = GetWindow(hwnd, GW_CHILD)) SetFocus(child);  // keys go to the plug-in
            return 0;

        case WM_SYSCOMMAND:
            switch (wParam & 0xFFF0) {  // keyboard shortcuts and the system menu too
                case SC_MINIMIZE:
                case SC_MAXIMIZE:
                    return 0;
                default:
                    break;
            }
            break;

        case WM_CLOSE:
            w.closed_ = true;
            yieldActivation(native.hwnd);
            w.detachView();
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            w.position_ = w.position();
            // Destroyed with its owner: the plug-in's view goes while its parent still exists.
            if (w.view_) {
                w.closed_ = true;
                w.detachView();
            }
            break;

        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if (hwnd == native.hwnd) native.hwnd = nullptr;
            break;

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace sub::vst3
