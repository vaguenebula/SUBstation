#pragma once

// A context menu (or a chooser's drop-down) worked out in C++: its items, what
// each does, and whether it is enabled, checked, or a submenu. The items that
// put up menus (the lanes, the headers, the choosers) build one, hand it to
// QML as a list (ArrangementMenu.qml shows it) and run the entry chosen by its
// id (trigger). The old widgets' QMenus, without their widgets: what an entry
// does stays in C++.
//
// A shortcut is only a hint shown beside the text (the window's actions handle
// the keys), as the old menus' setShortcutVisibleInContextMenu had it.

#include <QColor>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <functional>
#include <vector>

namespace sub::ui::arrangement {

struct MenuEntry {
    QString text;
    std::function<void()> action;
    bool enabled = true;
    bool checkable = false;
    bool checked = false;
    QString shortcut;  // a hint ("Ctrl+D")
    QString toolTip;
    // A swatch (a track colour) or a dot (an automated parameter) before the text.
    QColor swatch;
    QColor dot;
    bool separator = false;
    bool submenu = false;
    std::vector<MenuEntry> children;  // a submenu's entries
};

// Adds entries to a list of them (a menu's, or a submenu's).
class MenuList {
public:
    explicit MenuList(std::vector<MenuEntry>& entries) : entries_(&entries) {}

    // An entry, last; set more on what it returns (valid until the next add).
    MenuEntry& add(const QString& text, std::function<void()> action = {});
    void addSeparator();
    // A submenu, last: MenuList(entry.children) adds to it.
    MenuEntry& addSubmenu(const QString& text);

private:
    std::vector<MenuEntry>* entries_;
};

class MenuEntries {
public:
    MenuEntry& add(const QString& text, std::function<void()> action = {}) {
        return MenuList(entries_).add(text, std::move(action));
    }
    void addSeparator() { MenuList(entries_).addSeparator(); }
    MenuEntry& addSubmenu(const QString& text) { return MenuList(entries_).addSubmenu(text); }

    bool isEmpty() const { return entries_.empty(); }
    const std::vector<MenuEntry>& entries() const { return entries_; }

    // For QML: [{id, text, enabled, checkable, checked, shortcut, toolTip,
    // swatch, dot, separator, submenu, children: [...]}]; ids count every
    // entry, depth first.
    QVariantList toVariant() const;
    // Runs the entry with this id (from toVariant); false if there is none, or it is disabled.
    bool trigger(int id) const;
    // The entry with this text (a submenu's entries as "Submenu/Entry"), for tests.
    const MenuEntry* find(const QString& path) const;
    // Runs the entry with this text (as find() has it); false if there is none, or it is disabled.
    bool triggerText(const QString& path) const;
    // The texts of the top-level entries (separators as ""), for tests.
    QStringList texts() const;

private:
    std::vector<MenuEntry> entries_;
};

}  // namespace sub::ui::arrangement
