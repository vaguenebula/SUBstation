#include "browser/ItemListModel.h"

#include <QMimeData>

#include <algorithm>
#include <memory>

#include "ListModels.h"
#include "browser/BrowserMime.h"
#include "browser/Library.h"

namespace sub::app {

ItemListModel::ItemListModel(const Library* library, QObject* parent) : QAbstractListModel(parent), library_(library) {}

void ItemListModel::show(int total, std::function<std::vector<BrowserItem>(int, int)> source) {
    const bool counted = total != total_;
    beginResetModel();
    total_ = total;
    source_ = std::move(source);
    items_ = source_ ? source_(0, std::min(kPage, total_)) : std::vector<BrowserItem>();
    endResetModel();
    if (counted) Q_EMIT totalChanged();
}

void ItemListModel::setItems(std::vector<BrowserItem> items) {
    result_ = SearchResult();
    auto list = std::make_shared<const std::vector<BrowserItem>>(std::move(items));
    const int total = static_cast<int>(list->size());
    show(total, [list](int start, int count) {
        const auto begin = list->begin() + std::min<ptrdiff_t>(start, static_cast<ptrdiff_t>(list->size()));
        const auto end = list->begin() + std::min<ptrdiff_t>(start + count, static_cast<ptrdiff_t>(list->size()));
        return std::vector<BrowserItem>(begin, end);
    });
}

void ItemListModel::setSource(const SearchResult& result) {
    result_ = result;
    show(result.total(), [result](int start, int count) { return result.items(start, count); });
}

void ItemListModel::ensureRows(int rows) {
    rows = std::min(rows, total_);
    const int have = static_cast<int>(items_.size());
    if (rows <= have || !source_) return;
    std::vector<BrowserItem> more = source_(have, rows - have);
    if (more.empty()) return;
    beginInsertRows(QModelIndex(), have, have + static_cast<int>(more.size()) - 1);
    items_.insert(items_.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
    endInsertRows();
}

const BrowserItem* ItemListModel::item(int row) const {
    return row >= 0 && row < static_cast<int>(items_.size()) ? &items_[static_cast<size_t>(row)] : nullptr;
}

std::vector<BrowserItem> ItemListModel::items(const QList<int>& rows) const {
    std::vector<BrowserItem> out;
    for (const int row : rows)
        if (const BrowserItem* it = item(row)) out.push_back(*it);
    return out;
}

QVariantMap ItemListModel::get(int row) const { return rowMap(*this, row); }

void ItemListModel::usesChanged() {
    if (items_.empty()) return;
    Q_EMIT dataChanged(index(0), index(static_cast<int>(items_.size()) - 1), {ToolTipRole, UsesRole, Qt::ToolTipRole});
}

int ItemListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(items_.size());
}

QVariant ItemListModel::data(const QModelIndex& index, int role) const {
    const BrowserItem* it = index.isValid() ? item(index.row()) : nullptr;
    if (!it) return {};
    switch (role) {
        case NameRole: return it->name;
        case PathRole: return it->path;
        case KindRole: return kindName(it->kind);
        case DetailRole: return it->detail;
        case KeyRole: return it->key();
        case Qt::DisplayRole:
        case DisplayRole:
            if ((it->kind == ItemKind::Plugin || it->kind == ItemKind::Preset) && !it->detail.isEmpty())
                return it->name + QStringLiteral("   (") + it->detail + QLatin1Char(')');
            return it->name;
        case Qt::ToolTipRole:
        case ToolTipRole: {
            const int uses = library_ ? library_->uses(it->key()) : 0;
            QString tip = it->toolTip.isEmpty() ? it->path : it->toolTip;
            if (uses) tip += QStringLiteral("\nUsed %1 time%2").arg(uses).arg(uses != 1 ? QStringLiteral("s") : QString());
            return tip;
        }
        case IconRole:
            if (it->kind == ItemKind::Preset) return QStringLiteral("preset");
            return it->kind == ItemKind::Audio ? QStringLiteral("waveform") : QStringLiteral("plugin");
        case UsesRole: return library_ ? library_->uses(it->key()) : 0;
        case InstrumentRole: return it->plugin && it->plugin->instrument;
        case PluginRole: return it->plugin ? QVariant(it->plugin->toRef()) : QVariant();
        default: return {};
    }
}

QHash<int, QByteArray> ItemListModel::roleNames() const {
    return {{NameRole, "name"},       {PathRole, "path"},       {KindRole, "kind"},
            {DetailRole, "detail"},   {KeyRole, "key"},         {DisplayRole, "display"},
            {ToolTipRole, "toolTip"}, {IconRole, "icon"},       {UsesRole, "uses"},
            {InstrumentRole, "instrument"}, {PluginRole, "plugin"}};
}

Qt::ItemFlags ItemListModel::flags(const QModelIndex& index) const {
    const Qt::ItemFlags base = QAbstractListModel::flags(index);
    return index.isValid() ? base | Qt::ItemIsDragEnabled : base;
}

bool ItemListModel::canFetchMore(const QModelIndex& parent) const {
    return !parent.isValid() && static_cast<int>(items_.size()) < total_;
}

void ItemListModel::fetchMore(const QModelIndex& parent) {
    if (!parent.isValid()) ensureRows(static_cast<int>(items_.size()) + kPage);
}

QStringList ItemListModel::mimeTypes() const { return browserMimeTypes(); }

QMimeData* ItemListModel::mimeData(const QModelIndexList& indexes) const {
    std::vector<BrowserItem> items;
    for (const QModelIndex& index : indexes)
        if (const BrowserItem* it = index.isValid() ? item(index.row()) : nullptr) items.push_back(*it);
    return browserMimeData(items).release();  // the caller (the drag) owns it
}

}  // namespace sub::app
