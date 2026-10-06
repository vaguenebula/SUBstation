#pragma once
// Performance experiments (perf-experiments branch only): runtime switches the
// benchmarks flip to compare the engine with and without each idea, and the
// per-processor profile they read. Not part of the product.

#include <atomic>
#include <cstdint>

namespace sub::experiments {

// Per-processor timing: every process() call is timed, and its input and
// output peaks are measured.
inline std::atomic<bool> profile{false};

// Silence suspension: a processor whose input and output have stayed quiet
// (peak below quietThreshold) long enough, and that got no events, isn't called;
// its output is silence. It wakes as soon as its input or its events aren't.
inline std::atomic<bool> suspend{false};
inline std::atomic<float> quietThreshold{1.0e-5f};  // -100 dBFS
// Quiet this long (on top of the processor's tail and latency) before it sleeps.
inline std::atomic<int> holdMs{500};

// Tracks nobody hears (muted, or solo leaves them out) and that key no
// sidechain skip their render once their fader's ramp is done.
inline std::atomic<bool> skipUnheard{false};

// The master's devices process each chunk while the next chunk's tracks render
// (one chunk later: the output lags by a chunk more).
inline std::atomic<bool> pipelineMaster{false};

// Offline renders in chunks of this many frames (<= Renderer::kMaxBlock).
inline std::atomic<int> offlineBlock{1024};

// Scheduler: how long an idle worker spins before it sleeps (microseconds).
inline std::atomic<int> spinUs{50};
// Scheduler: > 0, a worker with nothing to do sleeps until this long before the
// next run is due (runs come once per callback), then spins for it (microseconds).
inline std::atomic<int> preWakeUs{0};
// Scheduler workers ask for real-time priority (SCHED_FIFO) where there is no
// MMCSS (read when a Scheduler starts its workers).
inline std::atomic<bool> workerRealtime{false};

// VST3 hosting (while profile is on): time in the host's own part of a
// process() call (events, parameters, context, copies in and out) and in the
// plug-in's; output parameter values handed to the main thread, and dropped (queue full).
inline std::atomic<uint64_t> vst3WrapperNs{0};
inline std::atomic<uint64_t> vst3PluginNs{0};
inline std::atomic<uint64_t> vst3OutParams{0};
inline std::atomic<uint64_t> vst3OutParamsDropped{0};

}  // namespace sub::experiments
