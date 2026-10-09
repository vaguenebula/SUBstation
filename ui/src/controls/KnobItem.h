#pragma once

// The rotary knob, drawn on the scene graph; Knob.qml wraps it with its tooltip
// and the small text field typing opens.
//
// Drag vertically (the cursor hides meanwhile): kDragPixels for the whole
// range, kFineDragPixels with Shift; pressing or letting go of Shift mid-drag
// changes the rate from there on. The wheel moves 1/50 of the range a notch
// (unless `wheel` is false: the event goes on to what is under it, a scrolling
// list). Double-click resets to `defaultValue` (if never set: the value the knob
// had when it was made). `logScale` (only when `from` > 0) moves evenly in
// log(value), for frequencies and times; `step` only takes multiples of it
// counted from `from`; `bipolar` draws the arc from the middle. With a
// `parser`, typing a digit (the knob has the focus after a click) opens a small
// text field (editRequested); applyTyped() parses and sets what was typed.
// A dot in its corner marks it automated ("on", red) or its automation
// overridden ("off", grey).
//
// `moved(value, gestureKey)` is emitted for user changes only, never for
// `value` set from outside: every value of one drag shares a gesture key (a new
// one for each wheel notch, reset or typed value), for the editor's undo merge
// key. `touched()` is emitted on every left press or double-click (pressing a
// control shows its automation). `relative` says whether the last user change
// was a drag or wheel (rather than typed or reset).

#include "controls/DragCursor.h"
#include "input/GestureKey.h"
#include "sg/SgCanvas.h"

#include <QColor>
#include <QJSValue>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <optional>

namespace sub::ui {

class SgPainter;

// The automation dot both knobs and value boxes draw: "on" red, "off" grey, else nothing.
void drawAutomationDot(SgPainter& painter, const QString& state, const QPointF& at);

class KnobItem : public SgCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(qreal value READ value WRITE setValue NOTIFY valueChanged)
    Q_PROPERTY(qreal from READ from WRITE setFrom NOTIFY rangeChanged)
    Q_PROPERTY(qreal to READ to WRITE setTo NOTIFY rangeChanged)
    Q_PROPERTY(qreal defaultValue READ defaultValue WRITE setDefaultValue RESET resetDefaultValue NOTIFY
                   defaultValueChanged)
    Q_PROPERTY(bool bipolar READ bipolar WRITE setBipolar NOTIFY lookChanged)
    Q_PROPERTY(bool logScale READ logScale WRITE setLogScale NOTIFY rangeChanged)
    Q_PROPERTY(qreal step READ step WRITE setStep NOTIFY rangeChanged)
    Q_PROPERTY(bool wheel READ wheel WRITE setWheel NOTIFY wheelChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY lookChanged)
    Q_PROPERTY(QString automation READ automation WRITE setAutomation NOTIFY lookChanged)
    Q_PROPERTY(QJSValue formatter READ formatter WRITE setFormatter NOTIFY textChanged)
    Q_PROPERTY(QJSValue parser READ parser WRITE setParser NOTIFY typeableChanged)
    Q_PROPERTY(QString text READ text NOTIFY textChanged)  // the value as the formatter has it (the tooltip)
    Q_PROPERTY(bool relative READ relative NOTIFY relativeChanged)
    Q_PROPERTY(bool dragging READ dragging NOTIFY draggingChanged)
    Q_PROPERTY(bool typeable READ typeable NOTIFY typeableChanged)  // has a parser

public:
    static constexpr double kStartAngle = 225.0;  // degrees, Qt's convention (0 = 3 o'clock, counter-clockwise)
    static constexpr double kSpan = 270.0;
    static constexpr double kDragPixels = 600.0;      // dragged this far, a knob turns through its whole range
    static constexpr double kFineDragPixels = 6000.0;  // with Shift
    static constexpr double kWheelNotches = 50.0;     // notches for the whole range

    explicit KnobItem(QQuickItem* parent = nullptr);

    qreal value() const { return value_; }
    void setValue(qreal value);
    qreal from() const { return from_; }
    void setFrom(qreal from);
    qreal to() const { return to_; }
    void setTo(qreal to);
    qreal defaultValue() const { return default_.value_or(value_); }
    void setDefaultValue(qreal value);
    void resetDefaultValue();
    bool bipolar() const { return bipolar_; }
    void setBipolar(bool bipolar);
    bool logScale() const { return logScale_; }
    void setLogScale(bool logScale);
    qreal step() const { return step_; }
    void setStep(qreal step);
    bool wheel() const { return wheel_; }
    void setWheel(bool wheel);
    QColor color() const { return color_; }
    void setColor(const QColor& color);
    QString automation() const { return automation_; }
    void setAutomation(const QString& state);  // "", "on" or "off"
    QJSValue formatter() const { return formatter_; }
    void setFormatter(const QJSValue& formatter);
    QJSValue parser() const { return parser_; }
    void setParser(const QJSValue& parser);
    QString text() const { return text_; }
    bool relative() const { return relative_; }
    bool dragging() const { return drag_.has_value(); }
    bool typeable() const;

    // C++ alternatives to the JavaScript formatter and parser.
    void setFormatFunction(std::function<QString(double)> format);
    void setParseFunction(std::function<std::optional<double>(const QString&)> parse);

    // Where `value` sits in the range, 0 to 1 (evenly in log(value) with logScale), and back.
    double fraction(double value) const;
    double fromFraction(double fraction) const;

    // The text field's text: parsed, and set as a user change (a gesture of its
    // own, not relative). False if it didn't parse.
    Q_INVOKABLE bool applyTyped(const QString& text);

Q_SIGNALS:
    void valueChanged();
    void rangeChanged();
    void defaultValueChanged();
    void lookChanged();
    void wheelChanged();
    void textChanged();
    void relativeChanged();
    void draggingChanged();
    void typeableChanged();
    void moved(qreal value, const QString& gestureKey);
    void touched();
    void editRequested(const QString& initialText);

protected:
    void paint(SgPainter& painter) override;
    void componentComplete() override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool event(QEvent* event) override;

private:
    // Whether a key typed opens the text field with it (a digit, "-", "+" or
    // "."; it doesn't go to the window's shortcuts then, such as 0 or -).
    bool typesInto(const QKeyEvent* event) const;

    struct Drag {
        qreal lastY;
        double fraction;  // where the drag has the knob, unrounded
        QString gesture;
    };

    bool effectiveLog() const { return logScale_ && from_ > 0; }
    double clamped(double value) const;
    void setFromUser(double value, const QString& gesture);
    void setRelative(bool relative);
    void updateText();
    void endDrag();

    double value_ = 0.0;
    double from_ = 0.0;
    double to_ = 1.0;
    std::optional<double> default_;
    bool bipolar_ = false;
    bool logScale_ = false;
    double step_ = 0.0;
    bool wheel_ = true;
    QColor color_;
    QString automation_;
    QJSValue formatter_;
    QJSValue parser_;
    std::function<QString(double)> format_;
    std::function<std::optional<double>(const QString&)> parse_;
    QString text_;
    bool relative_ = true;
    std::optional<Drag> drag_;
    DragCursor cursor_;
};

}  // namespace sub::ui
