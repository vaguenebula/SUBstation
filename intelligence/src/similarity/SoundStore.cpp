#include "similarity/SoundStore.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>

#include "core/Hash.h"

namespace sub::intelligence {

namespace {

constexpr char kMagic[8] = {'S', 'U', 'B', 'S', 'N', 'D', 'X', '1'};
constexpr uint8_t kAnalysed = 1, kReference = 2;

struct FileCloser {
    void operator()(std::FILE* f) const {
        if (f) std::fclose(f);
    }
};
using File = std::unique_ptr<std::FILE, FileCloser>;

class Reader {
public:
    Reader(const char* data, size_t size) : p_(data), end_(data + size) {}
    bool bytes(void* out, size_t n) {
        if (static_cast<size_t>(end_ - p_) < n) return false;
        std::memcpy(out, p_, n);
        p_ += n;
        return true;
    }
    bool u8(uint8_t& v) { return bytes(&v, 1); }
    bool u32(uint32_t& v) {
        unsigned char b[4];
        if (!bytes(b, 4)) return false;
        v = b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<uint32_t>(b[3]) << 24);
        return true;
    }
    bool u64(uint64_t& v) {
        uint32_t lo = 0, hi = 0;
        if (!u32(lo) || !u32(hi)) return false;
        v = (static_cast<uint64_t>(hi) << 32) | lo;
        return true;
    }
    bool str(std::string& s) {
        uint32_t n = 0;
        if (!u32(n) || static_cast<size_t>(end_ - p_) < n) return false;
        s.assign(p_, n);
        p_ += n;
        return true;
    }
    bool f32(float& v) {
        uint32_t bits = 0;
        if (!u32(bits)) return false;
        std::memcpy(&v, &bits, 4);
        return true;
    }

    bool atEnd() const { return p_ == end_; }

private:
    const char* p_;
    const char* end_;
};

}  // namespace

StoreWriter::StoreWriter(const FeatureSchema& schema, uint32_t count) : dims_(schema.dims()) {
    bytes_.append(kMagic, sizeof(kMagic));
    u32(kStoreFormat);
    u64(schema.key());
    u32(static_cast<uint32_t>(dims_));
    u32(count);
}

void StoreWriter::u32(uint32_t v) {
    const char b[4] = {static_cast<char>(v), static_cast<char>(v >> 8), static_cast<char>(v >> 16),
                       static_cast<char>(v >> 24)};
    bytes_.append(b, 4);
}

void StoreWriter::u64(uint64_t v) {
    u32(static_cast<uint32_t>(v));
    u32(static_cast<uint32_t>(v >> 32));
}

void StoreWriter::f32(float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, 4);
    u32(bits);
}

void StoreWriter::str(const std::string& s) {
    u32(static_cast<uint32_t>(s.size()));
    bytes_ += s;
}

void StoreWriter::add(const StoredSound& sound) {
    if (sound.analysed && sound.fingerprint.size() != dims_)
        throw std::invalid_argument("a fingerprint of " + std::to_string(sound.fingerprint.size()) +
                                    " features where the schema has " + std::to_string(dims_));
    add(sound, sound.fingerprint.data());
}

void StoreWriter::add(const StoredSound& sound, const float* fingerprint) {
    if (sound.analysed && !fingerprint && dims_ > 0) throw std::invalid_argument("an analysed sound without a fingerprint");
    str(sound.path);
    u64(sound.stamp.size);
    u64(sound.stamp.modified);
    u64(static_cast<uint64_t>(sound.seen));
    const uint8_t flags = (sound.analysed ? kAnalysed : 0) | (sound.reference ? kReference : 0);
    bytes_.push_back(static_cast<char>(flags));
    if (sound.reference) u64(sound.used);
    if (sound.analysed)
        for (size_t d = 0; d < dims_; ++d) f32(fingerprint[d]);
}

std::string StoreWriter::finish(const FeatureStatistics* statistics) {
    const bool withStatistics = statistics && statistics->spread.size() == dims_;
    bytes_.push_back(static_cast<char>(withStatistics ? 1 : 0));
    if (withStatistics) {
        u64(statistics->count);
        for (const float v : statistics->spread) f32(v);
    }
    u64(fnv1a(bytes_.data(), bytes_.size()));
    return std::move(bytes_);
}

bool writeStore(const std::string& file, const std::string& bytes) {
    const std::string temporary = file + ".tmp";
    {
        File f(platform::openFile(temporary, true));
        if (!f) return false;
        if (std::fwrite(bytes.data(), 1, bytes.size(), f.get()) != bytes.size()) return false;
        if (std::fflush(f.get()) != 0) return false;
    }
    return platform::replaceFile(temporary, file);
}

std::optional<StoreContents> readStore(const std::string& file, const FeatureSchema& schema) {
    std::string data;
    {
        File f(platform::openFile(file, false));
        if (!f) return std::nullopt;
        char chunk[1 << 16];
        size_t n;
        while ((n = std::fread(chunk, 1, sizeof(chunk), f.get())) > 0) data.append(chunk, n);
    }
    if (data.size() < sizeof(kMagic) + 8) return std::nullopt;
    uint64_t sum = 0;
    Reader tail(data.data() + data.size() - 8, 8);
    tail.u64(sum);
    if (sum != fnv1a(data.data(), data.size() - 8)) return std::nullopt;

    Reader in(data.data(), data.size() - 8);
    char magic[sizeof(kMagic)];
    uint32_t format = 0, dims = 0, count = 0;
    uint64_t key = 0;
    if (!in.bytes(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) return std::nullopt;
    if (!in.u32(format) || format != kStoreFormat || !in.u64(key) || key != schema.key() || !in.u32(dims) ||
        dims != schema.dims() || !in.u32(count))
        return std::nullopt;
    StoreContents contents;
    contents.sounds.reserve(std::min<size_t>(count, data.size() / 32));
    for (uint32_t i = 0; i < count; ++i) {
        StoredSound s;
        uint8_t flags = 0;
        uint64_t seen = 0;
        if (!in.str(s.path) || !in.u64(s.stamp.size) || !in.u64(s.stamp.modified) || !in.u64(seen) || !in.u8(flags))
            return std::nullopt;
        s.seen = static_cast<int64_t>(seen);
        s.analysed = flags & kAnalysed;
        s.reference = flags & kReference;
        if (s.reference && !in.u64(s.used)) return std::nullopt;
        if (s.analysed) {
            s.fingerprint.resize(dims);
            for (float& v : s.fingerprint)
                if (!in.f32(v)) return std::nullopt;
        }
        contents.sounds.push_back(std::move(s));
    }
    uint8_t withStatistics = 0;
    if (!in.u8(withStatistics)) return std::nullopt;
    if (withStatistics) {
        FeatureStatistics st;
        st.spread.resize(dims);
        if (!in.u64(st.count)) return std::nullopt;
        for (float& v : st.spread)
            if (!in.f32(v) || !(v > 0.f) || !std::isfinite(v)) return std::nullopt;
        contents.statistics = std::move(st);
    }
    if (!in.atEnd()) return std::nullopt;
    return contents;
}

}  // namespace sub::intelligence
