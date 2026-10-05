#pragma once

// The takes being recorded, over the track lanes (the old LanesCanvas's
// _draw_live_take): each a clip that grows, red-titled, from where it started
// to the playhead (what has come in lags the playhead by the input's latency
// and arrives a buffer at a time, so the take follows the playhead, smoothly),
// its waveform from the peaks the engine sends (the file isn't read until the
// take is done), or a MIDI take's notes so far (a held note reaches the take's
// end). An item of its own, over the lanes: it changes every frame while
// recording.

#include "arrangement/ArrangementItem.h"
#include "audio/LiveTake.h"

#include <QMap>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <optional>

namespace sub::ui {

class LiveTakes : public ArrangementItem {
    Q_OBJECT
    QML_ELEMENT

public:
    explicit LiveTakes(QQuickItem* parent = nullptr);

    // The takes drawn instead of the bridge's (the tests'; none: the bridge's).
    void setTakes(std::optional<QMap<QString, app::LiveTake>> takes);

protected:
    void paint(SgPainter& painter) override;
    void connectSession(app::Session* session) override;
    void connectArrangement(Arrangement* arrangement) override;

private:
    const QMap<QString, app::LiveTake>& takes() const;
    void drawTake(SgPainter& p, const QColor& trackColor, const app::LiveTake& take, double rowTop, int rowHeight,
                  const QRectF& visible) const;
    void drawNotes(SgPainter& p, const app::LiveTake& take, double takeEnd, const QRectF& area,
                   const QRectF& visible) const;

    std::optional<QMap<QString, app::LiveTake>> takes_;
};

}  // namespace sub::ui
