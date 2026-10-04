#pragma once
// The folders plug-ins are looked in, as the preferences list them: the
// standard folders first (always searched, not removable), then the user's own
// (marked when missing).

#include <QAbstractListModel>
#include <QStringList>

#include <vector>

namespace sub::app {

class PluginFolderModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        PathRole = Qt::UserRole + 1,
        DisplayRole,    // the path, "  (standard)" after a standard one
        StandardRole,   // a standard folder: always searched, can't be removed
        ExistsRole,     // false: a folder of the user's that isn't there (any more): shown in red
        RemovableRole,
        ToolTipRole,
    };
    Q_ENUM(Role)

    explicit PluginFolderModel(QObject* parent = nullptr);

    // Lists standardPluginFolders() and customPluginFolders() again.
    void refresh();
    int count() const { return static_cast<int>(rows_.size()); }
    // The row of a folder (compared as folders are), or -1.
    Q_INVOKABLE int find(const QString& folder) const;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void countChanged();

private:
    struct Row {
        QString path;
        bool standard = false;
        bool exists = true;
    };
    std::vector<Row> rows_;
};

}  // namespace sub::app
