#include "Model.h"

#include <cmath>

#include "Platform.h"
#include "Text.h"

namespace sub::browser {

std::shared_ptr<const FolderFiles> FolderFiles::make(std::vector<Name> names) {
    auto out = std::shared_ptr<FolderFiles>(new FolderFiles());
    out->files_.reserve(names.size());
    size_t bytes = 0;
    for (const auto& n : names) bytes += n.name.size() + n.lower.size();
    out->text_.reserve(bytes);
    auto add = [&](std::string_view s) {
        const auto at = static_cast<uint32_t>(out->text_.size());
        out->text_.append(s);
        return at;
    };
    for (auto& n : names) {
        File f{};
        f.name = add(n.name);
        f.nameLen = static_cast<uint16_t>(n.name.size());
        f.lower = n.lower == n.name ? f.name : add(n.lower);
        f.lowerLen = static_cast<uint16_t>(n.lower.size());
        f.fold = f.lower;  // the same for ASCII names
        f.foldLen = f.lowerLen;
        const bool ascii = isAscii(n.name);
        if (!ascii) {
            // str.casefold() of the name (not of its lower case), as the Name sort used.
            const std::string fold = pyCasefold(n.name);
            if (fold != n.lower) {
                f.fold = add(fold);
                f.foldLen = static_cast<uint16_t>(fold.size());
            }
        }
        // The name as in its key. Where keys ignore case an ASCII name's is its
        // lower case; elsewhere it is mostly the name itself. Either shares a copy.
        const std::string key = ascii && !platform::kCaseSensitivePaths ? n.lower : platform::nameKey(n.name);
        f.key = key == n.lower ? f.lower : key == n.name ? f.name : add(key);
        f.keyLen = static_cast<uint16_t>(key.size());
        out->files_.push_back(f);
    }
    return out;
}

bool FolderFiles::sameNames(const std::vector<Name>& names) const {
    if (names.size() != files_.size()) return false;
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i].name != name(i)) return false;
    return true;
}

std::string Snapshot::join(std::string_view folder, std::string_view name) {
    std::string path;
    path.reserve(folder.size() + 1 + name.size());
    path.append(folder);
    const char last = folder.empty() ? '\0' : folder.back();
#ifdef _WIN32
    const bool separated = last == '\\' || last == '/' || last == ':';  // "C:" alone is that drive's own folder
#else
    const bool separated = last == '/';
#endif
    if (!separated) path.push_back(platform::kSeparator);
    path.append(name);
    return path;
}

std::string placePrefix(std::string_view root) {
    std::string prefix = platform::kCaseSensitivePaths ? std::string(root) : pyLower(root);
    while (!prefix.empty() && platform::isSeparator(prefix.back())) prefix.pop_back();
    prefix.push_back(platform::kSeparator);
    return prefix;
}

void ExternalItem::prepare() {
    lower = pyLower(name);
    fold = pyCasefold(name);
    detailLower = pyLower(detail);
}

double Usage::rank(const UsageRecord& record, double now) const {
    if (record.score == 0.0) return 0.0;
    const double last = std::isnan(record.lastUsed) ? now : record.lastUsed;
    // As Python computes it: max(0.0, now - last) / 86400.0, then 0.5 ** (days / half-life).
    const double elapsed = now - last;
    const double days = (0.0 < elapsed ? elapsed : 0.0) / 86400.0;
    return record.score * std::pow(0.5, days / halfLifeDays);
}

int64_t Result::find(Kind kind, std::string_view identity) const {
    for (size_t i = 0; i < hits.size(); ++i) {
        const Hit& hit = hits[i];
        if (hit.group == kAudioGroup) {
            if (kind != Kind::Audio || !snapshot) continue;
            const AudioRef ref = snapshot->audio[hit.index];
            const SnapFolder& folder = snapshot->folders[ref.folder];
            const std::string_view name = folder.files->name(ref.file);
            if (identity.size() < name.size() + folder.path.size() || !identity.ends_with(name) ||
                !identity.starts_with(folder.path))
                continue;
            if (Snapshot::join(folder.path, name) == identity) return static_cast<int64_t>(i);
        } else {
            const auto group = groups.find(static_cast<int>(hit.group));
            if (group == groups.end()) continue;
            const ExternalItem& item = group->second->items[hit.index];
            if (item.kind == kind && item.key == identity) return static_cast<int64_t>(i);
        }
    }
    return -1;
}

}  // namespace sub::browser
