#pragma once

// The groups' bands over the track headers' column: for each group shown, a
// band in its colour down the column's left edge (kGroupBand wide, at its depth's kGroupIndent), from
// under the group's colour in its header (its name bar, with its fold button,
// and the row under it while its choosers show: TrackHeaderItem) to the bottom
// of the last track in it, unbroken across the lines between its tracks and
// their automation lanes. It is outlined: a line down its right edge; its left
// edge is the column's line, or the outline of the band of the group it is in;
// it stops a line short, so the line under the group closes it. Nested groups:
// a band per level, side by side. A folded (or empty) group's band is just its
// header's name bar. An item of
// its own over the headers rather than a part of each, so a band runs on from
// one header to the next; it takes no mouse input.
//
//   GroupBands { session: Session; arrangement: arrangement; width: headerWidth; height: ... }

#include "arrangement/ArrangementItem.h"

#include <QtQml/qqmlregistration.h>

namespace sub::ui {

class GroupBands : public ArrangementItem {
    Q_OBJECT
    QML_ELEMENT

public:
    explicit GroupBands(QQuickItem* parent = nullptr);

protected:
    void paint(SgPainter& painter) override;
    void connectSession(app::Session* session) override;
    void connectArrangement(Arrangement* arrangement) override;
};

}  // namespace sub::ui
