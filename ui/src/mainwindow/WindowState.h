#pragma once

// The main window's place and size, and its splitters, kept in QSettings
// between runs (window/geometry, window/splitter: the browser's, as
// MainWindow's _restore_window and closeEvent kept them; and
// window/device_splitter: the arrangement's over the device view). The geometry is the window's
// normal one (not maximized) and whether it was maximized; a saved place no
// screen shows any more is moved onto the primary screen. What a widget
// version of the program saved there (QWidget::saveGeometry's bytes) is
// ignored (the window then starts at its default size), and so is its
// QSplitter's state.

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
    inline static const QString kDeviceSplitterKey = QStringLiteral("window/device_splitter");

    explicit WindowState(QObject* parent = nullptr);

    QQuickWindow* window() const { return window_; }
    void setWindow(QQuickWindow* window);

    // Puts the window where it was saved (maximized, if it was): false if
    // nothing usable was saved (it keeps its size).
    Q_INVOKABLE bool restore();
    // The splitters' states as saved (SplitView.saveState()), or null.
    Q_INVOKABLE QVariant splitterState() const;
    Q_INVOKABLE QVariant deviceSplitterState() const;
    // Saves the window's geometry and the splitters' states.
    Q_INVOKABLE void save(const QVariant& splitterState, const QVariant& deviceSplitterState = QVariant());

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
