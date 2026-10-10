// The platform layer elsewhere than Windows: POSIX calls, and on Linux
// per-thread priorities.

#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/syscall.h>
#endif

#include "platform/Files.h"
#include "platform/Paths.h"
#include "platform/Threads.h"

namespace sub::platform {

// --- Paths.h ---------------------------------------------------------------------------

std::string nameKey(std::string_view name) { return std::string(name); }

std::string pathKey(std::string_view path) { return std::string(path); }

// --- Files.h ---------------------------------------------------------------------------

std::optional<FileStamp> stamp(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) return std::nullopt;
#ifdef __APPLE__
    const timespec& time = st.st_mtimespec;
#else
    const timespec& time = st.st_mtim;
#endif
    FileStamp s;
    s.size = static_cast<uint64_t>(st.st_size);
    s.modified = static_cast<uint64_t>(time.tv_sec) * 1'000'000'000u + static_cast<uint64_t>(time.tv_nsec);
    return s;
}

bool replaceFile(const std::string& from, const std::string& to) { return std::rename(from.c_str(), to.c_str()) == 0; }

std::FILE* openFile(const std::string& path, bool write) { return std::fopen(path.c_str(), write ? "wb" : "rb"); }

// --- Threads.h -------------------------------------------------------------------------

void enterBackgroundMode() {
#ifdef __linux__
    // On Linux the nice value and the I/O priority are per thread.
    const auto thread = static_cast<int>(syscall(SYS_gettid));
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(thread), 19) != 0) {
        // Not allowed: it stays at its priority.
    }
    // The idle I/O class: the disk is read for it only when nothing else wants it.
    constexpr int kWhoProcess = 1, kClassIdle = 3, kClassShift = 13;
    if (syscall(SYS_ioprio_set, kWhoProcess, thread, kClassIdle << kClassShift) != 0) {
        // Not supported: best effort as before.
    }
#endif
}

ScopedRealtimePriority::ScopedRealtimePriority() noexcept = default;

ScopedRealtimePriority::~ScopedRealtimePriority() = default;

}  // namespace sub::platform
