#include "audio/AmpResponse.h"

#include "builtin/AmpDesign.h"

namespace sub::app {

QList<double> ampToneResponseDb(int model, double bass, double middle, double treble, double presence,
                                double sampleRate, const QList<double>& frequencies) {
    const sub::amp::Voicing& voicing = sub::amp::voicing(model);
    QList<double> response;
    response.reserve(frequencies.size());
    for (const double frequency : frequencies)
        response.append(sub::amp::toneResponseDb(voicing, bass, middle, treble, presence, sampleRate, frequency));
    return response;
}

QList<double> ampTransfer(int model, double gain, double bass, double middle, double treble, double presence,
                          double volume, double sagDb, double sampleRate, const QList<double>& xs) {
    // The linear parts' gains are worked out once for the whole curve.
    const sub::amp::Transfer transfer(sub::amp::voicing(model), gain, bass, middle, treble, presence, volume, sagDb,
                                      sampleRate);
    QList<double> ys;
    ys.reserve(xs.size());
    for (const double x : xs)
        ys.append(transfer(x));
    return ys;
}

}  // namespace sub::app
