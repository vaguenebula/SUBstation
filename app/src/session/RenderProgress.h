#pragma once
// A render in the background (exporting, freezing, reversing long clips), as
// the UI shows it: a modal dialog with its title, what it is doing, how far it
// got, and Cancel (Esc, or closing it, too). While the engine renders on a
// thread of its own (RenderTask), the window goes on (it repaints, its meters
// move) but takes no edits: the render is of the project as it was when it
// started. One render at a time.
//
// The session's renders drive it (session/Renders.h); the UI binds to it: it
// shows the dialog while `active`, a busy bar while `busy` (plug-ins or samples
// still loading), else `progress`; Cancel calls cancel(). Cancelled, the label
// says "Cancelling…" and Cancel is disabled until the render has stopped (then
// `active` goes false). Cancel never takes the keyboard focus (Space, meant for
// the transport, doesn't cancel).

#include <QObject>
#include <QString>

namespace sub::app {

class RenderProgress : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString label READ label NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)  // 0..1
    Q_PROPERTY(bool busy READ busy NOTIFY changed)            // how far is unknown (a busy bar)
    Q_PROPERTY(bool cancelled READ cancelled NOTIFY changed)  // Cancel pressed: it is stopping

public:
    explicit RenderProgress(QObject* parent = nullptr);

    bool active() const { return active_; }
    QString title() const { return title_; }
    QString label() const { return label_; }
    double progress() const { return progress_; }
    bool busy() const { return busy_; }
    bool cancelled() const { return cancelled_; }

    // Cancel (the button, Esc, closing the dialog): the render stops, and the
    // dialog goes when it has.
    Q_INVOKABLE void cancel();

    // --- For the renders ---
    // A render starts: the dialog shows (not cancelled, at 0).
    void begin(const QString& title);
    // It ended: the dialog goes.
    void end();
    // What it does now (not shown once cancelled: "Cancelling…" stays).
    void setLabel(const QString& label);
    void setBusy(bool busy);
    // Part `index` of `count` (each a task followed in turn) is `fraction` done.
    void setProgress(int index, int count, double fraction);

Q_SIGNALS:
    void activeChanged();
    void changed();
    void cancelRequested();  // Cancel was pressed: the render cancels what it follows

private:
    bool active_ = false;
    QString title_;
    QString label_;
    double progress_ = 0.0;
    bool busy_ = false;
    bool cancelled_ = false;
};

}  // namespace sub::app
