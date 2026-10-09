#include "controls/KnobItem.h"

#include "input/Modifiers.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace sub::ui {

namespace {

constexpr double kPi = 3.14159265358979323846;
const QString kTypingKeys = QStringLiteral("0123456789-+.");

// A JavaScript parser's answer: a finite number, or nothing.
std::optional<double> parsedNumber(const QJSValue& result) {
    if (!result.isNumber())
        return std::nullopt;
    const double value = result.toNumber();
    if (!std::isfinite(value))
        return std::nullopt;
    return value;
}

}  // namespace

void drawAutomationDot(SgPainter& p, const QString& state, const QPointF& at) {
    const QColor color = Theme::automationDotColor(state);
    if (!color.isValid())
        return;
    p.save();
    p.setAntialiasing(true);
    p.fillEllipse(at, 2.5, 2.5, color);
    p.restore();
}

KnobItem::KnobItem(QQuickItem* parent) : SgCanvas(parent), color_(Theme::kAccent) {
    setAcceptedMouseButtons(Qt::LeftButton);
    setImplicitSize(28, 28);  // at least 22 x 22, as the Python UI's knob
    updateText();
}

void KnobItem::componentComplete() {
    SgCanvas::componentComplete();
    // Made from QML: the range is known only now.
    value_ = clamped(value_);
    if (!default_)
        default_ = value_;  // without a default, double-click goes back to where it started
    updateText();
    update();
}

double KnobItem::clamped(double value) const { return std::clamp(value, std::min(from_, to_), std::max(from_, to_)); }

void KnobItem::setValue(qreal value) {
    if (isComponentComplete())
        value = clamped(value);
    if (value == value_)
        return;
    value_ = value;
    updateText();
    update();
    Q_EMIT valueChanged();
}

void KnobItem::setFrom(qreal from) {
    if (from == from_)
        return;
    from_ = from;
    Q_EMIT rangeChanged();
    if (isComponentComplete())
        setValue(value_);
    update();
}

void KnobItem::setTo(qreal to) {
    if (to == to_)
        return;
    to_ = to;
    Q_EMIT rangeChanged();
    if (isComponentComplete())
        setValue(value_);
    update();
}

void KnobItem::setDefaultValue(qreal value) {
    if (default_ && *default_ == value)
        return;
    default_ = value;
    Q_EMIT defaultValueChanged();
}

void KnobItem::resetDefaultValue() {
    default_.reset();
    Q_EMIT defaultValueChanged();
}

void KnobItem::setBipolar(bool bipolar) {
    if (bipolar == bipolar_)
        return;
    bipolar_ = bipolar;
    update();
    Q_EMIT lookChanged();
}

void KnobItem::setLogScale(bool logScale) {
    if (logScale == logScale_)
        return;
    logScale_ = logScale;
    update();
    Q_EMIT rangeChanged();
}

void KnobItem::setStep(qreal step) {
    if (step == step_)
        return;
    step_ = step;
    Q_EMIT rangeChanged();
}

void KnobItem::setWheel(bool wheel) {
    if (wheel == wheel_)
        return;
    wheel_ = wheel;
    Q_EMIT wheelChanged();
}

void KnobItem::setColor(const QColor& color) {
    if (color == color_)
        return;
    color_ = color;
    update();
    Q_EMIT lookChanged();
}

void KnobItem::setAutomation(const QString& state) {
    if (state == automation_)
        return;
    automation_ = state;
    update();
    Q_EMIT lookChanged();
}

void KnobItem::setFormatter(const QJSValue& formatter) {
    formatter_ = formatter;
    updateText();
}

void KnobItem::setParser(const QJSValue& parser) {
    parser_ = parser;
    Q_EMIT typeableChanged();
}

void KnobItem::setFormatFunction(std::function<QString(double)> format) {
    format_ = std::move(format);
    updateText();
}

void KnobItem::setParseFunction(std::function<std::optional<double>(const QString&)> parse) {
    parse_ = std::move(parse);
    Q_EMIT typeableChanged();
}

bool KnobItem::typeable() const { return bool(parse_) || parser_.isCallable(); }

void KnobItem::updateText() {
    QString text;
    if (format_)
        text = format_(value_);
    else if (formatter_.isCallable())
        text = formatter_.call({QJSValue(value_)}).toString();
    else
        text = QString::number(value_, 'f', 2);
    if (text != text_) {
        text_ = text;
        Q_EMIT textChanged();
    }
}

double KnobItem::fraction(double value) const {
    if (effectiveLog())
        return std::log(value / from_) / std::log(to_ / from_);
    if (to_ == from_)
        return 0.0;
    return (value - from_) / (to_ - from_);
}

double KnobItem::fromFraction(double fraction) const {
    fraction = std::clamp(fraction, 0.0, 1.0);
    if (effectiveLog())
        return from_ * std::pow(to_ / from_, fraction);
    return from_ + fraction * (to_ - from_);
}

void KnobItem::setRelative(bool relative) {
    if (relative == relative_)
        return;
    relative_ = relative;
    Q_EMIT relativeChanged();
}

void KnobItem::setFromUser(double value, const QString& gesture) {
    if (step_ > 0)
        value = from_ + std::nearbyint((value - from_) / step_) * step_;
    value = clamped(value);
    if (value == value_)
        return;
    value_ = value;
    updateText();
    update();
    Q_EMIT valueChanged();
    Q_EMIT moved(value, gesture);
}

bool KnobItem::applyTyped(const QString& text) {
    std::optional<double> parsed;
    if (parse_)
        parsed = parse_(text);
    else if (parser_.isCallable())
        parsed = parsedNumber(parser_.call({QJSValue(text)}));
    if (!parsed)
        return false;
    setRelative(false);
    setFromUser(*parsed, newGestureKey());
    return true;
}

void KnobItem::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const double side = std::min(width(), height()) - 4;
    if (side <= 0)
        return;
    const QRectF rect((width() - side) / 2, (height() - side) / 2, side, side);
    p.drawArc(rect, kStartAngle - kSpan, kSpan, Theme::kSurfaceHover, 3, Qt::FlatCap);

    const double origin = bipolar_ ? 0.5 : 0.0;
    const double frac = fraction(value_);
    const double start = kStartAngle - origin * kSpan;
    const double sweep = -(frac - origin) * kSpan;
    if (std::abs(sweep) > 0.5)
        p.drawArc(rect, start, sweep, color_, 3, Qt::FlatCap);

    const double angle = (kStartAngle - frac * kSpan) * kPi / 180.0;
    const QPointF center = rect.center();
    const double radius = side / 2 - 2;
    const QPointF tip(center.x() + std::cos(angle) * radius, center.y() - std::sin(angle) * radius);
    const QPointF inner(center.x() + std::cos(angle) * radius * 0.3, center.y() - std::sin(angle) * radius * 0.3);
    p.drawLine(inner, tip, Theme::kText, 2, Qt::RoundCap);
    drawAutomationDot(p, automation_, QPointF(width() - 3.5, 3.5));
}

void KnobItem::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    Q_EMIT touched();
    if (typeable())
        forceActiveFocus(Qt::MouseFocusReason);  // so typing a digit edits
    const bool wasDragging = dragging();
    drag_ = Drag{event->position().y(), fraction(value_), newGestureKey()};
    cursor_.press(event->globalPosition());
    if (!wasDragging)
        Q_EMIT draggingChanged();
    event->accept();
}

void KnobItem::mouseMoveEvent(QMouseEvent* event) {
    if (!drag_ || !cursor_.dragging())
        return;
    // Moved on from where the mouse was last, so pressing or letting go of
    // Shift mid-drag changes the rate from here on (not the whole drag).
    const qreal y = event->position().y();
    const double pixels = (event->modifiers() & Qt::ShiftModifier) ? kFineDragPixels : kDragPixels;
    drag_->fraction = std::clamp(drag_->fraction + (drag_->lastY - y) / pixels, 0.0, 1.0);
    drag_->lastY = cursor_.moved(this, event->position(), event->globalPosition());
    setRelative(true);
    setFromUser(fromFraction(drag_->fraction), drag_->gesture);
}

void KnobItem::endDrag() {
    cursor_.release();
    if (drag_) {
        drag_.reset();
        Q_EMIT draggingChanged();
    }
}

void KnobItem::mouseReleaseEvent(QMouseEvent*) { endDrag(); }

void KnobItem::mouseUngrabEvent() { endDrag(); }

void KnobItem::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    Q_EMIT touched();
    setRelative(false);
    setFromUser(defaultValue(), newGestureKey());
}

void KnobItem::wheelEvent(QWheelEvent* event) {
    if (!wheel_) {
        event->ignore();
        return;
    }
    setRelative(true);
    const double notches = event->angleDelta().y() / 120.0;
    setFromUser(fromFraction(fraction(value_) + notches / kWheelNotches), newGestureKey());
    event->accept();
}

bool KnobItem::typesInto(const QKeyEvent* event) const {
    return typeable() && !event->text().isEmpty() && kTypingKeys.contains(event->text()) &&
           !hasShortcutModifier(event->modifiers());
}

void KnobItem::keyPressEvent(QKeyEvent* event) {
    if (typesInto(event)) {
        Q_EMIT editRequested(event->text());
        event->accept();
        return;
    }
    event->ignore();
}

bool KnobItem::event(QEvent* event) {
    // What it types into its text field comes to it as a key press, not to the window's shortcuts.
    if (event->type() == QEvent::ShortcutOverride && typesInto(static_cast<QKeyEvent*>(event))) {
        event->accept();
        return true;
    }
    return SgCanvas::event(event);
}

}  // namespace sub::ui
