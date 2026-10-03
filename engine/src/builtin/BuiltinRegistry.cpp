#include "builtin/BuiltinRegistry.h"

#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace sub {

BuiltinRegistry& BuiltinRegistry::instance() {
    static BuiltinRegistry registry;  // built on first use: the registrars run in no fixed order
    return registry;
}

void BuiltinRegistry::add(BuiltinCategory category, Factory factory) {
    entries_.push_back({category, std::move(factory)});
}

const std::vector<BuiltinInfo>& BuiltinRegistry::devices() const {
    std::call_once(described_, [this] {
        constexpr std::string_view kPrefix = "builtin:";
        std::vector<std::pair<BuiltinInfo, size_t>> list;
        for (size_t i = 0; i < entries_.size(); ++i) {
            const auto prototype = entries_[i].factory();
            std::string id = prototype->typeId();
            if (id.starts_with(kPrefix)) id.erase(0, kPrefix.size());
            list.push_back({{std::move(id), prototype->name(), entries_[i].category, prototype->params()}, i});
        }
        std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
            if (a.first.category != b.first.category) return a.first.category < b.first.category;
            return a.first.name < b.first.name;
        });
        for (auto& [info, entry] : list) {
            infos_.push_back(std::move(info));
            entryOf_.push_back(entry);
        }
    });
    return infos_;
}

std::shared_ptr<Processor> BuiltinRegistry::create(const std::string& id) const {
    const auto& list = devices();
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].id == id) return entries_[entryOf_[i]].factory();
    }
    throw std::invalid_argument("Unknown built-in device: " + id);
}

}  // namespace sub
