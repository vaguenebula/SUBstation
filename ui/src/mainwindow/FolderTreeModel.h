#pragma once

// A folder tree as a flat list, for a ListView: the browser's place shown as
// the old QTreeView showed it (a QFileSystemModel from a root index, folders
// first). Qt 6.4's TreeView has no root index, so this flattens the part of the
// source model under `rootIndex` instead: each row is a file or folder, with
// its depth; a folder's rows follow it while it is expanded.
//
// Expanded folders are remembered by path, so they stay open while the source
// loads, sorts and changes (QFileSystemModel does all three in the
// background). Every change of the source's structure is mirrored by working
// the rows out again and replacing only the run of rows that differs, so the
// view keeps its place and its current row.

#include <QAbstractListModel>
#include <QList>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <vector>

class QFileSystemModel;

namespace sub::ui {

class FolderTreeModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QAbstractItemModel* sourceModel READ sourceModel WRITE setSourceModel NOTIFY sourceModelChanged)
    Q_PROPERTY(QModelIndex rootIndex READ rootIndex WRITE setRootIndex NOTIFY rootIndexChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        PathRole,
        DepthRole,        // 0 for what is straight in the root
        IsDirRole,
        ExpandedRole,
        HasChildrenRole,  // a folder (it may turn out empty once read)
    };
    Q_ENUM(Role)

    explicit FolderTreeModel(QObject* parent = nullptr);

    QAbstractItemModel* sourceModel() const { return source_; }
    void setSourceModel(QAbstractItemModel* model);
    QModelIndex rootIndex() const { return root_; }
    void setRootIndex(const QModelIndex& index);
    int count() const { return static_cast<int>(rows_.size()); }

    Q_INVOKABLE void expand(int row);
    Q_INVOKABLE void collapse(int row);
    Q_INVOKABLE void toggle(int row);
    Q_INVOKABLE bool isExpanded(int row) const;
    Q_INVOKABLE QString path(int row) const;
    Q_INVOKABLE bool isDir(int row) const;
    Q_INVOKABLE int depth(int row) const;
    // The row of the folder a row is in (-1: it is straight in the root).
    Q_INVOKABLE int parentRow(int row) const;
    // The row showing a path, or -1.
    Q_INVOKABLE int rowOf(const QString& path) const;
    Q_INVOKABLE QStringList paths(const QList<int>& rows) const;
    // What a drag of these rows carries: {"text/uri-list": their file urls}.
    Q_INVOKABLE QVariantMap dragData(const QList<int>& rows) const;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

Q_SIGNALS:
    void sourceModelChanged();
    void rootIndexChanged();
    void countChanged();

private:
    struct Row {
        QPersistentModelIndex index;
        int depth = 0;
        friend bool operator==(const Row&, const Row&) = default;
    };

    QFileSystemModel* files() const;
    QString pathOf(const QModelIndex& index) const;
    bool dirOf(const QModelIndex& index) const;
    void collect(const QModelIndex& parent, int depth, std::vector<Row>& rows, QList<QPersistentModelIndex>& fetch);
    // Works the rows out again and replaces the run that differs.
    void rebuild();
    void reset();
    void sourceDataChanged(const QModelIndex& topLeft, const QModelIndex& bottomRight);

    QPointer<QAbstractItemModel> source_;
    QPersistentModelIndex root_;
    std::vector<Row> rows_;
    QSet<QString> expanded_;  // folders open, by path
    bool rebuilding_ = false;
    bool again_ = false;
};

}  // namespace sub::ui
