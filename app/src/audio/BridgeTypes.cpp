#include "audio/BridgeTypes.h"

#include "Processor.h"  // ParamInfo: the engine's mapping, which ProcessorParam shares

namespace sub::app {

namespace {

sub::ParamInfo engineInfo(const ProcessorParam& param) {
    sub::ParamInfo info;
    info.minValue = static_cast<float>(param.minValue);
    info.maxValue = static_cast<float>(param.maxValue);
    info.defaultValue = static_cast<float>(param.defaultValue);
    info.logScale = param.logScale;
    for (const QString& label : param.valueLabels) info.valueLabels.push_back(label.toStdString());
    info.steps = param.steps;
    return info;
}

}  // namespace

int ProcessorParam::stepCount() const { return engineInfo(*this).stepCount(); }

double ProcessorParam::toNormalized(double plain) const {
    return engineInfo(*this).toNormalized(static_cast<float>(plain));
}

double ProcessorParam::fromNormalized(double normalized) const {
    return engineInfo(*this).fromNormalized(static_cast<float>(normalized));
}

}  // namespace sub::app
