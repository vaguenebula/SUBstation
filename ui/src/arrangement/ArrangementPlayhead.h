#pragma once

// The playhead while playing, over the lanes (a line) or the ruler (a
// triangle at its foot): an item of its own, as it moves every frame. Stopped,
// it doesn't show (only the start marker does).

#include "arrangement/ArrangementItem.h"

#include <QtQml/qqmlregistration.h>

namespace sub::ui {

class ArrangementPlayhead : public ArrangementItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool ruler READ ruler WRITE setRuler NOTIFY rulerChanged)

public:
    explicit ArrangementPlayhead(QQuickItem* parent = nullptr);

    bool ruler() const { return ruler_; }
    void setRuler(bool ruler);

Q_SIGNALS:
    void rulerChanged();

protected:
    void paint(SgPainter& painter) override;
    void connectArrangement(Arrangement* arrangement) override;

private:
    bool ruler_ = false;
};

}  // namespace sub::ui
