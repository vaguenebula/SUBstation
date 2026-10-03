#pragma once
// MIDI input. Devices are opened, listed and closed on the main thread, like
// audio devices; their messages arrive on the driver's threads.
//
// Timing: each message is stamped with the host clock (steady_clock) when it
// arrives, and turned into a time on the audio device's sample clock right
// away, from the latest callback's (host time, sample time) pair (AudioClock).
// It is played one device buffer after that: a callback renders a buffer ahead
// of what is heard, so a message that came in during one buffer is placed at
// the same distance into the next, and the delay stays constant instead of
// jittering with the callbacks. Converting on arrival also makes the timing
// testable: a message stamped against a known clock reading lands on a known
// sample.
//
// The messages reach the audio thread through a single-consumer queue; the
// producers (driver threads, sendMidiInput()) serialise on their own mutex, so
// the audio thread never waits.
//
// WinMM is the first backend; Windows MIDI Services can be another later
// (MidiInputDevices is the only part that knows about it).

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "rt/RtUtils.h"

namespace sub {

// A short MIDI message on its way to the audio thread.
struct MidiInputEvent {
    int64_t time = 0;   // device sample at which it plays
    uint16_t port = 0;  // which input (Engine's port ids)
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
};

// The audio device's sample clock against the host clock: written by the audio
// thread at each callback (a seqlock: wait-free for it, readers retry), read
// by whoever stamps MIDI input.
class AudioClock {
public:
    struct Reading {
        bool running = false;
        int64_t hostTimeNs = 0;  // when the callback that rendered `sampleTime` began
        int64_t sampleTime = 0;
    };

    // Audio thread. Callbacks that share a host time (a backend splitting its
    // buffer) keep the first one's anchor.
    void update(int64_t hostTimeNs, int64_t sampleTime) noexcept {
        if (lastHost_ == hostTimeNs && running_.load(std::memory_order_relaxed)) return;
        lastHost_ = hostTimeNs;
        seq_.fetch_add(1, std::memory_order_seq_cst);
        hostTimeNs_.store(hostTimeNs, std::memory_order_seq_cst);
        sampleTime_.store(sampleTime, std::memory_order_seq_cst);
        running_.store(true, std::memory_order_seq_cst);
        seq_.fetch_add(1, std::memory_order_seq_cst);
    }
    // Edit side, while no callback runs (the device is closed or not started yet).
    void stop() noexcept {
        seq_.fetch_add(1, std::memory_order_seq_cst);
        running_.store(false, std::memory_order_seq_cst);
        lastHost_ = 0;
        seq_.fetch_add(1, std::memory_order_seq_cst);
    }
    Reading read() const noexcept {
        Reading r;
        for (;;) {
            const uint32_t before = seq_.load(std::memory_order_seq_cst);
            if (before & 1u) continue;  // being written
            r.running = running_.load(std::memory_order_seq_cst);
            r.hostTimeNs = hostTimeNs_.load(std::memory_order_seq_cst);
            r.sampleTime = sampleTime_.load(std::memory_order_seq_cst);
            if (seq_.load(std::memory_order_seq_cst) == before) return r;
        }
    }

private:
    std::atomic<uint32_t> seq_{0};
    std::atomic<bool> running_{false};
    std::atomic<int64_t> hostTimeNs_{0};
    std::atomic<int64_t> sampleTime_{0};
    int64_t lastHost_ = 0;  // audio thread
};

// The host clock MIDI input is stamped with: the one audio backends stamp their
// callbacks with (std::chrono::steady_clock, i.e. QueryPerformanceCounter).
int64_t hostTimeNs() noexcept;

// The system's MIDI input devices (WinMM). Main thread, except the handler,
// which the driver calls on its own thread for every short message (channel
// and system messages; System Exclusive is ignored).
class MidiInputDevices {
public:
    using Handler = std::function<void(uint16_t port, const uint8_t* message, int size, int64_t hostTimeNs)>;

    explicit MidiInputDevices(Handler handler);
    ~MidiInputDevices();
    MidiInputDevices(const MidiInputDevices&) = delete;
    MidiInputDevices& operator=(const MidiInputDevices&) = delete;

    // The inputs present now, by name (a second device of the same name gets " #2").
    std::vector<std::string> available();
    // Opens an input; its messages come tagged with `port`. Throws
    // std::runtime_error with a message for the user (no such device, or
    // another application has it).
    void open(const std::string& name, uint16_t port);
    void close(const std::string& name);
    void closeAll();
    std::vector<std::string> openNames() const;

private:
    struct Device;
    Handler handler_;
    std::vector<std::unique_ptr<Device>> open_;
};

}  // namespace sub
