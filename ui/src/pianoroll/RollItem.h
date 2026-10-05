#pragma once

// What the piano roll's drawn parts share (the note grid, the keys, the ruler,
// the velocity lane, the playhead): the session they edit through and the
// PianoRoll whose state they draw and change. Setting `session` on an item
// gives it to its roll if the roll has none, so QML can write
//
//   PianoRoll { id: roll }
//   NoteGrid { session: Session; roll: roll }
//
// An item draws again when its roll's content changes (repaint_all); a
// subclass connects what else it follows in rollConnected().

#include "pianoroll/PianoRoll.h"
#include "sg/SgCanvas.h"
#include "session/Session.h"

#include <QPointer>
#include <QtQml/qqmlregistration.h>

namespace sub::ui {

class RollItem : public SgCanvas {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("RollItem is the base of the piano roll's items")
    Q_PROPERTY(sub::app::Session* session READ session WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(sub::ui::PianoRoll* roll READ roll WRITE setRoll NOTIFY rollChanged)

public:
    explicit RollItem(QQuickItem* parent = nullptr);

    // The item's session, or its roll's.
    app::Session* session() const;
    void setSession(app::Session* session);
    PianoRoll* roll() const { return roll_; }
    void setRoll(PianoRoll* roll);

Q_SIGNALS:
    void sessionChanged();
    void rollChanged();

protected:
    // A new roll (never null): connect what the item follows besides contentChanged.
    virtual void rollConnected(PianoRoll* roll) { Q_UNUSED(roll); }

private:
    QPointer<app::Session> session_;
    QPointer<PianoRoll> roll_;
};

}  // namespace sub::ui
