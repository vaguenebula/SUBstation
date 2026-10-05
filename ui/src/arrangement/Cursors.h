#pragma once

// The arrangement's own mouse cursors, drawn once (on the GUI thread, when
// first asked for).

#include <QCursor>

namespace sub::ui::arrangement {

// Ableton-style bracket: '[' trims a clip's start, ']' its end. The bracket
// opens toward the clip being trimmed; the hotspot is on its upright.
QCursor trimCursor(bool left);
// The arrow with a small plus beside it: a click adds a breakpoint.
QCursor addCursor();

}  // namespace sub::ui::arrangement
