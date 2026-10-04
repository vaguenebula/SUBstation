#include "controls/ValueBoxItem.h"

#include "controls/KnobItem.h"
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
    emit valueChanged();
}

void ValueBoxItem::setFrom(qreal from) {
    if (from == from_)
        return;
    from_ = from;
    emit rangeChanged();
    if (isComponentComplete())
        setValue(value_);
}

void ValueBoxItem::setTo(qreal to) {
    if (to == to_)
        return;
    to_ = to;
    emit rangeChanged();
    if (isComponentComplete()) {
        setValue(value_);
        updateImplicitSize();
    }
}

void ValueBoxItem::setStep(qreal step) {
    if (step == step_)
        return;
    step_ = step;
    emit rangeChanged();
}

void ValueBoxItem::setDecimals(int decimals) {
    if (decimals == decimals_)
        return;
    decimals_ = decimals;
    emit rangeChanged();
    updateText();
    updateImplicitSize();
    if (isComponentComplete())
        setValue(value_);
}

void ValueBoxItem::setChoices(const QList<qreal>& choices) {
    if (choices == choices_)
        return;
    choices_ = choices;
    emit rangeChanged();
    if (isComponentComplete())
        setValue(value_);
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
    emit defaultValueChanged();
}

void ValueBoxItem::setWheel(bool wheel) {
    if (wheel == wheel_)
        return;
    wheel_ = wheel;
    emit wheelChanged();
}

void ValueBoxItem::setAutomation(const QString& state) {
    if (state == automation_)
        return;
    automation_ = state;
    update();
    emit lookChanged();
}

void ValueBoxItem::setFormatter(const QJSValue& formatter) {
    formatter_ = formatter;
    updateText();
    updateImplicitSize();
}

void ValueBoxItem::setParser(const QJSValue& parser) {
    parser_ = parser;
    emit parserChanged();
}

void ValueBoxItem::setFormatFunction(std::function<QString(double)> format) {
    format_ = std::move(format);
    updateText();
    updateImplicitSize();
}

void ValueBoxItem::setParseFunction(std::function<std::optional<double>(const QString&)> parse) {
    parse_ = std::move(parse);
    emit parserChanged();
}

void ValueBoxItem::setSampleText(const QString& text) {
    if (text == sampleText_)
        return;
    sampleText_ = text;
    updateImplicitSize();
    emit lookChanged();
}

void ValueBoxItem::setFont(const QFont& font) {
    if (font == font_)
        return;
    font_ = font;
    updateImplicitSize();
    update();
    emit lookChanged();
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
        emit textChanged();
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
    emit relativeChanged();
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
    emit valueChanged();
    emit moved(value, gesture);
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

void ValueBoxItem::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const QRectF rect = QRectF(0, 0, width(), height()).adjusted(0.5, 0.5, -0.5, -0.5);
    const bool active = hovered_ || dragging();
    p.fillRoundedRect(rect, 3, 3, active ? Theme::kSurfaceHover : Theme::kSurface);
    p.drawRoundedRect(rect, 3, 3, Theme::kBorder);
    p.drawText(rect, Qt::AlignCenter, text_, dragging() ? Theme::kAccent : Theme::kText, font_);
    drawAutomationDot(p, automation_, QPointF(6.0, rect.center().y()));
}

void ValueBoxItem::hoverEnterEvent(QHoverEvent*) {
    hovered_ = true;
    update();
    emit hoveredChanged();
}

void ValueBoxItem::hoverLeaveEvent(QHoverEvent*) {
    hovered_ = false;
    update();
    emit hoveredChanged();
}

// --- Interaction -----------------------------------------------------------------------

void ValueBoxItem::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    emit touched();
    if (typeable())
        forceActiveFocus(Qt::MouseFocusReason);  // so typing a digit edits
    const bool wasDragging = dragging();
    const qreal y = event->position().y();
    drag_ = Drag{y, value_, y, value_, newGestureKey()};
    cursor_.press(event->globalPosition());
    update();
    if (!wasDragging)
        emit draggingChanged();
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
    const double rate = (event->modifiers() & Qt::ShiftModifier) ? kFineDragRate : kDragRate;
    double value = drag_->lastValue + (drag_->lastY - y) * step_ * rate;
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
        emit draggingChanged();
    }
}

void ValueBoxItem::mouseReleaseEvent(QMouseEvent*) { endDrag(); }

void ValueBoxItem::mouseUngrabEvent() { endDrag(); }

void ValueBoxItem::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    emit touched();
    if (!default_)
        emit editRequested(format(value_).split(QLatin1Char(' ')).first(), true);
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

void ValueBoxItem::keyPressEvent(QKeyEvent* event) {
    if (default_ && !event->text().isEmpty() && kTypingKeys.contains(event->text())) {
        emit editRequested(event->text(), false);
        event->accept();
        return;
    }
    event->ignore();
}

}  // namespace sub::ui
