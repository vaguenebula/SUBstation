#include "audio/DisperserResponse.h"

#include "builtin/DisperserDesign.h"

namespace sub::app {

QList<double> disperserGroupDelayMs(int stages, double freq, double pinch, double sampleRate,
                                    const QList<double>& frequencies) {
    QList<double> delays;
    delays.reserve(frequencies.size());
    for (const double frequency : frequencies) {
        delays.append(sub::disperser::groupDelayMs(stages, freq, pinch, sampleRate, frequency));
    }
    return delays;
}

double disperserTunedFrequency(double freq, double sampleRate) {
    return sub::disperser::stageFrequency(freq, sampleRate);
}

double disperserPeakFrequency(double freq, double pinch, double sampleRate) {
    if (!(sampleRate > 0.0)) return 0.0;
    const sub::disperser::Stage stage = sub::disperser::design(freq, pinch, sampleRate);
    return sub::disperser::peakAngle(stage) * sampleRate / (2.0 * sub::disperser::kPi);
}

}  // namespace sub::app
