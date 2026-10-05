#pragma once

// SgCanvas: the base of every custom-drawn item (lanes, rulers, the piano roll,
// envelopes, meters, knobs, curves). A subclass implements paint(SgPainter&) the
// way a widget implemented paintEvent with a QPainter, and calls update() when
// it must be drawn again; updatePaintNode() runs paint() and turns what it drew
// into scene-graph nodes, reusing last frame's nodes and buffers.
//
// Threading: paint() runs on the scene graph's render thread, during the sync
// step, while the GUI thread is blocked. It may read anything (the item's own
// state, the application layer's models), but must not change anything: no
// property writes, no signals, no JavaScript, no QObject creation. Work out what
// to draw on the GUI thread (in setters and slots) and keep it in members;
// paint() only reads them.
//
// Performance: everything an item draws is redrawn when it repaints, so split
// what changes often (a playhead, a meter) into an item of its own, above or
// below the static part. Within an item, clip with SgPainter::setClipRect
// rather than QML's `clip: true` on parts of it, which breaks batching. Tens of
// thousands of rects a frame are fine (see test_ui_sg.cpp's benchmark).
//
// An item draws where its geometry says, unclipped: what it scrolls past its
// edges (a selected range, notes, a playhead) would show over its neighbours,
// the browser too. So an item whose content scrolls (the arrangement's lanes,
// ruler and bus lanes, the piano roll's parts, the clip view's waveforms) has
// `clip: true` itself in QML: a scissor for the item and its children, cheap.

#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

#include <memory>

namespace sub::ui {

class SgPainter;
struct SgRecording;

class SgCanvas : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("SgCanvas is a base class for C++ items")

public:
    // What the last frame drew (read on the GUI thread, after it rendered).
    struct Stats {
        int solidNodes = 0;    // vertex-coloured triangle batches
        int textureNodes = 0;  // text and images
        int vertices = 0;
        qint64 paintNs = 0;    // in paint()
        qint64 buildNs = 0;    // turning it into nodes
        int frames = 0;        // how many times it painted
        int nodesMade = 0;     // nodes made so far (the rest were reused)
    };

    explicit SgCanvas(QQuickItem* parent = nullptr);
    ~SgCanvas() override;

    Stats lastStats() const { return stats_; }

protected:
    // Draw the item, in item coordinates (0, 0 to width, height). Render thread; read only.
    virtual void paint(SgPainter& painter) = 0;

    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    // A press the item takes keeps the mouse till it is let go: a Flickable (or
    // list) around it (the clip view's controls, a rack's chains) doesn't take a
    // knob's or a graph's drag over as a scroll once it passes the drag
    // distance, as no scroll area took a widget's drag.
    bool event(QEvent* event) override;

private:
    std::unique_ptr<SgRecording> recording_;
    Stats stats_;
};

}  // namespace sub::ui
