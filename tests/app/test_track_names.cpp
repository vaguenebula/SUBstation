// Track names (model/TrackNames.h): # in a name is the track's number, its
// place from the top, so tracks coming, going and moving renumber those below
// them; new audio tracks are named by the files their clips play, MIDI tracks
// by their instrument, while their name is the one that gives; renaming,
// undoing, duplicating, saving and loading, and what takes are called.

#include "EditorFixture.h"
#include "TestSupport.h"

#include "io/Serialization.h"
#include "model/RecordedTake.h"
#include "model/TrackNames.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

using namespace sub::app;
using test::EditorFixture;

namespace {

QStringList names(const Project& project) {
    QStringList result;
    for (const Track& t : project.tracks()) result.append(t.name);
    return result;
}

// An audio track with clips of these files, one after the other.
QString audioTrack(EditorFixture& f, const QStringList& files) {
    std::vector<std::pair<QString, double>> sources;
    for (const QString& file : files) sources.emplace_back(QStringLiteral("C:/x/") + file, 1.0);
    const ClipRefs refs = f.editor.addClips({}, 0.0, sources, static_cast<int>(f.project.tracks().size()));
    return refs.isEmpty() ? QString() : refs.front().trackId;
}

}  // namespace

class TestTrackNames : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void numberedNames() {
        QCOMPARE(numberedName(QStringLiteral("# Kick"), 3), QStringLiteral("3 Kick"));
        QCOMPARE(numberedName(QStringLiteral("Vox"), 3), QStringLiteral("Vox"));
        QCOMPARE(numberedName(QStringLiteral("#-#"), 12), QStringLiteral("12-12"));
    }

    // An audio track: the file most of its clips play; a tie, the one played first.
    void audioTracksAreNamedByTheirClipsFiles() {
        EditorFixture f;
        const QString empty = f.editor.addAudioTrack();
        QCOMPARE(f.track(empty).name, QStringLiteral("1 Audio"));
        QCOMPARE(f.track(empty).nameTemplate, QStringLiteral("# Audio"));
        const QString drums = audioTrack(f, {"snare.wav", "kick.wav", "kick.wav"});
        QCOMPARE(f.track(drums).name, QStringLiteral("2 kick"));
        const QString tie = audioTrack(f, {"hat.wav", "clap.wav"});
        QCOMPARE(f.track(tie).name, QStringLiteral("3 hat"));
        // Clips added to a track: renamed as they say. Undone: as it was.
        f.editor.addClips(empty, 8.0, {{"C:/x/bass.wav", 1.0}});
        QCOMPARE(f.track(empty).name, QStringLiteral("1 bass"));
        f.stack.undo();
        QCOMPARE(f.track(empty).name, QStringLiteral("1 Audio"));
        f.stack.redo();
        QCOMPARE(f.track(empty).name, QStringLiteral("1 bass"));
        // Its clips gone: "Audio" again.
        f.editor.deleteClips({{empty, f.track(empty).clips.front().id}});
        QCOMPARE(f.track(empty).name, QStringLiteral("1 Audio"));
    }

    // A reversed clip is its original file's; a take is named without its time.
    void reversedClipsAndTakes() {
        EditorFixture f;
        const QString track = f.editor.addAudioTrack();
        Clip reversed = Clip::audio(QStringLiteral("c1"), QStringLiteral("C:/x/kick (reversed).wav"),
                                    QStringLiteral("kick"), 0.0, 1.0, 0.0, 1.0);
        reversed.reversedFrom = QStringLiteral("C:/x/kick.wav");
        f.project.setClips(track, {reversed});
        QCOMPARE(f.track(track).name, QStringLiteral("1 kick"));
        QCOMPARE(takeName(f.track(track)), QStringLiteral("kick"));  // (so its takes keep it so)

        const QString vox = f.editor.addAudioTrack();
        RecordedTake take;
        take.trackId = vox;
        take.path = QStringLiteral("C:/r/Audio 2026-10-08 141500.wav");
        take.durationSec = 1.0;
        RecordedTake again = take;
        again.path = QStringLiteral("C:/r/Audio 2026-10-08 141500 2_.wav");
        again.startSec = 2.0;
        f.editor.addRecordings({take, again});
        QCOMPARE(f.track(vox).name, QStringLiteral("2 Audio"));
        QCOMPARE(takeName(f.track(vox)), QStringLiteral("Audio"));
        f.editor.renameTrack(vox, QStringLiteral("Vox"));
        QCOMPARE(takeName(f.track(vox)), QStringLiteral("Vox"));  // (renamed: its name)
    }

    // A MIDI track: its instrument's name, as it changes.
    void midiTracksAreNamedByTheirInstrument() {
        EditorFixture f;
        const QString track = f.editor.addMidiTrack();
        QCOMPARE(f.track(track).name, QStringLiteral("1 Synth"));
        QCOMPARE(f.track(track).nameTemplate, QStringLiteral("# Synth"));
        const QString synth = f.track(track).devices.front().id;
        const QString rack = f.editor.groupDevices(track, {synth});
        QCOMPARE(f.track(track).name, QStringLiteral("1 Instrument Rack"));
        f.editor.renameRack(track, rack, QStringLiteral("Pads"));
        QCOMPARE(f.track(track).name, QStringLiteral("1 Pads"));
        f.editor.removeDevices(track, {rack});
        QCOMPARE(f.track(track).name, QStringLiteral("1 MIDI"));
        f.stack.undo();
        QCOMPARE(f.track(track).name, QStringLiteral("1 Pads"));
        // An effect doesn't name it; a plug-in instrument does.
        f.editor.addDevice(track, QStringLiteral("utility"));
        QCOMPARE(f.track(track).name, QStringLiteral("1 Pads"));
        PluginRef plugin;
        plugin.uid = QStringLiteral("X");
        plugin.name = QStringLiteral("Operator");
        plugin.instrument = true;
        const QString keys = f.editor.addMidiTrack(-1, {}, {}, plugin);
        QCOMPARE(f.track(keys).name, QStringLiteral("2 Operator"));
        const QString bare = f.editor.addMidiTrack(-1, {}, QString());
        QCOMPARE(f.track(bare).name, QStringLiteral("3 MIDI"));
    }

    // The number is the track's place: every track counts (groups, and what is
    // in them), and those below one coming, going or moving are renumbered.
    void numbersFollowTheTracksPlaces() {
        EditorFixture f;
        const QString a = f.editor.addAudioTrack();
        const QString b = f.editor.addMidiTrack();
        const QString vox = f.editor.addAudioTrack(-1, QStringLiteral("Vox"));  // (no #: no number)
        const QString c = f.editor.addAudioTrack();
        QCOMPARE(names(f.project), (QStringList{"1 Audio", "2 Synth", "Vox", "4 Audio"}));
        QSignalSpy changed(&f.project, &Project::trackChanged);
        f.editor.addAudioTrack(0);
        QCOMPARE(names(f.project), (QStringList{"1 Audio", "2 Audio", "3 Synth", "Vox", "5 Audio"}));
        QCOMPARE(changed.count(), 3);  // (those renumbered, each heard of; not Vox)
        f.stack.undo();
        QCOMPARE(names(f.project), (QStringList{"1 Audio", "2 Synth", "Vox", "4 Audio"}));
        f.editor.deleteTracks({a});
        QCOMPARE(names(f.project), (QStringList{"1 Synth", "Vox", "3 Audio"}));
        f.stack.undo();
        QVERIFY(f.editor.moveTracks({c}, 0, {}));
        QCOMPARE(names(f.project), (QStringList{"1 Audio", "2 Audio", "3 Synth", "Vox"}));
        QCOMPARE(f.project.trackIndex(c), 0);
        f.stack.undo();
        // A group counts, and so do the tracks in it.
        const QString group = f.editor.groupTracks({b});
        QCOMPARE(names(f.project), (QStringList{"1 Audio", "2 Group", "3 Synth", "Vox", "5 Audio"}));
        QCOMPARE(f.track(group).nameTemplate, QStringLiteral("# Group"));
        Q_UNUSED(vox);
    }

    // Renaming: the template ("# Lead": the number stays, by place); a name
    // other than what its contents give stays; their name again follows them.
    void renaming() {
        EditorFixture f;
        f.editor.addAudioTrack();
        const QString track = audioTrack(f, {"kick.wav"});
        QCOMPARE(f.track(track).name, QStringLiteral("2 kick"));
        f.editor.renameTrack(track, QStringLiteral("# Lead"));
        QCOMPARE(f.track(track).name, QStringLiteral("2 Lead"));
        QCOMPARE(f.track(track).value(TrackField::Name), TrackValue(QStringLiteral("# Lead")));
        f.editor.addClips(track, 4.0, {{"C:/x/snare.wav", 1.0}, {"C:/x/snare.wav", 1.0}});
        QCOMPARE(f.track(track).name, QStringLiteral("2 Lead"));  // (renamed: it stays)
        f.editor.addAudioTrack(0);
        QCOMPARE(f.track(track).name, QStringLiteral("3 Lead"));  // (but numbered by its place)
        f.editor.renameTrack(track, QStringLiteral("Lead"));
        QCOMPARE(f.track(track).name, QStringLiteral("Lead"));
        f.stack.undo();
        QCOMPARE(f.track(track).name, QStringLiteral("3 Lead"));
        f.editor.renameTrack(track, QStringLiteral("# snare"));  // (what its clips give: it follows them)
        f.editor.addClips(track, 12.0, {{"C:/x/hat.wav", 1.0}, {"C:/x/hat.wav", 1.0}, {"C:/x/hat.wav", 1.0}});
        QCOMPARE(f.track(track).name, QStringLiteral("3 hat"));
    }

    // Copies: numbered by their place; a name without a # of its own.
    void duplicates() {
        EditorFixture f;
        const QString kick = audioTrack(f, {"kick.wav"});
        const QString vox = f.editor.addAudioTrack(-1, QStringLiteral("Vox"));
        const QStringList copies = f.editor.duplicateTracks({kick, vox});
        QCOMPARE(copies.size(), 2);
        QCOMPARE(names(f.project), (QStringList{"1 kick", "Vox", "3 kick", "Vox 2"}));
        QVERIFY(namedByContents(f.track(copies[0])));
    }

    // Saved as its template; loaded, numbered by its place. A project saved
    // before ("3 Audio", "2 MIDI", "1 Group") is numbered and named so too.
    void savedAndLoaded() {
        EditorFixture f;
        f.editor.addAudioTrack();
        const QString kick = audioTrack(f, {"kick.wav"});
        f.editor.renameTrack(kick, QStringLiteral("# Drums"));
        f.editor.addMidiTrack();
        QJsonObject data = projectToJson(f.project);
        QJsonArray tracks = data.value(QStringLiteral("tracks")).toArray();
        QCOMPARE(tracks.at(1).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("# Drums"));
        Project loaded;
        loadInto(loaded, data);
        QCOMPARE(names(loaded), names(f.project));

        // As an older SUBstation saved it (before version 19).
        data.insert(QStringLiteral("version"), kNameTemplatesVersion - 1);
        const QStringList old{"7 Audio", "8 Audio", "9 MIDI"};
        for (int i = 0; i < 3; ++i) {
            QJsonObject t = tracks.at(i).toObject();
            t.insert(QStringLiteral("name"), old[i]);
            tracks.replace(i, t);
        }
        data.insert(QStringLiteral("tracks"), tracks);
        Project before;
        loadInto(before, data);
        QCOMPARE(names(before), (QStringList{"1 Audio", "2 kick", "3 Synth"}));
        QCOMPARE(before.tracks()[1].nameTemplate, QStringLiteral("# kick"));
        // Anything else stays as it was saved.
        QJsonObject t = tracks.at(0).toObject();
        t.insert(QStringLiteral("name"), QStringLiteral("3 Audio take"));
        tracks.replace(0, t);
        data.insert(QStringLiteral("tracks"), tracks);
        Project other;
        loadInto(other, data);
        QCOMPARE(other.tracks()[0].name, QStringLiteral("3 Audio take"));

        // Saved now, such a name is one typed: it stays.
        f.editor.renameTrack(kick, QStringLiteral("3 Audio"));
        Project now;
        loadInto(now, projectToJson(f.project));
        QCOMPARE(now.tracks()[1].name, QStringLiteral("3 Audio"));
        QCOMPARE(now.tracks()[1].nameTemplate, QStringLiteral("3 Audio"));
    }
};

QTEST_GUILESS_MAIN(TestTrackNames)
#include "test_track_names.moc"
