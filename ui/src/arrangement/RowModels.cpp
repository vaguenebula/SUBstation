#include "arrangement/RowModels.h"

#include <algorithm>

namespace sub::ui::arrangement {

namespace {

template <typename T, typename Id>
QStringList idsOf(const std::vector<T>& items, Id id) {
    QStringList ids;
    for (const T& item : items) ids << id(item);
    return ids;
}

// How `after` differs from `before`: the same (-1, 0), one inserted (its index,
// 1), one removed (its index, -1); anything else (0, 2).
std::pair<int, int> difference(const QStringList& before, const QStringList& after) {
    if (before == after) return {-1, 0};
    const auto sizeBefore = before.size(), sizeAfter = after.size();
    if (std::abs(sizeAfter - sizeBefore) != 1) return {0, 2};
    const QStringList& longer = sizeAfter > sizeBefore ? after : before;
    const QStringList& shorter = sizeAfter > sizeBefore ? before : after;
    qsizetype at = 0;
    while (at < shorter.size() && shorter[at] == longer[at]) ++at;
    for (qsizetype i = at; i < shorter.size(); ++i) {
        if (shorter[i] != longer[i + 1]) return {0, 2};
    }
    return {static_cast<int>(at), sizeAfter > sizeBefore ? 1 : -1};
}

// Applies `after` to `items`, telling the views as `difference` has it.
template <typename T, typename Id>
void apply(QAbstractListModel& model, std::vector<T>& items, const std::vector<T>& after, Id id,
           const std::function<void(int, int)>& insertRows, const std::function<void(int, int)>& removeRows,
           const std::function<void()>& endInsert, const std::function<void()>& endRemove,
           const std::function<void()>& reset, const std::function<void()>& endReset) {
    const auto [at, kind] = difference(idsOf(items, id), idsOf(after, id));
    if (kind == 0) {
        const bool changed = items != after;
        items = after;
        if (changed && !items.empty())
            Q_EMIT model.dataChanged(model.index(0), model.index(static_cast<int>(items.size()) - 1));
    } else if (kind == 1) {
        insertRows(at, at);
        items = after;
        endInsert();
        if (!items.empty()) Q_EMIT model.dataChanged(model.index(0), model.index(static_cast<int>(items.size()) - 1));
    } else if (kind == -1) {
        removeRows(at, at);
        items = after;
        endRemove();
        if (!items.empty()) Q_EMIT model.dataChanged(model.index(0), model.index(static_cast<int>(items.size()) - 1));
    } else {
        reset();
        items = after;
        endReset();
    }
}

}  // namespace

// --- TrackRowModel ------------------------------------------------------------------------

int TrackRowModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant TrackRowModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) return {};
    const Row& row = rows_[static_cast<size_t>(index.row())];
    switch (role) {
        case TrackIdRole: return row.trackId;
        case TopRole: return row.top;
        case MainHeightRole: return row.mainHeight;
        case RowHeightRole: return row.height();
        case HiddenRole: return row.hidden;
        case FoldedRole: return row.folded;
        case DepthRole: return row.depth;
        case NumberRole: return index.row() + 1;
        default: return {};
    }
}

QHash<int, QByteArray> TrackRowModel::roleNames() const {
    return {{TrackIdRole, "trackId"},     {TopRole, "top"},       {MainHeightRole, "mainHeight"},
            {RowHeightRole, "rowHeight"}, {HiddenRole, "hidden"}, {FoldedRole, "folded"},
            {DepthRole, "depth"},         {NumberRole, "number"}};
}

void TrackRowModel::update(const std::vector<Row>& rows) {
    apply(
        *this, rows_, rows, [](const Row& row) { return row.trackId; },
        [this](int first, int last) { beginInsertRows(QModelIndex(), first, last); },
        [this](int first, int last) { beginRemoveRows(QModelIndex(), first, last); },
        [this] { endInsertRows(); }, [this] { endRemoveRows(); }, [this] { beginResetModel(); },
        [this] { endResetModel(); });
}

// --- ReturnRowModel -----------------------------------------------------------------------

int ReturnRowModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(items_.size());
}

QVariant ReturnRowModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(items_.size())) return {};
    const Item& item = items_[static_cast<size_t>(index.row())];
    switch (role) {
        case TrackIdRole: return item.trackId;
        case MainHeightRole: return item.mainHeight;
        case RowHeightRole: return item.height;
        default: return {};
    }
}

QHash<int, QByteArray> ReturnRowModel::roleNames() const {
    return {{TrackIdRole, "trackId"}, {MainHeightRole, "mainHeight"}, {RowHeightRole, "rowHeight"}};
}

void ReturnRowModel::update(const std::vector<Item>& items) {
    apply(
        *this, items_, items, [](const Item& item) { return item.trackId; },
        [this](int first, int last) { beginInsertRows(QModelIndex(), first, last); },
        [this](int first, int last) { beginRemoveRows(QModelIndex(), first, last); },
        [this] { endInsertRows(); }, [this] { endRemoveRows(); }, [this] { beginResetModel(); },
        [this] { endResetModel(); });
}

}  // namespace sub::ui::arrangement
