#pragma once
// What the engine bridge keeps (EngineBridge::Private): the maps from the
// model's ids to the engine's, and the last value it gave the engine of
// everything it only pushes when it changes. The application layer's own
// header (it includes the engine's): the bridge's .cpp files and its tests.

#include "audio/EngineBridge.h"

#include "Engine.h"

#include <QByteArray>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace sub::app {

// The sidechain the engine gives a processor: (source engine track, tap, the
// source's processor it is taken after).
struct EngineBridge::SidechainState {
    quint32 source = 0;
    sub::SidechainTap tap = sub::SidechainTap::PostFader;
    quint32 tapProcessor = 0;

    friend bool operator==(const SidechainState&, const SidechainState&) = default;
};

// Where a track's output goes as the engine has it: an engine track (or
// sub::Engine::kMaster, or sub::Engine::kNoOutput), or a device's sidechain
// (`processor`, when not 0).
struct EngineBridge::OutputState {
    quint32 track = sub::Engine::kMaster;
    quint32 processor = 0;

    friend bool operator==(const OutputState&, const OutputState&) = default;
};

struct EngineBridge::Private {
    // A track's input as the engine has it: (device channels, the engine track
    // whose output it takes and where it is tapped, monitoring, armed, MIDI input).
    struct InputState {
        std::vector<int> channels;
        std::optional<quint32> source;
        sub::SidechainTap tap = sub::SidechainTap::PostFader;
        quint32 tapProcessor = 0;
        QString monitor;
        bool armed = false;
        std::optional<MidiInput> midi;

        friend bool operator==(const InputState&, const InputState&) = default;
    };
    // A rack chain's fader as the engine has it.
    struct ChainMixer {
        double volumeDb = 0.0;
        double pan = 0.0;
        bool mute = false;
        bool solo = false;

        friend bool operator==(const ChainMixer&, const ChainMixer&) = default;
    };

    // Model track id -> engine track id (kMaster -> sub::Engine::kMaster).
    QMap<QString, quint32> trackIds;
    // Chain key (a track's id for its own chain, a rack chain's id) -> its engine
    // chain. (Ids are unique in the project.) Each device's processor is in the
    // chain the bridge last put it in (`where`).
    QMap<QString, quint32> chains;
    QMap<QString, QString> chainOwner;  // chain key -> the track it is on
    QMap<QString, QString> rackOfChain;  // rack chain id -> its rack's device id
    QMap<QString, std::vector<quint32>> rackOrders;  // rack device id -> its engine chains, in the engine's order
    // Chain key -> [(model device id, processor)]; no processor: a plug-in that
    // didn't load (or waits to).
    QMap<QString, std::vector<ChainEntry>> devices;
    QMap<QString, std::optional<quint32>> pids;  // device id -> its processor (none: not loaded)
    QMap<QString, QString> where;  // device id -> the chain key its processor is in
    QMap<QString, ChainMixer> chainMixer;  // rack chain id -> its fader as the engine has it
    QSet<QString> syncing;  // tracks whose devices are being synced (not again from inside)
    QHash<quint32, bool> enabled;  // processor -> what the engine was told
    QMap<QString, bool> ownEnabled;  // device id -> whether the model had it on, as last pushed
    QHash<quint32, QString> pluginIds;  // processor of each plug-in -> the path it came from
    QMap<QString, QByteArray> pluginStates;  // device id -> its plug-in's state when it went away
    QHash<quint32, QStringList> paramIds;  // processor -> parameter ids by index (cache)
    QHash<quint32, std::vector<sub::ParamInfo>> paramInfos;  // processor -> its ParamInfos (cache)
    QHash<quint32, std::vector<ParamSpec>> paramSpecs;  // processor -> its automatable parameters (cache)
    QMap<QString, QSet<QString>> automating;  // owner -> the targets whose envelopes the engine plays
    // Owner -> {target key: the envelope its macro's automation plays it along}, as pushed.
    QMap<QString, QMap<QString, Envelope>> macroMoved;
    std::set<std::pair<QString, QString>> overridden;  // (owner, key) changed by hand while automated
    QMap<QString, std::pair<double, double>> mixer;  // owner -> (volume dB, pan) the engine has
    QMap<QString, bool> mutes;  // track id -> its mute as the engine has it
    QMap<QString, InputState> inputs;  // track id -> its input as the engine has it
    QMap<QString, OutputState> outputs;  // track id -> where its output goes
    QMap<QString, bool> trackIn;  // track id -> whether what goes into it is its input (Track In), as the engine has it
    QMap<QString, std::map<quint32, std::pair<double, bool>>> sends;  // track id -> {engine return: (gain, pre-fader)}
    QSet<QString> frozen;  // tracks the engine has frozen
    QMap<QString, QMap<QString, double>> sendLevels;  // track id -> {return id: level dB} the engine has
    std::map<quint32, SidechainState> sidechains;  // processor -> the sidechain the engine has
    int busy = 0;  // > 0 while a plug-in (or driver) call may run a message loop that calls us back
    QMap<QString, QString> pluginErrors;  // device id -> why its plug-in isn't loaded
    QMap<QString, QString> knownPlugins;  // plug-in uid -> file, from the scan: finds moved plug-ins
    std::function<uintptr_t()> ownerWindow = [] { return uintptr_t{0}; };  // the window owning plug-in editors
    QSet<QString> editorsWanted;  // device ids whose editor the user left open
    std::optional<QString> editorsTrack;  // the track whose editors are shown (the selected one)
    std::vector<quint32> hiddenEditors;  // processors of hidden editors, the longest hidden first

    QHash<QString, std::shared_ptr<sub::AudioSource>> sources;  // source key -> decoded file
    QHash<QString, std::vector<std::function<void()>>> loading;  // source key -> what waits for it
    QHash<QString, QString> failed;  // source key -> why it couldn't be decoded
    QHash<QString, AudioFileInfo> fileInfo;  // source key -> its header's
    int previewRequest = 0;  // the latest preview asked for (or stopped): a file still loading then is not played

    QMap<QString, MeterLevel> meters;  // track id or kMaster -> its meter
    QMap<QString, MeterLevel> chainMeters;  // rack chain id -> its meter
    double lastPosition = -1.0;
    bool lastPlaying = false;
    bool lastCountingIn = false;

    QMap<quint32, QString> recording;  // engine track id -> track id, while recording
    QMap<QString, LiveTake> liveTakes;  // track id -> its take while recording
    QSet<QString> previewing;  // tracks playing a dragged clip's preview (previewClips)
    QHash<QString, QString> reversed;  // source key -> its reversed copy, written this session
    QMap<QString, QString> midiErrors;  // MIDI input -> why it couldn't be opened

    bool deferring = false;  // plug-ins added now wait to load (a project opening)
    QStringList pendingPlugins;  // device ids whose plug-ins wait to load, in the order they load
    int pluginsTotal = 0;  // of those, and those loaded since the project opened
    QTimer pluginTimer;

    QThreadPool pool;  // decoding files
    // Built-in devices' states are restored one at a time, in the order they
    // were set, so the last one set wins.
    QThreadPool statePool;
    QTimer positionTimer;
    QTimer meterTimer;

    // For tests: reports to take as if the plug-ins had sent them (BridgeTestAccess).
    std::vector<sub::ProcessorEventRecord> injectedEvents;
};

// What a slot reacting to the project does, or (should something throw) says
// so: Qt must never see an exception, and the model goes on.
void guarded(const char* what, const std::function<void()>& body);

// Private::busy counted up while it lives: around a plug-in's or a driver's
// call that may run a message loop calling the bridge back. It counts down
// however the call ends, whatever it throws.
class BusyScope {
public:
    explicit BusyScope(int& busy) : busy_(busy) { ++busy_; }
    ~BusyScope() { --busy_; }
    BusyScope(const BusyScope&) = delete;
    BusyScope& operator=(const BusyScope&) = delete;

private:
    int& busy_;
};

// A processor's state as the engine has it (bytes), and as the application keeps it.
inline std::vector<uint8_t> stateBytes(const QByteArray& state) { return {state.begin(), state.end()}; }
inline QByteArray stateData(const std::vector<uint8_t>& bytes) {
    return QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
}

// Names as the engine lists them (drivers, channels, MIDI inputs).
inline QStringList stringList(const std::vector<std::string>& names) {
    QStringList list;
    for (const std::string& name : names) list.append(QString::fromStdString(name));
    return list;
}

}  // namespace sub::app
