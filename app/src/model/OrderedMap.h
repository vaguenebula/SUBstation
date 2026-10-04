#pragma once
// A small map that keeps its keys in the order they were first inserted, for
// the few places where that order shows: a track's envelopes (the first one
// automated is the one a lane shows first), and several tracks' devices changed
// together (their signals go out in the order given). Lookups are linear: these
// maps hold a handful of entries. Two maps are equal when they hold the same
// entries, whatever their order.

#include <QList>

#include <algorithm>
#include <initializer_list>
#include <utility>
#include <vector>

namespace sub::app {

template <typename K, typename V>
class OrderedMap {
public:
    using value_type = std::pair<K, V>;
    using iterator = typename std::vector<value_type>::iterator;
    using const_iterator = typename std::vector<value_type>::const_iterator;

    OrderedMap() = default;
    OrderedMap(std::initializer_list<value_type> entries) {
        for (const auto& [key, value] : entries) insert(key, value);
    }

    bool contains(const K& key) const { return find(key) != nullptr; }

    const V* find(const K& key) const {
        for (const auto& entry : entries_) {
            if (entry.first == key) return &entry.second;
        }
        return nullptr;
    }
    V* find(const K& key) {
        for (auto& entry : entries_) {
            if (entry.first == key) return &entry.second;
        }
        return nullptr;
    }

    V value(const K& key, const V& fallback = V{}) const {
        const V* found = find(key);
        return found != nullptr ? *found : fallback;
    }

    // Sets a key's value: in its place if it is there, else last.
    void insert(const K& key, V value) {
        if (V* found = find(key)) {
            *found = std::move(value);
        } else {
            entries_.emplace_back(key, std::move(value));
        }
    }

    // Whether there was such a key.
    bool remove(const K& key) {
        const auto it = std::find_if(entries_.begin(), entries_.end(),
                                     [&](const value_type& entry) { return entry.first == key; });
        if (it == entries_.end()) return false;
        entries_.erase(it);
        return true;
    }

    V& operator[](const K& key) {
        if (V* found = find(key)) return *found;
        entries_.emplace_back(key, V{});
        return entries_.back().second;
    }

    QList<K> keys() const {
        QList<K> result;
        result.reserve(static_cast<qsizetype>(entries_.size()));
        for (const auto& entry : entries_) result.append(entry.first);
        return result;
    }

    int size() const { return static_cast<int>(entries_.size()); }
    bool isEmpty() const { return entries_.empty(); }
    void clear() { entries_.clear(); }

    iterator begin() { return entries_.begin(); }
    iterator end() { return entries_.end(); }
    const_iterator begin() const { return entries_.begin(); }
    const_iterator end() const { return entries_.end(); }

    friend bool operator==(const OrderedMap& a, const OrderedMap& b) {
        if (a.entries_.size() != b.entries_.size()) return false;
        for (const auto& [key, value] : a.entries_) {
            const V* other = b.find(key);
            if (other == nullptr || !(*other == value)) return false;
        }
        return true;
    }
    friend bool operator!=(const OrderedMap& a, const OrderedMap& b) { return !(a == b); }

private:
    std::vector<value_type> entries_;
};

}  // namespace sub::app
