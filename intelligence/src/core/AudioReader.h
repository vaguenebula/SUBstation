// Reading audio files for analysis: decoded with miniaudio (WAV, FLAC, MP3),
// mixed down to mono, at the file's own rate up to 48 kHz (above that,
// resampled to 48 kHz). Only as much as an analysis needs is decoded.

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

}  // namespace sub::intelligence
