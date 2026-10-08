// HUMANBRO C++ runtime: velocity humanization with a model exported by export_cpp_model.py.
//
// Typical use (offline, whole clip at once: the features look at past AND future notes):
//
//     humanbro::Humanizer h("model.hbm");
//     humanbro::Score score;                 // fill from a MIDI file, a DAW clip, ...
//     std::vector<int> v = h.humanize(score); // one velocity per score.notes[i]
//
// The bar/beat grid comes from the score's own tempo map and time signatures (what a DAW
// host provides). This matches Python's humanize_midi.py with --beat_source midi, and the
// auto mode for grid-aligned input, which is what the quantized model is meant for.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace humanbro {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Note {
    int64_t onset_tick = 0;
    int64_t offset_tick = 0;  // key release; sustain pedal is not applied
    int pitch = 60;
    int velocity = 64;        // input velocity: only used by residual baselines and Options::mix
};

struct TempoChange {
    int64_t tick = 0;
    double us_per_quarter = 500000.0;
};

struct TimeSignature {
    int64_t tick = 0;
    int numerator = 4;
    int denominator = 4;
};

// A clip or piece in MIDI tick time. Notes may be in any order; results follow this order.
// Leave drums out: every note is treated as part of one piano texture.
struct Score {
    int ticks_per_quarter = 480;
    std::vector<Note> notes;
    std::vector<TempoChange> tempo_changes;       // empty -> 120 BPM; several events per tick: last wins
    std::vector<TimeSignature> time_signatures;   // empty -> 4/4; assumed to sit on bar lines
    int64_t end_tick = 0;                         // end of the clip; 0 -> last note-off
};

struct Options {
    bool smoothing = false;           // group-level zero-phase EMA that keeps voicing and accents
    double smoothing_alpha = 0.35;
    double smoothing_strength = 0.5;
    double accent_threshold = 8.0;
    double dynamics_scale = 1.0;      // >1 widens, <1 narrows the dynamic range around the median
    double offset = 0.0;              // shift all velocities
    double mix = 1.0;                 // 1 = model only, 0 = input velocities
    bool constant_baseline = false;   // residual models: baseline = base_velocity instead of input dynamics
    double base_velocity = 64.0;
};

// Features in canonical order (onset groups, then pitch), row-major.
struct FeatureTable {
    std::vector<std::string> names;
    std::vector<float> values;
    std::vector<int> note_index;  // canonical row -> index into Score::notes
    std::vector<int> group_id;    // onset group (chord) of each row

    std::size_t rows() const { return note_index.size(); }
    std::size_t cols() const { return names.size(); }
    float at(std::size_t r, std::size_t c) const { return values[r * names.size() + c]; }
};

struct BeatInfo {
    std::size_t beats = 0;
    double median_tempo_bpm = 0.0;
    int beats_per_bar = 4;
    int beat_unit = 4;
};

class Humanizer {
public:
    explicit Humanizer(const std::string& model_path);
    // From the .hbm file's contents (read by the caller, e.g. through a wide-path API).
    explicit Humanizer(std::vector<char> model_file);
    ~Humanizer();
    Humanizer(Humanizer&&) noexcept;
    Humanizer& operator=(Humanizer&&) noexcept;

    // One output velocity (1..127) per score.notes[i].
    std::vector<int> humanize(const Score& score, const Options& options = {}) const;

    // Lower-level steps, e.g. for diagnostics or a custom post-processing chain.
    FeatureTable features(const Score& score, BeatInfo* beat_info = nullptr) const;  // columns = model features
    std::vector<double> predict_raw(const FeatureTable& table) const;                // velocity or delta, canonical order

    const std::vector<std::string>& feature_names() const;
    const std::string& target_mode() const;  // "absolute" or "residual"
    bool quantizes_input() const;
    std::size_t num_trees() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Minimal Standard MIDI File support (format 0/1). Velocities are written by patching the
// original note-on bytes, so every other event (pedals, tempo, controllers, note-offs) is kept.
class MidiFile {
public:
    static MidiFile load(const std::string& path);
    void save_with_velocities(const std::string& path, const std::vector<int>& velocities) const;

    const Score& score() const { return score_; }

private:
    std::vector<uint8_t> bytes_;
    Score score_;                            // non-drum notes in file order
    std::vector<std::size_t> velocity_pos_;  // byte offset of each note's note-on velocity
};

}  // namespace humanbro
