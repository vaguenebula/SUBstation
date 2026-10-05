#include "sg/SgCanvas.h"

#include "sg/SgPainter.h"
#include "sg/SgTextureCache.h"

#include <QElapsedTimer>
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGOpacityNode>
#include <QSGSimpleTextureNode>
#include <QSGVertexColorMaterial>

#include <cstring>

namespace sub::ui {

namespace {

enum class NodeKind : std::uint8_t {
    Solid,         // a QSGGeometryNode of vertex-coloured triangles
    Texture,       // a QSGSimpleTextureNode
    FadedTexture,  // a QSGOpacityNode holding a QSGSimpleTextureNode
};

// The item's node: its children are the batches in paint order. It holds the
// textures they draw, so the cache can't free one still on screen.
class RootNode : public QSGNode {
public:
    std::vector<NodeKind> kinds;
    std::vector<std::shared_ptr<SgTexture>> held;
};

QSGNode* makeNode(NodeKind kind) {
    if (kind == NodeKind::Solid) {
        auto* node = new QSGGeometryNode;
        auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        node->setMaterial(new QSGVertexColorMaterial);
        node->setFlag(QSGNode::OwnsMaterial);
        return node;
    }
    auto* texture = new QSGSimpleTextureNode;
    texture->setOwnsTexture(false);
    if (kind == NodeKind::Texture)
        return texture;
    auto* opacity = new QSGOpacityNode;
    opacity->appendChildNode(texture);
    return opacity;
}

void removeFrom(RootNode* root, QSGNode* child, size_t index) {
    while (child) {
        QSGNode* next = child->nextSibling();
        root->removeChildNode(child);
        delete child;
        child = next;
    }
    root->kinds.resize(index);
}

void build(RootNode* root, const SgRecording& recording, SgCanvas::Stats& stats) {
    std::vector<std::shared_ptr<SgTexture>> held;
    QSGNode* child = root->firstChild();
    size_t index = 0;
    stats.solidNodes = stats.textureNodes = stats.vertices = 0;
    for (const SgRecording::Segment& segment : recording.segments) {
        const NodeKind kind = segment.kind == SgRecording::Kind::Solid
                                  ? NodeKind::Solid
                                  : (segment.opacity < 1.0f ? NodeKind::FadedTexture : NodeKind::Texture);
        if (child && root->kinds[index] != kind) {
            // The order of kinds changed from here on: new nodes for the rest.
            removeFrom(root, child, index);
            child = nullptr;
        }
        if (!child) {
            child = makeNode(kind);
            ++stats.nodesMade;
            root->appendChildNode(child);
            root->kinds.push_back(kind);
        }
        if (kind == NodeKind::Solid) {
            auto* node = static_cast<QSGGeometryNode*>(child);
            QSGGeometry* geometry = node->geometry();
            const auto* source = recording.vertices.data() + segment.first;
            const size_t bytes = size_t(segment.count) * sizeof(QSGGeometry::ColoredPoint2D);
            // Unchanged since the last frame: nothing to upload.
            if (geometry->vertexCount() != segment.count || std::memcmp(geometry->vertexData(), source, bytes) != 0) {
                if (geometry->vertexCount() != segment.count)
                    geometry->allocate(segment.count);
                std::memcpy(geometry->vertexData(), source, bytes);
                node->markDirty(QSGNode::DirtyGeometry);
            }
            ++stats.solidNodes;
            stats.vertices += segment.count;
        } else {
            auto* node = static_cast<QSGSimpleTextureNode*>(kind == NodeKind::Texture ? child : child->firstChild());
            if (node->texture() != segment.texture->texture)
                node->setTexture(segment.texture->texture);
            node->setRect(segment.target);
            node->setSourceRect(segment.source);
            node->setFiltering(segment.smooth ? QSGTexture::Linear : QSGTexture::Nearest);
            if (kind == NodeKind::FadedTexture)
                static_cast<QSGOpacityNode*>(child)->setOpacity(segment.opacity);
            held.push_back(segment.texture);
            ++stats.textureNodes;
        }
        child = child->nextSibling();
        ++index;
    }
    removeFrom(root, child, index);
    root->held.swap(held);  // the last frame's textures are let go only now
}

}  // namespace

SgCanvas::SgCanvas(QQuickItem* parent) : QQuickItem(parent), recording_(std::make_unique<SgRecording>()) {
    setFlag(ItemHasContents);
}

SgCanvas::~SgCanvas() = default;

QSGNode* SgCanvas::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    auto* root = static_cast<RootNode*>(oldNode);
    if (!root)
        root = new RootNode;
    QQuickWindow* window = this->window();
    const qreal dpr = window ? window->effectiveDevicePixelRatio() : 1.0;
    QElapsedTimer timer;
    timer.start();
    {
        SgPainter painter(*recording_, size(), dpr, SgTextureCache::forWindow(window));
        if (width() > 0 && height() > 0)
            paint(painter);
    }
    stats_.paintNs = timer.nsecsElapsed();
    timer.restart();
    build(root, *recording_, stats_);
    // The nodes hold the textures now. The item mustn't: it may outlive the
    // scene graph, and textures go only on the render thread, before it does.
    recording_->clear();
    stats_.buildNs = timer.nsecsElapsed();
    ++stats_.frames;
    return root;
}

bool SgCanvas::event(QEvent* event) {
    const bool handled = QQuickItem::event(event);
    if (event->type() == QEvent::MouseButtonPress && event->isAccepted())
        setKeepMouseGrab(true);
    else if (event->type() == QEvent::MouseButtonRelease)
        setKeepMouseGrab(false);
    return handled;
}

void SgCanvas::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        update();
}

}  // namespace sub::ui
