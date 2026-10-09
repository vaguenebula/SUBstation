#include "session/MidiPreferences.h"

#include <QSet>
#include <QVariantMap>

#include "audio/AudioSettings.h"
#include "audio/EngineBridge.h"
#include "model/Numbers.h"

namespace sub::app {

MidiPreferences::MidiPreferences(EngineBridge* bridge, QObject* parent) : QObject(parent), bridge_(bridge) {}

void MidiPreferences::refresh() {
    bridge_->openMidiInputs();
    show();
}

void MidiPreferences::setInputEnabled(const QString& name, bool enabled) {
    bridge_->setMidiInputEnabled(name, enabled);
    show();
}

void MidiPreferences::show() {
    const QSet<QString> disabled = disabledMidiInputs();
    const QMap<QString, QString>& errors = bridge_->midiErrors();
    inputs_.clear();
    for (const QString& name : bridge_->midiInputs()) {
        inputs_.append(QVariantMap{{QStringLiteral("name"), name},
                                   {QStringLiteral("enabled"), !disabled.contains(name)},
                                   {QStringLiteral("error"), errors.value(name)}});
    }
    const auto count = inputs_.size();
    if (count == 0) {
        status_ = QStringLiteral("No MIDI input is connected.");
    } else if (!errors.isEmpty()) {
        status_ = QStringLiteral("%1 could not be opened (see their tooltips).").arg(errors.size());
    } else {
        status_ = countText(count, QStringLiteral("MIDI input"), QStringLiteral("MIDI inputs"));
    }
    Q_EMIT changed();
}

}  // namespace sub::app
