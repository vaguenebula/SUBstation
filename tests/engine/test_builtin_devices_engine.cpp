// What every built-in device shares (BuiltinProcessor): input that isn't audio
// is taken as silence before any device sees it.

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "Processor.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/DspBlocks.h"
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
