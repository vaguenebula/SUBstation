#pragma once

// The main window's place and size, and its splitter, kept in QSettings
// between runs (window/geometry, window/splitter), as MainWindow's
// _restore_window and closeEvent kept them. The geometry is the window's
// normal one (not maximized) and whether it was maximized; a saved place no
// screen shows any more is moved onto the primary screen. What a widget
// version of the program saved there (QWidget::saveGeometry's bytes) is
// ignored: the window then starts at its default size.

#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QRect>
#include <QString>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

namespace sub::ui {

class WindowState : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickWindow* window READ window WRITE setWindow NOTIFY windowChanged)

public:
    inline static const QString kGeometryKey = QStringLiteral("window/geometry");
    inline static const QString kSplitterKey = QStringLiteral("window/splitter");

    explicit WindowState(QObject* parent = nullptr);

    QQuickWindow* window() const { return window_; }
    void setWindow(QQuickWindow* window);

    // Puts the window where it was saved (maximized, if it was): false if
    // nothing usable was saved (it keeps its size).
    Q_INVOKABLE bool restore();
    // The splitter's state as saved (SplitView.saveState()), or null.
    Q_INVOKABLE QVariant splitterState() const;
    // Saves the window's geometry and the splitter's state.
    Q_INVOKABLE void save(const QVariant& splitterState);

    // The window's normal geometry (as last seen while neither maximized nor full screen).
    QRect normalGeometry() const { return normal_; }

Q_SIGNALS:
    void windowChanged();

private:
    void track();

    QPointer<QQuickWindow> window_;
    QRect normal_;
};

}  // namespace sub::ui
