#pragma once

// What the Saturator's two graphs (SaturatorCurve, SaturatorColorGraph) share
// in painting, besides EditorPaint's.

#include <QColor>

#include <algorithm>

namespace sub::ui {

// `a` turning into `b` as `t` goes 0..1 (alpha too).
inline QColor colorBetween(const QColor& a, const QColor& b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    auto mix = [t](float x, float y) { return x + (y - x) * float(t); };
    return QColor::fromRgbF(mix(a.redF(), b.redF()), mix(a.greenF(), b.greenF()), mix(a.blueF(), b.blueF()),
                            mix(a.alphaF(), b.alphaF()));
}

}  // namespace sub::ui
