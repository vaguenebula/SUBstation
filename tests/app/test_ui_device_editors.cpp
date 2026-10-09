// The built-in devices' own editors (ui/qml/devices/editors, ui/src/devices):
// each editor loaded as the device view will load it (DeviceEditors's
// component, given the track and the device), with a real session over an
// engine; driven with the mouse and the keyboard; the project and (rendering
// offline) the engine checked. Also the shared parameter cell (DeviceParamKnob)
// and its menu. With SUBSTATION_UI_SCREENSHOTS set to a folder, every editor
// is saved there as a PNG, with something to show.

#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>
#include <QUrl>
#include <QWheelEvent>

#include <cmath>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

#include "Engine.h"
#include "TestSupport.h"
#include "Ui.h"
#include "analysis/SidechainFit.h"
#include "app/AppTypes.h"
#include "audio/EngineBridge.h"
#include "controls/KnobItem.h"
#include "controls/ValueBoxItem.h"
#include "devices/CurveGraph.h"
#include "devices/DeviceParam.h"
#include "devices/DisplayClock.h"
#include "devices/EqGraph.h"
#include "devices/EqView.h"
#include "devices/FilterGraph.h"
#include "devices/ReductionGraph.h"
#include "devices/SampleView.h"
#include "editor/ProjectEditor.h"
#include "io/Serialization.h"
#include "model/Automation.h"
#include "model/Clip.h"
#include "model/DeviceState.h"
#include "model/Project.h"
#include "model/Timebase.h"
#include "session/Session.h"
#include "sg/SgCanvas.h"
#include "sg/SgPainter.h"

using namespace sub::app;
using namespace sub::ui;
using sub::app::test::kSampleRate;

namespace {

constexpr double kPi = 3.14159265358979323846;
// The host's QML (at the end of the file: moc skips what follows a raw string).
extern const char* const kHost;

void save(const QImage& image, const QString& name) {
    const QString folder = qEnvironmentVariable("SUBSTATION_UI_SCREENSHOTS");
    if (folder.isEmpty())
        return;
    QDir().mkpath(folder);
    QVERIFY(image.save(QDir(folder).filePath(QStringLiteral("device-editors-") + name)));
}

QPoint scenePoint(QQuickItem* item, QPointF at) { return item->mapToScene(at).toPoint(); }

QPoint centerOf(QQuickItem* item) { return scenePoint(item, QPointF(item->width() / 2, item->height() / 2)); }

// A move with the left button held, with modifiers (QTest's mouseMove has none).
void dragTo(QQuickWindow* window, QPoint pos, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent event(QEvent::MouseMove, QPointF(pos), QPointF(window->mapToGlobal(pos)), Qt::NoButton,
                      Qt::LeftButton, modifiers);
    QGuiApplication::sendEvent(window, &event);
}

void wheel(QQuickWindow* window, QPoint pos, int angle, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QWheelEvent event(QPointF(pos), QPointF(window->mapToGlobal(pos)), QPoint(), QPoint(0, angle), Qt::NoButton,
                      modifiers, Qt::NoScrollPhase, false);
    QGuiApplication::sendEvent(window, &event);
}

std::vector<float> tone(double freq, int frames, double amplitude = 0.5) {
    std::vector<float> samples(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i)
        samples[size_t(i)] = float(amplitude * std::sin(2 * kPi * freq * i / kSampleRate));
    return samples;
}

// Interleaved stereo of the same mono signal.
std::vector<float> stereo(const std::vector<float>& mono) {
    std::vector<float> both;
    both.reserve(mono.size() * 2);
    for (float s : mono) {
        both.push_back(s);
        both.push_back(s);
    }
    return both;
}

float peakOf(const std::vector<float>& samples, size_t from = 0, size_t stride = 1, size_t offset = 0) {
    float most = 0.f;
    for (size_t i = from * stride + offset; i < samples.size(); i += stride)
        most = std::max(most, std::abs(samples[i]));
    return most;
}

// Many small rects, then a red one at the bottom right: in one solid run.
class ManyRects : public sub::ui::SgCanvas {
    Q_OBJECT

public:
    int count = 1000;

protected:
    void paint(sub::ui::SgPainter& p) override {
        for (int i = 0; i < count; ++i)
            p.fillRect(QRectF((i * 7) % 300, (i * 3) % 200, 1, 1), QColor(0, 0, 255));
        p.fillRect(QRectF(300, 200, 100, 100), QColor(255, 0, 0));
    }
};

}  // namespace

class TestUiDeviceEditors : public QObject {
    Q_OBJECT

    std::unique_ptr<sub::Engine> engine_;
    std::unique_ptr<Session> session_;
    std::unique_ptr<QQmlEngine> qml_;
    std::unique_ptr<QObject> root_;
    QQuickWindow* window_ = nullptr;
    sub::app::test::TempDir dir_;

    Project* project() const { return session_->project(); }
    ProjectEditor* editor() const { return session_->editor(); }
    QUndoStack* undo() const { return session_->undoStack(); }
    EngineBridge* bridge() const { return session_->bridge(); }

    double param(const QString& trackId, const QString& deviceId, const QString& paramId, double otherwise = -999) {
        const Device* device = project()->findDevice(trackId, deviceId);
        return device && device->params.contains(paramId) ? device->params.value(paramId) : otherwise;
    }

    // The device's body in the device view: the tallest device's (two rows of knobs and their names,
    // 6 px above and below them), as the host measures it.
    int bodyHeight() const { return root_->property("deviceBodyHeight").toInt(); }

    // Shows a device's editor (DeviceEditors's component for its kind), the body's size (the device
    // view's by default); the editor.
    QQuickItem* show(const QString& kind, const QString& trackId, const QString& deviceId, int height = 0) {
        if (height <= 0) height = bodyHeight();
        QVariant url;
        QMetaObject::invokeMethod(root_.get(), "editorFor", Q_RETURN_ARG(QVariant, url), Q_ARG(QVariant, kind));
        if (url.toString().isEmpty())
            return nullptr;
        QVariant item;
        QMetaObject::invokeMethod(root_.get(), "show", Q_RETURN_ARG(QVariant, item), Q_ARG(QVariant, url),
                                  Q_ARG(QVariant, trackId), Q_ARG(QVariant, deviceId), Q_ARG(QVariant, height));
        auto* editor = qvariant_cast<QQuickItem*>(item);
        if (!editor)
            return nullptr;
        fitted();
        QTest::qWait(50);  // (laid out, painted)
        return editor;
    }

    // Waits for the window to take the body's size (the X server resizes it later), so
    // clicks land inside it.
    bool fitted() {
        return QTest::qWaitFor([this] {
            return window_->width() == root_->property("bodyWidth").toInt() + 2
                && window_->height() == root_->property("bodyHeight").toInt() + 2;
        }, 3000);
    }

    // Every item under `root` (in the item tree) whose object name starts with `prefix`, in order.
    static QList<QQuickItem*> itemsNamed(QQuickItem* root, const QString& prefix) {
        QList<QQuickItem*> found;
        for (QQuickItem* child : root->childItems()) {
            if (child->objectName().startsWith(prefix))
                found << child;
            found += itemsNamed(child, prefix);
        }
        return found;
    }

    template <typename T = QQuickItem>
    T* find(QQuickItem* in, const QString& name) {
        for (QQuickItem* item : itemsNamed(in, name)) {
            if (item->objectName() == name) {
                if (auto* found = qobject_cast<T*>(item))
                    return found;
            }
        }
        qWarning() << "no" << name;
        return nullptr;
    }

    QImage grab() { return window_->grabWindow(); }

    // The EQ's window of a device, if one is open (EqWindows).
    QQuickWindow* eqWindowOf(const QString& track, const QString& device) {
        QVariant window;
        QMetaObject::invokeMethod(root_.get(), "eqWindow", Q_RETURN_ARG(QVariant, window), Q_ARG(QVariant, track),
                                  Q_ARG(QVariant, device));
        return qvariant_cast<QQuickWindow*>(window);
    }

    // The displays' clock ticked: what the editors read from the engine's displays as the view refreshes.
    void refreshDisplays() { Q_EMIT sub::ui::DisplayClock::instance()->tick(); }

    // A MIDI track whose instrument is a Sampler, its editor shown: (track, device, editor, its waveform).
    std::tuple<QString, QString, QQuickItem*, SampleView*> sampler() {
        const QString track = editor()->addMidiTrack();
        const QString device = editor()->addDevice(track, QStringLiteral("sampler"));
        QQuickItem* view = show(QStringLiteral("sampler"), track, device);
        return {track, device, view, view ? find<SampleView>(view, QStringLiteral("sampleView")) : nullptr};
    }

    std::optional<QString> stateOf(const QString& track, const QString& device) {
        const Device* found = project()->findDevice(track, device);
        return found ? found->state : std::nullopt;
    }

    // A built-in device's state as the engine has it.
    sub::app::deviceState::Values engineState(const QString& track, const QString& device) {
        bridge()->waitForDeviceStates();
        const auto id = bridge()->engineDeviceId(track, device);
        if (!id)
            return {};
        const std::vector<uint8_t> bytes = engine_->processorState(*id);
        return sub::app::deviceState::decode(
            QByteArray(reinterpret_cast<const char*>(bytes.data()), qsizetype(bytes.size())));
    }

    QString audioTrackWith(const std::vector<float>& mono, const QString& name, double seconds) {
        const QString path = sub::app::test::writeWav(dir_.path(name + QStringLiteral(".wav")), stereo(mono), 2);
        const ClipRefs refs = editor()->addClips(QString(), 0.0, {{path, seconds}});
        return refs.isEmpty() ? QString() : refs.first().trackId;
    }

private Q_SLOTS:
    void initTestCase() {
        if (QGuiApplication::platformName() == QLatin1String("offscreen") ||
            QGuiApplication::platformName() == QLatin1String("minimal"))
            QSKIP("needs a display: the offscreen platform renders Qt Quick in software, without this geometry");
        sub::app::test::prepareApplication();
        sub::ui::setUpApplication();
        engine_ = std::make_unique<sub::Engine>();
        engine_->setClipFadeMs(0);
        Session::Options options;
        options.scanner = QStringLiteral(SUBSTATION_SCANNER);
        options.scanPlugins = false;
        options.browserIndex = false;
        session_ = std::make_unique<Session>(*engine_, options);
        sub::ui::registerSession(session_.get());
        qmlRegisterType<ManyRects>("EditorsTest", 1, 0, "ManyRects");
        qml_ = std::make_unique<QQmlEngine>();
        sub::ui::setUpEngine(*qml_);
        QQmlComponent component(qml_.get());
        component.setData(kHost, QUrl(QStringLiteral("qrc:/test/Host.qml")));
        root_.reset(component.create());
        if (!root_)
            qWarning() << component.errors();
        QVERIFY(root_);
        window_ = qobject_cast<QQuickWindow*>(root_.get());
        QVERIFY(window_);
        window_->setPosition(200, 200);
        QVERIFY(QTest::qWaitForWindowExposed(window_));
        window_->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window_));
    }

    void cleanupTestCase() {
        root_.reset();
        qml_.reset();
        if (session_)
            session_->shutdown();
        session_.reset();
        engine_.reset();
    }

    void init() {
        QMetaObject::invokeMethod(root_.get(), "clear");
        project()->clear();
        undo()->clear();
        EqView::instance()->setPanel(false);  // (shared by every EQ: as the app starts)
        QTest::mouseMove(window_, QPoint(1, 1));
    }

    // --- What the editors needed of SgPainter ---------------------------------------------------

    void manyVerticesDraw() {
        // The renderer draws no node of more than 65535 vertices: what goes past that in one paint
        // (an EQ's two dozen curves) is drawn all the same, in nodes of its own.
        for (int count : {10000, 11000, 40000}) {
            QQmlComponent component(qml_.get());
            component.setData("import QtQuick\nimport EditorsTest\n"
                              "Window { width: 400; height: 300; visible: true; ManyRects { anchors.fill: parent } }",
                              QUrl(QStringLiteral("qrc:/test/ManyRects.qml")));
            std::unique_ptr<QObject> root(component.create());
            QVERIFY(root);
            auto* window = qobject_cast<QQuickWindow*>(root.get());
            auto* canvas = qobject_cast<ManyRects*>(window->contentItem()->childItems().first());
            QVERIFY(canvas);
            canvas->count = count;
            canvas->update();
            QVERIFY(QTest::qWaitForWindowExposed(window));
            QTest::qWait(50);
            const QImage image = window->grabWindow();
            QCOMPARE(QColor(image.pixel(350, 250)), QColor(255, 0, 0));  // the last rect drawn
            QCOMPARE(canvas->lastStats().solidNodes, (count * 6 + 6 + 65531) / 65532);
        }
    }


    void painterAdditions() {
        // An area filled with a vertical gradient (the EQ's band fill): between the curve and the
        // baseline only, crossing it too, the colour at each stop where it falls.
        sub::ui::SgRecording recording;
        {
            sub::ui::SgPainter p(recording, QSizeF(580, 156), 1.0, nullptr);
            p.setClipRect(QRectF(1, 1, 578, 154));
            std::vector<QPointF> curve;
            for (int i = 0; i <= 200; ++i)
                curve.emplace_back(1 + 578.0 * i / 200, 78.0 + 40.0 * std::sin(i / 20.0));
            QLinearGradient fill(QPointF(0, 11), QPointF(0, 145));
            fill.setColorAt(0.0, QColor(0, 255, 0, 95));
            fill.setColorAt(0.5, QColor(0, 255, 0, 25));
            fill.setColorAt(1.0, QColor(0, 255, 0, 95));
            p.fillToBaseline(curve.data(), int(curve.size()), 78.0, fill);
        }
        QVERIFY(!recording.vertices.empty());
        for (const auto& v : recording.vertices) {
            QVERIFY(v.y >= 38.0 - 1e-3 && v.y <= 118.0 + 1e-3);
            if (std::abs(v.y - 78.0) < 1e-3)
                QCOMPARE(int(v.a), 25);  // the middle stop, on the baseline
        }
        // A recording too big for one batch goes on in another (the renderer draws a batch only so big).
        {
            sub::ui::SgPainter p(recording, QSizeF(1000, 100), 1.0, nullptr);
            for (int i = 0; i < 20000; ++i)
                p.fillRect(QRectF(i % 1000, i / 1000, 1, 1), Qt::white);
        }
        QVERIFY(recording.segments.size() >= 2);
        for (const auto& segment : recording.segments)
            QVERIFY(segment.count <= 65535 && segment.count % 3 == 0);
    }

    // --- The registry -----------------------------------------------------------------------

    void registry() {
        for (const char* kind : {"compressor", "delay", "eq", "sampler", "sidechain"}) {
            QVariant url;
            QMetaObject::invokeMethod(root_.get(), "editorFor", Q_RETURN_ARG(QVariant, url),
                                      Q_ARG(QVariant, QString::fromLatin1(kind)));
            QVERIFY2(url.toString().endsWith(QStringLiteral("Editor.qml")), kind);
        }
        for (const char* kind : {"utility", "synth", "ott", "constructor", ""}) {  // the generic knobs
            QVariant url;
            QMetaObject::invokeMethod(root_.get(), "editorFor", Q_RETURN_ARG(QVariant, url),
                                      Q_ARG(QVariant, QString::fromLatin1(kind)));
            QCOMPARE(url.toString(), QString());
        }
    }

    // --- The Compressor ----------------------------------------------------------------------

    void compressor() {
        const QString track = audioTrackWith(std::vector<float>(kSampleRate, 0.5f), QStringLiteral("loud"), 1.0);
        QVERIFY(!track.isEmpty());
        const QString device = editor()->addDevice(track, QStringLiteral("compressor"));
        QQuickItem* view = show(QStringLiteral("compressor"), track, device);
        QVERIFY(view);
        // Every knob at once.
        const QList<QQuickItem*> cells = itemsNamed(view, QStringLiteral("param_"));
        QCOMPARE(cells.size(), 7);
        QVERIFY(view->implicitHeight() <= bodyHeight());  // fits the view

        // Its knobs edit the model as the generic ones do (undoably).
        auto* graph = find<ReductionGraph>(view, QStringLiteral("reductionGraph"));
        QVERIFY(graph);
        editor()->setDeviceParam(track, device, QStringLiteral("threshold"), -30.0);
        KnobItem* threshold = nullptr;
        for (QQuickItem* cell : cells)
            if (cell->property("paramId").toString() == QLatin1String("threshold"))
                threshold = qvariant_cast<KnobItem*>(cell->property("knob"));
        QVERIFY(threshold);
        QCOMPARE(threshold->value(), -30.0);
        undo()->undo();
        QCOMPARE(threshold->value(), -18.0);

        // What the engine reports as it renders reaches the graph with the meters.
        engine_->renderOffline(0.0, kSampleRate / 2);
        refreshDisplays();
        const double reduction = (20 * std::log10(0.5) + 18.0) * 0.75;  // the default threshold and ratio, at -6 dB
        QVERIFY2(std::abs(graph->reduction() - reduction) < 0.1, qPrintable(QString::number(graph->reduction())));
        QVERIFY(std::abs(graph->levelIn() - 20 * std::log10(0.5)) < 0.1);
        QTest::qWait(50);
        save(grab(), QStringLiteral("compressor.png"));
    }

    // --- The Sampler -------------------------------------------------------------------------

    void samplerLoadsASampleUndoably() {
        std::vector<float> wave = tone(440.0, kSampleRate);
        const QString path = sub::app::test::writeWav(dir_.path(QStringLiteral("tone.wav")), wave);
        auto [track, device, view, samples] = sampler();
        QVERIFY(view && samples);
        QCOMPARE(project()->track(track).devices.size(), size_t(1));
        QCOMPARE(project()->track(track).devices.front().kind, QStringLiteral("sampler"));
        // Two pages, named (the device's title bar shows them as tabs): Sample, then Controls.
        QCOMPARE(view->property("pages").toInt(), 2);
        QCOMPARE(view->property("pageNames").toStringList(), (QStringList{"Sample", "Controls"}));
        QQuickItem* samplePage = find(view, QStringLiteral("samplePage"));
        QQuickItem* controlsPage = find(view, QStringLiteral("controlsPage"));
        QVERIFY(samplePage->isVisible() && !controlsPage->isVisible());
        // Classic: its envelope under the display, with the filter's knobs, Transpose, Vol < Vel, Volume.
        const auto cell = [&](QQuickItem* page, const char* id) {
            return find(page, QStringLiteral("cell_") + QLatin1String(id));
        };
        for (const char* id : {"filter_freq", "filter_res", "attack", "decay", "sustain", "release", "tune", "velocity",
                               "volume"})
            QVERIFY2(cell(samplePage, id)->isVisible(), id);
        for (const char* id : {"fade_in", "fade_out"})
            QVERIFY2(!cell(samplePage, id)->isVisible(), id);
        QCOMPARE(qvariant_cast<sub::ui::DeviceParam*>(cell(samplePage, "velocity")->property("param"))->text(),
                 QStringLiteral("50 %"));
        // Controls: the root key and the rest.
        view->setProperty("page", 1);
        QVERIFY(!samplePage->isVisible() && controlsPage->isVisible());
        QCOMPARE(qvariant_cast<sub::ui::DeviceParam*>(cell(controlsPage, "root")->property("param"))->text(),
                 QStringLiteral("C3"));
        for (const char* id : {"root", "fine", "voices", "glide", "start", "end", "loop_start", "loop_fade", "lfo_volume",
                               "lfo_pitch", "lfo_filter", "lfo_pan", "pan", "gain"})
            QVERIFY2(cell(controlsPage, id)->isVisible(), id);
        view->setProperty("page", 0);
        QVERIFY(view->implicitHeight() <= bodyHeight());  // fits the view
        QCOMPARE(samples->samplePath(), QString());
        save(grab(), QStringLiteral("sampler-empty.png"));  // the hint to drop a sample

        samples->loadSample(path);
        QCOMPARE(sub::app::deviceState::fromModel(stateOf(track, device)),
                 (sub::app::deviceState::Values{{QStringLiteral("sample"), path}}));
        QCOMPARE(undo()->undoText(), QStringLiteral("Load Sample"));
        QCOMPARE(engineState(track, device), (sub::app::deviceState::Values{{QStringLiteral("sample"), path}}));

        undo()->undo();  // no sample again
        QVERIFY(!stateOf(track, device));
        QCOMPARE(samples->samplePath(), QString());
        QVERIFY(engineState(track, device).isEmpty());
        undo()->redo();
        QCOMPARE(engineState(track, device), (sub::app::deviceState::Values{{QStringLiteral("sample"), path}}));
        QCOMPARE(samples->samplePath(), path);

        // It plays what was loaded; the playhead follows the note.
        const auto clip = editor()->addMidiClip(track, 0.0, 4.0);
        QVERIFY(clip);
        editor()->setClipNotes(*clip, {Note{60, 0.0, 1.0, 127}}, QStringLiteral("Add Note"));
        const std::vector<float> out = engine_->renderOffline(0.0, kSampleRate / 4);
        QVERIFY(peakOf(out, 1000, 2, 0) > 0.2f);
        refreshDisplays();
        QVERIFY2(0.2 < samples->playhead() && samples->playhead() < 0.3,
                 qPrintable(QString::number(samples->playhead())));

        // Its waveform comes from the engine's cache: quick, and drawn.
        QTRY_VERIFY(samples->decoded());
        QTest::qWait(50);
        save(grab(), QStringLiteral("sampler.png"));
    }

    void samplerMarkersDropAndSaving() {
        const QString path = sub::app::test::writeWav(dir_.path(QStringLiteral("dc.wav")),
                                                     std::vector<float>(size_t(kSampleRate) * 2, 0.5f), 2);
        auto [track, device, view, samples] = sampler();
        QVERIFY(view && samples);

        // Dropping an audio file on the waveform loads it (other files are refused).
        {
            QMimeData mime;
            mime.setUrls({QUrl::fromLocalFile(dir_.path(QStringLiteral("notes.txt"))), QUrl::fromLocalFile(path)});
            QDropEvent drop(QPointF(40, 40), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(samples, &drop);
            QVERIFY(drop.isAccepted());
        }
        QCOMPARE(samples->samplePath(), path);
        QTRY_VERIFY(samples->decoded());

        // Dragging the Start marker is one undoable edit; it can't pass End.
        const QRectF plot = samples->plot();
        auto at = [&](double x) { return scenePoint(samples, QPointF(x, 40)); };
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at(plot.left()));
        dragTo(window_, at(plot.left() + plot.width() / 4));
        dragTo(window_, at(plot.left() + plot.width() / 2));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at(plot.left() + plot.width() / 2));
        const double start = param(track, device, QStringLiteral("start"));
        QVERIFY2(std::abs(start - 50.0) <= 1.0, qPrintable(QString::number(start)));
        auto* startKnob = find(view, QStringLiteral("knob_start"));  // (the Controls page's)
        QVERIFY(startKnob);
        QCOMPARE(qvariant_cast<KnobItem*>(startKnob->property("knob"))->value(), start);
        editor()->setDeviceParam(track, device, QStringLiteral("end"), 30.0);
        // Grabs End (Start is under it).
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at(samples->xOf(30.0)));
        QCOMPARE(samples->marker(), QStringLiteral("end"));
        dragTo(window_, at(plot.left() + 5));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at(plot.left() + 5));
        QCOMPARE(param(track, device, QStringLiteral("end")), param(track, device, QStringLiteral("start")));
        undo()->undo();
        undo()->undo();
        QCOMPARE(param(track, device, QStringLiteral("end"), 100.0), 100.0);
        undo()->undo();
        QCOMPARE(param(track, device, QStringLiteral("start"), 0.0), 0.0);

        // Its sample is saved with the project, and comes back with it.
        const QString file = dir_.path(QStringLiteral("song.gil"));
        saveProject(*project(), file);
        loadProject(*project(), file);
        const std::vector<Device>& devices = project()->track(track).devices;
        QCOMPARE(devices.size(), size_t(1));
        QCOMPARE(devices.front().kind, QStringLiteral("sampler"));
        QCOMPARE(sub::app::deviceState::fromModel(devices.front().state),
                 (sub::app::deviceState::Values{{QStringLiteral("sample"), path}}));
        QCOMPARE(engineState(track, devices.front().id),
                 (sub::app::deviceState::Values{{QStringLiteral("sample"), path}}));
    }

    void samplerModesSlicesAndWarp() {
        // Four tones, a quarter of a second each.
        std::vector<float> wave;
        for (const double freq : {220.0, 330.0, 440.0, 550.0}) {
            const std::vector<float> part = tone(freq, kSampleRate / 4);
            wave.insert(wave.end(), part.begin(), part.end());
        }
        const QString path = sub::app::test::writeWav(dir_.path(QStringLiteral("tones.wav")), wave);
        auto [track, device, view, samples] = sampler();
        QVERIFY(view && samples);
        samples->loadSample(path);
        QTRY_VERIFY(samples->decoded());
        QQuickItem* samplePage = find(view, QStringLiteral("samplePage"));
        auto value = [&](const char* id) { return param(track, device, QString::fromLatin1(id)); };
        auto click = [&](QQuickItem* item) {
            QVERIFY(item && item->isVisible());
            QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(item));
        };
        auto button = [&](const QString& name) {
            QQuickItem* item = find(view, name);
            return item ? qvariant_cast<QQuickItem*>(item->property("button")) : nullptr;
        };
        auto lit = [&](const QString& name) { return find(view, name)->property("lit").toBool(); };

        // The mode tabs: a click, an undo step.
        QCOMPARE(samples->mode(), 0);
        QVERIFY(find(view, QStringLiteral("mode0"))->property("checked").toBool());
        const int steps = undo()->count();
        click(find(view, QStringLiteral("mode1")));
        QCOMPARE(value("mode"), 1.0);
        QCOMPARE(undo()->count(), steps + 1);
        QCOMPARE(samples->mode(), 1);
        QVERIFY(find(view, QStringLiteral("mode1"))->property("checked").toBool());
        // 1-Shot: its fades under the display; Trigger or Gate on it, no loop.
        QVERIFY(find(samplePage, QStringLiteral("cell_fade_in"))->isVisible());
        QVERIFY(!find(samplePage, QStringLiteral("cell_attack"))->isVisible());
        QVERIFY(!find(view, QStringLiteral("loop"))->isVisible());
        QVERIFY(lit(QStringLiteral("trigger")) && !lit(QStringLiteral("gate")));
        click(button(QStringLiteral("gate")));
        QCOMPARE(value("trigger"), 1.0);
        QVERIFY(lit(QStringLiteral("gate")) && !lit(QStringLiteral("trigger")));

        // Slice: cut into four regions, the slice a note plays lit.
        click(find(view, QStringLiteral("mode2")));
        QCOMPARE(samples->mode(), 2);
        auto* sliceBy = find(view, QStringLiteral("sliceBy"));
        QVERIFY(sliceBy->isVisible() && find(view, QStringLiteral("sensitivity"))->isVisible());
        QVERIFY(QMetaObject::invokeMethod(sliceBy, "choose", Q_ARG(QVariant, 2)));  // as the list does
        QCOMPARE(value("slice_by"), 2.0);
        QVERIFY(find(view, QStringLiteral("regions"))->isVisible() && !find(view, QStringLiteral("sensitivity"))->isVisible());
        editor()->setDeviceParam(track, device, QStringLiteral("regions"), 4.0);
        QCOMPARE(samples->slices(), (QList<qreal>{0.0, 0.25, 0.5, 0.75}));
        const auto clip = editor()->addMidiClip(track, 0.0, 4.0);
        QVERIFY(clip);
        editor()->setClipNotes(*clip, {Note{38, 0.0, 0.25, 127}}, QStringLiteral("Add Note"));  // D1: the third
        engine_->renderOffline(0.0, kSampleRate / 16);
        refreshDisplays();
        QCOMPARE(samples->playingSlice(), 2);
        // By beats: the sample's four beats in eighths.
        QVERIFY(QMetaObject::invokeMethod(sliceBy, "choose", Q_ARG(QVariant, 1)));
        QVERIFY(find(view, QStringLiteral("sliceBeat"))->isVisible());
        QCOMPARE(samples->slices().size(), 8);
        // Snap moves them to the zero crossings nearby: as many.
        editor()->setDeviceParam(track, device, QStringLiteral("snap"), 1.0);
        QCOMPARE(samples->slices().size(), 8);

        // Classic with its loop: Loop on the display, its fade beside it; drag Loop Start's marker.
        click(find(view, QStringLiteral("mode0")));
        QVERIFY(!find(view, QStringLiteral("loopFade"))->isVisible());
        click(button(QStringLiteral("loop")));
        QCOMPARE(value("loop"), 1.0);
        QVERIFY(find(view, QStringLiteral("loopFade"))->isVisible());
        editor()->setDeviceParam(track, device, QStringLiteral("snap"), 0.0);
        editor()->setDeviceParam(track, device, QStringLiteral("loop_start"), 50.0);
        const QRectF plot = samples->plot();
        auto at = [&](double x) { return scenePoint(samples, QPointF(x, plot.center().y())); };
        QCOMPARE(samples->markerAt(samples->xOf(50.0)), QStringLiteral("loop"));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at(samples->xOf(50.0)));
        QCOMPARE(samples->marker(), QStringLiteral("loop"));
        dragTo(window_, at(samples->xOf(60.0)));
        dragTo(window_, at(samples->xOf(75.0)));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at(samples->xOf(75.0)));
        QVERIFY2(std::abs(value("loop_start") - 75.0) <= 1.0, qPrintable(QString::number(value("loop_start"))));
        undo()->undo();
        QCOMPARE(value("loop_start"), 50.0);
        // Where it is on Start, its handle at the bottom grabs it; anywhere else on the line, Start.
        editor()->setDeviceParam(track, device, QStringLiteral("loop_start"), 0.0);
        QCOMPARE(samples->markerAt(samples->xOf(0.0), plot.center().y()), QStringLiteral("start"));
        QCOMPARE(samples->markerAt(samples->xOf(0.0) + 2, plot.bottom() - 3), QStringLiteral("loop"));

        // Warp: halved and doubled.
        QCOMPARE(find(view, QStringLiteral("warpBeats"))->property("box").value<QQuickItem*>()->property("text").toString(),
                 QStringLiteral("1 Bar"));
        click(find(view, QStringLiteral("double")));
        QCOMPARE(value("warp_beats"), 8.0);
        click(find(view, QStringLiteral("halve")));
        click(find(view, QStringLiteral("halve")));
        QCOMPARE(value("warp_beats"), 2.0);
        click(button(QStringLiteral("warp")));
        QCOMPARE(value("warp"), 1.0);

        // Reverse, from the device's menu: the waveform drawn as it plays.
        auto* reverse = view->findChild<QObject*>(QStringLiteral("reverseSample"));
        QVERIFY(reverse);
        QVERIFY(!reverse->property("checked").toBool());
        QVERIFY(QMetaObject::invokeMethod(reverse, "trigger"));
        QCOMPARE(value("reverse"), 1.0);
        QVERIFY(reverse->property("checked").toBool());
        QVERIFY(QMetaObject::invokeMethod(reverse, "trigger"));
        QCOMPARE(value("reverse"), 0.0);
        QVERIFY(!reverse->property("checked").toBool());
    }

    // --- The Delay ----------------------------------------------------------------------------

    void delay() {
        const QString track = editor()->addMidiTrack();
        const QString device = editor()->addDevice(track, QStringLiteral("delay"));
        QQuickItem* view = show(QStringLiteral("delay"), track, device);
        QVERIFY(view);
        QVERIFY(view->implicitHeight() <= bodyHeight());  // fits the view
        auto value = [&](const char* id) { return param(track, device, QString::fromLatin1(id)); };
        auto button = [&](const QString& name) {
            QQuickItem* item = find(view, name);
            return item ? qvariant_cast<QQuickItem*>(item->property("button")) : nullptr;
        };
        auto lit = [&](const QString& name) { return find(view, name)->property("lit").toBool(); };

        // The sixteenths are exclusive buttons; a click sets the parameter, undoably.
        QCOMPARE(itemsNamed(view, QStringLiteral("lDivision")).size(), 8);
        QVERIFY(lit(QStringLiteral("lDivision3")));  // the default: 3 sixteenths
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(button(QStringLiteral("lDivision5"))));
        QCOMPARE(value("l_division"), 4.0);
        QVERIFY(lit(QStringLiteral("lDivision5")) && !lit(QStringLiteral("lDivision3")));
        undo()->undo();
        QVERIFY(!lit(QStringLiteral("lDivision5")));

        // Sync off shows the time knob instead of the grid; Link turns the right side off.
        QVERIFY(find(view, QStringLiteral("lSynced"))->isVisible());
        editor()->setDeviceParam(track, device, QStringLiteral("l_sync"), 0.0);
        QVERIFY(!find(view, QStringLiteral("lSynced"))->isVisible());
        QVERIFY(find(view, QStringLiteral("lTime"))->isVisible());
        QVERIFY(find(view, QStringLiteral("rightSide"))->isEnabled());
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(button(QStringLiteral("link"))));
        QCOMPARE(value("link"), 1.0);
        QVERIFY(!find(view, QStringLiteral("rightSide"))->isEnabled());
        QVERIFY(!button(QStringLiteral("rDivision5"))->isEnabled());

        // Mode buttons choose one; the graph drags the filter's frequency and width.
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(button(QStringLiteral("modeFade"))));
        QCOMPARE(value("mode"), 1.0);
        auto* graph = find<FilterGraph>(view, QStringLiteral("filterGraph"));
        QVERIFY(graph);
        const int steps = undo()->count();
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
                          scenePoint(graph, QPointF(graph->width() / 2, graph->height() / 2)));
        QVERIFY2(400 < value("freq") && value("freq") < 1000, qPrintable(QString::number(value("freq"))));
        QVERIFY2(4.0 < value("width") && value("width") < 6.0, qPrintable(QString::number(value("width"))));
        QCOMPARE(undo()->count(), steps + 1);
        QVERIFY(std::abs(FilterGraph::response(1000.0, 1000.0, 8.0)) < 0.01);
        QVERIFY(std::abs(FilterGraph::response(1000.0 * 16, 1000.0, 8.0) + 3.0) < 0.1);  // at a corner

        // The frequency box drags in log, and reads "k".
        auto* freq = qvariant_cast<ValueBoxItem*>(find(view, QStringLiteral("freq"))->property("box"));
        QVERIFY(freq && freq->logScale());
        QVERIFY(freq->applyTyped(QStringLiteral("2.5k")));
        QCOMPARE(value("freq"), 2500.0);
        QCOMPARE(freq->text(), QStringLiteral("2.50 kHz"));
        {
            const QPoint at = centerOf(freq);
            const int before = undo()->count();
            QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
            for (int dy = 20; dy <= 60; dy += 20)
                dragTo(window_, at - QPoint(0, dy));
            QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 60));
            // 60 px of the 600 for the whole range, in log.
            const double expected = 2500.0 * std::pow(18000.0 / 50.0, 60.0 / 600.0);
            QVERIFY2(std::abs(value("freq") / expected - 1) < 0.01, qPrintable(QString::number(value("freq"))));
            QCOMPARE(undo()->count(), before + 1);
        }

        // The spectrum of the input shows behind the filter's curve: a 1 kHz tone peaks at 1 kHz.
        graph->addSamples(tone(1000.0, 8192), 48000.0);
        std::vector<double> columns = graph->spectrum().columns(200, FilterGraph::kLow, FilterGraph::kHigh);
        const auto loudest = std::max_element(columns.begin(), columns.end());
        const double peak = 20.0 * std::pow(1000.0, (double(loudest - columns.begin()) + 0.5) / 200);
        QVERIFY2(900 < peak && peak < 1100, qPrintable(QString::number(peak)));
        QVERIFY(std::abs(*loudest - 20 * std::log10(0.5)) < 1.0);
        graph->addSamples({}, 48000.0);  // nothing new: it falls back slowly
        columns = graph->spectrum().columns(200, FilterGraph::kLow, FilterGraph::kHigh);
        QVERIFY(std::abs(*std::max_element(columns.begin(), columns.end()) - (*loudest - 1.0)) < 0.01);
        QTest::qWait(50);
        save(grab(), QStringLiteral("delay-linked.png"));
    }

    // --- The EQ --------------------------------------------------------------------------------

    void eq() {
        const QString track = editor()->addMidiTrack();
        const QString device = editor()->addDevice(track, QStringLiteral("eq"));
        QQuickItem* view = show(QStringLiteral("eq"), track, device);
        QVERIFY(view);
        QVERIFY(view->implicitHeight() <= bodyHeight());  // fits the view
        auto* graph = find<EqGraph>(view, QStringLiteral("eqGraph"));
        auto* panel = qvariant_cast<QQuickItem*>(view->property("panel"));
        QVERIFY(graph && panel);
        auto value = [&](const QString& id) { return param(track, device, id); };
        auto band = [&](int index) { return graph->bands()[size_t(index)]; };
        const EqView* shared = EqView::instance();

        // The band's controls start collapsed; the button beside the expand button shows them (the device widens).
        auto* panelButton = find(view, QStringLiteral("panelButton"));
        QVERIFY(!panel->isVisible() && view->implicitWidth() == 580);
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(panelButton));
        QVERIFY(panel->isVisible() && view->implicitWidth() == 580 + 8 + 162 + 6 && shared->panel());
        QVERIFY(fitted());
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(panelButton));
        QVERIFY(!panel->isVisible() && view->implicitWidth() == 580);
        QVERIFY(fitted());
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(panelButton));  // (shown for the rest)
        QVERIFY(fitted());
        QTest::qWait(50);  // laid out: the graph at its size
        QCOMPARE(graph->width(), 580.0);

        // Silence (or a spectrum falling back to it) draws flat along the floor: the tilt doesn't lift its top end.
        {
            std::vector<double> freqs(200);
            for (int i = 0; i < 200; ++i)
                freqs[size_t(i)] = 20.0 * std::pow(1000.0, i / 199.0);
            for (double db : sub::app::analysis::EqAnalyzer().columns(sub::app::analysis::EqAnalyzer::Input, freqs))
                QVERIFY(std::abs(db - sub::app::analysis::EqAnalyzer::kFloorDb) < 1e-9);
        }

        auto point = [&](double freq, double db) { return scenePoint(graph, QPointF(graph->xOf(freq), graph->yOf(db))); };
        auto at = [&](QPointF local) { return scenePoint(graph, local); };

        // Hovering the (flat) curve shows a ghost band; a click adds it, of the type for where it is.
        QTest::mouseMove(window_, point(1000.0, 0.0));
        QVERIFY(graph->ghost() && graph->ghost()->second == EqGraph::Bell);
        QTest::mouseMove(window_, point(1000.0, 9.0));  // off the curve: nothing to add
        QVERIFY(!graph->ghost());
        QTest::mouseMove(window_, point(1000.0, 0.0));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, point(1000.0, 0.0));
        dragTo(window_, point(2000.0, 6.0));  // and dragging it on is the same step
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, point(2000.0, 6.0));
        QCOMPARE(value("b1_used"), 1.0);
        QCOMPARE(value("b1_type"), double(EqGraph::Bell));
        QCOMPARE(graph->selected(), 0);
        QVERIFY(std::abs(value("b1_freq") / 2000.0 - 1) < 0.03 && std::abs(value("b1_gain") - 6.0) < 0.3);
        undo()->undo();
        QCOMPARE(value("b1_used"), 0.0);
        QVERIFY(!band(0));
        undo()->redo();

        for (const auto& [fraction, kind] : {std::pair{0.03, int(EqGraph::LowCut)}, std::pair{0.15, int(EqGraph::LowShelf)},
                                             std::pair{0.85, int(EqGraph::HighShelf)}, std::pair{0.97, int(EqGraph::HighCut)}}) {
            const QRectF plot = graph->plot();
            const double x = std::round(plot.left() + fraction * plot.width());
            const QPointF where(x, std::round(graph->curveY(x)));
            QTest::mouseMove(window_, at(where));
            QVERIFY(graph->ghost() && graph->ghost()->second == kind);
            QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, at(where));
        }
        QList<int> types;
        for (int i = 0; i < 5; ++i)
            types << (band(i) ? band(i)->type : -1);
        QCOMPARE(types, (QList<int>{EqGraph::Bell, EqGraph::LowCut, EqGraph::LowShelf, EqGraph::HighShelf, EqGraph::HighCut}));
        QCOMPARE(value("b2_slope"), 3.0);  // cuts start at 24 dB/octave
        QTest::qWait(50);
        save(grab(), QStringLiteral("eq-five-bands.png"));

        // Dragging a band moves it (one undo step); the wheel sets its Q.
        QPoint dot = at(graph->dot(*band(0)));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, dot);
        for (int step = 1; step < 6; ++step)
            dragTo(window_, dot + QPoint(0, 4 * step));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, dot + QPoint(0, 20));
        QVERIFY(value("b1_gain") < 6.0 - 2.0);
        QVERIFY(std::abs(value("b1_freq") / 2000.0 - 1) < 0.03);
        undo()->undo();
        QVERIFY(std::abs(value("b1_gain") - 6.0) < 0.3);
        double q = value("b1_q");
        wheel(window_, at(graph->dot(*band(0))), 120);
        QVERIFY(std::abs(value("b1_q") / (q * 1.15) - 1) < 0.01);

        // The wheel while dragging a cut sets its slope instead.
        const QPoint cut = at(graph->dot(*band(1)));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, cut);
        q = value("b2_q");
        wheel(window_, cut, 120);
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, cut);
        QCOMPARE(value("b2_slope"), 4.0);  // 24 dB/octave to 30
        QCOMPARE(value("b2_q"), q);

        // Double-click switches a band off and on; Alt-click deletes it; so does Delete.
        dot = at(graph->dot(*band(0)));
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, dot);
        QCOMPARE(value("b1_on"), 0.0);
        QVERIFY(!band(0)->on);
        QTest::mouseClick(window_, Qt::LeftButton, Qt::AltModifier, dot);
        QVERIFY(!band(0));
        graph->select(1);
        QVERIFY(graph->hasActiveFocus());
        QTest::keyClick(window_, Qt::Key_Delete);
        QVERIFY(!band(1));
        QCOMPARE(graph->selected(), -1);

        // The wheel's Q holds as the drag goes on: for a band the drag added (whose every move sets all its
        // parameters), and for a notch (whose drag sets its Q). Adding and changing it is still one step.
        for (const auto& [kind, freq] : {std::pair{int(EqGraph::Bell), 500.0}, std::pair{int(EqGraph::Notch), 300.0}}) {
            const double x = std::round(graph->xOf(freq));
            const QPointF where(x, std::round(graph->curveY(x)));
            QTest::mouseMove(window_, at(where));
            QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at(where));
            const int index = graph->selected();
            QVERIFY(index >= 0);
            if (kind == EqGraph::Notch)
                graph->setType(index, EqGraph::Notch);
            const double start = value(EqGraph::param(index, QStringLiteral("q")));
            wheel(window_, at(where), 240);
            for (int step = 1; step < 4; ++step)
                dragTo(window_, at(where + QPointF(5 * step, 0)));
            QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at(where + QPointF(15, 0)));
            const double now = value(EqGraph::param(index, QStringLiteral("q")));
            QVERIFY2(std::abs(now / (start * 1.15 * 1.15) - 1) < 0.01, qPrintable(QString::number(now)));
            if (kind == EqGraph::Bell) {
                undo()->undo();
                QVERIFY(!band(index));
            } else {
                graph->removeBand(index);
            }
        }

        // The panel edits the selected band: its type buttons, knobs and slope.
        graph->select(2);
        auto* controls = find(panel, QStringLiteral("bandControls"));
        QVERIFY(controls->isVisible());
        auto typeChecked = [&](int kind) {
            auto* button = find(panel, QStringLiteral("type") + QString::number(kind));
            return qvariant_cast<QQuickItem*>(button->property("background"))->property("checked").toBool();
        };
        QVERIFY(typeChecked(EqGraph::LowShelf));
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
                          centerOf(find(panel, QStringLiteral("type") + QString::number(EqGraph::TiltShelf))));
        QCOMPARE(value("b3_type"), double(EqGraph::TiltShelf));
        QVERIFY(typeChecked(EqGraph::TiltShelf) && !typeChecked(EqGraph::LowShelf));
        auto* freqKnob = qvariant_cast<KnobItem*>(find(panel, QStringLiteral("bandKnob_freq"))->property("knob"));
        Q_EMIT freqKnob->moved(150.0, newGestureKey());
        QCOMPARE(value("b3_freq"), 150.0);
        QCOMPARE(freqKnob->value(), 150.0);
        QVERIFY(QMetaObject::invokeMethod(find(panel, QStringLiteral("bandSlope")), "activated", Q_ARG(int, 5)));
        QCOMPARE(value("b3_slope"), 5.0);
        graph->select(-1);
        QVERIFY(!controls->isVisible());

        // With 24 bands there is no ghost to add another.
        {
            sub::app::OrderedMap<QString, double> all;
            for (int i = 0; i < EqGraph::kBands; ++i)
                all.insert(EqGraph::param(i, QStringLiteral("used")), 1.0);
            graph->set(all);
        }
        QCOMPARE(graph->freeBand(), -1);
        QTest::mouseMove(window_, at(QPointF(5, 5)));
        QVERIFY(!graph->ghost());

        // The analyzer: a 1 kHz tone peaks at 1 kHz.
        const std::vector<float> sine = tone(1000.0, sub::app::analysis::EqAnalyzer::kFftSize);
        for (int i = 0; i < 20; ++i)
            graph->analyzer().feed(sub::app::analysis::EqAnalyzer::Output, sine.data(), sine.size(), 48000.0);
        const std::vector<double> freqs = graph->columnFreqs();
        const std::vector<double> levels = graph->analyzer().columns(sub::app::analysis::EqAnalyzer::Output, freqs);
        const double peak = freqs[size_t(std::max_element(levels.begin(), levels.end()) - levels.begin())];
        QVERIFY2(900 < peak && peak < 1100, qPrintable(QString::number(peak)));

        // The window shows the same, and goes with the device.
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(find(view, QStringLiteral("expand"))));
        QQuickWindow* eqWindow = eqWindowOf(track, device);
        QVERIFY(eqWindow && eqWindow->isVisible());
        QVERIFY(QTest::qWaitForWindowExposed(eqWindow));
        auto* windowGraph = find<EqGraph>(eqWindow->contentItem(), QStringLiteral("eqGraph"));
        QVERIFY(windowGraph && windowGraph->bands()[3]);
        QCOMPARE(eqWindow->title(), QStringLiteral("EQ — ") + project()->track(track).name);
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(find(view, QStringLiteral("expand"))));
        QCOMPARE(eqWindowOf(track, device), eqWindow);
        windowGraph->setParamValue(QStringLiteral("output"), 3.0);
        QCOMPARE(value("output"), 3.0);
        auto* output = qvariant_cast<KnobItem*>(find(view, QStringLiteral("corner_output"))->property("knob"));
        QCOMPARE(output->value(), 3.0);
        save(eqWindow->grabWindow(), QStringLiteral("eq-window-24-bands.png"));
        const QPointer<QQuickWindow> gone(eqWindow);
        editor()->removeDevice(track, device);
        QTRY_VERIFY(!gone);
        QCOMPARE(eqWindowOf(track, device), nullptr);
        // Gone, it hears nothing more.
        Q_EMIT bridge()->automationStateChanged(track);
        editor()->deleteTracks({track});
        QCoreApplication::processEvents();
    }

    // --- The Sidechain -----------------------------------------------------------------------

    // A kick (short, every 4096 samples) and a bass at 55 Hz, each on a track of its own; a Sidechain on the
    // bass, its editor shown: (kick track, bass track, device, editor).
    std::tuple<QString, QString, QString, QQuickItem*> kickAndBass(int period = 4096, double seconds = 2.0) {
        const int frames = int(seconds * kSampleRate);
        std::vector<float> one(static_cast<size_t>(period)), kick(static_cast<size_t>(frames)),
            bass(static_cast<size_t>(frames));
        double phase = 0.0;
        for (int i = 0; i < period; ++i) {
            const double t = double(i) / kSampleRate;
            phase += 50 + 100 * std::exp(-t / 0.01);
            one[size_t(i)] = float(0.9 * std::sin(2 * kPi * phase / kSampleRate) * std::exp(-t / 0.02));
        }
        for (int i = 0; i < frames; ++i) {
            kick[size_t(i)] = one[size_t(i % period)];
            bass[size_t(i)] = float(0.4 * std::sin(2 * kPi * 55 * (double(i) / kSampleRate)));
        }
        const QString kickPath = sub::app::test::writeWav(dir_.path(QStringLiteral("kick.wav")), stereo(kick), 2);
        const QString bassPath = sub::app::test::writeWav(dir_.path(QStringLiteral("bass.wav")), stereo(bass), 2);
        const QString kickTrack = editor()->addClips(QString(), 0.0, {{kickPath, seconds}}, 0).first().trackId;
        const QString bassTrack = editor()->addClips(QString(), 0.0, {{bassPath, seconds}}, 1).first().trackId;
        const QString device = editor()->addDevice(bassTrack, QStringLiteral("sidechain"));
        return {kickTrack, bassTrack, device, show(QStringLiteral("sidechain"), bassTrack, device)};
    }

    void sidechain() {
        auto [kickTrack, bassTrack, device, view] = kickAndBass();
        QVERIFY(view);
        QVERIFY(view->implicitHeight() <= bodyHeight());  // fits the view
        auto* graph = find<CurveGraph>(view, QStringLiteral("curveGraph"));
        QVERIFY(graph);
        auto value = [&](const QString& id) { return param(bassTrack, device, id); };
        auto screen = [&](double x, double y) {
            const QPointF at = graph->toScreen(x, y);
            return scenePoint(graph, QPointF(std::round(at.x()), std::round(at.y())));
        };
        auto points = [&] { return graph->points(); };

        // The default curve: ducked at once, held, then back up.
        QCOMPARE(points().size(), size_t(3));
        QVERIFY(points()[0].x == 0.0 && points()[0].y == 0.0 && std::abs(points()[1].x - 0.1) < 1e-6 &&
                points()[1].y == 0.0 && points()[2].x == 1.0 && points()[2].y == 1.0);
        QVERIFY(!graph->hint().isEmpty());  // no sidechain yet: it says to choose one
        save(grab(), QStringLiteral("sidechain-empty.png"));
        // Its hint asks the device's frame for the sidechain menu.
        {
            QSignalSpy asked(view, SIGNAL(sidechainMenuRequested()));
            QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
                              scenePoint(graph, QPointF(graph->plot().center().x(), graph->plot().center().y())));
            QCOMPARE(asked.count(), 1);
            QCOMPARE(points().size(), size_t(3));  // (not a click on the curve)
        }

        // A click off the curve adds a point, and dragging it on is the same step.
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, screen(0.5, 0.95));
        dragTo(window_, screen(0.6, 0.9));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, screen(0.6, 0.9));
        QCOMPARE(points().size(), size_t(4));
        QVERIFY(std::abs(points()[2].x - 0.6) < 0.01 && std::abs(points()[2].y - 0.9) < 0.02);
        QCOMPARE(value(QStringLiteral("p4_used")), 1.0);
        undo()->undo();
        QCOMPARE(points().size(), size_t(3));
        undo()->redo();

        // The first point only moves up and down; a point between moves between its neighbours.
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, screen(0.0, 0.0));
        dragTo(window_, screen(0.3, 0.4));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, screen(0.3, 0.4));
        QVERIFY(points()[0].x == 0.0 && std::abs(points()[0].y - 0.4) < 0.02);
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, screen(0.6, 0.9));
        dragTo(window_, screen(0.95, 0.9));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, screen(0.95, 0.9));
        QVERIFY(std::abs(points()[2].x - 0.95) < 0.01);

        // Dragging between points bends the curve there; the wheel too; a double-click straightens it.
        std::vector<CurveGraph::Point> before = points();
        const double middle = (before[1].x + before[2].x) / 2;
        QPoint at = screen(middle, CurveGraph::curveValue(before, middle));
        QCOMPARE(graph->hit(graph->mapFromScene(at)), (std::pair{QStringLiteral("segment"), 1}));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(window_, at - QPoint(0, 30));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 30));
        const double bent = points()[1].curve;
        QVERIFY2(bent > before[1].curve + 0.4, qPrintable(QString::number(bent)));  // up: bulging up
        at = screen(middle, CurveGraph::curveValue(points(), middle));
        wheel(window_, at, -120);
        QVERIFY(std::abs(points()[1].curve - (bent - 0.08)) < 1e-9);
        at = screen(middle, CurveGraph::curveValue(points(), middle));
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(points()[1].curve, 0.0);

        // Double-click (or Alt-click) a point to remove it; the ends stay.
        const size_t count = points().size();
        const CurveGraph::Point p2 = points()[2];
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, screen(p2.x, p2.y));
        QCOMPARE(points().size(), count - 1);
        QTest::mouseClick(window_, Qt::LeftButton, Qt::AltModifier, screen(1.0, 1.0));
        QCOMPARE(points().size(), count - 1);
        graph->flip();
        QCOMPARE(points().back().y, 0.0);  // flipped: the end swells instead

        // The right-click menu: an inner point's Delete Point, the shapes, fit, flip, reset.
        {
            auto* menu = qvariant_cast<QObject*>(view->property("curveMenu"));
            QVERIFY(menu);
            QMetaObject::invokeMethod(menu, "build", Q_ARG(QVariant, 1));
            QStringList texts;
            for (int i = 0; i < menu->property("count").toInt(); ++i) {
                QQuickItem* item = nullptr;
                QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, i));
                texts << (item ? item->property("text").toString() : QString());
            }
            QCOMPARE(texts, (QStringList{"Delete Point", "", "Shapes", "Fit to Kick", "Flip Curve", "", "Reset Curve"}));
            graph->applyShape(1);  // Smooth
            QCOMPARE(points().size(), size_t(2));
            QCOMPARE(undo()->undoText(), QStringLiteral("Sidechain Shape: Smooth"));
            undo()->undo();
        }

        // The controls: the knobs set their parameters; Sync shows the synced length.
        auto* threshold = qvariant_cast<KnobItem*>(find(view, QStringLiteral("knob_threshold"))->property("knob"));
        Q_EMIT threshold->moved(-30.0, newGestureKey());
        QCOMPARE(value(QStringLiteral("threshold")), -30.0);
        QCOMPARE(undo()->undoText(), QStringLiteral("Change Threshold"));
        auto* thresholdParam = qvariant_cast<sub::ui::DeviceParam*>(find(view, QStringLiteral("knob_threshold"))->property("param"));
        QCOMPARE(thresholdParam->text(), QStringLiteral("-30.0 dB"));
        auto clickButton = [&](const QString& name) {
            QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
                              centerOf(qvariant_cast<QQuickItem*>(find(view, name)->property("button"))));
        };
        clickButton(QStringLiteral("sync"));
        QCOMPARE(value(QStringLiteral("sync")), 1.0);
        QVERIFY(find(view, QStringLiteral("knob_rate"))->isVisible() && !find(view, QStringLiteral("knob_length"))->isVisible());
        QVERIFY(std::abs(graph->lengthMs() - 500.0) < 1e-9);  // 1/4 at 120
        graph->setParamValue(QStringLiteral("rate"), 7.0);  // 1 Bar
        editor()->setTimeSignature(TimeSignature{3, 4});
        QVERIFY(std::abs(graph->lengthMs() - 1500.0) < 1e-9);  // a bar of 3/4, as the engine has it
        undo()->undo();
        undo()->undo();
        clickButton(QStringLiteral("sync"));
        auto* crossover = find(view, QStringLiteral("knob_crossover"));
        QVERIFY(!crossover->isEnabled());
        clickButton(QStringLiteral("lowsOnly"));
        QCOMPARE(value(QStringLiteral("range")), 1.0);
        QVERIFY(crossover->isEnabled());

        // With the kick as its sidechain, hits come (one each 4096 samples), and the fit follows the kick.
        editor()->setDeviceSidechain(bassTrack, device, Sidechain{kickTrack, kPreFader});
        QVERIFY(graph->hint().isEmpty());
        const int period = 4096;
        const double beatsPerChunk = period / (kSampleRate * 60.0 / project()->tempo());
        for (int chunk = 0; chunk < 12; ++chunk) {  // (each render is reset at its start: on a hit)
            engine_->renderOffline(chunk * beatsPerChunk, period);
            graph->readDisplays();
        }
        QCOMPARE(graph->hitCount(), 12);
        QVERIFY(graph->fit());
        const sub::app::sidechainFit::Fit fit = *graph->fit();
        QVERIFY(fit.spectra.bassHeard && fit.spectra.low < 55 && 55 < fit.spectra.high);
        QVERIFY(!graph->trail().empty() && graph->flash() > 0);  // the playhead rode the curve
        QTest::qWait(50);
        save(grab(), QStringLiteral("sidechain-fit.png"));

        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(find(view, QStringLiteral("fit"))));
        QCOMPARE(points().size(), fit.points.size());
        for (size_t i = 0; i < fit.points.size(); ++i)
            QVERIFY(std::abs(points()[i].x - fit.points[i].x) < 1e-4 && std::abs(points()[i].y - fit.points[i].y) < 1e-4);
        QVERIFY(std::abs(value(QStringLiteral("length")) - fit.length) < 1e-9);
        QCOMPARE(value(QStringLiteral("sync")), 0.0);
        undo()->undo();  // one step
        QVERIFY(points().size() != fit.points.size() || std::abs(value(QStringLiteral("length")) - fit.length) > 1e-9);
        undo()->redo();

        // Auto fits again at every hit, all in one step.
        QVERIFY(QMetaObject::invokeMethod(find(view, QStringLiteral("character")), "activated", Q_ARG(int, 2)));
        QCOMPARE(value(QStringLiteral("character")), 2.0);
        const sub::app::sidechainFit::Fit loose = *graph->fit();
        QVERIFY(loose.length > fit.length);  // Loose: out of the way for longer
        const int steps = undo()->count();
        clickButton(QStringLiteral("auto"));
        QCOMPARE(value(QStringLiteral("autofit")), 1.0);
        QVERIFY(std::abs(value(QStringLiteral("length")) - loose.length) < 1e-9);
        for (int chunk = 12; chunk < 16; ++chunk) {
            engine_->renderOffline(chunk * beatsPerChunk, period);
            graph->readDisplays();
        }
        QCOMPARE(undo()->count(), steps + 2);  // switching it on, then the fits (merged)
        clickButton(QStringLiteral("auto"));
        QCOMPARE(value(QStringLiteral("autofit")), 0.0);
    }

    // --- The parameter cell the device view shows for devices without an editor ----------------------

    QQuickItem* showKnob(const QString& track, const QString& device, const QString& paramId) {
        QVariant item;
        QMetaObject::invokeMethod(root_.get(), "showKnob", Q_RETURN_ARG(QVariant, item), Q_ARG(QVariant, track),
                                  Q_ARG(QVariant, device), Q_ARG(QVariant, paramId));
        fitted();
        QTest::qWait(30);
        return qvariant_cast<QQuickItem*>(item);
    }

    static QStringList menuTexts(QObject* menu) {
        QStringList texts;
        for (int i = 0; i < menu->property("count").toInt(); ++i) {
            QQuickItem* item = nullptr;
            QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, i));
            texts << (item ? item->property("text").toString() : QString());
        }
        return texts;
    }

    static QQuickItem* menuItem(QObject* menu, const QString& text) {
        for (int i = 0; i < menu->property("count").toInt(); ++i) {
            QQuickItem* item = nullptr;
            QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, i));
            if (item && item->property("text").toString() == text)
                return item;
        }
        return nullptr;
    }

    // Chooses a menu's entry, as a click on it does (its action triggers).
    static void trigger(QObject* menu, const QString& text) {
        QQuickItem* item = menuItem(menu, text);
        QVERIFY(item);
        auto* action = qvariant_cast<QObject*>(item->property("action"));
        QVERIFY(action);
        QVERIFY(QMetaObject::invokeMethod(action, "trigger"));
    }

    void deviceParamKnob() {
        const QString track = editor()->addAudioTrack();
        const QString device = editor()->addDevice(track, QStringLiteral("utility"));
        const QString key = automation::deviceKey(device, QStringLiteral("gain"));
        QQuickItem* cell = showKnob(track, device, QStringLiteral("gain"));
        QVERIFY(cell);
        auto* p = qvariant_cast<sub::ui::DeviceParam*>(cell->property("param"));
        auto* knob = qvariant_cast<KnobItem*>(cell->property("knob"));
        QVERIFY(p && knob && knob->isVisible());
        QCOMPARE(p->name(), QStringLiteral("Gain"));
        QCOMPARE(p->text(), QStringLiteral("0.0 dB"));
        QVERIFY(knob->from() == -60.0 && knob->to() == 24.0 && !knob->bipolar());
        QCOMPARE(cell->implicitWidth(), 84.0);  // PARAM_WIDTH

        // Dragging it sets the parameter, one undo step a drag; pressing it shows its automation.
        QSignalSpy touched(editor(), &ProjectEditor::parameterTouched);
        const int steps = undo()->count();
        const QPoint at = centerOf(knob);
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        for (int dy = 20; dy <= 60; dy += 20)
            dragTo(window_, at - QPoint(0, dy));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 60));
        QVERIFY(!touched.isEmpty());
        QCOMPARE(touched.first().at(0).toString(), track);
        QCOMPARE(touched.first().at(1).toString(), key);
        QVERIFY(std::abs(param(track, device, QStringLiteral("gain")) - 84.0 * 60 / 600) < 1e-6);
        QCOMPARE(undo()->count(), steps + 1);
        QCOMPARE(p->text(), QStringLiteral("8.4 dB"));
        undo()->undo();
        QCOMPARE(knob->value(), 0.0);

        // Its menu: nothing automated yet.
        auto* menu = qvariant_cast<QObject*>(cell->property("menu"));
        QVERIFY(menu);
        QMetaObject::invokeMethod(menu, "build");
        QCOMPARE(menuTexts(menu), (QStringList{"Show Automation", "Delete Automation"}));
        QVERIFY(menuItem(menu, QStringLiteral("Show Automation"))->isEnabled());
        QVERIFY(!menuItem(menu, QStringLiteral("Delete Automation"))->isEnabled());
        trigger(menu, QStringLiteral("Show Automation"));
        QCOMPARE(project()->automationView(track).key.value_or(QString()), key);

        // Automated: the red dot, the envelope's value; Delete Automation takes it away.
        editor()->setEnvelope(track, key, {{0.0, 0.5, 0.0}, {16.0, 0.5, 0.0}});
        QCOMPARE(p->automation(), QStringLiteral("on"));
        QCOMPARE(knob->automation(), QStringLiteral("on"));
        QVERIFY(std::abs(p->value() - (-60.0 + 0.5 * 84.0)) < 1e-6);
        QMetaObject::invokeMethod(menu, "build");
        QVERIFY(menuItem(menu, QStringLiteral("Delete Automation"))->isEnabled());
        trigger(menu, QStringLiteral("Delete Automation"));
        QVERIFY(project()->envelope(track, key).empty());
        QCOMPARE(p->automation(), QString());
        QCOMPARE(p->value(), 0.0);

        // Overridden: the grey dot, and Re-Enable Automation.
        editor()->setEnvelope(track, key, {{0.0, 0.5, 0.0}, {16.0, 0.5, 0.0}});
        bridge()->overrideAutomation(track, key);
        QCOMPARE(p->automation(), QStringLiteral("off"));
        QCOMPARE(knob->automation(), QStringLiteral("off"));
        QMetaObject::invokeMethod(menu, "build");
        QCOMPARE(menuTexts(menu), (QStringList{"Show Automation", "Delete Automation", "Re-Enable Automation"}));
        trigger(menu, QStringLiteral("Re-Enable Automation"));
        QCOMPARE(p->automation(), QStringLiteral("on"));
        save(grab(), QStringLiteral("param-knob-automated.png"));
        editor()->clearEnvelope(track, key);

        // In a rack, its macros can move it.
        const QString rack = editor()->groupDevices(track, {device});
        QVERIFY(!rack.isEmpty());
        QCOMPARE(p->rackId(), rack);
        QMetaObject::invokeMethod(menu, "build");
        QCOMPARE(menuTexts(menu), (QStringList{"Show Automation", "Delete Automation", "", "Map to Macro"}));
        p->mapToMacro(2);
        QCOMPARE(p->macro(), 2);
        QMetaObject::invokeMethod(menu, "build");
        QCOMPARE(menuTexts(menu).last(), QStringLiteral("Unmap from Macro 3"));
        trigger(menu, QStringLiteral("Unmap from Macro 3"));
        QCOMPARE(p->macro(), -1);

        // A parameter choosing between named values: a list.
        const QString keys = editor()->addMidiTrack();
        const QString sampler = editor()->addDevice(keys, QStringLiteral("sampler"));
        QQuickItem* loop = showKnob(keys, sampler, QStringLiteral("loop"));
        auto* list = qvariant_cast<QQuickItem*>(loop->property("list"));
        QVERIFY(list && list->isVisible() && !qvariant_cast<QQuickItem*>(loop->property("knob"))->isVisible());
        QCOMPARE(list->property("currentText").toString(), QStringLiteral("Off"));
        QVERIFY(QMetaObject::invokeMethod(list, "activated", Q_ARG(int, 1)));
        QCOMPARE(param(keys, sampler, QStringLiteral("loop")), 1.0);
        QCOMPARE(list->property("currentText").toString(), QStringLiteral("On"));
        // A stepped one turns in whole steps.
        QQuickItem* root = showKnob(keys, sampler, QStringLiteral("root"));
        QCOMPARE(qvariant_cast<KnobItem*>(root->property("knob"))->step(), 1.0);
    }

    // --- Screenshots with something to show ------------------------------------------------------------

    void screenshots() {
        if (qEnvironmentVariable("SUBSTATION_UI_SCREENSHOTS").isEmpty())
            QSKIP("SUBSTATION_UI_SCREENSHOTS not set");
        // Music, of a sort: a bass line's harmonics, a hat's noise, a kick every half a second.
        const int frames = kSampleRate * 2;
        std::vector<float> music(static_cast<size_t>(frames));
        quint32 seed = 12345;
        for (int i = 0; i < frames; ++i) {
            const double t = double(i) / kSampleRate, beat = std::fmod(t, 0.5);
            seed = seed * 1664525u + 1013904223u;
            const double noise = (double(seed >> 8) / double(1 << 24) - 0.5) * std::exp(-std::fmod(t, 0.25) / 0.02);
            double v = 0.6 * std::sin(2 * kPi * (50 + 100 * std::exp(-beat / 0.02)) * beat) * std::exp(-beat / 0.12);
            for (int h = 1; h <= 6; ++h)
                v += 0.12 / h * std::sin(2 * kPi * 110.0 * h * t);
            music[size_t(i)] = float(v + 0.15 * noise);
        }
        const QString track = audioTrackWith(music, QStringLiteral("music"), 2.0);

        // The Compressor, squashing it.
        const QString compressor = editor()->addDevice(track, QStringLiteral("compressor"));
        editor()->setDeviceParam(track, compressor, QStringLiteral("threshold"), -24.0);
        editor()->setDeviceParam(track, compressor, QStringLiteral("makeup"), 6.0);
        QVERIFY(show(QStringLiteral("compressor"), track, compressor));
        const int chunk = 2048;
        const double beatsPerChunk = chunk / (kSampleRate * 60.0 / project()->tempo());
        for (int i = 0; i < 40; ++i) {
            engine_->renderOffline(i * beatsPerChunk, chunk);
            refreshDisplays();
        }
        QTest::qWait(50);
        save(grab(), QStringLiteral("compressor.png"));
        editor()->setDeviceEnabled(track, compressor, false);

        // The Delay, its filter over the input's spectrum.
        const QString delay = editor()->addDevice(track, QStringLiteral("delay"));
        editor()->setDeviceParam(track, delay, QStringLiteral("freq"), 2200.0);
        editor()->setDeviceParam(track, delay, QStringLiteral("width"), 4.5);
        QVERIFY(show(QStringLiteral("delay"), track, delay));
        for (int i = 0; i < 12; ++i) {
            engine_->renderOffline(i * beatsPerChunk, chunk);
            refreshDisplays();
        }
        QTest::qWait(50);
        save(grab(), QStringLiteral("delay.png"));
        editor()->setDeviceEnabled(track, delay, false);

        // The EQ: a cut, a shelf, a bell cut, a band selected, the analyzer before and after.
        const QString eq = editor()->addDevice(track, QStringLiteral("eq"));
        QQuickItem* eqView = show(QStringLiteral("eq"), track, eq);
        QVERIFY(eqView);
        auto* graph = find<EqGraph>(eqView, QStringLiteral("eqGraph"));
        sub::app::OrderedMap<QString, double> bands;
        auto addBand = [&](int index, int type, double freq, double gain, double q, int slope) {
            bands.insert(EqGraph::param(index, QStringLiteral("used")), 1.0);
            bands.insert(EqGraph::param(index, QStringLiteral("type")), type);
            bands.insert(EqGraph::param(index, QStringLiteral("freq")), freq);
            bands.insert(EqGraph::param(index, QStringLiteral("gain")), gain);
            bands.insert(EqGraph::param(index, QStringLiteral("q")), q);
            bands.insert(EqGraph::param(index, QStringLiteral("slope")), slope);
        };
        addBand(0, EqGraph::LowCut, 32.0, 0.0, 0.71, 3);
        addBand(1, EqGraph::LowShelf, 110.0, 4.0, 0.71, 1);
        addBand(2, EqGraph::Bell, 420.0, -5.5, 2.2, 1);
        addBand(3, EqGraph::Bell, 3200.0, 3.0, 0.9, 1);
        addBand(4, EqGraph::HighShelf, 9000.0, 2.5, 0.71, 1);
        bands.insert(EqGraph::param(3, QStringLiteral("place")), 3.0);  // Mid
        graph->set(bands);
        graph->select(2);
        EqView::instance()->setPanel(true);
        QTest::qWait(50);
        for (int i = 0; i < 16; ++i) {
            engine_->renderOffline(i * beatsPerChunk, chunk);
            refreshDisplays();
        }
        QTest::mouseMove(window_, scenePoint(graph, graph->dot(*graph->bands()[3])));  // its badge
        QTest::qWait(300);
        save(grab(), QStringLiteral("eq.png"));
        // The ghost band where a click would add one.
        QTest::mouseMove(window_, scenePoint(graph, QPointF(graph->xOf(1500.0), graph->curveY(graph->xOf(1500.0)))));
        QTest::qWait(300);
        save(grab(), QStringLiteral("eq-ghost.png"));
        EqView::instance()->setPanel(false);
        fitted();
        QTest::qWait(50);
        save(grab(), QStringLiteral("eq-collapsed.png"));

        // The Sampler: a pluck, part of it played, looping, a note playing.
        const QString keys = editor()->addMidiTrack();
        const QString sampler = editor()->addDevice(keys, QStringLiteral("sampler"));
        std::vector<float> pluck(static_cast<size_t>(kSampleRate));
        for (int i = 0; i < kSampleRate; ++i) {
            const double t = double(i) / kSampleRate;
            pluck[size_t(i)] = float(0.9 * std::exp(-t / 0.18) *
                                     (std::sin(2 * kPi * 220 * t) + 0.4 * std::sin(2 * kPi * 660 * t + 0.3)) / 1.4);
        }
        const QString path = sub::app::test::writeWav(dir_.path(QStringLiteral("pluck.wav")), pluck);
        QQuickItem* samplerView = show(QStringLiteral("sampler"), keys, sampler);
        auto* samples = find<SampleView>(samplerView, QStringLiteral("sampleView"));
        samples->loadSample(path);
        editor()->setDeviceParam(keys, sampler, QStringLiteral("start"), 4.0);
        editor()->setDeviceParam(keys, sampler, QStringLiteral("end"), 72.0);
        editor()->setDeviceParam(keys, sampler, QStringLiteral("loop"), 1.0);
        QTRY_VERIFY(samples->decoded());
        samples->setPlayhead(0.31);
        QTest::qWait(50);
        save(grab(), QStringLiteral("sampler.png"));

        // A 1-Shot: a kick, its pitch falling, faded out; filtered, the LFO on.
        const QString kicks = editor()->addMidiTrack();
        const QString oneShot = editor()->addDevice(kicks, QStringLiteral("sampler"));
        std::vector<float> kick(static_cast<size_t>(kSampleRate / 2));
        double phase = 0.0;
        for (size_t i = 0; i < kick.size(); ++i) {
            const double t = double(i) / kSampleRate;
            phase += 2 * kPi * (48 + 110 * std::exp(-t / 0.03)) / kSampleRate;
            kick[i] = float(0.95 * std::exp(-t / 0.12) * std::sin(phase));
        }
        const QString kickPath = sub::app::test::writeWav(dir_.path(QStringLiteral("kick_one_shot.wav")), kick);
        QQuickItem* oneShotView = show(QStringLiteral("sampler"), kicks, oneShot);
        auto* kickSamples = find<SampleView>(oneShotView, QStringLiteral("sampleView"));
        kickSamples->loadSample(kickPath);
        for (const auto& [id, value] : std::initializer_list<std::pair<const char*, double>>{
                 {"mode", 1.0}, {"fade_out", 120.0}, {"end", 92.0}, {"filter", 1.0}, {"filter_freq", 6500.0},
                 {"filter_res", 20.0}, {"lfo", 1.0}, {"velocity", 35.0}, {"volume", -12.0}})
            editor()->setDeviceParam(kicks, oneShot, QString::fromLatin1(id), value);
        QTRY_VERIFY(kickSamples->decoded());
        QTest::qWait(50);
        save(grab(), QStringLiteral("sampler-oneshot.png"));

        // Sliced: a beat, cut at its transients, a slice playing.
        const QString beats = editor()->addMidiTrack();
        const QString slicer = editor()->addDevice(beats, QStringLiteral("sampler"));
        std::vector<float> loop(static_cast<size_t>(kSampleRate * 2), 0.f);
        quint32 noise = 777;
        const auto hit = [&](double at, double amplitude, double freq, double decay, double grit) {
            const auto from = static_cast<size_t>(at * kSampleRate);
            for (size_t i = from; i < loop.size(); ++i) {
                const double t = double(i - from) / kSampleRate;
                noise = noise * 1664525u + 1013904223u;
                const double n = double(noise >> 8) / double(1 << 24) - 0.5;
                loop[i] += float(amplitude * std::exp(-t / decay) * ((1 - grit) * std::sin(2 * kPi * freq * t) + grit * n));
            }
        };
        for (int i = 0; i < 4; ++i) {
            hit(0.5 * i, 0.8, 55, 0.15, 0.05);           // kicks
            hit(0.5 * i + 0.25, 0.55, 190, 0.08, 0.7);  // snares
            hit(0.5 * i + 0.125, 0.25, 5000, 0.02, 1.0);  // hats
            hit(0.5 * i + 0.375, 0.2, 5000, 0.02, 1.0);
        }
        const QString loopPath = sub::app::test::writeWav(dir_.path(QStringLiteral("beat_120bpm.wav")), loop);
        QQuickItem* sliceView = show(QStringLiteral("sampler"), beats, slicer);
        auto* loopSamples = find<SampleView>(sliceView, QStringLiteral("sampleView"));
        loopSamples->loadSample(loopPath);
        editor()->setDeviceParam(beats, slicer, QStringLiteral("mode"), 2.0);
        editor()->setDeviceParam(beats, slicer, QStringLiteral("sensitivity"), 80.0);
        editor()->setDeviceParam(beats, slicer, QStringLiteral("warp_beats"), 4.0);
        editor()->setDeviceParam(beats, slicer, QStringLiteral("warp"), 1.0);
        QTRY_VERIFY(loopSamples->decoded());
        loopSamples->setPlayhead(0.3);
        QTest::qWait(50);
        save(grab(), QStringLiteral("sampler-slice.png"));

        // Its Controls page.
        sliceView->setProperty("page", 1);
        QTest::qWait(50);
        save(grab(), QStringLiteral("sampler-controls.png"));
    }
};

QTEST_MAIN(TestUiDeviceEditors)
#include "test_ui_device_editors.moc"

namespace {
const char* const kHost = R"QML(
import QtQuick
import SUBstation

Window {
    id: window

    // The editor shown, the body of a device in the device view, in its frame's colours.
    function show(url, trackId, deviceId, height) {
        loader.setSource(url, { trackId: trackId, deviceId: deviceId })
        window.bodyHeight = height
        return loader.item
    }
    function clear() {
        loader.sourceComponent = null
        loader.source = ""
    }
    function editorFor(kind) {
        return DeviceEditors.editorFor(kind)
    }
    function eqWindow(trackId, deviceId) {
        return EqWindows.windowOf(trackId, deviceId)
    }
    // A parameter's cell, as the device view shows those of devices without an editor.
    function showKnob(trackId, deviceId, paramId) {
        loader.sourceComponent = knob
        loader.item.trackId = trackId
        loader.item.deviceId = deviceId
        loader.item.paramId = paramId
        window.bodyHeight = loader.item.implicitHeight + 8
        return loader.item
    }

    Component {
        id: knob
        DeviceParamKnob {
            x: 8
            y: 4
        }
    }

    // The body of the device view's tallest device: two rows of knobs (a hidden one measured),
    // 14 px apart, 6 px above and below them (DevicePanel's deviceHeight, less the frame and title bar).
    readonly property int deviceBodyHeight: 6 + 2 * probe.implicitHeight + 14 + 6
    DeviceParamKnob {
        id: probe
        visible: false
    }

    // The body's size, which the window follows (as the X server resizes it: later).
    property int bodyHeight: deviceBodyHeight
    readonly property int bodyWidth: loader.item ? loader.item.implicitWidth : 198

    width: bodyWidth + 2
    height: bodyHeight + 2
    visible: true
    color: Theme.border

    Rectangle {
        anchors.fill: parent
        anchors.margins: 1
        color: Theme.panelAlt
        radius: 3
    }
    Loader {
        id: loader
        x: 1
        y: 1
        width: window.bodyWidth
        height: window.bodyHeight
    }
}
)QML";
}  // namespace
