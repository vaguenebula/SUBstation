#pragma once
// Small real-time helpers shared by the engine. Everything that runs on the
// audio thread must be wait-free: no locks, no allocation, no deallocation.
//
// What depends on the CPU is here too, each for x86-64 and for arm64:
// ScopedNoDenormals (MXCSR, FPCR) and cpuRelax() (PAUSE, ISB).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define SUB_RT_X86 1
#include <xmmintrin.h>
#elif defined(__aarch64__)
#define SUB_RT_ARM64 1
#endif

namespace sub {

// Enables flush-to-zero / denormals-are-zero for the current scope. Denormal
// floats make recursive DSP (filters, reverb tails, plugins) extremely slow.
// x86: MXCSR's FTZ and DAZ bits; arm64: FPCR's FZ bit (which flushes inputs
// and results alike).
class ScopedNoDenormals {
public:
    ScopedNoDenormals() noexcept : saved_(read()) { write(saved_ | kFlushToZero); }
    ~ScopedNoDenormals() { write(saved_); }
    ScopedNoDenormals(const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;

private:
#if SUB_RT_X86
    using Register = unsigned int;
    static constexpr Register kFlushToZero = 0x8040u;
    static Register read() noexcept { return _mm_getcsr(); }
    static void write(Register value) noexcept { _mm_setcsr(value); }
#elif SUB_RT_ARM64
    using Register = uint64_t;
    static constexpr Register kFlushToZero = Register(1) << 24;
    static Register read() noexcept {
        Register value;
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(value));
        return value;
    }
    static void write(Register value) noexcept { __asm__ __volatile__("msr fpcr, %0" : : "r"(value)); }
#else  // (no other CPU is built for: nothing is flushed)
    using Register = int;
    static constexpr Register kFlushToZero = 0;
    static Register read() noexcept { return 0; }
    static void write(Register) noexcept {}
#endif
    Register saved_;
};

// Inside a spin-wait loop, between looks: tells the CPU this thread is waiting,
// so the other hyper-thread of its core runs and the loop draws less power
// (x86's PAUSE; arm64's ISB, which waits about as long, where its YIELD is a
// no-op on most cores).
inline void cpuRelax() noexcept {
#if SUB_RT_X86
    _mm_pause();
#elif SUB_RT_ARM64
    __asm__ __volatile__("isb sy" : : : "memory");
#endif
}

// The host clock: the time audio callbacks begin and MIDI input arrives,
// stamped with it (std::chrono::steady_clock: QueryPerformanceCounter on
// Windows, CLOCK_MONOTONIC elsewhere), in nanoseconds.
inline int64_t hostTimeNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Linear ramp towards a target; removes "zipper" noise from gain/pan changes.
class SmoothedValue {
public:
    void reset(double sampleRate, double rampSeconds) noexcept {
        rampLength_ = std::max(1, static_cast<int>(sampleRate * rampSeconds));
        snapTo(target_);
    }
    void snapTo(float value) noexcept {
        current_ = target_ = value;
        remaining_ = 0;
    }
    void setTarget(float value) noexcept {
        if (value == target_) return;
        target_ = value;
        remaining_ = rampLength_;
        step_ = (target_ - current_) / static_cast<float>(remaining_);
    }
    float next() noexcept {
        if (remaining_ > 0) {
            current_ += step_;
            if (--remaining_ == 0) current_ = target_;
        }
        return current_;
    }
    float current() const noexcept { return current_; }
    float target() const noexcept { return target_; }
    bool isSmoothing() const noexcept { return remaining_ > 0; }

private:
    float current_ = 0.f, target_ = 0.f, step_ = 0.f;
    int remaining_ = 0, rampLength_ = 1;
};

// Wait-free single-producer / single-consumer ring buffer. Head and tail sit on
// separate cache lines (MSVC warns about the resulting padding).
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
template <typename T, size_t Capacity>
class SpscQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    bool push(const T& item) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) == Capacity) return false;
        items_[head & (Capacity - 1)] = item;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& out) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        out = items_[tail & (Capacity - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }
    bool empty() const noexcept { return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire); }

private:
    std::array<T, Capacity> items_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// Keeps objects alive until the audio thread can no longer be using them.
//
// The audio thread loads shared pointers (snapshot, preview source) at the start
// of a callback and bumps an epoch counter at the end. An object is retired
// together with the epoch read *after* it was unpublished; any callback that
// could still see it finishes before the epoch moves past that value, so the
// object may be freed once `epoch > retiredEpoch` (or when no device runs).
class DeferredReleasePool {
public:
    void retire(std::shared_ptr<const void> object, uint64_t epoch) {
        if (object) items_.push_back({std::move(object), epoch});
    }
    void collect(uint64_t currentEpoch, bool audioThreadIdle) {
        std::erase_if(items_, [&](const Item& item) { return audioThreadIdle || currentEpoch > item.epoch; });
    }
    size_t size() const { return items_.size(); }

private:
    struct Item {
        std::shared_ptr<const void> object;
        uint64_t epoch;
    };
    std::vector<Item> items_;
};

// Meter peaks are written with "store max" by the audio thread and reset with
// exchange(0) by the UI.
inline void atomicStoreMax(std::atomic<float>& target, float value) noexcept {
    float current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

inline float dbToGain(float db) noexcept { return db <= -120.f ? 0.f : std::pow(10.f, db / 20.f); }

// Balance-style stereo pan with a sine taper: unity at centre, the opposite
// side fades out as the pan moves away from it.
inline void balanceGains(float pan, float& left, float& right) noexcept {
    pan = std::clamp(pan, -1.f, 1.f);
    constexpr float kHalfPi = 1.57079632679f;
    left = pan > 0.f ? std::cos(pan * kHalfPi) : 1.f;
    right = pan < 0.f ? std::cos(-pan * kHalfPi) : 1.f;
}

// Values one thread publishes for others to draw (a meter's readings, samples
// for an analyser). The writer never waits and never fails: it overwrites the
// oldest values. Each reader keeps its own position, so any number can follow
// the stream, and one that falls behind skips to the latest `capacity` values.
// (A reader overtaken while it reads may get a newer value in an older one's
// place; for drawing that is harmless.)
class DisplayStream {
public:
    // `capacity` is rounded up to a power of two.
    explicit DisplayStream(size_t capacity) {
        size_t size = 1;
        while (size < capacity) size <<= 1;
        slots_ = std::vector<std::atomic<float>>(size);
        mask_ = size - 1;
    }

    // Writer thread (real-time).
    void push(float value) noexcept {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        slots_[head & mask_].store(value, std::memory_order_relaxed);
        head_.store(head + 1, std::memory_order_release);
    }

    // Any thread: appends the values published since `position` (a previous
    // call's result; 0 the first time) to `out`, oldest first, and returns the
    // position to read from next.
    uint64_t read(uint64_t position, std::vector<float>& out) const {
        const uint64_t head = head_.load(std::memory_order_acquire);
        const uint64_t capacity = mask_ + 1;
        if (position > head) position = head;  // (a position from another stream)
        if (head - position > capacity) position = head - capacity;
        for (uint64_t i = position; i < head; ++i) out.push_back(slots_[i & mask_].load(std::memory_order_relaxed));
        return head;
    }

    uint64_t written() const noexcept { return head_.load(std::memory_order_acquire); }

private:
    std::vector<std::atomic<float>> slots_;
    size_t mask_ = 0;
    std::atomic<uint64_t> head_{0};
};

}  // namespace sub
