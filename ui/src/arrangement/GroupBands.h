#pragma once

// The groups' bands over the track headers' column: for each group shown, a
// band in its colour down the column's left edge (kGroupBand wide, at its
// depth's kGroupIndent), from the top of the group's header to the bottom of
// the last track in it, unbroken across the lines between its tracks and their
// automation lanes; it stops a line short, so the line under the group shows
// where it ends. Nested groups: a band per level, side by side. A folded (or
// empty) group's band is just its header's. The bar across the top of a group's
// header is the header's own (TrackHeaderItem), under its controls. An item of
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
