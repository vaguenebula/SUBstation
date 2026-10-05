// Editing automation: envelopes (points, curves, ranges, copy and paste) and
// the automation lanes a track shows.

#include "editor/EditorSupport.h"
#include "editor/ProjectEditor.h"

#include "model/Commands.h"

#include <QUndoStack>

#include <algorithm>

namespace sub::app {

using editing::Macro;

namespace {

// These lanes, each once, in their order.
QList<LaneRef> distinct(const QList<LaneRef>& lanes) {
    QList<LaneRef> result;
    for (const LaneRef& lane : lanes) {
        if (!result.contains(lane)) result.append(lane);
    }
    return result;
}

std::vector<int> indexList(const QList<int>& indices) { return {indices.begin(), indices.end()}; }

}  // namespace

void ProjectEditor::setEnvelope(const QString& owner, const QString& key, const Envelope& points, const QString& text,
                                const QString& mergeKey) {
    const Envelope nw = automation::normalize(points);
    const Envelope old = project_->envelope(owner, key);
    if (nw != old) push(std::make_unique<SetEnvelopeCommand>(project_, owner, key, old, nw, text, mergeKey));
}

int ProjectEditor::addAutomationPoint(const QString& owner, const QString& key, double beat, double value,
                                      const QString& mergeKey) {
    if (!project_->hasOwner(owner)) return -1;
    const auto [points, index] = automation::addPoint(project_->envelope(owner, key), beat, value);
    setEnvelope(owner, key, points, QStringLiteral("Add Automation Point"), mergeKey);
    return index;
}

QMap<int, int> ProjectEditor::moveAutomationPoints(const QString& owner, const QString& key, const Envelope& original,
                                                   const QList<int>& indices, double deltaBeats, double deltaValue,
                                                   const QString& mergeKey) {
    const auto [points, where] = automation::movePointsMapped(original, indexList(indices), deltaBeats, deltaValue);
    setEnvelope(owner, key, points, QStringLiteral("Move Automation"), mergeKey);
    return where;
}

void ProjectEditor::deleteAutomationPoints(const QString& owner, const QString& key, const QList<int>& indices) {
    if (!project_->hasOwner(owner)) return;
    setEnvelope(owner, key, automation::deletePoints(project_->envelope(owner, key), indexList(indices)),
                QStringLiteral("Delete Automation Point"));
}

void ProjectEditor::setAutomationCurve(const QString& owner, const QString& key, const Envelope& original, int index,
                                       double curve, const QString& mergeKey) {
    setEnvelope(owner, key, automation::setCurve(original, index, curve), QStringLiteral("Change Automation Curve"),
                mergeKey);
}

void ProjectEditor::clearEnvelope(const QString& owner, const QString& key) {
    if (project_->hasOwner(owner)) setEnvelope(owner, key, {}, QStringLiteral("Delete Envelope"));
}

void ProjectEditor::eachLane(const QString& text, const QList<LaneRef>& lanes,
                             const std::function<Envelope(const Envelope&)>& change) {
    // `change(envelope)` on each lane, as one undo step.
    std::vector<std::pair<LaneRef, Envelope>> changed;
    for (const LaneRef& lane : distinct(lanes)) {
        if (!project_->hasOwner(lane.first)) continue;
        const Envelope current = project_->envelope(lane.first, lane.second);
        Envelope nw = change(current);
        if (nw != current) changed.emplace_back(lane, std::move(nw));
    }
    if (changed.empty()) return;
    Macro macro(undoStack_, text);
    for (const auto& [lane, points] : changed) setEnvelope(lane.first, lane.second, points, text);
}

void ProjectEditor::deleteAutomationRange(double start, double end, const QList<LaneRef>& lanes) {
    eachLane(QStringLiteral("Delete Automation"), lanes,
             [&](const Envelope& points) { return automation::removeRange(points, start, end); });
}

void ProjectEditor::moveAutomationRange(double start, double end, const QMap<LaneRef, Envelope>& originals,
                                        double deltaBeats, double deltaValue, const QString& mergeKey) {
    QMap<LaneRef, Envelope> nw;
    QMap<LaneRef, Envelope> current;
    for (auto it = originals.constBegin(); it != originals.constEnd(); ++it) {
        if (it.value().empty() || !project_->hasOwner(it.key().first)) continue;
        nw.insert(it.key(), automation::moveRange(it.value(), start, end, deltaBeats, deltaValue));
        current.insert(it.key(), project_->envelope(it.key().first, it.key().second));
    }
    if (nw != current) {
        push(std::make_unique<SetEnvelopesCommand>(project_, current, nw, QStringLiteral("Move Automation"), mergeKey));
    }
}

void ProjectEditor::duplicateAutomationRange(double start, double end, const QList<LaneRef>& lanes) {
    eachLane(QStringLiteral("Duplicate Automation"), lanes, [&](const Envelope& points) {
        if (points.empty()) return points;
        return automation::pasteRange(points, automation::copyRange(points, start, end), end, end - start);
    });
}

std::optional<CopiedAutomation> ProjectEditor::copyAutomationRange(double start, double end,
                                                                   const QList<LaneRef>& lanes) const {
    if (end <= start) return std::nullopt;
    CopiedAutomation content{end - start, {}};
    for (const LaneRef& lane : distinct(lanes)) {
        if (!project_->hasOwner(lane.first)) continue;
        Envelope points = automation::copyRange(project_->envelope(lane.first, lane.second), start, end);
        if (!points.empty()) content.lanes.emplace_back(lane, std::move(points));
    }
    if (content.lanes.empty()) return std::nullopt;
    return content;
}

std::optional<CopiedAutomation> ProjectEditor::cutAutomationRange(double start, double end,
                                                                  const QList<LaneRef>& lanes) {
    auto content = copyAutomationRange(start, end, lanes);
    if (content) {
        QList<LaneRef> copied;
        for (const auto& [lane, _] : content->lanes) copied.append(lane);
        eachLane(QStringLiteral("Cut Automation"), copied,
                 [&](const Envelope& points) { return automation::removeRange(points, start, end); });
    }
    return content;
}

QList<std::optional<LaneRef>> ProjectEditor::automationPasteTargets(const CopiedAutomation& content,
                                                                    const QList<LaneRef>& lanes) const {
    const QList<LaneRef> selected = distinct(lanes);
    QList<LaneRef> targets;
    const auto copied = static_cast<qsizetype>(content.lanes.size());
    if (!selected.isEmpty() && (copied == 1 || copied == selected.size())) {
        targets = selected;
    } else {
        for (const auto& [lane, _] : content.lanes) targets.append(lane);
    }
    QList<std::optional<LaneRef>> result;
    for (const LaneRef& lane : targets) result.append(laneExists(lane) ? std::optional<LaneRef>(lane) : std::nullopt);
    return result;
}

bool ProjectEditor::laneExists(const LaneRef& lane) const {
    const auto& [owner, key] = lane;
    if (!project_->hasOwner(owner) || !automation::isKey(key)) return false;
    if (const auto device = automation::keyDevice(key)) return project_->hasDevice(owner, *device);
    const auto send = automation::keySend(key);
    return !send || project_->track(owner).sends.contains(*send);
}

QList<LaneRef> ProjectEditor::pasteAutomation(const CopiedAutomation& content, double atBeat,
                                              const QList<LaneRef>& lanes) {
    const double at = std::max(0.0, atBeat);
    const QList<std::optional<LaneRef>> targets = automationPasteTargets(content, lanes);
    QList<LaneRef> pasted;
    QMap<LaneRef, Envelope> sources;
    for (qsizetype i = 0; i < targets.size(); ++i) {
        if (!targets[i] || content.lanes.empty()) continue;
        const Envelope& points =
            content.lanes.size() == 1 ? content.lanes.front().second : content.lanes[static_cast<std::size_t>(i)].second;
        if (!pasted.contains(*targets[i])) pasted.append(*targets[i]);
        sources.insert(*targets[i], points);
    }
    QMap<LaneRef, Envelope> old;
    QMap<LaneRef, Envelope> changed;
    for (auto it = sources.constBegin(); it != sources.constEnd(); ++it) {
        const Envelope current = project_->envelope(it.key().first, it.key().second);
        const Envelope points = automation::dropRedundant(automation::pasteRange(current, it.value(), at, content.length),
                                                          {at, at + content.length});
        if (points != current) {
            old.insert(it.key(), current);
            changed.insert(it.key(), points);
        }
    }
    if (!changed.isEmpty()) {
        push(std::make_unique<SetEnvelopesCommand>(project_, old, changed, QStringLiteral("Paste Automation")));
    }
    return pasted;
}

void ProjectEditor::setAutomationLocked(bool locked) {
    if (locked != project_->automationLocked()) project_->updateSettings({{SettingsField::AutomationLocked, locked}});
}

// --- The lanes shown (view state) ---

void ProjectEditor::updateView(const QString& owner, const std::function<void(AutomationView&)>& change) {
    const AutomationView& view = project_->automationView(owner);
    AutomationView nw = view;
    change(nw);
    if (nw != view) project_->setAutomationView(owner, nw);
}

QString ProjectEditor::defaultAutomationKey(const QString& owner) const {
    const Track* track = project_->findTrack(owner);
    if (track == nullptr || track->automation.isEmpty()) return automation::kMixerVolume;
    return track->automation.begin()->first;
}

void ProjectEditor::showAutomation(const QString& owner, const QString& key) {
    if (!project_->hasOwner(owner)) return;
    const AutomationView& view = project_->automationView(owner);
    const QString shown =
        !key.isEmpty() ? key : (view.key && !view.key->isEmpty() ? *view.key : defaultAutomationKey(owner));
    updateView(owner, [&](AutomationView& v) {
        v.shown = true;
        v.key = shown;
    });
}

void ProjectEditor::hideAutomation(const QString& owner) {
    if (project_->hasOwner(owner)) updateView(owner, [](AutomationView& v) { v.shown = false; });
}

bool ProjectEditor::toggleAllAutomation() {
    const QStringList owners = project_->owners();
    const bool show = !std::all_of(owners.begin(), owners.end(),
                                   [&](const QString& o) { return project_->automationView(o).shown; });
    for (const QString& owner : owners) {
        if (show) {
            showAutomation(owner);
        } else {
            hideAutomation(owner);
        }
    }
    return show;
}

void ProjectEditor::addAutomationLane(const QString& owner) {
    if (!project_->hasOwner(owner)) return;
    const AutomationView& view = project_->automationView(owner);
    QStringList shown = view.lanes;
    if (view.key) shown.append(*view.key);
    QStringList candidates = project_->automation(owner).keys();
    candidates << automation::kMixerVolume << automation::kMixerPan;
    const auto free = std::find_if(candidates.begin(), candidates.end(), [&](const QString& k) { return !shown.contains(k); });
    const QString key = free != candidates.end() ? *free : candidates.front();
    const QString main = view.key && !view.key->isEmpty() ? *view.key : defaultAutomationKey(owner);
    updateView(owner, [&](AutomationView& v) {
        v.shown = true;
        v.key = main;
        v.lanes.append(key);
    });
}

void ProjectEditor::setAutomationLane(const QString& owner, int index, const QString& key) {
    if (!project_->hasOwner(owner)) return;
    const AutomationView& view = project_->automationView(owner);
    if (index < 0) {
        updateView(owner, [&](AutomationView& v) {
            v.shown = true;
            v.key = key;
        });
    } else if (index < view.lanes.size()) {
        updateView(owner, [&](AutomationView& v) { v.lanes[index] = key; });
    }
}

void ProjectEditor::removeAutomationLane(const QString& owner, int index) {
    if (!project_->hasOwner(owner)) return;
    if (index >= 0 && index < project_->automationView(owner).lanes.size()) {
        updateView(owner, [&](AutomationView& v) { v.lanes.removeAt(index); });
    }
}

void ProjectEditor::resetAutomationView(const QString& owner) {
    if (project_->hasOwner(owner)) updateView(owner, [](AutomationView& v) { v = AutomationView{}; });
}

}  // namespace sub::app
