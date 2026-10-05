// Filtering and ordering, exactly as the browser's Python search did it:
//
//   An item matches when every word of the query is in its lower-case name or
//   detail (folder, vendor, category).
//   "rank": most used (and most recently) first, then the best name matches, then
//           the list's own order.
//   "name": by casefolded name, then the list's own order.
//
// Searches read immutable snapshots and stop early when a newer one is asked for.

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Model.h"

namespace sub::browser {

// How well the query's terms match a lower-case name: in the name beats in its
// detail, and at the start of the name or of a word beats the middle of one.
// `joined` is the terms joined by spaces. `scratch` is reused between calls.
int matchQuality(std::string_view lower, bool audio, const std::vector<std::string>& terms, std::string_view joined,
                 std::vector<uint32_t>& scratch);

// Which use record, if any, each file of a snapshot has. Kept between searches
// while the snapshot and the use counts stay the same.
struct UsageCache {
    std::shared_ptr<const Snapshot> snapshot;
    std::shared_ptr<const Usage> usage;
    std::vector<int32_t> recordOfAudio;  // by position in snapshot->audio; -1 for none
    bool anyAudio = false;
};

struct SearchInputs {
    std::shared_ptr<const Snapshot> snapshot;
    std::unordered_map<int, std::shared_ptr<const ExternalGroup>> groups;
    std::shared_ptr<const Usage> usage;
};

// Runs a query. Returns null if `latest` moved past `generation` meanwhile.
std::shared_ptr<Result> runSearch(const Query& query, uint64_t generation, const SearchInputs& inputs,
                                  const std::atomic<uint64_t>& latest, UsageCache& cache);

}  // namespace sub::browser
