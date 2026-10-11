#include "audio/GateResponse.h"

#include "builtin/GateDesign.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sub::app {

namespace {

sub::gate::KeyFilter keyFilterType(int type) {
    return static_cast<sub::gate::KeyFilter>(std::clamp(type, 0, sub::gate::kKeyFilters - 1));
}

}  // namespace

GateRange gateThresholdRange() { return {sub::gate::kThresholdMinDb, sub::gate::kThresholdMaxDb}; }

GateRange gateReturnRange() { return {0.0, sub::gate::kReturnMaxDb}; }

GateRange gateKeyFreqRange() { return {sub::gate::kKeyFreqMin, sub::gate::kKeyFreqMax}; }

GateRange gateKeyQRange() { return {sub::gate::kKeyQMin, sub::gate::kKeyQMax}; }

GateRange gateKeyGainRange() { return {sub::gate::kKeyGainMinDb, sub::gate::kKeyGainMaxDb}; }

QList<double> gateKeyFilterDb(int type, double freq, double q, double gainDb, double sampleRate,
                              const QList<double>& frequencies) {
    QList<double> response;
    response.reserve(frequencies.size());
    if (!(sampleRate > 0.0)) {
        response.fill(0.0, frequencies.size());
        return response;
    }
    // One design for every frequency (keyFilterDb designs it per call).
    const sub::dsp::BiquadCoefficients filter = sub::gate::keyFilter(keyFilterType(type), freq, q, gainDb, sampleRate);
    for (const double at : frequencies)
        response.append(filter.magnitudeDb(std::min(at, 0.5 * sampleRate), sampleRate));
    return response;
}

bool gateKeyFilterUsesGain(int type) { return sub::gate::keyFilterUsesGain(keyFilterType(type)); }

bool gateKeyFilterUsesQ(int type) { return sub::gate::keyFilterUsesQ(keyFilterType(type)); }

bool gateFloorIsSilent(double floorDb) { return sub::gate::floorGain(static_cast<float>(floorDb)) <= 0.f; }

double gateGainDb(double pass, double floorDb) {
    const float gain = sub::gate::gain(static_cast<float>(std::clamp(pass, 0.0, 1.0)),
                                       sub::gate::floorGain(static_cast<float>(floorDb)));
    return gain > 0.f ? 20.0 * std::log10(static_cast<double>(gain)) : -std::numeric_limits<double>::infinity();
}

double gateCloseDb(double thresholdDb, double returnDb) {
    return sub::gate::closeDb(static_cast<float>(thresholdDb), static_cast<float>(returnDb));
}

int gateDisplaySamples() { return sub::gate::kDisplaySamples; }

}  // namespace sub::app
