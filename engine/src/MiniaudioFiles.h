#pragma once
// miniaudio's decoders and encoders on files, by UTF-8 path. On Windows its
// wide calls (its narrow ones read the path in the ANSI code page); elsewhere
// its narrow ones (its wide ones convert through the C library's locale, which
// may not be UTF-8).

#include <string>

#include "miniaudio.h"
#include "platform/Paths.h"

namespace sub {

inline ma_result initDecoderFile(const std::string& path, const ma_decoder_config& config, ma_decoder* decoder) {
#ifdef _WIN32
    return ma_decoder_init_file_w(platform::toNative(path).c_str(), &config, decoder);
#else
    return ma_decoder_init_file(path.c_str(), &config, decoder);
#endif
}

inline ma_result initEncoderFile(const std::string& path, const ma_encoder_config& config, ma_encoder* encoder) {
#ifdef _WIN32
    return ma_encoder_init_file_w(platform::toNative(path).c_str(), &config, encoder);
#else
    return ma_encoder_init_file(path.c_str(), &config, encoder);
#endif
}

}  // namespace sub
