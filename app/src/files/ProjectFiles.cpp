#include "files/ProjectFiles.h"

#include <QHash>
#include <QMap>
#include <QStringList>

#include <functional>

#include "model/DeviceState.h"
#include "model/Devices.h"
#include "model/Numbers.h"
#include "model/Paths.h"
#include "model/Project.h"

namespace sub::app {

namespace {

// Every use of a file in the project: (its path, the use), in the order met.
void eachUse(const Project& project, const std::function<void(const QString&, const ClipRef*, const DeviceRef*)>& use) {
    for (const Track* track : project.allTracks()) {
        if (track->isAudio()) {
            for (const Clip& clip : track->clips) {
                if (!clip.isAudio() || clip.path.isEmpty()) continue;
                const ClipRef ref{track->id, clip.id};
                use(clip.path, &ref, nullptr);
            }
        }
        for (const Device* device : iterDevices(track->devices)) {  // (in racks too)
            const QString path = deviceFile(*device);
            if (path.isEmpty()) continue;
            const DeviceRef ref{track->id, device->id};
            use(path, nullptr, &ref);
        }
    }
}

bool frozenTrack(const Project& project, const QString& trackId) {
    return project.hasOwner(trackId) && project.isFrozen(trackId);
}

}  // namespace

QString deviceFile(const Device& device) {
    if (device.isPlugin() || device.isRack() || !device.state) return {};
    return deviceState::fromModel(device.state).value(kDeviceFileKey);
}

std::optional<QString> withDeviceFile(const std::optional<QString>& state, const QString& path) {
    deviceState::Values values = deviceState::fromModel(state);
    if (path.isEmpty())
        values.remove(kDeviceFileKey);
    else
        values.insert(kDeviceFileKey, path);
    return deviceState::toModel(values);
}

std::vector<ProjectFile> projectFiles(const Project& project) {
    std::vector<ProjectFile> files;
    QHash<QString, size_t> byKey;
    eachUse(project, [&](const QString& path, const ClipRef* clip, const DeviceRef* device) {
        const QString key = pathIdentity(path);
        auto found = byKey.constFind(key);
        if (found == byKey.constEnd()) {
            found = byKey.insert(key, files.size());
            files.push_back({path, {}});
        }
        FileUses& uses = files[*found].uses;
        if (clip) uses.clips.append(*clip);
        if (device) uses.devices.append(*device);
    });
    return files;
}

FileUses fileUses(const Project& project, const QString& path) {
    FileUses uses;
    if (path.isEmpty()) return uses;
    const QString key = pathIdentity(path);
    eachUse(project, [&](const QString& used, const ClipRef* clip, const DeviceRef* device) {
        if (pathIdentity(used) != key) return;
        if (clip) uses.clips.append(*clip);
        if (device) uses.devices.append(*device);
    });
    return uses;
}

FileUses existingUses(const Project& project, const FileUses& uses) {
    FileUses existing;
    for (const ClipRef& ref : uses.clips) {
        if (project.findClip(ref.trackId, ref.clipId)) existing.clips.append(ref);
    }
    for (const DeviceRef& ref : uses.devices) {
        const bool there = project.hasOwner(ref.trackId) && project.findDevice(ref.trackId, ref.deviceId);
        if (there) existing.devices.append(ref);
    }
    return existing;
}

FileUses changeableUses(const Project& project, const FileUses& uses, FileUses* frozen) {
    FileUses changeable;
    const FileUses existing = existingUses(project, uses);
    for (const ClipRef& ref : existing.clips) {
        if (!frozenTrack(project, ref.trackId))
            changeable.clips.append(ref);
        else if (frozen)
            frozen->clips.append(ref);
    }
    for (const DeviceRef& ref : existing.devices) {
        if (!frozenTrack(project, ref.trackId))
            changeable.devices.append(ref);
        else if (frozen)
            frozen->devices.append(ref);
    }
    return changeable;
}

QString usesText(const Project& project, const FileUses& uses) {
    QStringList parts;
    if (!uses.clips.isEmpty()) {
        const int clips = static_cast<int>(uses.clips.size());
        parts << countText(clips, QStringLiteral("clip"), QStringLiteral("clips"));
    }
    // Devices by what they are called ("Sampler", "2 × Sampler").
    QMap<QString, int> devices;
    QStringList order;
    for (const DeviceRef& ref : uses.devices) {
        const Device* device = project.hasOwner(ref.trackId) ? project.findDevice(ref.trackId, ref.deviceId) : nullptr;
        const QString name = device ? deviceName(*device) : QStringLiteral("Device");
        if (!devices.contains(name)) order << name;
        ++devices[name];
    }
    for (const QString& name : order) {
        const int count = devices.value(name);
        parts << (count == 1 ? name : QStringLiteral("%1 × %2").arg(count).arg(name));
    }
    return parts.join(QStringLiteral(", "));
}

}  // namespace sub::app
