#include "BenchBackend.h"

#include <chrono>
#include <mutex>

#ifndef _WIN32
#include <pthread.h>
#include <sched.h>
#include <time.h>
#endif

#include "rt/RtUtils.h"

namespace sub {
namespace {

// Recorded by the callback thread into preallocated rings; taken under a lock
// by whoever asks (the callback thread never takes it: it only bumps a counter).
constexpr size_t kCapacity = size_t{1} << 21;
std::vector<float> gCallbackUs(kCapacity), gLateUs(kCapacity);
std::atomic<size_t> gWritten{0};
size_t gTaken = 0;
std::atomic<bool> gRealtime{false};
std::mutex gTakeLock;

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

BenchBackend::Stats BenchBackend::takeStats() {
    std::lock_guard lock(gTakeLock);
    Stats stats;
    const size_t written = gWritten.load(std::memory_order_acquire);
    const size_t from = written - gTaken > kCapacity ? written - kCapacity : gTaken;
    for (size_t i = from; i < written; ++i) {
        stats.callbackUs.push_back(gCallbackUs[i % kCapacity]);
        stats.lateUs.push_back(gLateUs[i % kCapacity]);
    }
    gTaken = written;
    stats.realtime = gRealtime.load();
    return stats;
}

BenchBackend::~BenchBackend() { close(); }

void BenchBackend::open(const DeviceConfig& config, AudioCallback* callback) {
    close();
    rate_ = config.sampleRate ? config.sampleRate : 48000;
    frames_ = config.bufferFrames ? config.bufferFrames : 128;
    left_.assign(frames_, 0.f);
    right_.assign(frames_, 0.f);
    callback_ = callback;
}

void BenchBackend::start() {
    if (!callback_ || thread_.joinable()) return;
    quit_.store(false);
    thread_ = std::thread([this] { run(); });
}

void BenchBackend::close() {
    quit_.store(true);
    if (thread_.joinable()) thread_.join();
    callback_ = nullptr;
}

DeviceState BenchBackend::state() const {
    DeviceState state;
    if (!callback_) return state;
    state.driver = kName;
    state.name = "Bench";
    state.sampleRate = rate_;
    state.bufferFrames = frames_;
    state.outputChannels = {0, 1};
    state.capabilities.outputNames = {"Left", "Right"};
    return state;
}

void BenchBackend::run() noexcept {
#ifndef _WIN32
    sched_param param{};
    param.sched_priority = 80;
    gRealtime.store(pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0);
#endif
    const int64_t period = static_cast<int64_t>(1e9 * frames_ / rate_);
    int64_t deadline = nowNs() + period;
    int64_t sampleTime = 0;
    float* outputs[2] = {left_.data(), right_.data()};
    while (!quit_.load(std::memory_order_relaxed)) {
#ifndef _WIN32
        timespec at{static_cast<time_t>(deadline / 1000000000), static_cast<long>(deadline % 1000000000)};
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &at, nullptr);
#else
        std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(deadline)));
#endif
        const int64_t started = nowNs();
        AudioIO io;
        io.outputs = outputs;
        io.numOutputs = 2;
        io.frames = frames_;
        io.sampleTime = sampleTime;
        io.hostTimeNs = started;
        callback_->audioCallback(io);
        const int64_t ended = nowNs();
        const size_t slot = gWritten.load(std::memory_order_relaxed);
        gCallbackUs[slot % kCapacity] = static_cast<float>(ended - started) / 1000.f;
        gLateUs[slot % kCapacity] = static_cast<float>(started - deadline) / 1000.f;
        gWritten.store(slot + 1, std::memory_order_release);
        sampleTime += frames_;
        deadline += period;
        // A callback that overran its buffer: the device would have dropped out.
        // Carry on from now rather than racing to catch up.
        if (ended > deadline + period) deadline = ended;
    }
}

}  // namespace sub
