#include "plugins/PluginListModel.h"

namespace sub::app {

PluginListModel::PluginListModel(QObject* parent) : QAbstractListModel(parent) {}

void PluginListModel::setPlugins(std::vector<PluginInfo> plugins) {
    if (plugins == plugins_) return;
    const bool counted = plugins.size() != plugins_.size();
    beginResetModel();
    plugins_ = std::move(plugins);
    endResetModel();
    if (counted) Q_EMIT countChanged();
}

QVariantMap PluginListModel::get(int row) const {
    QVariantMap out;
    if (row < 0 || row >= count()) return out;
    const QHash<int, QByteArray> names = roleNames();
    for (auto it = names.cbegin(); it != names.cend(); ++it)
        out.insert(QString::fromUtf8(it.value()), data(index(row), it.key()));
    return out;
}

int PluginListModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : count(); }

QVariant PluginListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= count()) return {};
    const PluginInfo& p = plugins_[static_cast<size_t>(index.row())];
    switch (role) {
        case Qt::DisplayRole:
        case NameRole: return p.name;
        case VendorRole: return p.vendor;
        case PathRole: return p.path;
        case UidRole: return p.uid;
        case FormatRole: return p.format;
        case VersionRole: return p.version;
        case CategoryRole: return p.category;
        case InstrumentRole: return p.instrument;
        case Qt::ToolTipRole:
        case ToolTipRole: return pluginToolTip(p);
        case RefRole: return p.toRef();
        default: return {};
    }
}

QHash<int, QByteArray> PluginListModel::roleNames() const {
    return {{NameRole, "name"},         {VendorRole, "vendor"},     {PathRole, "path"},
            {UidRole, "uid"},           {FormatRole, "format"},     {VersionRole, "version"},
            {CategoryRole, "category"}, {InstrumentRole, "instrument"}, {ToolTipRole, "toolTip"},
            {RefRole, "ref"}};
}

}  // namespace sub::app
