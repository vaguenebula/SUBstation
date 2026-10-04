#pragma once
// A flat list of browser items (search results, plug-ins...), draggable.
//
// It shows a source (a search's results) a page at a time: the first page at
// once, more as the view scrolls near the end (Qt's canFetchMore/fetchMore,
// which QML views call too). Views lay out every row they have, so a list of
// 200 000 files would cost the UI thread that much each time it changed; this
// way it costs a page.

#include <QAbstractListModel>
#include <QStringList>
#include <QVariantMap>

#include <functional>
#include <vector>

#include "browser/BrowserItem.h"
#include "browser/FileIndex.h"

namespace sub::app {

class Library;

class ItemListModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int total READ total NOTIFY totalChanged)  // rows in the whole list, shown or not yet

public:
    static constexpr int kPage = 256;

    enum Role {
        NameRole = Qt::UserRole + 1,
        PathRole,
        KindRole,        // "audio", "plugin", "device", "preset"
        DetailRole,
        KeyRole,
        DisplayRole,     // the name, and for a plug-in or preset its detail: "Name   (Vendor)"
        ToolTipRole,     // the item's tooltip (or path), and how often it was used
        IconRole,        // "waveform", "plugin" or "preset"
        UsesRole,        // how often it was used
        InstrumentRole,  // a plug-in that is an instrument
        PluginRole,      // a plug-in's PluginRef fields (PluginInfo::toRef), else nothing
    };
    Q_ENUM(Role)

    // `library`: for how often each item was used (may be null).
    explicit ItemListModel(const Library* library = nullptr, QObject* parent = nullptr);

    int total() const { return total_; }
    void setItems(std::vector<BrowserItem> items);
    void setSource(const SearchResult& result);
    const SearchResult& result() const { return result_; }  // the search shown (null for a plain list)

    // Have at least `rows` rows (or all there are).
    Q_INVOKABLE void ensureRows(int rows);
    // The item of a row that is there, or null.
    const BrowserItem* item(int row) const;
    std::vector<BrowserItem> items(const QList<int>& rows) const;  // those of these rows that are there
    Q_INVOKABLE QVariantMap get(int row) const;  // every role of a row, by name
    // The library's use counts changed: the rows' tooltips and uses with them.
    void usesChanged();

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;

Q_SIGNALS:
    void totalChanged();

private:
    void show(int total, std::function<std::vector<BrowserItem>(int, int)> source);

    const Library* library_;
    std::vector<BrowserItem> items_;  // the rows shown
    int total_ = 0;
    std::function<std::vector<BrowserItem>(int start, int count)> source_;
    SearchResult result_;
};

}  // namespace sub::app
