// The intelligence module's platform layer off Windows (see Platform.h).

#include "core/Platform.h"

#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>

#ifdef __linux__
#include <sys/syscall.h>
#endif

namespace sub::intelligence::platform {

std::string pathKey(std::string_view path) { return std::string(path); }

std::optional<FileStamp> stamp(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) return std::nullopt;
    FileStamp s;
    s.size = static_cast<uint64_t>(st.st_size);
#if defined(__APPLE__)
    s.modified = static_cast<uint64_t>(st.st_mtimespec.tv_sec) * 1000000000ull + static_cast<uint64_t>(st.st_mtimespec.tv_nsec);
#else
    s.modified = static_cast<uint64_t>(st.st_mtim.tv_sec) * 1000000000ull + static_cast<uint64_t>(st.st_mtim.tv_nsec);
#endif
    return s;
}

void enterBackgroundMode() {
#ifdef __linux__
    // On Linux the nice value and the I/O priority are per thread.
    const auto thread = static_cast<int>(syscall(SYS_gettid));
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(thread), 19) != 0) {
        // Not allowed: it stays at its priority.
    }
    constexpr int kWhoProcess = 1, kClassIdle = 3, kClassShift = 13;
    if (syscall(SYS_ioprio_set, kWhoProcess, thread, kClassIdle << kClassShift) != 0) {
        // Not supported: best effort as before.
    }
#endif
}

bool replaceFile(const std::string& from, const std::string& to) { return std::rename(from.c_str(), to.c_str()) == 0; }

std::FILE* openFile(const std::string& path, bool write) { return std::fopen(path.c_str(), write ? "wb" : "rb"); }

}  // namespace sub::intelligence::platform
