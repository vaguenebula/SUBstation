#include "audio/SaturatorResponse.h"

#include "builtin/SaturatorDesign.h"

namespace sub::app {

namespace {

// The engine's shape from the parameters, as the device builds it once they have settled.
sub::saturator::Shape engineShape(const SaturatorShape& s) {
    return sub::saturator::makeShape(s.type, float(s.thresholdDb), float(s.wsDrive), float(s.wsLin), float(s.wsCurve),
                                     float(s.wsDamp), float(s.wsDepth), float(s.wsPeriod));
}

}  // namespace

QList<double> saturatorCurve(const SaturatorShape& shape, const QList<double>& inputs) {
    const sub::saturator::Shape s = engineShape(shape);
    const sub::saturator::Clip clip = sub::saturator::clipAt(shape.clip);
    const float gain = sub::expDbToGain(float(shape.driveDb));
    QList<double> outputs;
    outputs.reserve(inputs.size());
    for (const double input : inputs)
        outputs.append(sub::saturator::transfer(s, clip, gain, float(input)));
    return outputs;
}

double saturatorCurveAt(const SaturatorShape& shape, double input) {
    return sub::saturator::transfer(engineShape(shape), sub::saturator::clipAt(shape.clip),
                                    sub::expDbToGain(float(shape.driveDb)), float(input));
}

double saturatorSlope(const SaturatorShape& shape) {
    constexpr double kAt = 1e-4;
    return saturatorCurveAt(shape, kAt) / kAt;
}

QList<double> saturatorColorDb(double baseDb, double freq, double width, double depthDb, double sampleRate,
                               const QList<double>& frequencies) {
    QList<double> db;
    db.reserve(frequencies.size());
    if (!(sampleRate > 0.0)) {
        db.fill(0.0, frequencies.size());
        return db;
    }
    const sub::saturator::ColorDesign design = sub::saturator::colorDesign(baseDb, freq, width, depthDb, sampleRate);
    for (const double f : frequencies)
        db.append(sub::saturator::colorResponseDb(design, f, sampleRate));
    return db;
}

double saturatorThresholdInput(double thresholdDb, double driveDb) {
    return double(sub::saturator::thresholdGain(float(thresholdDb))) / double(sub::expDbToGain(float(driveDb)));
}

}  // namespace sub::app
