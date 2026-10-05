#include "session/RenderProgress.h"

#include <algorithm>

namespace sub::app {

RenderProgress::RenderProgress(QObject* parent) : QObject(parent) {}

void RenderProgress::cancel() {
    if (!active_ || cancelled_) return;
    cancelled_ = true;
    label_ = QStringLiteral("Cancelling…");
    Q_EMIT changed();
    Q_EMIT cancelRequested();
}

void RenderProgress::begin(const QString& title) {
    title_ = title;
    label_.clear();
    progress_ = 0.0;
    busy_ = false;
    cancelled_ = false;
    Q_EMIT changed();
    if (!active_) {
        active_ = true;
        Q_EMIT activeChanged();
    }
}

void RenderProgress::end() {
    busy_ = false;
    Q_EMIT changed();
    if (active_) {
        active_ = false;
        Q_EMIT activeChanged();
    }
}

void RenderProgress::setLabel(const QString& label) {
    if (cancelled_ || label == label_) return;
    label_ = label;
    Q_EMIT changed();
}

void RenderProgress::setBusy(bool busy) {
    if (busy == busy_) return;
    busy_ = busy;
    Q_EMIT changed();
}

void RenderProgress::setProgress(int index, int count, double fraction) {
    const double progress = std::clamp((index + fraction) / std::max(1, count), 0.0, 1.0);
    if (progress == progress_) return;
    progress_ = progress;
    Q_EMIT changed();
}

}  // namespace sub::app
