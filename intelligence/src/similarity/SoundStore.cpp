#include "similarity/SoundStore.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "platform/Bytes.h"

namespace sub::intelligence {

namespace {

constexpr char kMagic[8] = {'S', 'U', 'B', 'S', 'N', 'D', 'X', '1'};
constexpr uint8_t kAnalysed = 1, kReference = 2;

}  // namespace

StoreWriter::StoreWriter(const FeatureSchema& schema, uint32_t count) : dims_(schema.dims()) {
    out_.raw(kMagic, sizeof(kMagic));
    out_.u32(kStoreFormat);
    out_.u64(schema.key());
    out_.u32(static_cast<uint32_t>(dims_));
    out_.u32(count);
}

void StoreWriter::add(const StoredSound& sound) {
    if (sound.analysed && sound.fingerprint.size() != dims_)
        throw std::invalid_argument("a fingerprint of " + std::to_string(sound.fingerprint.size()) +
                                    " features where the schema has " + std::to_string(dims_));
    add(sound, sound.fingerprint.data());
}

void StoreWriter::add(const StoredSound& sound, const float* fingerprint) {
    if (sound.analysed && !fingerprint && dims_ > 0) throw std::invalid_argument("an analysed sound without a fingerprint");
    out_.str(sound.path);
    out_.u64(sound.stamp.size);
    out_.u64(sound.stamp.modified);
    out_.u64(static_cast<uint64_t>(sound.seen));
    const uint8_t flags = (sound.analysed ? kAnalysed : 0) | (sound.reference ? kReference : 0);
    out_.u8(flags);
    if (sound.reference) out_.u64(sound.used);
    if (sound.analysed)
        for (size_t d = 0; d < dims_; ++d) out_.f32(fingerprint[d]);
}

std::string StoreWriter::finish(const FeatureStatistics* statistics) {
    const bool withStatistics = statistics && statistics->spread.size() == dims_;
    out_.u8(withStatistics ? 1 : 0);
    if (withStatistics) {
        out_.u64(statistics->count);
        for (const float v : statistics->spread) out_.f32(v);
    }
    out_.u64(platform::fnv1a(out_.bytes));
    return std::move(out_.bytes);
}

bool writeStore(const std::string& file, const std::string& bytes) { return platform::writeFileAtomically(file, bytes); }

std::optional<StoreContents> readStore(const std::string& file, const FeatureSchema& schema) {
    const std::optional<std::string> read = platform::readFile(file);
    if (!read) return std::nullopt;
    const std::string& data = *read;
    if (data.size() < sizeof(kMagic) + 8) return std::nullopt;
    const std::string_view body(data.data(), data.size() - 8);
    if (platform::ByteReader(std::string_view(data).substr(body.size())).u64() != platform::fnv1a(body)) return std::nullopt;

    platform::ByteReader in(body);
    char magic[sizeof(kMagic)];
    if (!in.raw(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) return std::nullopt;
    const uint32_t format = in.u32();
    const uint64_t key = in.u64();
    const uint32_t dims = in.u32();
    const uint32_t count = in.u32();
    if (!in.ok() || format != kStoreFormat || key != schema.key() || dims != schema.dims()) return std::nullopt;
    StoreContents contents;
    contents.sounds.reserve(std::min<size_t>(count, data.size() / 32));
    for (uint32_t i = 0; i < count; ++i) {
        StoredSound s;
        s.path = in.str();
        s.stamp.size = in.u64();
        s.stamp.modified = in.u64();
        s.seen = static_cast<int64_t>(in.u64());
        const uint8_t flags = in.u8();
        if (!in.ok()) return std::nullopt;
        s.analysed = flags & kAnalysed;
        s.reference = flags & kReference;
        if (s.reference) s.used = in.u64();
        if (s.analysed) {
            s.fingerprint.resize(dims);
            for (float& v : s.fingerprint) v = in.f32();
        }
        if (!in.ok()) return std::nullopt;
        contents.sounds.push_back(std::move(s));
    }
    if (in.u8()) {  // the statistics
        FeatureStatistics st;
        st.spread.resize(dims);
        st.count = in.u64();
        for (float& v : st.spread) {
            v = in.f32();
            if (!in.ok() || !(v > 0.f) || !std::isfinite(v)) return std::nullopt;
        }
        contents.statistics = std::move(st);
    }
    if (!in.ok() || !in.atEnd()) return std::nullopt;
    return contents;
}

}  // namespace sub::intelligence
