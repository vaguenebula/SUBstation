#include "controls/ValueBoxItem.h"

#include "controls/KnobItem.h"
#include "input/Modifiers.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QFontMetrics>
#include <QKeyEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace sub::ui {

namespace {

const QString kTypingKeys = QStringLiteral("0123456789-+.");

}  // namespace

ValueBoxItem::ValueBoxItem(QQuickItem* parent) : SgCanvas(parent), font_(uiFont()) {
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
    updateText();
    updateImplicitSize();
}

void ValueBoxItem::componentComplete() {
    SgCanvas::componentComplete();
    value_ = constrain(value_);  // made from QML: the range is known only now
    updateText();
    updateImplicitSize();
    update();
}

// --- Value ---------------------------------------------------------------------

double ValueBoxItem::constrain(double value) const {
    if (!choices_.isEmpty()) {
        double best = choices_.first();
        for (qreal choice : choices_)
            if (std::abs(choice - value) < std::abs(best - value))
                best = choice;
        return best;
    }
    value = std::clamp(value, std::min(from_, to_), std::max(from_, to_));
    const double scale = std::pow(10.0, decimals_);
    return std::nearbyint(value * scale) / scale;  // Python's round(): halves to even
}

int ValueBoxItem::choiceIndex(double value) const {
    const qsizetype index = choices_.indexOf(value);
    return index < 0 ? 0 : int(index);
}

void ValueBoxItem::setValue(qreal value) {
    if (isComponentComplete())
        value = constrain(value);
    if (value == value_)
        return;
    value_ = value;
    updateText();
    update();
    Q_EMIT valueChanged();
}

void ValueBoxItem::setFrom(qreal from) {
    if (from == from_)
        return;
    from_ = from;
    Q_EMIT rangeChanged();
    if (isComponentComplete())
        setValue(value_);
}

void ValueBoxItem::setTo(qreal to) {
    if (to == to_)
        return;
    to_ = to;
    Q_EMIT rangeChanged();
    if (isComponentComplete()) {
        setValue(value_);
        updateImplicitSize();
    }
}

void ValueBoxItem::setStep(qreal step) {
    if (step == step_)
        return;
    step_ = step;
    Q_EMIT rangeChanged();
}

void ValueBoxItem::setDecimals(int decimals) {
    if (decimals == decimals_)
        return;
    decimals_ = decimals;
    Q_EMIT rangeChanged();
    updateText();
    updateImplicitSize();
    if (isComponentComplete())
        setValue(value_);
}

void ValueBoxItem::setChoices(const QList<qreal>& choices) {
    if (choices == choices_)
        return;
    choices_ = choices;
    Q_EMIT rangeChanged();
    if (isComponentComplete())
        setValue(value_);
}

void ValueBoxItem::setLogScale(bool logScale) {
    if (logScale == logScale_)
        return;
    logScale_ = logScale;
    Q_EMIT rangeChanged();
}

double ValueBoxItem::movedInLog(double value, double fraction) const {
    return std::max(value, 1e-12) * std::pow(to_ / from_, fraction);
}

void ValueBoxItem::setDefaultValue(const QVariant& value) {
    std::optional<double> number;
    bool ok = false;
    const double v = value.toDouble(&ok);
    if (value.isValid() && !value.isNull() && ok)
        number = v;
    if (number == default_)
        return;
    default_ = number;
    Q_EMIT defaultValueChanged();
}

void ValueBoxItem::setWheel(bool wheel) {
    if (wheel == wheel_)
        return;
    wheel_ = wheel;
    Q_EMIT wheelChanged();
}

void ValueBoxItem::setAutomation(const QString& state) {
    if (state == automation_)
        return;
    automation_ = state;
    update();
    Q_EMIT lookChanged();
}

void ValueBoxItem::setFormatter(const QJSValue& formatter) {
    formatter_ = formatter;
    updateText();
    updateImplicitSize();
}

void ValueBoxItem::setParser(const QJSValue& parser) {
    parser_ = parser;
    Q_EMIT parserChanged();
}

void ValueBoxItem::setFormatFunction(std::function<QString(double)> format) {
    format_ = std::move(format);
    updateText();
    updateImplicitSize();
}

void ValueBoxItem::setParseFunction(std::function<std::optional<double>(const QString&)> parse) {
    parse_ = std::move(parse);
    Q_EMIT parserChanged();
}

void ValueBoxItem::setSampleText(const QString& text) {
    if (text == sampleText_)
        return;
    sampleText_ = text;
    updateImplicitSize();
    Q_EMIT lookChanged();
}

void ValueBoxItem::setFont(const QFont& font) {
    if (font == font_)
        return;
    font_ = font;
    updateImplicitSize();
    update();
    Q_EMIT lookChanged();
}

QString ValueBoxItem::format(double value) const {
    if (format_)
        return format_(value);
    if (formatter_.isCallable())
        return QJSValue(formatter_).call({QJSValue(value)}).toString();
    return QString::number(value, 'f', decimals_);
}

void ValueBoxItem::updateText() {
    const QString text = format(value_);
    if (text != text_) {
        text_ = text;
        Q_EMIT textChanged();
    }
}

void ValueBoxItem::updateImplicitSize() {
    // Wide enough for the sample text, or the maximum formatted.
    const QFontMetrics metrics(font_);
    const QString text = sampleText_.isEmpty() ? format(to_) : sampleText_;
    setImplicitSize(metrics.horizontalAdvance(text) + 16, std::max(20, metrics.height() + 6));
}

void ValueBoxItem::setRelative(bool relative) {
    if (relative == relative_)
        return;
    relative_ = relative;
    Q_EMIT relativeChanged();
}

void ValueBoxItem::setUser(double value, const QString& gesture, bool relative) {
    setRelative(relative);
    setFromUser(value, gesture);
}

void ValueBoxItem::setFromUser(double value, const QString& gesture) {
    value = constrain(value);
    if (value == value_)
        return;
    value_ = value;
    updateText();
    update();
    Q_EMIT valueChanged();
    Q_EMIT moved(value, gesture);
}

std::optional<double> ValueBoxItem::parseFloat(const QString& text) {
    QString cleaned = text.trimmed().toLower();
    for (const QString& suffix : {QStringLiteral("db"), QStringLiteral("bpm"), QStringLiteral("%")}) {
        if (cleaned.endsWith(suffix))
            cleaned.chop(suffix.size());
        cleaned = cleaned.trimmed();
    }
    if (cleaned == QLatin1String("-inf") || cleaned == QLatin1String("inf"))
        return -70.0;
    bool ok = false;
    const double value = QLocale::c().toDouble(cleaned, &ok);
    if (!ok || !std::isfinite(value))
        return std::nullopt;
    return value;
}

QVariant ValueBoxItem::parseNumber(const QString& text) {
    const std::optional<double> value = parseFloat(text);
    return value ? QVariant(*value) : QVariant::fromValue(nullptr);
}

bool ValueBoxItem::applyTyped(const QString& text) {
    std::optional<double> parsed;
    if (parse_) {
        parsed = parse_(text);
    } else if (parser_.isCallable()) {
        const QJSValue result = QJSValue(parser_).call({QJSValue(text)});
        if (result.isNumber() && std::isfinite(result.toNumber()))
            parsed = result.toNumber();
    } else {
        parsed = parseFloat(text);
    }
    if (!parsed)
        return false;
    setUser(*parsed, newGestureKey(), false);
    return true;
}

// --- Painting ------------------------------------------------------------------------

void ValueBoxItem::setFlat(bool flat) {
    if (flat == flat_) return;
    flat_ = flat;
    update();
    Q_EMIT lookChanged();
}

void ValueBoxItem::setFill(qreal fill) {
    if (fill == fill_) return;
    fill_ = fill;
    update();
    Q_EMIT lookChanged();
}

void ValueBoxItem::setFillFrom(qreal from) {
    if (from == fillFrom_) return;
    fillFrom_ = from;
    update();
    Q_EMIT lookChanged();
}

void ValueBoxItem::setFillColor(const QColor& color) {
    if (color == fillColor_) return;
    fillColor_ = color;
    update();
    Q_EMIT lookChanged();
}

void ValueBoxItem::paint(SgPainter& p) {
    if (flat_) {
        // A track header's box: tighter corners, the slider's fill under the value.
        p.setAntialiasing(true);
        const QRectF rect = QRectF(0, 0, width(), height()).adjusted(0.5, 0.5, -0.5, -0.5);
        const bool active = hovered_ || dragging();
        p.fillRoundedRect(rect, 2, 2, active ? Theme::surfaceHover() : Theme::surface());
        if (fill_ >= 0 && fillColor_.isValid()) {
            const QRectF inside = rect.adjusted(1, 1, -1, -1);
            const double a = inside.left() + std::clamp(fillFrom_, 0.0, 1.0) * inside.width();
            const double b = inside.left() + std::clamp(fill_, 0.0, 1.0) * inside.width();
            if (std::abs(b - a) >= 0.5) {
                p.fillRect(QRectF(std::min(a, b), inside.top(), std::abs(b - a), inside.height()), fillColor_);
            }
        }
        p.drawRoundedRect(rect, 2, 2, Theme::border());
        p.drawText(rect, Qt::AlignCenter, text_, dragging() ? Theme::accent() : Theme::text(), font_);
        drawAutomationDot(p, automation_, QPointF(4.0, rect.center().y()));
        return;
    }
    p.setAntialiasing(true);
    const QRectF rect = QRectF(0, 0, width(), height()).adjusted(0.5, 0.5, -0.5, -0.5);
    const bool active = hovered_ || dragging();
    p.fillRoundedRect(rect, 3, 3, active ? Theme::surfaceHover() : Theme::surface());
    p.drawRoundedRect(rect, 3, 3, Theme::border());
    p.drawText(rect, Qt::AlignCenter, text_, dragging() ? Theme::accent() : Theme::text(), font_);
    drawAutomationDot(p, automation_, QPointF(6.0, rect.center().y()));
}

void ValueBoxItem::hoverEnterEvent(QHoverEvent*) {
    hovered_ = true;
    update();
    Q_EMIT hoveredChanged();
}

void ValueBoxItem::hoverLeaveEvent(QHoverEvent*) {
    hovered_ = false;
    update();
    Q_EMIT hoveredChanged();
}

// --- Interaction -----------------------------------------------------------------------

void ValueBoxItem::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    Q_EMIT touched();
    if (typeable())
        forceActiveFocus(Qt::MouseFocusReason);  // so typing a digit edits
    const bool wasDragging = dragging();
    const qreal y = event->position().y();
    drag_ = Drag{y, value_, y, value_, newGestureKey()};
    cursor_.press(event->globalPosition());
    update();
    if (!wasDragging)
        Q_EMIT draggingChanged();
    event->accept();
}

void ValueBoxItem::mouseMoveEvent(QMouseEvent* event) {
    if (!drag_ || !cursor_.dragging())
        return;
    const qreal y = event->position().y();
    const double dy = drag_->originY - y;
    setRelative(true);
    if (!choices_.isEmpty()) {  // (counted from the press: the cursor doesn't jump)
        cursor_.moved(this, event->position(), event->globalPosition(), false);
        const int index = choiceIndex(constrain(drag_->originValue)) + int(dy / kChoicePixels);
        setFromUser(choices_[std::clamp(index, 0, int(choices_.size()) - 1)], drag_->gesture);
        return;
    }
    // From where the mouse was last, so pressing or letting go of Shift mid-drag
    // changes the rate from here on (not the whole drag).
    const bool fine = event->modifiers() & Qt::ShiftModifier;
    double value = 0.0;
    if (effectiveLog()) {
        const double pixels = fine ? KnobItem::kFineDragPixels : KnobItem::kDragPixels;
        value = movedInLog(drag_->lastValue, (drag_->lastY - y) / pixels);
    } else {
        value = drag_->lastValue + (drag_->lastY - y) * step_ * (fine ? kFineDragRate : kDragRate);
    }
    value = std::clamp(value, std::min(from_, to_), std::max(from_, to_));
    drag_->lastY = cursor_.moved(this, event->position(), event->globalPosition());
    drag_->lastValue = value;
    setFromUser(value, drag_->gesture);
}

void ValueBoxItem::endDrag() {
    cursor_.release();
    if (drag_) {
        drag_.reset();
        update();
        Q_EMIT draggingChanged();
    }
}

void ValueBoxItem::mouseReleaseEvent(QMouseEvent*) { endDrag(); }

void ValueBoxItem::mouseUngrabEvent() { endDrag(); }

void ValueBoxItem::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    Q_EMIT touched();
    if (!default_)
        Q_EMIT editRequested(format(value_).split(QLatin1Char(' ')).first(), true);
    else
        setUser(*default_, newGestureKey(), false);
}

void ValueBoxItem::wheelEvent(QWheelEvent* event) {
    if (!wheel_) {
        event->ignore();
        return;
    }
    const double notches = event->angleDelta().y() / 120.0;
    if (notches == 0.0)
        return;
    setRelative(true);
    if (effectiveLog()) {  // (a notch is a gesture, as a knob's)
        setFromUser(movedInLog(value_, notches / KnobItem::kWheelNotches), newGestureKey());
        event->accept();
        return;
    }
    if (wheelGesture_.isEmpty() || !wheelClock_.isValid() || wheelClock_.elapsed() > kWheelGesture * 1000.0)
        wheelGesture_ = newGestureKey();
    wheelClock_.start();
    if (!choices_.isEmpty()) {
        const int index = choiceIndex(value_) + (notches > 0 ? 1 : -1);
        setFromUser(choices_[std::clamp(index, 0, int(choices_.size()) - 1)], wheelGesture_);
    } else {
        setFromUser(value_ + notches * step_ * 10, wheelGesture_);
    }
    event->accept();
}

bool ValueBoxItem::typesInto(const QKeyEvent* event) const {
    return default_ && !event->text().isEmpty() && kTypingKeys.contains(event->text()) &&
           !hasShortcutModifier(event->modifiers());
}

void ValueBoxItem::keyPressEvent(QKeyEvent* event) {
    if (typesInto(event)) {
        Q_EMIT editRequested(event->text(), false);
        event->accept();
        return;
    }
    event->ignore();
}

bool ValueBoxItem::event(QEvent* event) {
    // What it types into its text field comes to it as a key press, not to the window's shortcuts.
    if (event->type() == QEvent::ShortcutOverride && typesInto(static_cast<QKeyEvent*>(event))) {
        event->accept();
        return true;
    }
    return SgCanvas::event(event);
}

}  // namespace sub::ui
