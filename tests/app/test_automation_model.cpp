// Automation in the model: envelope maths, target keys, the parameter mappings
// held against the engine's, and saving (the parts of
// tests/test_automation_model.py that need neither the editor nor the bridge).

#include "TestSupport.h"

#include "io/Serialization.h"
#include "model/Automation.h"
#include "model/Commands.h"
#include "model/Devices.h"
#include "model/ParamSpec.h"
#include "model/Project.h"

#include <Automation.h>  // the engine's (engine/src/Automation.h)

#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>
#include <QUndoStack>

#include <cmath>

using namespace sub::app;
namespace autom = sub::app::automation;

namespace {

Envelope env(std::initializer_list<AutomationPoint> points) { return Envelope(points); }

bool near(double a, double b, double tolerance = 1e-9) { return std::abs(a - b) <= tolerance; }
bool near(const std::optional<double>& a, double b, double tolerance = 1e-9) { return a && near(*a, b, tolerance); }

std::vector<double> beatsOf(const Envelope& points) {
    std::vector<double> beats;
    for (const AutomationPoint& p : points) beats.push_back(p.beat);
    return beats;
}

}  // namespace

class TestAutomationModel : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { test::prepareApplication(); }

    // --- Envelope maths ---

    void theEngineUsesTheSameCurveAndVolumeLaw() {
        QCOMPARE(static_cast<double>(sub::kAutomationCurvature), autom::kCurvature);
        QVERIFY(near(20 * std::log10(static_cast<double>(sub::kMaxVolumeGain)), autom::kMaxVolumeDb, 1e-5));
        // The engine's curve is the model's, at every bend.
        for (double bend : {-1.0, -0.4, 0.0, 0.3, 1.0}) {
            for (double x : {0.0, 0.1, 0.5, 0.9, 1.0}) {
                QVERIFY(near(autom::shape(x, bend),
                             static_cast<double>(sub::automationShape(static_cast<float>(x), static_cast<float>(bend))),
                             1e-5));
            }
        }
    }

    void valuesBetweenBeforeAndAfterThePoints() {
        const Envelope points = env({{1.0, 0.2}, {3.0, 0.6}, {3.0, 0.9}});  // a ramp, then a step
        QVERIFY(!autom::valueAt({}, 1.0));
        QCOMPARE(autom::valueAt(points, 0.0), std::optional<double>(0.2));  // holds the first value before it
        QVERIFY(near(autom::valueAt(points, 2.0), 0.4));
        QCOMPARE(autom::valueAt(points, 3.0), std::optional<double>(0.9));  // a step: the later point from its beat on
        QVERIFY(near(autom::leftValue(points, 3.0), 0.6));
        QCOMPARE(autom::valueAt(points, 10.0), std::optional<double>(0.9));
    }

    void curvesBulgeUpwardWhicheverWayTheSegmentGoes() {
        QVERIFY(*autom::valueAt(env({{0.0, 0.0, 0.5}, {1.0, 1.0}}), 0.5) > 0.5);
        QVERIFY(*autom::valueAt(env({{0.0, 1.0, 0.5}, {1.0, 0.0}}), 0.5) > 0.5);
        QVERIFY(*autom::valueAt(env({{0.0, 0.0, -0.5}, {1.0, 1.0}}), 0.5) < 0.5);
        QCOMPARE(autom::valueAt(env({{0.0, 0.0, 1.0}, {1.0, 1.0}}), 1.0), std::optional<double>(1.0));
    }

    void splittingKeepsCurvedSegmentsExactly() {
        const Envelope points = env({{0.0, 0.1, 0.7}, {4.0, 0.9, -0.3}, {6.0, 0.2}});
        for (double beat : {0.5, 3.9, 4.0, 5.0}) {
            const auto [split, index] = autom::splitAt(points, beat);
            QCOMPARE(split.size(), size_t(4));
            QCOMPARE(split[index].beat, beat);
            for (int i = 0; i < 56; ++i) {
                const double probe = i / 8.0;
                QVERIFY(near(*autom::valueAt(split, probe), *autom::valueAt(points, probe), 1e-9));
            }
        }
    }

    void addPoint() {
        auto [points, index] = autom::addPoint({}, 2.0, 0.5);
        QVERIFY(points == env({{2.0, 0.5}}) && index == 0);
        std::tie(points, index) = autom::addPoint(points, 1.0, 1.5);
        QVERIFY(index == 0 && points[0] == (AutomationPoint{1.0, 1.0}));  // clamped
        std::tie(points, index) = autom::addPoint(points, 2.0, 0.0);  // a step after the point at the same beat
        QCOMPARE(index, 2);
        QVERIFY(points[0].value == 1.0 && points[1].value == 0.5 && points[2].value == 0.0);
    }

    void movingPointsKeepsTheirOrder() {
        const Envelope points = env({{0.0, 0.5}, {1.0, 0.5}, {2.0, 0.5}, {3.0, 0.5}});
        Envelope moved = autom::movePoints(points, {1}, 5.0, 0.25);
        QVERIFY(moved[1] == (AutomationPoint{2.0, 0.75}));  // stops at its neighbour
        moved = autom::movePoints(points, {1, 2}, -3.0, -1.0);
        QVERIFY((beatsOf(moved) == std::vector<double>{0.0, 0.0, 1.0, 3.0}) && moved[1].value == 0.0);
        QCOMPARE(autom::movePoints(points, {0}, -1.0, 0.0)[0].beat, 0.0);  // not before 0
    }

    void deleteAndCurves() {
        const Envelope points = env({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}});
        QVERIFY(autom::deletePoints(points, {1}) == env({{0.0, 0.0}, {2.0, 0.0}}));
        QCOMPARE(autom::setCurve(points, 0, 2.0)[0].curve, 1.0);
        QCOMPARE(autom::segmentIndex(points, 0.5), std::optional<int>(0));
        QCOMPARE(autom::segmentIndex(points, 1.5), std::optional<int>(1));
        QVERIFY(!autom::segmentIndex(points, 2.5));
    }

    void removeRangeKeepsTheOutside() {
        const Envelope points = env({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}, {3.0, 1.0}, {4.0, 0.0}});
        const Envelope removed = autom::removeRange(points, 0.5, 2.5);
        for (double beat : {0.0, 0.25, 0.5, 2.5, 3.0, 3.5}) {
            QVERIFY(near(*autom::valueAt(removed, beat), *autom::valueAt(points, beat)));
        }
        QVERIFY(near(autom::valueAt(removed, 1.5), 0.5));  // straight across: 0.5 to 0.5
        QVERIFY(autom::removeRange(points, -1.0, 5.0).empty());  // every point: no envelope left
        QVERIFY(autom::removeRange(points, 0.2, 0.8) == points);  // no point in it: nothing to delete
    }

    void duplicateRange() {
        const Envelope points = env({{0.0, 0.0}, {2.0, 1.0}, {4.0, 0.0}});
        const Envelope doubled = autom::pasteRange(points, autom::copyRange(points, 0.0, 2.0), 2.0, 2.0);
        for (double beat : {0.0, 1.0, 1.5}) {
            QVERIFY(near(*autom::valueAt(doubled, beat + 2.0), *autom::valueAt(points, beat)));
            QVERIFY(near(*autom::valueAt(doubled, beat), *autom::valueAt(points, beat)));
        }
        QVERIFY(near(autom::valueAt(doubled, 5.0), 0.0));
    }

    void moveRange() {
        const Envelope ramp = env({{0.0, 0.0}, {8.0, 1.0}});
        const Envelope up = autom::moveRange(ramp, 2.0, 4.0, 0.0, 0.5);
        QVERIFY(near(autom::valueAt(up, 1.0), 0.125) && near(autom::valueAt(up, 5.0), 0.625));
        QVERIFY(near(autom::valueAt(up, 3.0), 0.875));
        QVERIFY(near(autom::valueAt(up, 2.0), 0.75) && near(autom::leftValue(up, 2.0), 0.25));  // a step
        QVERIFY(autom::moveRange(ramp, 2.0, 4.0, 0.0, 0.0) == ramp);  // no stray points
        const Envelope later = autom::moveRange(ramp, 2.0, 4.0, 4.0, 0.0);
        QVERIFY(near(autom::valueAt(later, 7.0), 0.375) && near(autom::valueAt(later, 1.0), 0.125));
        QCOMPARE(autom::moveRange(ramp, 2.0, 4.0, -10.0, 0.0)[0].beat, 0.0);  // not before the start
        QCOMPARE(autom::valueAt(autom::moveRange(ramp, 2.0, 4.0, 0.0, 2.0), 3.0), std::optional<double>(1.0));
    }

    void movingARangeKeepsTheEnvelopeOutsideIt() {
        // Every breakpoint is inside the range: the outside still holds its values.
        const Envelope points = env({{1.5, 0.3}, {4.0, 0.7}});
        const Envelope moved = autom::moveRange(points, 1.0, 4.0, 2.0, -0.2);
        QVERIFY(near(autom::valueAt(moved, 0.5), 0.3) && near(autom::valueAt(moved, 7.0), 0.7));
        QVERIFY(near(autom::valueAt(moved, 3.5), 0.1) && near(autom::valueAt(moved, 5.99), 0.5, 0.01));
        // Moved over breakpoints, it replaces them.
        const Envelope busy = env({{0.0, 0.5}, {1.0, 0.5}, {5.0, 0.9}, {5.5, 0.1}, {6.0, 0.9}, {8.0, 0.5}});
        const Envelope over = autom::moveRange(busy, 0.0, 2.0, 5.0, 0.0);
        QVERIFY(near(autom::valueAt(over, 5.5), 0.5) && near(autom::valueAt(over, 6.0), 0.5));
        for (const AutomationPoint& p : over) {
            if (p.beat > 5.0 && p.beat < 7.0) QVERIFY(p.value == 0.5 || near(p.value, 0.6));  // its own, not the 0.9s and 0.1
        }
    }

    void movingSeveralPointsOverridesWhereTheyLand() {
        const Envelope points = env({{0.0, 0.5}, {2.0, 0.2}, {3.0, 0.9}, {4.0, 0.3}, {6.0, 0.5}});
        auto [moved, where] = autom::movePointsMapped(points, {1, 2}, 1.5, 0.0);
        QVERIFY((beatsOf(moved) == std::vector<double>{0.0, 3.5, 4.5, 6.0}));
        QVERIFY((where == QMap<int, int>{{1, 1}, {2, 2}}));
        std::tie(moved, where) = autom::movePointsMapped(points, {1, 2}, 3.0, 0.0);
        QVERIFY((beatsOf(moved) == std::vector<double>{0.0, 4.0, 5.0, 6.0, 6.0}));
        QVERIFY((where == QMap<int, int>{{1, 2}, {2, 3}}));  // edges stay
        std::tie(moved, where) = autom::movePointsMapped(points, {1, 2}, -2.0, 0.0);
        QVERIFY(moved[0] == (AutomationPoint{0.0, 0.5}) && moved[1] == (AutomationPoint{0.0, 0.2}) &&
                moved[2] == (AutomationPoint{1.0, 0.9}));
        QVERIFY((where == QMap<int, int>{{1, 1}, {2, 2}}));
        // One point still stops at its neighbours.
        QVERIFY((beatsOf(autom::movePoints(points, {1}, 5.0, 0.0)) == std::vector<double>{0.0, 3.0, 3.0, 4.0, 6.0}));
    }

    void dropRedundantAndSpans() {
        const Envelope flat = env({{0.0, 0.5}, {1.0, 0.5}, {2.0, 0.5}, {3.0, 0.8}});
        QVERIFY(autom::dropRedundant(flat, {1.0}) == env({{0.0, 0.5}, {2.0, 0.5}, {3.0, 0.8}}));
        const Envelope line = env({{0.0, 0.0}, {1.0, 0.5}, {2.0, 1.0}});
        QVERIFY(autom::dropRedundant(line, {1.0}) == env({{0.0, 0.0}, {2.0, 1.0}}));
        QVERIFY(autom::dropRedundant(line, {2.0}) == line);  // the last point changes where it ends
        using Spans = std::vector<std::pair<double, double>>;
        QVERIFY((autom::mergeSpans({{4.0, 5.0}, {0.0, 1.0}, {1.0, 2.0}, {4.5, 6.0}}) == Spans{{0.0, 2.0}, {4.0, 6.0}}));
        QVERIFY(autom::shift(env({{1.0, 0.5}, {2.0, 0.6}}), -1.5) == env({{0.0, 0.5}, {0.5, 0.6}}));
        QVERIFY(autom::normalize(env({{2.0, 2.0, 3.0}, {-1.0, -1.0}})) == env({{0.0, 0.0}, {2.0, 1.0, 1.0}}));
        QCOMPARE(autom::sample(env({{0.0, 0.0}, {2.0, 1.0}}), 0.0, 2.0, 3).size(), size_t(3));
    }

    void keys() {
        const QString key = autom::deviceKey(QStringLiteral("abc"), QStringLiteral("7:x"));
        QVERIFY((autom::parseKey(key) == autom::Target{QStringLiteral("device"), QStringLiteral("abc"), QStringLiteral("7:x")}));
        QCOMPARE(autom::keyDevice(key), std::optional<QString>(QStringLiteral("abc")));
        QVERIFY((autom::parseKey(autom::kMixerVolume) == autom::Target{QStringLiteral("mixer"), QStringLiteral("volume"), {}}));
        QVERIFY(!autom::keyDevice(autom::kMixerPan));
        QVERIFY(!autom::isKey(QStringLiteral("mixer:bogus")) && !autom::isKey(QStringLiteral("nothing")));
        QCOMPARE(autom::keySend(autom::sendKey(QStringLiteral("r1"))), std::optional<QString>(QStringLiteral("r1")));
        QVERIFY(autom::isMixerKey(autom::sendKey(QStringLiteral("r1"))) && autom::isMixerKey(autom::kMixerPan));
        QVERIFY(!autom::isMixerKey(key) && !autom::keySend(QStringLiteral("send:")));
        const QString fader = autom::chainKey(QStringLiteral("rack"), QStringLiteral("c1"), autom::kChainPan);
        QCOMPARE(fader, QStringLiteral("device:rack:chain:c1:pan"));
        QCOMPARE(autom::keyChain(fader), std::optional<QString>(QStringLiteral("c1")));
        QVERIFY((autom::keyChainControl(fader) == autom::ChainControl{QStringLiteral("c1"), autom::kChainPan}));
        QCOMPARE(autom::keyDevice(fader), std::optional<QString>(QStringLiteral("rack")));
        QVERIFY(!autom::keyChain(QStringLiteral("device:rack:chain:c1:width")) && !autom::keyChain(key));
    }

    // --- Parameters ---

    void mixerMappings() {
        const auto specs = mixerSpecs();
        const ParamSpec& volume = specs[0];
        const ParamSpec& pan = specs[1];
        QCOMPARE(volume.name, QStringLiteral("Track Volume"));
        QCOMPARE(mixerSpecs(true)[0].name, QStringLiteral("Master Volume"));
        QVERIFY(near(volume.toNormalized(6.0), 1.0));
        QCOMPARE(volume.toNormalized(-70.0), 0.0);
        for (double db : {-40.0, -12.0, 0.0, 3.0}) QVERIFY(near(volume.fromNormalized(volume.toNormalized(db)), db, 1e-9));
        // The engine's gain for the lane's value is the fader's gain.
        QVERIFY(near(static_cast<double>(sub::automationVolumeGain(static_cast<float>(volume.toNormalized(-12.0)))),
                     std::pow(10.0, -12.0 / 20.0), 1e-6));
        QCOMPARE(volume.format(0.0), QStringLiteral("0.0 dB"));
        QCOMPARE(volume.formatNormalized(0.0), QStringLiteral("-inf dB"));
        QCOMPARE(pan.toNormalized(-1.0), 0.0);
        QCOMPARE(pan.fromNormalized(0.75), 0.5);
        QCOMPARE(pan.format(0.5), QStringLiteral("25R"));
        const auto withSends = mixerSpecs(false, {{QStringLiteral("r1"), QStringLiteral("A")}});
        QCOMPARE(withSends.size(), size_t(3));
        QCOMPARE(withSends[2].key, autom::sendKey(QStringLiteral("r1")));
        QCOMPARE(withSends[2].name, QStringLiteral("Send A"));
        QCOMPARE(withSends[2].defaultValue, autom::kMinVolumeDb);
        const auto faders = chainSpecs(QStringLiteral("rack"), {{QStringLiteral("c1"), QStringLiteral("Wet")}},
                                       QStringLiteral("Audio Effect Rack"));
        QCOMPARE(faders.size(), size_t(2));
        QCOMPARE(faders[0].name, QStringLiteral("Wet Volume"));
        QCOMPARE(faders[1].key, autom::chainKey(QStringLiteral("rack"), QStringLiteral("c1"), autom::kChainPan));
    }

    void deviceParametersMapAsTheEngineDoes() {
        const BuiltinDevice* synth = builtinDevice(QStringLiteral("synth"));
        QVERIFY(synth != nullptr);
        QVERIFY(!synth->info->params.empty());
        for (const sub::ParamInfo& info : synth->info->params) {
            const ParamSpec spec = ParamSpec::fromInfo(
                info, autom::deviceKey(QStringLiteral("d"), QString::fromStdString(info.id)), QStringLiteral("Synth"));
            for (double value : {0.0, 0.1, 0.33, 0.5, 0.74, 0.76, 1.0}) {
                const double engine = info.fromNormalized(static_cast<float>(value));
                QVERIFY2(near(spec.fromNormalized(value), engine, std::max(1e-5, std::abs(engine) * 1e-5)),
                         info.id.c_str());
            }
            QVERIFY(near(spec.toNormalized(info.defaultValue), info.toNormalized(info.defaultValue), 1e-6));
        }
        // Every built-in device's parameters map so.
        for (const BuiltinDevice& device : builtinDevices()) {
            for (const sub::ParamInfo& info : device.info->params) {
                const ParamSpec spec = ParamSpec::fromInfo(info, QStringLiteral("k"), device.name);
                for (double value : {0.0, 0.25, 0.5, 0.9, 1.0}) {
                    const double engine = info.fromNormalized(static_cast<float>(value));
                    QVERIFY2(near(spec.fromNormalized(value), engine, std::max(1e-4, std::abs(engine) * 1e-5)),
                             info.id.c_str());
                    QVERIFY(near(spec.toNormalized(engine), info.toNormalized(static_cast<float>(engine)), 1e-4));
                }
            }
        }
        const ParamSpec wave = ParamSpec::fromInfo(synth->info->params[0], QStringLiteral("w"), QStringLiteral("Synth"));
        QVERIFY(wave.discrete());
        QCOMPARE(wave.format(2.0), QStringLiteral("Saw"));
        QVERIFY(near(wave.quantize(0.6), 2.0 / 3.0));
    }

    void valuesShowInTheirUnits() {
        QCOMPARE(formatValue(-3.04, QStringLiteral("dB")), QStringLiteral("-3.0 dB"));
        QCOMPARE(formatValue(50.0, QStringLiteral("%")), QStringLiteral("50 %"));
        QCOMPARE(formatValue(12.5, QStringLiteral("%")), QStringLiteral("12 %"));  // half to even
        QCOMPARE(formatValue(0.25, QString()), QStringLiteral("+0.25"));
        QCOMPARE(formatValue(0.0, QString()), QStringLiteral("0.00"));
        QCOMPARE(formatValue(4.0, QStringLiteral(":1")), QStringLiteral("4.0:1"));
        QCOMPARE(formatValue(60.0, QStringLiteral("note")), QStringLiteral("C3"));
        QCOMPARE(formatValue(-2.0, QStringLiteral("st")), QStringLiteral("-2 st"));
        QCOMPARE(formatValue(3.0, QStringLiteral("ct")), QStringLiteral("+3 ct"));
        QCOMPARE(formatValue(0.2, QStringLiteral("st")), QStringLiteral("0 st"));
        QCOMPARE(formatValue(2500.0, QStringLiteral("Hz")), QStringLiteral("2.50 kHz"));
        QCOMPARE(formatValue(5.0, QStringLiteral("Hz")), QStringLiteral("5.00 Hz"));
        QCOMPARE(formatValue(440.0, QStringLiteral("Hz")), QStringLiteral("440 Hz"));
        QCOMPARE(formatValue(1500.0, QStringLiteral("ms")), QStringLiteral("1.50 s"));
        QCOMPARE(formatValue(2.5, QStringLiteral("ms")), QStringLiteral("2.5 ms"));
        QCOMPARE(formatValue(120.0, QStringLiteral("ms")), QStringLiteral("120 ms"));
        QCOMPARE(formatValue(1.5, QStringLiteral("x")), QStringLiteral("1.50 x"));
    }

    // --- Saving (test_save_and_load, with the changes made through commands) ---

    void saveAndLoad() {
        Project project;
        QUndoStack stack;
        Track keys = test::makeTrack(QStringLiteral("t1"), QStringLiteral("1 MIDI"), kMidiKind);
        keys.devices = {newDevice(QStringLiteral("synth"))};
        const QString deviceKey = autom::deviceKey(keys.devices[0].id, QStringLiteral("cutoff"));
        stack.push(new InsertTrackCommand(&project, keys, 0));
        stack.push(new SetEnvelopeCommand(&project, QStringLiteral("t1"), deviceKey, {},
                                          env({{0.0, 0.2, 0.3}, {2.0, 0.9}}), QStringLiteral("Change Automation")));
        stack.push(new SetEnvelopeCommand(&project, kMaster, autom::kMixerVolume, {}, env({{1.0, 0.5}}),
                                          QStringLiteral("Change Automation")));
        stack.push(new UpdateTrackCommand(&project, kMaster, TrackField::Pan, 0.0, -0.25,
                                          QStringLiteral("Change Master Pan")));
        project.setAutomationView(QStringLiteral("t1"), AutomationView{true, deviceKey, {autom::kMixerVolume}});
        project.updateSettings({{SettingsField::AutomationLocked, true}});
        QJsonObject data =
            QJsonDocument::fromJson(QJsonDocument(projectToJson(project)).toJson()).object();  // (through the text)
        QCOMPARE(data[QStringLiteral("version")].toInt(), kProjectVersion);
        QJsonArray tracks = data[QStringLiteral("tracks")].toArray();
        QJsonObject track = tracks[0].toObject();
        QJsonObject automation = track[QStringLiteral("automation")].toObject();
        automation[QStringLiteral("mixer:unknown")] = QJsonArray{QJsonArray{0, 1, 0}};  // from a later version: dropped
        track[QStringLiteral("automation")] = automation;
        tracks[0] = track;
        data[QStringLiteral("tracks")] = tracks;
        Project loaded;
        loadInto(loaded, data);
        const Track& restored = loaded.tracks()[0];
        QVERIFY(restored.automation == project.track(QStringLiteral("t1")).automation);
        QVERIFY(restored.automationView == project.track(QStringLiteral("t1")).automationView);
        QVERIFY((loaded.master().automation == EnvelopeMap{{autom::kMixerVolume, env({{1.0, 0.5}})}}));
        QCOMPARE(loaded.master().pan, -0.25);
        QVERIFY(loaded.automationLocked());
        // Projects from before automation load without any.
        track.remove(QStringLiteral("automation"));
        track.remove(QStringLiteral("automation_view"));
        tracks[0] = track;
        data[QStringLiteral("tracks")] = tracks;
        QJsonObject master = data[QStringLiteral("master")].toObject();
        master.remove(QStringLiteral("pan"));
        data[QStringLiteral("master")] = master;
        data.remove(QStringLiteral("automation_locked"));
        loadInto(loaded, data);
        QVERIFY(loaded.tracks()[0].automation.isEmpty() && loaded.master().pan == 0.0 && !loaded.automationLocked());
        QVERIFY(loaded.tracks()[0].automationView == AutomationView{});
    }

    void envelopesKeepTheOrderTheyWereFirstSetIn() {
        // (The first automated target is the one a lane shows first.)
        Project project;
        project.insertTrack(test::makeTrack(QStringLiteral("t"), QStringLiteral("T")), 0);
        const QString device = autom::deviceKey(QStringLiteral("d"), QStringLiteral("gain"));
        project.setEnvelope(QStringLiteral("t"), autom::kMixerPan, env({{0.0, 0.5}}));
        project.setEnvelope(QStringLiteral("t"), device, env({{0.0, 0.5}}));
        project.setEnvelope(QStringLiteral("t"), autom::kMixerPan, env({{1.0, 0.5}}));  // stays first
        QCOMPARE(project.automation(QStringLiteral("t")).keys(), (QList<QString>{autom::kMixerPan, device}));
        project.setEnvelope(QStringLiteral("t"), autom::kMixerPan, {});  // empty: no automation
        QCOMPARE(project.automation(QStringLiteral("t")).keys(), (QList<QString>{device}));
        QVERIFY(project.envelope(QStringLiteral("t"), autom::kMixerPan).empty());
    }
};

QTEST_GUILESS_MAIN(TestAutomationModel)
#include "test_automation_model.moc"
