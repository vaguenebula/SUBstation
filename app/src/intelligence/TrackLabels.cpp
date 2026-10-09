#include "intelligence/TrackLabels.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>
#include <QVariantMap>

#include "audio/EngineBridge.h"
#include "labels/Effects.h"
#include "labels/TrackLabels.h"
#include "model/DeviceState.h"
#include "model/Devices.h"
#include "model/Project.h"
#include "model/TrackNames.h"
#include "plugins/PluginIndex.h"
#include "plugins/PresetName.h"  // the engine's: a preset's name in a plug-in's saved state

namespace sub::app {

namespace labels = intelligence::labels;

namespace {

std::string utf8(const QString& text) { return text.toStdString(); }

QStringList strings(const std::vector<std::string>& list) {
    QStringList out;
    for (const std::string& text : list) out.append(QString::fromStdString(text));
    return out;
}

}  // namespace

TrackLabels::TrackLabels(Project* project, EngineBridge* bridge, PluginIndex* plugins, QObject* parent)
    : QObject(parent), project_(project), bridge_(bridge), plugins_(plugins) {
    settle_.setSingleShot(true);
    settle_.setInterval(kSettleMs);
    connect(&settle_, &QTimer::timeout, this, [this] {
        ++revision_;
        Q_EMIT changed();
    });
    const auto stale = [this] { invalidate(); };
    // What the project holds: tracks, their clips, devices, mixers, sends; the tempo (clips' lengths in beats).
    connect(project, &Project::trackInserted, this, stale);
    connect(project, &Project::trackRemoved, this, stale);
    connect(project, &Project::returnInserted, this, stale);
    connect(project, &Project::returnRemoved, this, stale);
    connect(project, &Project::trackChanged, this, stale);
    connect(project, &Project::tracksArranged, this, stale);
    connect(project, &Project::clipsChanged, this, stale);
    connect(project, &Project::devicesChanged, this, stale);
    connect(project, &Project::chainChanged, this, stale);
    connect(project, &Project::deviceParamChanged, this, stale);
    connect(project, &Project::deviceStateChanged, this, stale);
    connect(project, &Project::freezeChanged, this, stale);
    connect(project, &Project::settingsChanged, this, stale);
    connect(project, &Project::reset, this, stale);
    if (bridge) {
        // What plug-ins say: their presets, their parameters edited in their editors. (Not
        // pluginParamsChanged: automation playing says it at every poll; labels read the
        // values as they are whenever they are made.)
        connect(bridge, &EngineBridge::pluginPresetChanged, this, stale);
        connect(bridge, &EngineBridge::pluginParamEdited, this, stale);
        connect(bridge, &EngineBridge::pluginParamsRebuilt, this, stale);
        connect(bridge, &EngineBridge::devicesLoaded, this, stale);
    }
    if (plugins) {
        const auto takeCategories = [this] {
            categories_.clear();
            for (const PluginInfo& plugin : plugins_->plugins()) categories_.insert(plugin.uid, plugin.category);
            invalidate();
        };
        connect(plugins, &PluginIndex::updated, this, takeCategories);
        takeCategories();
    }
}

void TrackLabels::invalidate() {
    stale_ = true;
    settle_.start();  // (again: once the last change of a burst has settled)
}

void TrackLabels::refresh() const {
    if (!stale_ || !project_) return;
    const std::vector<labels::TrackFacts> facts = this->facts();
    const std::vector<labels::TrackLabel> made = labels::labelTracks(facts);
    labels_.clear();
    for (size_t i = 0; i < facts.size(); ++i) labels_.insert(QString::fromStdString(facts[i].id), made[i]);
    stale_ = false;
}

const labels::TrackLabel* TrackLabels::find(const QString& trackId) const {
    refresh();
    const auto it = labels_.constFind(trackId);
    return it == labels_.constEnd() ? nullptr : &it.value();
}

QString TrackLabels::label(const QString& trackId) const {
    const labels::TrackLabel* found = find(trackId);
    return found ? QString::fromStdString(found->label) : QString();
}

QString TrackLabels::toolTip(const QString& trackId) const {
    const labels::TrackLabel* found = find(trackId);
    if (!found || found->label.empty()) return {};
    QStringList lines{QString::fromStdString(found->label)};
    lines.append(strings(found->details));
    return lines.join(u'\n');
}

QVariantList TrackLabels::describe() const {
    QVariantList out;
    if (!project_) return out;
    std::vector<const Track*> tracks;
    for (const Track& track : project_->tracks()) tracks.push_back(&track);
    for (const Track& track : project_->returns()) tracks.push_back(&track);
    tracks.push_back(&project_->master());
    for (const Track* track : tracks) {
        const labels::TrackLabel* found = find(track->id);
        if (!found) continue;
        out.append(QVariantMap{
            {QStringLiteral("id"), track->id},
            {QStringLiteral("name"), track->name},
            {QStringLiteral("kind"), track->kind},
            {QStringLiteral("parent"), track->parent.value_or(QString())},
            {QStringLiteral("label"), QString::fromStdString(found->label)},
            {QStringLiteral("family"), QString::fromStdString(found->family)},
            {QStringLiteral("role"), QString::fromStdString(found->role)},
            {QStringLiteral("traits"), strings(found->traits)},
            {QStringLiteral("details"), strings(found->details)},
        });
    }
    return out;
}

bool TrackLabels::namedByUser(const Track& track) {
    if (track.isMaster() || track.isReturn() || namedByContents(track) || hasPlainName(track)) return false;
    // What it is called without its number ("# Kick", "3 Kick") and with or
    // without the numbers copies get (" 2"): a name SUBstation gave is one of them.
    static const QRegularExpression number(QStringLiteral("^(?:#|\\d+)\\s+"));
    static const QRegularExpression copy(QStringLiteral("\\s+\\d+$"));
    QStringList names{track.nameSource().trimmed()};
    names[0].remove(number);
    for (int i = 0; i < 2; ++i) {
        QString shorter = names.last();
        shorter.remove(copy);
        if (shorter == names.last() || shorter.isEmpty()) break;
        names.append(shorter);
    }
    const auto gave = [&](const QString& given) {
        if (given.isEmpty()) return false;
        for (const QString& name : names) {
            if (name.compare(given, Qt::CaseInsensitive) == 0) return true;
        }
        return false;
    };
    if (names.first().isEmpty()) return false;
    if (track.isGroup()) return !gave(QStringLiteral("Group"));
    if (gave(QStringLiteral("Audio")) || gave(QStringLiteral("MIDI")) || gave(contentsLabel(track))) return false;
    for (const Clip& clip : track.clips) {
        if (!clip.isAudio()) continue;
        if (gave(QFileInfo(clip.path).completeBaseName()) || gave(QFileInfo(clip.reversedFrom).completeBaseName())) {
            return false;
        }
    }
    for (const Device* device : iterDevices(track.devices)) {
        if (gave(deviceName(*device)) || gave(kindName(*device))) return false;
    }
    return true;
}

TrackLabels::SavedState TrackLabels::saved(const Device& device) const {
    if (!device.state || device.state->isEmpty()) return {};
    const auto it = saved_.constFind(device.id);
    if (it != saved_.constEnd() && it->state.constData() == device.state->constData() &&
        it->state.size() == device.state->size()) {
        return *it;
    }
    const QByteArray bytes = QByteArray::fromBase64(device.state->toLatin1());
    SavedState state{*device.state, {}, {}};
    state.preset = QString::fromStdString(
        sub::vst3::presetNameFromPreset(reinterpret_cast<const uint8_t*>(bytes.constData()), static_cast<size_t>(bytes.size())));
    state.params = labels::paramsInState(std::string_view(bytes.constData(), static_cast<size_t>(bytes.size())));
    saved_.insert(device.id, state);
    return state;
}

labels::DeviceFact TrackLabels::deviceFacts(const QString& trackId, const Device& device) const {
    labels::DeviceFact facts;
    facts.kind = utf8(device.kind);
    facts.enabled = device.enabled;
    if (device.isPlugin() && device.plugin) {
        const PluginRef& plugin = *device.plugin;
        facts.name = utf8(plugin.name);
        facts.vendor = utf8(plugin.vendor);
        facts.category = utf8(categories_.value(plugin.uid));
        facts.instrument = plugin.instrument;
        // Its preset as it says now, else as its saved state says.
        const QString live = bridge_ ? bridge_->pluginPresetName(trackId, device.id) : QString();
        facts.preset = utf8(live.isEmpty() ? saved(device).preset : live);
        // An effect's amounts (its mix, its drive...): only those the labeller reads,
        // as it has them while loaded, else as its saved state has them.
        if (!plugin.instrument && bridge_) {
            const QList<ProcessorParam> params = bridge_->deviceParams(trackId, device.id);
            for (int i = 0; i < params.size(); ++i) {
                const ProcessorParam& param = params[i];
                if (param.hidden || !labels::readsParam(utf8(param.name))) continue;
                const auto value = bridge_->deviceParamValue(trackId, device.id, i);
                if (!value) continue;
                facts.params.push_back({utf8(param.id), utf8(param.name), *value, param.toNormalized(*value),
                                        utf8(param.unit), utf8(bridge_->deviceParamText(trackId, device.id, i, *value))});
            }
        }
        if (!plugin.instrument && facts.params.empty()) facts.params = saved(device).params;
    } else if (device.isRack()) {
        facts.name = utf8(device.name.value_or(QString()));
        for (const Chain& chain : device.chains) {
            std::vector<labels::DeviceFact> devices;
            for (const Device& inner : chain.devices) devices.push_back(deviceFacts(trackId, inner));
            facts.chains.push_back(std::move(devices));
        }
    } else {
        const BuiltinDevice* builtin = builtinDevice(device.kind);
        facts.name = utf8(builtin ? builtin->name : device.kind);
        facts.instrument = builtin && builtin->instrument;
        for (auto it = device.params.constBegin(); it != device.params.constEnd(); ++it) {
            const sub::ParamInfo* info = builtinParamInfo(device.kind, it.key());
            labels::ParamFact param;
            param.id = utf8(it.key());
            param.value = it.value();
            if (info) {
                param.name = info->name;
                param.unit = info->unit;
                param.normalized = info->toNormalized(static_cast<float>(it.value()));
            }
            facts.params.push_back(std::move(param));
        }
        if (device.kind == QStringLiteral("sampler")) {
            facts.sample = utf8(deviceState::fromModel(device.state).value(QStringLiteral("sample")));
        }
    }
    return facts;
}

std::vector<labels::TrackFacts> TrackLabels::facts() const {
    std::vector<labels::TrackFacts> out;
    if (!project_) return out;
    const double tempo = project_->tempo();
    const auto add = [&](const Track& track) {
        labels::TrackFacts facts;
        facts.id = utf8(track.id);
        facts.kind = utf8(track.kind);
        facts.name = utf8(track.name);
        facts.namedByUser = namedByUser(track);
        facts.parent = utf8(track.parent.value_or(QString()));
        facts.volumeDb = track.volumeDb;
        facts.pan = track.pan;
        facts.mute = track.mute;
        facts.frozen = track.frozen.has_value();
        for (const Device& device : track.devices) facts.devices.push_back(deviceFacts(track.id, device));
        for (const Clip& clip : track.clips) {
            if (clip.isAudio() && clip.plays()) {
                facts.audio.push_back({utf8(clip.reversedFrom.isEmpty() ? clip.path : clip.reversedFrom),
                                       clip.lengthBeats(tempo)});
            }
            for (const PlayedNote& played : clip.heardNotes()) {
                facts.notes.push_back({played.note.pitch, played.start, played.end, played.note.velocity});
            }
        }
        for (auto it = track.sends.constBegin(); it != track.sends.constEnd(); ++it) {
            facts.sends.push_back({utf8(it.key()), it.value().levelDb});
        }
        out.push_back(std::move(facts));
    };
    for (const Track& track : project_->tracks()) add(track);
    for (const Track& track : project_->returns()) add(track);
    add(project_->master());
    return out;
}

}  // namespace sub::app
