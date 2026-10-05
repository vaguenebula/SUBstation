#pragma once

// The lane of a strip without clips, the master's or a return track's: grid and
// loop region, and its automation, in it and in lanes below it, edited as a
// track's (Envelopes.h). Its height follows its automation (Arrangement's
// masterRows / returnRows); the playhead is an ArrangementPlayhead over it.

#include "arrangement/ArrangementItem.h"
#include "arrangement/Envelopes.h"
#include "arrangement/MenuEntries.h"
#include "arrangement/TrackLayout.h"

#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <optional>
#include <vector>

namespace sub::ui {

class BusLane : public ArrangementItem, public arrangement::LanesHost {
    Q_OBJECT
    QML_ELEMENT
    // "master", or a return track's id.
    Q_PROPERTY(QString owner READ owner WRITE setOwner NOTIFY ownerChanged)

public:
    explicit BusLane(QQuickItem* parent = nullptr);
    ~BusLane() override;

    QString owner() const { return owner_; }
    void setOwner(const QString& owner);

    // Its own lane's height and the lanes below it.
    arrangement::AutomationRows rows() const;
    std::vector<arrangement::EnvelopeArea> envelopeAreas() const override;
    void updateHover(const QPointF& pos, Qt::KeyboardModifiers modifiers);
    const std::optional<arrangement::Hover>& hoverPoint() const { return hoverPoint_; }
    arrangement::MenuEntries contextMenu(const QPointF& pos);
    Q_INVOKABLE void triggerMenu(int id);

    app::Session* hostSession() const override { return session(); }
    Arrangement* hostArrangement() const override { return arrangement(); }
    double hostWidth() const override { return width(); }
    void setHostCursor(const QCursor& cursor) override { setCursor(cursor); }
    void repaint() override { ArrangementItem::repaint(); }

Q_SIGNALS:
    void ownerChanged();
    void menuRequested(const QVariantList& entries, const QPointF& pos);

protected:
    void paint(SgPainter& painter) override;
    void updatePolish() override;
    void connectSession(app::Session* session) override;
    void connectArrangement(Arrangement* arrangement) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void hoverEnterEvent(QHoverEvent* event) override { hoverMoveEvent(event); }
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;

private:
    void press(const QPointF& pos, Qt::KeyboardModifiers modifiers);

    QString owner_;
    std::unique_ptr<arrangement::Gesture> gesture_;
    std::optional<arrangement::Hover> hoverPoint_;
    std::optional<QPointF> hoverPos_;
    arrangement::MenuEntries menu_;
    std::vector<arrangement::EnvelopeArea> areas_;
    std::vector<arrangement::EnvelopeLook> looks_;
    arrangement::AutomationRows rows_;
};

}  // namespace sub::ui
