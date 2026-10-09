// FNV-1a (64-bit): the hash of saved fingerprints' checksums and keys
// (SoundStore.h, FeatureSchema::key) and of paths in search results.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sub::intelligence {

inline constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
inline constexpr uint64_t kFnvPrime = 1099511628211ull;

// Of `size` bytes, continuing from `hash` (to hash several pieces as one).
inline uint64_t fnv1a(const void* data, size_t size, uint64_t hash = kFnvOffsetBasis) {
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

}  // namespace sub::intelligence
