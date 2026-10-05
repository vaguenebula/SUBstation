// Duplicating tracks (Ctrl+D on tracks: new clips and devices, automation and
// routing following the copies, one undo step) and cutting, copying and
// pasting automation (Ctrl+C/X/V on a lane range: onto the lanes selected, or
// those it came from).

#include "EditorFixture.h"
#include "TestSupport.h"

#include <QTest>

#include <cmath>

using namespace sub::app;
using test::EditorFixture;
using test::env;
namespace autom = sub::app::automation;

namespace {

bool near(double a, double b, double tolerance = 1e-9) { return std::abs(a - b) <= tolerance; }

double valueAt(const Envelope& points, double beat) { return autom::valueAt(points, beat).value_or(-1.0); }

QStringList order(const Project& project) {
    QStringList ids;
    for (const Track& t : project.tracks()) ids.append(t.id);
    return ids;
}

QSet<QString> deviceIds(const std::vector<Device>& devices) {
    QSet<QString> ids;
    for (const Device* d : iterDevices(devices)) ids.insert(d->id);
    return ids;
}

}  // namespace

class TestEditorDuplicate : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    // --- Duplicating tracks ---

    void duplicatedTracksAreNewTracksLikeThem() {
        EditorFixture f;
        Project& p = f.project;
        const QString source = f.editor.addAudioTrack();
        const QString track = f.editor.addMidiTrack();
        const ClipRef clip = *f.editor.addMidiClip(track, 0.0, 4.0);
        f.editor.setClipNotes(clip, {Note{60, 0.0, 1.0}}, "Add Note");
        const QString synth = f.track(track).devices[0].id;
        const QString utility = f.editor.addDevice(track, "utility");
        const QString rack = f.editor.groupDevices(track, {utility});
        const QString chain = p.device(track, rack).chains[0].id;
        f.editor.setDeviceSidechain(track, synth, Sidechain{source});
        f.editor.setEnvelope(track, autom::kMixerVolume, env({{0.0, 0.2}, {4.0, 0.8}}));
        f.editor.setEnvelope(track, autom::deviceKey(utility, "gain"), env({{0.0, 0.5}}));
        f.editor.setEnvelope(track, autom::chainKey(rack, chain, "volume"), env({{0.0, 0.3}}));
        f.editor.showAutomation(track, autom::deviceKey(utility, "gain"));
        f.editor.setDevicesFolded(track, {utility}, true);
        p.updateTrack(track, TrackField::Armed, true);
        const QString after = f.editor.addAudioTrack();

        const int steps = f.stack.count();
        const QStringList copies = f.editor.duplicateTracks({track});
        QCOMPARE(copies.size(), 1);
        const Track& copy = f.track(copies[0]);
        QCOMPARE(f.stack.count(), steps + 1);
        QCOMPARE(f.stack.undoText(), QStringLiteral("Duplicate Track"));
        QCOMPARE(order(p), (QStringList{source, track, copy.id, after}));  // right after it
        QVERIFY(copy.id != track && copy.name != f.track(track).name && copy.kind == kMidiKind && !copy.armed);
        QCOMPARE(copy.clips.size(), size_t(1));
        QVERIFY(copy.clips[0].id != clip.clipId);
        QVERIFY(copy.clips[0].notes == p.clip(clip.trackId, clip.clipId).notes);
        const QSet<QString> old = deviceIds(f.track(track).devices);
        const QSet<QString> nw = deviceIds(copy.devices);
        QCOMPARE(nw.size(), old.size());
        QVERIFY(!old.intersects(nw));
        const Device& copiedSynth = copy.devices[0];
        const Device& copiedRack = copy.devices[1];
        const Device& copiedUtility = copiedRack.chains[0].devices[0];
        QVERIFY(copiedSynth.sidechain == Sidechain{source});  // from a track not copied: the same
        QVERIFY(p.isDeviceFolded(copiedUtility.id));
        // Its automation is the copies' devices'.
        const QString gain = autom::deviceKey(copiedUtility.id, "gain");
        const QStringList keys = copy.automation.keys();
        QCOMPARE(QSet<QString>(keys.begin(), keys.end()),
                 (QSet<QString>{autom::kMixerVolume, gain,
                                autom::chainKey(copiedRack.id, copiedRack.chains[0].id, "volume")}));
        QCOMPARE(copy.automationView.key, std::optional<QString>(gain));
        f.stack.undo();
        QCOMPARE(order(p), (QStringList{source, track, after}));
    }

    void duplicatingAGroupCopiesWhatIsInIt() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = f.editor.addAudioTrack();
        const QString b = f.editor.addAudioTrack();
        const QString outside = f.editor.addAudioTrack();
        const QString group = f.editor.groupTracks({a, b});
        f.editor.setTrackInputTrack(b, a);  // (b hears a, in the same group)
        const QString comp = f.editor.addDevice(b, "utility");
        f.editor.setDeviceSidechain(b, comp, Sidechain{a});
        f.editor.setTrackInputTrack(outside, a);
        const QStringList copies = f.editor.duplicateTracks({a, group});  // a goes with its group
        QCOMPARE(copies.size(), 1);
        const auto& tracks = p.tracks();
        QCOMPARE(order(p).mid(0, 3), (QStringList{group, a, b}));
        QCOMPARE(tracks.back().id, outside);
        const Track& groupCopy = tracks[3];
        const Track& aCopy = tracks[4];
        const Track& bCopy = tracks[5];
        QVERIFY(groupCopy.id == copies[0] && groupCopy.isGroup() && !groupCopy.parent);
        QVERIFY(aCopy.parent == groupCopy.id && bCopy.parent == groupCopy.id);
        // Routing among the copied tracks goes among the copies.
        QVERIFY(bCopy.inputTrack == aCopy.id && bCopy.devices[0].sidechain == Sidechain{aCopy.id});
        QVERIFY(f.track(outside).inputTrack == a);
        f.stack.undo();
        QCOMPARE(p.tracks().size(), size_t(4));
    }

    void copiesLetGoOfWhatIsGone() {
        // Pasted later, copies let go of the inputs, sidechains and sends whose tracks are gone.
        EditorFixture f;
        Project& p = f.project;
        const QString source = f.editor.addAudioTrack();
        const QString track = f.editor.addAudioTrack();
        const QString ret = f.editor.addReturnTrack();
        f.editor.setTrackInputTrack(track, source);
        const QString comp = f.editor.addDevice(track, "utility");
        f.editor.setDeviceSidechain(track, comp, Sidechain{source});
        f.editor.setSend(track, ret, -6.0);
        f.editor.setEnvelope(track, autom::sendKey(ret), env({{0.0, 0.5}}));
        const auto copied = f.editor.copyTracks({track});
        f.editor.deleteTracks({source, ret});
        const QStringList pasted = f.editor.pasteTracks(*copied);
        QCOMPARE(pasted.size(), 1);
        const Track& copy = f.track(pasted[0]);
        QCOMPARE(order(p).back(), copy.id);  // (after nothing: last)
        QVERIFY(!copy.inputTrack && !copy.devices[0].sidechain && copy.sends.isEmpty() && copy.automation.isEmpty());
        QCOMPARE(f.stack.undoText(), QStringLiteral("Paste Track"));
    }

    // --- Copying automation ---

    void cutCopyAndPasteAutomation() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = f.editor.addAudioTrack();
        const QString b = f.editor.addAudioTrack();
        f.editor.setEnvelope(a, autom::kMixerPan, env({{0.0, 0.0}, {2.0, 1.0}, {4.0, 0.0}}));
        f.editor.setEnvelope(a, autom::kMixerVolume, env({{0.0, 0.5}}));
        QVERIFY(!f.editor.copyAutomationRange(1.0, 3.0, {{b, autom::kMixerPan}}));  // nothing there

        const auto content = f.editor.copyAutomationRange(1.0, 3.0, {{a, autom::kMixerPan}, {b, autom::kMixerPan}});
        QVERIFY(content);
        QCOMPARE(content->length, 2.0);
        QCOMPARE(content->lanes.size(), size_t(1));
        QVERIFY((content->lanes[0].first == LaneRef{a, autom::kMixerPan}));
        // Onto the lane it came from, later on.
        QCOMPARE(f.editor.pasteAutomation(*content, 6.0), (QList<LaneRef>{{a, autom::kMixerPan}}));
        const Envelope pan = p.envelope(a, autom::kMixerPan);
        QVERIFY(near(valueAt(pan, 7.0), 1.0));
        QVERIFY(near(valueAt(pan, 6.0), 0.5));
        QCOMPARE(f.stack.undoText(), QStringLiteral("Paste Automation"));
        // One lane copied: onto each lane selected.
        f.editor.pasteAutomation(*content, 0.0, {{b, autom::kMixerPan}, {b, autom::kMixerVolume}});
        QVERIFY(near(valueAt(p.envelope(b, autom::kMixerPan), 1.0), 1.0));
        QVERIFY(near(valueAt(p.envelope(b, autom::kMixerVolume), 1.0), 1.0));
        f.stack.undo();
        QVERIFY(p.envelope(b, autom::kMixerPan).empty() && p.envelope(b, autom::kMixerVolume).empty());

        // Two lanes: one to one onto two selected; onto another number, where they came from.
        const auto two = f.editor.copyAutomationRange(0.0, 4.0, {{a, autom::kMixerVolume}, {a, autom::kMixerPan}});
        f.editor.pasteAutomation(*two, 0.0, {{b, autom::kMixerPan}, {b, autom::kMixerVolume}});
        QVERIFY(near(valueAt(p.envelope(b, autom::kMixerPan), 2.0), 0.5));  // a's volume
        QVERIFY(near(valueAt(p.envelope(b, autom::kMixerVolume), 2.0), 1.0));  // a's pan
        const auto targets = f.editor.automationPasteTargets(*two, {{b, autom::kMixerPan}});
        QCOMPARE(targets.size(), 2);
        QVERIFY((targets[0] == LaneRef{a, autom::kMixerVolume} && targets[1] == LaneRef{a, autom::kMixerPan}));

        // Cut: copied, and gone from the range, as one step.
        const int steps = f.stack.count();
        const auto cut = f.editor.cutAutomationRange(1.0, 3.0, {{a, autom::kMixerPan}});
        QVERIFY(cut == content);
        QCOMPARE(f.stack.count(), steps + 1);
        QVERIFY(near(valueAt(p.envelope(a, autom::kMixerPan), 2.0), 0.5));  // straight across

        // A lane whose device is gone takes nothing.
        const QString utility = f.editor.addDevice(a, "utility");
        const QString gain = autom::deviceKey(utility, "gain");
        f.editor.setEnvelope(a, gain, env({{0.0, 0.25}}));
        const auto copied = f.editor.copyAutomationRange(0.0, 1.0, {{a, gain}});
        f.editor.removeDevice(a, utility);
        QVERIFY(f.editor.pasteAutomation(*copied, 0.0).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestEditorDuplicate)
#include "test_editor_duplicate.moc"
