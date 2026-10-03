// What the browser searches: the audio files of the index (in immutable
// snapshots the indexer publishes), items given by the UI (built-in devices,
// plug-ins), and how often each was used. Everything a search reads is
// immutable and shared, so searches never lock against the indexer.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sub::browser {

enum class Kind : uint8_t { Audio = 0, Plugin = 1, Device = 2, Preset = 3 };

// The audio files of one folder, in the order the folder listed them.
class FolderFiles {
public:
    struct Name {
        std::string name;   // as on disk
        std::string lower;  // pyLower(name)
    };
    // `names` in listing order.
    static std::shared_ptr<const FolderFiles> make(std::vector<Name> names);

    size_t size() const { return files_.size(); }
    std::string_view name(size_t i) const { return view(files_[i].name, files_[i].nameLen); }
    std::string_view lower(size_t i) const { return view(files_[i].lower, files_[i].lowerLen); }
    std::string_view fold(size_t i) const { return view(files_[i].fold, files_[i].foldLen); }
    // Casefolding the name gives its lower case (true of every ASCII name).
    bool foldIsLower(size_t i) const { return files_[i].fold == files_[i].lower; }
    // Windows' lower case of the name, as in the item's key (os.path.normcase).
    std::string_view ntLower(size_t i) const { return view(files_[i].ntLower, files_[i].ntLowerLen); }
    bool sameNames(const std::vector<Name>& names) const;

private:
    struct File {
        uint32_t name, lower, fold, ntLower;
        uint16_t nameLen, lowerLen, foldLen, ntLowerLen;
    };
    std::string_view view(uint32_t at, uint16_t len) const { return {text_.data() + at, len}; }
    std::string text_;
    std::vector<File> files_;
};

// A folder as a snapshot shows it.
struct SnapFolder {
    std::shared_ptr<const FolderFiles> files;
    std::string path;         // as shown: the place's root as given, then the folder names
    std::string pathLower;    // pyLower(path), for the Places filter
    std::string key;          // os.path.normcase(path), for use counts
    std::string detail;       // what its files show as detail: the folder's name
    std::string detailLower;
};

struct AudioRef {
    uint32_t folder;
    uint32_t file;
};

// The index at one moment. Its maps point into it, so it is never copied.
struct Snapshot {
    Snapshot() = default;
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;

    uint64_t version = 0;
    std::vector<SnapFolder> folders;  // in the order the walk first reached them
    // The list's own order: by lower-case name, then walk order (the Python index
    // sorted its walk by name.lower(), which keeps the walk order for equal names).
    std::vector<AudioRef> audio;
    std::vector<uint32_t> byFold;  // positions in `audio` by casefolded name (ties: own order)
    std::unordered_map<std::string_view, uint32_t> folderByKey;
    std::unordered_map<std::string_view, uint32_t> folderByPath;

    // `folder.path` joined with a file name, as os.scandir joins them.
    static std::string join(std::string_view folder, std::string_view name);
};

// An item the UI lists besides the files (a built-in device, a plug-in, a preset).
struct ExternalItem {
    Kind kind = Kind::Plugin;
    std::string name, path, detail, key, tag;
    std::string lower, fold, detailLower;  // filled in by `prepare`
    void prepare();
};

struct ExternalGroup {
    std::vector<ExternalItem> items;
};

// How often items were used (library.py keeps the records; these are copies).
struct UsageRecord {
    std::string key;
    double score = 0.0;      // 0 when the record has none
    double lastUsed = 0.0;   // NaN when the record has none
};

struct Usage {
    Usage() = default;
    Usage(const Usage&) = delete;
    Usage& operator=(const Usage&) = delete;

    uint64_t version = 0;
    double halfLifeDays = 30.0;
    std::vector<UsageRecord> records;
    std::unordered_map<std::string_view, uint32_t> byKey;
    // library.Library.rank(): each use counts 1 when it happens, half of that every half-life after.
    double rank(const UsageRecord& record, double now) const;
};

enum class Sort : uint8_t { Rank, Name };

// Groups: 0 is the index's audio files, others are external groups.
inline constexpr int kAudioGroup = 0;

struct Query {
    std::string text;
    Sort sort = Sort::Rank;
    double now = 0.0;             // for how recent uses are
    std::vector<int> groups;      // what to list, in this order
    std::string tag;              // external items with this tag only ('' for all)
    std::string placePrefix;      // files under this folder only: its lower-case path and '\' ('' for all)
};

struct Hit {
    uint32_t group;
    uint32_t index;  // in the snapshot's `audio`, or the group's items
};

// A finished search: the items it found, in order, and what they point into.
struct Result {
    uint64_t generation = 0;
    std::shared_ptr<const Snapshot> snapshot;
    std::unordered_map<int, std::shared_ptr<const ExternalGroup>> groups;
    std::vector<Hit> hits;
    double searchMs = 0.0;

    // Position of an item: an audio file by its path, other items by key. -1 if absent.
    int64_t find(Kind kind, std::string_view identity) const;
};

}  // namespace sub::browser
