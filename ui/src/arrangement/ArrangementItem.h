#pragma once

// What the arrangement's drawn parts share (the ruler, the lanes, the returns'
// and the master's lanes, the playhead, the live takes): the session they edit
// through and the Arrangement whose state they draw and change. QML writes
//
//   Arrangement { id: arrangement; session: Session }
//   ArrangementLanes { session: Session; arrangement: arrangement }
//
// An item draws again when the view moves (zoom, scroll, the grid); a subclass
// connects what else it follows in connectArrangement() and connectSession().

#include "arrangement/Arrangement.h"
#include "sg/SgCanvas.h"
#include "session/Session.h"

#include <QList>
#include <QPointer>
#include <QtQml/qqmlregistration.h>

namespace sub::ui {

class ArrangementItem : public SgCanvas {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("ArrangementItem is the base of the arrangement's items")
    Q_PROPERTY(sub::app::Session* session READ session WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(sub::ui::Arrangement* arrangement READ arrangement WRITE setArrangement NOTIFY arrangementChanged)

public:
    explicit ArrangementItem(QQuickItem* parent = nullptr);

    app::Session* session() const { return session_; }
    void setSession(app::Session* session);
    Arrangement* arrangement() const { return arrangement_; }
    void setArrangement(Arrangement* arrangement);

    // Draw again (and work out what paint() reads first: updatePolish()).
    void repaint();

Q_SIGNALS:
    void sessionChanged();
    void arrangementChanged();

protected:
    // Both the session and the arrangement are set: connect what the item follows.
    virtual void connectSession(app::Session* session) { Q_UNUSED(session); }
    virtual void connectArrangement(Arrangement* arrangement) { Q_UNUSED(arrangement); }
    bool ready() const { return session_ && arrangement_; }

private:
    void connectAll();

    QPointer<app::Session> session_;
    QPointer<Arrangement> arrangement_;
    QList<QPointer<QObject>> connected_;  // the senders the item follows
};

}  // namespace sub::ui
