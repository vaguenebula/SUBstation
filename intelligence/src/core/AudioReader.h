// Reading audio files for analysis: decoded with miniaudio (WAV, FLAC, MP3),
// mixed down to mono, at the file's own rate up to 48 kHz (above that,
// resampled to 48 kHz), or at exactly the rate an extractor analyses at. Only
// as much as an analysis needs is decoded.

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace sub::intelligence {

struct AudioError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct MonoAudio {
    std::vector<float> samples;  // the channels' mean
    uint32_t sampleRate = 0;
    // The whole file's length in seconds, where the format tells it without
    // decoding everything (WAV, FLAC); otherwise what was decoded (MP3), and
    // `truncated` says whether there is more.
    double fileSeconds = 0.0;
    bool truncated = false;
};

// Decodes up to `maxSeconds` of `path` (UTF-8, the system's form) from
// `startSeconds` in. Throws AudioError if the file can't be read or decoded.
MonoAudio readMono(const std::string& path, double startSeconds, double maxSeconds, uint32_t maxSampleRate = 48000);

// The same, at exactly `sampleRate` (decoded at the file's own rate, then
// resampled with resampleMono()).
MonoAudio readMonoAt(const std::string& path, double startSeconds, double maxSeconds, uint32_t sampleRate);

// Mono samples at `from` Hz, resampled to `to` Hz: band-limited interpolation,
// a Kaiser-windowed sinc (16 zero crossings either side, flat to 97% of the
// lower rate's Nyquist frequency, about -90 dB past it). No delay: the sound
// starts where it did. (miniaudio's resampler is linear, which takes a few dB
// off the top octave: a 48 kHz copy of a sound would analyse darker than a
// 44.1 kHz one.) `from` == `to` copies them.
std::vector<float> resampleMono(const float* samples, size_t count, uint32_t from, uint32_t to);

}  // namespace sub::intelligence
