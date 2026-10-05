#include "pianoroll/RollItem.h"

namespace sub::ui {

RollItem::RollItem(QQuickItem* parent) : SgCanvas(parent) {}

app::Session* RollItem::session() const {
    if (session_) return session_;
    return roll_ ? roll_->session() : nullptr;
}

void RollItem::setSession(app::Session* session) {
    if (session == session_) return;
    session_ = session;
    if (roll_ && !roll_->session()) roll_->setSession(session);
    Q_EMIT sessionChanged();
    update();
}

void RollItem::setRoll(PianoRoll* roll) {
    if (roll == roll_) return;
    if (roll_) disconnect(roll_, nullptr, this, nullptr);
    roll_ = roll;
    if (roll) {
        if (session_ && !roll->session()) roll->setSession(session_);
        connect(roll, &PianoRoll::contentChanged, this, &QQuickItem::update);
        rollConnected(roll);
    }
    Q_EMIT rollChanged();
    update();
}

}  // namespace sub::ui
