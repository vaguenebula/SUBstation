#include "arrangement/MenuEntries.h"

#include <QVariantMap>

#include <utility>

namespace sub::ui::arrangement {

namespace {

QVariantList toList(const std::vector<MenuEntry>& entries, int& next) {
    QVariantList list;
    for (const MenuEntry& entry : entries) {
        QVariantMap item;
        item.insert(QStringLiteral("id"), next++);
        item.insert(QStringLiteral("text"), entry.text);
        item.insert(QStringLiteral("enabled"), entry.enabled);
        item.insert(QStringLiteral("checkable"), entry.checkable);
        item.insert(QStringLiteral("checked"), entry.checked);
        item.insert(QStringLiteral("shortcut"), entry.shortcut);
        item.insert(QStringLiteral("toolTip"), entry.toolTip);
        item.insert(QStringLiteral("swatch"), entry.swatch.isValid() ? QVariant(entry.swatch) : QVariant());
        item.insert(QStringLiteral("dot"), entry.dot.isValid() ? QVariant(entry.dot) : QVariant());
        item.insert(QStringLiteral("separator"), entry.separator);
        item.insert(QStringLiteral("search"), entry.search);
        item.insert(QStringLiteral("submenu"), entry.submenu);
        item.insert(QStringLiteral("children"), toList(entry.children, next));
        list.append(item);
    }
    return list;
}

const MenuEntry* findId(const std::vector<MenuEntry>& entries, int id, int& next) {
    for (const MenuEntry& entry : entries) {
        if (next++ == id) return &entry;
        if (const MenuEntry* found = findId(entry.children, id, next)) return found;
    }
    return nullptr;
}

// The entry of `entries` with this text; a search field's entries are among them.
const MenuEntry* findText(const std::vector<MenuEntry>& entries, const QString& text) {
    for (const MenuEntry& entry : entries) {
        if (entry.search) {
            if (const MenuEntry* found = findText(entry.children, text)) return found;
        } else if (!entry.separator && entry.text == text) {
            return &entry;
        }
    }
    return nullptr;
}

bool run(const MenuEntry* entry) {
    if (entry == nullptr || !entry->enabled || entry->separator || entry->search || !entry->action) return false;
    const std::function<void()> action = entry->action;  // (it may replace the menu it is in)
    action();
    return true;
}

}  // namespace

MenuEntry& MenuList::add(const QString& text, std::function<void()> action) {
    MenuEntry entry;
    entry.text = text;
    entry.action = std::move(action);
    entries_->push_back(std::move(entry));
    return entries_->back();
}

void MenuList::addSeparator() {
    if (entries_->empty() || entries_->back().separator) return;  // (QMenu shows none there either)
    MenuEntry entry;
    entry.separator = true;
    entry.enabled = false;
    entries_->push_back(std::move(entry));
}

MenuEntry& MenuList::addSearch() {
    MenuEntry entry;
    entry.search = true;
    entries_->push_back(std::move(entry));
    return entries_->back();
}

MenuEntry& MenuList::addSubmenu(const QString& text) {
    MenuEntry& entry = add(text);
    entry.submenu = true;
    return entry;
}

QVariantList MenuEntries::toVariant() const {
    int next = 0;
    return toList(entries_, next);
}

bool MenuEntries::trigger(int id) const {
    int next = 0;
    return run(findId(entries_, id, next));
}

const MenuEntry* MenuEntries::find(const QString& path) const {
    const QStringList parts = path.split(u'/');
    const std::vector<MenuEntry>* level = &entries_;
    const MenuEntry* found = nullptr;
    for (const QString& part : parts) {
        found = findText(*level, part);
        if (found == nullptr) return nullptr;
        level = &found->children;
    }
    return found;
}

bool MenuEntries::triggerText(const QString& path) const { return run(find(path)); }

QStringList MenuEntries::texts() const {
    QStringList texts;
    for (const MenuEntry& entry : entries_) {
        texts << (entry.separator || entry.search ? QString() : entry.text);
        if (entry.search) {
            for (const MenuEntry& child : entry.children) texts << child.text;
        }
    }
    return texts;
}

}  // namespace sub::ui::arrangement
