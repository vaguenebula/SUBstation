#pragma once
// The file an export writes, as it renders: interleaved stereo float frames go
// in, the file's format comes out (EngineOffline.cpp's WAV writer; MP3 through
// LAME's encoder, Mp3Writer.cpp). Unless kept, the file is deleted when its
// writer goes: a render cancelled, or failed, leaves nothing behind.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include "platform/Paths.h"

namespace sub {

class AudioFileWriter {
public:
    virtual ~AudioFileWriter() = default;
    // Frames in [-1, 1] (louder ones are clipped by the file's format). A short
    // write (a full disk) throws: the file would be shorter than the render.
    virtual void write(const float* samples, int64_t frames) = 0;
    // Done: the file is finished and stays. Throws if it can't be finished.
    virtual void keep() = 0;

protected:
    explicit AudioFileWriter(std::string path) : path_(std::move(path)) {}

    // From a writer's destructor, once its file is closed: the file goes,
    // unless keep() finished it (and set kept_).
    void deleteUnlessKept() noexcept {
        if (kept_) return;
        std::error_code ignored;
        std::filesystem::remove(platform::toPath(path_), ignored);
    }

    const std::string path_;
    bool kept_ = false;
};

// Throws std::invalid_argument unless `kbps` is an MP3 bitrate (32 to 320).
void checkMp3Bitrate(int kbps);

// A new MP3 file at `path` (created now: throws std::runtime_error if it can't
// be), at a constant `kbps` (32 to 320), joint stereo, LAME's quality 2 (its
// -h). Rendered at up to 48 kHz it keeps the sample rate; above that it is
// resampled to 48 or 44.1 kHz (whichever divides it), MP3's highest. Its first
// frame is LAME's info tag (its length, the encoder's delay and padding, for
// gapless players).
std::unique_ptr<AudioFileWriter> makeMp3Writer(const std::string& path, double sampleRate, int kbps);

}  // namespace sub
