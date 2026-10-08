#include "pianoroll/ChordLane.h"

#include "model/Numbers.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <algorithm>
#include <cmath>

namespace sub::ui {

namespace {

constexpr float kFillAlpha = 0.32f;
constexpr float kEdgeAlpha = 0.85f;
constexpr double kMinLabelWidth = 14.0;
constexpr float kHueOfC = 0.58f;

}  // namespace

ChordLane::ChordLane(QQuickItem* parent) : RollItem(parent) {}

QColor ChordLane::chordColor(int root, bool minor) {
    const int fifths = (root * 7) % 12;  // C 0, G 1, D 2, ...: neighbours a fifth apart
    // From blue at C (red is for notes out of the key), soft enough to read as bands.
    const float hue = std::fmod(kHueOfC + fifths / 12.0f, 1.0f);
    return QColor::fromHsvF(hue, 0.5f, minor ? 0.72f : 0.95f);
}

void ChordLane::rollConnected(PianoRoll* roll) { connect(roll, &PianoRoll::viewChanged, this, &QQuickItem::update); }

void ChordLane::paint(SgPainter& p) {
    PianoRoll* roll = this->roll();
    if (!roll) return;
    const QRectF visible = p.rect();
    const timeline::Timeline& view = roll->view();
    const double h = height();
    const QFont font = uiFont(8, true);
    for (const PianoRoll::RollChord& chord : roll->chords()) {
        const double x0 = app::roundHalfEven(view.beatToX(chord.start));
        const double x1 = app::roundHalfEven(view.beatToX(chord.end));
        if (x1 <= visible.left() || x0 >= visible.right()) continue;
        QColor color = chordColor(chord.root, chord.minor);
        QColor fill = color;
        fill.setAlphaF(kFillAlpha);
        p.fillRect(QRectF(x0, 0, x1 - x0, h), fill);
        color.setAlphaF(kEdgeAlpha);
        p.fillRect(QRectF(x0, 0, 1, h), color);
        // The name at the chord's start, or at the lane's left while its start is scrolled away.
        const double left = std::max(x0, visible.left()) + 4;
        const double width = x1 - left - 2;
        if (width >= kMinLabelWidth) {
            p.drawText(QRectF(left, 0, width, h), Qt::AlignVCenter | Qt::AlignLeft,
                       SgPainter::elidedText(chord.name, font, width), Theme::kText, font);
        }
    }
}

}  // namespace sub::ui
