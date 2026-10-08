// Humanizing velocities by machine learning: HUMANBRO's model (vendored in
// third_party/humanbro), an XGBoost tree ensemble trained on the MAESTRO piano
// performances, which predicts each note's velocity from its musical context:
// its pitch, where it falls in the bar, the melody and the chords around it, the
// texture, the phrase. It never sees a velocity, so the same notes always get
// the same prediction.
//
// A part (a track's notes) is humanized as a whole, since each note's features
// look at the notes before and after it: the notes around those humanized come
// along as context. How loud a part is overall a score doesn't say, so the
// model's velocities are levelled to the targets' own mean first; each target
// then moves `amount` of the way to its levelled velocity. Notes are given in
// beats on the song's timeline, bar lines every bar's length from beat 0; the
// part is placed from the bar its first note is in (the features count beats
// and bars from the start, as from a score's).
//
// The model shipped (models/velocity.hbm) is HUMANBRO's "quantized" one,
// trained on performances snapped to the beat grid: it snaps the notes it is
// given to the grid too, so drawn, quantized and played-in notes all suit it.
// Loading it takes about 100 ms and 16 MB; a part of 200 notes takes about
// 40 ms, of 10 000 about 200 ms (on all cores past 512 notes).
//
// Timing will be a model of its own, beside this one.

#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace humanbro {
class Humanizer;
}

namespace sub::intelligence::humanize {

struct ModelError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// A note of the part, in quarter-note beats on the song's timeline.
struct Note {
    int pitch = 60;  // MIDI note number
    double start = 0.0;
    double length = 0.0;
    int velocity = 100;   // 1..127
    bool target = false;  // humanized; otherwise only context
};

// The song's tempo and time signature.
struct Meter {
    double tempo = 120.0;  // quarter notes a minute
    int numerator = 4;
    int denominator = 4;
};

class VelocityModel {
public:
    // Loads a model (a .hbm file; the path UTF-8, the system's form). Throws
    // ModelError if it can't be read, isn't one, or predicts something other
    // than velocities outright (HUMANBRO's residual models).
    explicit VelocityModel(const std::string& path);
    ~VelocityModel();
    VelocityModel(VelocityModel&&) noexcept;
    VelocityModel& operator=(VelocityModel&&) noexcept;

    // The part's velocities, one per note in order: the targets humanized by
    // `amount` (0..1; 1: the model's, levelled), the others as they are.
    std::vector<int> humanize(const std::vector<Note>& part, const Meter& meter, double amount) const;
    // The model's velocity for each note, as it predicts it (not levelled).
    std::vector<double> predict(const std::vector<Note>& part, const Meter& meter) const;

private:
    std::unique_ptr<humanbro::Humanizer> model_;
};

}  // namespace sub::intelligence::humanize
