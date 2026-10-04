// Plug-in editor windows where the engine has none (platforms other than
// Windows): the window never opens, so Processor::openEditor() returns false.

#include "plugins/EditorWindow.h"

namespace sub::vst3 {

EditorWindow::EditorWindow(Steinberg::IPtr<Steinberg::IPlugView> view, void*, const std::string&, const Position*)
    : view_(std::move(view)) {}

EditorWindow::~EditorWindow() = default;

void EditorWindow::setTitle(const std::string&) {}
void EditorWindow::bringToFront() {}
void EditorWindow::setVisible(bool) {}
bool EditorWindow::isVisible() const { return false; }
EditorWindow::Position EditorWindow::position() const { return position_; }

Steinberg::tresult PLUGIN_API EditorWindow::resizeView(Steinberg::IPlugView*, Steinberg::ViewRect*) {
    return Steinberg::kResultFalse;
}

}  // namespace sub::vst3
