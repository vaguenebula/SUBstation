#pragma once
// The user's preset library: devices saved by name (a device's save button),
// listed in the browser's Presets section.
//
// A preset is a .gilpreset file (savePreset: the device as the project file
// stores it, a rack with everything in it). The library is a folder
// (Documents/SUBstation/Presets, or SUBSTATION_PRESETS) with a folder per kind of
// device, named as the kind is (a plug-in's name, a built-in device's, "Audio
// Effect Rack" or "Instrument Rack"): the browser groups presets by it. A rack
// takes the name of the preset it is saved as or loaded from (Device::name).
// Presets straight in the library folder (put there by hand) are listed too,
// ungrouped.
//
// A preset loads as a new device (loadPreset), or into a device of the same kind
// (loadsInto, the editor's loadPresetInto).
//
// Default presets: a device saved as the default for its kind (a built-in
// device by its kind, a plug-in by its class id; not racks) is what new devices
// of that kind start as (defaultDevice, which the editor asks through its device
// defaults). They are in the library's Defaults folder, which the browser
// doesn't list: "<device name>.gilpreset" for a built-in device, "<plug-in name>
// (<class id>).gilpreset" for a plug-in.
//
// Functions taking a `root` use the library folder when it is "".

#include "model/Device.h"

#include <QString>

#include <optional>
#include <vector>

namespace sub::app {

// The library's folder of default presets (not listed).
inline const QString kDefaultsFolder = QStringLiteral("Defaults");

QString libraryDir();

struct PresetFile {
    QString path;
    QString name;  // the file's name, without its extension
    QString group;  // the device it is for (its folder in the library); "" straight in the library

    friend bool operator==(const PresetFile&, const PresetFile&) = default;
};

// A name as a file name: characters Windows refuses become "_". Throws
// EditError for one that is empty (or only dots and spaces) or reserved.
QString presetFileName(const QString& name);
// The library folder for presets of a device: its name (not the defaults' folder).
QString groupOf(const Device& device);
// Where a preset of `device` called `name` goes in the library.
QString presetPath(const Device& device, const QString& name, const QString& root = {});
// Save a device as a preset in the library (replacing one of that name); its path.
QString saveToLibrary(const Device& device, const QString& name, const QString& root = {});
// The presets in the library: by group, then by name (ignoring case).
std::vector<PresetFile> listPresets(const QString& root = {});
// Give a preset another name (in its folder); its new path. Throws EditError if
// another preset there has that name (a change of case is no clash), or for a
// name that can't be a file's.
QString renamePreset(const QString& path, const QString& name);

// Where the default preset of a kind of device is (none: racks, which have none).
std::optional<QString> defaultPath(const QString& kind, const std::optional<PluginRef>& plugin = std::nullopt,
                                   const QString& root = {});
bool hasDefault(const QString& kind, const std::optional<PluginRef>& plugin = std::nullopt,
                const QString& root = {});
// Save a device (not a rack: EditError) as the default preset of its kind
// (replacing the one there was); its path.
QString saveDefault(const Device& device, const QString& root = {});
// New devices of this kind start as they come again. Whether there was a default.
bool clearDefault(const QString& kind, const std::optional<PluginRef>& plugin = std::nullopt,
                  const QString& root = {});
// A new device of this kind (and plug-in) as its default preset has it (new
// ids), or none: it has none (or one that can't be read, or is for another
// device: it is ignored). A plug-in keeps the PluginRef asked for (where it is
// now) with the preset's state; a built-in device takes the preset's
// parameters over its defaults (those it was saved without stay at theirs).
std::optional<Device> defaultDevice(const QString& kind, const std::optional<PluginRef>& plugin = std::nullopt,
                                    const QString& root = {});

}  // namespace sub::app
