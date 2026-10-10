// Binary files' fields and their checksum, the same on every system: numbers
// little-endian whatever the CPU's order, so a file written on one system
// reads on any. The browser's saved index and the sound similarity's store are
// made of these. (On little-endian CPUs, all of today's, a number is copied as
// it is; only a big-endian one swaps its bytes.)

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace sub::platform {

// FNV-1a (64-bit) of `size` bytes, continuing from `hash` (kFnvOffsetBasis to
// start; another hash to hash several pieces as one): the files' checksums, and
// a hash of keys and paths. (The hash has no default here, so that
// fnv1a("text", hash) can only be the text's.)
inline constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
inline constexpr uint64_t kFnvPrime = 1099511628211ull;

inline uint64_t fnv1a(const void* data, size_t size, uint64_t hash) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

inline uint64_t fnv1a(std::string_view text, uint64_t hash = kFnvOffsetBasis) {
    return fnv1a(text.data(), text.size(), hash);
}

// A number with its bytes in the other order (std::byteswap is C++23).
template <typename T>
constexpr T byteSwapped(T v) {
    T out = 0;
    for (size_t i = 0; i < sizeof(T); ++i) out = static_cast<T>((out << 8) | ((v >> (8 * i)) & 0xFF));
    return out;
}

// Appends fields to `bytes`.
class ByteWriter {
public:
    std::string bytes;

    void raw(const void* data, size_t size) { bytes.append(static_cast<const char*>(data), size); }
    void u8(uint8_t v) { bytes.push_back(static_cast<char>(v)); }
    void u32(uint32_t v) { number(v); }
    void u64(uint64_t v) { number(v); }
    void f32(float v) { number(std::bit_cast<uint32_t>(v)); }
    // Its length (u32), then its bytes.
    void str(std::string_view s) {
        u32(static_cast<uint32_t>(s.size()));
        bytes.append(s);
    }

private:
    template <typename T>
    void number(T v) {
        if constexpr (std::endian::native == std::endian::big) v = byteSwapped(v);
        bytes.append(reinterpret_cast<const char*>(&v), sizeof v);
    }
};

// Reads fields from bytes. A field past the end reads as zero (or empty) and
// makes ok() false for good: read a record's fields, then check once.
class ByteReader {
public:
    explicit ByteReader(std::string_view data) : p_(data.data()), end_(data.data() + data.size()) {}

    bool ok() const { return ok_; }
    bool atEnd() const { return p_ == end_; }

    bool raw(void* out, size_t size) {
        if (!ok_ || static_cast<size_t>(end_ - p_) < size) {
            ok_ = false;
            return false;
        }
        std::memcpy(out, p_, size);
        p_ += size;
        return true;
    }
    uint8_t u8() {
        unsigned char b = 0;
        raw(&b, 1);
        return b;
    }
    uint32_t u32() { return number<uint32_t>(); }
    uint64_t u64() { return number<uint64_t>(); }
    float f32() { return std::bit_cast<float>(number<uint32_t>()); }
    std::string str() {
        const uint32_t size = u32();
        if (!ok_ || static_cast<size_t>(end_ - p_) < size) {
            ok_ = false;
            return {};
        }
        std::string s(p_, size);
        p_ += size;
        return s;
    }

private:
    template <typename T>
    T number() {
        T v = 0;
        if (raw(&v, sizeof v) && std::endian::native == std::endian::big) v = byteSwapped(v);
        return v;
    }

    const char* p_;
    const char* end_;
    bool ok_ = true;
};

}  // namespace sub::platform
