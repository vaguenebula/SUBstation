#pragma once
// Devices on a track's chain, racks and their chains, and the helpers that
// walk the device tree (devices in racks, in chains of racks, ...).
//
// A device may be a rack (kind "rack": a device group). Its chains each process
// its input, side by side, through their devices (racks too, at most
// kMaxRackDepth deep) and a mixer of their own (volume, pan, mute, solo among
// the rack's chains), and it puts out their sum; its chains' devices all hear
// the track's notes, so a rack of instruments layers them. Devices nested in
// racks are the track's devices as much as those in its own chain: device ids
// are unique in the project, so Project::device() finds them wherever they sit,
// and their automation is the track's (by device id). A rack's macros are its
// parameters ("macro1"..): each can be mapped to parameters of devices in it
// (MacroMapping), which follow it across their range (all of it, part of it, or
// the other way round). A rack has 1 to kMaxMacroCount of them (a new one
// kDefaultMacroCount), each with a name of its own if given one (macroNames).
//
// The helpers hand out pointers into the vectors they walk: they stay valid
// until a vector they point into changes.

#include <QMap>
#include <QSet>
#include <QString>

#include <optional>
#include <vector>

namespace sub::app {

// Which plug-in a device is: enough to load it, to find it again if it moved,
// and to name it if it is missing.
struct PluginRef {
    QString format = QStringLiteral("VST3");
    QString uid;  // VST3 class id
    QString name;
    QString vendor;
    QString path;  // where it was when last loaded
    bool instrument = false;

    friend bool operator==(const PluginRef&, const PluginRef&) = default;
};

inline const QString kPluginKind = QStringLiteral("plugin");
inline const QString kRackKind = QStringLiteral("rack");  // a device group: chains side by side
inline constexpr int kMaxRackDepth = 8;  // racks nest at most this deep (the engine's MAX_RACK_DEPTH)
inline constexpr int kDefaultMacroCount = 4;  // a new rack's macros
inline constexpr int kMaxMacroCount = 16;     // a rack has 1 to this many: its parameters macroParam(0..15)

// A rack's macro's parameter id: "macro1" for the first.
QString macroParam(int index);
// Which macro a rack's parameter is ("macro3": 2); none: no macro's.
std::optional<int> macroIndex(const QString& paramId);

// Where a sidechain takes its source's signal (Sidechain::tap), unless after
// one of its devices: as Ableton's Post Mixer, Post FX and Pre FX.
inline const QString kPostFader = QStringLiteral("post");
inline const QString kPreFader = QStringLiteral("pre");
inline const QString kPreFx = QStringLiteral("pre-fx");

// What a device's sidechain (aux) input hears: a track's signal (a group's, a
// return's: `trackId`), after its fader and pan (kPostFader), before it
// (kPreFader, after all its devices), before all its devices (kPreFx: what they
// hear; on a MIDI track, after its instrument), or after one of its devices
// (`tap`: that device's id; one in a rack's chain too, before that chain's
// fader; before the fader while that device isn't on the track). It isn't heard
// on its own, so the source's mute and solo silence it only after the fader.
struct Sidechain {
    QString trackId;
    QString tap = kPostFader;

    // The device it is taken after (none: after the fader, before it, or before the devices).
    std::optional<QString> tapDevice() const;

    friend bool operator==(const Sidechain&, const Sidechain&) = default;
};

// A rack's macro (0-based) moving a parameter of a device in the rack: the
// macro's 0..1 is the parameter's `low`..`high` (normalized, as automation is;
// `low` > `high` turns it the other way; 0.5..1 moves it over its upper half).
struct MacroMapping {
    int macro = 0;
    QString deviceId;
    QString paramId;
    double low = 0.0;
    double high = 1.0;

    // The parameter's normalized value for the macro at `value` (0..1).
    double target(double value) const;

    friend bool operator==(const MacroMapping&, const MacroMapping&) = default;
};

struct Chain;

// An insert device on a track: a built-in one ("synth", "utility"), a plug-in
// (kind "plugin", with `plugin` saying which), or a rack (kind "rack": its
// `chains`, its macros' `macros` mappings, and `macroNames`, one per macro).
//
// A built-in device's state is its `params`, and `state` for what isn't a
// parameter (a sampler's sample: see DeviceState.h), base64; the engine follows
// the model. A plug-in keeps its own state; `state` holds it (base64) as last
// saved, for loading the project. Its `params` only record values changed from
// the host, for undo. A rack's params are its macros' values (macroParam()). A
// device with a sidechain (aux) input may hear a track there (`sidechain`).
struct Device {
    QString id;
    QString kind;
    bool enabled = true;
    QMap<QString, double> params;
    std::optional<PluginRef> plugin;
    std::optional<QString> state;
    std::optional<Sidechain> sidechain;
    std::vector<Chain> chains;  // a rack's
    std::vector<MacroMapping> macros;  // a rack's
    // A rack's macros, one each: its name as the user gave it ("": "Macro N"; macroName()).
    std::vector<QString> macroNames;
    // A rack's own name (the preset it was saved as or loaded from); none: by its kind.
    std::optional<QString> name;

    bool isPlugin() const { return kind == kPluginKind; }
    bool isRack() const { return kind == kRackKind; }

    bool operator==(const Device& other) const;
    bool operator!=(const Device& other) const { return !(*this == other); }
};

// A chain of a rack: its devices, and its mixer (volume, pan, mute, and solo:
// while any of a rack's chains is soloed, only those are heard).
struct Chain {
    QString id;
    QString name;
    std::vector<Device> devices;
    double volumeDb = 0.0;
    double pan = 0.0;
    bool mute = false;
    bool solo = false;

    bool operator==(const Chain& other) const;
    bool operator!=(const Chain& other) const { return !(*this == other); }
};

// How many macros a rack has (0: not a rack).
int macroCount(const Device& rack);
// A rack's macro's name: the one it was given, else "Macro N".
QString macroName(const Device& rack, int index);

// A rack and one of its chains.
template <typename DeviceT, typename ChainT>
struct RackChainOf {
    DeviceT* rack;
    ChainT* chain;
};
using RackChain = RackChainOf<Device, Chain>;
using ConstRackChain = RackChainOf<const Device, const Chain>;

// Every device in a chain, depth first: each, then (a rack) those in its chains.
std::vector<Device*> iterDevices(std::vector<Device>& devices);
std::vector<const Device*> iterDevices(const std::vector<Device>& devices);
// Every rack chain in a chain (and in racks in it), with its rack, depth first.
std::vector<RackChain> iterChains(std::vector<Device>& devices);
std::vector<ConstRackChain> iterChains(const std::vector<Device>& devices);

// Where a device is: its index in the chain, or for one in a rack the rack's
// index, the chain's and its own in that chain (and so on, deeper); none: nowhere.
std::optional<std::vector<int>> devicePath(const std::vector<Device>& devices, const QString& deviceId);
Device& deviceAt(std::vector<Device>& devices, const std::vector<int>& path);
const Device& deviceAt(const std::vector<Device>& devices, const std::vector<int>& path);
// A device by id (in racks too); null: not there.
Device* findDevice(std::vector<Device>& devices, const QString& deviceId);
const Device* findDevice(const std::vector<Device>& devices, const QString& deviceId);
// A rack chain by id, in a chain or in the racks in it (depth first, as
// iterChains), with its rack; both null: not there.
RackChain findChain(std::vector<Device>& devices, const QString& chainId);
ConstRackChain findChain(const std::vector<Device>& devices, const QString& chainId);
// Where a rack's own chain of this id is among its chains (-1: none of its own).
int chainIndex(const Device& rack, const QString& chainId);
// The devices of a chain (the vector itself): `devices` (no chain: a track's
// own) or the rack chain with that id in it; null: not there.
std::vector<Device>* chainDevices(std::vector<Device>& devices, const std::optional<QString>& chain);
const std::vector<Device>* chainDevices(const std::vector<Device>& devices, const std::optional<QString>& chain);
// The chain a device is in: none for the track's own (or nowhere), else the rack chain's id.
std::optional<QString> containerOf(const std::vector<Device>& devices, const QString& deviceId);
// How many racks a chain is in (0: a track's own).
int rackDepth(const std::vector<Device>& devices, const std::optional<QString>& chain);
// New ids for a device and everything in it (its chains too), in place: a copy
// (a preset loaded, a chain duplicated) of devices that exist. Its macro
// mappings follow their devices; mappings to devices not in it go.
void refreshIds(Device& device);
// How deep racks nest in a device: 0 for a device, 1 for a rack without racks in it.
int rackHeight(const Device& device);

}  // namespace sub::app
