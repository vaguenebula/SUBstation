// What every built-in device shares (BuiltinProcessor): input that isn't audio
// is taken as silence before any device sees it; and what the engine does with
// their latency.

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "Engine.h"
#include "Processor.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/DspBlocks.h"
#include "harness/Fixtures.h"
#include "harness/Test.h"

TEST_CASE("every built-in effect takes NaN, infinity and absurd levels as silence") {
    // A broken plug-in or file can hand on NaN or infinity; a device's recursive states would
    // keep it for good (and its output, and the mix's after it). One bad sample on each channel,
    // then ordinary noise: every sample out stays finite, and the device still plays.
    const float bad[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                         -std::numeric_limits<float>::infinity(), 3e38f};
    for (const sub::BuiltinInfo& info : sub::BuiltinRegistry::instance().devices()) {
        if (info.isInstrument()) continue;
        for (const float value : bad) {
            INFO(info.id + " given " + std::to_string(value));
            const std::shared_ptr<sub::Processor> device = sub::BuiltinRegistry::instance().create(info.id);
            device->prepare(48000.0, 1024);
            sub::dsp::Noise noise;
            std::vector<float> left(256), right(256);
            float* channels[2] = {left.data(), right.data()};
            sub::ProcessContext ctx;
            ctx.playing = true;
            bool finite = true;
            double energy = 0.0;
            for (int block = 0; block < 200; ++block) {
                for (int i = 0; i < 256; ++i) {
                    left[size_t(i)] = 0.25f * noise.next();
                    right[size_t(i)] = 0.25f * noise.next();
                }
                if (block == 3) left[17] = value;
                if (block == 5) right[200] = value;
                ctx.samplePos = int64_t(block) * 256;
                ctx.beatPos = double(ctx.samplePos) / ctx.samplesPerBeat();
                device->process(ctx, channels, 2, 256);
                for (int i = 0; i < 256; ++i) {
                    finite = finite && std::isfinite(left[size_t(i)]) && std::isfinite(right[size_t(i)]);
                    if (block >= 150) energy += double(left[size_t(i)]) * left[size_t(i)];
                }
            }
            CHECK(finite);
            CHECK(energy > 0.0);
        }
    }
}

TEST_CASE("a latency changed back after another edit is still compensated") {
    // A device's latency grows, an edit rebuilds the snapshot before the engine's idle() (which then
    // aligns to the longer latency), and the latency goes back to what the device last reported: the
    // engine sees it differ from what the snapshot compensates and realigns.
    struct Case {
        const char* kind;
        const char* param;
        float longer, back;
        const char* passParam;  // a setting that lets the click through untouched
        float passValue;
    };
    const Case cases[] = {
        {"saturator", "hq", 1.f, 0.f, "mix", 0.f},
        {"limiter", "lookahead", 2.f, 1.f, nullptr, 0.f},
        {"gate", "lookahead", 2.f, 1.f, "floor", 0.f},
        {"sidechain", "lookahead", 10.f, 0.f, "depth", 0.f},
    };
    const auto clickAt = [](const std::vector<float>& out) {
        size_t best = 0;
        for (size_t f = 0; f < out.size() / 2; ++f)
            if (std::abs(out[2 * f]) > std::abs(out[2 * best])) best = f;
        return best;
    };
    for (const Case& c : cases) {
        INFO(c.kind);
        sub::Engine engine;
        engine.setClipFadeMs(0);
        const uint32_t wet = subtest::stereoClickTrack(engine, 0.5f, 0.5f, 1000, 1.0);
        const uint32_t dry = subtest::stereoClickTrack(engine, 0.5f, 0.5f, 1000, 1.0);
        const uint32_t device = engine.addBuiltinProcessor(engine.trackChain(wet), c.kind, -1);
        if (c.passParam) subtest::setParam(engine, device, c.passParam, c.passValue);
        engine.idle();
        subtest::setParam(engine, device, c.param, c.longer);
        engine.addTrack();  // rebuilds the snapshot with the longer latency
        subtest::setParam(engine, device, c.param, c.back);
        engine.idle();
        engine.setTrackMute(dry, true);
        CHECK_EQ(clickAt(engine.renderOffline(0.0, 24000)), size_t{1000});
    }
}
