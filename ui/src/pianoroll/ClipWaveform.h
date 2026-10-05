#pragma once

// The clip view's waveforms (clip_view.py's ClipWaveform): each open audio
// clip's whole source file fitted to the width, in its track's colour and
// scaled by its gain, the part the clip plays tinted and the rest dimmed,
// with S and E flags at its start and end, as in Ableton's sample editor. One
// clip gets a time ruler (steps at least 70 px apart); several are stacked in
// bands of at least 40 px, labelled, with "+N more" when they don't fit.

#include "audio/Waveform.h"
#include "model/Clip.h"
#include "pianoroll/ClipViewController.h"
#include "sg/SgCanvas.h"
#include "session/Session.h"

#include <QColor>
#include <QPointer>
#include <QRectF>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <vector>

namespace sub::ui {

class ClipWaveform : public SgCanvas {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(sub::app::Session* session READ session WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(sub::ui::ClipViewController* controller READ controller WRITE setController NOTIFY controllerChanged)

public:
    static constexpr int kRulerHeight = 20;
    static constexpr int kMinBandHeight = 40;  // each clip's waveform when several are open

    explicit ClipWaveform(QQuickItem* parent = nullptr);

    app::Session* session() const { return session_; }
    void setSession(app::Session* session);
    ClipViewController* controller() const { return controller_; }
    void setController(ClipViewController* controller);

    // A time on the ruler: seconds (minutes:seconds from a minute), with as
    // many decimals as the step needs.
    static QString formatTime(double seconds, double step);

Q_SIGNALS:
    void sessionChanged();
    void controllerChanged();

protected:
    void paint(SgPainter& painter) override;

private:
    // A clip as it is drawn: its track's colour, its source (null while it
    // loads) and why it couldn't be loaded. Worked out on the GUI thread.
    struct Band {
        app::Clip clip;
        QColor color;
        app::Waveform source;
        QString loadError;
    };

    void refreshBands();
    // One clip's source in `area`; its length in seconds (0: not loaded).
    double drawBand(SgPainter& painter, const Band& band, const QRectF& area) const;
    void drawLabel(SgPainter& painter, const QRectF& band, const QString& name) const;
    void drawRuler(SgPainter& painter, double totalSec) const;

    QPointer<app::Session> session_;
    QPointer<ClipViewController> controller_;
    std::vector<Band> bands_;
};

}  // namespace sub::ui
