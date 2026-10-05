#pragma once
// Preferences › MIDI, without its widgets: the MIDI inputs connected, each on
// (tracks can hear and record it) or off. Inputs plugged in later show up with
// Refresh (and on the next start). MIDI tracks hear the inputs that are on (all
// of them, or the one they choose).

#include <QObject>
#include <QString>
#include <QVariantList>

namespace sub::app {

class EngineBridge;

class MidiPreferences : public QObject {
    Q_OBJECT
    // [{name, enabled, error}]: `error` ("": none) why it couldn't be opened (shown in red, as its tooltip).
    Q_PROPERTY(QVariantList inputs READ inputs NOTIFY changed)
    // "2 MIDI inputs", "No MIDI input is connected.", "1 could not be opened (see their tooltips)."
    Q_PROPERTY(QString status READ status NOTIFY changed)

public:
    explicit MidiPreferences(EngineBridge* bridge, QObject* parent = nullptr);

    QVariantList inputs() const { return inputs_; }
    QString status() const { return status_; }

    // Lists the inputs (the dialog shows).
    Q_INVOKABLE void open() { show(); }
    // Refresh: looks for MIDI inputs plugged in or taken out since.
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void setInputEnabled(const QString& name, bool enabled);

Q_SIGNALS:
    void changed();

private:
    void show();

    EngineBridge* bridge_;
    QVariantList inputs_;
    QString status_;
};

}  // namespace sub::app
