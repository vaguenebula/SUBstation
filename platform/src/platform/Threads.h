// Threads' priorities: the background ones yield to everything, the audio
// ones go before everything.

#pragma once

namespace sub::platform {

// The calling thread's CPU, I/O and memory priority to background, for good: it
// yields to everything else, the audio thread and disk reads for playback
// first. (Windows: THREAD_MODE_BACKGROUND_BEGIN; Linux: the lowest nice value
// and the idle I/O class, for this thread only; elsewhere nothing yet.)
void enterBackgroundMode();

// The calling thread at real-time priority while the object lives, for threads
// that render audio beside the driver's own (the render workers). Windows:
// MMCSS's "Pro Audio" task; elsewhere nothing yet, and active() is false.
class ScopedRealtimePriority {
public:
    ScopedRealtimePriority() noexcept;
    ~ScopedRealtimePriority();
    ScopedRealtimePriority(const ScopedRealtimePriority&) = delete;
    ScopedRealtimePriority& operator=(const ScopedRealtimePriority&) = delete;

    // Whether the system raised it.
    bool active() const noexcept { return handle_ != nullptr; }

private:
    void* handle_ = nullptr;  // the system's (MMCSS's task handle)
};

}  // namespace sub::platform
