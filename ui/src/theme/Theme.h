#pragma once

// The look: a dark, Ableton-like theme. One source of truth for every colour
// and font, read by the scene-graph items in C++ (Theme::kLane) and by QML
// (Theme.lane), and by the Qt Quick Controls style (ui/style), which draws the
// standard controls as the old stylesheet did.

#include <QColor>
#include <QFont>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

namespace sub::ui {

// The UI font: Segoe UI, 9 pt by default.
QFont uiFont(qreal pointSize = 9.0, bool bold = false);
// The fixed-width font (Consolas) at a size.
QFont monoFont(qreal pointSize = 9.0);

class Theme : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Base colours
    Q_PROPERTY(QColor window READ window CONSTANT)
    Q_PROPERTY(QColor panel READ panel CONSTANT)
    Q_PROPERTY(QColor panelAlt READ panelAlt CONSTANT)
    Q_PROPERTY(QColor surface READ surface CONSTANT)
    Q_PROPERTY(QColor surfaceHover READ surfaceHover CONSTANT)
    Q_PROPERTY(QColor border READ border CONSTANT)
    Q_PROPERTY(QColor text READ text CONSTANT)
    Q_PROPERTY(QColor textDim READ textDim CONSTANT)
    Q_PROPERTY(QColor textDisabled READ textDisabled CONSTANT)
    Q_PROPERTY(QColor accent READ accent CONSTANT)
    Q_PROPERTY(QColor accentText READ accentText CONSTANT)
    // Arrangement
    Q_PROPERTY(QColor lane READ lane CONSTANT)
    Q_PROPERTY(QColor laneSelected READ laneSelected CONSTANT)
    Q_PROPERTY(QColor emptyArea READ emptyArea CONSTANT)
    Q_PROPERTY(QColor gridBar READ gridBar CONSTANT)
    Q_PROPERTY(QColor gridBeat READ gridBeat CONSTANT)
    Q_PROPERTY(QColor gridSub READ gridSub CONSTANT)
    Q_PROPERTY(QColor playhead READ playhead CONSTANT)
    Q_PROPERTY(QColor insertMarker READ insertMarker CONSTANT)
    Q_PROPERTY(QColor loopOn READ loopOn CONSTANT)
    Q_PROPERTY(QColor loopOff READ loopOff CONSTANT)
    Q_PROPERTY(QColor loopRegion READ loopRegion CONSTANT)
    Q_PROPERTY(QColor selectionOutline READ selectionOutline CONSTANT)
    Q_PROPERTY(QColor selection READ selection CONSTANT)
    Q_PROPERTY(QColor rubberBand READ rubberBand CONSTANT)
    Q_PROPERTY(QColor waveform READ waveform CONSTANT)
    // Piano roll
    Q_PROPERTY(QColor keyWhite READ keyWhite CONSTANT)
    Q_PROPERTY(QColor keyBlack READ keyBlack CONSTANT)
    Q_PROPERTY(QColor keyLabel READ keyLabel CONSTANT)
    Q_PROPERTY(QColor blackKeyRow READ blackKeyRow CONSTANT)
    Q_PROPERTY(QColor outsideClip READ outsideClip CONSTANT)
    // Controls
    Q_PROPERTY(QColor activatorOn READ activatorOn CONSTANT)
    Q_PROPERTY(QColor soloOn READ soloOn CONSTANT)
    Q_PROPERTY(QColor playOn READ playOn CONSTANT)
    Q_PROPERTY(QColor recordOn READ recordOn CONSTANT)
    Q_PROPERTY(QColor meterLow READ meterLow CONSTANT)
    Q_PROPERTY(QColor meterMid READ meterMid CONSTANT)
    Q_PROPERTY(QColor meterHigh READ meterHigh CONSTANT)
    Q_PROPERTY(QColor meterBg READ meterBg CONSTANT)
    Q_PROPERTY(QColor deviceHeader READ deviceHeader CONSTANT)
    Q_PROPERTY(QColor deviceHeaderSelected READ deviceHeaderSelected CONSTANT)
    Q_PROPERTY(QColor scopeLine READ scopeLine CONSTANT)
    Q_PROPERTY(QColor scopeGlow READ scopeGlow CONSTANT)
    Q_PROPERTY(QColor scopeAxis READ scopeAxis CONSTANT)
    Q_PROPERTY(QColor frozen READ frozen CONSTANT)
    Q_PROPERTY(QColor frozenTint READ frozenTint CONSTANT)
    // From the stylesheet and the widgets
    Q_PROPERTY(QColor scrollHandleHover READ scrollHandleHover CONSTANT)
    Q_PROPERTY(QColor deviceHeaderHover READ deviceHeaderHover CONSTANT)
    Q_PROPERTY(QColor automationOn READ automationOn CONSTANT)
    Q_PROPERTY(QColor automationOff READ automationOff CONSTANT)

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
    // Base colours
    static constexpr QColor kWindow{0x1c, 0x1c, 0x1c};        // WINDOW
    static constexpr QColor kPanel{0x25, 0x25, 0x25};         // PANEL
    static constexpr QColor kPanelAlt{0x2c, 0x2c, 0x2c};      // PANEL_ALT
    static constexpr QColor kSurface{0x36, 0x36, 0x36};       // SURFACE
    static constexpr QColor kSurfaceHover{0x40, 0x40, 0x40};  // SURFACE_HOVER
    static constexpr QColor kBorder{0x11, 0x11, 0x11};        // BORDER
    static constexpr QColor kText{0xd9, 0xd9, 0xd9};          // TEXT
    static constexpr QColor kTextDim{0x8e, 0x8e, 0x8e};       // TEXT_DIM
    static constexpr QColor kTextDisabled{0x5c, 0x5c, 0x5c};  // TEXT_DISABLED
    static constexpr QColor kAccent{0xff, 0xa6, 0x2b};        // ACCENT
    static constexpr QColor kAccentText{0x1a, 0x1a, 0x1a};    // ACCENT_TEXT
    // Arrangement
    static constexpr QColor kLane{0x2b, 0x2b, 0x2b};              // LANE
    static constexpr QColor kLaneSelected{0x34, 0x34, 0x34};      // LANE_SELECTED
    static constexpr QColor kEmptyArea{0x22, 0x22, 0x22};         // EMPTY_AREA
    static constexpr QColor kGridBar{0x4a, 0x4a, 0x4a};           // GRID_BAR
    static constexpr QColor kGridBeat{0x39, 0x39, 0x39};          // GRID_BEAT
    static constexpr QColor kGridSub{0x31, 0x31, 0x31};           // GRID_SUB
    static constexpr QColor kPlayhead{0xf2, 0xf2, 0xf2};          // PLAYHEAD
    static constexpr QColor kInsertMarker{0xff, 0xa6, 0x2b};      // INSERT_MARKER
    static constexpr QColor kLoopOn{0xc9, 0xc9, 0xc9};            // LOOP_ON
    static constexpr QColor kLoopOff{0x5d, 0x5d, 0x5d};           // LOOP_OFF
    static constexpr QColor kLoopRegion{0xff, 0xff, 0xff, 12};    // LOOP_REGION
    static constexpr QColor kSelectionOutline{0xff, 0xff, 0xff};  // SELECTION_OUTLINE
    // SELECTION: a time selection on the grid (selected clips are one), and automation ranges.
    static constexpr QColor kSelection{0xa8, 0xc8, 0xeb, 72};
    static constexpr QColor kRubberBand{0xff, 0xa6, 0x2b, 40};  // RUBBER_BAND
    static constexpr QColor kWaveform{0x16, 0x16, 0x16, 230};   // WAVEFORM: also MIDI notes drawn in clips
    // Piano roll
    static constexpr QColor kKeyWhite{0xd2, 0xd2, 0xd2};       // KEY_WHITE
    static constexpr QColor kKeyBlack{0x1f, 0x1f, 0x1f};       // KEY_BLACK
    static constexpr QColor kKeyLabel{0x4a, 0x4a, 0x4a};       // KEY_LABEL
    static constexpr QColor kBlackKeyRow{0x25, 0x25, 0x25};    // BLACK_KEY_ROW
    static constexpr QColor kOutsideClip{0x00, 0x00, 0x00, 110};  // OUTSIDE_CLIP: content a clip has but doesn't play
    // Controls
    static constexpr QColor kActivatorOn{0xff, 0xc2, 0x33};          // ACTIVATOR_ON
    static constexpr QColor kSoloOn{0x4f, 0xa3, 0xff};               // SOLO_ON
    static constexpr QColor kPlayOn{0x5f, 0xd3, 0x5f};               // PLAY_ON
    static constexpr QColor kRecordOn{0xff, 0x5a, 0x4d};             // RECORD_ON
    static constexpr QColor kMeterLow{0x3f, 0xcf, 0x55};             // METER_LOW
    static constexpr QColor kMeterMid{0xf2, 0xd0, 0x24};             // METER_MID
    static constexpr QColor kMeterHigh{0xff, 0x4a, 0x3d};            // METER_HIGH
    static constexpr QColor kMeterBg{0x14, 0x14, 0x14};              // METER_BG
    static constexpr QColor kDeviceHeader{0x3e, 0x3e, 0x3e};         // DEVICE_HEADER: a device's title bar
    static constexpr QColor kDeviceHeaderSelected{0x57, 0x57, 0x57};  // DEVICE_HEADER_SELECTED
    static constexpr QColor kScopeLine{0xff, 0xb8, 0x4d};            // SCOPE_LINE
    static constexpr QColor kScopeGlow{0xff, 0xa6, 0x2b, 60};        // SCOPE_GLOW
    static constexpr QColor kScopeAxis{0xff, 0xff, 0xff, 22};        // SCOPE_AXIS
    static constexpr QColor kFrozen{0x8f, 0xd3, 0xff};               // FROZEN: a frozen track's snowflake
    static constexpr QColor kFrozenTint{0x8f, 0xd3, 0xff, 34};       // FROZEN_TINT: over a frozen track's lane
    // Not constants in the Python UI's theme, but part of its look: from its stylesheet and widgets.
    static constexpr QColor kScrollHandleHover{0x55, 0x55, 0x55};      // a scroll bar's handle under the mouse
    static constexpr QColor kDeviceHeaderHover{0xff, 0xff, 0xff, 28};  // a device header button under the mouse
    static constexpr QColor kAutomationOn{0xff, 0x4a, 0x3d};   // the dot of an automated control
    static constexpr QColor kAutomationOff{0x8c, 0x8c, 0x8c};  // the dot of a control whose automation is overridden

    static constexpr const char* kUiFontFamily = "Segoe UI";
    static constexpr const char* kMonoFontFamily = "Consolas";  // MONO_FONT
    static constexpr int kRadius = 3;
    static constexpr int kControlHeight = 28;
    static constexpr int kScrollBarWidth = 12;
    static constexpr int kIconSize = 14;

    explicit Theme(QObject* parent = nullptr);

    QColor window() const { return kWindow; }
    QColor panel() const { return kPanel; }
    QColor panelAlt() const { return kPanelAlt; }
    QColor surface() const { return kSurface; }
    QColor surfaceHover() const { return kSurfaceHover; }
    QColor border() const { return kBorder; }
    QColor text() const { return kText; }
    QColor textDim() const { return kTextDim; }
    QColor textDisabled() const { return kTextDisabled; }
    QColor accent() const { return kAccent; }
    QColor accentText() const { return kAccentText; }
    QColor lane() const { return kLane; }
    QColor laneSelected() const { return kLaneSelected; }
    QColor emptyArea() const { return kEmptyArea; }
    QColor gridBar() const { return kGridBar; }
    QColor gridBeat() const { return kGridBeat; }
    QColor gridSub() const { return kGridSub; }
    QColor playhead() const { return kPlayhead; }
    QColor insertMarker() const { return kInsertMarker; }
    QColor loopOn() const { return kLoopOn; }
    QColor loopOff() const { return kLoopOff; }
    QColor loopRegion() const { return kLoopRegion; }
    QColor selectionOutline() const { return kSelectionOutline; }
    QColor selection() const { return kSelection; }
    QColor rubberBand() const { return kRubberBand; }
    QColor waveform() const { return kWaveform; }
    QColor keyWhite() const { return kKeyWhite; }
    QColor keyBlack() const { return kKeyBlack; }
    QColor keyLabel() const { return kKeyLabel; }
    QColor blackKeyRow() const { return kBlackKeyRow; }
    QColor outsideClip() const { return kOutsideClip; }
    QColor activatorOn() const { return kActivatorOn; }
    QColor soloOn() const { return kSoloOn; }
    QColor playOn() const { return kPlayOn; }
    QColor recordOn() const { return kRecordOn; }
    QColor meterLow() const { return kMeterLow; }
    QColor meterMid() const { return kMeterMid; }
    QColor meterHigh() const { return kMeterHigh; }
    QColor meterBg() const { return kMeterBg; }
    QColor deviceHeader() const { return kDeviceHeader; }
    QColor deviceHeaderSelected() const { return kDeviceHeaderSelected; }
    QColor scopeLine() const { return kScopeLine; }
    QColor scopeGlow() const { return kScopeGlow; }
    QColor scopeAxis() const { return kScopeAxis; }
    QColor frozen() const { return kFrozen; }
    QColor frozenTint() const { return kFrozenTint; }
    QColor scrollHandleHover() const { return kScrollHandleHover; }
    QColor deviceHeaderHover() const { return kDeviceHeaderHover; }
    QColor automationOn() const { return kAutomationOn; }
    QColor automationOff() const { return kAutomationOff; }

    QFont font() const;
    QFont smallFont() const;
    QFont listFont() const;
    QFont listHeadingFont() const;
    QString monoFontFamily() const;
    int radius() const { return kRadius; }
    int controlHeight() const { return kControlHeight; }
    int scrollBarWidth() const { return kScrollBarWidth; }
    int iconSize() const { return kIconSize; }

    // theme.ui_font(point_size, bold), for QML: Theme.uiFont(11, true).
    Q_INVOKABLE QFont uiFont(qreal pointSize = 9.0, bool bold = false) const;
    Q_INVOKABLE QFont monoFont(qreal pointSize = 9.0) const;
    // The colour of an automation dot: "on" (automated), "off" (overridden); else transparent.
    Q_INVOKABLE QColor automationColor(const QString& state) const;
    // A shortcut as the menus show it ("Ctrl+Shift+S"), from a string or a
    // QKeySequence::StandardKey (what Action.shortcut holds).
    Q_INVOKABLE QString shortcutText(const QVariant& shortcut) const;
    // A menu or button text without its mnemonic marks ("&File" -> "File", "&&" -> "&").
    Q_INVOKABLE static QString withoutMnemonics(const QString& text);

    // The colour of an automation dot (C++): kAutomationOn, kAutomationOff, or an invalid QColor.
    static QColor automationDotColor(const QString& state);

    // A push button's look: the old stylesheet's QPushButton rules for its
    // `role` ("" for a plain button, "activator", "solo", "play", "record",
    // "arm", "re-enable", "tool", "flat", "small", "device-header") in a state,
    // the cascade worked out (a later rule of the same weight wins).
    struct ButtonLook {
        QColor background;  // transparent for none
        QColor text;
        int border = 1;  // px of kBorder
        int radius = 3;
        int paddingH = 12, paddingV = 4;
        int minWidth = 0, minHeight = 0;
        qreal pointSize = 9.0;
        int weight = QFont::Normal;  // 600 (DemiBold) for activator and solo
    };
    static ButtonLook buttonLook(const QString& role, bool hovered, bool pressed, bool checked, bool enabled);
    // The same for QML: a map with the fields above.
    Q_INVOKABLE QVariantMap buttonStyle(const QString& role, bool hovered, bool pressed, bool checked,
                                        bool enabled) const;
    static QStringList buttonRoles();
};

}  // namespace sub::ui
