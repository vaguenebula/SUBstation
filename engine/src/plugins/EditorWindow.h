#pragma once
// A top-level window holding a VST3 plug-in's editor (IPlugView). It is owned
// by the main window, so it floats above it; the application's event loop
// dispatches its events like any other window's. It sizes itself to the view,
// follows the view's resize requests (IPlugFrame), lets the user resize it when
// the view can resize, and tells the view its content scale (the screen's DPI).
//
// Main thread only. The window is the system's: one file per system holds it
// (`Native`): EditorWindowWin32.cpp, a Win32 window; EditorWindowNone.cpp
// where plug-ins show no editor yet (it never opens). What every system's
// window does with the view is EditorWindow.cpp's.

#include <cstdint>
#include <memory>
#include <string>

#include "pluginterfaces/base/smartpointer.h"
#include "pluginterfaces/gui/iplugview.h"
#include "plugins/Vst3Support.h"

namespace sub::vst3 {

class EditorWindow final : public Steinberg::IPlugFrame {
public:
    struct Position {  // the window's top-left corner, in screen coordinates
        int x = 0;
        int y = 0;
    };

    // Whether this system's window can hold the view (the view supports its
    // platform type: an HWND on Windows).
    static bool canHold(Steinberg::IPlugView& view);

    // Opens the window and attaches the view to it; check isOpen() afterwards. It
    // goes at `position` (kept on screen) if given, else over the owner (the
    // main window's native handle).
    EditorWindow(Steinberg::IPtr<Steinberg::IPlugView> view, void* ownerWindow, const std::string& title,
                 const Position* position = nullptr);
    ~EditorWindow();
    EditorWindow(const EditorWindow&) = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;

    bool isOpen() const noexcept;
    // The user closed the window; the owner deletes this object when it notices.
    bool wasClosed() const noexcept { return closed_; }
    void setTitle(const std::string& title);
    void bringToFront();
    // Hidden, the window keeps its place and the plug-in its view; shown again,
    // it doesn't take the focus.
    void setVisible(bool visible);
    bool isVisible() const;
    // Where the window is, or was when it went.
    Position position() const;

    // IPlugFrame
    tresult PLUGIN_API resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize) override;
    SUB_HOST_OWNED_FUNKNOWN(Steinberg::IPlugFrame)

private:
    struct Native;  // the system's window and what only it needs (EditorWindowWin32.cpp...)

    // What every system's window does with the view (EditorWindow.cpp):
    // Detaches the view from the window (the window closing, or going).
    void detachView();
    // Tells the view the size it now has (the window was resized), unless it is
    // that size already.
    void sizeView(Steinberg::ViewRect& size);
    // Tells the view the scale its window's screen draws at.
    void setContentScale(float scale);

    Steinberg::IPtr<Steinberg::IPlugView> view_;
    std::unique_ptr<Native> native_;
    bool resizable_ = false;
    bool resizing_ = false;  // inside resizeView: the window resizing itself mustn't size the view back
    bool closed_ = false;
    float scale_ = 1.f;
    Position position_;  // kept when the window goes
};

}  // namespace sub::vst3
