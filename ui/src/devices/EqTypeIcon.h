#pragma once

// A filter type's button face (eq.py's TypeButton): its shape drawn small,
// in the band's colour while checked.

#include "sg/SgCanvas.h"

#include <QColor>
#include <QtQml/qqmlregistration.h>

namespace sub::ui {

class EqTypeIcon : public SgCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int kind READ kind WRITE setKind NOTIFY lookChanged)  // EqGraph::Type
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY lookChanged)
    Q_PROPERTY(bool checked READ checked WRITE setChecked NOTIFY lookChanged)
    Q_PROPERTY(bool hovered READ hovered WRITE setHovered NOTIFY lookChanged)

public:
    explicit EqTypeIcon(QQuickItem* parent = nullptr);

    int kind() const { return kind_; }
    void setKind(int kind);
    QColor color() const { return color_; }
    void setColor(const QColor& color);
    bool checked() const { return checked_; }
    void setChecked(bool checked);
    bool hovered() const { return hovered_; }
    void setHovered(bool hovered);

Q_SIGNALS:
    void lookChanged();

protected:
    void paint(SgPainter& painter) override;
    void itemChange(ItemChange change, const ItemChangeData& data) override;

private:
    int kind_ = 0;
    QColor color_;
    bool checked_ = false;
    bool hovered_ = false;
};

}  // namespace sub::ui
