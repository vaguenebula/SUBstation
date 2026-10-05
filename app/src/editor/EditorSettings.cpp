// Editing the project's settings: tempo, time signature, key and loop.

#include "editor/ProjectEditor.h"

#include "model/Commands.h"
#include "model/Edits.h"
#include "model/Numbers.h"

#include <QUndoStack>

#include <algorithm>

namespace sub::app {

void ProjectEditor::setSettings(const QString& text, const SettingsValues& values, const QString& mergeKey) {
    SettingsValues old;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) old.insert(it.key(), project_->setting(it.key()));
    if (old != values) push(std::make_unique<UpdateSettingsCommand>(project_, old, values, text, mergeKey));
}

void ProjectEditor::setTempo(double bpm, const QString& mergeKey) {
    const Project& p = *project_;
    const double tempo = std::clamp(formatFixed(bpm, 2).toDouble(), 20.0, 999.0);  // (rounded as Python's round(bpm, 2))
    if (tempo == p.tempo()) return;
    ClipLists current;
    for (const Track& t : p.tracks()) current.insert(t.id, t.clips);
    // Within one drag, fit from the clips as they were when the drag began, so
    // going up and back down doesn't leave clips trimmed.
    ClipLists baseline = current;
    const int index = undoStack_->index();
    const auto* last = index > 0 ? dynamic_cast<const SetTempoCommand*>(undoStack_->command(index - 1)) : nullptr;
    if (!mergeKey.isEmpty() && last != nullptr && last->mergeKey() == mergeKey &&
        last->oldValue().clips.keys() == current.keys()) {
        baseline = last->oldValue().clips;
    }
    ClipLists fitted;
    for (auto it = baseline.constBegin(); it != baseline.constEnd(); ++it) {
        fitted.insert(it.key(), edits::fitToTempo(it.value(), tempo));
    }
    push(std::make_unique<SetTempoCommand>(project_, TempoState{p.tempo(), current}, TempoState{tempo, fitted}, mergeKey));
}

void ProjectEditor::setTimeSignature(const TimeSignature& timeSignature) {
    setSettings(QStringLiteral("Change Time Signature"), {{SettingsField::TimeSignature, timeSignature}});
}

void ProjectEditor::setTimeSignature(int numerator, int denominator) {
    if (numerator < 1 ||
        std::find(kValidDenominators.begin(), kValidDenominators.end(), denominator) == kValidDenominators.end()) {
        return;  // (not a time signature)
    }
    setTimeSignature(TimeSignature{numerator, denominator});
}

void ProjectEditor::setKey(const std::optional<Key>& key) {
    setSettings(QStringLiteral("Change Key"), {{SettingsField::Key, key}});
}

void ProjectEditor::setKeyByName(const QString& name) { setKey(keyFromName(name)); }

void ProjectEditor::setLoop(bool enabled, double start, double end, const QString& mergeKey) {
    start = std::max(0.0, start);
    end = std::max(start + 0.25, end);
    setSettings(QStringLiteral("Change Loop"),
                {{SettingsField::LoopEnabled, enabled}, {SettingsField::LoopStart, start}, {SettingsField::LoopEnd, end}},
                mergeKey);
}

void ProjectEditor::setLoopEnabled(bool enabled) {
    setSettings(QStringLiteral("Toggle Loop"), {{SettingsField::LoopEnabled, enabled}});
}

}  // namespace sub::app
