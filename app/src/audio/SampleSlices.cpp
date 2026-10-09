#include "audio/SampleSlices.h"

#include "builtin/SampleSlicing.h"

#include <algorithm>
#include <array>
#include <iterator>

namespace sub::app::sampleSlices {
namespace {

// The waveform's first two channels (one twice for a mono file), as the Sampler reads them.
struct Channels {
    std::array<const float*, 2> data{};
    int count = 0;

    explicit Channels(const Waveform& waveform) {
        count = std::min(2, waveform.channels());
        data[0] = waveform.channelData(0);
        data[1] = count > 1 ? waveform.channelData(1) : data[0];
    }
};

}  // namespace

std::vector<Transient> transients(const Waveform& waveform, bool reversed) {
    if (waveform.isNull() || waveform.frames() <= 0)
        return {};
    const Channels channels(waveform);
    const std::vector<sub::slicing::Onset> onsets = sub::slicing::detectOnsets(
        channels.data.data(), channels.count, waveform.frames(), waveform.sampleRate(), reversed);
    std::vector<Transient> result;
    result.reserve(onsets.size());
    for (const sub::slicing::Onset& onset : onsets)
        result.push_back({onset.frame, onset.strength});
    return result;
}

std::vector<qint64> sliceStarts(const Settings& settings, const std::vector<Transient>& transients, qint64 start,
                                qint64 end, double sampleRate) {
    std::vector<sub::slicing::Onset> onsets;
    onsets.reserve(transients.size());
    for (const Transient& transient : transients)
        onsets.push_back({transient.frame, transient.strength});
    sub::slicing::SliceSettings engine;
    engine.by = static_cast<sub::slicing::SliceBy>(settings.by);
    engine.sensitivity = static_cast<float>(settings.sensitivity);
    engine.regionBeats = settings.regionBeats;
    engine.divisionBeats = settings.divisionBeats;
    engine.regions = settings.regions;
    std::array<int64_t, sub::slicing::kMaxSlices> starts{};
    const int count =
        sub::slicing::sliceStarts(engine, onsets.data(), onsets.size(), start, end, sampleRate, starts.data());
    return {starts.begin(), starts.begin() + count};
}

double divisionBeats(int index) {
    constexpr auto count = static_cast<int>(std::size(sub::slicing::kSliceDivisionBeats));
    return sub::slicing::kSliceDivisionBeats[std::clamp(index, 0, count - 1)];
}

int firstSliceKey() { return sub::slicing::kFirstSliceKey; }

int maxSlices() { return sub::slicing::kMaxSlices; }

qint64 nearestZeroCrossing(const Waveform& waveform, bool reversed, qint64 frame) {
    if (waveform.isNull())
        return frame;
    const Channels channels(waveform);
    const auto reach = static_cast<int64_t>(sub::slicing::kSnapSeconds * waveform.sampleRate());
    return sub::slicing::nearestZeroCrossing(channels.data.data(), channels.count, waveform.frames(), reversed, frame,
                                             reach);
}

std::vector<qint64> snapSliceStarts(const Waveform& waveform, bool reversed, std::vector<qint64> starts, qint64 end) {
    const auto snap = [&](qint64 frame) { return nearestZeroCrossing(waveform, reversed, frame); };
    const int count = sub::slicing::snapSliceStarts(starts.data(), static_cast<int>(starts.size()), end, snap);
    starts.resize(static_cast<std::size_t>(count));
    return starts;
}

}  // namespace sub::app::sampleSlices
