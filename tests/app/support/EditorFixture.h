#pragma once
// The editor as the tests use it (the Python tests' `editor` fixture): a new
// project, its undo stack and a ProjectEditor on them, with what the editor
// refused (its `refused` messages) collected.

#include "editor/ProjectEditor.h"
#include "model/Automation.h"
#include "model/Project.h"

#include <QObject>
#include <QStringList>
#include <QUndoStack>

#include <initializer_list>
#include <utility>

namespace sub::app::test {

struct EditorFixture {
    Project project;
    QUndoStack stack;
    ProjectEditor editor{&project, &stack};
    QStringList messages;  // what the editor refused, and why

    EditorFixture() {
        QObject::connect(&editor, &ProjectEditor::refused, [this](const QString& message) { messages.append(message); });
    }

    // Short names, as the Python tests have them.
    Project& p() { return project; }
    const Track& track(const QString& id) const { return project.track(id); }
};

// An envelope of (beat, value) points (and curves, if given).
Envelope env(std::initializer_list<std::pair<double, double>> points);

// An audio clip of `durationSec` of "a.wav" (its whole source) at `start`.
Clip audioClip(const QString& id, double start, double durationSec, const QString& path = QStringLiteral("a.wav"));

}  // namespace sub::app::test
