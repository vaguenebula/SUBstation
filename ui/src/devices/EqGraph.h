#pragma once

// The EQ's curve (and the model of its bands), after FabFilter's Pro-Q:
// 10 Hz..22 kHz, ± EqView::range dB; each band's curve and the total from the
// engine's own design (sub::app::eqResponseDb), the selected and hovered bands
// filled in their colours, the total with a glow; the analyzer (EqAnalyzer, the
// displays "input" and "output") behind it.
//
// Hover the curve and a ghost band shows where a click adds one, of the type
// for where it is (kZones: a low cut at the far left, then a low shelf, bells,
// a high shelf and a high cut); keep the button down to drag it on. Drag a band
// across for its frequency and up and down for its gain (a cut, notch or band
// pass: its Q; Ctrl: the Q), Shift finely; the wheel sets its Q (Alt, or while
// a cut is dragged: its slope). Double-click a band to switch it off and on,
// Alt-click it (or select it and press Delete) to remove it; double-click
// anywhere else to add one. Right-click a band (bandMenuRequested) or the
// background (viewMenuRequested) for the menus QML shows. The labels at the
// top left and right switch the analyzer's mode and the range.
//
// Every change goes through ProjectEditor::setDeviceParams, so a drag (adding
// a band and dragging it on, too: the same parameters every move) is one undo
// step. The selected band's controls (EqBandPanel.qml) read `selectedBand` and
// call the invokables here.

#include "devices/DeviceCanvas.h"
#include "devices/EditorPaint.h"

#include "analysis/Spectrum.h"

#include <QColor>
#include <QElapsedTimer>
#include <QPointF>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <array>
#include <optional>
#include <vector>

namespace sub::ui {

class EqGraph : public DeviceCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int selected READ selected WRITE select NOTIFY selectedChanged)  // -1: none
    Q_PROPERTY(int hover READ hover NOTIFY hoverChanged)                        // the band under the mouse
    Q_PROPERTY(QVariantMap selectedBand READ selectedBand NOTIFY bandsChanged)  // band(selected)
    Q_PROPERTY(bool hasBands READ hasBands NOTIFY bandsChanged)
    Q_PROPERTY(int freeBand READ freeBand NOTIFY bandsChanged)  // the first unused (-1: all 24 are)
    Q_PROPERTY(double scale READ scale NOTIFY bandsChanged)    // the gain scale, 0..2
    // px kept free at the top right (the expand and panel buttons over it).
    Q_PROPERTY(qreal rightInset READ rightInset WRITE setRightInset NOTIFY rightInsetChanged)
    // How often the analyzer reads the displays (ms; 0: as the meters update, the device view's way).
    Q_PROPERTY(int displayInterval READ displayInterval WRITE setDisplayInterval NOTIFY displayIntervalChanged)
    Q_PROPERTY(bool ghostShown READ ghostShown NOTIFY hoverChanged)
    Q_PROPERTY(QStringList types READ types CONSTANT)
    Q_PROPERTY(QStringList slopes READ slopes CONSTANT)  // "6 dB/oct"...
    Q_PROPERTY(QStringList places READ places CONSTANT)

public:
    static constexpr int kBands = 24;
    enum Type { Bell, LowShelf, LowCut, HighShelf, HighCut, Notch, BandPass, TiltShelf };
    Q_ENUM(Type)
    static constexpr int kSlopes[9] = {6, 12, 18, 24, 30, 36, 48, 72, 96};  // dB/octave, as the engine lists them
    static constexpr double kFreqMin = 10.0, kFreqMax = 22000.0;
    static constexpr double kGainMax = 30.0;
    static constexpr double kQMin = 0.025, kQMax = 40.0;
    static constexpr int kWidth = 580;  // in the device view
    static constexpr int kPanelWidth = 162;
    static constexpr int kSpacing = 8;
    static constexpr int kPanelInset = 6;  // px between the panel and the device's right edge
    static constexpr double kDotRadius = 6.5, kDotHoverRadius = 8.5;
    static constexpr double kHitRadius = 11.0;
    static constexpr double kCurveHit = 12.0;  // px from the curve where the ghost band shows
    static constexpr double kMargin = 10.0;    // px between ± the range and the graph's top and bottom

    struct Band {
        int index = 0;
        bool on = true;
        int type = Bell;
        double freq = 1000.0;
        double gain = 0.0;  // as set (before the gain scale)
        double q = 1.0;
        int slope = 1;
        int place = 0;

        friend bool operator==(const Band&, const Band&) = default;
    };

    explicit EqGraph(QQuickItem* parent = nullptr);
    ~EqGraph() override;

    // A band's parameter id (bands from 0): "b1_freq".
    static QString param(int band, const QString& name);
    // Each band its own colour, round the colour wheel.
    static QColor bandColor(int band);
    // The type a click adds this fraction of the way across the graph.
    static int typeAt(double fraction);
    Q_INVOKABLE static bool hasGain(int type);   // a bell, a shelf or a tilt
    Q_INVOKABLE static bool hasSlope(int type);  // a cut or a shelf
    static QString formatFreq(double freq);

    int selected() const { return selected_; }
    Q_INVOKABLE void select(int index);
    int hover() const { return hover_; }
    QVariantMap selectedBand() const { return band(selected_); }
    bool hasBands() const;
    int freeBand() const;
    double scale() const { return scale_; }
    qreal rightInset() const { return rightInset_; }
    void setRightInset(qreal inset);
    int displayInterval() const { return displayInterval_; }
    void setDisplayInterval(int ms);
    bool ghostShown() const { return ghost_.has_value(); }
    QStringList types() const;
    QStringList slopes() const;
    QStringList places() const;

    const std::array<std::optional<Band>, kBands>& bands() const { return bands_; }
    // {index, on, type, freq, gain, q, slope, place, color}; empty for none.
    Q_INVOKABLE QVariantMap band(int index) const;
    Q_INVOKABLE QColor colorOf(int index) const { return bandColor(index); }
    Q_INVOKABLE QString freqText(double freq) const { return formatFreq(freq); }

    // Editing: all through setParams.
    Q_INVOKABLE void setParamValue(const QString& paramId, double value, const QString& mergeKey = QString(),
                                   const QString& text = QStringLiteral("Change EQ"));
    Q_INVOKABLE void setBandParam(int index, const QString& name, double value, const QString& mergeKey = QString(),
                                  const QString& text = QStringLiteral("Change EQ Band"));
    Q_INVOKABLE void setType(int index, int type);
    Q_INVOKABLE void toggleBand(int index);
    Q_INVOKABLE void removeBand(int index);
    Q_INVOKABLE void removeAll();
    Q_INVOKABLE void touchBand(int index, const QString& name);
    void set(const sub::app::OrderedMap<QString, double>& values, const QString& mergeKey = QString(),
             const QString& text = QStringLiteral("Change EQ"));

    // Geometry, for the tests too.
    QRectF plot() const;
    LogAxis frequencyAxis() const;
    double xOf(double freq) const;
    double freqAt(double x) const;
    double yOf(double db) const;
    double dbAt(double y) const;
    QPointF dot(const Band& band) const;
    // Where a band's dot is for a level (its gain, or its curve at its frequency).
    QPointF dotAt(const Band& band, double db) const;
    int bandAt(const QPointF& pos) const;  // -1: none
    double curveY(double x) const;
    std::vector<double> columnFreqs() const;
    // Where a click adds a band, and its type.
    std::optional<std::pair<QPointF, int>> ghost() const { return ghost_; }
    sub::app::analysis::EqAnalyzer& analyzer() { return analyzer_; }
    // The analyzer takes what the engine played since, and the graph shows it.
    Q_INVOKABLE void feed();
    // The analyzer's columns again (after feeding it by hand).
    void refreshAnalyzer();

Q_SIGNALS:
    void selectedChanged();
    void hoverChanged();
    void bandsChanged();
    void rightInsetChanged();
    void displayIntervalChanged();
    void bandMenuRequested(int band, QPointF position);
    void viewMenuRequested(QPointF position);

protected:
    void sync() override;
    void refreshDisplays() override;
    void paint(SgPainter& painter) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    bool event(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct Drag {
        int band = 0;
        QPointF origin;
        double freq = 0.0, gain = 0.0, q = 0.0;
        QString gesture;
        sub::app::OrderedMap<QString, double> base;  // a band added on the press: all its parameters, each move
    };
    struct Curve {
        std::vector<double> db;  // per column
        double dotDb = 0.0;      // at its frequency (a band without gain's dot sits on its curve)
        // What it was worked out for.
        std::optional<Band> band;
        double gain = 0.0;
        double sampleRate = 0.0;
        double lastFreq = 0.0;
    };

    double half() const;
    QString overlayAt(const QPointF& pos) const;
    QRectF overlayRect(bool analyzer) const;
    std::vector<double> response(const Band& band, const std::vector<double>& freqs, double gain) const;
    void refreshCurves();
    void hoverAt(const QPointF& pos);
    void add(const QPointF& pos, int type);
    void startDrag(int index, const QPointF& pos, const QString& gesture = QString(),
                   const sub::app::OrderedMap<QString, double>& base = {});
    void dragTo(const QPointF& pos, Qt::KeyboardModifiers modifiers);
    void wheelSet(int index, const sub::app::OrderedMap<QString, double>& changes, const QString& gesture,
                  const QString& text);
    double targetRadius(int index) const;
    void animateSoon();
    void animate();
    void setHover(int hover, const std::optional<std::pair<QPointF, int>>& ghost);

    std::array<std::optional<Band>, kBands> bands_;
    double scale_ = 1.0;
    int selected_ = -1;
    int hover_ = -1;
    std::optional<std::pair<QPointF, int>> ghost_;
    std::pair<QPointF, int> lastGhost_{QPointF(), Bell};
    bool hasLastGhost_ = false;
    std::optional<Drag> drag_;
    bool addedOnPress_ = false;
    QString wheelGesture_;
    QElapsedTimer wheelClock_;
    std::array<double, kBands> radius_{};  // each dot's radius, easing to its target
    double ghostAlpha_ = 0.0;
    QTimer animation_;
    qreal rightInset_ = 0.0;
    int displayInterval_ = 0;
    QTimer displayTimer_;
    sub::app::analysis::EqAnalyzer analyzer_;

    // What paint() draws, worked out on the GUI thread.
    std::vector<double> freqs_;  // the columns' frequencies
    std::array<Curve, kBands> curves_;
    std::vector<double> total_;
    std::vector<double> inputColumns_, outputColumns_;
    bool inputLive_ = false, outputLive_ = false;
};

}  // namespace sub::ui
