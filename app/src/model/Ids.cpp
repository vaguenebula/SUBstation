#include "model/Ids.h"

#include <QUuid>

namespace sub::app {

QString newId() {
    return QString::fromLatin1(QUuid::createUuid().toRfc4122().toHex().left(12));
}

}  // namespace sub::app
