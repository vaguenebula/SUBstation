// Plug-in editor windows where the engine has none yet (platforms other than
// Windows): no view is held, the window never opens, so
// Processor::openEditor() returns false.

#include "plugins/EditorWindow.h"

namespace sub::vst3 {

struct EditorWindow::Native {};

bool EditorWindow::canHold(Steinberg::IPlugView&) { return false; }

EditorWindow::EditorWindow(Steinberg::IPtr<Steinberg::IPlugView> view, void*, const std::string&, const Position*)
    : view_(std::move(view)), native_(std::make_unique<Native>()) {}

EditorWindow::~EditorWindow() = default;

bool EditorWindow::isOpen() const noexcept { return false; }
void EditorWindow::setTitle(const std::string&) {}
void EditorWindow::bringToFront() {}
void EditorWindow::setVisible(bool) {}
bool EditorWindow::isVisible() const { return false; }
EditorWindow::Position EditorWindow::position() const { return position_; }

Steinberg::tresult PLUGIN_API EditorWindow::resizeView(Steinberg::IPlugView*, Steinberg::ViewRect*) {
    return Steinberg::kResultFalse;
}

}  // namespace sub::vst3
