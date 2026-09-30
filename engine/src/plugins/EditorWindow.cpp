#include "plugins/EditorWindow.h"

#include <windows.h>

#include <algorithm>

#include "PathUtils.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"

namespace gil::vst3 {
namespace {

using Steinberg::kResultTrue;
using Steinberg::ViewRect;

constexpr wchar_t kWindowClass[] = L"GILStudioPluginEditor";

LRESULT CALLBACK editorWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (auto* window = reinterpret_cast<EditorWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
        return window->handleMessage(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

HINSTANCE thisModule() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&editorWindowProc), &module);
    return module;
}

void registerWindowClass() {
    static bool registered = false;  // main thread only
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = editorWindowProc;
    wc.hInstance = thisModule();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // the plug-in paints everything
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    registered = true;
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

}  // namespace

EditorWindow::EditorWindow(Steinberg::IPtr<Steinberg::IPlugView> view, void* ownerWindow, const std::string& title,
                           const Position* position)
    : view_(std::move(view)) {
    registerWindowClass();
    auto owner = static_cast<HWND>(ownerWindow);
    if (owner && !IsWindow(owner)) owner = nullptr;
    resizable_ = view_->canResize() == kResultTrue;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    if (resizable_) style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    // Created where it will be (if known), so it has that screen's scale from the start.
    hwnd_ = CreateWindowExW(0, kWindowClass, widen(title).c_str(), style, position ? position->x : CW_USEDEFAULT,
                            position ? position->y : CW_USEDEFAULT, 400, 300, owner, nullptr, thisModule(), this);
    if (!hwnd_) {
        view_ = nullptr;
        return;
    }
    updateContentScale();  // before attaching: the plug-in may size itself for it
    view_->setFrame(this);
    ViewRect rect{};
    if (view_->getSize(&rect) == kResultTrue && width(rect) > 0 && height(rect) > 0) {
        setClientSize(width(rect), height(rect));
    }
    if (position) {
        placeAt(*position);
    } else {
        placeOverOwner(owner);
    }
    if (view_->attached(hwnd_, Steinberg::kPlatformTypeHWND) != kResultTrue) {
        view_->setFrame(nullptr);
        view_ = nullptr;
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return;
    }
    // Some plug-ins only know their size once attached.
    if (view_->getSize(&rect) == kResultTrue && width(rect) > 0 && height(rect) > 0) {
        setClientSize(width(rect), height(rect));
    }
    ShowWindow(hwnd_, SW_SHOW);
    rememberPosition();
}

EditorWindow::~EditorWindow() {
    detachView();
    if (hwnd_) {
        rememberPosition();
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void EditorWindow::detachView() {
    if (!view_) return;
    view_->setFrame(nullptr);
    view_->removed();
    view_ = nullptr;
}

void EditorWindow::setTitle(const std::string& title) {
    if (hwnd_) SetWindowTextW(hwnd_, widen(title).c_str());
}

void EditorWindow::bringToFront() {
    if (!hwnd_) return;
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd_);
}

void EditorWindow::setVisible(bool visible) {
    if (hwnd_) ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

bool EditorWindow::isVisible() const { return hwnd_ && IsWindowVisible(hwnd_); }

EditorWindow::Position EditorWindow::position() const {
    RECT window{};
    if (hwnd_ && !IsIconic(hwnd_) && GetWindowRect(hwnd_, &window)) return {window.left, window.top};
    return position_;
}

void EditorWindow::rememberPosition() { position_ = position(); }

void EditorWindow::updateContentScale() {
    scale_ = static_cast<float>(GetDpiForWindow(hwnd_)) / USER_DEFAULT_SCREEN_DPI;
    Steinberg::FUnknownPtr<Steinberg::IPlugViewContentScaleSupport> support(view_);
    if (support) support->setContentScaleFactor(scale_);
}

void EditorWindow::setClientSize(int clientWidth, int clientHeight) {
    const RECT frame = frameInsets(hwnd_);
    SetWindowPos(hwnd_, nullptr, 0, 0, clientWidth + frame.right - frame.left, clientHeight + frame.bottom - frame.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void EditorWindow::placeOverOwner(HWND owner) {
    static int cascade = 0;  // successive editors don't hide each other
    const int offset = (cascade++ % 6) * 28;
    RECT window{};
    GetWindowRect(hwnd_, &window);
    const int w = window.right - window.left;
    const int h = window.bottom - window.top;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(owner ? owner : hwnd_, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT area = monitor.rcWork;
    RECT anchor = area;
    if (owner) GetWindowRect(owner, &anchor);
    int x = anchor.left + (anchor.right - anchor.left - w) / 2 + offset;
    int y = anchor.top + (anchor.bottom - anchor.top - h) / 3 + offset;
    x = std::max<int>(area.left, std::min<int>(x, area.right - w));
    y = std::max<int>(area.top, std::min<int>(y, area.bottom - h));
    SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void EditorWindow::placeAt(Position position) {
    RECT window{};
    GetWindowRect(hwnd_, &window);
    const int w = window.right - window.left;
    const int h = window.bottom - window.top;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromPoint({position.x, position.y}, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT area = monitor.rcWork;  // a screen may have gone, or the window grown, since
    const int x = std::max<int>(area.left, std::min<int>(position.x, area.right - w));
    const int y = std::max<int>(area.top, std::min<int>(position.y, area.bottom - h));
    SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

tresult PLUGIN_API EditorWindow::resizeView(Steinberg::IPlugView* view, ViewRect* newSize) {
    if (!view_ || view != view_.get() || !newSize || width(*newSize) <= 0 || height(*newSize) <= 0) {
        return Steinberg::kInvalidArgument;
    }
    if (inDpiChange_) {  // WM_GETDPISCALEDSIZE asked the view; WM_DPICHANGED applies the size
        dpiChangeSize_ = {width(*newSize), height(*newSize)};
        return kResultTrue;
    }
    if (resizing_) return Steinberg::kResultFalse;
    resizing_ = true;
    setClientSize(width(*newSize), height(*newSize));
    resizing_ = false;
    ViewRect current{};
    if (view_->getSize(&current) != kResultTrue || width(current) != width(*newSize) ||
        height(current) != height(*newSize)) {
        view_->onSize(newSize);
    }
    return kResultTrue;
}

intptr_t EditorWindow::handleMessage(HWND hwnd, unsigned message, uintptr_t wParam, intptr_t lParam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;  // the plug-in paints

        case WM_SIZE:
            if (!resizing_ && view_ && wParam != SIZE_MINIMIZED) {
                RECT client{};
                GetClientRect(hwnd, &client);
                ViewRect wanted(0, 0, client.right, client.bottom);
                ViewRect current{};
                if (view_->getSize(&current) != kResultTrue || width(current) != width(wanted) ||
                    height(current) != height(wanted)) {
                    view_->onSize(&wanted);
                }
            }
            return 0;

        case WM_SIZING:
            if (view_ && resizable_) {
                auto* rect = reinterpret_cast<RECT*>(lParam);
                const RECT frame = frameInsets(hwnd);
                const int frameWidth = frame.right - frame.left;
                const int frameHeight = frame.bottom - frame.top;
                ViewRect wanted(0, 0, rect->right - rect->left - frameWidth, rect->bottom - rect->top - frameHeight);
                if (view_->checkSizeConstraint(&wanted) == kResultTrue) {
                    const int w = width(wanted) + frameWidth;
                    const int h = height(wanted) + frameHeight;
                    if (wParam == WMSZ_LEFT || wParam == WMSZ_TOPLEFT || wParam == WMSZ_BOTTOMLEFT) {
                        rect->left = rect->right - w;
                    } else {
                        rect->right = rect->left + w;
                    }
                    if (wParam == WMSZ_TOP || wParam == WMSZ_TOPLEFT || wParam == WMSZ_TOPRIGHT) {
                        rect->top = rect->bottom - h;
                    } else {
                        rect->bottom = rect->top + h;
                    }
                }
            }
            return TRUE;

        case WM_GETDPISCALEDSIZE: {
            // The window moves to a screen with another scale: tell the view first,
            // and size the window to what it asks for.
            inDpiChange_ = true;
            dpiChangeSize_ = {0, 0};
            scale_ = static_cast<float>(wParam) / USER_DEFAULT_SCREEN_DPI;
            Steinberg::FUnknownPtr<Steinberg::IPlugViewContentScaleSupport> support(view_);
            if (support) support->setContentScaleFactor(scale_);
            if (dpiChangeSize_.cx > 0 && dpiChangeSize_.cy > 0) {
                RECT frame{0, 0, dpiChangeSize_.cx, dpiChangeSize_.cy};
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
            const bool asked = inDpiChange_;
            inDpiChange_ = false;
            if (!asked) updateContentScale();
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_SETFOCUS:
            if (HWND child = GetWindow(hwnd, GW_CHILD)) SetFocus(child);  // keys go to the plug-in
            return 0;

        case WM_CLOSE:
            closed_ = true;
            detachView();
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            rememberPosition();
            // Destroyed with its owner: the plug-in's view goes while its parent still exists.
            if (view_) {
                closed_ = true;
                detachView();
            }
            break;

        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if (hwnd == hwnd_) hwnd_ = nullptr;
            break;

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace gil::vst3
