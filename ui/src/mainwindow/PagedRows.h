#pragma once

// A list model as it is (an identity proxy) whose further rows are fetched
// only when the view asks: the browser's results come a page at a time
// (ItemListModel), and a QML view would otherwise fetch every page, one after
// another, as soon as the list is shown (QQmlDelegateModel fetches more while
// the model can), which for 200 000 files is what paging is there to avoid.
// The view calls fetchMore() as it scrolls near the end.

#include <QIdentityProxyModel>
#include <QtQml/qqmlregistration.h>

namespace sub::ui {

class PagedRows : public QIdentityProxyModel {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QAbstractItemModel* model READ sourceModel WRITE setModel NOTIFY modelChanged)

public:
    explicit PagedRows(QObject* parent = nullptr) : QIdentityProxyModel(parent) {}

    void setModel(QAbstractItemModel* model);

    // The view is near its end: the next page, if there is one.
    Q_INVOKABLE void fetchMore();
    Q_INVOKABLE bool hasMore() const;

    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

Q_SIGNALS:
    void modelChanged();
};

}  // namespace sub::ui
