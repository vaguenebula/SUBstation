#include "audio/ChorusVoices.h"

#include "builtin/ChorusDesign.h"

#include <algorithm>

namespace sub::app {

static_assert(kChorusMinRate == sub::chorus::kMinRate && kChorusMaxRate == sub::chorus::kMaxRate);

namespace {

sub::chorus::Layout engineLayout(const ChorusLayout& layout) {
    return sub::chorus::layout(layout.mode, layout.taps, layout.time);
}

double fraction(double percent) { return std::clamp(percent, 0.0, 100.0) / 100.0; }

}  // namespace

bool operator==(const ChorusLayout& a, const ChorusLayout& b) { return engineLayout(a) == engineLayout(b); }

int chorusVoices(const ChorusLayout& layout) { return sub::chorus::voices(engineLayout(layout)); }

double chorusCentreMs(const ChorusLayout& layout, double amountPercent) {
    return sub::chorus::centreMs(engineLayout(layout), fraction(amountPercent));
}

double chorusSwingMs(const ChorusLayout& layout, double amountPercent) {
    return sub::chorus::swingMs(engineLayout(layout), fraction(amountPercent));
}

double chorusLowestMs(const ChorusLayout& layout) { return sub::chorus::lowestMs(engineLayout(layout)); }

double chorusHighestMs(const ChorusLayout& layout) { return sub::chorus::highestMs(engineLayout(layout)); }

double chorusVoicePhase(const ChorusLayout& layout, int channel, int voice, double offsetDegrees) {
    return sub::chorus::voicePhase(engineLayout(layout), channel, voice, std::clamp(offsetDegrees, 0.0, 180.0) / 360.0);
}

double chorusDelayMs(const ChorusLayout& layout, double amountPercent, double shapePercent, double phase) {
    const sub::chorus::Layout l = engineLayout(layout);
    const double lfo = sub::chorus::lfoValue(l.mode, phase, fraction(shapePercent));
    return sub::chorus::delayMs(l, fraction(amountPercent), lfo);
}

double chorusPeakDetuneCents(const ChorusLayout& layout, double rateHz, double amountPercent, double shapePercent) {
    return sub::chorus::peakDetuneCents(engineLayout(layout), rateHz, fraction(amountPercent), fraction(shapePercent));
}

}  // namespace sub::app
