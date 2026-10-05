#pragma once

// What every EQ editor shows alike (not saved): the curve's range (± dB), the
// analyzer's mode, and whether the device view shows the selected band's
// controls beside the curve. One for the application: QML's `EqView` singleton
// and the EqGraph items share it.

#include <QList>
#include <QObject>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

namespace sub::ui {

class EqView : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(double range READ range WRITE setRange NOTIFY changed)
    Q_PROPERTY(int analyzer READ analyzer WRITE setAnalyzer NOTIFY changed)  // an index of analyzerModes
    Q_PROPERTY(bool panel READ panel WRITE setPanel NOTIFY changed)
    Q_PROPERTY(QList<qreal> ranges READ ranges CONSTANT)
    Q_PROPERTY(QStringList analyzerModes READ analyzerModes CONSTANT)

public:
    static constexpr double kRanges[4] = {3.0, 6.0, 12.0, 30.0};  // the curve's scale: ± this many dB

    static EqView* instance();
    // QML's singleton: the one instance.
    static EqView* create(QQmlEngine*, QJSEngine*);

    double range() const { return range_; }
    void setRange(double range);
    int analyzer() const { return analyzer_; }
    void setAnalyzer(int mode);
    bool panel() const { return panel_; }
    void setPanel(bool shown);
    QList<qreal> ranges() const;
    QStringList analyzerModes() const;

    // A click on the graph's labels: the next range, the next analyzer mode.
    Q_INVOKABLE void nextRange();
    Q_INVOKABLE void nextAnalyzer();

Q_SIGNALS:
    void changed();

private:
    explicit EqView(QObject* parent = nullptr);

    double range_ = 12.0;
    int analyzer_ = 3;  // Pre + Post
    bool panel_ = false;
};

}  // namespace sub::ui
