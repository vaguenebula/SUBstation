#include "Metronome.h"

#include <algorithm>
#include <cmath>

namespace sub {
namespace {

std::vector<float> makeClick(double sampleRate, double frequency, float amplitude) {
    const size_t length = static_cast<size_t>(sampleRate * 0.035);
    const double attack = sampleRate * 0.001;
    std::vector<float> click(length);
    for (size_t i = 0; i < length; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const double env = std::min(1.0, i / attack) * std::exp(-t * 120.0);
        click[i] = static_cast<float>(amplitude * env * std::sin(2.0 * M_PI * frequency * t));
    }
    return click;
}

}  // namespace

void Metronome::prepare(double sampleRate) {
    accentClick_ = makeClick(sampleRate, 1568.0, 0.6f);
    normalClick_ = makeClick(sampleRate, 1046.5, 0.45f);
    voice_ = nullptr;
    voicePos_ = 0;
}

void Metronome::renderUntil(float* left, float* right, int from, int to) {
    if (!voice_) return;
    const std::vector<float>& click = *voice_;
    for (int i = from; i < to && voicePos_ < click.size(); ++i, ++voicePos_) {
        left[i] += click[voicePos_];
        right[i] += click[voicePos_];
    }
    if (voicePos_ >= click.size()) voice_ = nullptr;
}

void Metronome::start(bool accent) {
    voice_ = accent ? &accentClick_ : &normalClick_;
    voicePos_ = 0;
}

}  // namespace sub
