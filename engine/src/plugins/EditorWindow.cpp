// What every system's editor window does with the plug-in's view (see
// EditorWindow.h; the windows themselves are EditorWindowWin32.cpp's...).

#include "plugins/EditorWindow.h"

#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"

namespace sub::vst3 {

void EditorWindow::detachView() {
    if (!view_) return;
    view_->setFrame(nullptr);
    view_->removed();
    view_ = nullptr;
}

void EditorWindow::sizeView(Steinberg::ViewRect& size) {
    Steinberg::ViewRect current{};
    if (view_->getSize(&current) != Steinberg::kResultTrue || current.getWidth() != size.getWidth() ||
        current.getHeight() != size.getHeight()) {
        view_->onSize(&size);
    }
}

void EditorWindow::setContentScale(float scale) {
    scale_ = scale;
    Steinberg::FUnknownPtr<Steinberg::IPlugViewContentScaleSupport> support(view_);
    if (support) support->setContentScaleFactor(scale_);
}

}  // namespace sub::vst3
