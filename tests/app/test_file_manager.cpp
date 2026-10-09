// The File Manager and hot swaps: the files a project plays (its audio clips
// and samplers, in racks too; one file however its path is written, in another
// case too on Windows), one file put in place of another (whole files
// and stretches of them, a clip running into the next one trimmed, frozen
// tracks refused, a hot swap's tries one undo step that goes when it is back
// where it began), missing files found again (by name and folders, moving
// along with one found, the search in the background, Locate), the File
// Manager's list and actions, and hot-swapping from the browser through the
// session (started on a clip: every clip and sampler playing its file, the
// browser listing similar sounds; a file the user chooses swaps, the browser's
// selection changing by itself doesn't, a double-click keeps it and ends, any
// other edit or an undo ends it).

#include "EditorFixture.h"
#include "SessionFixture.h"
#include "TestSupport.h"

#include "browser/BrowserController.h"
#include "browser/FileIndex.h"
#include "files/FileManager.h"
#include "files/HotSwap.h"
#include "files/MissingFiles.h"
#include "files/ProjectFiles.h"
#include "model/DeviceState.h"
#include "model/Devices.h"
#include "model/Edits.h"
#include "model/Paths.h"
#include "platform/Paths.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>

#include <cmath>

using namespace sub::app;
using sub::app::test::audioClip;
using sub::app::test::EditorFixture;
using sub::app::test::SessionFixture;
using sub::app::test::TempDir;

namespace {

bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

// A WAV file of `seconds` of a quiet tone at `path` (its folders made).
QString wav(const QString& path, double seconds) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    const std::vector<float> samples(static_cast<size_t>(seconds * test::kSampleRate), 0.25f);
    return test::writeWav(path, samples);
}

Device samplerWith(const QString& path) {
    Device sampler = newDevice(QStringLiteral("sampler"));
    sampler.state = withDeviceFile(std::nullopt, path);
    return sampler;
}

QString clipPath(const Project& project, const QString& trackId, const QString& clipId) {
    return project.clip(trackId, clipId).path;
}

const QString kKick = QStringLiteral("D:/Samples/Drums/Kick.wav");
const QString kSnare = QStringLiteral("D:/Samples/Drums/Snare.wav");

}  // namespace

class TestFileManager : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }
    void init() { QSettings().clear(); }

    // --- Clips with another file -------------------------------------------------------------

    void aReplacedClipKeepsItsPlaceAndSettings() {
        Clip whole = Clip::audio(QStringLiteral("c"), kKick, QStringLiteral("Kick"), 4.0, 0.5, 0.0, 0.5);
        whole.gainDb = -3.0;
        whole.transpose = 2;
        whole.pan = 0.25;
        whole.fadeOutSec = 0.1;
        whole.muted = true;
        // A clip playing all of its file plays all of the new one.
        const Clip replaced = edits::replaceFile(whole, QStringLiteral("D:/Samples/Drums/808.wav"), 0.8);
        QCOMPARE(replaced.id, QStringLiteral("c"));
        QCOMPARE(replaced.path, QStringLiteral("D:/Samples/Drums/808.wav"));
        QCOMPARE(replaced.name, QStringLiteral("808"));
        QCOMPARE(replaced.startBeat, 4.0);
        QVERIFY(near(replaced.offsetSec, 0.0) && near(replaced.durationSec, 0.8) && near(replaced.sourceDurationSec, 0.8));
        QCOMPARE(replaced.gainDb, -3.0);
        QCOMPARE(replaced.transpose, 2);
        QCOMPARE(replaced.pan, 0.25);
        QCOMPARE(replaced.fadeOutSec, 0.1);
        QVERIFY(replaced.muted);
        QVERIFY(edits::playsWholeFile(replaced));

        // A stretch of a file: the same stretch, as far as the new file goes.
        const Clip part = Clip::audio(QStringLiteral("p"), QStringLiteral("loop.wav"), QStringLiteral("loop"), 0.0, 1.0,
                                      2.0, 8.0);
        QVERIFY(!edits::playsWholeFile(part));
        Clip other = edits::replaceFile(part, QStringLiteral("other.wav"), 4.0);
        QVERIFY(near(other.offsetSec, 2.0) && near(other.durationSec, 1.0) && near(other.sourceDurationSec, 4.0));
        other = edits::replaceFile(part, QStringLiteral("short.wav"), 2.5);
        QVERIFY(near(other.offsetSec, 2.0) && near(other.durationSec, 0.5));
        other = edits::replaceFile(part, QStringLiteral("tiny.wav"), 1.5);  // (not that far: from its start)
        QVERIFY(near(other.offsetSec, 0.0) && near(other.durationSec, 1.0));

        // Fades are held to the new length; a reversed clip plays the new file forwards.
        Clip faded = whole;
        faded.fadeInSec = 0.3;
        faded.fadeOutSec = 0.2;
        const Clip shorter = edits::replaceFile(faded, QStringLiteral("x.wav"), 0.25);
        QVERIFY(shorter.fadeInSec + shorter.fadeOutSec <= 0.25 + 1e-9);
        const Clip reversed = edits::reverseClip(whole, QStringLiteral("D:/Project/Reversed/Kick R.wav"), 0.5);
        QCOMPARE(reversed.reversedFrom, kKick);
        QVERIFY(edits::replaceFile(reversed, QStringLiteral("x.wav"), 1.0).reversedFrom.isEmpty());
        // A MIDI clip has no file.
        const Clip midi = Clip::midi(QStringLiteral("m"), {}, 0.0, 4.0);
        QCOMPARE(edits::replaceFile(midi, QStringLiteral("x.wav"), 1.0), midi);
    }

    void aRelinkedClipFollowsItsFile() {
        const Clip clip = Clip::audio(QStringLiteral("c"), kKick, QStringLiteral("Kick"), 2.0, 0.5, 0.1, 0.7);
        Clip moved = edits::relinkFile(clip, kKick, QStringLiteral("E:/Samples/Drums/Kick.wav"));
        QCOMPARE(moved.path, QStringLiteral("E:/Samples/Drums/Kick.wav"));
        moved.path = clip.path;
        QCOMPARE(moved, clip);  // (nothing else changes)
        QCOMPARE(edits::relinkFile(clip, kSnare, QStringLiteral("E:/Snare.wav")), clip);
        // What a reversed clip was reversed from follows too.
        const Clip reversed = edits::reverseClip(clip, QStringLiteral("D:/Project/Reversed/Kick R.wav"), 0.7);
        const Clip relinked = edits::relinkFile(reversed, kKick, QStringLiteral("E:/Samples/Drums/Kick.wav"));
        QCOMPARE(relinked.path, reversed.path);
        QCOMPARE(relinked.reversedFrom, QStringLiteral("E:/Samples/Drums/Kick.wav"));
    }

    void aFileIsTheSameFileHoweverItsPathIsWritten() {
        QVERIFY(samePath(QStringLiteral("/a/b/../c.wav"), QStringLiteral("/a/./c.wav")));
        QVERIFY(samePath(QStringLiteral("/a//c.wav"), QStringLiteral("/a/c.wav")));
        QVERIFY(!samePath(QStringLiteral("/a/c.wav"), QStringLiteral("/b/c.wav")));
        // A relative path is where it is from the current folder.
        QCOMPARE(absoluteCleanPath(QStringLiteral("x/../c.wav")), QDir::current().absoluteFilePath(QStringLiteral("c.wav")));
        QVERIFY(samePath(QStringLiteral("c.wav"), QDir::current().absoluteFilePath(QStringLiteral("c.wav"))));
        // Names in another case: the same file where the system ignores case (Windows), another elsewhere.
        QCOMPARE(samePath(QStringLiteral("/a/Kick.wav"), QStringLiteral("/A/KICK.wav")), !sub::platform::kCaseSensitivePaths);
        QCOMPARE(pathIdentity(QStringLiteral("/a/Kick.wav")) == pathIdentity(QStringLiteral("/a/kick.wav")),
                 !sub::platform::kCaseSensitivePaths);
    }

    // --- Finding missing files ---------------------------------------------------------------

    void missingFilesAreMatchedByNameAndFolders() {
        QCOMPARE(missing::sharedTail(kKick, QStringLiteral("E:/Samples/Drums/Kick.wav")), 3);
        QCOMPARE(missing::sharedTail(QStringLiteral("D:/a/Kick.wav"), QStringLiteral("E:/b/kick.WAV")), 1);
        QCOMPARE(missing::sharedTail(QStringLiteral("D:/a/Kick.wav"), QStringLiteral("D:/a/Snare.wav")), 0);
        QCOMPARE(missing::bestMatch(kKick, {QStringLiteral("C:/Downloads/Kick.wav"),
                                            QStringLiteral("E:/Backup/Samples/Drums/Kick.wav"),
                                            QStringLiteral("E:/Other/Drums/Kick.wav")}),
                 QStringLiteral("E:/Backup/Samples/Drums/Kick.wav"));
        QCOMPARE(missing::bestMatch(QStringLiteral("D:/x/Kick.wav"),
                                    {QStringLiteral("C:/a/Kick.wav"), QStringLiteral("C:/b/Kick.wav")}),
                 QStringLiteral("C:/a/Kick.wav"));  // (a tie: the first)
        QCOMPARE(missing::bestMatch(QStringLiteral("D:/x/Kick.wav"), {QStringLiteral("C:/a/Snare.wav")}), QString());
    }

    void oneFileFoundTellsWhereTheOthersWent() {
        const QSet<QString> there{QStringLiteral("E:/Samples/Bass/Sub.wav"), QStringLiteral("E:/Samples/Drums/Snare.wav"),
                                  QStringLiteral("F:/New/Hat.wav")};
        const auto exists = [&](const QString& path) { return there.contains(path); };
        const QStringList missing{kSnare, QStringLiteral("D:/Samples/Bass/Sub.wav"),
                                  QStringLiteral("D:/Samples/Keys/Rhodes.wav"), QStringLiteral("C:/Else/Sub.wav")};
        const QMap<QString, QString> moved =
            missing::moveAlong(kKick, QStringLiteral("E:/Samples/Drums/Kick.wav"), missing, exists);
        const QMap<QString, QString> expected{{kSnare, QStringLiteral("E:/Samples/Drums/Snare.wav")},
                                              {QStringLiteral("D:/Samples/Bass/Sub.wav"), QStringLiteral("E:/Samples/Bass/Sub.wav")}};
        QCOMPARE(moved, expected);
        // A folder renamed: its files went with it.
        QCOMPARE(missing::moveAlong(QStringLiteral("D:/Old/Kick.wav"), QStringLiteral("F:/New/Kick.wav"),
                                    {QStringLiteral("D:/Old/Hat.wav")}, exists),
                 (QMap<QString, QString>{{QStringLiteral("D:/Old/Hat.wav"), QStringLiteral("F:/New/Hat.wav")}}));
        // Nothing moved (the same folder): nothing to go by.
        QVERIFY(missing::moveAlong(kKick, kKick, missing, [](const QString&) { return true; }).isEmpty());

        // Matching: the best by name, then where the others went (sharing more folders wins).
        QMultiHash<QString, QString> found;
        found.insert(QStringLiteral("kick.wav"), QStringLiteral("C:/Downloads/Kick.wav"));
        found.insert(QStringLiteral("kick.wav"), QStringLiteral("E:/Samples/Drums/Kick.wav"));
        found.insert(QStringLiteral("hat.wav"), QStringLiteral("C:/Downloads/Hat.wav"));
        const QSet<QString> moreThere{QStringLiteral("E:/Samples/Drums/Snare.wav"), QStringLiteral("E:/Samples/Drums/Hat.wav")};
        const QMap<QString, QString> matched =
            missing::match({kKick, QStringLiteral("D:/Samples/Drums/Hat.wav"), kSnare}, found,
                           [&](const QString& path) { return moreThere.contains(path); });
        const QMap<QString, QString> wanted{{kKick, QStringLiteral("E:/Samples/Drums/Kick.wav")},
                                            {QStringLiteral("D:/Samples/Drums/Hat.wav"), QStringLiteral("E:/Samples/Drums/Hat.wav")},
                                            {kSnare, QStringLiteral("E:/Samples/Drums/Snare.wav")}};
        QCOMPARE(matched, wanted);
    }

    // --- The project's files -----------------------------------------------------------------

    void theProjectsFilesAreItsClipsAndSamplers() {
        EditorFixture f;
        const QString a = f.editor.addAudioTrack();
        const QString b = f.editor.addAudioTrack();
        f.editor.commitClips(QStringLiteral("Add"), {{a, {audioClip(QStringLiteral("a1"), 0.0, 0.5, kKick),
                                                         audioClip(QStringLiteral("a2"), 4.0, 0.5, kKick),
                                                         audioClip(QStringLiteral("a3"), 8.0, 1.0, kSnare)}},
                                                    {b, {audioClip(QStringLiteral("b1"), 0.0, 0.5, kKick)}}});
        // A sampler playing it too, in a rack.
        const Device rack = newRack({newChain(QStringLiteral("Chain"), {samplerWith(kKick)})});
        const QString sampler = rack.chains.front().devices.front().id;
        const QString m = f.editor.addMidiTrackWith(rack);
        QVERIFY(!m.isEmpty());
        QCOMPARE(deviceFile(f.project.device(m, sampler)), kKick);

        const std::vector<ProjectFile> files = projectFiles(f.project);
        QCOMPARE(files.size(), size_t{2});
        QCOMPARE(files[0].path, kKick);
        QCOMPARE(files[0].uses.clips, (ClipRefs{{a, QStringLiteral("a1")}, {a, QStringLiteral("a2")}, {b, QStringLiteral("b1")}}));
        QCOMPARE(files[0].uses.devices, (QList<DeviceRef>{{m, sampler}}));
        QCOMPARE(files[1].path, kSnare);
        QCOMPARE(fileUses(f.project, kKick), files[0].uses);
        QCOMPARE(usesText(f.project, files[0].uses),
                 QStringLiteral("3 clips, %1").arg(deviceName(f.project.device(m, sampler))));
        QCOMPARE(usesText(f.project, files[1].uses), QStringLiteral("1 clip"));
#ifdef Q_OS_WIN
        // Paths as Windows compares them: one file however it is spelt.
        QCOMPARE(fileUses(f.project, QStringLiteral("d:/samples/drums/KICK.wav")), files[0].uses);
#endif
        // A device's file, set and taken away.
        QCOMPARE(deviceState::fromModel(withDeviceFile(std::nullopt, kSnare)).value(kDeviceFileKey), kSnare);
        QVERIFY(deviceFile(samplerWith(QString())).isEmpty());
    }

    // --- Replacing and relinking -------------------------------------------------------------

    void aFileIsReplacedEverywhereInOneStep() {
        EditorFixture f;
        const QString a = f.editor.addAudioTrack();
        f.editor.commitClips(QStringLiteral("Add"), {{a, {audioClip(QStringLiteral("k1"), 0.0, 0.5, kKick),
                                                         audioClip(QStringLiteral("k2"), 1.0, 0.5, kKick)}}});
        Device sampler = samplerWith(kKick);
        const QString m = f.editor.addMidiTrackWith(sampler);
        const int steps = f.stack.count();
        const QString longer = QStringLiteral("D:/Samples/Drums/808.wav");
        QVERIFY(f.editor.replaceFile(fileUses(f.project, kKick), longer, 1.5, QStringLiteral("Replace Kick with 808")));
        QCOMPARE(f.stack.count(), steps + 1);
        QCOMPARE(f.stack.undoText(), QStringLiteral("Replace Kick with 808"));
        // The first would run into the second (1.5 s is 3 beats): trimmed there.
        const Clip& k1 = f.project.clip(a, QStringLiteral("k1"));
        const Clip& k2 = f.project.clip(a, QStringLiteral("k2"));
        QCOMPARE(k1.path, longer);
        QCOMPARE(k1.name, QStringLiteral("808"));
        QVERIFY(near(k1.durationSec, 0.5) && near(k1.sourceDurationSec, 1.5));
        QVERIFY(near(k2.durationSec, 1.5));
        QCOMPARE(deviceFile(f.project.device(m, sampler.id)), longer);
        // Undone: all of it.
        f.stack.undo();
        QCOMPARE(clipPath(f.project, a, QStringLiteral("k1")), kKick);
        QVERIFY(near(f.project.clip(a, QStringLiteral("k2")).durationSec, 0.5));
        QCOMPARE(deviceFile(f.project.device(m, sampler.id)), kKick);
        // Nothing to change, or no file: no step.
        QVERIFY(!f.editor.replaceFile(FileUses{}, longer, 1.5, QStringLiteral("Replace")));
        QVERIFY(!f.editor.replaceFile(fileUses(f.project, kKick), longer, 0.0, QStringLiteral("Replace")));
        QCOMPARE(f.stack.index(), steps);
    }

    void aHotSwapsTriesAreOneStepFromWhereItBegan() {
        EditorFixture f;
        const QString a = f.editor.addAudioTrack();
        Clip kick = audioClip(QStringLiteral("k1"), 0.0, 0.5, kKick);
        kick.name = QStringLiteral("Kick");  // (as dropped in: its file's name)
        kick.fadeOutSec = 0.1;
        f.editor.commitClips(QStringLiteral("Add"), {{a, {kick, audioClip(QStringLiteral("k2"), 4.0, 0.5, kKick)}}});
        const FileUses uses{{{a, QStringLiteral("k1")}}, {}};
        const int steps = f.stack.count();
        const QString key = QStringLiteral("hot swap");
        // A long sample: trimmed at the next clip (its fade out with it)...
        QVERIFY(f.editor.replaceFile(uses, QStringLiteral("Long.wav"), 3.0, QStringLiteral("Hot-Swap Long"), key));
        QVERIFY(near(f.project.clip(a, QStringLiteral("k1")).durationSec, 2.0));
        QCOMPARE(f.project.clip(a, QStringLiteral("k1")).fadeOutSec, 0.0);
        // ... and a short one after it: from the clip as it was before the hot swap.
        QVERIFY(f.editor.replaceFile(uses, QStringLiteral("Short.wav"), 0.25, QStringLiteral("Hot-Swap Short"), key));
        const Clip& now = f.project.clip(a, QStringLiteral("k1"));
        QCOMPARE(now.path, QStringLiteral("Short.wav"));
        QVERIFY(near(now.durationSec, 0.25));
        QCOMPARE(now.fadeOutSec, 0.1);
        QCOMPARE(f.stack.count(), steps + 1);
        QCOMPARE(f.stack.undoText(), QStringLiteral("Hot-Swap Short"));
        // Back to the file it began with: no step at all.
        QVERIFY(f.editor.replaceFile(uses, kKick, 0.5, QStringLiteral("Hot-Swap Kick"), key));
        QCOMPARE(f.stack.count(), steps);
        QCOMPARE(f.project.clip(a, QStringLiteral("k1")), kick);
        // Another hot swap is a step of its own.
        QVERIFY(f.editor.replaceFile(uses, QStringLiteral("Long.wav"), 3.0, QStringLiteral("Hot-Swap Long"), key));
        QVERIFY(f.editor.replaceFile(uses, QStringLiteral("Short.wav"), 0.25, QStringLiteral("Hot-Swap Short"),
                                     QStringLiteral("another")));
        QCOMPARE(f.stack.count(), steps + 2);
    }

    void frozenTracksKeepTheirFilesButFollowThem() {
        EditorFixture f;
        const QString a = f.editor.addAudioTrack();
        f.editor.commitClips(QStringLiteral("Add"), {{a, {audioClip(QStringLiteral("k1"), 0.0, 0.5, kKick)}}});
        f.editor.freezeTracks({{a, Freeze{QStringLiteral("frozen.wav"), 2.0, 120.0}}});
        QVERIFY(f.project.isFrozen(a));
        // Another file is other audio: refused.
        QVERIFY(!f.editor.replaceFile(fileUses(f.project, kKick), QStringLiteral("808.wav"), 1.0, QStringLiteral("Replace")));
        QVERIFY(!f.messages.isEmpty() && f.messages.back().contains(QStringLiteral("frozen")));
        FileUses frozen;
        QVERIFY(changeableUses(f.project, fileUses(f.project, kKick), &frozen).isEmpty());
        QCOMPARE(frozen.clips.size(), 1);
        // The same file found somewhere else is the same audio: taken.
        const QString moved = QStringLiteral("E:/Samples/Drums/Kick.wav");
        QVERIFY(f.editor.relinkFiles({{kKick, moved}}));
        QCOMPARE(clipPath(f.project, a, QStringLiteral("k1")), moved);
    }

    void filesFoundAreFollowedEverywhere() {
        EditorFixture f;
        const QString a = f.editor.addAudioTrack();
        const Clip kick = audioClip(QStringLiteral("k1"), 0.0, 0.5, kKick);
        const Clip reversed = edits::reverseClip(audioClip(QStringLiteral("r1"), 2.0, 0.5, kKick),
                                                 QStringLiteral("D:/Project/Reversed/Kick R.wav"), 0.5);
        f.editor.commitClips(QStringLiteral("Add"), {{a, {kick, reversed, audioClip(QStringLiteral("s1"), 4.0, 0.5, kSnare)}}});
        const Device sampler = samplerWith(kKick);
        const QString m = f.editor.addMidiTrackWith(sampler);
        const int steps = f.stack.count();
        const QString kickThere = QStringLiteral("E:/Samples/Drums/Kick.wav");
        const QString snareThere = QStringLiteral("E:/Samples/Drums/Snare.wav");
        QVERIFY(f.editor.relinkFiles({{kKick, kickThere}, {kSnare, snareThere}}));
        QCOMPARE(f.stack.count(), steps + 1);
        QCOMPARE(f.stack.undoText(), QStringLiteral("Locate Missing Files"));
        QCOMPARE(clipPath(f.project, a, QStringLiteral("k1")), kickThere);
        QCOMPARE(clipPath(f.project, a, QStringLiteral("s1")), snareThere);
        QCOMPARE(f.project.clip(a, QStringLiteral("r1")).reversedFrom, kickThere);
        QCOMPARE(f.project.clip(a, QStringLiteral("r1")).name, QStringLiteral("r1"));
        QCOMPARE(deviceFile(f.project.device(m, sampler.id)), kickThere);
        f.stack.undo();
        QCOMPARE(clipPath(f.project, a, QStringLiteral("k1")), kKick);
        QCOMPARE(deviceFile(f.project.device(m, sampler.id)), kKick);
        QVERIFY(!f.editor.relinkFiles({{QStringLiteral("D:/Nowhere.wav"), kickThere}}));
    }

    // --- The File Manager --------------------------------------------------------------------

    void missingFilesAreFoundInTheProjectsFolder() {
        TempDir places;
        QSettings().setValue(QStringLiteral("browser/places"), QStringList{places.path()});
        TempDir dir;  // (gone after the session, which may still be reading its files)
        SessionFixture f;
        const QString kick = wav(dir.path(QStringLiteral("Samples/Drums/Kick.wav")), 0.5);
        const QString snare = wav(dir.path(QStringLiteral("Samples/Drums/Snare.wav")), 0.5);
        const QString a = f.clipTrack(kick, 0.5, QStringLiteral("A"));
        const QString b = f.clipTrack(snare, 0.5, QStringLiteral("B"), 4.0);
        FileManager& files = *f.s().files();
        files.update();
        QCOMPARE(files.fileCount(), 2);
        QCOMPARE(files.missingCount(), 0);
        QCOMPARE(files.summary(), QStringLiteral("2 files"));
        const QString song = dir.path(QStringLiteral("Project/song.gilproj"));
        QVERIFY(QDir().mkpath(dir.path(QStringLiteral("Project"))));
        QVERIFY(f.s().saveProjectAs(song));

        // The samples move into the project's folder: the project opens with them missing.
        QVERIFY(QDir().mkpath(dir.path(QStringLiteral("Project/Library"))));
        QVERIFY(QDir().rename(dir.path(QStringLiteral("Samples")), dir.path(QStringLiteral("Project/Library/Samples"))));
        QVERIFY(f.s().openProject(song));
        QCOMPARE(f.lastMessage(), QStringLiteral("Opened song.gilproj: 2 files are missing (the File Manager finds them)"));
        QCOMPARE(files.missingCount(), 2);
        QCOMPARE(files.summary(), QStringLiteral("2 files, 2 missing"));
        QVERIFY(files.files()->rows().front().missing);
        QVERIFY(files.actions(kick).contains(QVariantMap{{QStringLiteral("action"), QStringLiteral("locate")},
                                                         {QStringLiteral("label"), QStringLiteral("Locate…")}}));

        // Search: in the project's folder.
        files.search();
        QVERIFY(files.searching());
        QVERIFY(files.waitForSearch());
        QCOMPARE(files.missingCount(), 0);
        const QString kickNow = dir.path(QStringLiteral("Project/Library/Samples/Drums/Kick.wav"));
        QVERIFY(samePath(clipPath(f.project(), a, QStringLiteral("Ac")), kickNow));
        QVERIFY(samePath(clipPath(f.project(), b, QStringLiteral("Bc")),
                                dir.path(QStringLiteral("Project/Library/Samples/Drums/Snare.wav"))));
        QCOMPARE(f.lastMessage(), QStringLiteral("Found all 2 missing files"));
        QCOMPARE(f.stack().undoText(), QStringLiteral("Locate Missing Files"));
        QVERIFY(f.waitForSource(kickNow));
        // Undone, they are missing again.
        f.stack().undo();
        files.update();
        QCOMPARE(files.missingCount(), 2);
        // Nothing there: said so.
        QVERIFY(QDir().rename(dir.path(QStringLiteral("Project/Library/Samples")), dir.path(QStringLiteral("Away"))));
        files.search();
        QVERIFY(files.waitForSearch());
        QCOMPARE(f.lastMessage(), QStringLiteral("No missing files were found"));
        // A folder chosen: found there.
        files.searchFolder(dir.path(QStringLiteral("Away")));
        QVERIFY(files.waitForSearch());
        QCOMPARE(files.missingCount(), 0);
        QCOMPARE(f.lastMessage(), QStringLiteral("Found all 2 missing files in %1").arg(QDir::toNativeSeparators(dir.path(QStringLiteral("Away")))));
    }

    void missingFilesAreFoundInTheBrowsersPlaces() {
        TempDir places;
        QSettings().setValue(QStringLiteral("browser/places"), QStringList{places.path()});
        TempDir dir;  // (gone after the session, which may still be reading its files)
        SessionFixture f;
        const QString kick = wav(dir.path(QStringLiteral("Kick.wav")), 0.5);
        const QString a = f.clipTrack(kick, 0.5, QStringLiteral("A"));
        QVERIFY(QFile::remove(kick));
        const QString there = wav(places.path() + QStringLiteral("/Drums/Kick.wav"), 0.5);
        f.s().browser()->rescan();
        QVERIFY(f.s().browser()->index().waitIdle());
        FileManager& files = *f.s().files();
        files.refresh();
        QCOMPARE(files.missingCount(), 1);
        files.search();  // (an unsaved project: no folder of its own)
        QVERIFY(files.waitForSearch());
        QVERIFY(samePath(clipPath(f.project(), a, QStringLiteral("Ac")), there));
        QCOMPARE(f.lastMessage(), QStringLiteral("Found the missing file"));
    }

    void locatingOneFileFindsTheRestOfItsFolder() {
        TempDir dir;  // (gone after the session, which may still be reading its files)
        SessionFixture f;
        const QString kick = wav(dir.path(QStringLiteral("Old/Kick.wav")), 0.5);
        const QString snare = wav(dir.path(QStringLiteral("Old/Snare.wav")), 0.5);
        const QString a = f.clipTrack(kick, 0.5, QStringLiteral("A"));
        const QString b = f.clipTrack(snare, 0.5, QStringLiteral("B"));
        QVERIFY(QDir().rename(dir.path(QStringLiteral("Old")), dir.path(QStringLiteral("New"))));
        FileManager& files = *f.s().files();
        files.refresh();
        QCOMPARE(files.missingFiles().size(), 2);
        QVERIFY(files.locate(kick, dir.path(QStringLiteral("New/Kick.wav"))));
        QCOMPARE(files.missingCount(), 0);
        QVERIFY(samePath(clipPath(f.project(), a, QStringLiteral("Ac")), dir.path(QStringLiteral("New/Kick.wav"))));
        QVERIFY(samePath(clipPath(f.project(), b, QStringLiteral("Bc")), dir.path(QStringLiteral("New/Snare.wav"))));
        QCOMPARE(f.lastMessage(), QStringLiteral("Located Kick.wav, and 1 file more where it went"));
        QCOMPARE(f.stack().undoText(), QStringLiteral("Locate Missing Files"));
        // Not an audio file: refused.
        QVERIFY(!files.locate(snare, dir.path(QStringLiteral("notes.txt"))));
    }

    void theFileManagerReplacesListsAndFilters() {
        TempDir dir;  // (gone after the session, which may still be reading its files)
        SessionFixture f;
        const QString kick = wav(dir.path(QStringLiteral("Kick.wav")), 0.5);
        const QString snare = wav(dir.path(QStringLiteral("Snare.wav")), 0.5);
        const QString other = wav(dir.path(QStringLiteral("Other Kick.wav")), 0.25);
        const QString a = f.clipTrack(kick, 0.5, QStringLiteral("A"));
        f.clipTrack(snare, 0.5, QStringLiteral("B"));
        FileManager& files = *f.s().files();
        files.update();
        FileListModel& rows = *files.files();
        QCOMPARE(rows.count(), 2);
        QCOMPARE(rows.get(0).value(QStringLiteral("name")).toString(), QStringLiteral("Kick.wav"));
        QCOMPARE(rows.get(0).value(QStringLiteral("uses")).toString(), QStringLiteral("1 clip"));
        QCOMPARE(rows.get(0).value(QStringLiteral("folder")).toString(), QDir::toNativeSeparators(dir.path()));
        QCOMPARE(rows.rowOf(snare), 1);
        // Filtered by name (and folder).
        files.setFilter(QStringLiteral("snar"));
        QCOMPARE(rows.count(), 1);
        QCOMPARE(rows.get(0).value(QStringLiteral("path")).toString(), snare);
        files.setFilter({});
        QCOMPARE(rows.count(), 2);

        // Replaced: everywhere it plays, one step.
        QVERIFY(files.replace(kick, other));
        QCOMPARE(clipPath(f.project(), a, QStringLiteral("Ac")), other);
        QVERIFY(near(f.project().clip(a, QStringLiteral("Ac")).durationSec, 0.25));
        QCOMPARE(f.lastMessage(), QStringLiteral("Replaced Kick with Other Kick (1 clip)"));
        QCOMPARE(f.stack().undoText(), QStringLiteral("Replace Kick with Other Kick"));
        files.update();
        QCOMPARE(rows.rowOf(other), 0);
        QCOMPARE(rows.rowOf(kick), -1);
        f.stack().undo();
        QCOMPARE(clipPath(f.project(), a, QStringLiteral("Ac")), kick);
        // Not an audio file, or itself: nothing.
        QVERIFY(!files.replace(kick, dir.path(QStringLiteral("notes.txt"))));
        QVERIFY(!files.replace(kick, kick));
        // Select Clips: its clips are selected.
        files.selectClips(kick);
        QCOMPARE(f.selection().clips(), (QSet<ClipRef>{{a, QStringLiteral("Ac")}}));
        // A file that is there: no Locate…; it can be shown in its folder.
        const QVariantList actions = files.actions(kick);
        QVERIFY(!actions.contains(QVariantMap{{QStringLiteral("action"), QStringLiteral("locate")},
                                              {QStringLiteral("label"), QStringLiteral("Locate…")}}));
        QVERIFY(actions.contains(QVariantMap{{QStringLiteral("action"), QStringLiteral("showInFolder")},
                                             {QStringLiteral("label"), QStringLiteral("Show in Folder")}}));
        // Show in File Manager: said to the panel (the filter goes if it hides the file).
        files.setFilter(QStringLiteral("snare"));
        QSignalSpy revealed(&files, &FileManager::revealRequested);
        files.reveal(kick);
        QCOMPARE(revealed.count(), 1);
        QCOMPARE(files.filter(), QString());
    }

    // --- Hot-swapping ------------------------------------------------------------------------

    void theBrowsersSelectionIsHotSwappedIn() {
        TempDir dir;  // (gone after the session, which may still be reading its files)
        SessionFixture f;
        const QString kick = wav(dir.path(QStringLiteral("Kick.wav")), 0.5);
        const QString first = wav(dir.path(QStringLiteral("Kick 2.wav")), 0.25);
        const QString second = wav(dir.path(QStringLiteral("Kick 3.wav")), 0.75);
        const QString kept = wav(dir.path(QStringLiteral("Kick 4.wav")), 0.3);
        const QString a = f.clipTrack(kick, 0.5, QStringLiteral("A"));
        const QString b = f.clipTrack(kick, 0.5, QStringLiteral("B"), 8.0);  // (the same kick elsewhere)
        HotSwap& hot = *f.s().hotSwap();
        BrowserController& browser = *f.s().browser();
        const int steps = f.stack().count();
        const size_t tracks = f.project().tracks().size();
        // Started on one clip: every clip playing its file, and the browser lists similar sounds.
        QSignalSpy similar(&browser, &BrowserController::similarChanged);
        QVERIFY(hot.startClip({a, QStringLiteral("Ac")}));
        QVERIFY(hot.active());
        QCOMPARE(hot.name(), QStringLiteral("Kick"));
        QCOMPARE(hot.usesText(), QStringLiteral("2 clips"));
        QCOMPARE(hot.originPath(), kick);
        QVERIFY(!similar.isEmpty());
        QVERIFY(samePath(browser.similarTo(), kick));
        // What the user chooses in the browser plays in its place, at once, everywhere.
        browser.chooseFile(first);
        QCOMPARE(clipPath(f.project(), a, QStringLiteral("Ac")), first);
        QCOMPARE(clipPath(f.project(), b, QStringLiteral("Bc")), first);
        QCOMPARE(hot.name(), QStringLiteral("Kick 2"));
        browser.chooseFile(second);
        QCOMPARE(clipPath(f.project(), a, QStringLiteral("Ac")), second);
        QVERIFY(near(f.project().clip(a, QStringLiteral("Ac")).durationSec, 0.75));
        QCOMPARE(f.stack().count(), steps + 1);
        QCOMPARE(f.stack().undoText(), QStringLiteral("Hot-Swap Kick 3"));
        // A folder is no sample; the current item changing by itself is no choice.
        browser.chooseFile(dir.path());
        browser.treeCurrentChanged(kept);
        QCOMPARE(clipPath(f.project(), a, QStringLiteral("Ac")), second);
        QVERIFY(hot.active());
        // A double-click (or Enter) keeps it and ends the hot swap: nothing is added.
        browser.activateFile(kept);
        QVERIFY(!hot.active());
        QCOMPARE(clipPath(f.project(), a, QStringLiteral("Ac")), kept);
        QCOMPARE(f.project().tracks().size(), tracks);
        QCOMPARE(f.stack().count(), steps + 1);
        // Once it ended, the browser adds files again (no track selected: on a new one).
        f.selection().selectTrack(QString());
        browser.activateFile(first);
        QCOMPARE(f.project().tracks().size(), tracks + 1);
        f.stack().undo();
        f.stack().undo();
        QCOMPARE(clipPath(f.project(), a, QStringLiteral("Ac")), kick);
    }

    void aHotSwapOfAFileTakesItsSamplersToo() {
        TempDir dir;  // (gone after the session, which may still be reading its files)
        SessionFixture f;
        const QString kick = wav(dir.path(QStringLiteral("Kick.wav")), 0.5);
        const QString other = wav(dir.path(QStringLiteral("Other.wav")), 0.5);
        const QString a = f.clipTrack(kick, 0.5, QStringLiteral("A"));
        const Device sampler = samplerWith(kick);
        const QString m = f.editor().addMidiTrackWith(sampler);
        FileManager& files = *f.s().files();
        HotSwap& hot = *f.s().hotSwap();
        QVERIFY(files.hotSwap(kick));
        QCOMPARE(hot.uses().clips, (ClipRefs{{a, QStringLiteral("Ac")}}));
        QCOMPARE(hot.uses().devices, (QList<DeviceRef>{{m, sampler.id}}));
        QVERIFY(hot.swap(other));
        QCOMPARE(clipPath(f.project(), a, QStringLiteral("Ac")), other);
        QCOMPARE(deviceFile(f.project().device(m, sampler.id)), other);
        // Another edit (deleting what it swaps, too) ends it.
        f.editor().deleteClips({{a, QStringLiteral("Ac")}});
        QVERIFY(!hot.active());
        // Another project ends it too.
        f.stack().undo();
        QVERIFY(hot.startFile(other));
        f.s().newProject();
        QVERIFY(!hot.active());
        QVERIFY(!hot.startFile(other));  // (nothing plays it now)
    }

    // A hot swap left running while the user goes on with something else (the
    // case that swapped a kick for the bass loop dragged in afterwards): the
    // browser's selection changing swaps nothing, and any other edit or an
    // undo ends it, so a sample dragged in later is added, never swapped.
    void aHotSwapEndsWhenTheUserDoesSomethingElse() {
        TempDir dir;  // (gone after the session, which may still be reading its files)
        SessionFixture f;
        const QString kick = wav(dir.path(QStringLiteral("Kick.wav")), 0.5);
        const QString loop = wav(dir.path(QStringLiteral("Bass Loop 150.wav")), 3.2);
        const QString a = f.clipTrack(kick, 0.5, QStringLiteral("A"));
        HotSwap& hot = *f.s().hotSwap();
        BrowserController& browser = *f.s().browser();
        QVERIFY(hot.startClip({a, QStringLiteral("Ac")}));
        // The current item changing (a press that becomes a drag, a list searched again) is no choice.
        browser.treeCurrentChanged(loop);
        QCOMPARE(clipPath(f.project(), a, QStringLiteral("Ac")), kick);
        QVERIFY(hot.active());
        // Another edit ends it: what is chosen afterwards swaps nothing.
        f.editor().setTempo(128.0);
        QVERIFY(!hot.active());
        browser.chooseFile(loop);
        QCOMPARE(clipPath(f.project(), a, QStringLiteral("Ac")), kick);
        // An undo ends it too, taking back what it swapped in.
        QVERIFY(hot.startClip({a, QStringLiteral("Ac")}));
        browser.chooseFile(loop);
        QCOMPARE(clipPath(f.project(), a, QStringLiteral("Ac")), loop);
        QVERIFY(hot.active());  // (its own swaps don't end it)
        f.stack().undo();
        QVERIFY(!hot.active());
        QCOMPARE(clipPath(f.project(), a, QStringLiteral("Ac")), kick);
        browser.chooseFile(loop);
        QCOMPARE(clipPath(f.project(), a, QStringLiteral("Ac")), kick);
    }

    void frozenClipsAreNotHotSwapped() {
        TempDir dir;  // (gone after the session, which may still be reading its files)
        SessionFixture f;
        const QString kick = wav(dir.path(QStringLiteral("Kick.wav")), 0.5);
        const QString a = f.clipTrack(kick, 0.5, QStringLiteral("A"));
        f.editor().freezeTracks({{a, Freeze{dir.path(QStringLiteral("frozen.wav")), 2.0, 120.0}}});
        HotSwap& hot = *f.s().hotSwap();
        QVERIFY(!hot.startClip({a, QStringLiteral("Ac")}));
        QVERIFY(!hot.active());
        QVERIFY(f.lastMessage().contains(QStringLiteral("frozen")));
    }
};

QTEST_GUILESS_MAIN(TestFileManager)
#include "test_file_manager.moc"
