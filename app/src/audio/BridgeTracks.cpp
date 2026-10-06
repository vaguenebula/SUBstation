// Model -> engine for tracks: engine tracks for the project's tracks (and
// groups, returns, the master), their mixers, where their outputs go, their
// sends, their clips and notes (and a drag's preview of them), and the
// project's tempo, time signature and loop.

#include "audio/AudioFiles.h"
#include "audio/AudioSettings.h"
#include "audio/BridgePrivate.h"
#include "audio/EngineDescs.h"

#include "model/Project.h"
#include "model/Timebase.h"

#include <algorithm>

namespace sub::app {

void EngineBridge::onReset() {
    removeEngineTracks();
    Private& d = *d_;
    d.devices.clear();
    d.pids.clear();
    d.where.clear();
    d.chainOwner.clear();
    d.chainOwner.insert(kMaster, kMaster);
    d.rackOfChain.clear();
    d.rackOrders.clear();
    d.chainMixer.clear();
    d.chainMeters.clear();
    d.enabled.clear();
    d.pluginIds.clear();
    d.pluginStates.clear();
    d.paramIds.clear();
    d.paramInfos.clear();
    d.paramSpecs.clear();
    d.automating.clear();
    d.overridden.clear();
    d.mixer.clear();
    d.inputs.clear();
    d.outputs.clear();
    d.sends.clear();
    d.sendLevels.clear();
    d.frozen.clear();
    d.sidechains.clear();
    d.pluginErrors.clear();
    d.meters.clear();
    d.editorsWanted.clear();
    d.hiddenEditors.clear();
    d.previewing.clear();
    d.reversed.clear();
    stopLoadingPlugins();
    QStringList ids;
    for (const Track* track : project_->allTracks()) ids.append(track->id);
    d.deferring = true;  // its plug-ins load after it shows (BridgeLoading.cpp)
    try {
        for (const QString& id : ids) addEngineTrack(id);
    } catch (...) {
        d.deferring = false;
        throw;
    }
    d.deferring = false;
    pushOutputs();
    pushAllSends();  // (into returns added after the tracks sending to them)
    pushAllInputs();  // (from tracks added after the tracks taking them)
    pushSidechains();
    pushSettings();
    // Forget decoded audio the new project doesn't use.
    QSet<QString> used;
    for (const Track& track : project_->tracks()) {
        if (track.isMidi()) continue;
        for (const Clip& clip : track.clips) used.insert(sourceKey(clip.path));
    }
    for (const Track* track : project_->allTracks()) {
        if (track->frozen) used.insert(sourceKey(track->frozen->path));
    }
    for (auto it = d.sources.begin(); it != d.sources.end();) {
        it = used.contains(it.key()) ? std::next(it) : d.sources.erase(it);
    }
    engine_.releaseUnusedSources();
    startLoadingPlugins();
}

// Every track goes from the engine, with its devices; the master stays, without its devices.
void EngineBridge::removeEngineTracks() {
    forgetChainDevices(kMaster, true);
    const QMap<QString, quint32> trackIds = d_->trackIds;
    for (auto it = trackIds.constBegin(); it != trackIds.constEnd(); ++it) {
        if (it.key() == kMaster) continue;
        forgetChainDevices(it.key(), false);
        engine_.removeTrack(it.value());
        d_->trackIds.remove(it.key());
        dropChain(it.key());
    }
    if (!d_->chainOwner.contains(kMaster)) d_->chainOwner.insert(kMaster, kMaster);
}

void EngineBridge::addEngineTrack(const QString& trackId) {
    const Track& track = project_->track(trackId);
    const bool master = track.isMaster();
    const bool isReturn = track.isReturn();
    if (!master) {  // the engine always has the master
        const quint32 engineId = engine_.addTrack();
        d_->trackIds.insert(trackId, engineId);
        if (trackId == d_->observed) engine_.setTrackObserved(engineId, true);
        d_->chains.insert(trackId, engine_.trackChain(engineId));
        d_->chainOwner.insert(trackId, trackId);
    }
    pushMixer(trackId);
    pushInput(trackId);
    pushFrozen(trackId);
    pushClips(trackId);
    syncDevices(trackId);
    pushAutomation(trackId);
    if (!master) pushOutputs();  // into its group, and what is in it (back) into it
    if (isReturn) {
        pushAllSends();  // its sends, and the sends into it
    } else if (!master) {
        pushSends(trackId);
    }
    if (!master) pushAllInputs();  // the inputs taken from it (back)
    pushSidechains();  // its devices', and those it is the source of
}

void EngineBridge::onTrackRemoved(const QString& trackId) {
    Private& d = *d_;
    forgetChainDevices(trackId, false);
    std::optional<quint32> engineId;
    if (d.trackIds.contains(trackId)) engineId = d.trackIds.take(trackId);
    dropChain(trackId);
    if (engineId) engine_.removeTrack(*engineId);  // also removes its devices
    d.meters.remove(trackId);
    d.automating.remove(trackId);
    d.mixer.remove(trackId);
    d.inputs.remove(trackId);
    d.outputs.remove(trackId);
    d.sends.remove(trackId);
    d.sendLevels.remove(trackId);
    d.frozen.remove(trackId);
    // What went into it goes to the engine's master now, and the sends into it
    // and the inputs from it are gone (the model has its say next).
    if (engineId) {
        for (quint32& output : d.outputs) {
            if (output == *engineId) output = sub::Engine::kMaster;
        }
        for (auto& sends : d.sends) sends.erase(*engineId);
        for (Private::InputState& input : d.inputs) {
            if (input.source == engineId) input.source.reset();
        }
        for (auto it = d.sidechains.begin(); it != d.sidechains.end();) {
            it = it->second.source == *engineId ? d.sidechains.erase(it) : std::next(it);
        }
    }
    for (auto it = d.overridden.begin(); it != d.overridden.end();) {
        it = it->first == trackId ? d.overridden.erase(it) : std::next(it);
    }
}

void EngineBridge::onTrackChanged(const QString& trackId) {
    if (!d_->trackIds.contains(trackId)) return;
    const Track& track = project_->track(trackId);
    const double volumeDb = track.volumeDb;
    const double pan = track.pan;
    overrideChangedMixer(trackId, volumeDb, pan);
    overrideChangedSends(trackId);
    pushMixer(trackId);
    pushInput(trackId);
    pushSends(trackId);
    pushSidechains();  // (a route that stood in a sidechain's way may have gone)
}

// A send level changed by hand while automated: its automation stops.
void EngineBridge::overrideChangedSends(const QString& trackId) {
    QMap<QString, double> now;
    const SendMap& sends = project_->track(trackId).sends;
    for (auto it = sends.constBegin(); it != sends.constEnd(); ++it) now.insert(it.key(), it.value().levelDb);
    const auto old = d_->sendLevels.constFind(trackId);
    const std::optional<QMap<QString, double>> before =
        old != d_->sendLevels.constEnd() ? std::optional(*old) : std::nullopt;
    d_->sendLevels.insert(trackId, now);
    if (!before) return;
    for (auto it = now.constBegin(); it != now.constEnd(); ++it) {
        if (!before->contains(it.key()) || before->value(it.key()) != it.value()) {
            overrideAutomation(trackId, automation::sendKey(it.key()));
        }
    }
}

// A mixer control changed by hand while automated: its automation stops.
void EngineBridge::overrideChangedMixer(const QString& owner, double volumeDb, double pan) {
    const auto it = d_->mixer.constFind(owner);
    if (it == d_->mixer.constEnd()) return;
    const std::pair<double, double> old = *it;
    if (volumeDb != old.first) overrideAutomation(owner, automation::kMixerVolume);
    if (pan != old.second) overrideAutomation(owner, automation::kMixerPan);
}

void EngineBridge::pushMixer(const QString& trackId) {
    const auto engineId = engineTrackId(trackId);
    if (!engineId) return;
    const Track& track = project_->track(trackId);
    engine_.setTrackGain(*engineId, static_cast<float>(dbToGain(track.volumeDb)));
    engine_.setTrackPan(*engineId, static_cast<float>(track.pan));
    if (!track.isMaster()) {
        engine_.setTrackMute(*engineId, track.mute);
        engine_.setTrackSolo(*engineId, track.solo);
    }
    d_->mixer.insert(trackId, {track.volumeDb, track.pan});
}

// Every track's output into its group's engine track (or the master). Changed
// routes go to the master first, so that no step closes a cycle (a group moving
// into what was in it).
void EngineBridge::pushOutputs() {
    Private& d = *d_;
    const quint32 master = sub::Engine::kMaster;
    std::vector<std::pair<QString, quint32>> changed;  // in track order
    for (const Track& track : project_->tracks()) {
        if (!d.trackIds.contains(track.id)) continue;
        const quint32 wanted = track.parent ? d.trackIds.value(*track.parent, master) : master;
        if (d.outputs.value(track.id, master) != wanted) changed.emplace_back(track.id, wanted);
    }
    for (const auto& [trackId, output] : changed) {
        if (d.outputs.value(trackId, master) != master) {
            engine_.setTrackOutput(d.trackIds.value(trackId), master);
            d.outputs.insert(trackId, master);
        }
    }
    for (const auto& [trackId, output] : changed) {
        if (output != master) engine_.setTrackOutput(d.trackIds.value(trackId), output);
        d.outputs.insert(trackId, output);
    }
    if (!changed.empty()) pushSidechains();
}

// The engine sends a track should have: its sends, and silent ones for those
// automated without having been set (so that the automation plays).
std::vector<std::pair<quint32, std::pair<double, bool>>> EngineBridge::wantedSends(const Track& track) const {
    std::vector<std::pair<QString, Send>> sends;
    for (auto it = track.sends.constBegin(); it != track.sends.constEnd(); ++it) sends.emplace_back(it.key(), it.value());
    const auto has = [&](const QString& returnId) {
        return std::any_of(sends.begin(), sends.end(), [&](const auto& send) { return send.first == returnId; });
    };
    for (const auto& entry : track.automation) {
        const auto returnId = automation::keySend(entry.first);
        if (returnId && !has(*returnId) && project_->hasReturn(*returnId) &&
            !project_->wouldCycle(track.id, *returnId)) {
            sends.emplace_back(*returnId, Send{});
        }
    }
    std::vector<std::pair<quint32, std::pair<double, bool>>> wanted;
    for (const auto& [returnId, send] : sends) {
        if (d_->trackIds.contains(returnId) && project_->hasReturn(returnId)) {
            wanted.push_back({d_->trackIds.value(returnId), {dbToGain(send.levelDb), send.preFader}});
        }
    }
    return wanted;
}

// A track's sends to the engine: those going away first (no step closes a cycle).
void EngineBridge::pushSends(const QString& trackId) {
    const auto engineId = engineTrackId(trackId);
    if (!engineId || trackId == kMaster) return;
    const auto wanted = wantedSends(project_->track(trackId));
    std::map<quint32, std::pair<double, bool>>& current = d_->sends[trackId];
    std::vector<quint32> going;
    for (const auto& [returnId, state] : current) {
        const bool kept = std::any_of(wanted.begin(), wanted.end(), [&](const auto& w) { return w.first == returnId; });
        if (!kept) going.push_back(returnId);
    }
    for (const quint32 returnId : going) {
        engine_.removeTrackSend(*engineId, returnId);
        current.erase(returnId);
    }
    for (const auto& [returnId, state] : wanted) {
        const auto it = current.find(returnId);
        if (it != current.end() && it->second == state) continue;
        try {
            engine_.setTrackSend(*engineId, returnId, static_cast<float>(state.first), state.second);
        } catch (const std::invalid_argument&) {
            continue;  // a cycle with a send another track hasn't given up yet: it comes with that track's turn
        }
        current[returnId] = state;
    }
    if (!d_->sendLevels.contains(trackId)) {
        QMap<QString, double> levels;
        const SendMap& sends = project_->track(trackId).sends;
        for (auto it = sends.constBegin(); it != sends.constEnd(); ++it) levels.insert(it.key(), it.value().levelDb);
        d_->sendLevels.insert(trackId, levels);
    }
}

void EngineBridge::pushAllSends() {
    QStringList senders;
    for (const Track* track : project_->senders()) senders.append(track->id);
    for (const QString& trackId : senders) pushSends(trackId);
}

// A track's clips (a MIDI track's notes) to the engine; a frozen track's frozen
// audio instead (what of it plays: its segments).
void EngineBridge::pushClips(const QString& trackId) {
    const auto engineId = engineTrackId(trackId);
    if (!engineId || trackId == kMaster) return;
    const Track& track = project_->track(trackId);
    if (track.frozen) {
        const std::vector<Clip> segments = track.frozen->playing(trackId);
        const bool midi = track.isMidi();
        requestSource(track.frozen->path);
        if (midi) engine_.setTrackNotes(*engineId, {});
        std::vector<sub::ClipDesc> clips;
        for (const Clip& segment : segments) clips.push_back(clipDesc(segment));
        engine_.setTrackClips(*engineId, clips);
        return;
    }
    if (track.isMidi()) {
        engine_.setTrackNotes(*engineId, noteDescs(track));
        return;
    }
    std::vector<sub::ClipDesc> clips;
    QStringList paths;
    for (const Clip& clip : track.clips) {
        paths.append(clip.path);
        clips.push_back(clipDesc(clip));
    }
    for (const QString& path : paths) requestSource(path);
    engine_.setTrackClips(*engineId, clips);
}

void EngineBridge::previewClips(const QMap<QString, std::vector<Clip>>& clipsByTrack,
                                const QMap<QString, std::vector<Clip>>& frozenByTrack) {
    for (auto it = clipsByTrack.constBegin(); it != clipsByTrack.constEnd(); ++it) {
        const QString& trackId = it.key();
        const auto engineId = engineTrackId(trackId);
        if (!engineId || !project_->hasTrack(trackId) || project_->isFrozen(trackId)) continue;
        d_->previewing.insert(trackId);
        if (project_->track(trackId).isMidi()) {
            engine_.setTrackNotes(*engineId, clipNoteDescs(it.value()));
        } else {
            std::vector<sub::ClipDesc> clips;
            for (const Clip& clip : it.value()) clips.push_back(clipDesc(clip));
            engine_.setTrackClips(*engineId, clips);
        }
    }
    // Frozen tracks play these segments of their frozen audio.
    for (auto it = frozenByTrack.constBegin(); it != frozenByTrack.constEnd(); ++it) {
        const QString& trackId = it.key();
        const auto engineId = engineTrackId(trackId);
        const Track* track = project_->hasTrack(trackId) ? project_->findTrack(trackId) : nullptr;
        if (!engineId || track == nullptr || !track->frozen) continue;
        d_->previewing.insert(trackId);
        std::vector<sub::ClipDesc> clips;
        for (const Clip& clip : it.value()) clips.push_back(clipDesc(clip));
        engine_.setTrackClips(*engineId, clips);
    }
    // Tracks previewed before but not now play their own clips again.
    const QSet<QString> previewing = d_->previewing;
    for (const QString& trackId : previewing) {
        if (clipsByTrack.contains(trackId) || frozenByTrack.contains(trackId)) continue;
        d_->previewing.remove(trackId);
        if (project_->hasTrack(trackId)) pushClips(trackId);
    }
}

void EngineBridge::endClipPreview() {
    const QSet<QString> previewed = std::exchange(d_->previewing, {});
    for (const QString& trackId : previewed) {
        if (project_->hasTrack(trackId)) pushClips(trackId);
    }
}

void EngineBridge::pushSettings() {
    engine_.setTempo(project_->tempo());
    engine_.setTimeSignature(project_->timeSignature().numerator, project_->timeSignature().denominator);
    engine_.setLoop(project_->loopEnabled(), project_->loopStart(), project_->loopEnd());
}

void EngineBridge::applyAudioThreads() {
    const int chosen = sub::app::audioThreads();
    engine_.setAudioThreads(chosen > 0 ? chosen : sub::Engine::defaultAudioThreads());
}

void EngineBridge::applyBackgroundFreezing() {
    sub::BackgroundFreezingSettings settings = engine_.backgroundFreezing();
    settings.enabled = sub::app::backgroundFreezing();
    engine_.setBackgroundFreezing(settings);
}

void EngineBridge::setObservedTrack(const QString& trackId) {
    if (trackId == d_->observed) return;
    guarded("observed track", [&] {
        if (const auto old = engineTrackId(d_->observed)) engine_.setTrackObserved(*old, false);
        d_->observed = trackId;
        if (const auto now = engineTrackId(trackId)) engine_.setTrackObserved(*now, true);
    });
}

void EngineBridge::chooseAudioThreads(int threads) {
    sub::app::setAudioThreads(threads == sub::Engine::defaultAudioThreads() ? 0 : threads);
    guarded("audio threads", [&] { applyAudioThreads(); });
}

void EngineBridge::chooseBackgroundFreezing(bool enabled) {
    sub::app::setBackgroundFreezingSetting(enabled);
    guarded("background freezing", [&] { applyBackgroundFreezing(); });
}

int EngineBridge::audioThreads() const { return engine_.audioThreads(); }

int EngineBridge::defaultAudioThreads() { return sub::Engine::defaultAudioThreads(); }

}  // namespace sub::app
