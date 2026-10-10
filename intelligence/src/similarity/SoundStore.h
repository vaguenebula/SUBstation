// The fingerprints saved between runs (sound-index.bin), so a library is
// analysed once: only files that are new or changed are analysed again.
//
// A store holds one extractor's fingerprints, as it made them: the header has
// its schema's key (FeatureSchema::key: its name, version, settings and
// features). Fingerprints are used only by the same extractor, as it is: any
// difference (another extractor, another version, other settings) and they are
// all made again. A file is told by its path and its stamp (size and last-write
// time): one whose stamp changed is analysed again.
//
// Format (little-endian):
//
//   "SUBSNDX1"                   8 bytes magic
//   u32 format                   3 (kStoreFormat)
//   u64 key                      FeatureSchema::key()
//   u32 dims                     features per fingerprint
//   u32 count
//   count x {
//     str path                   UTF-8 (WTF-8), the system's form
//     u64 size, u64 modified     the file's stamp when analysed
//     i64 seen                   when it was last in the library (seconds since
//                                1970; 0: never, a reference only)
//     u8  flags                  1: analysed (else it couldn't be: not tried
//                                again until it changes), 2: a reference
//     u64 used                   (references only) when last searched from
//     f32 x dims                 (analysed only) the fingerprint
//   }
//   u8  statistics               1 if the library's statistics follow
//   u64 measured; f32 x dims spread   (FeatureStatistics)
//   u64 FNV-1a                   of everything before it
//   str = u32 length + bytes
//
// Anything unexpected (another magic, format or schema, a bad checksum, a
// truncated file) and the file is ignored: everything is analysed again.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "platform/Bytes.h"
#include "platform/Files.h"
#include "similarity/FeatureSchema.h"
#include "similarity/Similarity.h"

namespace sub::intelligence {

inline constexpr uint32_t kStoreFormat = 3;

struct StoredSound {
    std::string path;
    platform::FileStamp stamp;
    bool analysed = false;
    bool reference = false;  // searched from, outside the library
    uint64_t used = 0;       // a reference's: when (a counter) it was last searched from
    int64_t seen = 0;        // when it was last in the library (seconds since 1970; 0: never)
    std::vector<float> fingerprint;  // dims, if analysed
};

struct StoreContents {
    std::vector<StoredSound> sounds;
    std::optional<FeatureStatistics> statistics;  // the library's, when saved
};

// Builds a store's bytes, one sound at a time.
class StoreWriter {
public:
    StoreWriter(const FeatureSchema& schema, uint32_t count);
    // `fingerprint`: schema.dims() floats, if the sound is analysed (throws
    // std::invalid_argument if it is null).
    void add(const StoredSound& sound, const float* fingerprint);
    // Its own fingerprint (throws std::invalid_argument if it is analysed and
    // that isn't schema.dims() floats).
    void add(const StoredSound& sound);
    // The bytes, with the statistics (if any) and the checksum.
    std::string finish(const FeatureStatistics* statistics = nullptr);

private:
    size_t dims_;
    platform::ByteWriter out_;
};

// Writes the bytes to `file` (through `file`.tmp, moved over it). False on failure.
bool writeStore(const std::string& file, const std::string& bytes);

// What `file` holds, if it is a store of this format made by `schema`'s
// extractor as it is; none otherwise (or if it is missing).
std::optional<StoreContents> readStore(const std::string& file, const FeatureSchema& schema);

}  // namespace sub::intelligence
