#pragma once
// The parts of ASIO hosting that don't need the SDK: converting between the
// engine's float samples and the drivers' sample formats, and choosing buffer
// sizes. The numbers are ASIO's own (ASIOSampleType in asio.h).

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <vector>

namespace sub::asio {

enum SampleType : long {
    Int16MSB = 0,
    Int24MSB = 1,  // packed, 3 bytes
    Int32MSB = 2,
    Float32MSB = 3,
    Float64MSB = 4,
    Int32MSB16 = 8,  // 32-bit containers holding 16, 18, 20 or 24-bit samples in their low bits
    Int32MSB18 = 9,
    Int32MSB20 = 10,
    Int32MSB24 = 11,
    Int16LSB = 16,
    Int24LSB = 17,
    Int32LSB = 18,
    Float32LSB = 19,
    Float64LSB = 20,
    Int32LSB16 = 24,
    Int32LSB18 = 25,
    Int32LSB20 = 26,
    Int32LSB24 = 27,
};

struct SampleFormat {
    int bytes = 0;  // per sample; 0: not a format the engine plays (DSD)
    int bits = 0;   // significant bits (integer formats)
    bool isFloat = false;
    bool bigEndian = false;
};

inline SampleFormat sampleFormat(long type) noexcept {
    switch (type) {
        case Int16LSB: return {2, 16, false, false};
        case Int24LSB: return {3, 24, false, false};
        case Int32LSB: return {4, 32, false, false};
        case Float32LSB: return {4, 32, true, false};
        case Float64LSB: return {8, 64, true, false};
        case Int32LSB16: return {4, 16, false, false};
        case Int32LSB18: return {4, 18, false, false};
        case Int32LSB20: return {4, 20, false, false};
        case Int32LSB24: return {4, 24, false, false};
        case Int16MSB: return {2, 16, false, true};
        case Int24MSB: return {3, 24, false, true};
        case Int32MSB: return {4, 32, false, true};
        case Float32MSB: return {4, 32, true, true};
        case Float64MSB: return {8, 64, true, true};
        case Int32MSB16: return {4, 16, false, true};
        case Int32MSB18: return {4, 18, false, true};
        case Int32MSB20: return {4, 20, false, true};
        case Int32MSB24: return {4, 24, false, true};
        default: return {};
    }
}

namespace detail {

// Writes the low `bytes` bytes of `value` in the given byte order.
inline void storeBytes(uint8_t* dst, uint64_t value, int bytes, bool bigEndian) noexcept {
    for (int b = 0; b < bytes; ++b) dst[bigEndian ? bytes - 1 - b : b] = static_cast<uint8_t>(value >> (8 * b));
}

inline uint64_t loadBytes(const uint8_t* src, int bytes, bool bigEndian) noexcept {
    uint64_t value = 0;
    for (int b = 0; b < bytes; ++b) value |= static_cast<uint64_t>(src[bigEndian ? bytes - 1 - b : b]) << (8 * b);
    return value;
}

inline double clampSample(float x) noexcept {
    // NaN (from a broken plug-in) plays as silence. Tested first: NaN fails every
    // comparison below, so it would pass through them.
    if ((std::bit_cast<uint32_t>(x) & 0x7fffffffu) > 0x7f800000u) return 0.0;
    if (x > 1.f) return 1.0;
    if (x < -1.f) return -1.0;
    return x;
}

}  // namespace detail

// One channel's `frames` samples from the engine into a driver buffer.
inline void toDevice(long type, const float* src, void* dst, int frames) noexcept {
    const SampleFormat format = sampleFormat(type);
    auto* out = static_cast<uint8_t*>(dst);
    if (format.bytes == 0) return;
    if (format.isFloat) {
        for (int i = 0; i < frames; ++i, out += format.bytes) {
            uint64_t bits = 0;
            if (format.bytes == 4) {
                const float value = src[i];
                uint32_t word;
                std::memcpy(&word, &value, 4);
                bits = word;
            } else {
                const double value = src[i];
                std::memcpy(&bits, &value, 8);
            }
            detail::storeBytes(out, bits, format.bytes, format.bigEndian);
        }
        return;
    }
    const double scale = static_cast<double>((int64_t{1} << (format.bits - 1)) - 1);
    for (int i = 0; i < frames; ++i, out += format.bytes) {
        const double scaled = detail::clampSample(src[i]) * scale;
        const auto value = static_cast<int64_t>(scaled < 0.0 ? scaled - 0.5 : scaled + 0.5);
        detail::storeBytes(out, static_cast<uint64_t>(value), format.bytes, format.bigEndian);
    }
}

// One channel's `frames` samples from a driver buffer into the engine.
inline void fromDevice(long type, const void* src, float* dst, int frames) noexcept {
    const SampleFormat format = sampleFormat(type);
    const auto* in = static_cast<const uint8_t*>(src);
    if (format.bytes == 0) {
        std::fill_n(dst, frames, 0.f);
        return;
    }
    if (format.isFloat) {
        for (int i = 0; i < frames; ++i, in += format.bytes) {
            const uint64_t bits = detail::loadBytes(in, format.bytes, format.bigEndian);
            if (format.bytes == 4) {
                const auto word = static_cast<uint32_t>(bits);
                float value;
                std::memcpy(&value, &word, 4);
                dst[i] = value;
            } else {
                double value;
                std::memcpy(&value, &bits, 8);
                dst[i] = static_cast<float>(value);
            }
        }
        return;
    }
    const double scale = 1.0 / static_cast<double>(int64_t{1} << (format.bits - 1));
    const int unused = 64 - format.bits;  // the bits above the sample, whatever the driver left in them
    for (int i = 0; i < frames; ++i, in += format.bytes) {
        const uint64_t raw = detail::loadBytes(in, format.bytes, format.bigEndian);
        const int64_t value = static_cast<int64_t>(raw << unused) >> unused;  // sign-extended
        dst[i] = static_cast<float>(static_cast<double>(value) * scale);
    }
}

// The buffer sizes to offer, from what a driver's getBufferSize() reports.
// Granularity -1 means powers of two between the limits; 0, only the preferred size.
inline std::vector<uint32_t> bufferSizeOptions(long minSize, long maxSize, long preferred, long granularity) {
    std::vector<uint32_t> sizes;
    const auto add = [&](long size) {
        if (size > 0 && size >= minSize && size <= maxSize) sizes.push_back(static_cast<uint32_t>(size));
    };
    if (granularity == -1) {
        for (long size = 1; size > 0 && size <= maxSize; size *= 2) add(size);
    } else if (granularity > 0) {
        // Any step of the granularity would do; these are the ones worth listing.
        constexpr long kCommon[] = {16,  24,  32,  48,  64,   96,   128,  160,  192,  256,  320,  384,
                                    448, 512, 640, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192, 16384};
        for (const long size : kCommon) {
            if ((size - minSize) % granularity == 0) add(size);
        }
        add(minSize);
        add(maxSize);
    }
    add(preferred);
    if (sizes.empty() && preferred > 0) sizes.push_back(static_cast<uint32_t>(preferred));  // a driver out of spec
    std::sort(sizes.begin(), sizes.end());
    sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
    return sizes;
}

// The size to ask the driver for: `requested` if it takes it, else the nearest
// one it offers (the larger on a tie); 0 asks for its preferred size.
inline long chooseBufferSize(uint32_t requested, long minSize, long maxSize, long preferred, long granularity) {
    const auto want = static_cast<long>(requested);
    if (want <= 0) return preferred;
    const bool inRange = want >= minSize && want <= maxSize;
    if (inRange && granularity == -1 && (want & (want - 1)) == 0) return want;
    if (inRange && granularity > 0 && (want - minSize) % granularity == 0) return want;
    if (want == preferred) return want;
    const std::vector<uint32_t> sizes = bufferSizeOptions(minSize, maxSize, preferred, granularity);
    if (sizes.empty()) return preferred;
    long best = static_cast<long>(sizes.front());
    for (const uint32_t size : sizes) {
        const long candidate = static_cast<long>(size);
        if (std::labs(candidate - want) <= std::labs(best - want)) best = candidate;
    }
    return best;
}

}  // namespace sub::asio
