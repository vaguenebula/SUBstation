#pragma once

// A Gate key filter type's button face: its shape drawn small (low shelf, bell,
// high shelf, low-pass, band-pass, high-pass: the device's S/C EQ Type, in
// Live's order), in the accent while checked.
// (EqTypeIcon draws the EQ's own types; this is the Gate's six, in its order.)

#include "sg/SgCanvas.h"

#include <QtQml/qqmlregistration.h>

namespace sub::ui {

class GateFilterIcon : public SgCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int type READ type WRITE setType NOTIFY lookChanged)  // 0..5, as sc_eq_type
    Q_PROPERTY(bool checked READ checked WRITE setChecked NOTIFY lookChanged)
    Q_PROPERTY(bool hovered READ hovered WRITE setHovered NOTIFY lookChanged)

public:
    explicit GateFilterIcon(QQuickItem* parent = nullptr);

    int type() const { return type_; }
    void setType(int type);
    bool checked() const { return checked_; }
    void setChecked(bool checked);
    bool hovered() const { return hovered_; }
    void setHovered(bool hovered);

Q_SIGNALS:
    void lookChanged();

protected:
    void paint(SgPainter& painter) override;

private:
    int type_ = 5;
    bool checked_ = false;
    bool hovered_ = false;
};

}  // namespace sub::ui
