#pragma once
// Where a new track goes: its index among the arrangement's tracks, and the
// group it goes into.

#include <QString>

#include <optional>
#include <variant>

namespace sub::app {

// A new track's group is the one at the place it is inserted (as
// Project::parentAt has it: amid a group's tracks it has to be in that group).
struct AtIndex {
    friend bool operator==(AtIndex, AtIndex) = default;
};
inline constexpr AtIndex kAtIndex{};

// A new track's group: kAtIndex, none (std::nullopt), or a group (its id). A
// group it can't be in there gives way to the one there.
using TrackParent = std::variant<AtIndex, std::optional<QString>>;

// Where a track inserted "after" another goes (ProjectEditor::insertionPoint):
// after it and what is in it, in its group. No index: last, in no group.
struct InsertionPoint {
    std::optional<int> index;
    std::optional<QString> parent;

    friend bool operator==(const InsertionPoint&, const InsertionPoint&) = default;
};

}  // namespace sub::app
