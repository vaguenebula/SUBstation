#pragma once
// The plug-ins found, as a list for the UI (one row a plug-in, by name).

#include <QAbstractListModel>

#include <vector>

#include "plugins/PluginInfo.h"

namespace sub::app {

class PluginListModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        VendorRole,
        PathRole,
        UidRole,
        FormatRole,
        VersionRole,
        CategoryRole,
        InstrumentRole,
        ToolTipRole,
        RefRole,  // what a device needs to load it (PluginInfo::toRef)
    };
    Q_ENUM(Role)

    explicit PluginListModel(QObject* parent = nullptr);

    void setPlugins(std::vector<PluginInfo> plugins);
    const std::vector<PluginInfo>& plugins() const { return plugins_; }
    int count() const { return static_cast<int>(plugins_.size()); }
    Q_INVOKABLE QVariantMap get(int row) const;  // every role of a row, by name

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void countChanged();

private:
    std::vector<PluginInfo> plugins_;
};

}  // namespace sub::app
