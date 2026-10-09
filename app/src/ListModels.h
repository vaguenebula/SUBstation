#pragma once
// What the application's list models share.

#include <QAbstractItemModel>
#include <QVariantMap>

namespace sub::app {

// Every role of a list model's row, by its name (roleNames()): what a model's
// get(row) hands QML. Empty for a row that isn't there.
QVariantMap rowMap(const QAbstractItemModel& model, int row);

}  // namespace sub::app
