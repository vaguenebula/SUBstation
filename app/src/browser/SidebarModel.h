#pragma once
// The browser's sidebar as a flat list: section headings, entries and their
// sub-entries (one level, always shown), each with the scope it lists.
//
//   CATEGORIES
//   All · Samples · Built-in (its categories) · Plug-ins (Instruments, Audio
//   Effects) · Presets (a sub-entry per device they are for)
//   PLACES
//   each place · Add Folder…

#include <QAbstractListModel>

#include <optional>
#include <vector>

#include "browser/BrowserSearch.h"

namespace sub::app {

class SidebarModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    struct Entry {
        QString title;
        Scope scope;           // what it lists (none for a heading)
        bool section = false;  // a heading: not selectable
        int depth = 0;         // 1 for a sub-entry
        QString icon;          // "search", "waveform", "plugin", "preset", "folder" or ''
        QString toolTip;
        bool dim = false;      // shown dimmed (headings, "Add Folder…")
    };

    enum Role {
        TitleRole = Qt::UserRole + 1,
        ScopeRole,       // [kind] or [kind, sub]; empty for a heading
        SectionRole,
        DepthRole,
        IconRole,
        ToolTipRole,
        DimRole,
        SelectableRole,
    };
    Q_ENUM(Role)

    explicit SidebarModel(QObject* parent = nullptr);

    void setEntries(std::vector<Entry> entries);
    const std::vector<Entry>& entries() const { return entries_; }
    int count() const { return static_cast<int>(entries_.size()); }
    // The row of the entry with this scope, or -1.
    Q_INVOKABLE int find(const QStringList& scope) const;
    int find(const Scope& scope) const;
    // The titles of an entry's sub-entries.
    QStringList children(const Scope& scope) const;
    void setToolTip(const Scope& scope, const QString& toolTip);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void countChanged();

private:
    std::vector<Entry> entries_;
};

}  // namespace sub::app
