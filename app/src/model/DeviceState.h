#pragma once
// A built-in device's state besides its parameters (a sampler's sample), as the
// engine stores it (BuiltinProcessor::encodeState): named text values, one per
// line, "name=value", where a backslash escapes a backslash ("\\") or a newline
// ("\n"). The model keeps it, base64-encoded, in Device::state, as it keeps a
// plug-in's state.

#include <QByteArray>
#include <QMap>
#include <QString>

#include <optional>

namespace sub::app::deviceState {

using Values = QMap<QString, QString>;

QByteArray encode(const Values& values);
Values decode(const QByteArray& state);
// Values as Device::state keeps them (none for none).
std::optional<QString> toModel(const Values& values);
// A built-in device's values from its Device::state (none if it has none, or it's unreadable).
Values fromModel(const std::optional<QString>& state);

}  // namespace sub::app::deviceState
