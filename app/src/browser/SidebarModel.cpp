#include "browser/SidebarModel.h"

namespace sub::app {

SidebarModel::SidebarModel(QObject* parent) : QAbstractListModel(parent) {}

void SidebarModel::setEntries(std::vector<Entry> entries) {
    const bool counted = entries.size() != entries_.size();
    beginResetModel();
    entries_ = std::move(entries);
    endResetModel();
    if (counted) Q_EMIT countChanged();
}

int SidebarModel::find(const QStringList& scope) const { return find(Scope::fromList(scope)); }

int SidebarModel::find(const Scope& scope) const {
    for (size_t i = 0; i < entries_.size(); ++i)
        if (!entries_[i].section && entries_[i].scope == scope) return static_cast<int>(i);
    return -1;
}

QStringList SidebarModel::children(const Scope& scope) const {
    QStringList titles;
    const int row = find(scope);
    if (row < 0) return titles;
    for (size_t i = static_cast<size_t>(row) + 1; i < entries_.size() && entries_[i].depth > entries_[static_cast<size_t>(row)].depth; ++i)
        titles << entries_[i].title;
    return titles;
}

void SidebarModel::setToolTip(const Scope& scope, const QString& toolTip) {
    const int row = find(scope);
    if (row < 0 || entries_[static_cast<size_t>(row)].toolTip == toolTip) return;
    entries_[static_cast<size_t>(row)].toolTip = toolTip;
    Q_EMIT dataChanged(index(row), index(row), {ToolTipRole, Qt::ToolTipRole});
}

int SidebarModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : count(); }

QVariant SidebarModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= count()) return {};
    const Entry& entry = entries_[static_cast<size_t>(index.row())];
    switch (role) {
        case Qt::DisplayRole:
        case TitleRole: return entry.title;
        case ScopeRole: return entry.section ? QStringList() : entry.scope.toList();
        case SectionRole: return entry.section;
        case DepthRole: return entry.depth;
        case IconRole: return entry.icon;
        case Qt::ToolTipRole:
        case ToolTipRole: return entry.toolTip;
        case DimRole: return entry.dim;
        case SelectableRole: return !entry.section;
        default: return {};
    }
}

QHash<int, QByteArray> SidebarModel::roleNames() const {
    return {{TitleRole, "title"}, {ScopeRole, "scope"},     {SectionRole, "section"}, {DepthRole, "depth"},
            {IconRole, "icon"},   {ToolTipRole, "toolTip"}, {DimRole, "dim"},         {SelectableRole, "selectable"}};
}

}  // namespace sub::app
