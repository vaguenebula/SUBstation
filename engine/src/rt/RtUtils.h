#pragma once
// Small real-time helpers shared by the engine. Everything that runs on the
// audio thread must be wait-free: no locks, no allocation, no deallocation.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <xmmintrin.h>

namespace gil {

// Enables flush-to-zero / denormals-are-zero for the current scope. Denormal
// floats make recursive DSP (filters, reverb tails, plugins) extremely slow.
class ScopedNoDenormals {
public:
    ScopedNoDenormals() noexcept : saved_(_mm_getcsr()) { _mm_setcsr(saved_ | 0x8040u); }
    ~ScopedNoDenormals() { _mm_setcsr(saved_); }
    ScopedNoDenormals(const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;

private:
    unsigned int saved_;
};

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

}  // namespace gil
