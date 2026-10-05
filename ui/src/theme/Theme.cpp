#include "theme/Theme.h"

#include <QKeySequence>
#include <QVariant>

namespace sub::ui {

QFont uiFont(qreal pointSize, bool bold) {
    QFont font(QString::fromLatin1(Theme::kUiFontFamily));
    font.setPointSizeF(pointSize);
    font.setBold(bold);
    return font;
}

QFont monoFont(qreal pointSize) {
    QFont font(QString::fromLatin1(Theme::kMonoFontFamily));
    font.setStyleHint(QFont::Monospace);
    font.setPointSizeF(pointSize);
    return font;
}

Theme::Theme(QObject* parent) : QObject(parent) {}

QFont Theme::font() const { return sub::ui::uiFont(); }

QFont Theme::smallFont() const { return sub::ui::uiFont(8.0); }
QFont Theme::listFont() const { return sub::ui::uiFont(10.0); }
QFont Theme::listHeadingFont() const { return sub::ui::uiFont(8.5, true); }

QString Theme::monoFontFamily() const { return QString::fromLatin1(kMonoFontFamily); }

QFont Theme::uiFont(qreal pointSize, bool bold) const { return sub::ui::uiFont(pointSize, bold); }

QFont Theme::monoFont(qreal pointSize) const { return sub::ui::monoFont(pointSize); }

QColor Theme::automationColor(const QString& state) const {
    const QColor color = automationDotColor(state);
    return color.isValid() ? color : QColor(Qt::transparent);
}

QString Theme::shortcutText(const QVariant& shortcut) const {
    if (!shortcut.isValid())
        return {};
    QKeySequence sequence;
    if (shortcut.typeId() == QMetaType::QKeySequence)
        sequence = shortcut.value<QKeySequence>();
    else if (shortcut.typeId() == QMetaType::QString)
        sequence = QKeySequence(shortcut.toString());
    else if (shortcut.canConvert<int>())
        sequence = QKeySequence(static_cast<QKeySequence::StandardKey>(shortcut.toInt()));
    return sequence.toString(QKeySequence::NativeText);
}

QString Theme::withoutMnemonics(const QString& text) {
    QString result;
    result.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i] == QLatin1Char('&') && i + 1 < text.size())
            ++i;  // "&x" shows x; "&&" shows one "&"
        result += text[i];
    }
    return result;
}

QStringList Theme::buttonRoles() {
    return {QString(),
            QStringLiteral("activator"),
            QStringLiteral("solo"),
            QStringLiteral("play"),
            QStringLiteral("record"),
            QStringLiteral("arm"),
            QStringLiteral("re-enable"),
            QStringLiteral("tool"),
            QStringLiteral("flat"),
            QStringLiteral("small"),
            QStringLiteral("device-header")};
}

Theme::ButtonLook Theme::buttonLook(const QString& role, bool hovered, bool pressed, bool checked, bool enabled) {
    // The stylesheet's rules in its order; pseudo-states only apply where Qt
    // would (a disabled button isn't hovered or pressed).
    hovered = hovered && enabled;
    pressed = pressed && enabled;
    ButtonLook look;
    // QPushButton { background: SURFACE; border: 1px solid BORDER; border-radius: 3px; padding: 4px 12px; }
    look.background = kSurface;
    look.text = kText;
    if (hovered)
        look.background = kSurfaceHover;  // :hover
    if (pressed)
        look.background = kPanel;  // :pressed
    if (!enabled)
        look.text = kTextDisabled;  // :disabled
    if (checked) {                  // :checked
        look.background = kAccent;
        look.text = kAccentText;
    }
    if (role == QLatin1String("activator") && checked) {
        look.background = kActivatorOn;
        look.text = kAccentText;
    } else if (role == QLatin1String("solo") && checked) {
        look.background = kSoloOn;
        look.text = kAccentText;
    } else if (role == QLatin1String("play") && checked) {
        look.background = kPlayOn;
    } else if ((role == QLatin1String("record") || role == QLatin1String("arm")) && checked) {
        look.background = kRecordOn;
        look.text = kAccentText;
    } else if (role == QLatin1String("re-enable") && checked) {
        look.background = kAccent;
    }
    if (role == QLatin1String("record") || role == QLatin1String("re-enable") || role == QLatin1String("tool")) {
        look.paddingH = look.paddingV = 2;
        look.minWidth = 26;
        look.minHeight = 22;
    } else if (role == QLatin1String("arm")) {
        look.paddingH = look.paddingV = 0;
        look.pointSize = 8.0;
        look.radius = 8;
    } else if (role == QLatin1String("activator") || role == QLatin1String("solo")) {
        look.paddingH = look.paddingV = 0;
        look.pointSize = 8.0;
        look.weight = QFont::DemiBold;
        look.radius = 2;
    } else if (role == QLatin1String("flat")) {
        // Later than every state rule: no background in any state, dim text but under the mouse.
        look.paddingH = look.paddingV = 0;
        look.border = 0;
        look.background = Qt::transparent;
        look.text = hovered ? kText : kTextDim;
    } else if (role == QLatin1String("small")) {
        look.paddingH = 6;
        look.paddingV = 0;
        look.pointSize = 8.0;
    } else if (role == QLatin1String("device-header")) {
        look.paddingH = look.paddingV = 0;
        look.border = 0;
        look.radius = 2;
        look.background = Qt::transparent;
        look.text = kTextDim;
        if (hovered) {
            look.background = kDeviceHeaderHover;
            look.text = kText;
        }
        if (checked) {
            look.background = kAccent;
            look.text = kAccentText;
        }
        if (!enabled) {
            look.background = Qt::transparent;
            look.text = kTextDisabled;
        }
    }
    return look;
}

QVariantMap Theme::buttonStyle(const QString& role, bool hovered, bool pressed, bool checked, bool enabled) const {
    const ButtonLook look = buttonLook(role, hovered, pressed, checked, enabled);
    return {{QStringLiteral("background"), look.background}, {QStringLiteral("text"), look.text},
            {QStringLiteral("border"), look.border},         {QStringLiteral("radius"), look.radius},
            {QStringLiteral("paddingH"), look.paddingH},     {QStringLiteral("paddingV"), look.paddingV},
            {QStringLiteral("minWidth"), look.minWidth},     {QStringLiteral("minHeight"), look.minHeight},
            {QStringLiteral("pointSize"), look.pointSize},   {QStringLiteral("weight"), look.weight}};
}

QColor Theme::automationDotColor(const QString& state) {
    if (state == QLatin1String("on"))
        return kAutomationOn;
    if (state == QLatin1String("off"))
        return kAutomationOff;
    return {};
}

}  // namespace sub::ui
