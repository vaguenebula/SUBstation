#include "audio/SpectralResponse.h"

#include "builtin/SpectralDesign.h"

namespace sub::app {

QList<double> spectralThresholdDb(double threshold, double tilt, const QList<double>& frequencies) {
    QList<double> levels;
    levels.reserve(frequencies.size());
    for (const double frequency : frequencies) levels.append(sub::spectral::thresholdDb(threshold, tilt, frequency));
    return levels;
}

QList<double> spectralBelowDb(double threshold, double below, double tilt, const QList<double>& frequencies) {
    QList<double> levels;
    levels.reserve(frequencies.size());
    for (const double frequency : frequencies)
        levels.append(sub::spectral::belowDb(threshold, below, tilt, frequency));
    return levels;
}

double spectralGainDb(double levelDb, double thresholdDb, double belowDb, double ratio, double upward, double knee,
                      double range) {
    return sub::spectral::gainDb(levelDb, thresholdDb, belowDb, sub::spectral::downSlope(ratio),
                                 sub::spectral::upSlope(upward), knee, range);
}

QList<double> spectralFocusWeights(double low, double high, const QList<double>& frequencies) {
    QList<double> weights;
    weights.reserve(frequencies.size());
    for (const double frequency : frequencies) weights.append(sub::spectral::focusWeight(low, high, frequency));
    return weights;
}

QList<double> spectralDisplayFrequencies() {
    QList<double> frequencies;
    frequencies.reserve(sub::spectral::kDisplayPoints);
    for (int j = 0; j < sub::spectral::kDisplayPoints; ++j) frequencies.append(sub::spectral::displayFrequency(j));
    return frequencies;
}

double spectralPinkDb(double frequency) { return sub::spectral::pinkDb(frequency); }

int spectralLatencySamples(double sampleRate) { return sub::spectral::frameSize(sampleRate); }

}  // namespace sub::app
