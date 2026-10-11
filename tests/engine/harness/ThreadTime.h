#pragma once
// What a piece of work costs: the CPU time this thread has used, whatever else
// the machine is doing (a wall clock counts the time other processes had the
// core). The cost tests time with it in Release builds only, against generous
// bounds: they catch work done per sample that should be done per block, or an
// allocation storm, and are not benchmarks.

#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

namespace subtest {

// The CPU time this thread has used, in seconds. On Windows it is counted at the
// scheduler's tick (about 16 ms): for long stretches only.
inline double threadSeconds() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
    const auto ticks = [](const FILETIME& t) {
        return static_cast<double>((static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime);
    };
    return 1e-7 * (ticks(kernel) + ticks(user));  // (100 ns ticks)
#else
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return static_cast<double>(t.tv_sec) + 1e-9 * static_cast<double>(t.tv_nsec);
#endif
}

// The same, finely enough to time one audio callback, in the thread clock's own
// units: nanoseconds, or on Windows (whose thread times count in the scheduler's
// ticks) the cycles the thread ran. For comparing pieces of work with each other.
inline double threadTicks() {
#ifdef _WIN32
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    return static_cast<double>(cycles);
#else
    return 1e9 * threadSeconds();
#endif
}

}  // namespace subtest
