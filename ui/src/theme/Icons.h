#pragma once

// Small vector icons drawn with QPainter, so they stay crisp at any size and
// device pixel ratio. Each is drawn on a 64 x 64 grid, scaled to the size asked
// for, in its colour (or in Theme::kTextDisabled for the disabled variant). QML
// gets them through the image provider:
//
//   image://icons/<name>[?color=%23rrggbb][&state=on|off][&mode=disabled]
//
// (Icons.url() builds that.) `state` picks the On or Off picture of the icons
// that have both: lock_envelopes (on: closed, automation stays put) and fold
// (on: folded, pointing right; off: open, pointing down). Without `color`
// each icon has its own default.
//
// Names: play, stop, record, metronome, loop, follow, re_enable_automation,
// lock_envelopes, headphones, folder, waveform, plugin, preset, plugin_window,
// sidechain, snowflake, save, link, infinity, expand, sliders, fold, search,
// app_icon.

#include <QColor>
#include <QImage>
#include <QObject>
#include <QQuickImageProvider>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

class QPainter;

namespace sub::ui {

class Icons : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QStringList names READ names CONSTANT)

public:
    static constexpr int kGrid = 64;  // the icons' drawing coordinates

    explicit Icons(QObject* parent = nullptr);

    static QStringList names();
    static bool has(const QString& name);
    // The icon's own colour: the one it has without `color`.
    static QColor defaultColor(const QString& name);
    // `name` drawn on a transparent square `pixels` wide: in `color` (the
    // default when invalid), its On or Off picture, or the disabled variant.
    // A null image for an unknown name.
    static QImage image(const QString& name, int pixels, const QColor& color = QColor(), bool on = false,
                        bool disabled = false);
    // Draws `name` on `painter` in the 64 x 64 grid (scale the painter first).
    static bool draw(QPainter& painter, const QString& name, const QColor& color, bool on);

    // The image provider's URL for an icon. `color` may be undefined (the icon's default).
    Q_INVOKABLE static QString url(const QString& name, const QVariant& color = QVariant(), bool on = false,
                                   bool disabled = false);
};

// "image://icons/...": registered on the engine by sub::ui::setUpEngine().
class IconProvider : public QQuickImageProvider {
public:
    IconProvider();
    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;
};

}  // namespace sub::ui
