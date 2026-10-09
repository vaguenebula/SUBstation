#include "sg/SgPainter.h"

#include "sg/SgTextureCache.h"

#include <QFontMetricsF>
#include <QSGTexture>

#include <algorithm>
#include <cmath>

namespace sub::ui {

namespace {

constexpr double kPi = 3.14159265358979323846;
// A join turning more than 120 degrees is bevelled instead of mitred (its miter
// would reach more than twice the half-width out).
constexpr double kMiterLimit = 2.0;
// How far out a mitred corner of an antialiased convex fill may go, in fringes.
constexpr double kFillMiterLimit = 4.0;

double cross(double ax, double ay, double bx, double by) { return ax * by - ay * bx; }

}  // namespace

void SgRecording::clear() {
    vertices.clear();
    segments.clear();
}

SgPainter::Rgba SgPainter::Rgba::scaled(float f) const {
    auto s = [f](std::uint8_t v) { return std::uint8_t(std::lround(std::clamp(v * f, 0.0f, 255.0f))); };
    return {s(r), s(g), s(b), s(a)};
}

SgPainter::SgPainter(SgRecording& recording, const QSizeF& size, qreal devicePixelRatio, SgTextureCache* cache)
    : recording_(recording), size_(size), dpr_(devicePixelRatio > 0 ? devicePixelRatio : 1.0), cache_(cache) {
    recording_.clear();
}

SgPainter::~SgPainter() { closeSolid(); }

// --- State --------------------------------------------------------------------

void SgPainter::save() { saved_.push_back(state_); }

void SgPainter::restore() {
    if (saved_.empty())
        return;
    state_ = saved_.back();
    saved_.pop_back();
}

void SgPainter::translate(qreal dx, qreal dy) { state_.offset += QPointF(dx, dy); }

void SgPainter::setClipRect(const QRectF& rect) {
    const QRectF item = rect.normalized().translated(state_.offset);
    QRectF clip = state_.clipped ? state_.clip.intersected(item) : item;
    if (clip.width() <= 0 || clip.height() <= 0)
        clip = QRectF(item.topLeft(), QSizeF(0, 0));  // nothing gets through
    state_.clip = clip;
    state_.clipped = true;
}

QRectF SgPainter::clipRect() const {
    const QRectF clip = state_.clipped ? state_.clip : QRectF(QPointF(), size_);
    return clip.translated(-state_.offset);
}

void SgPainter::setOpacity(qreal opacity) { state_.opacity = float(std::clamp(opacity, 0.0, 1.0)); }

SgPainter::Rgba SgPainter::premultiplied(const QColor& color) const {
    const QRgb rgba = color.rgba();
    const float a = qAlpha(rgba) / 255.0f * state_.opacity;
    auto c = [a](int v) { return std::uint8_t(std::lround(v * a)); };
    return {c(qRed(rgba)), c(qGreen(rgba)), c(qBlue(rgba)), std::uint8_t(std::lround(255.0f * a))};
}

// --- Vertices and clipping --------------------------------------------------------

SgPainter::Vertex SgPainter::vertex(double x, double y, const Rgba& c) {
    Vertex v;
    v.set(float(x), float(y), c.r, c.g, c.b, c.a);
    return v;
}

void SgPainter::beginSolid() {
    if (solidOpen_)
        return;
    SgRecording::Segment segment;
    segment.kind = SgRecording::Kind::Solid;
    segment.first = int(recording_.vertices.size());
    recording_.segments.push_back(std::move(segment));
    solidOpen_ = true;
}

void SgPainter::closeSolid() {
    if (!solidOpen_)
        return;
    solidOpen_ = false;
    SgRecording::Segment& segment = recording_.segments.back();
    segment.count = int(recording_.vertices.size()) - segment.first;
    if (segment.count == 0)
        recording_.segments.pop_back();
}

double SgPainter::snapAliased(double v) const { return std::floor(v * dpr_ + 0.5) / dpr_; }

void SgPainter::room(int count) {
    if (solidOpen_ &&
        int(recording_.vertices.size()) - recording_.segments.back().first + count > kMaxSolidVertices) {
        closeSolid();
        beginSolid();
    }
}

void SgPainter::triangle(const Vertex& a, const Vertex& b, const Vertex& c) {
    if (state_.clipped) {
        clippedTriangle(a, b, c);
        return;
    }
    room(3);
    push(a);
    push(b);
    push(c);
}

void SgPainter::quad(const Vertex& a, const Vertex& b, const Vertex& c, const Vertex& d) {
    triangle(a, b, c);
    triangle(a, c, d);
}

void SgPainter::clippedTriangle(const Vertex& a, const Vertex& b, const Vertex& c) {
    const float cx0 = float(state_.clip.left()), cx1 = float(state_.clip.right());
    const float cy0 = float(state_.clip.top()), cy1 = float(state_.clip.bottom());
    const float minX = std::min({a.x, b.x, c.x}), maxX = std::max({a.x, b.x, c.x});
    const float minY = std::min({a.y, b.y, c.y}), maxY = std::max({a.y, b.y, c.y});
    if (maxX <= cx0 || minX >= cx1 || maxY <= cy0 || minY >= cy1)
        return;
    if (minX >= cx0 && maxX <= cx1 && minY >= cy0 && maxY <= cy1) {
        room(3);
        push(a);
        push(b);
        push(c);
        return;
    }
    // Sutherland-Hodgman against the clip's four sides, colours interpolated.
    struct C {
        float x, y, r, g, b, a;
    };
    auto from = [](const Vertex& v) { return C{v.x, v.y, float(v.r), float(v.g), float(v.b), float(v.a)}; };
    C polygon[12] = {from(a), from(b), from(c)};
    C buffer[12];
    int count = 3;
    auto clipSide = [&](auto inside, auto at) {
        int out = 0;
        for (int i = 0; i < count; ++i) {
            const C& p = polygon[i];
            const C& q = polygon[(i + 1) % count];
            const bool pIn = inside(p), qIn = inside(q);
            if (pIn)
                buffer[out++] = p;
            if (pIn != qIn) {
                const float t = at(p, q);
                buffer[out++] = C{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.r + (q.r - p.r) * t,
                                  p.g + (q.g - p.g) * t, p.b + (q.b - p.b) * t, p.a + (q.a - p.a) * t};
            }
        }
        std::copy(buffer, buffer + out, polygon);
        count = out;
    };
    clipSide([&](const C& p) { return p.x >= cx0; }, [&](const C& p, const C& q) { return (cx0 - p.x) / (q.x - p.x); });
    clipSide([&](const C& p) { return p.x <= cx1; }, [&](const C& p, const C& q) { return (cx1 - p.x) / (q.x - p.x); });
    clipSide([&](const C& p) { return p.y >= cy0; }, [&](const C& p, const C& q) { return (cy0 - p.y) / (q.y - p.y); });
    clipSide([&](const C& p) { return p.y <= cy1; }, [&](const C& p, const C& q) { return (cy1 - p.y) / (q.y - p.y); });
    if (count < 3)
        return;
    auto to = [](const C& c) {
        Vertex v;
        auto byte = [](float f) { return std::uint8_t(std::lround(std::clamp(f, 0.0f, 255.0f))); };
        v.set(c.x, c.y, byte(c.r), byte(c.g), byte(c.b), byte(c.a));
        return v;
    };
    const Vertex first = to(polygon[0]);
    room(3 * (count - 2));
    for (int i = 1; i + 1 < count; ++i) {
        push(first);
        push(to(polygon[i]));
        push(to(polygon[i + 1]));
    }
}

void SgPainter::rectItem(double x0, double y0, double x1, double y1, const Rgba& c) {
    if (state_.clipped) {
        x0 = std::max(x0, state_.clip.left());
        y0 = std::max(y0, state_.clip.top());
        x1 = std::min(x1, state_.clip.right());
        y1 = std::min(y1, state_.clip.bottom());
    }
    if (x1 <= x0 || y1 <= y0)
        return;
    const Vertex a = vertex(x0, y0, c), b = vertex(x1, y0, c), d = vertex(x1, y1, c), e = vertex(x0, y1, c);
    room(6);
    push(a);
    push(b);
    push(d);
    push(a);
    push(d);
    push(e);
}

// --- Rects -------------------------------------------------------------------------

void SgPainter::fillRect(const QRectF& rect, const QColor& color) {
    const Rgba c = premultiplied(color);
    if (c.a == 0)
        return;
    beginSolid();
    const QRectF r = rect.normalized().translated(state_.offset);
    rectItem(r.left(), r.top(), r.right(), r.bottom(), c);
}

void SgPainter::fillRect(const QRectF& rect, const QLinearGradient& gradient) {
    const QGradientStops stops = gradient.stops();
    const QRectF r = rect.normalized();
    if (stops.isEmpty() || r.isEmpty())
        return;
    const QPointF s = gradient.start(), e = gradient.finalStop();
    const double gx = e.x() - s.x(), gy = e.y() - s.y();
    const double length2 = gx * gx + gy * gy;
    if (stops.size() == 1 || length2 <= 0.0) {
        fillRect(r, stops.last().second);
        return;
    }
    auto tAt = [&](double x, double y) { return ((x - s.x()) * gx + (y - s.y()) * gy) / length2; };
    auto colorAt = [&](double t) {
        t = std::clamp(t, 0.0, 1.0);
        if (t <= stops.first().first)
            return premultiplied(stops.first().second);
        for (int i = 1; i < stops.size(); ++i) {
            if (t <= stops[i].first) {
                const double t0 = stops[i - 1].first, t1 = stops[i].first;
                const double f = t1 > t0 ? (t - t0) / (t1 - t0) : 1.0;
                const Rgba a = premultiplied(stops[i - 1].second), b = premultiplied(stops[i].second);
                auto mix = [f](std::uint8_t p, std::uint8_t q) { return std::uint8_t(std::lround(p + (q - p) * f)); };
                return Rgba{mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a)};
            }
        }
        return premultiplied(stops.last().second);
    };
    beginSolid();
    const double ox = state_.offset.x(), oy = state_.offset.y();
    if (gx == 0.0 || gy == 0.0) {
        // Along one axis: split the rect where the stops fall, so every stop shows.
        const bool vertical = gx == 0.0;
        const double a0 = vertical ? r.top() : r.left(), a1 = vertical ? r.bottom() : r.right();
        const double origin = vertical ? s.y() : s.x(), span = vertical ? gy : gx;
        std::vector<double> cuts{a0, a1};
        for (const QGradientStop& stop : stops) {
            const double at = origin + stop.first * span;
            if (at > a0 && at < a1)
                cuts.push_back(at);
        }
        std::sort(cuts.begin(), cuts.end());
        for (size_t i = 0; i + 1 < cuts.size(); ++i) {
            const double c0 = cuts[i], c1 = cuts[i + 1];
            if (c1 <= c0)
                continue;
            const Rgba k0 = colorAt((c0 - origin) / span), k1 = colorAt((c1 - origin) / span);
            if (vertical)
                quad(vertex(r.left() + ox, c0 + oy, k0), vertex(r.right() + ox, c0 + oy, k0),
                     vertex(r.right() + ox, c1 + oy, k1), vertex(r.left() + ox, c1 + oy, k1));
            else
                quad(vertex(c0 + ox, r.top() + oy, k0), vertex(c1 + ox, r.top() + oy, k1),
                     vertex(c1 + ox, r.bottom() + oy, k1), vertex(c0 + ox, r.bottom() + oy, k0));
        }
        return;
    }
    auto corner = [&](double x, double y) { return vertex(x + ox, y + oy, colorAt(tAt(x, y))); };
    quad(corner(r.left(), r.top()), corner(r.right(), r.top()), corner(r.right(), r.bottom()),
         corner(r.left(), r.bottom()));
}

void SgPainter::drawRect(const QRectF& rect, const QColor& color, qreal width) {
    const Rgba c = premultiplied(color);
    if (c.a == 0)
        return;
    beginSolid();
    const QRectF r = rect.normalized().translated(state_.offset);
    const double w = width > 0 ? width : px();
    if (!state_.antialias) {
        // On whole pixels, as QPainter's aliased outline: four bars.
        const double h = w / 2;
        const double ox0 = snapAliased(r.left() - h), ox1 = std::max(snapAliased(r.right() + h), ox0 + px());
        const double oy0 = snapAliased(r.top() - h), oy1 = std::max(snapAliased(r.bottom() + h), oy0 + px());
        const double ix0 = std::max(snapAliased(r.left() + h), ox0 + px());
        const double ix1 = std::min(snapAliased(r.right() - h), ox1 - px());
        const double iy0 = std::max(snapAliased(r.top() + h), oy0 + px());
        const double iy1 = std::min(snapAliased(r.bottom() - h), oy1 - px());
        if (ix1 <= ix0 || iy1 <= iy0) {
            rectItem(ox0, oy0, ox1, oy1, c);
            return;
        }
        rectItem(ox0, oy0, ox1, iy0, c);
        rectItem(ox0, iy1, ox1, oy1, c);
        rectItem(ox0, iy0, ix0, iy1, c);
        rectItem(ix1, iy0, ox1, iy1, c);
        return;
    }
    scratch_ = {{r.left(), r.top()}, {r.right(), r.top()}, {r.right(), r.bottom()}, {r.left(), r.bottom()}};
    stroke(scratch_, true, w, Qt::SquareCap, c);
}

void SgPainter::roundedRectOutline(std::vector<P>& out, double x0, double y0, double x1, double y1, double rx,
                                   double ry, int perCorner) const {
    out.clear();
    rx = std::clamp(rx, 0.0, (x1 - x0) / 2);
    ry = std::clamp(ry, 0.0, (y1 - y0) / 2);
    // Clockwise on screen from the top-left corner; every corner has the same
    // number of points (even with no radius), so an inner and an outer outline pair up.
    const double centres[4][2] = {{x0 + rx, y0 + ry}, {x1 - rx, y0 + ry}, {x1 - rx, y1 - ry}, {x0 + rx, y1 - ry}};
    const double starts[4] = {kPi, 1.5 * kPi, 0.0, 0.5 * kPi};
    for (int corner = 0; corner < 4; ++corner) {
        for (int j = 0; j <= perCorner; ++j) {
            const double t = starts[corner] + 0.5 * kPi * j / perCorner;
            out.push_back({centres[corner][0] + rx * std::cos(t), centres[corner][1] + ry * std::sin(t)});
        }
    }
}

void SgPainter::fillRoundedRect(const QRectF& rect, qreal xRadius, qreal yRadius, const QColor& color) {
    const Rgba c = premultiplied(color);
    const QRectF r = rect.normalized().translated(state_.offset);
    if (c.a == 0 || r.isEmpty())
        return;
    if ((xRadius <= 0 || yRadius <= 0) && !state_.antialias) {
        beginSolid();
        rectItem(r.left(), r.top(), r.right(), r.bottom(), c);
        return;
    }
    beginSolid();
    const int perCorner = std::max(1, arcSegments(std::max(xRadius, yRadius) + px(), 0.5 * kPi));
    if (!state_.antialias) {
        roundedRectOutline(scratch_, r.left(), r.top(), r.right(), r.bottom(), xRadius, yRadius, perCorner);
        fillConvex(scratch_, c);
        return;
    }
    const double f = px() / 2;
    roundedRectOutline(scratch_, r.left() + f, r.top() + f, r.right() - f, r.bottom() - f, xRadius - f, yRadius - f,
                       perCorner);
    roundedRectOutline(scratch2_, r.left() - f, r.top() - f, r.right() + f, r.bottom() + f, xRadius + f,
                       yRadius + f, perCorner);
    fillConvexAntialiased(scratch_, scratch2_, c);
}

void SgPainter::drawRoundedRect(const QRectF& rect, qreal xRadius, qreal yRadius, const QColor& color, qreal width) {
    if (xRadius <= 0 || yRadius <= 0) {
        drawRect(rect, color, width);
        return;
    }
    const Rgba c = premultiplied(color);
    if (c.a == 0)
        return;
    beginSolid();
    const QRectF r = rect.normalized().translated(state_.offset);
    const int perCorner = std::max(1, arcSegments(std::max(xRadius, yRadius), 0.5 * kPi));
    roundedRectOutline(scratch_, r.left(), r.top(), r.right(), r.bottom(), xRadius, yRadius, perCorner);
    stroke(scratch_, true, width > 0 ? width : px(), Qt::SquareCap, c);
}

// --- Lines --------------------------------------------------------------------------

void SgPainter::drawLine(const QPointF& from, const QPointF& to, const QColor& color, qreal width,
                         Qt::PenCapStyle cap) {
    const Rgba c = premultiplied(color);
    if (c.a == 0)
        return;
    beginSolid();
    const double w = width > 0 ? width : px();
    const QPointF a = from + state_.offset, b = to + state_.offset;
    if (!state_.antialias && cap != Qt::RoundCap && (a.x() == b.x() || a.y() == b.y())) {
        // Upright or level, without antialiasing: a bar on whole pixels, where
        // QPainter's aliased rendering puts it.
        const double h = w / 2, e = cap == Qt::SquareCap ? h : 0.0;
        double x0, x1, y0, y1;
        if (a.x() == b.x() && a.y() != b.y()) {
            x0 = a.x() - h;
            x1 = a.x() + h;
            y0 = std::min(a.y(), b.y()) - e;
            y1 = std::max(a.y(), b.y()) + e;
        } else {
            x0 = std::min(a.x(), b.x()) - e;
            x1 = std::max(a.x(), b.x()) + e;
            y0 = a.y() - h;
            y1 = a.y() + h;
        }
        double sx0 = snapAliased(x0), sx1 = snapAliased(x1), sy0 = snapAliased(y0), sy1 = snapAliased(y1);
        if (x1 > x0 && sx1 <= sx0)
            sx1 = sx0 + px();
        if (y1 > y0 && sy1 <= sy0)
            sy1 = sy0 + px();
        rectItem(sx0, sy0, sx1, sy1, c);
        return;
    }
    scratch_ = {{a.x(), a.y()}, {b.x(), b.y()}};
    stroke(scratch_, false, w, cap, c);
}

void SgPainter::drawPolyline(const QPointF* points, int count, const QColor& color, qreal width,
                             Qt::PenCapStyle cap) {
    const Rgba c = premultiplied(color);
    if (c.a == 0 || count < 1)
        return;
    beginSolid();
    scratch_.resize(size_t(count));
    for (int i = 0; i < count; ++i)
        scratch_[size_t(i)] = {points[i].x() + state_.offset.x(), points[i].y() + state_.offset.y()};
    stroke(scratch_, false, width > 0 ? width : px(), cap, c);
}

void SgPainter::drawPolygon(const QPointF* points, int count, const QColor& color, qreal width) {
    const Rgba c = premultiplied(color);
    if (c.a == 0 || count < 2)
        return;
    beginSolid();
    scratch_.resize(size_t(count));
    for (int i = 0; i < count; ++i)
        scratch_[size_t(i)] = {points[i].x() + state_.offset.x(), points[i].y() + state_.offset.y()};
    stroke(scratch_, true, width > 0 ? width : px(), Qt::SquareCap, c);
}

void SgPainter::stroke(std::vector<P>& points, bool closed, double width, Qt::PenCapStyle cap, const Rgba& color) {
    // Repeated points make no segment.
    size_t n = 0;
    for (size_t i = 0; i < points.size(); ++i) {
        if (n > 0 && std::abs(points[i].x - points[n - 1].x) < 1e-6 && std::abs(points[i].y - points[n - 1].y) < 1e-6)
            continue;
        points[n++] = points[i];
    }
    points.resize(n);
    if (closed && n > 1 && std::abs(points[0].x - points[n - 1].x) < 1e-6 &&
        std::abs(points[0].y - points[n - 1].y) < 1e-6)
        points.resize(--n);
    if (n == 0)
        return;

    const bool aa = state_.antialias;
    Rgba c = color;
    double w = width;
    if (aa && w < px()) {  // thinner than a pixel: a pixel, fainter
        c = c.scaled(float(w / px()));
        w = px();
    }
    const double h = w / 2;
    const double hc = aa ? std::max(0.0, h - px() / 2) : h;  // the solid core's half-width
    const double ho = h + px() / 2;                          // the feathered edge's
    const Rgba clear{};

    if (n == 1) {  // a point: a square or a dot as wide as the pen
        if (closed || cap == Qt::FlatCap)
            return;
        const P p = points[0];
        if (cap == Qt::RoundCap) {
            strokeCap(p, {1, 0}, h, hc, ho, Qt::RoundCap, c, clear);
            strokeCap(p, {-1, 0}, h, hc, ho, Qt::RoundCap, c, clear);
        } else if (!aa) {
            rectItem(snapAliased(p.x - h), snapAliased(p.y - h), std::max(snapAliased(p.x + h), snapAliased(p.x - h) + px()),
                     std::max(snapAliased(p.y + h), snapAliased(p.y - h) + px()), c);
        } else {
            std::vector<P> inner{{p.x - h + px() / 2, p.y - h + px() / 2}, {p.x + h - px() / 2, p.y - h + px() / 2},
                                 {p.x + h - px() / 2, p.y + h - px() / 2}, {p.x - h + px() / 2, p.y + h - px() / 2}};
            std::vector<P> outer{{p.x - ho, p.y - ho}, {p.x + ho, p.y - ho}, {p.x + ho, p.y + ho}, {p.x - ho, p.y + ho}};
            fillConvexAntialiased(inner, outer, c);
        }
        return;
    }
    if (closed && n < 3)
        closed = false;

    const size_t segments = closed ? n : n - 1;
    struct Segment {
        P d, normal;
    };
    std::vector<Segment> segs(segments);
    for (size_t i = 0; i < segments; ++i) {
        const P a = points[i], b = points[(i + 1) % n];
        const double dx = b.x - a.x, dy = b.y - a.y;
        const double length = std::sqrt(dx * dx + dy * dy);
        segs[i].d = {dx / length, dy / length};
        segs[i].normal = {-dy / length, dx / length};
    }
    auto before = [&](size_t v) -> const Segment& { return segs[(v + segments - 1) % segments]; };
    auto after = [&](size_t v) -> const Segment& { return segs[v % segments]; };

    // Joins: a miter where the turn is mild, else a bevel.
    std::vector<P> miter(n);
    std::vector<char> mitred(n, 0);
    for (size_t v = 0; v < n; ++v) {
        if (!closed && (v == 0 || v == n - 1))
            continue;
        const P na = before(v).normal, nb = after(v).normal;
        const double mx = na.x + nb.x, my = na.y + nb.y;
        const double m2 = mx * mx + my * my;  // (2 cos(turn / 2))^2
        if (m2 > 4.0 / (kMiterLimit * kMiterLimit)) {
            miter[v] = {2 * mx / m2, 2 * my / m2};
            mitred[v] = 1;
        }
    }

    // Flat and square ends: how far past the end point the line goes (square:
    // half the width). Antialiased, the solid part stops half a pixel short and
    // the end's feather goes on from there.
    const double reach = cap == Qt::SquareCap ? h : 0.0;
    const double endShift = cap == Qt::RoundCap ? 0.0 : (aa ? reach - px() / 2 : reach);

    for (size_t i = 0; i < segments; ++i) {
        P a = points[i], b = points[(i + 1) % n];
        const Segment& s = segs[i];
        const P oa = mitred[i] ? miter[i] : s.normal;
        const P ob = mitred[(i + 1) % n] ? miter[(i + 1) % n] : s.normal;
        if (!closed && i == 0) {
            a.x -= s.d.x * endShift;
            a.y -= s.d.y * endShift;
        }
        if (!closed && i == segments - 1) {
            b.x += s.d.x * endShift;
            b.y += s.d.y * endShift;
        }
        if (hc > 0)
            quad(vertex(a.x + oa.x * hc, a.y + oa.y * hc, c), vertex(b.x + ob.x * hc, b.y + ob.y * hc, c),
                 vertex(b.x - ob.x * hc, b.y - ob.y * hc, c), vertex(a.x - oa.x * hc, a.y - oa.y * hc, c));
        if (aa) {
            for (double side : {1.0, -1.0}) {
                quad(vertex(a.x + side * oa.x * hc, a.y + side * oa.y * hc, c),
                     vertex(b.x + side * ob.x * hc, b.y + side * ob.y * hc, c),
                     vertex(b.x + side * ob.x * ho, b.y + side * ob.y * ho, clear),
                     vertex(a.x + side * oa.x * ho, a.y + side * oa.y * ho, clear));
            }
        }
    }

    // Bevels: fill the wedge on the outside of each sharp turn.
    for (size_t v = 0; v < n; ++v) {
        if ((!closed && (v == 0 || v == n - 1)) || mitred[v])
            continue;
        const Segment& sa = before(v);
        const Segment& sb = after(v);
        const double side = cross(sa.d.x, sa.d.y, sb.d.x, sb.d.y) > 0 ? -1.0 : 1.0;
        const P p = points[v];
        const P na{sa.normal.x * side, sa.normal.y * side}, nb{sb.normal.x * side, sb.normal.y * side};
        if (hc > 0)
            triangle(vertex(p.x, p.y, c), vertex(p.x + na.x * hc, p.y + na.y * hc, c),
                     vertex(p.x + nb.x * hc, p.y + nb.y * hc, c));
        if (aa)
            quad(vertex(p.x + na.x * hc, p.y + na.y * hc, c), vertex(p.x + na.x * ho, p.y + na.y * ho, clear),
                 vertex(p.x + nb.x * ho, p.y + nb.y * ho, clear), vertex(p.x + nb.x * hc, p.y + nb.y * hc, c));
    }

    if (!closed) {
        const P startOut{-segs.front().d.x, -segs.front().d.y};
        strokeCap(points.front(), startOut, h, hc, ho, cap, c, clear);
        strokeCap(points.back(), segs.back().d, h, hc, ho, cap, c, clear);
    }
}

void SgPainter::strokeCap(const P& at, const P& dir, double h, double hc, double ho, Qt::PenCapStyle cap,
                          const Rgba& c, const Rgba& clear) {
    const P normal{-dir.y, dir.x};
    if (cap == Qt::RoundCap) {
        // A half disc from one side round the end to the other.
        const int steps = std::max(4, arcSegments(ho, kPi));
        const double start = std::atan2(normal.y, normal.x);
        const double turn = std::atan2(dir.y, dir.x) - start;  // +-90 degrees: which way round
        const double sweep = (std::sin(turn) >= 0 ? 1.0 : -1.0) * kPi;
        for (int j = 0; j < steps; ++j) {
            const double t0 = start + sweep * j / steps, t1 = start + sweep * (j + 1) / steps;
            const double c0 = std::cos(t0), s0 = std::sin(t0), c1 = std::cos(t1), s1 = std::sin(t1);
            if (hc > 0)
                triangle(vertex(at.x, at.y, c), vertex(at.x + c0 * hc, at.y + s0 * hc, c),
                         vertex(at.x + c1 * hc, at.y + s1 * hc, c));
            if (state_.antialias)
                quad(vertex(at.x + c0 * hc, at.y + s0 * hc, c), vertex(at.x + c0 * ho, at.y + s0 * ho, clear),
                     vertex(at.x + c1 * ho, at.y + s1 * ho, clear), vertex(at.x + c1 * hc, at.y + s1 * hc, c));
        }
        return;
    }
    if (!state_.antialias)
        return;  // the line already reaches its end
    // The end's feather: a pixel wide, across the line and its side feathers.
    const double reach = (cap == Qt::SquareCap ? h : 0.0) - px() / 2;
    const P q{at.x + dir.x * reach, at.y + dir.y * reach};
    const P e{q.x + dir.x * px(), q.y + dir.y * px()};
    auto at2 = [&](const P& base, double offset, const Rgba& k) {
        return vertex(base.x + normal.x * offset, base.y + normal.y * offset, k);
    };
    quad(at2(q, ho, clear), at2(q, hc, c), at2(e, hc, clear), at2(e, ho, clear));
    quad(at2(q, hc, c), at2(q, -hc, c), at2(e, -hc, clear), at2(e, hc, clear));
    quad(at2(q, -hc, c), at2(q, -ho, clear), at2(e, -ho, clear), at2(e, -hc, clear));
}

// --- Polygons and ellipses ---------------------------------------------------------

int SgPainter::arcSegments(double radius, double spanRadians) const {
    const double span = std::abs(spanRadians);
    const double byAngle = span / (2 * kPi) * 16;           // at least 16 for a whole turn
    const double byLength = span * std::max(0.0, radius) * dpr_ / 3;  // and no side over 3 pixels
    return std::clamp(int(std::ceil(std::max(byAngle, byLength))), 1, 2048);
}

void SgPainter::ellipseOutline(std::vector<P>& out, double cx, double cy, double rx, double ry, int steps) const {
    out.resize(size_t(steps));
    for (int j = 0; j < steps; ++j) {
        const double t = 2 * kPi * j / steps;
        out[size_t(j)] = {cx + rx * std::cos(t), cy + ry * std::sin(t)};
    }
}

void SgPainter::fillConvex(const std::vector<P>& outline, const Rgba& c) {
    if (outline.size() < 3)
        return;
    const Vertex first = vertex(outline[0].x, outline[0].y, c);
    for (size_t i = 1; i + 1 < outline.size(); ++i)
        triangle(first, vertex(outline[i].x, outline[i].y, c), vertex(outline[i + 1].x, outline[i + 1].y, c));
}

void SgPainter::fillConvexAntialiased(const std::vector<P>& inner, const std::vector<P>& outer, const Rgba& c) {
    fillConvex(inner, c);
    const Rgba clear{};
    const size_t n = std::min(inner.size(), outer.size());
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        quad(vertex(inner[i].x, inner[i].y, c), vertex(inner[j].x, inner[j].y, c),
             vertex(outer[j].x, outer[j].y, clear), vertex(outer[i].x, outer[i].y, clear));
    }
}

void SgPainter::fillEllipse(const QRectF& rect, const QColor& color) {
    const Rgba c = premultiplied(color);
    const QRectF r = rect.normalized().translated(state_.offset);
    if (c.a == 0 || r.isEmpty())
        return;
    beginSolid();
    const double cx = r.center().x(), cy = r.center().y(), rx = r.width() / 2, ry = r.height() / 2;
    const int steps = std::max(8, arcSegments(std::max(rx, ry) + px(), 2 * kPi));
    if (!state_.antialias) {
        ellipseOutline(scratch_, cx, cy, rx, ry, steps);
        fillConvex(scratch_, c);
        return;
    }
    const double f = px() / 2;
    ellipseOutline(scratch_, cx, cy, std::max(0.0, rx - f), std::max(0.0, ry - f), steps);
    ellipseOutline(scratch2_, cx, cy, rx + f, ry + f, steps);
    // Smaller than a pixel across: fainter rather than gone.
    const Rgba k = std::min(rx, ry) < f ? c.scaled(float(std::min(1.0, 2 * std::min(rx, ry) / px()))) : c;
    fillConvexAntialiased(scratch_, scratch2_, k);
}

void SgPainter::drawEllipse(const QRectF& rect, const QColor& color, qreal width) {
    const Rgba c = premultiplied(color);
    const QRectF r = rect.normalized().translated(state_.offset);
    if (c.a == 0)
        return;
    beginSolid();
    const double rx = r.width() / 2, ry = r.height() / 2;
    ellipseOutline(scratch_, r.center().x(), r.center().y(), rx, ry, std::max(8, arcSegments(std::max(rx, ry), 2 * kPi)));
    stroke(scratch_, true, width > 0 ? width : px(), Qt::SquareCap, c);
}

void SgPainter::drawArc(const QRectF& rect, qreal startDegrees, qreal spanDegrees, const QColor& color, qreal width,
                        Qt::PenCapStyle cap) {
    if (std::abs(spanDegrees) >= 360.0) {
        drawEllipse(rect, color, width);
        return;
    }
    const Rgba c = premultiplied(color);
    const QRectF r = rect.normalized().translated(state_.offset);
    if (c.a == 0 || spanDegrees == 0.0)
        return;
    beginSolid();
    const double cx = r.center().x(), cy = r.center().y(), rx = r.width() / 2, ry = r.height() / 2;
    const double start = startDegrees * kPi / 180.0, span = spanDegrees * kPi / 180.0;
    const int steps = std::max(2, arcSegments(std::max(rx, ry), span));
    scratch_.resize(size_t(steps) + 1);
    for (int j = 0; j <= steps; ++j) {
        const double t = start + span * j / steps;
        scratch_[size_t(j)] = {cx + rx * std::cos(t), cy - ry * std::sin(t)};  // counter-clockwise on screen
    }
    stroke(scratch_, false, width > 0 ? width : px(), cap, c);
}

void SgPainter::fillPolygon(const QPointF* points, int count, const QColor& color) {
    const Rgba c = premultiplied(color);
    if (c.a == 0 || count < 3)
        return;
    std::vector<P>& outline = scratch_;
    outline.clear();
    for (int i = 0; i < count; ++i) {
        const P p{points[i].x() + state_.offset.x(), points[i].y() + state_.offset.y()};
        if (!outline.empty() && std::abs(p.x - outline.back().x) < 1e-9 && std::abs(p.y - outline.back().y) < 1e-9)
            continue;
        outline.push_back(p);
    }
    while (outline.size() > 1 && std::abs(outline.front().x - outline.back().x) < 1e-9 &&
           std::abs(outline.front().y - outline.back().y) < 1e-9)
        outline.pop_back();
    const size_t n = outline.size();
    if (n < 3)
        return;
    double area = 0;
    for (size_t i = 0; i < n; ++i) {
        const P& a = outline[i];
        const P& b = outline[(i + 1) % n];
        area += a.x * b.y - b.x * a.y;
    }
    if (std::abs(area) < 1e-12)
        return;
    const double orientation = area > 0 ? 1.0 : -1.0;
    bool convex = true;
    for (size_t i = 0; i < n && convex; ++i) {
        const P& a = outline[i];
        const P& b = outline[(i + 1) % n];
        const P& d = outline[(i + 2) % n];
        if (cross(b.x - a.x, b.y - a.y, d.x - b.x, d.y - b.y) * orientation < -1e-9)
            convex = false;
    }
    beginSolid();
    if (!state_.antialias) {
        if (convex)
            fillConvex(outline, c);
        else
            fillConcave(outline, c);
        return;
    }
    // Each corner's outward offset (unit edges' normals mitred, capped).
    std::vector<P> offsets(n);
    for (size_t i = 0; i < n; ++i) {
        const P& prev = outline[(i + n - 1) % n];
        const P& here = outline[i];
        const P& next = outline[(i + 1) % n];
        auto outward = [orientation](const P& a, const P& b) {
            const double dx = b.x - a.x, dy = b.y - a.y, length = std::sqrt(dx * dx + dy * dy);
            return P{orientation * dy / length, -orientation * dx / length};
        };
        const P na = outward(prev, here), nb = outward(here, next);
        const double mx = na.x + nb.x, my = na.y + nb.y, m2 = mx * mx + my * my;
        if (m2 > 4.0 / (kFillMiterLimit * kFillMiterLimit))
            offsets[i] = {2 * mx / m2, 2 * my / m2};
        else
            offsets[i] = {(na.x + nb.x) * kFillMiterLimit / 2, (na.y + nb.y) * kFillMiterLimit / 2};
    }
    const double f = px() / 2;
    std::vector<P> outer(n);
    if (convex) {
        std::vector<P> inner(n);
        for (size_t i = 0; i < n; ++i) {
            inner[i] = {outline[i].x - offsets[i].x * f, outline[i].y - offsets[i].y * f};
            outer[i] = {outline[i].x + offsets[i].x * f, outline[i].y + offsets[i].y * f};
        }
        fillConvexAntialiased(inner, outer, c);
        return;
    }
    // Concave: the polygon itself, then a feather outside it.
    std::vector<P> copy = outline;
    fillConcave(copy, c);
    for (size_t i = 0; i < n; ++i)
        outer[i] = {copy[i].x + offsets[i].x * f, copy[i].y + offsets[i].y * f};
    const Rgba clear{};
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        quad(vertex(copy[i].x, copy[i].y, c), vertex(copy[j].x, copy[j].y, c), vertex(outer[j].x, outer[j].y, clear),
             vertex(outer[i].x, outer[i].y, clear));
    }
}

void SgPainter::fillConcave(const std::vector<P>& outline, const Rgba& c) {
    // Ear clipping: cut off a corner with nothing inside it, again and again.
    const int n = int(outline.size());
    double area = 0;
    for (int i = 0; i < n; ++i) {
        const P& a = outline[size_t(i)];
        const P& b = outline[size_t((i + 1) % n)];
        area += a.x * b.y - b.x * a.y;
    }
    const double orientation = area > 0 ? 1.0 : -1.0;
    std::vector<int> prev(static_cast<size_t>(n)), next(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        prev[size_t(i)] = (i + n - 1) % n;
        next[size_t(i)] = (i + 1) % n;
    }
    auto pt = [&](int i) -> const P& { return outline[size_t(i)]; };
    auto convexAt = [&](int i) {
        const P& a = pt(prev[size_t(i)]);
        const P& b = pt(i);
        const P& d = pt(next[size_t(i)]);
        return cross(b.x - a.x, b.y - a.y, d.x - b.x, d.y - b.y) * orientation > 0;
    };
    auto inside = [&](const P& p, const P& a, const P& b, const P& d) {
        const double c1 = cross(b.x - a.x, b.y - a.y, p.x - a.x, p.y - a.y) * orientation;
        const double c2 = cross(d.x - b.x, d.y - b.y, p.x - b.x, p.y - b.y) * orientation;
        const double c3 = cross(a.x - d.x, a.y - d.y, p.x - d.x, p.y - d.y) * orientation;
        return c1 >= 0 && c2 >= 0 && c3 >= 0;
    };
    auto cut = [&](int a, int b, int d) {
        triangle(vertex(pt(a).x, pt(a).y, c), vertex(pt(b).x, pt(b).y, c), vertex(pt(d).x, pt(d).y, c));
    };
    int remaining = n;
    int i = 0;
    int misses = 0;
    while (remaining > 3) {
        const int a = prev[size_t(i)], d = next[size_t(i)];
        bool ear = convexAt(i);
        if (ear) {
            for (int j = next[size_t(d)]; j != a; j = next[size_t(j)]) {
                const P& p = pt(j);
                const bool shared = (p.x == pt(a).x && p.y == pt(a).y) || (p.x == pt(i).x && p.y == pt(i).y) ||
                                    (p.x == pt(d).x && p.y == pt(d).y);
                if (!shared && !convexAt(j) && inside(p, pt(a), pt(i), pt(d))) {
                    ear = false;
                    break;
                }
            }
        }
        if (ear) {
            cut(a, i, d);
            next[size_t(a)] = d;
            prev[size_t(d)] = a;
            --remaining;
            i = d;
            misses = 0;
            continue;
        }
        i = next[size_t(i)];
        if (++misses > remaining) {
            // No ear left (a self-touching or degenerate outline): fan what remains.
            const int first = i;
            for (int j = next[size_t(first)]; next[size_t(j)] != first; j = next[size_t(j)])
                cut(first, j, next[size_t(j)]);
            return;
        }
    }
    cut(prev[size_t(i)], i, next[size_t(i)]);
}

void SgPainter::fillToBaseline(const QPointF* points, int count, qreal baseY, const QColor& color) {
    const Rgba c = premultiplied(color);
    if (c.a == 0 || count < 2)
        return;
    beginSolid();
    const double ox = state_.offset.x(), oy = state_.offset.y(), base = baseY + oy;
    for (int i = 0; i + 1 < count; ++i) {
        const double ax = points[i].x() + ox, ay = points[i].y() + oy;
        const double bx = points[i + 1].x() + ox, by = points[i + 1].y() + oy;
        const double da = ay - base, db = by - base;
        if (da * db < 0) {  // crosses the baseline: two triangles meeting there
            const double x = ax + (bx - ax) * da / (da - db);
            triangle(vertex(ax, ay, c), vertex(x, base, c), vertex(ax, base, c));
            triangle(vertex(bx, by, c), vertex(bx, base, c), vertex(x, base, c));
        } else {
            quad(vertex(ax, ay, c), vertex(bx, by, c), vertex(bx, base, c), vertex(ax, base, c));
        }
    }
}

void SgPainter::fillToBaseline(const QPointF* points, int count, qreal baseY, const QLinearGradient& gradient) {
    const QGradientStops stops = gradient.stops();
    if (stops.isEmpty() || count < 2)
        return;
    const QPointF s = gradient.start(), e = gradient.finalStop();
    const double gx = e.x() - s.x(), gy = e.y() - s.y();
    const double length2 = gx * gx + gy * gy;
    if (stops.size() == 1 || length2 <= 0.0) {
        fillToBaseline(points, count, baseY, stops.last().second);
        return;
    }
    auto colorAt = [&](double x, double y) {
        const double t = std::clamp(((x - s.x()) * gx + (y - s.y()) * gy) / length2, 0.0, 1.0);
        if (t <= stops.first().first)
            return premultiplied(stops.first().second);
        for (int i = 1; i < stops.size(); ++i) {
            if (t <= stops[i].first) {
                const double t0 = stops[i - 1].first, t1 = stops[i].first;
                const double f = t1 > t0 ? (t - t0) / (t1 - t0) : 1.0;
                const Rgba a = premultiplied(stops[i - 1].second), b = premultiplied(stops[i].second);
                auto mix = [f](std::uint8_t p, std::uint8_t q) { return std::uint8_t(std::lround(p + (q - p) * f)); };
                return Rgba{mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a)};
            }
        }
        return premultiplied(stops.last().second);
    };
    // Vertical: each piece is cut where the stops fall, so the colour is linear in y within each cut.
    std::vector<double> cuts;
    if (gx == 0.0) {
        for (const QGradientStop& stop : stops)
            cuts.push_back(s.y() + stop.first * gy);
        std::sort(cuts.begin(), cuts.end());
    }
    beginSolid();
    const double ox = state_.offset.x(), oy = state_.offset.y();
    // A convex piece (in the current coordinates), cut into horizontal slabs, each a fan.
    auto fill = [&](std::vector<P> polygon) {
        std::vector<double> bounds{-1e30};
        bounds.insert(bounds.end(), cuts.begin(), cuts.end());
        bounds.push_back(1e30);
        std::vector<P> slab, buffer;
        for (size_t b = 0; b + 1 < bounds.size(); ++b) {
            const double y0 = bounds[b], y1 = bounds[b + 1];
            slab = polygon;
            auto clipSide = [&](auto inside, double at) {
                buffer.clear();
                for (size_t i = 0; i < slab.size(); ++i) {
                    const P& p = slab[i];
                    const P& q = slab[(i + 1) % slab.size()];
                    const bool pIn = inside(p), qIn = inside(q);
                    if (pIn)
                        buffer.push_back(p);
                    if (pIn != qIn) {
                        const double t = (at - p.y) / (q.y - p.y);
                        buffer.push_back({p.x + (q.x - p.x) * t, at});
                    }
                }
                slab.swap(buffer);
            };
            clipSide([&](const P& p) { return p.y >= y0; }, y0);
            clipSide([&](const P& p) { return p.y <= y1; }, y1);
            if (slab.size() < 3)
                continue;
            auto v = [&](const P& p) { return vertex(p.x + ox, p.y + oy, colorAt(p.x, p.y)); };
            const Vertex first = v(slab[0]);
            for (size_t i = 1; i + 1 < slab.size(); ++i)
                triangle(first, v(slab[i]), v(slab[i + 1]));
        }
    };
    for (int i = 0; i + 1 < count; ++i) {
        const P a{points[i].x(), points[i].y()}, b{points[i + 1].x(), points[i + 1].y()};
        const double da = a.y - baseY, db = b.y - baseY;
        if (da * db < 0) {  // crosses the baseline: two triangles meeting there
            const double x = a.x + (b.x - a.x) * da / (da - db);
            fill({a, {x, baseY}, {a.x, baseY}});
            fill({b, {b.x, baseY}, {x, baseY}});
        } else {
            fill({a, b, {b.x, baseY}, {a.x, baseY}});
        }
    }
}

void SgPainter::fillColumns(qreal x0, qreal dx, const float* y0, const float* y1, int count, const QColor& color,
                            qreal minHeight) {
    const Rgba c = premultiplied(color);
    if (c.a == 0 || count <= 0)
        return;
    beginSolid();
    recording_.vertices.reserve(recording_.vertices.size() + size_t(count) * 6);
    const double ox = state_.offset.x() + x0, oy = state_.offset.y();
    for (int i = 0; i < count; ++i) {
        double top = std::min(y0[i], y1[i]) + oy, bottom = std::max(y0[i], y1[i]) + oy;
        if (bottom - top < minHeight) {
            const double middle = (top + bottom) / 2;
            top = middle - minHeight / 2;
            bottom = middle + minHeight / 2;
        }
        double left = ox + i * dx, right = left + dx;
        if (right < left)
            std::swap(left, right);
        rectItem(left, top, right, bottom, c);
    }
}

void SgPainter::fillBand(qreal x0, qreal dx, const float* tops, const float* bottoms, int count,
                         const QColor& color) {
    const Rgba c = premultiplied(color);
    if (c.a == 0 || count <= 0)
        return;
    beginSolid();
    const double ox = state_.offset.x() + x0, oy = state_.offset.y(), h = px() / 2;
    // Where an outline runs within column i: from halfway to the column before,
    // through its centre, to halfway to the one after.
    const auto span = [&](const float* ys, int i) {
        const double here = ys[i];
        const double before = (ys[std::max(i - 1, 0)] + here) / 2, after = (ys[std::min(i + 1, count - 1)] + here) / 2;
        return std::pair{std::min({before, here, after}) + oy, std::max({before, here, after}) + oy};
    };
    const Rgba clear{};
    for (int i = 0; i < count; ++i) {
        double left = ox + i * dx, right = left + dx;
        if (right < left)
            std::swap(left, right);
        const auto shade = [&](double y0, const Rgba& from, double y1, const Rgba& to) {
            quad(vertex(left, y0, from), vertex(right, y0, from), vertex(right, y1, to), vertex(left, y1, to));
        };
        const auto [t0, t1] = span(tops, i);
        const auto [b0, b1] = span(bottoms, i);
        // The top edge: clear at t0 - h, solid from t1 + h; the bottom one: solid to b0 - h, clear at b1 + h.
        const double a0 = t0 - h, a1 = t1 + h, z0 = b0 - h, z1 = b1 + h;
        if (a1 <= z0) {
            shade(a0, clear, a1, c);
            shade(a1, c, z0, c);
            shade(z0, c, z1, clear);
        } else {  // (a thin band's edges: they meet where they are as solid as each other)
            const double meet = (a0 * (z1 - z0) + z1 * (a1 - a0)) / ((z1 - z0) + (a1 - a0));
            const Rgba peak = c.scaled(float(std::clamp((meet - a0) / (a1 - a0), 0.0, 1.0)));
            shade(a0, clear, meet, peak);
            shade(meet, peak, z1, clear);
        }
    }
}

// --- Text and images ------------------------------------------------------------------

void SgPainter::drawTexture(const std::shared_ptr<SgTexture>& texture, const QRectF& target, const QRectF& source,
                            bool smooth, const QRectF* clipTo) {
    if (!texture || !texture->texture || target.isEmpty() || state_.opacity <= 0.0f)
        return;
    QRectF visible = target;
    if (clipTo)
        visible = visible.intersected(*clipTo);
    if (state_.clipped)
        visible = visible.intersected(state_.clip);
    if (visible.width() <= 0 || visible.height() <= 0)
        return;
    QRectF from = source;
    if (visible != target) {  // clipped: the part of the source that shows
        const double sx = source.width() / target.width(), sy = source.height() / target.height();
        from = QRectF(source.x() + (visible.x() - target.x()) * sx, source.y() + (visible.y() - target.y()) * sy,
                      visible.width() * sx, visible.height() * sy);
    }
    closeSolid();
    SgRecording::Segment segment;
    segment.kind = SgRecording::Kind::Texture;
    segment.texture = texture;
    segment.target = visible;
    segment.source = from;
    segment.opacity = state_.opacity;
    segment.smooth = smooth;
    recording_.segments.push_back(std::move(segment));
}

void SgPainter::textAt(const QPointF& boxTopLeft, const std::shared_ptr<SgTexture>& texture, const QRectF* clipTo) {
    // The image starts the padding before the layout box, on a device pixel, so glyphs stay sharp.
    const double x = std::round((boxTopLeft.x() - texture->origin.x()) * dpr_) / dpr_;
    const double y = std::round((boxTopLeft.y() - texture->origin.y()) * dpr_) / dpr_;
    const QSizeF pixels = texture->texture ? QSizeF(texture->texture->textureSize()) : texture->size * dpr_;
    drawTexture(texture, QRectF(QPointF(x, y), pixels / dpr_), QRectF(QPointF(), pixels), true, clipTo);
}

void SgPainter::drawText(const QRectF& rect, int flags, const QString& text, const QColor& color, const QFont& font) {
    if (text.isEmpty() || !cache_ || color.alpha() == 0)
        return;
    const QRectF r = rect.normalized().translated(state_.offset);
    const auto texture = cache_->text(text, font, color, flags & (Qt::AlignHorizontal_Mask | Qt::TextWordWrap),
                                      r.width(), dpr_);
    const QSizeF box = texture->box;
    double x = r.left(), y = r.top();
    if (flags & Qt::AlignRight)
        x = r.right() - box.width();
    else if (flags & Qt::AlignHCenter)
        x = r.left() + (r.width() - box.width()) / 2;
    if (flags & Qt::AlignBottom)
        y = r.bottom() - box.height();
    else if (flags & Qt::AlignVCenter)
        y = r.top() + (r.height() - box.height()) / 2;
    textAt(QPointF(x, y), texture, (flags & Qt::TextDontClip) ? nullptr : &r);
}

void SgPainter::drawText(const QPointF& baseline, const QString& text, const QColor& color, const QFont& font) {
    if (text.isEmpty() || !cache_ || color.alpha() == 0)
        return;
    const auto texture = cache_->text(text, font, color, 0, 0.0, dpr_);
    const QPointF at = baseline + state_.offset;
    textAt(QPointF(at.x(), at.y() - texture->ascent), texture, nullptr);
}

void SgPainter::drawImage(const QRectF& target, const QImage& image, const QRectF& source, bool smooth) {
    if (image.isNull() || !cache_)
        return;
    const auto texture = cache_->image(image);
    const QRectF from = source.isNull() ? QRectF(QPointF(), QSizeF(image.size())) : source;
    drawTexture(texture, target.normalized().translated(state_.offset), from, smooth, nullptr);
}

qreal SgPainter::textWidth(const QString& text, const QFont& font) {
    return QFontMetricsF(font).horizontalAdvance(text);
}

QString SgPainter::elidedText(const QString& text, const QFont& font, qreal width, Qt::TextElideMode mode) {
    return QFontMetricsF(font).elidedText(text, mode, width);
}

}  // namespace sub::ui
