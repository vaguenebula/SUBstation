#include "arrangement/ArrangementItem.h"

#include "audio/EngineBridge.h"
#include "editor/ProjectEditor.h"
#include "model/Project.h"
#include "session/ArrangementActions.h"
#include "session/Selection.h"

namespace sub::ui {

ArrangementItem::ArrangementItem(QQuickItem* parent) : SgCanvas(parent) {}

void ArrangementItem::setSession(app::Session* session) {
    if (session == session_) return;
    session_ = session;
    connectAll();
    Q_EMIT sessionChanged();
    repaint();
}

void ArrangementItem::setArrangement(Arrangement* arrangement) {
    if (arrangement == arrangement_) return;
    arrangement_ = arrangement;
    connectAll();
    Q_EMIT arrangementChanged();
    repaint();
}

void ArrangementItem::connectAll() {
    // What the item followed goes first (a session or an arrangement replaced).
    for (QObject* sender : connected_) {
        if (sender) disconnect(sender, nullptr, this, nullptr);
    }
    connected_.clear();
    if (!ready()) return;
    connected_ = {session_.data(),           session_->project(),     session_->bridge(),  session_->selection(),
                  session_->editor(),        session_->arrangement(), session_->undoStack(), arrangement_.data()};
    connect(arrangement_, &Arrangement::viewChanged, this, &ArrangementItem::repaint);
    connect(arrangement_, &Arrangement::gridChanged, this, &ArrangementItem::repaint);
    connectSession(session_);
    connectArrangement(arrangement_);
}

void ArrangementItem::repaint() {
    polish();
    update();
}

}  // namespace sub::ui
