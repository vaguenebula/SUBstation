#pragma once

// The look: one source of truth for every colour and font, read by the
// scene-graph items in C++ (Theme::lane()) and by QML (Theme.lane), and by the
// Qt Quick Controls style (ui/style), which draws the standard controls as the
// old stylesheet did.
//
// The colours are the current theme's (Palettes.h: Default, Disableton,
// Flashbang, Gay), chosen in Options › Preferences… › Look and Feel and kept in
// the settings. A new theme applies at once: QML's bindings follow `changed`,
// and every item in every window is polished and repainted (C++ items read the
// colours as they paint).

#include "theme/Palettes.h"

#include <QColor>
#include <QFont>
#include <QObject>
#include <QPalette>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class QJSEngine;
class QQmlEngine;

namespace sub::ui {

// The UI font: Segoe UI, 9 pt by default.
QFont uiFont(qreal pointSize = 9.0, bool bold = false);
// The fixed-width font (Consolas) at a size.
QFont monoFont(qreal pointSize = 9.0);

class Theme : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // The current theme, and every theme there is (Default first).
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY changed)
    Q_PROPERTY(QStringList names READ names CONSTANT)

    // Base colours
    Q_PROPERTY(QColor window READ window NOTIFY changed)
    Q_PROPERTY(QColor panel READ panel NOTIFY changed)
    Q_PROPERTY(QColor panelAlt READ panelAlt NOTIFY changed)
    Q_PROPERTY(QColor surface READ surface NOTIFY changed)
    Q_PROPERTY(QColor surfaceHover READ surfaceHover NOTIFY changed)
    Q_PROPERTY(QColor border READ border NOTIFY changed)
    Q_PROPERTY(QColor text READ text NOTIFY changed)
    Q_PROPERTY(QColor textDim READ textDim NOTIFY changed)
    Q_PROPERTY(QColor textDisabled READ textDisabled NOTIFY changed)
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor accentText READ accentText NOTIFY changed)
    // Arrangement
    Q_PROPERTY(QColor lane READ lane NOTIFY changed)
    Q_PROPERTY(QColor laneSelected READ laneSelected NOTIFY changed)
    Q_PROPERTY(QColor emptyArea READ emptyArea NOTIFY changed)
    Q_PROPERTY(QColor gridBar READ gridBar NOTIFY changed)
    Q_PROPERTY(QColor gridBeat READ gridBeat NOTIFY changed)
    Q_PROPERTY(QColor gridSub READ gridSub NOTIFY changed)
    Q_PROPERTY(QColor playhead READ playhead NOTIFY changed)
    Q_PROPERTY(QColor insertMarker READ insertMarker NOTIFY changed)
    Q_PROPERTY(QColor loopOn READ loopOn NOTIFY changed)
    Q_PROPERTY(QColor loopOff READ loopOff NOTIFY changed)
    Q_PROPERTY(QColor loopRegion READ loopRegion NOTIFY changed)
    Q_PROPERTY(QColor selectionOutline READ selectionOutline NOTIFY changed)
    Q_PROPERTY(QColor selection READ selection NOTIFY changed)
    Q_PROPERTY(QColor rubberBand READ rubberBand NOTIFY changed)
    Q_PROPERTY(QColor waveform READ waveform NOTIFY changed)
    Q_PROPERTY(QColor groupOutline READ groupOutline NOTIFY changed)
    Q_PROPERTY(QColor deactivatedClip READ deactivatedClip NOTIFY changed)
    Q_PROPERTY(QColor deactivatedContent READ deactivatedContent NOTIFY changed)
    // Piano roll
    Q_PROPERTY(QColor keyWhite READ keyWhite NOTIFY changed)
    Q_PROPERTY(QColor keyBlack READ keyBlack NOTIFY changed)
    Q_PROPERTY(QColor keyLabel READ keyLabel NOTIFY changed)
    Q_PROPERTY(QColor blackKeyRow READ blackKeyRow NOTIFY changed)
    Q_PROPERTY(QColor outsideClip READ outsideClip NOTIFY changed)
    Q_PROPERTY(QColor outOfKey READ outOfKey NOTIFY changed)
    Q_PROPERTY(QColor pasteMarker READ pasteMarker NOTIFY changed)
    // Track headers
    Q_PROPERTY(QColor volumeFill READ volumeFill NOTIFY changed)
    // Controls
    Q_PROPERTY(QColor activatorOn READ activatorOn NOTIFY changed)
    Q_PROPERTY(QColor soloOn READ soloOn NOTIFY changed)
    Q_PROPERTY(QColor playOn READ playOn NOTIFY changed)
    Q_PROPERTY(QColor recordOn READ recordOn NOTIFY changed)
    Q_PROPERTY(QColor meterLow READ meterLow NOTIFY changed)
    Q_PROPERTY(QColor meterMid READ meterMid NOTIFY changed)
    Q_PROPERTY(QColor meterHigh READ meterHigh NOTIFY changed)
    Q_PROPERTY(QColor meterBg READ meterBg NOTIFY changed)
    Q_PROPERTY(QColor deviceHeader READ deviceHeader NOTIFY changed)
    Q_PROPERTY(QColor deviceHeaderSelected READ deviceHeaderSelected NOTIFY changed)
    Q_PROPERTY(QColor deviceHeaderHover READ deviceHeaderHover NOTIFY changed)
    Q_PROPERTY(QColor scopeLine READ scopeLine NOTIFY changed)
    Q_PROPERTY(QColor scopeGlow READ scopeGlow NOTIFY changed)
    Q_PROPERTY(QColor scopeAxis READ scopeAxis NOTIFY changed)
    Q_PROPERTY(QColor frozen READ frozen NOTIFY changed)
    Q_PROPERTY(QColor frozenTint READ frozenTint NOTIFY changed)
    Q_PROPERTY(QColor knob READ knob NOTIFY changed)
    Q_PROPERTY(QColor knobTrack READ knobTrack NOTIFY changed)
    Q_PROPERTY(QColor scrollHandle READ scrollHandle NOTIFY changed)
    Q_PROPERTY(QColor scrollHandleHover READ scrollHandleHover NOTIFY changed)
    Q_PROPERTY(QColor automationOn READ automationOn NOTIFY changed)
    Q_PROPERTY(QColor automationOff READ automationOff NOTIFY changed)

    // Fonts: the UI font (9 pt), the small one buttons with a role use (8 pt),
    // the browser's lists' (10 pt: Qt Quick draws small text smaller than the
    // widgets did at the same size) and their headings (8.5 pt, bold), and the
    // fixed-width family.
    Q_PROPERTY(QFont font READ font CONSTANT)
    Q_PROPERTY(QFont smallFont READ smallFont CONSTANT)
    Q_PROPERTY(QFont listFont READ listFont CONSTANT)
    Q_PROPERTY(QFont listHeadingFont READ listHeadingFont CONSTANT)
    Q_PROPERTY(QString monoFontFamily READ monoFontFamily CONSTANT)

    // Metrics the stylesheet used everywhere.
    Q_PROPERTY(int radius READ radius CONSTANT)                  // buttons, boxes, line edits: 3 px corners
    Q_PROPERTY(int controlHeight READ controlHeight CONSTANT)    // the transport bar's boxes and buttons
    Q_PROPERTY(int scrollBarWidth READ scrollBarWidth CONSTANT)  // 12 px
    Q_PROPERTY(int iconSize READ iconSize CONSTANT)              // a button's icon: 14 px

public:
    static constexpr const char* kUiFontFamily = "Segoe UI";
    static constexpr const char* kMonoFontFamily = "Consolas";  // MONO_FONT
    static constexpr int kRadius = 3;
    static constexpr int kControlHeight = 28;
    static constexpr int kScrollBarWidth = 12;
    static constexpr int kIconSize = 14;
    static constexpr const char* kSettingsKey = "ui/theme";  // the theme's name in QSettings

    // The one there is, QML's singleton: it says `changed`. (No other can be
    // made: Qt 6.4's QML would make its own with a public constructor, and
    // never hear of a new theme.)
    static Theme* instance();
    static Theme* create(QQmlEngine*, QJSEngine*);

    // The current theme: its name, and its colours.
    static QString name();
    static const Palette& colors();
    static QStringList names();
    // Makes `name` the theme (an unknown name: Default) and keeps it in the
    // settings: Look and Feel's choice.
    void setName(const QString& name);
    // Makes `name` the theme, without keeping it; false for an unknown name
    // (the theme stays). The application's palette follows, and every window
    // repaints.
    static bool apply(const QString& name);
    // The theme kept in the settings (Default if none, or one that's gone).
    static QString savedName();

    // Base colours
    static QColor window() { return colors().window; }
    static QColor panel() { return colors().panel; }
    static QColor panelAlt() { return colors().panelAlt; }
    static QColor surface() { return colors().surface; }
    static QColor surfaceHover() { return colors().surfaceHover; }
    static QColor border() { return colors().border; }
    static QColor text() { return colors().text; }
    static QColor textDim() { return colors().textDim; }
    static QColor textDisabled() { return colors().textDisabled; }
    static QColor accent() { return colors().accent; }
    static QColor accentText() { return colors().accentText; }
    // Arrangement
    static QColor lane() { return colors().lane; }
    static QColor laneSelected() { return colors().laneSelected; }
    static QColor emptyArea() { return colors().emptyArea; }
    static QColor gridBar() { return colors().gridBar; }
    static QColor gridBeat() { return colors().gridBeat; }
    static QColor gridSub() { return colors().gridSub; }
    static QColor playhead() { return colors().playhead; }
    static QColor insertMarker() { return colors().insertMarker; }
    static QColor loopOn() { return colors().loopOn; }
    static QColor loopOff() { return colors().loopOff; }
    static QColor loopRegion() { return colors().loopRegion; }
    static QColor selectionOutline() { return colors().selectionOutline; }
    static QColor selection() { return colors().selection; }
    static QColor rubberBand() { return colors().rubberBand; }
    static QColor waveform() { return colors().waveform; }
    static QColor groupOutline() { return colors().groupOutline; }
    static QColor deactivatedClip() { return colors().deactivatedClip; }
    static QColor deactivatedContent() { return colors().deactivatedContent; }
    // Piano roll
    static QColor keyWhite() { return colors().keyWhite; }
    static QColor keyBlack() { return colors().keyBlack; }
    static QColor keyLabel() { return colors().keyLabel; }
    static QColor blackKeyRow() { return colors().blackKeyRow; }
    static QColor outsideClip() { return colors().outsideClip; }
    static QColor outOfKey() { return colors().outOfKey; }
    static QColor pasteMarker() { return colors().pasteMarker; }
    // Track headers
    static QColor volumeFill() { return colors().volumeFill; }
    // Controls
    static QColor activatorOn() { return colors().activatorOn; }
    static QColor soloOn() { return colors().soloOn; }
    static QColor playOn() { return colors().playOn; }
    static QColor recordOn() { return colors().recordOn; }
    static QColor meterLow() { return colors().meterLow; }
    static QColor meterMid() { return colors().meterMid; }
    static QColor meterHigh() { return colors().meterHigh; }
    static QColor meterBg() { return colors().meterBg; }
    static QColor deviceHeader() { return colors().deviceHeader; }
    static QColor deviceHeaderSelected() { return colors().deviceHeaderSelected; }
    static QColor deviceHeaderHover() { return colors().deviceHeaderHover; }
    static QColor scopeLine() { return colors().scopeLine; }
    static QColor scopeGlow() { return colors().scopeGlow; }
    static QColor scopeAxis() { return colors().scopeAxis; }
    static QColor frozen() { return colors().frozen; }
    static QColor frozenTint() { return colors().frozenTint; }
    static QColor knob() { return colors().knob; }
    static QColor knobTrack() { return colors().knobTrack; }
    static QColor scrollHandle() { return colors().scrollHandle; }
    static QColor scrollHandleHover() { return colors().scrollHandleHover; }
    static QColor automationOn() { return colors().automationOn; }
    static QColor automationOff() { return colors().automationOff; }

    QFont font() const;
    QFont smallFont() const;
    QFont listFont() const;
    QFont listHeadingFont() const;
    QString monoFontFamily() const;
    int radius() const { return kRadius; }
    int controlHeight() const { return kControlHeight; }
    int scrollBarWidth() const { return kScrollBarWidth; }
    int iconSize() const { return kIconSize; }

    // The application's palette, from the current theme: what controls
    // without a look of their own fall back to.
    static QPalette qtPalette();

    // theme.ui_font(point_size, bold), for QML: Theme.uiFont(11, true).
    Q_INVOKABLE QFont uiFont(qreal pointSize = 9.0, bool bold = false) const;
    Q_INVOKABLE QFont monoFont(qreal pointSize = 9.0) const;
    // The colour of an automation dot: "on" (automated), "off" (overridden); else transparent.
    Q_INVOKABLE QColor automationColor(const QString& state) const;
    // A shortcut as the menus show it ("Ctrl+Shift+S"), from what
    // Action.shortcut holds (keySequences(): its first key sequence).
    Q_INVOKABLE QString shortcutText(const QVariant& shortcut) const;
    // A menu or button text without its mnemonic marks ("&File" -> "File", "&&" -> "&").
    Q_INVOKABLE static QString withoutMnemonics(const QString& text);

    // The colour of an automation dot (C++): automationOn(), automationOff(), or an invalid QColor.
    static QColor automationDotColor(const QString& state);

    // A push button's look: the old stylesheet's QPushButton rules for its
    // `role` ("" for a plain button, "activator", "solo", "play", "record",
    // "arm", "re-enable", "tool", "flat", "small", "device-header") in a state,
    // the cascade worked out (a later rule of the same weight wins).
    struct ButtonLook {
        QColor background;  // transparent for none
        QColor text;
        int border = 1;  // px of border()
        int radius = 3;
        int paddingH = 12, paddingV = 4;
        int minWidth = 0, minHeight = 0;
        qreal pointSize = 9.0;
        int weight = QFont::Normal;  // 600 (DemiBold) for activator and solo
    };
    static ButtonLook buttonLook(const QString& role, bool hovered, bool pressed, bool checked, bool enabled);
    // The same for QML: a map with the fields above. Not a binding's
    // dependency by itself: read Theme.name in the binding too, so it follows
    // a new theme (RoleButton does).
    Q_INVOKABLE QVariantMap buttonStyle(const QString& role, bool hovered, bool pressed, bool checked,
                                        bool enabled) const;
    static QStringList buttonRoles();

Q_SIGNALS:
    // A new theme: every colour may have changed.
    void changed();

private:
    Theme() = default;
};

}  // namespace sub::ui
