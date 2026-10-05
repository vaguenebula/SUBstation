#pragma once

// The kick's spectrum against the input's, and where they clash
// (sidechain.py's ClashView): the kick in orange, the input in blue, in pink
// where both are loud, the clash band marked. It draws its CurveGraph's fit; a
// click fits the curve (CurveGraph::fitNow).

#include "devices/CurveGraph.h"
#include "sg/SgCanvas.h"

#include <QPointer>
#include <QtQml/qqmlregistration.h>

namespace sub::ui {

class ClashView : public SgCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(sub::ui::CurveGraph* graph READ graph WRITE setGraph NOTIFY graphChanged)

public:
    static constexpr int kMinimumHeight = 44;

    explicit ClashView(QQuickItem* parent = nullptr);

    CurveGraph* graph() const { return graph_; }
    void setGraph(CurveGraph* graph);

Q_SIGNALS:
    void graphChanged();

protected:
    void paint(SgPainter& painter) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    QPointer<CurveGraph> graph_;
};

}  // namespace sub::ui
