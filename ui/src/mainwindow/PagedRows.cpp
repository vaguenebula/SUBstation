#include "PagedRows.h"

namespace sub::ui {

void PagedRows::setModel(QAbstractItemModel* model) {
    if (model == sourceModel()) return;
    setSourceModel(model);
    Q_EMIT modelChanged();
}

void PagedRows::fetchMore() {
    if (hasMore()) sourceModel()->fetchMore(QModelIndex());
}

bool PagedRows::hasMore() const { return sourceModel() && sourceModel()->canFetchMore(QModelIndex()); }

// (Views don't fetch by themselves: they ask, with fetchMore().)
bool PagedRows::canFetchMore(const QModelIndex&) const { return false; }

void PagedRows::fetchMore(const QModelIndex&) {}

}  // namespace sub::ui
