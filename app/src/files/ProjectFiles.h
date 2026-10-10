#pragma once
// The files a project plays, and what plays each: its audio clips, and the
// built-in devices whose state names a file (a sampler's sample, under
// "sample"; see DeviceState.h). What the File Manager lists, replaces and
// locates (FileManager.h), and what a hot swap swaps (HotSwap.h).
//
// A clip counts for the file it plays (`path`: a reversed clip's is its
// reversed copy). Frozen tracks' own audio (Freeze) isn't listed: the
// application makes it, and makes it again. Paths are grouped as the system
// compares them (pathIdentity: on Windows in any case) and keep the spelling of
// the first use met, in the arrangement's order: the tracks, the returns, the
// master; on each, its clips, then its devices (in racks too).

#include <QList>
#include <QString>

#include <optional>
#include <tuple>
#include <vector>

#include "editor/ClipRef.h"
#include "model/Device.h"

namespace sub::app {

class Project;

// A device by where it is: its track (a return, kMaster) and its own id.
struct DeviceRef {
    QString trackId;
    QString deviceId;

    friend bool operator==(const DeviceRef&, const DeviceRef&) = default;
    friend bool operator<(const DeviceRef& a, const DeviceRef& b) {
        return std::tie(a.trackId, a.deviceId) < std::tie(b.trackId, b.deviceId);
    }
};

// What plays a file (or what a replacement changes): clips and devices.
struct FileUses {
    ClipRefs clips;
    QList<DeviceRef> devices;

    bool isEmpty() const { return clips.isEmpty() && devices.isEmpty(); }
    int count() const { return static_cast<int>(clips.size() + devices.size()); }

    friend bool operator==(const FileUses&, const FileUses&) = default;
};

// A file the project plays.
struct ProjectFile {
    QString path;
    FileUses uses;
};

// The state value a built-in device keeps its file under.
inline const QString kDeviceFileKey = QStringLiteral("sample");

// The file a built-in device's state names ("": none; a plug-in or a rack: none).
QString deviceFile(const Device& device);
// A built-in device's state (Device::state) naming `path` instead.
std::optional<QString> withDeviceFile(const std::optional<QString>& state, const QString& path);

// Every file the project plays, in the order met.
std::vector<ProjectFile> projectFiles(const Project& project);
// What plays one file (nothing: none of it).
FileUses fileUses(const Project& project, const QString& path);
// What of `uses` is still there, and on a track that isn't frozen (nor in a
// frozen group): what a replacement may change. What is on frozen tracks goes
// into `frozen`, if given.
FileUses changeableUses(const Project& project, const FileUses& uses, FileUses* frozen = nullptr);
// What of `uses` is still there (on frozen tracks too).
FileUses existingUses(const Project& project, const FileUses& uses);
// "3 clips", "Sampler", "2 clips, Sampler"..., for the File Manager's rows.
QString usesText(const Project& project, const FileUses& uses);

}  // namespace sub::app
