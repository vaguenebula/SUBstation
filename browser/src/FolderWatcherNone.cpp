// Watching folder trees where the browser has no way to yet (POSIX systems
// without inotify): nothing is watched, and changes show on a rescan or the
// next start (see FolderWatcher in Platform.h).

#include "Platform.h"

namespace sub::browser::platform {

struct FolderWatcher::State {};

FolderWatcher::FolderWatcher(const NativeString&) : state_(std::make_unique<State>()) {}

FolderWatcher::~FolderWatcher() = default;

bool FolderWatcher::ok() const { return false; }

WaitHandle FolderWatcher::handle() const { return -1; }

FolderWatcher::Changes FolderWatcher::take(std::vector<NativeString>& paths) {
    paths.clear();
    return Changes::Failed;
}

}  // namespace sub::browser::platform
