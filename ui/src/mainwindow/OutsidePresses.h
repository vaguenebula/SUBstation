#pragma once

// Tells when a mouse button is pressed anywhere outside an item: in its window
// away from it, or in another window of the application. The browser stops a
// preview that way ("a preview stops when you click anywhere outside the
// browser", as its application-wide event filter did), and ends a hot swap.
// Presses while `ignore` is true (the browser's own menus or lists open), and
// presses in the items of `alsoInside`, count as inside. It only watches while
// `enabled`.

#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class QQuickItem;

namespace sub::ui {

class OutsidePresses : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickItem* item READ item WRITE setItem NOTIFY itemChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(bool ignore READ ignore WRITE setIgnore NOTIFY ignoreChanged)
    // Other items whose presses count as inside (Items; anything else is passed over).
    Q_PROPERTY(QVariantList alsoInside READ alsoInside WRITE setAlsoInside NOTIFY alsoInsideChanged)

public:
    explicit OutsidePresses(QObject* parent = nullptr);
    ~OutsidePresses() override;

    QQuickItem* item() const { return item_; }
    void setItem(QQuickItem* item);
    bool enabled() const { return enabled_; }
    void setEnabled(bool enabled);
    bool ignore() const { return ignore_; }
    void setIgnore(bool ignore);
    QVariantList alsoInside() const { return alsoInside_; }
    void setAlsoInside(const QVariantList& items);

Q_SIGNALS:
    void itemChanged();
    void enabledChanged();
    void ignoreChanged();
    void alsoInsideChanged();
    void pressed();  // outside the item

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void watch();

    QPointer<QQuickItem> item_;
    bool enabled_ = true;
    bool ignore_ = false;
    QVariantList alsoInside_;
    bool watching_ = false;
};

}  // namespace sub::ui
