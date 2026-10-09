// An MP3 file being written by an export, through LAME's encoder
// (third_party/lame: LGPL, compiled into the engine as C).
#include "AudioFileWriter.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "platform/Paths.h"
#include "lame.h"

namespace sub {
namespace {

class Mp3Writer final : public AudioFileWriter {
public:
    Mp3Writer(std::string path, double sampleRate, int kbps) : path_(std::move(path)) {
        if (kbps < 32 || kbps > 320) throw std::invalid_argument("MP3 bitrate must be 32 to 320 kbps");
        const int rate = static_cast<int>(std::lround(sampleRate));
        lame_ = lame_init();
        if (lame_ == nullptr) throw std::runtime_error("Could not start the MP3 encoder");
        lame_set_in_samplerate(lame_, rate);
        if (rate > 48000) lame_set_out_samplerate(lame_, rate % 48000 == 0 ? 48000 : 44100);
        lame_set_num_channels(lame_, 2);
        lame_set_mode(lame_, JOINT_STEREO);
        lame_set_VBR(lame_, vbr_off);
        lame_set_brate(lame_, kbps);
        lame_set_quality(lame_, 2);
        lame_set_write_id3tag_automatic(lame_, 0);  // (no tags: the info tag is the first frame)
        lame_set_bWriteVbrTag(lame_, 1);
        if (lame_init_params(lame_) < 0) {
            lame_close(lame_);
            throw std::runtime_error("The MP3 encoder can't encode at " + std::to_string(rate) + " Hz, " +
                                     std::to_string(kbps) + " kbps");
        }
        file_.open(platform::toPath(path_), std::ios::binary | std::ios::trunc);
        if (!file_) {
            lame_close(lame_);
            throw std::runtime_error("Could not create " + path_);
        }
    }

    ~Mp3Writer() override {
        lame_close(lame_);
        if (file_.is_open()) file_.close();
        if (!kept_) {
            std::error_code ignored;
            std::filesystem::remove(platform::toPath(path_), ignored);
        }
    }

    Mp3Writer(const Mp3Writer&) = delete;
    Mp3Writer& operator=(const Mp3Writer&) = delete;

    void write(const float* samples, int64_t frames) override {
        constexpr int64_t kMost = 1 << 16;  // (LAME takes an int of frames at a time)
        for (int64_t done = 0; done < frames;) {
            const int n = static_cast<int>(std::min(kMost, frames - done));
            // LAME's worst case: 1.25 times the frames, and 7200 bytes.
            encoded_.resize(static_cast<size_t>(n) * 5 / 4 + 7200);
            const int bytes = lame_encode_buffer_interleaved_ieee_float(lame_, samples + done * 2, n, encoded_.data(),
                                                                        static_cast<int>(encoded_.size()));
            if (bytes < 0) throw std::runtime_error("The MP3 encoder failed (" + std::to_string(bytes) + ")");
            put(bytes);
            done += n;
        }
    }

    void keep() override {
        encoded_.resize(7200);
        const int bytes = lame_encode_flush(lame_, encoded_.data(), static_cast<int>(encoded_.size()));
        if (bytes < 0) throw std::runtime_error("The MP3 encoder failed (" + std::to_string(bytes) + ")");
        put(bytes);
        // The info tag, now that the length is known, over the first frame (written empty).
        unsigned char tag[2880];
        const size_t tagBytes = lame_get_lametag_frame(lame_, tag, sizeof tag);
        if (tagBytes > 0 && tagBytes <= sizeof tag) {
            file_.seekp(0);
            file_.write(reinterpret_cast<const char*>(tag), static_cast<std::streamsize>(tagBytes));
        }
        file_.close();
        if (file_.fail()) throw std::runtime_error("Could not write " + path_);
        kept_ = true;
    }

private:
    void put(int bytes) {
        if (bytes == 0) return;
        file_.write(reinterpret_cast<const char*>(encoded_.data()), bytes);
        if (!file_) throw std::runtime_error("Could not write " + path_);
    }

    std::string path_;
    lame_global_flags* lame_ = nullptr;
    std::ofstream file_;
    std::vector<unsigned char> encoded_;
    bool kept_ = false;
};

}  // namespace

std::unique_ptr<AudioFileWriter> makeMp3Writer(const std::string& path, double sampleRate, int kbps) {
    return std::make_unique<Mp3Writer>(path, sampleRate, kbps);
}

}  // namespace sub
