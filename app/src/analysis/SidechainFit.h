#pragma once
// Fitting the Sidechain device's curve to a kick: where (in frequency) the
// kick and the input (a bass) clash, and how the kick's energy there rises and
// dies away; the curve ducks the input just as much and as long as that.
//
// - Capture keeps what the device's displays stream (the key, the input and
//   the samples since each hit, in step, by their absolute sample index) and
//   hands out each hit's kick once enough of it has come.
// - spectra() compares the kick's spectrum with the input's: their clash is
//   the geometric mean of the two (each against its own loudest), so it is loud
//   only where both are; its band is where it is within kClashDb of its peak.
// - analyze() weights the kick by that clash (zero-phase, in the frequency
//   domain), takes its envelope (the analytic signal's magnitude), and turns it
//   into the reduction a curve needs: full while the kick is at its peak there,
//   none once it is kRangeDb below it (per character: Tight ducks only while the
//   kick is loud, Loose for as long as it lingers). The curve's length is where
//   that ends, and its points come from fitPoints().
// - fitPoints() approximates a curve with as few breakpoints as it takes
//   (adding one where it is furthest off), each segment bent as automation is
//   (model/Automation.h shape), the bend that fits best.
//
// Plain maths, no UI: the Sidechain's editor (ui/src/devices) does the drawing
// and writes the result to the device's parameters, through the ProjectEditor,
// so it saves and undoes like any edit.

#include <QStringList>
#include <QtGlobal>

#include <array>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace sub::app::sidechainFit {

inline const QStringList kCharacters{QStringLiteral("Tight"), QStringLiteral("Natural"), QStringLiteral("Loose")};
// Per character: how far below its peak the kick still ducks the input.
inline constexpr std::array<double, 3> kRangeDb{12.0, 20.0, 30.0};
inline constexpr double kClashLow = 20.0;  // Hz: where a clash is looked for
inline constexpr double kClashHigh = 2000.0;
inline constexpr double kClashDb = 10.0;  // the clash band: where the clash is within this of its peak
inline constexpr int kColumns = 160;  // spectra's points, log-spaced from kClashLow to kClashHigh
inline constexpr double kSmoothOctaves = 1.0 / 6.0;  // spectra are averaged over this much either side
inline constexpr double kFloorDb = -80.0;
inline constexpr int kMaxPoints = 10;
inline constexpr double kTolerance = 0.02;  // fitPoints() stops once the curve is this close everywhere
inline constexpr double kKickSeconds = 0.8;  // of each hit, at most (less when the next comes first)
inline constexpr double kPreSeconds = 0.005;  // kept before each hit (for filtering it)
inline constexpr double kBassSeconds = 2.0;  // of the input before a hit, for its spectrum
inline constexpr int kFftSize = 8192;
inline constexpr double kDecaySmoothSeconds = 0.04;  // the envelope's decay (in dB) is smoothed over this
inline constexpr double kLengthMin = 10.0;  // ms, as the device's `length`
inline constexpr double kLengthMax = 2000.0;

struct Spectra {
    std::vector<double> freqs;  // Hz, kColumns log-spaced
    std::vector<double> kick;   // dB against its loudest
    std::vector<double> bass;   // dB against its loudest (kFloorDb everywhere if there was none)
    std::vector<double> clash;  // dB against its loudest
    double low = 0.0;  // the clash band, Hz
    double high = 0.0;
    double peak = 0.0;
    bool bassHeard = false;
};

// A curve's breakpoint: x 0..1 of the length, y (1: untouched, 0: ducked), and
// how the segment after it bends (-1..1), as automation's.
struct FitPoint {
    double x = 0.0;
    double y = 0.0;
    double curve = 0.0;

    friend bool operator==(const FitPoint&, const FitPoint&) = default;
};

struct Fit {
    Spectra spectra;
    std::vector<double> times;     // ms from the hit
    std::vector<double> envelope;  // the kick's envelope in the clash band, 0..1 (linear, against its peak)
    std::vector<double> target;    // the curve the envelope calls for (1: untouched, 0: ducked), at `times`
    double length = 0.0;           // ms
    std::vector<FitPoint> points;  // the curve fitted
};

// --- Spectra ---

// The kick's and the bass's spectra, where they clash, and the band they clash most in.
Spectra spectra(const std::vector<double>& kick, const std::vector<double>& bass, double sampleRate);

// --- The envelope ---

// The kick's envelope where it clashes: weighted by the clash (in amplitude,
// zero-phase), the analytic signal's magnitude, smoothed over a few ms. From
// `pre` samples in (the hit) on.
std::vector<double> clashEnvelope(const std::vector<double>& kick, double sampleRate, const Spectra& found,
                                  int pre = 0);
// How much the input must give way (0..1) along the kick's envelope: all of it
// from the hit to the envelope's peak (a kick whose pitch falls into the band
// gets there late, but the input must be out of the way of its attack too),
// then less as it falls, none `rangeDb` below the peak (and never more again).
// Its decay is in dB, smoothed over `smooth` samples: it falls evenly.
std::vector<double> reduction(const std::vector<double>& envelope, double rangeDb, int smooth = 0);

// --- Points ---

// Automation's shape (model/Automation.h), as the fit has it: how far a bent
// segment has come at x (0..1).
double shape(double x, double bend);
// A curve of points (in order of x) at x: as the device plays it.
double curveAt(const std::vector<FitPoint>& points, double x);
std::vector<double> curveAt(const std::vector<FitPoint>& points, const std::vector<double>& x);
// Breakpoints for the curve y(x), x from 0 to 1: the ends, then one at a time
// where the fitted curve is furthest off, until it is within `tolerance` or
// there are `maxPoints`. Rounded (x and y to 5 places, the bend to 4).
std::vector<FitPoint> fitPoints(const std::vector<double>& x, const std::vector<double>& y,
                                int maxPoints = kMaxPoints, double tolerance = kTolerance);

// --- Analysis ---

// The fit for these hits (each from `pre` samples before its hit) over this
// input. None without a kick to speak of.
std::optional<Fit> analyze(const std::vector<std::vector<double>>& kicks, const std::vector<double>& bass,
                           double sampleRate, int character = 1, int pre = 0);

// --- Capturing hits ---

// A stream's latest values, by absolute index.
class Ring {
public:
    explicit Ring(std::size_t size);

    // Values from absolute index `start`: a gap (or the first values) starts it again from these.
    void feed(qint64 start, const float* values, std::size_t count);
    // What it holds of [start, end).
    std::vector<float> get(qint64 start, qint64 end) const;
    qint64 start() const { return start_; }  // what it holds: [start, end)
    qint64 end() const { return end_; }

private:
    std::vector<float> data_;
    qint64 start_ = 0;
    qint64 end_ = 0;
};

// The device's displays as they come ("key", "input", "phase", in step), and
// its hits: each hit's kick, once kKickSeconds of it (or all of it until the
// next hit) has come.
class Capture {
public:
    enum class Stream { Key, Input, Phase };
    // A hit's kick (from kPreSeconds before it) and the input (from
    // kBassSeconds before it), both to the kick's end.
    using Hit = std::pair<std::vector<float>, std::vector<float>>;

    explicit Capture(double sampleRate = 48000.0, double keepSeconds = 6.0);

    double sampleRate() const { return sampleRate_; }
    // A display's values since the last, from absolute index `start`.
    void feed(Stream stream, qint64 start, const std::vector<float>& values);
    const Ring& stream(Stream stream) const;
    // The key's loudest since the last call (then 0 again).
    float takeKeyPeak();
    // The hits whose kick has all come since the last call.
    std::vector<Hit> hits();
    // The latest phase (-1: no curve playing, or nothing came).
    double lastPhase() const { return lastPhase_; }
    int hitCount() const { return hitCount_; }
    // The hits whose kick hasn't all come yet.
    const std::vector<qint64>& pending() const { return pending_; }

private:
    double sampleRate_;
    Ring key_;
    Ring input_;
    Ring phase_;
    std::vector<qint64> pending_;
    double lastPhase_ = -1.0;
    int hitCount_ = 0;
    float keyPeak_ = 0.0f;
};

}  // namespace sub::app::sidechainFit
