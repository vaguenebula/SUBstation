#include "model/ParamSpec.h"

#include "model/Automation.h"
#include "model/Device.h"
#include "model/Notes.h"
#include "model/Numbers.h"
#include "model/Timebase.h"

#include "Processor.h"

#include <algorithm>
#include <cmath>

namespace sub::app {

QString formatValue(double value, const QString& unit) {
    if (unit == u"dB") return formatFixed(value, 1) + QStringLiteral(" dB");
    if (unit == u"%") return formatFixed(value, 0) + QStringLiteral(" %");
    if (unit.isEmpty()) return value != 0.0 ? formatFixed(value, 2, true) : QStringLiteral("0.00");
    if (unit == u":1") return formatFixed(value, 1) + QStringLiteral(":1");  // a ratio
    if (unit == u"note") return notes::noteName(static_cast<int>(roundHalfEven(value)));  // a MIDI key
    if (unit == u"stages") {  // a count (the Disperser's)
        const auto whole = static_cast<long long>(roundHalfEven(value));
        return QString::number(whole) + (whole == 1 ? QStringLiteral(" stage") : QStringLiteral(" stages"));
    }
    if (unit == u"st" || unit == u"ct") {  // semitones, cents
        const auto whole = static_cast<long long>(roundHalfEven(value));
        if (whole == 0) return QStringLiteral("0 ") + unit;
        return (whole > 0 ? QStringLiteral("+") : QString()) + QString::number(whole) + u' ' + unit;
    }
    if (unit == u"Hz") {
        if (value >= 1000) return formatFixed(value / 1000, 2) + QStringLiteral(" kHz");
        return value < 10 ? formatFixed(value, 2) + QStringLiteral(" Hz") : formatFixed(value, 0) + QStringLiteral(" Hz");
    }
    if (unit == u"ms") {
        if (value >= 1000) return formatFixed(value / 1000, 2) + QStringLiteral(" s");
        return value < 10 ? formatFixed(value, 1) + QStringLiteral(" ms") : formatFixed(value, 0) + QStringLiteral(" ms");
    }
    return formatFixed(value, 2) + u' ' + unit;
}

ParamSpec ParamSpec::fromInfo(const sub::ParamInfo& info, const QString& key, const QString& group,
                              std::function<QString(double)> text) {
    ParamSpec spec;
    spec.key = key;
    spec.name = QString::fromStdString(info.name);
    spec.group = group;
    spec.minimum = info.minValue;
    spec.maximum = info.maxValue;
    spec.defaultValue = info.defaultValue;
    spec.unit = QString::fromStdString(info.unit);
    spec.scale = info.isLog() ? Scale::Log : Scale::Linear;
    spec.steps = info.stepCount();
    for (const std::string& label : info.valueLabels) spec.labels.append(QString::fromStdString(label));
    spec.text = std::move(text);
    return spec;
}

double ParamSpec::toNormalized(double plain) const {
    if (scale == Scale::Fader) return automation::volumeToNormalized(plain);
    const double span = maximum - minimum;
    if (!(span > 0)) return 0.0;
    if (steps > 0) {
        const auto last = static_cast<double>(steps);
        return std::clamp(std::round(plain - minimum), 0.0, last) / last;
    }
    plain = std::clamp(plain, minimum, maximum);
    if (scale == Scale::Log) return std::log(plain / minimum) / std::log(maximum / minimum);
    return (plain - minimum) / span;
}

double ParamSpec::fromNormalized(double value) const {
    value = std::clamp(value, 0.0, 1.0);
    if (scale == Scale::Fader) return automation::normalizedToVolume(value);
    if (steps > 0) {
        const auto last = static_cast<double>(steps);
        return minimum + std::min(last, std::floor(value * (last + 1.0)));
    }
    if (scale == Scale::Log) return minimum * std::pow(maximum / minimum, value);
    return minimum + value * (maximum - minimum);
}

double ParamSpec::quantize(double value) const { return steps > 0 ? toNormalized(fromNormalized(value)) : value; }

QString ParamSpec::format(double plain) const {
    if (text) return text(plain);
    if (!labels.isEmpty()) {
        const auto index = static_cast<long long>(roundHalfEven(plain - minimum));
        return labels.at(static_cast<qsizetype>(std::max(0LL, std::min<long long>(labels.size() - 1, index))));
    }
    return formatValue(plain, unit);
}

QString ParamSpec::formatNormalized(double value) const { return format(fromNormalized(value)); }

bool ParamSpec::operator==(const ParamSpec& other) const {
    return key == other.key && name == other.name && group == other.group && minimum == other.minimum &&
           maximum == other.maximum && defaultValue == other.defaultValue && unit == other.unit &&
           scale == other.scale && steps == other.steps && labels == other.labels;
}

std::vector<ParamSpec> mixerSpecs(bool master, const std::vector<std::pair<QString, QString>>& sends) {
    const QString who = master ? QStringLiteral("Master") : QStringLiteral("Track");
    ParamSpec volume;
    volume.key = automation::kMixerVolume;
    volume.name = who + QStringLiteral(" Volume");
    volume.group = QStringLiteral("Mixer");
    volume.minimum = automation::kMinVolumeDb;
    volume.maximum = automation::kMaxVolumeDb;
    volume.defaultValue = 0.0;
    volume.unit = QStringLiteral("dB");
    volume.scale = ParamSpec::Scale::Fader;
    volume.text = formatDb;
    ParamSpec pan;
    pan.key = automation::kMixerPan;
    pan.name = who + QStringLiteral(" Pan");
    pan.group = QStringLiteral("Mixer");
    pan.minimum = -1.0;
    pan.maximum = 1.0;
    pan.defaultValue = 0.0;
    pan.text = formatPan;
    std::vector<ParamSpec> specs{volume, pan};
    if (!master) specs.push_back(switchSpec(automation::kMixerOn, QStringLiteral("Track Activator"), volume.group));
    for (const auto& [returnId, letter] : sends) specs.push_back(sendSpec(returnId, letter));
    return specs;
}

ParamSpec switchSpec(const QString& key, const QString& name, const QString& group) {
    ParamSpec spec;
    spec.key = key;
    spec.name = name;
    spec.group = group;
    spec.minimum = 0.0;
    spec.maximum = 1.0;
    spec.defaultValue = 1.0;
    spec.steps = 1;
    spec.labels = {QStringLiteral("Off"), QStringLiteral("On")};
    return spec;
}

ParamSpec deviceOnSpec(const QString& deviceId, const QString& group) {
    return switchSpec(automation::deviceOnKey(deviceId), QStringLiteral("Device On"), group);
}

ParamSpec sendSpec(const QString& returnId, const QString& letter) {
    ParamSpec spec;
    spec.key = automation::sendKey(returnId);
    spec.name = QStringLiteral("Send ") + letter;
    spec.group = QStringLiteral("Mixer");
    spec.minimum = automation::kMinVolumeDb;
    spec.maximum = automation::kMaxVolumeDb;
    spec.defaultValue = automation::kMinVolumeDb;
    spec.unit = QStringLiteral("dB");
    spec.scale = ParamSpec::Scale::Fader;
    spec.text = formatDb;
    return spec;
}

std::vector<ParamSpec> macroSpecs(const QString& rackId, const QStringList& names, const QString& group) {
    std::vector<ParamSpec> specs;
    for (qsizetype i = 0; i < names.size(); ++i) {
        ParamSpec macro;
        macro.key = automation::deviceKey(rackId, macroParam(static_cast<int>(i)));
        macro.name = names[i];
        macro.group = group;
        macro.text = [](double value) { return QStringLiteral("%1 %").arg(std::lround(value * 100.0)); };
        specs.push_back(macro);
    }
    return specs;
}

std::vector<ParamSpec> chainSpecs(const QString& rackId, const std::vector<std::pair<QString, QString>>& chains,
                                  const QString& group) {
    std::vector<ParamSpec> specs;
    for (const auto& [chainId, name] : chains) {
        ParamSpec volume;
        volume.key = automation::chainKey(rackId, chainId, automation::kChainVolume);
        volume.name = name + QStringLiteral(" Volume");
        volume.group = group;
        volume.minimum = automation::kMinVolumeDb;
        volume.maximum = automation::kMaxVolumeDb;
        volume.defaultValue = 0.0;
        volume.unit = QStringLiteral("dB");
        volume.scale = ParamSpec::Scale::Fader;
        volume.text = formatDb;
        specs.push_back(volume);
        ParamSpec pan;
        pan.key = automation::chainKey(rackId, chainId, automation::kChainPan);
        pan.name = name + QStringLiteral(" Pan");
        pan.group = group;
        pan.minimum = -1.0;
        pan.maximum = 1.0;
        pan.defaultValue = 0.0;
        pan.text = formatPan;
        specs.push_back(pan);
    }
    return specs;
}

}  // namespace sub::app
