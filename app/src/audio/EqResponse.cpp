#include "audio/EqResponse.h"

#include "builtin/EqDesign.h"

namespace sub::app {

QList<double> eqResponseDb(int type, double freq, double gain, double q, int slope, double sampleRate,
                           const QList<double>& frequencies) {
    const sub::eq::Design design = sub::eq::design(type, freq, gain, q, slope, sampleRate);
    QList<double> response;
    response.reserve(frequencies.size());
    for (const double frequency : frequencies) response.append(sub::eq::responseDb(design, frequency, sampleRate));
    return response;
}

}  // namespace sub::app
