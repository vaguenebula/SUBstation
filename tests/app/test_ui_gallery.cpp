// The look, end to end: a gallery of every shared control and a sample of
// SgPainter drawing renders (the main window, on a session, is
// test_ui_mainwindow's). With SUBSTATION_UI_SCREENSHOTS set to a folder, it is
// saved there as a PNG to look at.

#include <QDir>
#include <QFile>
#include <QImage>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <QtMath>
#include <QtQml/qqml.h>

#include <cmath>
#include <vector>

#include "Ui.h"
#include "sg/SgCanvas.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

using sub::ui::SgPainter;
using sub::ui::Theme;

namespace {

// A sample of what the views will draw: an arrangement's lanes, clips with
// waveforms and MIDI notes, an envelope, a selection, the playhead, a piano
// roll's notes and an EQ curve.
class GalleryCanvas : public sub::ui::SgCanvas {
    Q_OBJECT
    QML_ELEMENT

protected:
    void paint(SgPainter& p) override {
        const double w = width(), h = height();
        p.fillRect(QRectF(0, 0, w, h), Theme::emptyArea());
        const QFont font = sub::ui::uiFont(8);
        // Two lanes, the second selected.
        const double laneH = 70;
        for (int lane = 0; lane < 2; ++lane)
            p.fillRect(QRectF(0, lane * (laneH + 1), w * 0.62, laneH), lane ? Theme::laneSelected() : Theme::lane());
        // The grid: bars every 96 px, beats every 24, sixteenths every 6.
        for (double x = 0; x < w * 0.62; x += 6) {
            const int i = int(std::lround(x / 6));
            const QColor color = i % 16 == 0 ? Theme::gridBar() : (i % 4 == 0 ? Theme::gridBeat() : Theme::gridSub());
            p.drawLine(QPointF(x, 0), QPointF(x, 2 * laneH), color, 1.0, Qt::FlatCap);
        }
        p.fillRect(QRectF(96, 0, 192, 2 * laneH + 1), Theme::loopRegion());
        // An audio clip with its waveform.
        const QColor track(0x4f, 0x9d, 0xde);
        const QRectF clip(30, 1, 250, laneH - 2);
        p.fillRect(clip, track.darker(160));
        p.fillRect(QRectF(clip.left(), clip.top(), clip.width(), 16), track);
        p.drawText(QRectF(clip.left() + 4, clip.top(), clip.width() - 8, 16), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("Drums.wav"), Theme::accentText(), font);
        const int columns = int(clip.width());
        std::vector<float> top(static_cast<size_t>(columns)), bottom(static_cast<size_t>(columns));
        const double mid = clip.top() + 16 + (clip.height() - 16) / 2, half = (clip.height() - 18) / 2;
        for (int i = 0; i < columns; ++i) {
            const double envelope = std::exp(-std::fmod(i, 62.0) / 18.0);
            const double a = envelope * (0.35 + 0.6 * std::abs(std::sin(i * 0.9)));
            top[size_t(i)] = float(mid - a * half);
            bottom[size_t(i)] = float(mid + a * half * 0.9);
        }
        p.fillColumns(clip.left(), 1.0, top.data(), bottom.data(), columns, Theme::waveform(), 1.0);
        p.drawRect(clip, track.lighter(130));
        // A MIDI clip with notes, in the second lane.
        const QRectF midi(300, laneH + 2, 160, laneH - 2);
        const QColor green(0x6c, 0xc0, 0x4a);
        p.fillRect(midi, green.darker(170));
        p.fillRect(QRectF(midi.left(), midi.top(), midi.width(), 16), green);
        p.drawText(QRectF(midi.left() + 4, midi.top(), midi.width() - 8, 16), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("Bass"), Theme::accentText(), font);
        for (int n = 0; n < 12; ++n)
            p.fillRect(QRectF(midi.left() + 4 + n * 13, midi.top() + 20 + (n * 7 % 40), 10, 3), Theme::waveform());
        // An envelope over the first lane, antialiased, with its breakpoints.
        p.save();
        p.setAntialiasing(true);
        const QPointF envelope[] = {{0, 50}, {80, 50}, {140, 12}, {220, 40}, {300, 25}, {420, 60}, {560, 30}};
        p.fillToBaseline(envelope, 7, laneH, QColor(255, 166, 43, 30));
        p.drawPolyline(envelope, 7, Theme::accent(), 1.5);
        for (const QPointF& point : envelope)
            p.fillEllipse(point, 3, 3, Theme::accent());
        p.restore();
        // A time selection, the insert marker, the playhead.
        p.fillRect(QRectF(330, 0, 96, 2 * laneH + 1), Theme::selection());
        p.drawLine(QPointF(200, 0), QPointF(200, 2 * laneH), Theme::playhead(), 1.0);
        p.save();
        p.setAntialiasing(true);
        const QPointF marker[] = {{140, 0}, {148, 0}, {144, 6}};
        p.fillPolygon(marker, 3, Theme::insertMarker());
        p.restore();

        // A piano roll's corner: keys, rows, notes with names.
        p.save();
        p.translate(w * 0.64, 0);
        p.setClipRect(QRectF(0, 0, w * 0.36, 2 * laneH + 1));
        const double rowH = 10;
        for (int row = 0; row < 15; ++row) {
            const int pitch = 72 - row;
            const bool black = QList<int>{1, 3, 6, 8, 10}.contains(pitch % 12);
            p.fillRect(QRectF(0, row * rowH, 40, rowH), black ? Theme::keyBlack() : Theme::keyWhite());
            p.fillRect(QRectF(40, row * rowH, w, rowH), black ? Theme::blackKeyRow() : Theme::lane());
            p.drawLine(QPointF(40, row * rowH), QPointF(w, row * rowH), Theme::gridSub(), 1.0, Qt::FlatCap);
            if (pitch % 12 == 0)
                p.drawText(QRectF(2, row * rowH, 36, rowH), Qt::AlignRight | Qt::AlignVCenter,
                           QStringLiteral("C%1").arg(pitch / 12 - 2), Theme::keyLabel(), font);
        }
        const int notes[][3] = {{0, 3, 40}, {48, 5, 30}, {80, 7, 50}, {140, 2, 36}, {180, 10, 60}};
        for (const auto& note : notes) {
            const QRectF r(44 + note[0], note[1] * rowH + 1, note[2], rowH - 1);
            p.fillRect(r, green);
            p.drawRect(r.adjusted(0, 0, -1, -1), Theme::selectionOutline());
        }
        p.restore();

        // An EQ curve and its fill below a grid, antialiased.
        p.save();
        p.translate(0, 2 * laneH + 10);
        const QRectF eq(0, 0, w, h - 2 * laneH - 10);
        p.fillRect(eq, Theme::meterBg());
        for (int i = 1; i < 8; ++i)
            p.drawLine(QPointF(eq.width() * i / 8, 0), QPointF(eq.width() * i / 8, eq.height()), Theme::gridSub(), 1.0,
                       Qt::FlatCap);
        p.drawLine(QPointF(0, eq.height() / 2), QPointF(eq.width(), eq.height() / 2), Theme::gridBeat(), 1.0,
                   Qt::FlatCap);
        std::vector<QPointF> curve;
        for (int x = 0; x <= int(eq.width()); x += 2) {
            const double t = x / eq.width();
            const double bump = 22 * std::exp(-std::pow((t - 0.3) / 0.06, 2)) - 14 * std::exp(-std::pow((t - 0.7) / 0.1, 2));
            curve.emplace_back(x, eq.height() / 2 - bump);
        }
        p.setAntialiasing(true);
        p.fillToBaseline(curve.data(), int(curve.size()), eq.height() / 2, QColor(255, 166, 43, 40));
        p.drawPolyline(curve.data(), int(curve.size()), Theme::scopeLine(), 2.0);
        p.fillEllipse(QPointF(eq.width() * 0.3, eq.height() / 2 - 22), 5, 5, Theme::accent());
        p.drawEllipse(QRectF(eq.width() * 0.7 - 5, eq.height() / 2 + 14 - 5, 10, 10), Theme::text(), 1.5);
        p.drawText(QRectF(6, 4, 200, 14), Qt::AlignLeft | Qt::AlignTop, QStringLiteral("1.00 kHz  +6.0 dB  Q 0.71"),
                   Theme::text(), font);
        p.restore();
    }
};

// A stand-in for the engine bridge's scope: a sine at 440 Hz (48 kHz) and an
// overtone, 800 more samples written every time it is read.
class FakeScope : public QObject {
    Q_OBJECT
    Q_PROPERTY(quint64 scopeWritten READ scopeWritten)

public:
    quint64 scopeWritten() const { return written_; }
    Q_INVOKABLE QList<float> scopeSamples(int frames) {
        written_ += 800;
        QList<float> samples;
        for (int i = 0; i < frames; ++i) {
            const double t = double(written_ - quint64(frames) + quint64(i)) / 48000.0;
            samples << float(0.7 * std::sin(2 * M_PI * 440.0 * t) + 0.15 * std::sin(2 * M_PI * 1320.0 * t));
        }
        return samples;
    }

private:
    quint64 written_ = 48000;
};

// The gallery's QML (at the end of the file: moc skips what follows a raw string).
extern const char* const kGallery;

void save(const QImage& image, const QString& name) {
    const QString folder = qEnvironmentVariable("SUBSTATION_UI_SCREENSHOTS");
    if (folder.isEmpty())
        return;
    QDir().mkpath(folder);
    QVERIFY(image.save(QDir(folder).filePath(name)));
}

bool near(QRgb a, QColor b, int tolerance = 3) {
    return qAbs(qRed(a) - b.red()) <= tolerance && qAbs(qGreen(a) - b.green()) <= tolerance &&
           qAbs(qBlue(a) - b.blue()) <= tolerance;
}

}  // namespace

class TestUiGallery : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() {
        if (QGuiApplication::platformName() == QLatin1String("offscreen") ||
            QGuiApplication::platformName() == QLatin1String("minimal"))
            QSKIP("needs a display: the offscreen platform renders Qt Quick in software, without this geometry");
        sub::ui::setUpApplication();
        qmlRegisterType<GalleryCanvas>("Gallery", 1, 0, "GalleryCanvas");
    }

    void gallery() {
        QQmlEngine engine;
        sub::ui::setUpEngine(engine);
        FakeScope feed;
        engine.rootContext()->setContextProperty(QStringLiteral("scopeFeed"), &feed);
        QList<QQmlError> warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError>& list) { warnings += list; });
        // From a file: Qt 6.8 can't find a document's inline components (Heading)
        // when it was given as data, without a file of its own.
        QTemporaryDir folder;
        const QString file = folder.filePath(QStringLiteral("Gallery.qml"));
        {
            QFile out(file);
            QVERIFY(out.open(QIODevice::WriteOnly));
            out.write(kGallery);
        }
        QQmlComponent component(&engine, QUrl::fromLocalFile(file));
        std::unique_ptr<QObject> root(component.create());
        if (!root)
            qWarning() << component.errors();
        QVERIFY(root);
        auto* window = qobject_cast<QQuickWindow*>(root.get());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTest::qWait(400);  // icons load, the scope polls
        for (const QQmlError& warning : warnings)
            qWarning() << warning.toString();
        QVERIFY(warnings.isEmpty());
        const QImage image = window->grabWindow();
        save(image, QStringLiteral("gallery.png"));
        QVERIFY(near(image.pixel(2, 2), Theme::window()));

        QMetaObject::invokeMethod(window, "openPopups");
        QTest::qWait(300);
        QVERIFY(warnings.isEmpty());
        const QImage popups = window->grabWindow();
        save(popups, QStringLiteral("gallery-popups.png"));
        const qreal dpr = window->effectiveDevicePixelRatio();
        QVERIFY(near(popups.pixel(int(650 * dpr), int(450 * dpr)), Theme::window()));  // the dialog's body
    }
};

QTEST_MAIN(TestUiGallery)
#include "test_ui_gallery.moc"

namespace {
const char* const kGallery = R"QML(
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SUBstation
import Gallery

ApplicationWindow {
    id: root
    width: 1180
    height: 820
    visible: true
    title: "Gallery"

    component Heading: Label {
        color: Theme.textDim
        font: Theme.uiFont(9, true)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        Heading { text: "Button roles: normal, checked, disabled (and the plain style Button)" }
        Row {
            spacing: 6
            Repeater {
                model: ["", "activator", "solo", "play", "record", "arm", "re-enable", "tool", "flat", "small", "device-header"]
                Column {
                    spacing: 3
                    RoleButton { role: modelData; text: modelData === "" ? "Plain" : (modelData === "arm" ? "\u25cf" : modelData); width: 72; height: 22 }
                    RoleButton { role: modelData; text: modelData === "" ? "Plain" : (modelData === "arm" ? "\u25cf" : modelData); checkable: true; checked: true; width: 72; height: 22 }
                    RoleButton { role: modelData; text: modelData === "" ? "Plain" : (modelData === "arm" ? "\u25cf" : modelData); enabled: false; width: 72; height: 22 }
                }
            }
            Column {
                spacing: 3
                Button { text: "OK"; width: 72; height: 22 }
                Button { text: "Cancel"; width: 72; height: 22; highlighted: true }
                Button { text: "Off"; width: 72; height: 22; enabled: false }
            }
        }

        Heading { text: "The transport's buttons (ToggleButton, IconButton) and the track header's" }
        Row {
            spacing: 5
            ToggleButton { role: "tool"; iconName: "metronome"; height: 28 }
            ToggleButton { role: "tool"; text: "\u2328"; height: 28 }
            ToggleButton { role: "play"; iconName: "play"; checked: true; height: 28 }
            IconButton { iconName: "stop"; height: 28 }
            ToggleButton { role: "record"; iconName: "record"; height: 28 }
            ToggleButton { role: "re-enable"; iconName: "re_enable_automation"; checked: true; height: 28 }
            ToggleButton { role: "re-enable"; iconName: "re_enable_automation"; enabled: false; height: 28 }
            ToggleButton { role: "tool"; iconName: "lock_envelopes"; height: 28 }
            ToggleButton { role: "tool"; iconName: "lock_envelopes"; checked: true; height: 28 }
            ToggleButton { role: "tool"; iconName: "loop"; checked: true; height: 28 }
            ToggleButton { role: "tool"; iconName: "follow"; height: 28 }
            Item { width: 20; height: 1 }
            ToggleButton { role: "activator"; text: "1"; checked: true; width: 22; height: 18 }
            ToggleButton { role: "solo"; text: "S"; width: 22; height: 18 }
            ToggleButton { role: "solo"; text: "S"; checked: true; width: 22; height: 18 }
            ToggleButton { role: "arm"; text: "\u25cf"; checked: true; width: 18; height: 18 }
            ToggleButton { role: "device-header"; iconName: "plugin_window"; width: 20; height: 18 }
            ToggleButton { role: "device-header"; iconName: "sidechain"; checked: true; width: 20; height: 18 }
            ToggleButton { role: "small"; text: "Sync"; checked: true; height: 18 }
            ToggleButton { role: "small"; text: "Fit"; checkable: false; height: 18 }
        }

        Heading { text: "Icons (image://icons): 24 px, 14 px, and disabled" }
        Row {
            spacing: 6
            Repeater {
                model: Icons.names
                Column {
                    spacing: 2
                    Icon { name: modelData; size: 24 }
                    Icon { name: modelData; size: 14; anchors.horizontalCenter: parent.horizontalCenter }
                    Icon { name: modelData; size: 14; enabled: false; anchors.horizontalCenter: parent.horizontalCenter }
                }
            }
            Icon { name: "fold"; checked: true; size: 24 }
        }

        Heading { text: "Knobs, value boxes, meters, the oscilloscope" }
        RowLayout {
            spacing: 10
            Knob { value: 0.3 }
            Knob { from: -1; to: 1; value: -0.45; bipolar: true }
            Knob { from: -1; to: 1; value: 0.6; bipolar: true; automation: "on" }
            Knob { from: 20; to: 20000; value: 1000; logScale: true; automation: "off" }
            Knob { value: 0.8; color: Theme.soloOn; implicitWidth: 40; implicitHeight: 40 }
            ValueBox { from: 20; to: 999; value: 120; step: 0.25; decimals: 2; sampleText: "999.00"; Layout.preferredHeight: 28 }
            ValueBox { from: 1; to: 32; value: 4; step: 0.1; decimals: 0; sampleText: "32"; Layout.preferredHeight: 28 }
            Label { text: "/" }
            ValueBox { from: 1; to: 32; value: 8; decimals: 0; choices: [1, 2, 4, 8, 16, 32]; sampleText: "32"; Layout.preferredHeight: 28 }
            ValueBox { from: -70; to: 6; value: -6; step: 0.25; decimals: 1; defaultValue: 0; automation: "on"
                       sampleText: "-70.0 dB"; formatter: v => (v <= -70 ? "-inf" : v.toFixed(1)) + " dB" }
            Meter { id: meter1; implicitHeight: 60 }
            Meter { id: meter2; implicitHeight: 60 }
            Meter { id: meter3; implicitHeight: 60 }
            Oscilloscope { feed: scopeFeed }
            Item { Layout.fillWidth: true }
        }

        Heading { text: "Standard controls in the style" }
        RowLayout {
            spacing: 10
            ComboBox { model: ["No Key", "C Major", "A Minor", "F# Minor"]; focusPolicy: Qt.NoFocus }
            ComboBox { model: ["No Count-In", "Count-In 1 Bar", "Count-In 2 Bars"]; enabled: false }
            TextField { placeholderText: "Search"; implicitWidth: 140 }
            TextField { text: "Focused"; implicitWidth: 100; focus: true }
            CheckBox { text: "Exclusive mode"; checked: true }
            CheckBox { text: "Off" }
            ProgressBar { value: 0.6; implicitWidth: 120; implicitHeight: 10 }
            TabBar { TabButton { text: "Audio" } TabButton { text: "MIDI" } TabButton { text: "Plug-ins" } }
            Rectangle {
                width: 110; height: 70; color: Theme.panel
                ListView {
                    anchors.fill: parent; clip: true
                    model: 20
                    delegate: ItemDelegate { width: ListView.view.width - 12; text: "Item " + index; highlighted: index === 1 }
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOn }
                }
            }
            Item { Layout.fillWidth: true }
        }

        Heading { text: "Not in the style: the Basic style's, in the theme's palette" }
        RowLayout {
            spacing: 10
            Slider { value: 0.4; implicitWidth: 120 }
            SpinBox { value: 4; implicitWidth: 100; focusPolicy: Qt.NoFocus }
            RadioButton { text: "WASAPI"; checked: true }
            RadioButton { text: "ASIO" }
            Switch { checked: true }
            Item { Layout.fillWidth: true }
        }

        Heading { text: "SgPainter: lanes, clips, waveform columns, an envelope, a selection, the playhead; a piano roll; an EQ curve" }
        GalleryCanvas {
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
    }

    Component.onCompleted: {
        meter1.setLevels(0.5, 0.25)
        meter2.setLevels(1.0, 0.9)
        meter3.setLevels(0.05, 0.01)
    }

    // The popups, opened by the test for a second picture.
    function openPopups() {
        dialog.open()
        menu.popup(Qt.point(40, 520))
        tip.visible = true
    }

    Dialog {
        id: dialog
        x: 640
        y: 380
        width: 360
        modal: false
        title: "Export Audio"
        standardButtons: Dialog.Ok | Dialog.Cancel
        ColumnLayout {
            anchors.fill: parent
            spacing: 6
            Label { text: "Range: the arrangement" }
            ComboBox { model: ["16-bit", "24-bit", "32-bit float"]; currentIndex: 1 }
            CheckBox { text: "Normalize"; checked: true }
        }
    }

    Menu {
        id: menu
        Action { text: "Show Automation"; shortcut: "A" }
        Action { text: "Delete Automation"; enabled: false }
        MenuSeparator {}
        Action { text: "Snap to Grid"; checkable: true; checked: true; shortcut: "Ctrl+4" }
        Menu { title: "Record &Quantization"; Action { text: "1/16" } }
        Action { text: "Re-Enable Automation" }
    }

    RoleButton {
        x: 1000
        y: 60
        text: "Hover me"
        ToolTip {
            id: tip
            text: "Solo (S); Ctrl-click to solo it along with others"
        }
    }
}
)QML";
}  // namespace
