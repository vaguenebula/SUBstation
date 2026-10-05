#pragma once
// Automation envelopes: breakpoints that move a parameter over time.
//
// Pure functions on envelopes (vectors of AutomationPoint, values), shared by
// the model, the editor, the UI and the engine bridge.
//
// - Every target is automated the same way, in normalized values (0..1): a
//   device's parameters (see ParamSpec.h), and the mixer's volume and pan of a
//   track or the master. The engine plays the same envelopes
//   (engine/src/Automation.h); the shape of curved segments and the mixer's
//   mappings here must match it.
// - An envelope belongs to an owner, a track id or kMaster, and is keyed by its
//   target: kMixerVolume, kMixerPan, sendKey(return id) (the level of the
//   owner's send to a return track), deviceKey(device id, parameter id), or
//   chainKey(rack id, chain id, kChainVolume or kChainPan) (a rack chain's
//   fader: a target of its rack). Device ids are unique in the project, so a key
//   keeps pointing at its device wherever the device sits (inside a rack too). A
//   send's level, and a chain's volume, map as a volume does.
// - Before its first breakpoint an envelope holds the first one's value, after the
//   last the last one's. Breakpoints may share a beat (a step): from that beat on,
//   the later one's value counts.
// - Each breakpoint's `curve` (-1..1) bends the segment that starts at it: 0 is a
//   straight line, positive bulges upward, negative downward, whichever way the
//   segment goes. Segments are exponential, so splitting one at any point gives two
//   segments of the same kind that together follow it exactly.

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace sub::app {

struct AutomationPoint {
    double beat = 0.0;
    double value = 0.0;  // normalized, 0..1
    double curve = 0.0;  // how the segment to the next point bends, -1..1

    friend bool operator==(const AutomationPoint&, const AutomationPoint&) = default;
};

// Points sorted by beat (points at the same beat keep their order).
using Envelope = std::vector<AutomationPoint>;

// How an owner's automation shows in the arrangement: whether it does, the
// target shown in its main lane (none: none chosen yet), and the targets of the
// lanes shown below it.
struct AutomationView {
    bool shown = false;
    std::optional<QString> key;
    QStringList lanes;

    friend bool operator==(const AutomationView&, const AutomationView&) = default;
};

// The master's automation owner id (and its track id).
inline const QString kMaster = QStringLiteral("master");

namespace automation {

inline constexpr double kCurvature = 6.0;  // engine: kAutomationCurvature
inline const QString kMixerVolume = QStringLiteral("mixer:volume");
inline const QString kMixerPan = QStringLiteral("mixer:pan");
inline const QStringList kMixerKeys{kMixerVolume, kMixerPan};
inline const QString kSendPrefix = QStringLiteral("send:");

// The faders' range. On a volume lane the gain is the cube of the value, so the
// lane's 1 is +6 dB, 0 dB sits at about 0.79 and 0 is silence (engine: kMaxVolumeGain).
inline constexpr double kMaxVolumeDb = 6.0;
inline constexpr double kMinVolumeDb = -70.0;  // the faders' floor: at or below it is silence

// A rack chain's fader is a target of its rack, as a parameter "chain:<chain id>:volume" (or ":pan").
inline const QString kChainVolume = QStringLiteral("volume");
inline const QString kChainPan = QStringLiteral("pan");
inline const QString kChainPrefix = QStringLiteral("chain:");

// --- Keys ---

// What a key targets: ("mixer", "volume" / "pan"), ("send", return id) or
// ("device", device id, parameter id).
struct Target {
    QString kind;   // "mixer", "send" or "device"
    QString id;     // "volume" / "pan", the return's id, or the device's id
    QString param;  // a device's parameter id ("" for the others)

    friend bool operator==(const Target&, const Target&) = default;
};

// A rack chain's fader a key targets.
struct ChainControl {
    QString chainId;
    QString control;  // kChainVolume or kChainPan

    friend bool operator==(const ChainControl&, const ChainControl&) = default;
};

QString deviceKey(const QString& deviceId, const QString& paramId);
QString sendKey(const QString& returnId);
QString chainKey(const QString& rackId, const QString& chainId, const QString& control);
// The parts of a key; none if it isn't an automation target.
std::optional<Target> parseKey(const QString& key);
bool isKey(const QString& key);
// The device a key targets (none for the mixer, sends, or what isn't a key).
std::optional<QString> keyDevice(const QString& key);
// The return track whose send a key targets (none for anything else).
std::optional<QString> keySend(const QString& key);
// The rack chain whose fader a key targets (none for anything else).
std::optional<QString> keyChain(const QString& key);
// (chain id, kChainVolume or kChainPan) of a key targeting a rack chain's fader.
std::optional<ChainControl> keyChainControl(const QString& key);
// Whether a key targets the owner's mixer: its volume, pan or a send (not a device).
bool isMixerKey(const QString& key);

// --- Mixer mappings ---

double volumeToNormalized(double db);
// dB; kMinVolumeDb (silence) at the bottom of the lane.
double normalizedToVolume(double value);
double panToNormalized(double pan);
double normalizedToPan(double value);

// --- Evaluating ---

// How far a segment bent by `bend` has come at x (0..1): positive bends move
// early and level off.
double shape(double x, double bend);
double segmentValue(const AutomationPoint& start, const AutomationPoint& end, double beat);
// How many points are at or before `beat`.
int countAtOrBefore(const Envelope& points, double beat);
// The envelope's value at `beat`; none if it has no points.
std::optional<double> valueAt(const Envelope& points, double beat);
// The value just before `beat` (where a step at `beat` hasn't happened yet).
std::optional<double> leftValue(const Envelope& points, double beat);
// (beat, value) at `count` evenly spaced beats from start to end (for drawing).
std::vector<std::pair<double, double>> sample(const Envelope& points, double start, double end, int count);

// --- Editing (each returns a new envelope) ---

// Sorted by beat (points at the same beat keep their order), within range.
Envelope normalize(const Envelope& points);
// Add a point at `beat` without changing the envelope (a curved segment is
// split into two that follow it exactly). Returns the envelope and the new
// point's index (after any points already at that beat). Throws
// std::invalid_argument for an empty envelope: it has nothing to split.
std::pair<Envelope, int> splitAt(const Envelope& points, double beat);
// A new point, at `value`. Returns the envelope and the point's index.
std::pair<Envelope, int> addPoint(const Envelope& points, double beat, double value);
Envelope movePoints(const Envelope& points, const std::vector<int>& indices, double deltaBeats, double deltaValue);
// Move points together in time and value; returns the envelope and where each
// moved point is now ({old index: new index}). One point stays between its
// neighbours (and after 0). Several override what they land on: the points
// between the first and the last of them go.
std::pair<Envelope, QMap<int, int>> movePointsMapped(const Envelope& points, const std::vector<int>& indices,
                                                     double deltaBeats, double deltaValue);
Envelope deletePoints(const Envelope& points, const std::vector<int>& indices);
Envelope setCurve(const Envelope& points, int index, double curve);
// The point starting the segment that runs through `beat` (none before the
// first point or after the last: no segment to bend there).
std::optional<int> segmentIndex(const Envelope& points, double beat);
// Delete the automation between two beats: the envelope outside stays as it
// was, and runs straight across the range. Deleting every point deletes the
// envelope.
Envelope removeRange(const Envelope& points, double start, double end);
// The envelope from start to end, as points from beat 0.
Envelope copyRange(const Envelope& points, double start, double end);
// Put `content` (points from beat 0, `length` long) at `start`, replacing what
// was there; the envelope outside stays as it was.
Envelope pasteRange(const Envelope& points, const Envelope& content, double start, double length);
// Without the points at these beats that don't change the envelope (the edges
// range edits add where the envelope is flat or straight).
Envelope dropRedundant(const Envelope& points, const std::vector<double>& beats);
// Move the envelope between two beats up or down (`deltaValue`) and in time
// (`deltaBeats`, over what is where it lands; where it was, the envelope runs
// straight across). Breakpoints at the range's edges keep the envelope outside
// it as it was: moved up or down, it steps there.
Envelope moveRange(const Envelope& points, double start, double end, double deltaBeats, double deltaValue);
bool hasPointsIn(const Envelope& points, double start, double end);
// Beat ranges, sorted, with those that overlap or touch joined.
std::vector<std::pair<double, double>> mergeSpans(std::vector<std::pair<double, double>> spans);
Envelope shift(const Envelope& points, double deltaBeats);

}  // namespace automation

}  // namespace sub::app
