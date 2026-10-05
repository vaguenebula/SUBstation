#include "devices/EqView.h"

#include <QCoreApplication>
#include <QJSEngine>
#include <QQmlEngine>

namespace sub::ui {

EqView::EqView(QObject* parent) : QObject(parent) {}

EqView* EqView::instance() {
    static EqView* view = new EqView(QCoreApplication::instance());
    return view;
}

EqView* EqView::create(QQmlEngine*, QJSEngine*) {
    EqView* view = instance();
    QJSEngine::setObjectOwnership(view, QJSEngine::CppOwnership);
    return view;
}

void EqView::setRange(double range) {
    if (range == range_)
        return;
    range_ = range;
    Q_EMIT changed();
}

void EqView::setAnalyzer(int mode) {
    mode = ((mode % 4) + 4) % 4;
    if (mode == analyzer_)
        return;
    analyzer_ = mode;
    Q_EMIT changed();
}

void EqView::setPanel(bool shown) {
    if (shown == panel_)
        return;
    panel_ = shown;
    Q_EMIT changed();
}

QList<qreal> EqView::ranges() const { return {kRanges[0], kRanges[1], kRanges[2], kRanges[3]}; }

QStringList EqView::analyzerModes() const {
    return {QStringLiteral("Analyzer Off"), QStringLiteral("Pre"), QStringLiteral("Post"), QStringLiteral("Pre + Post")};
}

void EqView::nextRange() {
    const QList<qreal> all = ranges();
    const qsizetype index = all.indexOf(range_);
    setRange(all[(index + 1) % all.size()]);
}

void EqView::nextAnalyzer() { setAnalyzer(analyzer_ + 1); }

}  // namespace sub::ui
