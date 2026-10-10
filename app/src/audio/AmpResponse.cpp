#include "audio/AmpResponse.h"

#include "builtin/AmpDesign.h"

#include <QMap>

#include <cmath>
#include <string>
#include <vector>

namespace sub::app {

QStringList ampModelNames() {
    QStringList names;
    for (const std::string& label : sub::amp::modelLabels()) names.append(QString::fromStdString(label));
    return names;
}

int ampModelCount() { return sub::amp::kModels; }

double ampDisplayFloorDb() { return sub::amp::kDisplayFloorDb; }

QList<double> ampToneResponseDb(int model, double bass, double middle, double treble, double presence,
                                double sampleRate, const QList<double>& frequencies) {
    const sub::amp::Voicing& voicing = sub::amp::voicing(model);
    QList<double> response;
    response.reserve(frequencies.size());
    for (const double frequency : frequencies)
        response.append(sub::amp::toneResponseDb(voicing, bass, middle, treble, presence, sampleRate, frequency));
    return response;
}

struct AmpTransferCurve::Parts {
    Parts(int model, double gain, double bass, double middle, double treble, double presence, double volume,
          double sampleRate)
        : transfer(sub::amp::voicing(model), gain, bass, middle, treble, presence, volume, sampleRate) {}

    sub::amp::Transfer transfer;
    std::vector<sub::amp::Transfer::Wave> waves;  // the power stage's input for each tone's peak (|x|)
    std::vector<int> wave;                        // each x's
    std::vector<bool> low;                        // whether it takes the tone's lowest value (x < 0)
};

AmpTransferCurve::AmpTransferCurve() = default;
AmpTransferCurve::~AmpTransferCurve() = default;

void AmpTransferCurve::prepare(int model, double gain, double bass, double middle, double treble, double presence,
                               double volume, double sampleRate, const QList<double>& xs) {
    parts_ = std::make_unique<Parts>(model, gain, bass, middle, treble, presence, volume, sampleRate);
    // A tone's peak gives both of its ends (x and -x): its preamp's part once.
    QMap<double, int> amplitudes;
    parts_->wave.reserve(size_t(xs.size()));
    parts_->low.reserve(size_t(xs.size()));
    for (const double x : xs) {
        const double a = std::abs(x);
        auto found = amplitudes.constFind(a);
        if (found == amplitudes.constEnd()) {
            found = amplitudes.insert(a, int(parts_->waves.size()));
            parts_->waves.push_back(parts_->transfer.preamp(a));
        }
        parts_->wave.push_back(found.value());
        parts_->low.push_back(x < 0.0);
    }
}

QList<double> AmpTransferCurve::at(double sagDb) const {
    QList<double> ys;
    if (!parts_) return ys;
    std::vector<sub::amp::Transfer::Peaks> peaks;
    peaks.reserve(parts_->waves.size());
    for (const sub::amp::Transfer::Wave& w : parts_->waves)
        peaks.push_back(parts_->transfer.peaksOf(parts_->transfer.power(w, sagDb)));
    ys.reserve(qsizetype(parts_->wave.size()));
    for (size_t i = 0; i < parts_->wave.size(); ++i) {
        const sub::amp::Transfer::Peaks& p = peaks[size_t(parts_->wave[i])];
        ys.append(parts_->low[i] ? p.low : p.high);
    }
    return ys;
}

double AmpTransferCurve::smallSignalGain(double sagDb) const {
    return parts_ ? parts_->transfer.smallSignalGain(sagDb) : 0.0;
}

QList<double> ampTransfer(int model, double gain, double bass, double middle, double treble, double presence,
                          double volume, double sagDb, double sampleRate, const QList<double>& xs) {
    AmpTransferCurve curve;
    curve.prepare(model, gain, bass, middle, treble, presence, volume, sampleRate, xs);
    return curve.at(sagDb);
}

}  // namespace sub::app
