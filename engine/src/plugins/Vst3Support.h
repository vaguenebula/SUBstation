#pragma once
// Host-side VST3 building blocks the audio thread uses. Unlike the SDK's
// examples, none of them allocates after it is set up, so a plug-in's
// process() call never makes the host allocate.

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>
#include <xmmintrin.h>

#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "rt/RtUtils.h"

namespace gil::vst3 {

using Steinberg::int32;
using Steinberg::tresult;
using Steinberg::uint32;
using Steinberg::Vst::ParamID;
using Steinberg::Vst::ParamValue;

// FUnknown for objects the host owns: plug-ins may addRef/release them, but
// their lifetime is the host's business.
#define GIL_HOST_OWNED_FUNKNOWN(Interface)                                                   \
    tresult PLUGIN_API queryInterface(const Steinberg::TUID queried, void** obj) override {  \
        if (Steinberg::FUnknownPrivate::iidEqual(queried, Interface::iid) ||                \
            Steinberg::FUnknownPrivate::iidEqual(queried, Steinberg::FUnknown::iid)) {      \
            *obj = this;                                                                     \
            return Steinberg::kResultOk;                                                     \
        }                                                                                    \
        *obj = nullptr;                                                                      \
        return Steinberg::kNoInterface;                                                      \
    }                                                                                        \
    uint32 PLUGIN_API addRef() override { return 1; }                                        \
    uint32 PLUGIN_API release() override { return 1; }

// Short critical sections between the main thread and the odd plug-in that
// calls the host from another thread.
class SpinLock {
public:
    void lock() noexcept {
        while (flag_.test_and_set(std::memory_order_acquire)) _mm_pause();
    }
    void unlock() noexcept { flag_.clear(std::memory_order_release); }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

// Lets the main thread take a plug-in away from the rendering thread for a
// moment (restarting it, loading its state). The rendering thread never waits:
// while the plug-in is taken, it skips it and the audio passes through.
class ProcessGuard {
public:
    // Rendering thread.
    bool tryEnter() noexcept {
        int expected = kIdle;
        return state_.compare_exchange_strong(expected, kProcessing, std::memory_order_acquire);
    }
    void leave() noexcept { state_.store(kIdle, std::memory_order_release); }

    // Main thread; nests.
    void suspend() {
        if (depth_++ > 0) return;
        int expected = kIdle;
        while (!state_.compare_exchange_weak(expected, kSuspended, std::memory_order_acquire)) {
            expected = kIdle;
            std::this_thread::yield();
        }
    }
    void resume() {
        if (--depth_ == 0) state_.store(kIdle, std::memory_order_release);
    }

private:
    enum : int { kIdle = 0, kProcessing = 1, kSuspended = 2 };
    std::atomic<int> state_{kIdle};
    int depth_ = 0;
};

class ScopedSuspend {
public:
    explicit ScopedSuspend(ProcessGuard& guard) : guard_(guard) { guard_.suspend(); }
    ~ScopedSuspend() { guard_.resume(); }
    ScopedSuspend(const ScopedSuspend&) = delete;
    ScopedSuspend& operator=(const ScopedSuspend&) = delete;

private:
    ProcessGuard& guard_;
};

// Events for one process call, in time order.
class HostEventList final : public Steinberg::Vst::IEventList {
public:
    void setCapacity(size_t capacity) {
        events_.assign(capacity, {});
        count_ = 0;
    }
    void clear() noexcept { count_ = 0; }
    bool add(const Steinberg::Vst::Event& event) noexcept {
        if (count_ >= static_cast<int32>(events_.size())) return false;
        events_[count_++] = event;
        return true;
    }

    int32 PLUGIN_API getEventCount() override { return count_; }
    tresult PLUGIN_API getEvent(int32 index, Steinberg::Vst::Event& event) override {
        if (index < 0 || index >= count_) return Steinberg::kInvalidArgument;
        event = events_[index];
        return Steinberg::kResultOk;
    }
    tresult PLUGIN_API addEvent(Steinberg::Vst::Event& event) override {
        return add(event) ? Steinberg::kResultOk : Steinberg::kOutOfMemory;
    }
    GIL_HOST_OWNED_FUNKNOWN(Steinberg::Vst::IEventList)

private:
    std::vector<Steinberg::Vst::Event> events_;
    int32 count_ = 0;
};

// One parameter's changes within a block, sorted by sample offset.
class HostParamQueue final : public Steinberg::Vst::IParamValueQueue {
public:
    static constexpr int32 kMaxPoints = 16;

    void reset(ParamID id, size_t slot) noexcept {
        id_ = id;
        slot_ = slot;
        count_ = 0;
    }
    size_t slot() const noexcept { return slot_; }
    ParamValue lastValue() const noexcept { return count_ > 0 ? points_[count_ - 1].value : 0.0; }

    ParamID PLUGIN_API getParameterId() override { return id_; }
    int32 PLUGIN_API getPointCount() override { return count_; }
    tresult PLUGIN_API getPoint(int32 index, int32& sampleOffset, ParamValue& value) override {
        if (index < 0 || index >= count_) return Steinberg::kInvalidArgument;
        sampleOffset = points_[index].offset;
        value = points_[index].value;
        return Steinberg::kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 sampleOffset, ParamValue value, int32& index) override {
        int32 at = count_;
        while (at > 0 && points_[at - 1].offset > sampleOffset) --at;
        if (at > 0 && points_[at - 1].offset == sampleOffset) {  // the later value wins
            points_[at - 1].value = value;
            index = at - 1;
            return Steinberg::kResultOk;
        }
        if (count_ == kMaxPoints) {
            if (at < count_) return Steinberg::kOutOfMemory;
            points_[count_ - 1] = {sampleOffset, value};  // full: keep the latest value
            index = count_ - 1;
            return Steinberg::kResultOk;
        }
        for (int32 i = count_; i > at; --i) points_[i] = points_[i - 1];
        points_[at] = {sampleOffset, value};
        ++count_;
        index = at;
        return Steinberg::kResultOk;
    }
    GIL_HOST_OWNED_FUNKNOWN(Steinberg::Vst::IParamValueQueue)

private:
    struct Point {
        int32 offset;
        ParamValue value;
    };
    ParamID id_ = 0;
    size_t slot_ = 0;
    std::array<Point, kMaxPoints> points_{};
    int32 count_ = 0;
};

// The parameter changes of one block. Finding a parameter's queue is a hash
// lookup, as plug-ins may report hundreds of output parameters per block.
class HostParamChanges final : public Steinberg::Vst::IParameterChanges {
public:
    void setCapacity(size_t queues) {
        queues_ = std::vector<HostParamQueue>(queues);
        size_t slots = 16;
        while (slots < 2 * queues) slots *= 2;
        slots_.assign(slots, -1);
        used_ = 0;
    }
    void clear() noexcept {
        for (int32 i = 0; i < used_; ++i) slots_[queues_[i].slot()] = -1;
        used_ = 0;
    }
    bool add(ParamID id, int32 offset, ParamValue value) noexcept {
        int32 index = 0;
        auto* queue = addParameterData(id, index);
        return queue && queue->addPoint(offset, value, index) == Steinberg::kResultOk;
    }
    HostParamQueue& queue(int32 index) noexcept { return queues_[index]; }

    int32 PLUGIN_API getParameterCount() override { return used_; }
    Steinberg::Vst::IParamValueQueue* PLUGIN_API getParameterData(int32 index) override {
        return index >= 0 && index < used_ ? &queues_[index] : nullptr;
    }
    Steinberg::Vst::IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32& index) override {
        if (slots_.empty()) return nullptr;
        const size_t mask = slots_.size() - 1;
        size_t slot = (static_cast<size_t>(id) * 2654435761u) & mask;
        while (slots_[slot] >= 0) {
            if (queues_[slots_[slot]].getParameterId() == id) {
                index = slots_[slot];
                return &queues_[index];
            }
            slot = (slot + 1) & mask;
        }
        if (used_ >= static_cast<int32>(queues_.size())) return nullptr;
        slots_[slot] = used_;
        queues_[used_].reset(id, slot);
        index = used_;
        return &queues_[used_++];
    }
    GIL_HOST_OWNED_FUNKNOWN(Steinberg::Vst::IParameterChanges)

private:
    std::vector<HostParamQueue> queues_;
    std::vector<int32> slots_;  // hash table of queue indices, -1 = empty
    int32 used_ = 0;
};

struct ParamChange {
    ParamID id = 0;
    ParamValue value = 0.0;
};

// Parameter values on their way to the rendering thread. Several threads may
// push (the UI, a plug-in calling back from elsewhere); one thread pops.
class ParamChangeQueue {
public:
    bool push(ParamID id, ParamValue value) noexcept {
        lock_.lock();
        const bool pushed = queue_.push({id, value});
        lock_.unlock();
        return pushed;
    }
    bool pop(ParamChange& change) noexcept { return queue_.pop(change); }
    bool empty() const noexcept { return queue_.empty(); }

private:
    SpinLock lock_;
    SpscQueue<ParamChange, 4096> queue_;
};

}  // namespace gil::vst3
