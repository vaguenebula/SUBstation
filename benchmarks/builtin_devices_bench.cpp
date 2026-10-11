// What each built-in audio effect costs: the device alone (no engine, no
// graph), fed stereo noise and tones at about -12 dBFS in blocks as the audio
// thread gives them, its time against how long the audio plays.
//
//     builtin_devices_bench [--device ID[,ID...]] [--seconds 10] [--block 256] [--rate 48000]
//         [--set PARAM=VALUE ...] [--json out.json]
//
// Without --device, every built-in audio effect at its defaults. --set changes a
// parameter (by id, plain value) on every device that has it, before it is
// prepared: a heavy setting (a reverb's quality, oversampling) measured the same
// way. The load is the best of three runs; 100% would be a whole core.
//
// Qt-free: it links the engine alone.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <stdexcept>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "Json.h"
#include "Processor.h"
#include "builtin/BuiltinRegistry.h"
#include "rt/RtUtils.h"

namespace {

using sub::bench::Json;
using Clock = std::chrono::steady_clock;

struct Options {
    std::vector<std::string> devices;
    double seconds = 10.0;
    int block = 256;
    double rate = 48000.0;
    std::map<std::string, float> settings;
    std::string json;
};

std::vector<std::string> split(const std::string& text, char by) {
    std::vector<std::string> parts;
    std::stringstream in(text);
    for (std::string part; std::getline(in, part, by);)
        if (!part.empty()) parts.push_back(part);
    return parts;
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(arg + " needs a value");
            return argv[++i];
        };
        if (arg == "--device") {
            o.devices = split(next(), ',');
        } else if (arg == "--seconds") {
            o.seconds = std::stod(next());
        } else if (arg == "--block") {
            o.block = std::clamp(std::stoi(next()), 1, 1024);
        } else if (arg == "--rate") {
            o.rate = std::stod(next());
        } else if (arg == "--set") {
            const std::string setting = next();
            const auto at = setting.find('=');
            if (at == std::string::npos) throw std::runtime_error("--set takes PARAM=VALUE");
            o.settings[setting.substr(0, at)] = std::stof(setting.substr(at + 1));
        } else if (arg == "--json") {
            o.json = next();
        } else {
            throw std::runtime_error("unknown argument " + arg);
        }
    }
    return o;
}

// Stereo test audio: noise and two tones, a different mix per side, about -12 dBFS.
void fill(std::vector<float>& left, std::vector<float>& right, double rate) {
    uint32_t state = 0x12345678u;
    for (size_t i = 0; i < left.size(); ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        const float noise = static_cast<float>(state) * (2.f / 4294967296.f) - 1.f;
        const double t = static_cast<double>(i) / rate;
        const auto low = static_cast<float>(std::sin(2 * 3.14159265358979 * 110.0 * t));
        const auto high = static_cast<float>(std::sin(2 * 3.14159265358979 * 1760.0 * t));
        left[i] = 0.12f * noise + 0.15f * low + 0.05f * high;
        right[i] = 0.12f * noise + 0.10f * low + 0.08f * high;
    }
}

// Seconds of processing for `frames` frames of `left`/`right`, in blocks.
double run(sub::Processor& device, const std::vector<float>& left, const std::vector<float>& right, int block,
           double rate) {
    std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
    float* channels[2] = {l.data(), r.data()};
    sub::ProcessContext ctx;
    ctx.sampleRate = rate;
    ctx.playing = true;
    sub::ScopedNoDenormals noDenormals;
    double busy = 0.0;
    for (size_t at = 0; at + static_cast<size_t>(block) <= left.size(); at += static_cast<size_t>(block)) {
        std::copy_n(left.data() + at, block, l.data());
        std::copy_n(right.data() + at, block, r.data());
        ctx.samplePos = static_cast<int64_t>(at);
        ctx.beatPos = static_cast<double>(at) / ctx.samplesPerBeat();
        const auto start = Clock::now();
        device.process(ctx, channels, 2, block);
        busy += std::chrono::duration<double>(Clock::now() - start).count();
    }
    return busy;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options o = parse(argc, argv);
        auto& registry = sub::BuiltinRegistry::instance();
        std::vector<std::string> ids = o.devices;
        if (ids.empty()) {
            for (const auto& info : registry.devices())
                if (!info.isInstrument()) ids.push_back(info.id);
        }
        const auto frames = static_cast<size_t>(o.seconds * o.rate);
        std::vector<float> left(frames), right(frames);
        fill(left, right, o.rate);

        std::printf("%-22s %10s %10s\n", "device", "load", "x realtime");
        Json results = Json::array();
        for (const std::string& id : ids) {
            double best = 1e300;
            for (int attempt = 0; attempt < 3; ++attempt) {
                const std::shared_ptr<sub::Processor> device = registry.create(id);
                const auto& params = device->params();
                for (const auto& [param, value] : o.settings) {
                    for (size_t i = 0; i < params.size(); ++i)
                        if (params[i].id == param) device->setParam(static_cast<int>(i), value);
                }
                device->prepare(o.rate, 1024);
                best = std::min(best, run(*device, left, right, o.block, o.rate));
            }
            const double load = best / o.seconds;
            std::printf("%-22s %9.2f%% %10.0f\n", id.c_str(), 100.0 * load, 1.0 / std::max(load, 1e-12));
            results.push(Json::object().set("device", id).set("load_percent", 100.0 * load));
        }
        if (!o.json.empty()) {
            const Json report = Json::object()
                                    .set("seconds", o.seconds)
                                    .set("block", o.block)
                                    .set("rate", o.rate)
                                    .set("results", results);
            if (!report.save(o.json, 2)) throw std::runtime_error("couldn't write " + o.json);
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "builtin_devices_bench: %s\n", e.what());
        return 1;
    }
}
