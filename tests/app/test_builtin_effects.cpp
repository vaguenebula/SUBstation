// The ten built-in audio effects (Saturator, Amp, Erosion, Multiband Dynamics,
// Reverb, Limiter, Chorus-Ensemble, Phaser-Flanger, Spectral Compressor, Gate)
// with the rest of the application, what each device's own tests leave out: a
// project with one, every parameter away from its default and one automated,
// saved and opened again as it was (in the model and the engine); presets of
// them, as new devices and loaded into devices; in a rack's chain (rendering as
// on the track, their latency the rack's); their latency compensated against a
// dry track to the sample (after opening the project, and as it changes, too);
// undo and redo reaching the engine; the sidechained ones keyed by another track
// through the editor (and once the project is opened again); and switched off
// and on again without a click or anything they held before. Each test runs
// once per device, a row per kind.

#include "SessionFixture.h"
#include "TestSupport.h"

#include "io/Serialization.h"
#include "model/Automation.h"
#include "model/Devices.h"
#include "session/DeviceSelection.h"

#include <QJsonObject>
#include <QSettings>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <random>

using namespace sub::app;
using sub::app::test::SessionFixture;
using sub::app::test::TempDir;

namespace {

constexpr int kRate = test::kSampleRate;
constexpr int kSpb = kRate / 2;  // samples per beat at 120 BPM

const QStringList kEffects{QStringLiteral("saturator"), QStringLiteral("amp"),     QStringLiteral("erosion"),
                           QStringLiteral("multiband"), QStringLiteral("reverb"),  QStringLiteral("limiter"),
                           QStringLiteral("chorus"),    QStringLiteral("phaser"),  QStringLiteral("spectral"),
                           QStringLiteral("gate")};

// A test's rows: one per kind.
void kindRows(const QStringList& kinds) {
    QTest::addColumn<QString>("kind");
    for (const QString& kind : kinds) QTest::newRow(qPrintable(kind)) << kind;
}

const std::vector<sub::ParamInfo>& infos(const QString& kind) { return builtinDevice(kind)->info->params; }

// A value away from a parameter's default: a list's next choice (a switch the
// other way; the last one's, the one before), the next whole value, or a
// continuous one moved by 30 % of its knob's travel.
double awayFromDefault(const sub::ParamInfo& info) {
    if (const int count = info.stepCount(); count > 0) {
        const double index = std::round(info.defaultValue - info.minValue);
        return info.minValue + (index < count ? index + 1.0 : index - 1.0);
    }
    const float from = info.toNormalized(info.defaultValue);
    return info.fromNormalized(from < 0.5f ? from + 0.3f : from - 0.3f);
}

// Every parameter of a kind away from its default.
QMap<QString, double> allAway(const QString& kind) {
    QMap<QString, double> values;
    for (const sub::ParamInfo& info : infos(kind)) {
        values.insert(QString::fromStdString(info.id), awayFromDefault(info));
    }
    return values;
}

// The parameter the tests automate: the first continuous one.
QString automatedParam(const QString& kind) {
    for (const sub::ParamInfo& info : infos(kind)) {
        if (info.automatable && info.stepCount() == 0) return QString::fromStdString(info.id);
    }
    return {};
}

// The settings that give a device its latency where it is optional (the
// Saturator's Hi-Quality, the longest lookahead of the Limiter and the Gate).
QMap<QString, double> latencyOn(const QString& kind) {
    if (kind == u"saturator") return {{QStringLiteral("hq"), 1.0}};
    if (kind == u"limiter" || kind == u"gate") return {{QStringLiteral("lookahead"), 2.0}};
    return {};
}

// The devices with a latency, and settings with which they pass their input
// through, that much later: fully dry, no erosion, a click under the
// Limiter's ceiling, a Gate open whatever it hears.
const QStringList kLatent{QStringLiteral("saturator"), QStringLiteral("amp"),      QStringLiteral("erosion"),
                          QStringLiteral("limiter"),   QStringLiteral("spectral"), QStringLiteral("gate")};

QMap<QString, double> passThrough(const QString& kind) {
    QMap<QString, double> values = latencyOn(kind);
    if (kind == u"saturator" || kind == u"amp" || kind == u"spectral") values.insert(QStringLiteral("mix"), 0.0);
    if (kind == u"erosion") values.insert(QStringLiteral("amount"), 0.0);
    if (kind == u"gate") {
        values.insert(QStringLiteral("threshold"), -70.0);
        values.insert(QStringLiteral("floor"), 0.0);
    }
    return values;
}

// Where `actual` (a device's parameters, the model's or the engine's) isn't
// `expected`, and what is missing from it or more than it: "" if they are the same.
QString differences(const QMap<QString, double>& actual, const QMap<QString, double>& expected) {
    QStringList found;
    for (auto it = expected.begin(); it != expected.end(); ++it) {
        if (!actual.contains(it.key())) {
            found.append(it.key() + QStringLiteral(" missing"));
        } else if (static_cast<float>(actual.value(it.key())) != static_cast<float>(it.value())) {
            found.append(QStringLiteral("%1 %2 (not %3)").arg(it.key()).arg(actual.value(it.key())).arg(it.value()));
        }
    }
    for (auto it = actual.begin(); it != actual.end(); ++it) {
        if (!expected.contains(it.key())) found.append(it.key() + QStringLiteral(" not expected"));
    }
    return found.join(QStringLiteral(", "));
}

// --- Signals (interleaved stereo) and what is measured of renders ---

size_t frameAt(double beats) { return static_cast<size_t>(std::llround(beats * kSpb)); }

std::vector<float> silence(double beats) { return std::vector<float>(2 * frameAt(beats), 0.f); }

// Noise (uniform in -level..level, its own in each channel) over [from, to) beats.
void addNoise(std::vector<float>& samples, double from, double to, float level, unsigned seed) {
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> uniform(-level, level);
    for (size_t i = 2 * frameAt(from); i < std::min(samples.size(), 2 * frameAt(to)); ++i) samples[i] = uniform(random);
}

// A sine over [from, to) beats, in phase with the file's start.
void addSine(std::vector<float>& samples, double from, double to, float level, double hz) {
    for (size_t f = frameAt(from); f < std::min(samples.size() / 2, frameAt(to)); ++f) {
        const double phase = 2.0 * std::numbers::pi * hz * static_cast<double>(f) / kRate;
        const float value = level * static_cast<float>(std::sin(phase));
        samples[2 * f] = samples[2 * f + 1] = value;
    }
}

// The largest absolute sample of frames [from, to), both channels.
double peakIn(const std::vector<float>& samples, size_t from = 0, size_t to = SIZE_MAX) {
    double peak = 0.0;
    for (size_t i = 2 * from; i < std::min(samples.size(), 2 * std::min(to, samples.size())); ++i) {
        peak = std::max(peak, static_cast<double>(std::abs(samples[i])));
    }
    return peak;
}

double rmsIn(const std::vector<float>& samples, size_t from, size_t to) {
    double sum = 0.0;
    for (size_t i = 2 * from; i < 2 * to; ++i) sum += static_cast<double>(samples[i]) * samples[i];
    return std::sqrt(sum / static_cast<double>(2 * (to - from)));
}

// The largest difference between two renders over frames [from, to).
double maxDifference(const std::vector<float>& a, const std::vector<float>& b, size_t from = 0, size_t to = SIZE_MAX) {
    if (a.size() != b.size()) return std::numeric_limits<double>::infinity();
    double most = 0.0;
    for (size_t i = 2 * from; i < std::min(a.size(), 2 * std::min(to, a.size())); ++i) {
        most = std::max(most, std::abs(static_cast<double>(a[i]) - b[i]));
    }
    return most;
}

// The largest step from one sample to the next over frames [from, to), both channels.
double maxStep(const std::vector<float>& samples, size_t from, size_t to) {
    double most = 0.0;
    for (size_t i = 2 * std::max<size_t>(from, 1); i < 2 * to; ++i) {
        most = std::max(most, std::abs(static_cast<double>(samples[i]) - samples[i - 2]));
    }
    return most;
}

std::vector<float> plus(const std::vector<float>& a, const std::vector<float>& b) {
    std::vector<float> sum(a);
    for (size_t i = 0; i < sum.size() && i < b.size(); ++i) sum[i] += b[i];
    return sum;
}

// The frame of a channel's largest absolute sample.
size_t loudestFrame(const std::vector<float>& samples, int channel) {
    size_t best = 0;
    for (size_t f = 0; f < samples.size() / 2; ++f) {
        if (std::abs(samples[2 * f + channel]) > std::abs(samples[2 * best + channel])) best = f;
    }
    return best;
}

// A session with the effects' helpers: tracks playing signals made here, renders, the engine's devices.
struct Effects : SessionFixture {
    TempDir dir;
    QStringList files;  // the signals played

    // A track's clip plays `samples` from beat 0 (the engine has the file).
    void play(const QString& trackId, const std::vector<float>& samples) {
        const QString path = test::writeWav(dir.path(QStringLiteral("signal%1.wav").arg(files.size())), samples, 2);
        files.append(path);
        engine.loadSource(path.toStdString());
        const double seconds = static_cast<double>(samples.size() / 2) / kRate;
        editor().commitClips(QStringLiteral("Play"), {{trackId, {Clip::audio(trackId + QStringLiteral("c"), path,
                                                                              QStringLiteral("signal"), 0.0, seconds,
                                                                              0.0, seconds)}}});
    }
    // A new audio track playing `samples`; its id.
    QString track(const std::vector<float>& samples, const QString& name = {}) {
        const QString id = editor().addAudioTrack(-1, name);
        play(id, samples);
        return id;
    }
    // Until the bridge has decoded every signal played (as it does once a project opens).
    bool waitForSignals() {
        return std::all_of(files.begin(), files.end(), [this](const QString& path) { return waitForSource(path); });
    }
    // `count` beats rendered offline from `from`.
    std::vector<float> beats(double count, double from = 0.0) {
        return render(static_cast<int64_t>(frameAt(count)), from);
    }
    // Only `keep` is heard (the others muted).
    void solo(const QString& keep) {
        for (const Track& t : project().tracks()) {
            editor().setTrackParam(t.id, TrackField::Mute, t.id == keep ? 0.0 : 1.0);
        }
    }
    void setParams(const QString& trackId, const QString& deviceId, const QMap<QString, double>& values) {
        for (auto it = values.begin(); it != values.end(); ++it) {
            editor().setDeviceParam(trackId, deviceId, it.key(), it.value());
        }
    }
    quint32 processor(const QString& trackId, const QString& deviceId) const {
        return *bridge().engineDeviceId(trackId, deviceId);
    }
    // The engine's values of a device's parameters, by id.
    QMap<QString, double> engineValues(const QString& trackId, const QString& deviceId) {
        QMap<QString, double> values;
        const quint32 id = processor(trackId, deviceId);
        const std::vector<sub::ParamInfo> params = engine.processorParams(id);
        for (size_t i = 0; i < params.size(); ++i) {
            values.insert(QString::fromStdString(params[i].id), engine.processorParam(id, static_cast<int>(i)));
        }
        return values;
    }
    // Where the model and the engine differ from `expected` ("" nowhere).
    QString differ(const QString& trackId, const QString& deviceId, const QMap<QString, double>& expected) {
        const QString model = differences(project().device(trackId, deviceId).params, expected);
        const QString engineSide = differences(engineValues(trackId, deviceId), expected);
        if (model.isEmpty() && engineSide.isEmpty()) return {};
        return QStringLiteral("model: %1; engine: %2").arg(model, engineSide);
    }
};

}  // namespace

class TestBuiltinEffects : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }
    void init() { QSettings().clear(); }

    // Every parameter away from its default (lists and switches too), one automated:
    // the project file brings them all back, to the model and to the engine.
    void aProjectOpensAsItWasSaved_data() { kindRows(kEffects); }
    void aProjectOpensAsItWasSaved() {
        QFETCH(QString, kind);
        Effects f;
        Session& s = f.s();
        const QString track = f.editor().addAudioTrack();
        const QString device = f.editor().addDevice(track, kind);
        QVERIFY(!device.isEmpty());
        const QMap<QString, double> values = allAway(kind);
        QCOMPARE(values.size(), static_cast<int>(infos(kind).size()));
        for (const sub::ParamInfo& info : infos(kind)) {
            const QString id = QString::fromStdString(info.id);
            QVERIFY2(static_cast<float>(values.value(id)) != info.defaultValue, qPrintable(id));
        }
        f.setParams(track, device, values);
        QVERIFY2(f.differ(track, device, values).isEmpty(), qPrintable(f.differ(track, device, values)));
        const QString param = automatedParam(kind);
        QVERIFY(!param.isEmpty());
        const QString key = automation::deviceKey(device, param);
        const Envelope envelope{{0.0, 0.2, 0.0}, {4.0, 0.8, 0.5}};
        f.editor().setEnvelope(track, key, envelope);
        QVERIFY(f.bridge().isAutomated(track, key));
        const QJsonObject saved = deviceToJson(f.project().device(track, device));
        const QString path = f.dir.path(kind + QStringLiteral(".gilproj"));
        QVERIFY(s.saveProjectAs(path));

        s.newProject();
        QVERIFY(f.project().tracks().empty());
        QVERIFY(s.openProject(path));
        const Device* opened = f.project().findDevice(track, device);
        QVERIFY(opened);
        QCOMPARE(opened->kind, kind);
        QCOMPARE(deviceToJson(*opened), saved);
        QVERIFY2(f.differ(track, device, values).isEmpty(), qPrintable(f.differ(track, device, values)));
        QCOMPARE(f.project().envelope(track, key), envelope);
        QVERIFY(f.bridge().isAutomated(track, key));
    }

    // Saved as a preset, every parameter comes back: as a new device, and loaded
    // into a device of its kind (one undo step, as far as the engine).
    void presetsBringBackEveryParameter_data() { kindRows(kEffects); }
    void presetsBringBackEveryParameter() {
        QFETCH(QString, kind);
        Effects f;
        DeviceSelection& view = *f.s().deviceSelection();
        const QString track = f.s().insertAudioTrack();
        f.selection().selectTrack(track);
        const QString device = f.editor().addDevice(track, kind);
        const QMap<QString, double> values = allAway(kind);
        f.setParams(track, device, values);
        const QString path = view.savePreset(device, QStringLiteral("Everything Moved"));
        QVERIFY(!path.isEmpty());
        QVERIFY2(differences(loadPreset(path).params, values).isEmpty(),
                 qPrintable(differences(loadPreset(path).params, values)));

        QVERIFY(view.insertPreset(path));
        const Device& added = f.project().track(track).devices.back();
        QVERIFY(added.id != device);
        QCOMPARE(added.kind, kind);
        QVERIFY2(f.differ(track, added.id, values).isEmpty(), qPrintable(f.differ(track, added.id, values)));

        const QString target = f.editor().addDevice(track, kind);
        const QMap<QString, double> defaults = builtinDevice(kind)->defaults;
        QVERIFY2(f.differ(track, target, defaults).isEmpty(), qPrintable(f.differ(track, target, defaults)));
        QVERIFY(view.loadPresetInto(target, path));
        QVERIFY2(f.differ(track, target, values).isEmpty(), qPrintable(f.differ(track, target, values)));
        f.stack().undo();
        QVERIFY2(f.differ(track, target, defaults).isEmpty(), qPrintable(f.differ(track, target, defaults)));
        f.stack().redo();
        QVERIFY2(f.differ(track, target, values).isEmpty(), qPrintable(f.differ(track, target, values)));
    }

    // In a rack's chain a device renders as it does on the track, and its latency
    // is the rack's: an empty chain beside it (its input) comes out with it.
    void inARackChainAsOnTheTrack_data() { kindRows(kEffects); }
    void inARackChainAsOnTheTrack() {
        QFETCH(QString, kind);
        Effects f;
        std::vector<float> noise = silence(2.0);
        addNoise(noise, 0.0, 2.0, 0.5f, 1);  // (over the Gate's threshold)
        const QString track = f.track(noise);
        const std::vector<float> raw = f.beats(1);
        const QString device = f.editor().addDevice(track, kind);
        f.setParams(track, device, latencyOn(kind));
        f.bridge().pollPlugins();  // (the meter timer's: a new latency realigns the tracks)
        const int latency = f.bridge().deviceLatency(track, device);
        QCOMPARE(latency > 0, kLatent.contains(kind));
        const std::vector<float> plain = f.beats(1);
        QVERIFY(peakIn(plain) > 0.01);
        QCOMPARE(maxDifference(f.beats(1), plain), 0.0);  // (renders repeat exactly)

        const QString rack = f.editor().groupDevices(track, {device});
        QVERIFY(!rack.isEmpty());
        QCOMPARE(f.project().track(track).devices.size(), size_t{1});
        QCOMPARE(f.bridge().deviceLatency(track, rack), latency);
        const std::vector<float> chained = f.beats(1);
        QVERIFY2(maxDifference(chained, plain) < 1e-6, qPrintable(QString::number(maxDifference(chained, plain))));

        f.editor().addRackChain(track, rack);
        QCOMPARE(f.bridge().deviceLatency(track, rack), latency);
        const std::vector<float> both = f.beats(1);
        const std::vector<float> expected = plus(plain, raw);
        QVERIFY2(maxDifference(both, expected) < 1e-5, qPrintable(QString::number(maxDifference(both, expected))));
    }

    // A latent device passing its input through: its track lines up with a dry
    // track to the sample (the engine delays the dry one), and still does when
    // the project is opened again.
    void theirLatencyLinesUpWithADryTrack_data() { kindRows(kLatent); }
    void theirLatencyLinesUpWithADryTrack() {
        QFETCH(QString, kind);
        Effects f;
        constexpr size_t kClick = 1000;
        std::vector<float> click = silence(1.0);
        click[2 * kClick] = click[2 * kClick + 1] = 0.5f;
        const QString wet = f.track(click, QStringLiteral("Wet"));
        const QString dry = f.track(click, QStringLiteral("Dry"));
        const QString device = f.editor().addDevice(wet, kind);
        f.setParams(wet, device, passThrough(kind));
        f.bridge().pollPlugins();  // (the meter timer's: a new latency realigns the tracks)
        QVERIFY(f.bridge().deviceLatency(wet, device) > 0);

        const auto check = [&](const QString& when) {
            f.solo(wet);
            const std::vector<float> wetOut = f.beats(1);
            f.solo(dry);
            const std::vector<float> dryOut = f.beats(1);
            for (int channel = 0; channel < 2; ++channel) {
                const size_t at = loudestFrame(wetOut, channel);
                QVERIFY2(at == kClick,
                         qPrintable(QStringLiteral("%1: the click at %2, not %3").arg(when).arg(at).arg(kClick)));
                QCOMPARE(loudestFrame(dryOut, channel), kClick);
                // (Hi-Quality's filters spread it out, evenly about it.)
                for (size_t d = 1; d < 16; ++d) {
                    QVERIFY(std::abs(wetOut[2 * (kClick - d) + channel] - wetOut[2 * (kClick + d) + channel]) < 1e-4f);
                }
            }
            if (kind != u"saturator") {
                QVERIFY2(maxDifference(wetOut, dryOut) < 1e-6,
                         qPrintable(when + QStringLiteral(": %1 apart").arg(maxDifference(wetOut, dryOut))));
            }
        };
        check(QStringLiteral("made"));
        if (QTest::currentTestFailed()) return;
        QVERIFY(f.waitForSignals());  // (the bridge's own decoding done before the project goes)
        QVERIFY(f.s().saveProjectAs(f.dir.path(QStringLiteral("aligned.gilproj"))));
        f.s().newProject();
        QVERIFY(f.s().openProject(f.dir.path(QStringLiteral("aligned.gilproj"))));
        QVERIFY(f.waitForSignals());
        check(QStringLiteral("opened"));
        if (QTest::currentTestFailed()) return;
        f.bridge().pollPlugins();  // (the meter timer's, as it runs once the project has opened)
        // A shorter latency (Hi-Quality off, the shortest lookahead), and back by undo: realigned each time.
        const QMap<QString, double> optional = latencyOn(kind);
        if (optional.isEmpty()) return;
        for (auto it = optional.begin(); it != optional.end(); ++it) {
            f.editor().setDeviceParam(wet, device, it.key(), 0.0);
        }
        f.bridge().pollPlugins();
        check(QStringLiteral("shorter"));
        if (QTest::currentTestFailed()) return;
        f.stack().undo();
        f.bridge().pollPlugins();
        check(QStringLiteral("undone"));
    }

    // Undo and redo of parameter changes reach the engine: all of them in one
    // step (as a preset loads them), or one (as a knob turns it); and it sounds so.
    void undoAndRedoReachTheEngine_data() { kindRows(kEffects); }
    void undoAndRedoReachTheEngine() {
        QFETCH(QString, kind);
        Effects f;
        std::vector<float> noise = silence(2.0);
        addNoise(noise, 0.0, 2.0, 0.25f, 2);
        const QString track = f.track(noise);
        const QString device = f.editor().addDevice(track, kind);
        const QMap<QString, double> defaults = builtinDevice(kind)->defaults;
        const QMap<QString, double> values = allAway(kind);
        const std::vector<float> before = f.beats(1);

        f.editor().setDeviceParams(track, device, values.keys(), values.values());
        QVERIFY2(f.differ(track, device, values).isEmpty(), qPrintable(f.differ(track, device, values)));
        const std::vector<float> after = f.beats(1);
        QVERIFY(maxDifference(after, before) > 1e-3);
        f.stack().undo();
        QVERIFY2(f.differ(track, device, defaults).isEmpty(), qPrintable(f.differ(track, device, defaults)));
        QVERIFY2(maxDifference(f.beats(1), before) < 1e-6, "undone, it sounds as before");
        f.stack().redo();
        QVERIFY2(f.differ(track, device, values).isEmpty(), qPrintable(f.differ(track, device, values)));
        QVERIFY2(maxDifference(f.beats(1), after) < 1e-6, "redone, it sounds as after");

        const QString param = automatedParam(kind);
        QMap<QString, double> turned = values;
        turned.insert(param, defaults.value(param));
        f.editor().setDeviceParam(track, device, param, defaults.value(param));
        QVERIFY2(f.differ(track, device, turned).isEmpty(), qPrintable(f.differ(track, device, turned)));
        f.stack().undo();
        QVERIFY2(f.differ(track, device, values).isEmpty(), qPrintable(f.differ(track, device, values)));
        f.stack().redo();
        QVERIFY2(f.differ(track, device, turned).isEmpty(), qPrintable(f.differ(track, device, turned)));
    }

    // A sidechain given through the editor keys the device: a muted track heard
    // before its fader, silent for the first half and loud for the second, gates
    // or ducks the steady noise the device hears, as it does once the project is
    // opened again. Without it, its own input keys it.
    void aSidechainKeysTheDevice_data() {
        kindRows({QStringLiteral("gate"), QStringLiteral("multiband"), QStringLiteral("spectral")});
    }
    void aSidechainKeysTheDevice() {
        QFETCH(QString, kind);
        Effects f;
        std::vector<float> steady = silence(2.0);
        addNoise(steady, 0.0, 2.0, 0.25f, 3);
        std::vector<float> key = silence(2.0);
        addNoise(key, 1.0, 2.0, 0.5f, 4);
        const QString heard = f.track(steady, QStringLiteral("Heard"));
        const QString keyTrack = f.track(key, QStringLiteral("Key"));
        f.editor().setTrackParam(keyTrack, TrackField::Mute, 1.0);
        const QString device = f.editor().addDevice(heard, kind);
        const bool gate = kind == u"gate";
        if (gate) {  // shut but for a key over -30 dB
            f.setParams(heard, device, {{QStringLiteral("threshold"), -30.0}, {QStringLiteral("floor"), -75.0},
                                        {QStringLiteral("attack"), 0.1}, {QStringLiteral("release"), 5.0}});
        } else if (kind == u"multiband") {  // every band squashed over -50 dB
            for (const QString& band : {QStringLiteral("low"), QStringLiteral("mid"), QStringLiteral("high")}) {
                f.setParams(heard, device,
                            {{band + QStringLiteral("_above"), -50.0}, {band + QStringLiteral("_above_ratio"), 100.0},
                             {band + QStringLiteral("_attack"), 1.0}, {band + QStringLiteral("_release"), 10.0}});
            }
        } else {  // every bin squashed over -60 dB
            f.setParams(heard, device, {{QStringLiteral("threshold"), -60.0}, {QStringLiteral("ratio"), 20.0},
                                        {QStringLiteral("range"), 48.0}, {QStringLiteral("attack"), 1.0},
                                        {QStringLiteral("release"), 10.0}});
        }
        QVERIFY(f.bridge().hasSidechainInput(heard, device));
        f.editor().setDeviceSidechain(heard, device, Sidechain{keyTrack, kPreFader});
        QCOMPARE(f.project().device(heard, device).sidechain, std::optional<Sidechain>(Sidechain{keyTrack, kPreFader}));
        const auto info = f.engine.processorSidechain(f.processor(heard, device));
        QVERIFY(info);
        QCOMPARE(info->trackId, *f.bridge().engineTrackId(keyTrack));
        QCOMPARE(info->tap, sub::SidechainTap::PreFader);

        // Halves of the render, away from where the key starts.
        const auto first = [](const std::vector<float>& out) { return rmsIn(out, frameAt(0.2), frameAt(0.9)); };
        const auto second = [](const std::vector<float>& out) { return rmsIn(out, frameAt(1.2), frameAt(1.9)); };
        const double input = first(steady);
        const auto checkKeyed = [&](const QString& when) {
            const std::vector<float> keyed = f.beats(2);
            const QString levels =
                QStringLiteral("%1: %2 then %3 of %4").arg(when).arg(first(keyed)).arg(second(keyed)).arg(input);
            if (gate) {
                QVERIFY2(first(keyed) < 0.01 * input && second(keyed) > 0.7 * input, qPrintable(levels));
            } else {
                QVERIFY2(first(keyed) > 0.7 * input && second(keyed) < 0.4 * input, qPrintable(levels));
            }
        };
        checkKeyed(QStringLiteral("made"));
        if (QTest::currentTestFailed()) return;
        // The project keeps it.
        QVERIFY(f.waitForSignals());
        QVERIFY(f.s().saveProjectAs(f.dir.path(QStringLiteral("keyed.gilproj"))));
        f.s().newProject();
        QVERIFY(f.s().openProject(f.dir.path(QStringLiteral("keyed.gilproj"))));
        QVERIFY(f.waitForSignals());
        const auto reopened = f.engine.processorSidechain(f.processor(heard, device));
        QVERIFY(reopened);
        QCOMPARE(reopened->trackId, *f.bridge().engineTrackId(keyTrack));
        checkKeyed(QStringLiteral("opened"));
        if (QTest::currentTestFailed()) return;

        f.editor().setDeviceSidechain(heard, device, std::nullopt);
        QVERIFY(!f.engine.processorSidechain(f.processor(heard, device)));
        const std::vector<float> own = f.beats(2);
        QVERIFY2(std::abs(first(own) / second(own) - 1.0) < 0.2,
                 qPrintable(QStringLiteral("%1 then %2").arg(first(own)).arg(second(own))));
    }

    // Switched off and on again by its automation (as by hand while playing), a
    // device doesn't click: off in the middle of a sound, it fades between its
    // sound and its input. On again in silence, it plays nothing of what it held,
    // and goes on as one that never heard the sound before. Switched by hand, the
    // input passes and it comes back as it was.
    void switchedOffAndOnAgainCleanly_data() { kindRows(kEffects); }
    void switchedOffAndOnAgainCleanly() {
        QFETCH(QString, kind);
        Effects f;
        std::vector<float> sound = silence(4.0);  // a tone over beats 0..1.5 and 3..4
        addSine(sound, 0.0, 1.5, 0.3f, 440.0);
        addSine(sound, 3.0, 4.0, 0.3f, 440.0);
        std::vector<float> later = silence(4.0);  // the tone over beats 3..4 only
        addSine(later, 3.0, 4.0, 0.3f, 440.0);
        const QString track = f.track(sound);
        const std::vector<float> raw = f.beats(4);
        const QString device = f.editor().addDevice(track, kind);
        const std::vector<float> on = f.beats(4);

        // On, off at beat 1, on again at 2.
        const QString key = automation::deviceOnKey(device);
        f.editor().setEnvelope(track, key, {{0.0, 1.0, 0.0}, {1.0, 1.0, 0.0}, {1.0, 0.0, 0.0}, {2.0, 0.0, 0.0},
                                            {2.0, 1.0, 0.0}});
        const std::vector<float> switched = f.beats(4);
        f.play(track, later);
        const std::vector<float> unheard = f.beats(4);

        // Off in the sound: a fade between the device's sound and its input, no sharper than either.
        const size_t from = frameAt(1.0) - 480, to = frameAt(1.0) + 960;
        const double steepest = std::max(maxStep(on, from, to), maxStep(raw, from, to));
        QVERIFY2(maxStep(switched, from, to) <= 1.25 * steepest + 0.005,
                 qPrintable(QStringLiteral("a step of %1, at most %2 without the switch")
                                .arg(maxStep(switched, from, to))
                                .arg(steepest)));
        // On at beat 2, in silence: nothing (until the second tone, as early as the latency lets the
        // device's filters or frames reach it); then as if the first tone had never been.
        const size_t latency = static_cast<size_t>(f.bridge().deviceLatency(track, device));
        const double left = peakIn(switched, frameAt(1.5) + 48, frameAt(3.0) - latency);
        QVERIFY2(left == 0.0, qPrintable(QStringLiteral("%1 after the tone").arg(left)));
        const double apart = maxDifference(switched, unheard, frameAt(2.0), frameAt(4.0));
        QVERIFY2(apart < 1e-6, qPrintable(QStringLiteral("%1 apart").arg(apart)));
        QVERIFY(peakIn(switched, frameAt(3.0)) > 0.05);

        // By hand: off, the input passes; on again, as it was.
        f.play(track, sound);
        f.editor().setEnvelope(track, key, {});
        f.editor().setDeviceEnabled(track, device, false);
        QVERIFY2(maxDifference(f.beats(4), raw) < 1e-6, "off, the input passes");
        f.editor().setDeviceEnabled(track, device, true);
        QVERIFY2(maxDifference(f.beats(4), on) < 1e-6, "on again, as it was");
    }
};

QTEST_GUILESS_MAIN(TestBuiltinEffects)
#include "test_builtin_effects.moc"
