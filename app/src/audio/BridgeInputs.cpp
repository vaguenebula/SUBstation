// Tracks' inputs: what each track hears and records (audio channels, another
// track's output, a MIDI input), its monitoring, the audio inputs the device
// opens with, and the MIDI inputs (the computer keyboard's too).

#include "audio/AudioSettings.h"
#include "audio/BridgePrivate.h"

#include "model/Project.h"

#include <algorithm>
#include <set>

namespace sub::app {

namespace {

sub::MonitorMode monitorMode(const QString& mode) {
    if (mode == u"off") return sub::MonitorMode::Off;
    if (mode == u"in") return sub::MonitorMode::In;
    return sub::MonitorMode::Auto;
}

}  // namespace

// The engine track whose output a track takes as its input (sub::Engine::kMaster:
// the master's); none: none, or one the engine hasn't (yet).
std::optional<quint32> EngineBridge::inputSource(const Track& track) const {
    if (!track.inputTrack || !track.isAudio()) return std::nullopt;
    if (*track.inputTrack == kMaster) return sub::Engine::kMaster;
    return engineTrackId(*track.inputTrack);
}

void EngineBridge::pushInput(const QString& trackId) {
    const auto engineId = engineTrackId(trackId);
    if (!engineId || trackId == kMaster) return;
    const Track& track = project_->track(trackId);
    Private::InputState state;
    if (!track.isMidi()) state.channels = track.input;
    state.source = inputSource(track);
    if (state.source && *state.source != sub::Engine::kMaster) {
        std::tie(state.tap, state.tapProcessor) = engineTap(*track.inputTrack, track.inputTap);
    }
    state.monitor = track.monitor;
    state.armed = track.armed;
    if (track.isMidi()) state.midi = track.midiInput;
    const auto found = d_->inputs.constFind(trackId);
    const std::optional<Private::InputState> old =
        found != d_->inputs.constEnd() ? std::optional(*found) : std::nullopt;
    if (old && *old == state) return;  // (a mixer change)
    if (!old || old->channels != state.channels || old->source != state.source || old->tap != state.tap ||
        old->tapProcessor != state.tapProcessor) {
        if (!state.source) {
            engine_.setTrackInput(*engineId, state.channels);
        } else {
            try {
                engine_.setTrackInputTrack(*engineId, *state.source, state.tap, state.tapProcessor);
            } catch (const std::invalid_argument&) {
                // A cycle with a route another change hasn't undone yet: it comes with that change.
                engine_.setTrackInput(*engineId, {});
                state.channels.clear();
                state.source.reset();
                state.tap = sub::SidechainTap::PostFader;
                state.tapProcessor = 0;
            }
        }
    }
    d_->inputs.insert(trackId, state);
    if (!old || old->monitor != state.monitor) engine_.setTrackMonitor(*engineId, monitorMode(state.monitor));
    if (!old || old->armed != state.armed) engine_.setTrackArmed(*engineId, state.armed);
    if (!old || old->midi != state.midi) {
        if (!state.midi) {
            engine_.setTrackMidiInput(*engineId, false, {}, 0);
        } else {
            engine_.setTrackMidiInput(*engineId, true, state.midi->device.toStdString(), state.midi->channel);
        }
    }
    if (!state.channels.empty() && !isRecording()) openInputs(state.channels);
}

void EngineBridge::pushAllInputs() {
    QStringList ids;
    for (const Track& track : project_->tracks()) ids.append(track.id);
    for (const QString& trackId : ids) pushInput(trackId);
}

// An ASIO device that hasn't these inputs open opens again with them too.
void EngineBridge::openInputs(const std::vector<int>& channels) {
    const sub::DeviceStatus status = engine_.deviceStatus();
    const std::set<int> open(status.inputChannels.begin(), status.inputChannels.end());
    const bool all = std::all_of(channels.begin(), channels.end(), [&](int c) { return open.count(c) > 0; });
    if (!status.open || status.backend != "ASIO" || all) return;
    const auto names = engine_.deviceCapabilities().inputNames;
    if (std::any_of(channels.begin(), channels.end(), [&](int c) { return c >= static_cast<int>(names.size()); })) {
        return;  // not this device's: silent until a device that has them
    }
    // As it runs now (not as saved: it may have opened with its own settings instead).
    AudioSettings settings;
    settings.driver = QStringLiteral("ASIO");
    settings.deviceName = QString::fromStdString(status.name);
    settings.sampleRate = static_cast<int>(status.sampleRate);
    settings.bufferFrames = static_cast<int>(status.bufferFrames);
    settings.outputChannels = status.outputChannels;
    std::set<int> inputs = open;
    inputs.insert(channels.begin(), channels.end());
    settings.inputChannels.assign(inputs.begin(), inputs.end());
    const QString error = openDevice(settings);
    if (error.isEmpty()) {
        settings.save();
    } else {
        Q_EMIT statusMessage(QStringLiteral("The input could not be opened: ") + error);
    }
}

QStringList EngineBridge::inputNames() const {
    if (!engine_.deviceStatus().open) return {};
    return stringList(engine_.deviceCapabilities().inputNames);
}

QStringList EngineBridge::midiInputs() const { return stringList(engine_.midiInputDevices()); }

QStringList EngineBridge::midiInputChoices() const { return midiInputs() << kComputerKeyboard; }

void EngineBridge::sendMidi(const QList<int>& message, const QString& device) {
    if (message.isEmpty() || message.size() > 3) return;  // not a MIDI message
    std::vector<uint8_t> bytes;
    for (const int byte : message) {
        if (byte < 0 || byte > 255) return;  // not a MIDI message
        bytes.push_back(static_cast<uint8_t>(byte));
    }
    guarded("MIDI input", [&] { engine_.sendMidiInput(device.toStdString(), bytes); });
}

bool EngineBridge::isMidiInputOpen(const QString& name) const {
    const auto open = engine_.openMidiInputs();
    return std::find(open.begin(), open.end(), name.toStdString()) != open.end();
}

void EngineBridge::openMidiInputs() {
    const QSet<QString> disabled = disabledMidiInputs();
    const QStringList connected = midiInputs();
    for (const std::string& open : engine_.openMidiInputs()) {
        const QString name = QString::fromStdString(open);
        if (disabled.contains(name) || !connected.contains(name)) engine_.closeMidiInput(open);
    }
    QMap<QString, QString> failed;
    for (const QString& name : connected) {
        if (disabled.contains(name) || isMidiInputOpen(name)) continue;
        try {
            engine_.openMidiInput(name.toStdString());
        } catch (const std::exception& error) {
            const QString message = QString::fromStdString(error.what());
            failed.insert(name, message);
            if (!d_->midiErrors.contains(name)) Q_EMIT statusMessage(message);
        }
    }
    d_->midiErrors = failed;
}

void EngineBridge::setMidiInputEnabled(const QString& name, bool enabled) {
    setMidiInputDisabled(name, !enabled);
    openMidiInputs();
}

const QMap<QString, QString>& EngineBridge::midiErrors() const { return d_->midiErrors; }

}  // namespace sub::app
