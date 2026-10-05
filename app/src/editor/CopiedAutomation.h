#pragma once
// Automation copied from a lane range (Ctrl+C / Ctrl+X on automation lanes):
// what ProjectEditor::copyAutomationRange gives and pasteAutomation takes.

#include "model/Automation.h"
#include "model/Project.h"  // LaneRef

#include <utility>
#include <vector>

namespace sub::app {

// Automation copied from a lane range, `length` beats long: each lane's
// envelope over it (points from beat 0), in the lanes' order.
struct CopiedAutomation {
    double length = 0.0;
    std::vector<std::pair<LaneRef, Envelope>> lanes;

    friend bool operator==(const CopiedAutomation&, const CopiedAutomation&) = default;
};

}  // namespace sub::app
