#pragma once
// A top-level window holding a VST3 plug-in's editor (IPlugView). It is a plain
// Win32 window owned by the main window, so it floats above it; Qt's event loop
// dispatches its messages like any other window's. It sizes itself to the view,
// follows the view's resize requests (IPlugFrame), lets the user resize it when
// the view can resize, and tells the view about DPI changes.
//
// Main thread only.

#include <cstdint>
#include <string>

#include "pluginterfaces/base/smartpointer.h"
#include "pluginterfaces/gui/iplugview.h"
#include "plugins/Vst3Support.h"

struct HWND__;
using HWND = HWND__*;

namespace gil::vst3 {

class EditorWindow final : public Steinberg::IPlugFrame {
public:
    // Opens the window and attaches the view to it; check isOpen() afterwards.
    EditorWindow(Steinberg::IPtr<Steinberg::IPlugView> view, void* ownerWindow, const std::string& title);
    ~EditorWindow();
    EditorWindow(const EditorWindow&) = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;

    bool isOpen() const noexcept { return hwnd_ != nullptr; }
    // The user closed the window; the owner deletes this object when it notices.
    bool wasClosed() const noexcept { return closed_; }
    void setTitle(const std::string& title);
    void bringToFront();

    // IPlugFrame
    tresult PLUGIN_API resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize) override;
    GIL_HOST_OWNED_FUNKNOWN(Steinberg::IPlugFrame)

    // The window procedure's work (called by it, from the message loop).
    intptr_t handleMessage(HWND hwnd, unsigned message, uintptr_t wParam, intptr_t lParam);

private:
    void setClientSize(int width, int height);
    void placeOverOwner(HWND owner);
    void updateContentScale();
    void detachView();

    struct Size {
        int cx = 0;
        int cy = 0;
    };

    Steinberg::IPtr<Steinberg::IPlugView> view_;
    HWND hwnd_ = nullptr;
    bool resizable_ = false;
    bool resizing_ = false;     // inside resizeView: our own WM_SIZE must not call onSize
    bool inDpiChange_ = false;  // between WM_GETDPISCALEDSIZE and WM_DPICHANGED
    Size dpiChangeSize_;        // the size the view asked for during a DPI change
    bool closed_ = false;
    float scale_ = 1.f;
};

}  // namespace gil::vst3
