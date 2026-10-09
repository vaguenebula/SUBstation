#include "humanize/VelocityModel.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

#include <humanbro/humanbro.hpp>

#include "platform/Files.h"

namespace sub::intelligence::humanize {

namespace {

// Fine enough that rounding to ticks changes nothing the model hears (half a
// millisecond at 120 BPM); it snaps notes to a 1/16 or triplet grid anyway.
constexpr int kTicksPerQuarter = 960;

std::vector<char> readModel(const std::string& path) {
    std::optional<std::vector<char>> data = platform::readFile<std::vector<char>>(path);
    if (!data) throw ModelError((platform::stamp(path) ? "can't read the model file " : "can't open the model file ") + path);
    return std::move(*data);
}

}  // namespace

VelocityModel::VelocityModel(const std::string& path) {
    try {
        model_ = std::make_unique<humanbro::Humanizer>(readModel(path));
    } catch (const std::exception& error) {  // (humanbro::Error, or whatever else a damaged file makes it throw)
        throw ModelError(path + ": " + error.what());
    }
    if (model_->target_mode() != "absolute")
        throw ModelError(path + ": the model predicts " + model_->target_mode() + " velocities, not velocities outright");
}

VelocityModel::~VelocityModel() = default;
VelocityModel::VelocityModel(VelocityModel&&) noexcept = default;
VelocityModel& VelocityModel::operator=(VelocityModel&&) noexcept = default;

std::vector<double> VelocityModel::predict(const std::vector<Note>& part, const Meter& meter) const {
    if (part.empty()) return {};
    // From the bar the first note is in, as a score would start.
    const double barBeats = meter.numerator * 4.0 / meter.denominator;
    double first = part.front().start;
    for (const Note& note : part) first = std::min(first, note.start);
    const double origin = std::floor(first / barBeats + 1e-9) * barBeats;

    humanbro::Score score;
    score.ticks_per_quarter = kTicksPerQuarter;
    score.tempo_changes = {{0, 60'000'000.0 / std::max(1.0, meter.tempo)}};
    score.time_signatures = {{0, meter.numerator, meter.denominator}};
    score.notes.reserve(part.size());
    for (const Note& note : part) {
        const auto onset = std::llround((note.start - origin) * kTicksPerQuarter);
        const auto offset = std::llround((note.start + note.length - origin) * kTicksPerQuarter);
        score.notes.push_back({std::max<int64_t>(0, onset), std::max<int64_t>(0, offset), std::clamp(note.pitch, 0, 127),
                               std::clamp(note.velocity, 1, 127)});
    }
    try {
        const humanbro::FeatureTable table = model_->features(score);
        const std::vector<double> raw = model_->predict_raw(table);  // (in the table's order)
        std::vector<double> velocities(part.size(), 0.0);
        for (size_t row = 0; row < table.rows(); ++row) velocities[static_cast<size_t>(table.note_index[row])] = raw[row];
        return velocities;
    } catch (const humanbro::Error& error) {
        throw ModelError(error.what());
    }
}

std::vector<int> VelocityModel::humanize(const std::vector<Note>& part, const Meter& meter, double amount) const {
    std::vector<int> velocities;
    velocities.reserve(part.size());
    double sumIn = 0.0;
    size_t targets = 0;
    for (const Note& note : part) {
        velocities.push_back(note.velocity);
        if (note.target) {
            sumIn += note.velocity;
            ++targets;
        }
    }
    amount = std::clamp(amount, 0.0, 1.0);
    if (targets == 0 || amount == 0.0) return velocities;

    const std::vector<double> predicted = predict(part, meter);
    double sumPredicted = 0.0;
    for (size_t i = 0; i < part.size(); ++i) {
        if (part[i].target) sumPredicted += predicted[i];
    }
    const double level = (sumIn - sumPredicted) / static_cast<double>(targets);
    for (size_t i = 0; i < part.size(); ++i) {
        if (!part[i].target) continue;
        const double in = part[i].velocity;
        const double out = in + amount * (predicted[i] + level - in);
        velocities[i] = static_cast<int>(std::clamp(std::lround(out), 1L, 127L));
    }
    return velocities;
}

}  // namespace sub::intelligence::humanize
