// The whole application, end to end: a project made through the session (an
// audio track in a group, a MIDI track with an instrument, an effect, notes and
// automation, a return fed by a send), saved, opened again into the main
// window (Main.qml, every view real), shown, played offline, edited through
// the window's actions and undone. No QML warning or error may come up on the
// way. Runs on a display (xvfb here), like the other UI tests.

#include <QDir>
#include <QHash>
#include <QImage>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTest>
#include <QUndoStack>

#include <cmath>
#include <memory>
#include <vector>

#include "TestSupport.h"
#include "UiTestSupport.h"
#include "audio/EngineBridge.h"
#include "editor/ProjectEditor.h"
#include "model/Automation.h"
#include "model/Project.h"
#include "session/ArrangementActions.h"
#include "session/Selection.h"
#include "session/Session.h"
#include "theme/Theme.h"

using namespace sub::app;
using sub::ui::Theme;

namespace {

// QML's own complaints, wherever they come from (the engine's warnings, or
// qWarning from a binding or a JavaScript error), are collected here.
QStringList& qmlComplaints() {
    static QStringList complaints;
    return complaints;
}
QtMessageHandler previousHandler = nullptr;

void collectQmlComplaints(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    const bool qml = message.contains(QLatin1String(".qml:")) || message.contains(QLatin1String("qrc:/")) ||
                     QLatin1String(context.category ? context.category : "").startsWith(QLatin1String("qt.qml"));
    if (type != QtDebugMsg && type != QtInfoMsg && qml) qmlComplaints() << message;
    if (previousHandler) previousHandler(type, context, message);
}

}  // namespace

class TestUiApp : public QObject {
    Q_OBJECT

    std::unique_ptr<test::TempDir> dir_;
    std::unique_ptr<test::UiSession> ui_;
    QQuickWindow* window_ = nullptr;

    Session& session() { return ui_->session(); }
    Project& project() { return *session().project(); }
    ProjectEditor& editor() { return *session().editor(); }
    EngineBridge& bridge() { return *session().bridge(); }

    QObject* action(const QString& name) {
        for (QObject* object : window_->findChildren<QObject*>(name))
            if (object->inherits("QQuickAction")) return object;
        return nullptr;
    }
    void trigger(const QString& name) {
        QObject* found = action(name);
        QVERIFY2(found, qPrintable(name));
        QVERIFY(QMetaObject::invokeMethod(found, "trigger"));
    }
    QQuickItem* item(const QString& name) { return window_->findChild<QQuickItem*>(name); }

    // The level of a mono mix of interleaved stereo: its RMS.
    static double rms(const std::vector<float>& stereo) {
        double sum = 0.0;
        for (const float s : stereo) sum += double(s) * s;
        return stereo.empty() ? 0.0 : std::sqrt(sum / double(stereo.size()));
    }

private Q_SLOTS:
    void initTestCase() {
        test::prepareApplication();
        if (!test::haveDisplay()) QSKIP("needs a display: Qt Quick's software renderer draws none of this geometry");
        previousHandler = qInstallMessageHandler(collectQmlComplaints);
        dir_ = std::make_unique<test::TempDir>();
        QDir().mkpath(dir_->path(QStringLiteral("place")));
        QSettings().setValue(QStringLiteral("browser/places"), QStringList{dir_->path(QStringLiteral("place"))});
        sub::ui::setUpApplication();
        ui_ = std::make_unique<test::UiSession>();
        connect(&ui_->qml(), &QQmlEngine::warnings, this, [](const QList<QQmlError>& list) {
            for (const QQmlError& error : list) qmlComplaints() << error.toString();
        });
        window_ = ui_->show("import SUBstation\n\nMain {}\n");
        QVERIFY(window_);
        window_->resize(1440, 860);
        QTest::qWait(100);
    }

    void cleanupTestCase() {
        ui_.reset();
        qInstallMessageHandler(previousHandler);
        QVERIFY2(qmlComplaints().isEmpty(), qPrintable(qmlComplaints().join(QLatin1Char('\n'))));
    }

    void aProjectMadeSavedOpenedShownAndPlayed() {
        // An audio track playing a second of a quiet tone, in a group.
        std::vector<float> tone(size_t(2 * test::kSampleRate));
        for (size_t i = 0; i < tone.size() / 2; ++i) {
            const float s = 0.25f * float(std::sin(2.0 * M_PI * 220.0 * double(i) / test::kSampleRate));
            tone[2 * i] = tone[2 * i + 1] = s;
        }
        const QString wav = test::writeWav(dir_->path(QStringLiteral("place/Tone 120bpm.wav")), tone, 2);
        const ClipRefs audio = editor().addClips(QString(), 0.0, {{wav, 1.0}}, 0);
        QCOMPARE(audio.size(), qsizetype(1));
        const QString audioTrack = audio.front().trackId;
        const QString group = editor().groupTracks({audioTrack});
        QVERIFY(!group.isEmpty());

        // A MIDI track: the default instrument, a compressor after it, notes, and its volume automated.
        const QString midiTrack = editor().addMidiTrack();
        QVERIFY(!midiTrack.isEmpty());
        QVERIFY(!editor().addDevice(midiTrack, QStringLiteral("compressor")).isEmpty());
        const auto clip = editor().addMidiClip(midiTrack, 0.0, 4.0);
        QVERIFY(clip.has_value());
        editor().setClipNotes(*clip, {Note{60, 0.0, 1.0, 100}, Note{64, 1.0, 1.0, 100}, Note{67, 2.0, 2.0, 100}},
                              QStringLiteral("Notes"));
        editor().addAutomationPoint(midiTrack, automation::kMixerVolume, 0.0, 0.5);
        editor().addAutomationPoint(midiTrack, automation::kMixerVolume, 4.0, 0.8);

        // A return with a delay, fed by the MIDI track.
        const QString ret = editor().addReturnTrack();
        QVERIFY(!editor().addDevice(ret, QStringLiteral("delay")).isEmpty());
        editor().setSend(midiTrack, ret, -6.0, false);

        // Saved, then a new project, then that one opened again.
        const QString path = dir_->path(QStringLiteral("Song.gilproj"));
        QVERIFY(session().saveProjectAs(path));
        QVERIFY(session().clean());
        QCOMPARE(window_->title(), QStringLiteral("Song - SUBstation"));
        session().newProject();
        QVERIFY(project().tracks().empty());
        QVERIFY(session().openProject(path));
        QCOMPARE(window_->title(), QStringLiteral("Song - SUBstation"));

        // Everything is back.
        QCOMPARE(project().tracks().size(), size_t(3));  // the group, its track, the MIDI track
        QCOMPARE(project().returns().size(), size_t(1));
        const Track* midi = nullptr;
        for (const Track& track : project().tracks())
            if (track.isMidi()) midi = &track;
        QVERIFY(midi);
        QCOMPARE(midi->devices.size(), size_t(2));
        QCOMPARE(midi->clips.size(), size_t(1));
        QCOMPARE(midi->clips.front().notes.size(), size_t(3));
        QCOMPARE(project().envelope(midi->id, automation::kMixerVolume).size(), size_t(2));
        QCOMPARE(midi->sends.size(), qsizetype(1));
        const QString midiId = midi->id;

        // Shown: the arrangement, and the MIDI track's devices in the device view.
        session().selection()->selectTrack(midiId, true);
        QTRY_VERIFY(item(QStringLiteral("devicePanel"))->isVisible());
        QTest::qWait(300);
        test::screenshot(window_, QStringLiteral("app-project"));

        // Played (offline): the tone and the synth are heard, the master's meter moves.
        QTRY_VERIFY_WITH_TIMEOUT(bridge().devicesReady(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!bridge().isLoading(wav), 10000);
        const std::vector<float> out = ui_->engine().renderOffline(0.0, 2 * test::kSampleRate);
        QVERIFY2(rms(out) > 0.01, qPrintable(QString::number(rms(out))));

        // Edited through the window: a new audio track (Ctrl+T), undone, redone.
        const size_t before = project().tracks().size();
        trigger(QStringLiteral("insertAudioTrack"));
        QCOMPARE(project().tracks().size(), before + 1);
        QVERIFY(!session().clean());
        trigger(QStringLiteral("undo"));
        QCOMPARE(project().tracks().size(), before);
        QVERIFY(session().clean());
        trigger(QStringLiteral("redo"));
        QCOMPARE(project().tracks().size(), before + 1);

        // The MIDI clip opens in the clip view, over the arrangement.
        Q_EMIT session().arrangement()->clipViewRequested(
            QVariantList{QVariantMap{{QStringLiteral("trackId"), midiId}, {QStringLiteral("clipId"), clip->clipId}}},
            midiId, clip->clipId);
        QTRY_VERIFY(item(QStringLiteral("clipView"))->isVisible());
        QTest::qWait(200);
        test::screenshot(window_, QStringLiteral("app-piano-roll"));
        trigger(QStringLiteral("clipView"));  // Shift+Tab: back to the arrangement
        QTRY_VERIFY(!item(QStringLiteral("clipView"))->isVisible());

        session().undoStack()->setClean();  // (no question when the window goes)
    }

    // Look and Feel's themes apply to the whole window at once: what QML
    // colours (the transport bar, a button's look) and what the C++ items draw
    // (the ruler).
    void themesRepaintTheWindow() {
        QQuickItem* bar = item(QStringLiteral("transportBar"));
        QQuickItem* ruler = item(QStringLiteral("ruler"));
        QQuickItem* play = item(QStringLiteral("play"));
        QVERIFY(bar && ruler && play);
        const auto mostCommon = [this](QQuickItem* part) {
            const QImage image = window_->grabWindow();
            const QRect rect = QRectF(part->mapToScene(QPointF(0, 0)) * image.devicePixelRatio(),
                                      part->size() * image.devicePixelRatio()).toRect() & image.rect();
            QHash<QRgb, int> counts;
            for (int y = rect.top(); y <= rect.bottom(); ++y)
                for (int x = rect.left(); x <= rect.right(); ++x)
                    ++counts[image.pixel(x, y)];
            QRgb best = 0;
            for (auto it = counts.cbegin(); it != counts.cend(); ++it)
                if (it.value() > counts.value(best)) best = it.key();
            return QColor::fromRgb(best);
        };
        for (const QString& name : Theme::names()) {
            Theme::apply(name);
            QTRY_COMPARE(mostCommon(bar), Theme::panel());
            QTRY_COMPARE(mostCommon(ruler), Theme::panel());
            QCOMPARE(play->property("look").toMap().value(QStringLiteral("background")).value<QColor>(), Theme::surface());
            test::screenshot(window_, QStringLiteral("app-theme-") + name.toLower());
        }
        Theme::apply(Theme::names().front());
        QTRY_COMPARE(mostCommon(ruler), Theme::panel());
    }
};

QTEST_MAIN(TestUiApp)
#include "test_ui_app.moc"
