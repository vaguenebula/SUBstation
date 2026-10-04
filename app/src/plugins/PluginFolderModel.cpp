#include "plugins/PluginFolderModel.h"

#include <QDir>
#include <QFileInfo>

#include "browser/PathKeys.h"
#include "plugins/PluginPaths.h"
#include "plugins/PluginSettings.h"

namespace sub::app {

PluginFolderModel::PluginFolderModel(QObject* parent) : QAbstractListModel(parent) { refresh(); }

void PluginFolderModel::refresh() {
    std::vector<Row> rows;
    for (const QString& folder : standardPluginFolders()) rows.push_back({folder, true, true});
    for (const QString& folder : customPluginFolders()) rows.push_back({folder, false, QFileInfo(folder).isDir()});
    const bool counted = rows.size() != rows_.size();
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
    if (counted) emit countChanged();
}

int PluginFolderModel::find(const QString& folder) const {
    for (size_t i = 0; i < rows_.size(); ++i)
        if (sameFolder(rows_[i].path, folder)) return static_cast<int>(i);
    return -1;
}

int PluginFolderModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : count(); }

QVariant PluginFolderModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= count()) return {};
    const Row& row = rows_[static_cast<size_t>(index.row())];
    const QString shown = QDir::toNativeSeparators(row.path);
    switch (role) {
        case PathRole: return row.path;
        case Qt::DisplayRole:
        case DisplayRole: return row.standard ? shown + QStringLiteral("  (standard)") : shown;
        case StandardRole: return row.standard;
        case ExistsRole: return row.exists;
        case RemovableRole: return !row.standard;
        case Qt::ToolTipRole:
        case ToolTipRole:
            if (row.standard) return QStringLiteral("A standard VST3 folder: always searched.");
            return row.exists ? shown : shown + QStringLiteral("\nThis folder doesn't exist (any more).");
        default: return {};
    }
}

QHash<int, QByteArray> PluginFolderModel::roleNames() const {
    return {{PathRole, "path"},         {DisplayRole, "display"},     {StandardRole, "standard"},
            {ExistsRole, "exists"},     {RemovableRole, "removable"}, {ToolTipRole, "toolTip"}};
}

}  // namespace sub::app
