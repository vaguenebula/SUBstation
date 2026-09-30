#include "Search.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "Text.h"

namespace gil::browser {

namespace {

constexpr uint32_t kCheckEvery = 4096;  // items between looks at whether the search is still wanted

bool contains(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    const char first = needle.front();
    const char* p = hay.data();
    const char* last = hay.data() + (hay.size() - needle.size());
    while (p <= last) {
        p = static_cast<const char*>(std::memchr(p, first, static_cast<size_t>(last - p) + 1));
        if (!p) return false;
        if (std::memcmp(p + 1, needle.data() + 1, needle.size() - 1) == 0) return true;
        ++p;
    }
    return false;
}

// (lower-case path + '\').startswith(prefix)
bool underFolder(std::string_view pathLower, std::string_view prefix) {
    if (prefix.size() <= pathLower.size()) return pathLower.starts_with(prefix);
    return prefix.size() == pathLower.size() + 1 && prefix.back() == '\\' && prefix.starts_with(pathLower);
}

struct Scored {
    Hit hit;
    double rank;
    int quality;
};

class Search {
public:
    Search(const Query& query, uint64_t generation, const SearchInputs& inputs, const std::atomic<uint64_t>& latest,
           UsageCache& cache)
        : query_(query), generation_(generation), inputs_(inputs), latest_(latest), cache_(cache) {
        terms_ = pySplit(pyLower(query.text));
        for (const auto& term : terms_) {
            if (!joined_.empty()) joined_ += ' ';
            joined_ += term;
        }
    }

    std::shared_ptr<Result> run() {
        const auto start = std::chrono::steady_clock::now();
        result_ = std::make_shared<Result>();
        result_->generation = generation_;
        result_->snapshot = inputs_.snapshot;
        if (!filter()) return nullptr;
        if (!(query_.sort == Sort::Name ? sortByName() : sortByRank())) return nullptr;
        result_->searchMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return std::move(result_);
    }

private:
    struct Range {
        int group;
        size_t begin, end;  // in result_->hits
    };

    bool cancelled() const { return latest_.load(std::memory_order_relaxed) != generation_; }

    const SnapFolder& folderOf(uint32_t audio) const {
        return inputs_.snapshot->folders[inputs_.snapshot->audio[audio].folder];
    }
    const ExternalItem& external(const Hit& hit) const { return result_->groups.at(static_cast<int>(hit.group))->items[hit.index]; }

    std::string_view lowerOf(const Hit& hit) const {
        if (hit.group == kAudioGroup) {
            const AudioRef ref = inputs_.snapshot->audio[hit.index];
            return inputs_.snapshot->folders[ref.folder].files->lower(ref.file);
        }
        return external(hit).lower;
    }

    std::string_view foldOf(const Hit& hit) const {
        if (hit.group == kAudioGroup) {
            const AudioRef ref = inputs_.snapshot->audio[hit.index];
            return inputs_.snapshot->folders[ref.folder].files->fold(ref.file);
        }
        return external(hit).fold;
    }

    bool filter() {
        auto& hits = result_->hits;
        for (const int group : query_.groups) {
            const size_t begin = hits.size();
            if (group == kAudioGroup) {
                if (!filterAudio()) return false;
            } else if (const auto it = inputs_.groups.find(group); it != inputs_.groups.end()) {
                result_->groups[group] = it->second;
                const auto& items = it->second->items;
                for (uint32_t i = 0; i < items.size(); ++i) {
                    const ExternalItem& item = items[i];
                    if (!query_.tag.empty() && item.tag != query_.tag) continue;
                    bool ok = true;
                    for (size_t t = 0; t < terms_.size() && ok; ++t)
                        ok = contains(item.lower, terms_[t]) || contains(item.detailLower, terms_[t]);
                    if (ok) hits.push_back({static_cast<uint32_t>(group), i});
                }
            }
            ranges_.push_back({group, begin, hits.size()});
        }
        return !cancelled();
    }

    bool filterAudio() {
        const Snapshot* snap = inputs_.snapshot.get();
        if (!snap) return true;
        const size_t folders = snap->folders.size();
        std::vector<uint8_t> folderOk(folders, 1);
        if (!query_.placePrefix.empty())
            for (size_t f = 0; f < folders; ++f) folderOk[f] = underFolder(snap->folders[f].pathLower, query_.placePrefix);
        // Whether each term is in each folder's detail: then its files all have it.
        std::vector<uint8_t> inDetail(terms_.size() * folders);
        for (size_t t = 0; t < terms_.size(); ++t)
            for (size_t f = 0; f < folders; ++f) inDetail[t * folders + f] = contains(snap->folders[f].detailLower, terms_[t]);

        auto& hits = result_->hits;
        hits.reserve(hits.size() + (terms_.empty() ? snap->audio.size() : snap->audio.size() / 8));
        const auto count = static_cast<uint32_t>(snap->audio.size());
        for (uint32_t i = 0; i < count; ++i) {
            if (i % kCheckEvery == 0 && cancelled()) return false;
            const AudioRef ref = snap->audio[i];
            if (!folderOk[ref.folder]) continue;
            bool ok = true;
            if (!terms_.empty()) {
                const std::string_view lower = snap->folders[ref.folder].files->lower(ref.file);
                for (size_t t = 0; t < terms_.size() && ok; ++t)
                    ok = inDetail[t * folders + ref.folder] || contains(lower, terms_[t]);
            }
            if (ok) hits.push_back({kAudioGroup, i});
        }
        return true;
    }

    // Stable by casefolded name over the whole list (groups one after another).
    bool sortByName() {
        auto& hits = result_->hits;
        std::vector<Hit> merged;
        std::vector<Hit> part;
        auto less = [this](const Hit& a, const Hit& b) { return foldOf(a) < foldOf(b); };
        for (const Range& range : ranges_) {
            part.assign(hits.begin() + static_cast<ptrdiff_t>(range.begin), hits.begin() + static_cast<ptrdiff_t>(range.end));
            if (range.group == kAudioGroup && !part.empty()) {
                // The snapshot has the files in casefold order already: keep the ones found.
                const Snapshot& snap = *inputs_.snapshot;
                std::vector<uint8_t> found(snap.audio.size());
                for (const Hit& hit : part) found[hit.index] = 1;
                part.clear();
                for (size_t i = 0; i < snap.byFold.size(); ++i) {
                    if (i % kCheckEvery == 0 && cancelled()) return false;
                    if (found[snap.byFold[i]]) part.push_back({kAudioGroup, snap.byFold[i]});
                }
            } else {
                std::stable_sort(part.begin(), part.end(), less);
            }
            if (merged.empty()) {
                merged.swap(part);
            } else if (!part.empty()) {
                std::vector<Hit> both;
                both.reserve(merged.size() + part.size());
                std::merge(merged.begin(), merged.end(), part.begin(), part.end(), std::back_inserter(both), less);
                merged.swap(both);
            }
            if (cancelled()) return false;
        }
        hits.swap(merged);
        return true;
    }

    void resolveAudioUsage() {
        const auto& snap = inputs_.snapshot;
        const auto& usage = inputs_.usage;
        if (cache_.snapshot == snap && cache_.usage == usage) return;
        cache_.snapshot = snap;
        cache_.usage = usage;
        cache_.anyAudio = false;
        cache_.recordOfAudio.assign(snap ? snap->audio.size() : 0, -1);
        if (!snap || !usage) return;
        // Position in `audio` of each folder's files.
        std::vector<uint32_t> folderStart(snap->folders.size() + 1, 0);
        for (size_t f = 0; f < snap->folders.size(); ++f)
            folderStart[f + 1] = folderStart[f] + static_cast<uint32_t>(snap->folders[f].files->size());
        std::vector<uint32_t> position;
        for (uint32_t r = 0; r < usage->records.size(); ++r) {
            // "audio:" + os.path.normcase(os.path.normpath(path)): the folder's key and the name.
            std::string_view key = usage->records[r].key;
            if (!key.starts_with("audio:")) continue;
            key.remove_prefix(6);
            const size_t cut = key.rfind('\\');
            if (cut == std::string_view::npos) continue;
            const std::string_view name = key.substr(cut + 1);
            auto folder = snap->folderByKey.find(key.substr(0, cut));
            if (folder == snap->folderByKey.end()) folder = snap->folderByKey.find(key.substr(0, cut + 1));  // "c:\"
            if (folder == snap->folderByKey.end()) continue;
            const FolderFiles& files = *snap->folders[folder->second].files;
            for (uint32_t i = 0; i < files.size(); ++i) {
                if (files.ntLower(i) != name) continue;
                if (position.empty()) {
                    position.resize(folderStart.back());
                    for (uint32_t p = 0; p < snap->audio.size(); ++p)
                        position[folderStart[snap->audio[p].folder] + snap->audio[p].file] = p;
                }
                cache_.recordOfAudio[position[folderStart[folder->second] + i]] = static_cast<int32_t>(r);
                cache_.anyAudio = true;
                break;
            }
        }
    }

    // Most used first, then (with a query) the best matches; stable.
    bool sortByRank() {
        auto& hits = result_->hits;
        const Usage* usage = inputs_.usage.get();
        std::vector<double> recordRank;
        if (usage) {
            recordRank.reserve(usage->records.size());
            for (const auto& record : usage->records) recordRank.push_back(usage->rank(record, query_.now));
            if (inputs_.snapshot) resolveAudioUsage();
        }
        auto rankOf = [&](const Hit& hit) -> double {
            if (!usage) return 0.0;
            if (hit.group == kAudioGroup) {
                const int32_t r = cache_.anyAudio ? cache_.recordOfAudio[hit.index] : -1;
                return r < 0 ? 0.0 : recordRank[static_cast<size_t>(r)];
            }
            const auto it = usage->byKey.find(external(hit).key);
            return it == usage->byKey.end() ? 0.0 : recordRank[it->second];
        };

        const bool scored = !terms_.empty();
        const int maxQuality = 8 + 3 * static_cast<int>(terms_.size());
        std::vector<Scored> ranked, unranked;  // rank above 0 / below 0
        std::vector<uint32_t> bucketSize(static_cast<size_t>(maxQuality) + 1, 0);
        std::vector<uint16_t> quality(scored ? hits.size() : 0);
        std::vector<uint8_t> zero(hits.size());
        std::vector<uint32_t> starts;
        for (size_t i = 0; i < hits.size(); ++i) {
            if (i % kCheckEvery == 0 && cancelled()) return false;
            const Hit hit = hits[i];
            const double rank = rankOf(hit);
            int q = 0;
            if (scored) {
                const bool audio = hit.group == kAudioGroup || external(hit).kind == Kind::Audio;
                q = matchQuality(lowerOf(hit), audio, terms_, joined_, starts);
                quality[i] = static_cast<uint16_t>(q);
            }
            if (rank > 0.0) {
                ranked.push_back({hit, rank, q});
            } else if (rank < 0.0) {
                unranked.push_back({hit, rank, q});
            } else {
                zero[i] = 1;
                ++bucketSize[static_cast<size_t>(q)];
            }
        }
        auto byRankThenQuality = [](const Scored& a, const Scored& b) {
            return a.rank != b.rank ? a.rank > b.rank : a.quality > b.quality;
        };
        std::stable_sort(ranked.begin(), ranked.end(), byRankThenQuality);
        std::stable_sort(unranked.begin(), unranked.end(), byRankThenQuality);

        std::vector<Hit> out;
        out.reserve(hits.size());
        for (const auto& s : ranked) out.push_back(s.hit);
        // The unused ones by quality, best first, keeping their order (a counting sort).
        std::vector<size_t> bucketStart(bucketSize.size(), 0);
        size_t at = out.size();
        for (int q = maxQuality; q >= 0; --q) {
            bucketStart[static_cast<size_t>(q)] = at;
            at += bucketSize[static_cast<size_t>(q)];
        }
        out.resize(at);
        for (size_t i = 0; i < hits.size(); ++i)
            if (zero[i]) out[bucketStart[scored ? quality[i] : 0]++] = hits[i];
        for (const auto& s : unranked) out.push_back(s.hit);
        hits.swap(out);
        return !cancelled();
    }

    const Query& query_;
    const uint64_t generation_;
    const SearchInputs& inputs_;
    const std::atomic<uint64_t>& latest_;
    UsageCache& cache_;
    std::vector<std::string> terms_;
    std::string joined_;
    std::shared_ptr<Result> result_;
    std::vector<Range> ranges_;
};

}  // namespace

int matchQuality(std::string_view lower, bool audio, const std::vector<std::string>& terms, std::string_view joined,
                 std::vector<uint32_t>& scratch) {
    std::string_view stem = lower;
    if (audio) {
        const size_t dot = lower.rfind('.');
        if (dot != std::string_view::npos) stem = lower.substr(0, dot);  // name.rsplit(".", 1)[0]
    }
    int quality = stem == joined ? 8 : 0;
    bool haveStarts = false;
    for (const auto& term : terms) {
        if (lower.starts_with(term)) {
            quality += 3;
            continue;
        }
        if (!haveStarts) {
            wordStarts(lower, scratch);
            haveStarts = true;
        }
        bool atWord = false;
        for (const uint32_t at : scratch) {
            if (lower.substr(at).starts_with(term)) {
                atWord = true;
                break;
            }
        }
        if (atWord)
            quality += 2;
        else if (contains(lower, term))
            quality += 1;
    }
    return quality;
}

std::shared_ptr<Result> runSearch(const Query& query, uint64_t generation, const SearchInputs& inputs,
                                  const std::atomic<uint64_t>& latest, UsageCache& cache) {
    return Search(query, generation, inputs, latest, cache).run();
}

}  // namespace gil::browser
