#pragma once
// Helpers the application layer's tests share.

#include "model/Clip.h"
#include "model/Device.h"
#include "model/Track.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace sub::app::test {

// The engine's sample rate when no device is open.
inline constexpr int kSampleRate = 48000;

// Call first (initTestCase): the application is "SUBstation Tests", so tests
// never touch the user's settings (each test executable keeps its own, in a
// temporary folder), and the folders the application keeps things in (the
// preset library, the browser's index, plug-in caches, recordings) are
// temporary ones.
void prepareApplication();

// An environment variable set (or, for none, unset) for as long as it lives,
// then as it was: a test's own folder for SUBSTATION_RECORDINGS or
// SUBSTATION_PRESETS, never the user's after it, even when a check fails.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::optional<QString>& value);
    ~ScopedEnv();
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* name_;
    QByteArray before_;
    bool was_;
};

// A temporary folder, gone with it.
class TempDir {
public:
    TempDir();
    bool isValid() const { return dir_.isValid(); }
    QString path() const { return dir_.path(); }
    // A path in it.
    QString path(const QString& name) const;

private:
    QTemporaryDir dir_;
};

// Writes float samples in [-1, 1) as 16-bit PCM: `samples` holds `channels`
// interleaved channels. Returns the path.
QString writeWav(const QString& path, const std::vector<float>& samples, int channels = 1,
                 int sampleRate = kSampleRate);

// A track of the arrangement, as the tests make them.
Track makeTrack(const QString& id, const QString& name, const QString& kind = kAudioKind,
                const std::optional<QString>& parent = std::nullopt);
// A device with these parameters.
Device makeDevice(const QString& id, const QString& kind, const QMap<QString, double>& params = {});

// The ids of some devices or tracks, in order; the devices' kinds.
QStringList ids(const std::vector<Device>& devices);
QStringList ids(const std::vector<const Track*>& tracks);
QStringList kinds(const std::vector<Device>& devices);

// A time rounded to 6 decimals, so sums of beats compare equal.
inline double round6(double value) { return std::round(value * 1e6) / 1e6; }

// Where clips start and end, in beats (at `tempo`: the tests' 120 BPM unless
// said), rounded with round6().
using Spans = std::vector<std::pair<double, double>>;
Spans spans(const std::vector<Clip>& clips, double tempo = 120.0);

}  // namespace sub::app::test
