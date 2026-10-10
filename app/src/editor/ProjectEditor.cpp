#include "editor/ProjectEditor.h"

#include "model/Errors.h"
#include "model/Ids.h"

#include <QUndoCommand>
#include <QUndoStack>

namespace sub::app {

ProjectEditor::ProjectEditor(Project* project, QUndoStack* undoStack, QObject* parent)
    : QObject(parent), project_(project), undoStack_(undoStack) {}

ProjectEditor::~ProjectEditor() = default;

bool ProjectEditor::push(std::unique_ptr<QUndoCommand> command) {
    if (const auto problem = frozenProblem(*command)) {
        Q_EMIT refused(*problem);
        return false;
    }
    undoStack_->push(command.release());
    return true;
}

bool ProjectEditor::reportRefusal(const std::function<void()>& edit) {
    try {
        edit();
        return true;
    } catch (const EditError& error) {
        Q_EMIT refused(error.message());
        return false;
    }
}

// --- For QML ---

bool ProjectEditor::trySetTrackParam(const QString& trackId, const QString& field, double value,
                                     const QString& mergeKey) {
    const auto known = trackFieldFromName(field);
    const QList<TrackField> mixer{TrackField::VolumeDb, TrackField::Pan, TrackField::Mute, TrackField::Solo};
    if (!known || !mixer.contains(*known) || !project_->hasOwner(trackId)) return false;
    return reportRefusal([&] { setTrackParam(trackId, *known, value, mergeKey); });
}

bool ProjectEditor::trySetSendLevel(const QString& trackId, const QString& returnId, double levelDb,
                                    const QString& mergeKey) {
    if (!project_->hasOwner(trackId)) return false;
    return reportRefusal([&] { setSend(trackId, returnId, levelDb, std::nullopt, mergeKey); });
}

bool ProjectEditor::trySetSendPreFader(const QString& trackId, const QString& returnId, bool preFader) {
    if (!project_->hasOwner(trackId)) return false;
    return reportRefusal([&] { setSend(trackId, returnId, std::nullopt, preFader); });
}

bool ProjectEditor::trySetTrackInput(const QString& trackId, const QList<int>& channels) {
    if (!project_->hasOwner(trackId)) return false;
    return reportRefusal([&] { setTrackInput(trackId, std::vector<int>(channels.begin(), channels.end())); });
}

bool ProjectEditor::trySetTrackInputTrack(const QString& trackId, const QString& sourceId) {
    if (!project_->hasOwner(trackId)) return false;
    return reportRefusal([&] {
        setTrackInputTrack(trackId, optionalId(sourceId));
    });
}

bool ProjectEditor::trySetTrackMonitor(const QString& trackId, const QString& mode) {
    if (!project_->hasOwner(trackId)) return false;
    return reportRefusal([&] { setTrackMonitor(trackId, mode); });
}

bool ProjectEditor::trySetTrackInputTap(const QString& trackId, const QString& tap) {
    if (!project_->hasOwner(trackId)) return false;
    return reportRefusal([&] { setTrackInputTap(trackId, tap); });
}

bool ProjectEditor::trySetTrackOutput(const QString& trackId, const QString& to, const QString& id) {
    if (!project_->hasOwner(trackId)) return false;
    return reportRefusal([&] {
        Output output;
        if (to == u"master") {
            output = Output::master();
        } else if (to == u"none") {
            output = Output::none();
        } else if (to == u"track") {
            output = Output::track(id);
        } else if (to == u"sidechain") {
            output = Output::sidechain(id);
        } else if (to != u"group") {
            throw EditError(QStringLiteral("No such output: %1").arg(to));
        }
        setTrackOutput(trackId, output);
    });
}

bool ProjectEditor::trySetTrackMidiInput(const QString& trackId, bool enabled, const QString& device, int channel) {
    if (!project_->hasOwner(trackId)) return false;
    return reportRefusal([&] {
        setTrackMidiInput(trackId, enabled ? std::optional<MidiInput>(MidiInput{device, channel}) : std::nullopt);
    });
}

bool ProjectEditor::trySetDeviceSidechain(const QString& trackId, const QString& deviceId,
                                          const QString& sourceTrackId, const QString& tap) {
    if (!project_->hasDevice(trackId, deviceId)) return false;
    return reportRefusal([&] {
        setDeviceSidechain(trackId, deviceId,
                           sourceTrackId.isEmpty() ? std::nullopt : std::optional<Sidechain>(Sidechain{sourceTrackId, tap}));
    });
}

QString ProjectEditor::tryAddRackChain(const QString& trackId, const QString& rackId, int index, const QString& name) {
    if (!project_->hasOwner(trackId)) return {};
    QString chain;
    reportRefusal([&] { chain = addRackChain(trackId, rackId, index, name); });
    return chain;
}

bool ProjectEditor::tryMapMacro(const QString& trackId, const QString& rackId, int index, const QString& deviceId,
                                const QString& paramId, double low, double high) {
    if (!project_->hasDevice(trackId, rackId)) return false;
    return reportRefusal([&] { mapMacro(trackId, rackId, index, deviceId, paramId, low, high); });
}

}  // namespace sub::app
