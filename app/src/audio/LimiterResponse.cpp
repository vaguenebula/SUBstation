#include "audio/LimiterResponse.h"

#include "builtin/LimiterDesign.h"

namespace sub::app {

LimiterLine limiterLine(bool maximize, double gainDb, double ceilingDb, double thresholdDb, double outputDb) {
    const sub::limiter::Scales s = sub::limiter::scales(maximize, float(gainDb), float(ceilingDb), float(thresholdDb),
                                                        float(outputDb));
    return {double(s.lineDb), double(s.postDb) - double(s.lineDb)};
}

double limiterSoftKneeDb() { return sub::limiter::softKneeDb(); }

double limiterSoftTopDb() { return sub::limiter::softTopDb(); }

double limiterSoftClipDb(double inDb) { return sub::limiter::softClipDb(float(inDb)); }

int limiterMeterSamples() { return sub::limiter::kMeterSamples; }

double limiterFloorDb() { return double(sub::limiter::kFloorDb); }

}  // namespace sub::app
