#pragma once

// The Ableton-style number box, drawn on the scene graph; ValueBox.qml wraps it
// with the text field typing opens.
//
// Drag vertically to change it (the cursor hides meanwhile): kDragRate steps
// a pixel, kFineDragRate with Shift (pressing or letting go of Shift mid-drag
// changes the rate from there on). The wheel moves ten steps a notch (one choice with `choices`); notches
// less than kWheelGesture seconds apart are one gesture. With `choices` it only
// takes those values (the time signature's denominator) and a drag moves one
// choice per 10 pixels, counted from the press. Values are rounded to
// `decimals` and kept between `from` and `to`.
//
// Double-click: with a `defaultValue`, resets to it (and typing a digit, the box
// having the focus after a click, opens the text field: the digit, "-", "+" or
// "." doesn't go to the window's shortcuts, such as 0 or -); without one, opens the
// text field showing the value (its first word) all selected. The default
// parser (parseNumber) strips "dB", "bpm" and "%" and reads "-inf" as -70.
//
// `logScale` (only when `from` > 0) drags and wheels evenly in log(value), as
// a knob does (KnobItem::kDragPixels for the whole range, a notch 1/50 of it),
// for frequencies.
//
// Signals and `relative` as KnobItem's: `moved(value, gestureKey)` for user
// changes only, one key per drag or wheel run; `touched()` on a left press or
// double-click.

#include "controls/DragCursor.h"
#include "sg/SgCanvas.h"

#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QJSValue>
#include <QList>
#include <QString>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <optional>

namespace sub::ui {

class ValueBoxItem : public SgCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(qreal value READ value WRITE setValue NOTIFY valueChanged)
    Q_PROPERTY(qreal from READ from WRITE setFrom NOTIFY rangeChanged)
    Q_PROPERTY(qreal to READ to WRITE setTo NOTIFY rangeChanged)
    Q_PROPERTY(qreal step READ step WRITE setStep NOTIFY rangeChanged)
    Q_PROPERTY(int decimals READ decimals WRITE setDecimals NOTIFY rangeChanged)
    Q_PROPERTY(QList<qreal> choices READ choices WRITE setChoices NOTIFY rangeChanged)
    Q_PROPERTY(bool logScale READ logScale WRITE setLogScale NOTIFY rangeChanged)
    Q_PROPERTY(QVariant defaultValue READ defaultValue WRITE setDefaultValue NOTIFY defaultValueChanged)
    Q_PROPERTY(bool wheel READ wheel WRITE setWheel NOTIFY wheelChanged)
    Q_PROPERTY(QString automation READ automation WRITE setAutomation NOTIFY lookChanged)
    Q_PROPERTY(QJSValue formatter READ formatter WRITE setFormatter NOTIFY textChanged)
    Q_PROPERTY(QJSValue parser READ parser WRITE setParser NOTIFY parserChanged)
    Q_PROPERTY(QString sampleText READ sampleText WRITE setSampleText NOTIFY lookChanged)
    Q_PROPERTY(QFont font READ font WRITE setFont NOTIFY lookChanged)
    Q_PROPERTY(QString text READ text NOTIFY textChanged)
    Q_PROPERTY(bool relative READ relative NOTIFY relativeChanged)
    Q_PROPERTY(bool dragging READ dragging NOTIFY draggingChanged)
    Q_PROPERTY(bool hovered READ hovered NOTIFY hoveredChanged)
    Q_PROPERTY(bool typeable READ typeable NOTIFY defaultValueChanged)  // typing a digit edits (has a default)
    // The flat box of a track's header (Ableton's layout): tighter corners;
    // with a `fill` (0..1, from `fillFrom`: 0 for a volume, 0.5 for a pan; < 0:
    // none) drawn in `fillColor`, a slider.
    Q_PROPERTY(bool flat READ flat WRITE setFlat NOTIFY lookChanged)
    Q_PROPERTY(qreal fill READ fill WRITE setFill NOTIFY lookChanged)
    Q_PROPERTY(qreal fillFrom READ fillFrom WRITE setFillFrom NOTIFY lookChanged)
    Q_PROPERTY(QColor fillColor READ fillColor WRITE setFillColor NOTIFY lookChanged)

public:
    static constexpr double kDragRate = 0.25;       // steps per pixel dragged
    static constexpr double kFineDragRate = 0.025;  // with Shift
    static constexpr double kWheelGesture = 0.6;    // seconds: wheel notches closer than this are one gesture
    static constexpr double kChoicePixels = 10.0;   // dragged this far, a box with choices moves one

    explicit ValueBoxItem(QQuickItem* parent = nullptr);

    qreal value() const { return value_; }
    void setValue(qreal value);
    qreal from() const { return from_; }
    void setFrom(qreal from);
    qreal to() const { return to_; }
    void setTo(qreal to);
    qreal step() const { return step_; }
    void setStep(qreal step);
    int decimals() const { return decimals_; }
    void setDecimals(int decimals);
    QList<qreal> choices() const { return choices_; }
    void setChoices(const QList<qreal>& choices);
    bool logScale() const { return logScale_; }
    void setLogScale(bool logScale);
    // A number, or undefined for none.
    QVariant defaultValue() const { return default_ ? QVariant(*default_) : QVariant(); }
    void setDefaultValue(const QVariant& value);
    bool wheel() const { return wheel_; }
    void setWheel(bool wheel);
    QString automation() const { return automation_; }
    void setAutomation(const QString& state);  // "", "on" or "off"
    QJSValue formatter() const { return formatter_; }
    void setFormatter(const QJSValue& formatter);
    QJSValue parser() const { return parser_; }
    void setParser(const QJSValue& parser);
    QString sampleText() const { return sampleText_; }
    void setSampleText(const QString& text);
    QFont font() const { return font_; }
    void setFont(const QFont& font);
    QString text() const { return text_; }
    bool relative() const { return relative_; }
    bool dragging() const { return drag_.has_value(); }
    bool hovered() const { return hovered_; }
    bool typeable() const { return default_.has_value(); }
    bool flat() const { return flat_; }
    void setFlat(bool flat);
    qreal fill() const { return fill_; }
    void setFill(qreal fill);
    qreal fillFrom() const { return fillFrom_; }
    void setFillFrom(qreal from);
    QColor fillColor() const { return fillColor_; }
    void setFillColor(const QColor& color);

    // C++ alternatives to the JavaScript formatter and parser.
    void setFormatFunction(std::function<QString(double)> format);
    void setParseFunction(std::function<std::optional<double>(const QString&)> parse);

    QString format(double value) const;
    // A number from what was typed: "-12.5 dB" -> -12.5, "120bpm" -> 120, "50 %" -> 50, "-inf" -> -70.
    static std::optional<double> parseFloat(const QString& text);
    // The same for QML: a number, or null.
    Q_INVOKABLE static QVariant parseNumber(const QString& text);
    // The text field's text: parsed and set as a user change. False if it didn't parse.
    Q_INVOKABLE bool applyTyped(const QString& text);

Q_SIGNALS:
    void valueChanged();
    void rangeChanged();
    void defaultValueChanged();
    void wheelChanged();
    void lookChanged();
    void textChanged();
    void parserChanged();
    void relativeChanged();
    void draggingChanged();
    void hoveredChanged();
    void moved(qreal value, const QString& gestureKey);
    void touched();
    // Open the text field with `initialText`, all of it selected or not.
    void editRequested(const QString& initialText, bool selectAll);

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
    void hoverEnterEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    bool event(QEvent* event) override;

private:
    // Whether a key typed opens the text field with it (a digit...: see above).
    bool typesInto(const QKeyEvent* event) const;
    struct Drag {
        qreal originY;
        double originValue;
        // The last y and the value dragged to there, unrounded, so slow (fine) drags add up.
        qreal lastY;
        double lastValue;
        QString gesture;
    };

    double constrain(double value) const;
    bool effectiveLog() const { return logScale_ && from_ > 0 && to_ > 0; }
    // `value` moved by `fraction` of the whole range, in log(value).
    double movedInLog(double value, double fraction) const;
    int choiceIndex(double value) const;
    void setUser(double value, const QString& gesture, bool relative);
    void setFromUser(double value, const QString& gesture);
    void setRelative(bool relative);
    void updateText();
    void updateImplicitSize();
    void endDrag();

    double value_ = 0.0;
    double from_ = 0.0;
    double to_ = 1.0;
    double step_ = 0.01;
    int decimals_ = 2;
    QList<qreal> choices_;
    bool logScale_ = false;
    std::optional<double> default_;
    bool wheel_ = true;
    QString automation_;
    QJSValue formatter_;
    QJSValue parser_;
    std::function<QString(double)> format_;
    std::function<std::optional<double>(const QString&)> parse_;
    QString sampleText_;
    QFont font_;
    QString text_;
    bool relative_ = true;
    bool hovered_ = false;
    std::optional<Drag> drag_;
    DragCursor cursor_;
    QString wheelGesture_;
    QElapsedTimer wheelClock_;  // since the gesture's last notch
    bool flat_ = false;
    qreal fill_ = -1.0;
    qreal fillFrom_ = 0.0;
    QColor fillColor_;
};

}  // namespace sub::ui
