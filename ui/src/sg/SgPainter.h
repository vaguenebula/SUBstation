#pragma once

// SgPainter: a QPainter-like recorder for the Qt Quick scene graph, so QPainter
// code ports to the GPU almost line for line. An SgCanvas hands one to its
// paint(); what is drawn becomes scene-graph nodes in paint order: solid
// geometry (rects, lines, polygons, arcs, waveform columns) batched into as few
// vertex-coloured triangle nodes as possible, text and images as textured
// nodes between them.
//
// Differences from QPainter worth knowing when porting:
// - No pen or brush state: each call takes its colour (and a pen's width and
//   cap). Antialiasing is state (setAntialiasing, saved and restored), off by
//   default as in QPainter; it feathers edges with a 1-pixel ramp.
// - Transforms are translations only; clips are rectangles only (setClipRect
//   intersects). Clipping is done on the CPU, so it never splits a batch.
// - Angles (drawArc) are in degrees, not sixteenths; 0 is 3 o'clock and
//   positive turns counter-clockwise, as in QPainter.
// - Without antialiasing, lines and outlines land on whole pixels the way
//   QPainter's aliased rendering puts them (a 1 px line at x = 10 fills the
//   column from 10 to 11; drawRect(QRectF(0, 0, 10, 10)) covers 11 x 11
//   pixels), so pixel-exact QPainter code looks the same.
// - fillRect, fillColumns and fillToBaseline are never antialiased: a pixel is
//   filled when its centre is inside, so rects stay crisp at any zoom.
// - drawText(rect, flags, ...) clips to the rect unless Qt::TextDontClip, as
//   QPainter does. Text is rendered once per (text, font, colour) at the
//   window's device pixel ratio and cached as a texture.

#include <QColor>
#include <QFont>
#include <QImage>
#include <QLinearGradient>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QtQuick/qsggeometry.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace sub::ui {

class SgTextureCache;
struct SgTexture;

// What one paint() recorded: vertices for solid geometry, and segments in paint
// order. An SgCanvas keeps one between frames so its memory is reused.
struct SgRecording {
    enum class Kind : std::uint8_t { Solid, Texture };
    struct Segment {
        Kind kind = Kind::Solid;
        int first = 0;  // Solid: the first vertex in `vertices`
        int count = 0;  // Solid: how many (triangles: a multiple of 3)
        std::shared_ptr<SgTexture> texture;  // Texture: what to draw,
        QRectF target;                       // where (item coordinates),
        QRectF source;                       // which part of it (texture pixels),
        float opacity = 1.0f;
        bool smooth = true;  // linear filtering (else nearest)
    };
    std::vector<QSGGeometry::ColoredPoint2D> vertices;
    std::vector<Segment> segments;

    void clear();
};

class SgPainter {
public:
    // `cache` may be null (no window yet): text and images are then skipped.
    SgPainter(SgRecording& recording, const QSizeF& size, qreal devicePixelRatio, SgTextureCache* cache);
    SgPainter(const SgPainter&) = delete;
    SgPainter& operator=(const SgPainter&) = delete;
    ~SgPainter();

    // --- State ------------------------------------------------------------

    QSizeF size() const { return size_; }
    QRectF rect() const { return QRectF(QPointF(), size_); }  // the item, in item coordinates
    qreal devicePixelRatio() const { return dpr_; }

    void save();
    void restore();
    void translate(qreal dx, qreal dy);
    void translate(const QPointF& offset) { translate(offset.x(), offset.y()); }
    QPointF translation() const { return state_.offset; }
    // Intersects the clip with `rect` (in the current coordinates).
    void setClipRect(const QRectF& rect);
    bool hasClip() const { return state_.clipped; }
    QRectF clipRect() const;  // in the current coordinates; the item's rect when unclipped
    void setOpacity(qreal opacity);  // multiplies everything drawn after it (as QPainter's)
    qreal opacity() const { return state_.opacity; }
    void setAntialiasing(bool on) { state_.antialias = on; }
    bool antialiasing() const { return state_.antialias; }

    // --- Solid geometry -------------------------------------------------------

    void fillRect(const QRectF& rect, const QColor& color);
    // A linear gradient: exact for vertical or horizontal ones (any number of
    // stops); other directions interpolate between the corners.
    void fillRect(const QRectF& rect, const QLinearGradient& gradient);
    // The outline of `rect`, a pen of `width` centred on its edges (as QPainter::drawRect).
    void drawRect(const QRectF& rect, const QColor& color, qreal width = 1.0);
    void fillRoundedRect(const QRectF& rect, qreal xRadius, qreal yRadius, const QColor& color);
    void drawRoundedRect(const QRectF& rect, qreal xRadius, qreal yRadius, const QColor& color, qreal width = 1.0);
    // A line; `cap` as QPen's (its default is Qt::SquareCap).
    void drawLine(const QPointF& from, const QPointF& to, const QColor& color, qreal width = 1.0,
                  Qt::PenCapStyle cap = Qt::SquareCap);
    void drawLine(qreal x1, qreal y1, qreal x2, qreal y2, const QColor& color, qreal width = 1.0,
                  Qt::PenCapStyle cap = Qt::SquareCap) {
        drawLine(QPointF(x1, y1), QPointF(x2, y2), color, width, cap);
    }
    // Joined segments (miter joins, bevelled where sharper than 120 degrees).
    void drawPolyline(const QPointF* points, int count, const QColor& color, qreal width = 1.0,
                      Qt::PenCapStyle cap = Qt::SquareCap);
    void drawPolyline(const QPolygonF& points, const QColor& color, qreal width = 1.0,
                      Qt::PenCapStyle cap = Qt::SquareCap) {
        drawPolyline(points.constData(), int(points.size()), color, width, cap);
    }
    // A closed outline through the points.
    void drawPolygon(const QPointF* points, int count, const QColor& color, qreal width = 1.0);
    void drawPolygon(const QPolygonF& points, const QColor& color, qreal width = 1.0) {
        drawPolygon(points.constData(), int(points.size()), color, width);
    }
    // A simple (not self-intersecting) polygon: a fan when convex, else ear clipping.
    void fillPolygon(const QPointF* points, int count, const QColor& color);
    void fillPolygon(const QPolygonF& points, const QColor& color) {
        fillPolygon(points.constData(), int(points.size()), color);
    }
    // The area between a curve whose x only increases and the line y = baseY
    // (an envelope's area): a strip of quads, cheaper than fillPolygon. No antialiasing.
    void fillToBaseline(const QPointF* points, int count, qreal baseY, const QColor& color);
    void fillEllipse(const QRectF& rect, const QColor& color);
    void fillEllipse(const QPointF& center, qreal rx, qreal ry, const QColor& color) {
        fillEllipse(QRectF(center.x() - rx, center.y() - ry, 2 * rx, 2 * ry), color);
    }
    void drawEllipse(const QRectF& rect, const QColor& color, qreal width = 1.0);
    // A thick arc of the ellipse in `rect`, from `startDegrees` through `spanDegrees`.
    void drawArc(const QRectF& rect, qreal startDegrees, qreal spanDegrees, const QColor& color, qreal width = 1.0,
                 Qt::PenCapStyle cap = Qt::FlatCap);
    // Waveforms: column i spans x0 + i * dx to x0 + (i + 1) * dx, and y from
    // y0[i] to y1[i] (either order). One quad per column, all in one batch;
    // a column shorter than `minHeight` is made that tall about its middle.
    void fillColumns(qreal x0, qreal dx, const float* y0, const float* y1, int count, const QColor& color,
                     qreal minHeight = 0.0);

    // --- Text and images ------------------------------------------------------

    // As QPainter::drawText(rect, flags, text): alignment flags (Qt::AlignLeft,
    // AlignHCenter, AlignRight, AlignTop, AlignVCenter, AlignBottom), Qt::TextWordWrap,
    // Qt::TextDontClip; several lines with "\n".
    void drawText(const QRectF& rect, int flags, const QString& text, const QColor& color, const QFont& font);
    // As QPainter::drawText(point, text): the baseline starts at `baseline`.
    void drawText(const QPointF& baseline, const QString& text, const QColor& color, const QFont& font);
    // `image` scaled into `target`; `source` in image pixels (all of it by default).
    void drawImage(const QRectF& target, const QImage& image, const QRectF& source = QRectF(), bool smooth = true);

    // Text measuring, as QFontMetricsF (any thread).
    static qreal textWidth(const QString& text, const QFont& font);
    static QString elidedText(const QString& text, const QFont& font, qreal width,
                              Qt::TextElideMode mode = Qt::ElideRight);

private:
    using Vertex = QSGGeometry::ColoredPoint2D;
    struct Rgba {
        std::uint8_t r = 0, g = 0, b = 0, a = 0;
        Rgba scaled(float f) const;
    };
    struct State {
        QPointF offset;
        QRectF clip;  // item coordinates
        bool clipped = false;
        float opacity = 1.0f;
        bool antialias = false;
    };
    struct P {
        double x, y;
    };

    Rgba premultiplied(const QColor& color) const;
    void beginSolid();
    void closeSolid();
    void push(const Vertex& v) { recording_.vertices.push_back(v); }
    void triangle(const Vertex& a, const Vertex& b, const Vertex& c);
    void quad(const Vertex& a, const Vertex& b, const Vertex& c, const Vertex& d);
    void clippedTriangle(const Vertex& a, const Vertex& b, const Vertex& c);
    void rectItem(double x0, double y0, double x1, double y1, const Rgba& color);  // item coordinates
    static Vertex vertex(double x, double y, const Rgba& c);
    double px() const { return 1.0 / dpr_; }  // one device pixel, in item coordinates
    double snapAliased(double v) const;

    // Strokes and fills in item coordinates.
    void stroke(std::vector<P>& points, bool closed, double width, Qt::PenCapStyle cap, const Rgba& color);
    void strokeCap(const P& at, const P& dir, double h, double hc, double ho, Qt::PenCapStyle cap, const Rgba& c,
                   const Rgba& clear);
    void fillConvex(const std::vector<P>& outline, const Rgba& color);
    void fillConvexAntialiased(const std::vector<P>& inner, const std::vector<P>& outer, const Rgba& color);
    void fillConcave(const std::vector<P>& outline, const Rgba& color);
    void roundedRectOutline(std::vector<P>& out, double x0, double y0, double x1, double y1, double rx, double ry,
                            int perCorner) const;
    void ellipseOutline(std::vector<P>& out, double cx, double cy, double rx, double ry, int steps) const;
    int arcSegments(double radius, double spanRadians) const;
    void drawTexture(const std::shared_ptr<SgTexture>& texture, const QRectF& target, const QRectF& source,
                     bool smooth, const QRectF* clipTo);
    void textAt(const QPointF& topLeft, const std::shared_ptr<SgTexture>& texture, const QRectF* clipTo);

    SgRecording& recording_;
    QSizeF size_;
    qreal dpr_;
    SgTextureCache* cache_;
    State state_;
    std::vector<State> saved_;
    bool solidOpen_ = false;
    std::vector<P> scratch_;
    std::vector<P> scratch2_;
};

}  // namespace sub::ui
