// The fingerprints saved between runs (sound-index.bin), so a library is
// analysed once: only files that are new or changed are analysed again.
//
// Format (little-endian):
//
//   "SUBSNDX1"                   8 bytes magic
//   u32 format                   1 (kStoreFormat)
//   u32 feature version          kFeatureVersion: another and the file is ignored
//   u32 dims                     kDims
//   u32 count
//   count x {
//     str path                   UTF-8 (WTF-8), the system's form
//     u64 size, u64 modified     the file's stamp when analysed
//     u8  flags                  1: analysed (else it couldn't be: not tried
//                                again until it changes), 2: a reference
//     u64 used                   (references only) when last searched from
//     f32 x dims                 (analysed only) the fingerprint
//   }
//   u64 FNV-1a                   of everything before it
//   str = u32 length + bytes
//
// Anything unexpected (another magic, format, feature version or size, a bad
// checksum, a truncated file) and the file is ignored: everything is analysed
// again.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/Platform.h"
#include "similarity/SoundFeatures.h"

namespace sub::intelligence {

inline constexpr uint32_t kStoreFormat = 1;

struct StoredSound {
    std::string path;
    platform::FileStamp stamp;
    bool analysed = false;
    bool reference = false;  // searched from, outside the library
    uint64_t used = 0;       // a reference's: when (a counter) it was last searched from
    Fingerprint fingerprint{};
};

// Builds a store's bytes, one sound at a time.
class StoreWriter {
public:
    explicit StoreWriter(uint32_t count);
    void add(const StoredSound& sound);
    // The bytes, with their checksum.
    std::string finish();

private:
    void u32(uint32_t v);
    void u64(uint64_t v);
    std::string bytes_;
};

// Writes the bytes to `file` (through `file`.tmp, moved over it). False on failure.
bool writeStore(const std::string& file, const std::string& bytes);

// The sounds saved in `file`; none if it is missing or not one this version reads.
std::optional<std::vector<StoredSound>> readStore(const std::string& file);

}  // namespace sub::intelligence
