#pragma once
// Kinds of devices: the built-in ones (as the engine has them), what devices
// are called, which are instruments, and new devices, racks and chains.

#include "model/Device.h"

#include "builtin/BuiltinRegistry.h"  // the engine's built-in devices (and Processor.h's ParamInfo)

#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

#include <optional>
#include <utility>
#include <vector>

namespace sub::app {

// A built-in device, as the engine describes it (engine/src/builtin/devices/):
// a new one needs nothing here.
struct BuiltinDevice {
    QString kind;  // its id: "utility"
    QString name;  // "Utility"
    QString category;  // "Instruments" or "Audio Effects": how the browser groups them
    bool instrument = false;
    QMap<QString, double> defaults;  // param id -> its default value
    const sub::BuiltinInfo* info = nullptr;  // the engine's description (its parameters)
};

// Every built-in device, instruments first, then by name.
const std::vector<BuiltinDevice>& builtinDevices();
// A built-in device by kind; null: no such one.
const BuiltinDevice* builtinDevice(const QString& kind);
// How the browser's Built-in category groups the devices: (category, kinds), in order.
std::vector<std::pair<QString, QStringList>> builtinCategories();

// New MIDI tracks come with it, ready to play.
inline const QString kDefaultInstrument = QStringLiteral("synth");

// Whether a device of this kind (and plug-in) is an instrument.
bool isInstrument(const QString& kind, const std::optional<PluginRef>& plugin = std::nullopt);
// An instrument, or a rack with one in it (an instrument rack: it plays the track's notes).
bool deviceIsInstrument(const Device& device);
// Whether a preset can be loaded into a device in place: a device of the same
// kind (the same plug-in; a rack into a rack, both instrument racks or neither).
bool loadsInto(const Device& preset, const Device& device);
// What a device is called: a rack by its own name if it has one (its
// preset's), else every device by its kind (kindName).
QString deviceName(const Device& device);
// The name of a device's kind: a plug-in's, a built-in device's, Audio Effect
// Rack or Instrument Rack.
QString kindName(const Device& device);

// A new device of a kind (new id): a built-in one with its parameters at their
// defaults, a plug-in (kind kPluginKind: `plugin` says which; throws EditError
// without one), or an empty rack. Throws EditError for a kind there is no such
// device of.
Device newDevice(const QString& kind, const std::optional<PluginRef>& plugin = std::nullopt);
// A rack with these chains, its macros at 0.
Device newRack(std::vector<Chain> chains);
Chain newChain(const QString& name, std::vector<Device> devices = {});

// A built-in device's parameter as the engine describes it (null: no such one).
const sub::ParamInfo* builtinParamInfo(const QString& kind, const QString& paramId);

// The ids of these devices and of everything in them.
QSet<QString> deviceIdsOfList(const std::vector<Device>& devices);
// A device's id, and those of everything in it (a rack).
QSet<QString> deviceIdsOf(const Device& device);

}  // namespace sub::app
