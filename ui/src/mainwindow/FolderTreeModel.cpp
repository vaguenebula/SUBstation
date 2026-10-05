#include "FolderTreeModel.h"

#include <QFileInfo>
#include <QFileSystemModel>
#include <QUrl>

#include <algorithm>

namespace sub::ui {

FolderTreeModel::FolderTreeModel(QObject* parent) : QAbstractListModel(parent) {}

QFileSystemModel* FolderTreeModel::files() const { return qobject_cast<QFileSystemModel*>(source_.data()); }

void FolderTreeModel::setSourceModel(QAbstractItemModel* model) {
    if (model == source_) return;
    if (source_) disconnect(source_, nullptr, this, nullptr);
    source_ = model;
    if (source_) {
        connect(source_, &QAbstractItemModel::rowsInserted, this, &FolderTreeModel::rebuild);
        connect(source_, &QAbstractItemModel::rowsRemoved, this, &FolderTreeModel::rebuild);
        connect(source_, &QAbstractItemModel::rowsMoved, this, &FolderTreeModel::rebuild);
        connect(source_, &QAbstractItemModel::layoutChanged, this, &FolderTreeModel::rebuild);
        connect(source_, &QAbstractItemModel::modelReset, this, &FolderTreeModel::rebuild);
        connect(source_, &QAbstractItemModel::dataChanged, this, &FolderTreeModel::sourceDataChanged);
        connect(source_, &QObject::destroyed, this, &FolderTreeModel::reset);
    }
    Q_EMIT sourceModelChanged();
    reset();
}

void FolderTreeModel::setRootIndex(const QModelIndex& index) {
    if (QModelIndex(root_) == index) return;
    root_ = index;
    Q_EMIT rootIndexChanged();
    reset();
}

QString FolderTreeModel::pathOf(const QModelIndex& index) const {
    if (QFileSystemModel* model = files()) return model->filePath(index);
    return index.data(QFileSystemModel::FilePathRole).toString();
}

bool FolderTreeModel::dirOf(const QModelIndex& index) const {
    if (QFileSystemModel* model = files()) return model->isDir(index);
    return source_ && source_->hasChildren(index);
}

void FolderTreeModel::collect(const QModelIndex& parent, int depth, std::vector<Row>& rows,
                              QList<QPersistentModelIndex>& fetch) {
    const int n = source_->rowCount(parent);
    for (int i = 0; i < n; ++i) {
        const QModelIndex child = source_->index(i, 0, parent);
        rows.push_back({QPersistentModelIndex(child), depth});
        if (dirOf(child) && expanded_.contains(pathOf(child))) {
            if (source_->canFetchMore(child)) fetch << QPersistentModelIndex(child);
            collect(child, depth + 1, rows, fetch);
        }
    }
}

void FolderTreeModel::reset() {
    beginResetModel();
    rows_.clear();
    endResetModel();
    Q_EMIT countChanged();
    rebuild();
}

void FolderTreeModel::rebuild() {
    if (rebuilding_) {  // (the source changed while it was being read)
        again_ = true;
        return;
    }
    rebuilding_ = true;
    do {
        again_ = false;
        std::vector<Row> rows;
        QList<QPersistentModelIndex> fetch;
        const bool rooted = source_ && root_.isValid();
        if (rooted) {
            if (source_->canFetchMore(root_)) fetch << root_;
            collect(root_, 0, rows, fetch);
        }
        // Replace only the run of rows that differs: the rows before and after it stay.
        const size_t before = rows_.size();
        size_t prefix = 0;
        while (prefix < before && prefix < rows.size() && rows_[prefix] == rows[prefix]) ++prefix;
        size_t suffix = 0;
        while (suffix < before - prefix && suffix < rows.size() - prefix &&
               rows_[before - 1 - suffix] == rows[rows.size() - 1 - suffix])
            ++suffix;
        if (before - suffix > prefix) {
            beginRemoveRows({}, int(prefix), int(before - suffix - 1));
            rows_.erase(rows_.begin() + qsizetype(prefix), rows_.begin() + qsizetype(before - suffix));
            endRemoveRows();
        }
        if (rows.size() - suffix > prefix) {
            beginInsertRows({}, int(prefix), int(rows.size() - suffix - 1));
            rows_.insert(rows_.begin() + qsizetype(prefix), rows.begin() + qsizetype(prefix),
                         rows.begin() + qsizetype(rows.size() - suffix));
            endInsertRows();
        }
        if (before != rows_.size()) Q_EMIT countChanged();
        if (!rows_.empty()) Q_EMIT dataChanged(index(0), index(count() - 1), {ExpandedRole, HasChildrenRole});
        // Folders opened but not read yet are read now (QFileSystemModel reads them in the
        // background: their rows come with rowsInserted).
        for (const QPersistentModelIndex& folder : fetch)
            if (folder.isValid() && source_ && source_->canFetchMore(folder)) source_->fetchMore(folder);
    } while (again_);
    rebuilding_ = false;
}

void FolderTreeModel::sourceDataChanged(const QModelIndex& topLeft, const QModelIndex& bottomRight) {
    if (rows_.empty() || topLeft.column() > 0) return;
    const QModelIndex parent = topLeft.parent();
    int first = -1, last = -1;
    for (int row = 0; row < count(); ++row) {
        const QModelIndex& shown = rows_[size_t(row)].index;
        if (shown.parent() == parent && shown.row() >= topLeft.row() && shown.row() <= bottomRight.row()) {
            if (first < 0) first = row;
            last = row;
        }
    }
    if (first >= 0) Q_EMIT dataChanged(index(first), index(last));
}

void FolderTreeModel::expand(int row) {
    if (row < 0 || row >= count() || !isDir(row)) return;
    const QString folder = path(row);
    if (expanded_.contains(folder)) return;
    expanded_.insert(folder);
    rebuild();
}

void FolderTreeModel::collapse(int row) {
    if (row < 0 || row >= count()) return;
    if (!expanded_.remove(path(row))) return;
    rebuild();
}

void FolderTreeModel::toggle(int row) {
    if (isExpanded(row))
        collapse(row);
    else
        expand(row);
}

bool FolderTreeModel::isExpanded(int row) const {
    return row >= 0 && row < count() && isDir(row) && expanded_.contains(path(row));
}

QString FolderTreeModel::path(int row) const {
    return row >= 0 && row < count() ? pathOf(rows_[size_t(row)].index) : QString();
}

bool FolderTreeModel::isDir(int row) const { return row >= 0 && row < count() && dirOf(rows_[size_t(row)].index); }

int FolderTreeModel::depth(int row) const { return row >= 0 && row < count() ? rows_[size_t(row)].depth : 0; }

int FolderTreeModel::parentRow(int row) const {
    if (row < 0 || row >= count()) return -1;
    const int level = rows_[size_t(row)].depth;
    for (int above = row - 1; above >= 0; --above)
        if (rows_[size_t(above)].depth < level) return above;
    return -1;
}

int FolderTreeModel::rowOf(const QString& wanted) const {
    for (int row = 0; row < count(); ++row)
        if (path(row) == wanted) return row;
    return -1;
}

QStringList FolderTreeModel::paths(const QList<int>& rows) const {
    QStringList out;
    QList<int> sorted = rows;
    std::sort(sorted.begin(), sorted.end());
    for (int row : sorted)
        if (row >= 0 && row < count()) out << path(row);
    return out;
}

QVariantMap FolderTreeModel::dragData(const QList<int>& rows) const {
    QString list;
    for (const QString& file : paths(rows)) list += QUrl::fromLocalFile(file).toString(QUrl::FullyEncoded) + QStringLiteral("\r\n");
    if (list.isEmpty()) return {};
    return {{QStringLiteral("text/uri-list"), list}};
}

int FolderTreeModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : count(); }

QVariant FolderTreeModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= count()) return {};
    const Row& row = rows_[size_t(index.row())];
    if (!row.index.isValid()) return {};
    switch (role) {
        case Qt::DisplayRole:
        case NameRole: return row.index.data(Qt::DisplayRole);
        case PathRole: return pathOf(row.index);
        case DepthRole: return row.depth;
        case IsDirRole: return dirOf(row.index);
        case ExpandedRole: return dirOf(row.index) && expanded_.contains(pathOf(row.index));
        case HasChildrenRole: return dirOf(row.index) && source_->hasChildren(row.index);
        default: return {};
    }
}

QHash<int, QByteArray> FolderTreeModel::roleNames() const {
    return {{NameRole, "name"},         {PathRole, "path"},         {DepthRole, "depth"},
            {IsDirRole, "isDir"},       {ExpandedRole, "expanded"}, {HasChildrenRole, "hasChildren"}};
}

}  // namespace sub::ui
