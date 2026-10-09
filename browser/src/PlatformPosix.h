// What the browser's POSIX files share (PlatformPosix.cpp, FolderWatcherInotify.cpp):
// reading a folder's entries.

#pragma once

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

namespace sub::browser::platform {

inline bool dotOrDotDot(const char* name) {
    return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

// Whether a directory entry is a real directory (not a symbolic link to one).
inline bool isDirectory(DIR* dir, const dirent* entry) {
#ifdef DT_UNKNOWN
    if (entry->d_type != DT_UNKNOWN) return entry->d_type == DT_DIR;
#endif
    struct stat st;
    return fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode);
}

}  // namespace sub::browser::platform
