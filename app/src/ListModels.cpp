#include "ListModels.h"

#include <QHash>

namespace sub::app {

QVariantMap rowMap(const QAbstractItemModel& model, int row) {
    QVariantMap out;
    if (row < 0 || row >= model.rowCount()) return out;
    const QModelIndex index = model.index(row, 0);
    const QHash<int, QByteArray> names = model.roleNames();
    for (auto it = names.cbegin(); it != names.cend(); ++it) {
        out.insert(QString::fromUtf8(it.value()), model.data(index, it.key()));
    }
    return out;
}

}  // namespace sub::app
