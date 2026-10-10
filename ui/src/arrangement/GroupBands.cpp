#include "arrangement/GroupBands.h"

#include "arrangement/TrackLayout.h"
#include "model/Project.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QColor>
#include <QRectF>

#include <algorithm>
#include <vector>

namespace sub::ui {

using arrangement::Row;

GroupBands::GroupBands(QQuickItem* parent) : ArrangementItem(parent) {}

void GroupBands::connectSession(app::Session* session) {
    app::Project* project = session->project();
    // A group's colour, or what is in it.
    connect(project, &app::Project::trackChanged, this, &QQuickItem::update);
    connect(project, &app::Project::tracksArranged, this, &QQuickItem::update);
    connect(project, &app::Project::reset, this, &QQuickItem::update);
}

void GroupBands::connectArrangement(Arrangement* arrangement) {
    connect(arrangement, &Arrangement::layoutChanged, this, &QQuickItem::update);
    connect(arrangement, &Arrangement::vscrollChanged, this, &QQuickItem::update);
}

void GroupBands::paint(SgPainter& p) {
    if (!ready()) return;
    const app::Project& project = *session()->project();
    const std::vector<Row>& rows = arrangement()->layout().rows();
    const int scroll = arrangement()->scrollY();
    const double h = height();
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        if (row.hidden) continue;
        const app::Track* group = project.findTrack(row.trackId);
        if (!group || !group->isGroup()) continue;
        // What is in it: the rows after it that are deeper (a hidden one is
        // where the next shown row starts, with no height).
        size_t end = i + 1;
        while (end < rows.size() && rows[end].depth > row.depth) ++end;
        // From under its colour in its header (its fold button is on that): its
        // name bar and the row under it while its choosers show, else all of it.
        const double colored = row.automation ? arrangement::kGroupBlock : row.mainHeight - 1;
        const double top = row.top + colored - scroll;
        const double bottom = rows[end - 1].bottom() - 1 - scroll;  // (the line under the group shows)
        if (bottom <= top || bottom <= 0 || top >= h) continue;
        // Its left edge: the column's line (as each header has it) stays.
        const double left = std::max(1, row.depth * arrangement::kGroupIndent);
        const double right = row.depth * arrangement::kGroupIndent + arrangement::kGroupBand;
        p.fillRect(QRectF(left, top, right - left, bottom - top), QColor(group->color));
        p.fillRect(QRectF(right - 1, top, 1, bottom - top), Theme::border());  // its outline
    }
}

}  // namespace sub::ui
