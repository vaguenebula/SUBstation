// Built-in "EQ" device, after FabFilter's Pro-Q: up to 24 bands, each a bell,
// a low or high shelf, a low or high cut, a notch, a band pass or a tilt shelf
// (see builtin/EqDesign.h), on both channels or only the left, right, mid or
// side; then an output gain. A gain scale turns every band's gain up or down
// together.
//
// A band is there (`used`) or not; one that is there can be switched off
// (`on`). Its frequency, gain and Q glide (about 15 ms) so dragging it is
// smooth; its sections are designed again as they do.
//
// Its editor draws spectra of the input and the output behind the curve: the
// displays "input" and "output" are each summed to mono, one value per sample.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "builtin/BuiltinProcessor.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/EqDesign.h"
#include "rt/RtUtils.h"

namespace sub {
namespace {

constexpr int kBands = 24;
constexpr int kChunk = 32;  // samples between coefficient updates while a band glides

class EqProcessor final : public BuiltinProcessor {
public:
    // Per band: kBandParams parameters from bandParam(band, which); then the global ones.
    enum BandParam { Used = 0, On, Type, Freq, Gain, Q, Slope, Place, kBandParams };
    enum Placement { Stereo = 0, Left, Right, Mid, Side };
    static constexpr int kOutput = kBands * kBandParams;
    static constexpr int kScale = kOutput + 1;

    enum Display { InputDisplay = 0, OutputDisplay };

    EqProcessor() : BuiltinProcessor(infos(), {{"input", 1}, {"output", 1}}) {}

    std::string typeId() const override { return "builtin:eq"; }
    std::string name() const override { return "EQ"; }

    // A narrow band rings for about Q / (pi f) seconds; until it is 60 dB down.
    int tailSamples() const override {
        double longest = 0.0;
        for (int b = 0; b < kBands; ++b) {
            if (!active(b)) continue;
            longest = std::max(longest, 7.0 * param(bandParam(b, Q)) / (eq::kPi * param(bandParam(b, Freq))));
        }
        return static_cast<int>(std::min(5.0, longest) * sampleRate_);
    }

    void prepare(double sampleRate, int) override {
        sampleRate_ = sampleRate;
        glide_ = 1.0 - std::exp(-kChunk / (0.015 * sampleRate));
        output_.reset(sampleRate, 0.02);
        reset();
    }

    void reset() override {
        for (Band& band : bands_) band = Band{};
        output_.snapTo(dbToGain(param(kOutput)));
    }

protected:
    void render(const ProcessContext&, float* const* ch, int numChannels, int numFrames) override {
        const int n = std::min(numChannels, 2);
        if (n <= 0) return;
        output_.setTarget(dbToGain(param(kOutput)));
        const double scale = param(kScale) / 100.0;

        for (int start = 0; start < numFrames; start += kChunk) {
            const int length = std::min(kChunk, numFrames - start);
            float* left = ch[0] + start;
            float* right = n > 1 ? ch[1] + start : nullptr;
            for (int i = 0; i < length; ++i) publish(InputDisplay, right ? 0.5f * (left[i] + right[i]) : left[i]);

            for (int b = 0; b < kBands; ++b) {
                Band& band = bands_[b];
                if (!active(b)) {
                    band.wasActive = false;
                    continue;
                }
                update(b, scale);
                const auto place = static_cast<Placement>(std::lround(param(bandParam(b, Place))));
                if (!right) {  // one channel: everything but the side
                    if (place != Side) filter(band, 0, left, length);
                    continue;
                }
                switch (place) {
                    case Stereo:
                        filter(band, 0, left, length);
                        filter(band, 1, right, length);
                        break;
                    case Left: filter(band, 0, left, length); break;
                    case Right: filter(band, 1, right, length); break;
                    case Mid:
                    case Side: {
                        for (int i = 0; i < length; ++i) {  // to mid and side, and back after
                            const float l = left[i], r = right[i];
                            left[i] = 0.5f * (l + r);
                            right[i] = 0.5f * (l - r);
                        }
                        filter(band, place == Mid ? 0 : 1, place == Mid ? left : right, length);
                        for (int i = 0; i < length; ++i) {
                            const float m = left[i], s = right[i];
                            left[i] = m + s;
                            right[i] = m - s;
                        }
                        break;
                    }
                }
            }

            for (int i = 0; i < length; ++i) {
                const float gain = output_.next();
                left[i] *= gain;
                if (right) right[i] *= gain;
                publish(OutputDisplay, right ? 0.5f * (left[i] + right[i]) : left[i]);
            }
        }
    }

private:
    struct Band {
        eq::Design design;
        double state[eq::kMaxSections][2][2] = {};  // per section, per channel: the TDF-II's two
        double freq = 0.0, gain = 0.0, q = 0.0;      // as they glide (freq and Q in log2)
        int type = -1, slope = -1;
        bool wasActive = false;
    };

    static int bandParam(int band, int which) noexcept { return band * kBandParams + which; }
    static float dbToGain(float db) noexcept { return std::pow(10.f, db / 20.f); }

    bool active(int b) const noexcept {
        return param(bandParam(b, Used)) >= 0.5f && param(bandParam(b, On)) >= 0.5f;
    }

    // Glides the band towards its parameters, and designs it again if it moved.
    void update(int b, double scale) noexcept {
        Band& band = bands_[b];
        const int type = static_cast<int>(std::lround(param(bandParam(b, Type))));
        const int slope = static_cast<int>(std::lround(param(bandParam(b, Slope))));
        const double freq = std::log2(std::max(1.f, param(bandParam(b, Freq))));
        const double gain = param(bandParam(b, Gain)) * scale;
        const double q = std::log2(std::max(0.01f, param(bandParam(b, Q))));
        bool redesign = type != band.type || slope != band.slope;
        if (!band.wasActive) {  // (back) on: from where it is now, from silence
            band.freq = freq;
            band.gain = gain;
            band.q = q;
            for (auto& section : band.state) for (auto& channel : section) channel[0] = channel[1] = 0.0;
            band.wasActive = true;
            redesign = true;
        }
        const auto glide = [&](double& value, double target, double close) {
            if (value == target) return;
            value += glide_ * (target - value);
            if (std::abs(target - value) < close) value = target;
            redesign = true;
        };
        glide(band.freq, freq, 1e-4);
        glide(band.gain, gain, 1e-3);
        glide(band.q, q, 1e-4);
        band.type = type;
        band.slope = slope;
        if (redesign) {
            const int count = band.design.count;
            band.design = eq::design(type, std::exp2(band.freq), band.gain, std::exp2(band.q), slope, sampleRate_);
            for (int s = count; s < band.design.count; ++s) {  // sections it didn't have start silent
                for (auto& channel : band.state[s]) channel[0] = channel[1] = 0.0;
            }
        }
    }

    // Channel c's samples through the band's sections (transposed direct form II, in double).
    static void filter(Band& band, int c, float* x, int length) noexcept {
        for (int s = 0; s < band.design.count; ++s) {
            const eq::Biquad& q = band.design.sections[s];
            double z1 = band.state[s][c][0], z2 = band.state[s][c][1];
            for (int i = 0; i < length; ++i) {
                const double in = x[i];
                const double out = q.b0 * in + z1;
                z1 = q.b1 * in - q.a1 * out + z2;
                z2 = q.b2 * in - q.a2 * out;
                x[i] = static_cast<float>(out);
            }
            // (flushed: no denormals ringing out for ever)
            band.state[s][c][0] = std::abs(z1) < 1e-20 ? 0.0 : z1;
            band.state[s][c][1] = std::abs(z2) < 1e-20 ? 0.0 : z2;
        }
    }

    static const std::vector<ParamInfo>& infos() {
        static const std::vector<ParamInfo> kInfos = [] {
            static const std::vector<std::string> kOnOff = {"Off", "On"};
            static const std::vector<std::string> kTypes = {"Bell", "Low Shelf", "Low Cut", "High Shelf",
                                                            "High Cut", "Notch", "Band Pass", "Tilt Shelf"};
            std::vector<std::string> slopes;
            for (const int s : eq::kSlopes) slopes.push_back(std::to_string(s) + " dB/oct");
            static const std::vector<std::string> kPlaces = {"Stereo", "Left", "Right", "Mid", "Side"};
            std::vector<ParamInfo> list;
            for (int b = 0; b < kBands; ++b) {
                const std::string id = "b" + std::to_string(b + 1) + "_";
                const std::string name = "Band " + std::to_string(b + 1) + " ";
                ParamInfo used{id + "used", name + "Used", "", 0.f, 1.f, 0.f, false, kOnOff};
                used.automatable = false;
                used.hidden = true;
                list.push_back(used);
                list.push_back({id + "on", name + "On", "", 0.f, 1.f, 1.f, false, kOnOff});
                list.push_back({id + "type", name + "Type", "", 0.f, 7.f, 0.f, false, kTypes});
                list.push_back({id + "freq", name + "Freq", "Hz", 10.f, 22000.f, 1000.f, true});
                list.push_back({id + "gain", name + "Gain", "dB", -30.f, 30.f, 0.f});
                list.push_back({id + "q", name + "Q", "", 0.025f, 40.f, 1.f, true});
                list.push_back({id + "slope", name + "Slope", "", 0.f, static_cast<float>(eq::kNumSlopes - 1), 1.f,
                                false, slopes});
                list.push_back({id + "place", name + "Placement", "", 0.f, 4.f, 0.f, false, kPlaces});
            }
            list.push_back({"output", "Output", "dB", -36.f, 36.f, 0.f});
            list.push_back({"scale", "Gain Scale", "%", 0.f, 200.f, 100.f});
            return list;
        }();
        return kInfos;
    }

    double sampleRate_ = 48000.0;
    double glide_ = 0.1;
    Band bands_[kBands];
    SmoothedValue output_;
};

}  // namespace

SUB_REGISTER_BUILTIN(EqProcessor, AudioEffect);

}  // namespace sub
