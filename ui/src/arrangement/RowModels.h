#pragma once

// The rows the arrangement's QML lays out: a header per track of the
// arrangement (TrackRowModel, where each sits in the scrolled column) and a
// lane and a header per return track (ReturnRowModel, how tall each is). They
// follow the Arrangement's layout. A change that keeps the same tracks in the
// same order changes the rows' data; a track coming or going inserts or removes
// its row, so the other headers stay as they are (a rename under way, a drag);
// anything else (tracks moved) resets the model.

#include "arrangement/TrackLayout.h"

#include <QAbstractListModel>
#include <QStringList>

#include <vector>

namespace sub::app {
class Project;
}

namespace sub::ui::arrangement {

class TrackRowModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        TrackIdRole = Qt::UserRole + 1,
        TopRole,         // content y
        MainHeightRole,  // its own lane
        RowHeightRole,   // with its automation lanes below
        HiddenRole,      // in a folded group
        FoldedRole,
        DepthRole,
        NumberRole,      // its place among the tracks, from 1 (the activator shows it)
    };

    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void update(const std::vector<Row>& rows);

private:
    std::vector<Row> rows_;
};

class ReturnRowModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        TrackIdRole = Qt::UserRole + 1,
        MainHeightRole,
        RowHeightRole,
    };

    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    struct Item {
        QString trackId;
        int mainHeight = 0;
        int height = 0;

        friend bool operator==(const Item&, const Item&) = default;
    };
    void update(const std::vector<Item>& items);
    const std::vector<Item>& items() const { return items_; }

private:
    std::vector<Item> items_;
};

}  // namespace sub::ui::arrangement
