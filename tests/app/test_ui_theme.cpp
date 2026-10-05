// The theme: every colour of the Python UI's theme with its value, the
// stylesheet's button rules, the icons (each drawn, in its colour, its states,
// the disabled variant) and the image provider that serves them to QML.

#include <QGuiApplication>
#include <QImage>
#include <QKeySequence>
#include <QMetaProperty>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>

#include <memory>

#include "Ui.h"
#include "theme/Icons.h"
#include "theme/Theme.h"

using sub::ui::IconProvider;
using sub::ui::Icons;
using sub::ui::Theme;

namespace {

// Records the sizes QML asks for, and serves the icons.
class ProbeProvider : public QQuickImageProvider {
public:
    ProbeProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override {
        requested = requestedSize;
        return icons.requestImage(id, size, requestedSize);
    }
    IconProvider icons;
    QSize requested;
};

// How many pixels of `image` are visible (alpha over 0).
int coverage(const QImage& image) {
    int count = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qAlpha(image.pixel(x, y)) > 0)
                ++count;
    return count;
}

// The most opaque pixel's colour, unpremultiplied.
QColor strongest(const QImage& image) {
    QRgb best = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qAlpha(image.pixel(x, y)) > qAlpha(best))
                best = image.pixel(x, y);
    return QColor::fromRgba(best);
}

}  // namespace

class TestUiTheme : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void colorsAreTheme() {
        // The Python UI's theme constants, value for value.
        const QList<std::pair<const char*, QColor>> expected = {
            {"window", QColor("#1c1c1c")},          {"panel", QColor("#252525")},
            {"panelAlt", QColor("#2c2c2c")},        {"surface", QColor("#363636")},
            {"surfaceHover", QColor("#404040")},    {"border", QColor("#111111")},
            {"text", QColor("#d9d9d9")},            {"textDim", QColor("#8e8e8e")},
            {"textDisabled", QColor("#5c5c5c")},    {"accent", QColor("#ffa62b")},
            {"accentText", QColor("#1a1a1a")},      {"lane", QColor("#2b2b2b")},
            {"laneSelected", QColor("#343434")},    {"emptyArea", QColor("#222222")},
            {"gridBar", QColor("#4a4a4a")},         {"gridBeat", QColor("#393939")},
            {"gridSub", QColor("#313131")},         {"playhead", QColor("#f2f2f2")},
            {"insertMarker", QColor("#ffa62b")},    {"loopOn", QColor("#c9c9c9")},
            {"loopOff", QColor("#5d5d5d")},         {"loopRegion", QColor(255, 255, 255, 12)},
            {"selectionOutline", QColor("#ffffff")}, {"selection", QColor(168, 200, 235, 72)},
            {"rubberBand", QColor(255, 166, 43, 40)}, {"waveform", QColor(22, 22, 22, 230)},
            {"keyWhite", QColor("#d2d2d2")},        {"keyBlack", QColor("#1f1f1f")},
            {"keyLabel", QColor("#4a4a4a")},        {"blackKeyRow", QColor("#252525")},
            {"outsideClip", QColor(0, 0, 0, 110)},  {"activatorOn", QColor("#ffc233")},
            {"soloOn", QColor("#4fa3ff")},          {"playOn", QColor("#5fd35f")},
            {"recordOn", QColor("#ff5a4d")},        {"meterLow", QColor("#3fcf55")},
            {"meterMid", QColor("#f2d024")},        {"meterHigh", QColor("#ff4a3d")},
            {"meterBg", QColor("#141414")},         {"deviceHeader", QColor("#3e3e3e")},
            {"deviceHeaderSelected", QColor("#575757")}, {"scopeLine", QColor("#ffb84d")},
            {"scopeGlow", QColor(255, 166, 43, 60)}, {"scopeAxis", QColor(255, 255, 255, 22)},
            {"frozen", QColor("#8fd3ff")},          {"frozenTint", QColor(143, 211, 255, 34)},
        };
        Theme theme;
        for (const auto& [name, color] : expected) {
            const QVariant value = theme.property(name);
            QVERIFY2(value.isValid(), name);
            QCOMPARE(value.value<QColor>().rgba(), color.rgba());
        }
        QCOMPARE(Theme::kLane.rgba(), QColor("#2b2b2b").rgba());
        QCOMPARE(Theme::kSelection.alpha(), 72);
        QCOMPARE(theme.property("monoFontFamily").toString(), QStringLiteral("Consolas"));
        const QFont font = theme.uiFont(11, true);
        QCOMPARE(font.family(), QStringLiteral("Segoe UI"));
        QCOMPARE(font.pointSizeF(), 11.0);
        QVERIFY(font.bold());
        QCOMPARE(sub::ui::uiFont().pointSizeF(), 9.0);
        // Every colour property is constant and readable from QML's side.
        int colors = 0;
        for (int i = theme.metaObject()->propertyOffset(); i < theme.metaObject()->propertyCount(); ++i) {
            const QMetaProperty property = theme.metaObject()->property(i);
            QVERIFY(property.isConstant());
            if (property.metaType().id() == QMetaType::QColor)
                ++colors;
        }
        QCOMPARE(colors, int(expected.size()) + 4);  // and the stylesheet's own four
    }

    void buttonRolesFollowTheStylesheet() {
        using L = Theme::ButtonLook;
        auto look = [](const char* role, bool hovered, bool pressed, bool checked, bool enabled) {
            return Theme::buttonLook(QString::fromLatin1(role), hovered, pressed, checked, enabled);
        };
        L plain = look("", false, false, false, true);
        QCOMPARE(plain.background, Theme::kSurface);
        QCOMPARE(plain.text, Theme::kText);
        QCOMPARE(plain.border, 1);
        QCOMPARE(plain.radius, 3);
        QCOMPARE(plain.paddingH, 12);
        QCOMPARE(look("", true, false, false, true).background, Theme::kSurfaceHover);
        QCOMPARE(look("", true, true, false, true).background, Theme::kPanel);       // pressed over hover
        QCOMPARE(look("", true, true, true, true).background, Theme::kAccent);       // checked over both
        QCOMPARE(look("", false, false, true, true).text, Theme::kAccentText);
        QCOMPARE(look("", false, false, false, false).text, Theme::kTextDisabled);
        QCOMPARE(look("activator", false, false, true, true).background, Theme::kActivatorOn);
        QCOMPARE(look("activator", false, false, false, true).weight, int(QFont::DemiBold));
        QCOMPARE(look("activator", false, false, false, true).pointSize, 8.0);
        QCOMPARE(look("solo", true, false, true, true).background, Theme::kSoloOn);
        const L play = look("play", false, false, true, true);
        QCOMPARE(play.background, Theme::kPlayOn);
        QCOMPARE(play.text, Theme::kAccentText);
        QCOMPARE(look("record", false, false, true, true).background, Theme::kRecordOn);
        QCOMPARE(look("record", false, false, false, true).minWidth, 26);
        QCOMPARE(look("record", false, false, false, true).minHeight, 22);
        const L arm = look("arm", false, false, true, true);
        QCOMPARE(arm.background, Theme::kRecordOn);
        QCOMPARE(arm.radius, 8);
        QCOMPARE(look("re-enable", false, false, true, true).background, Theme::kAccent);
        QCOMPARE(look("tool", false, false, false, true).paddingH, 2);
        // Flat: never a background; dim, but TEXT under the mouse.
        const L flat = look("flat", true, true, true, true);
        QCOMPARE(flat.background.alpha(), 0);
        QCOMPARE(flat.text, Theme::kText);
        QCOMPARE(flat.border, 0);
        QCOMPARE(look("flat", false, false, true, true).text, Theme::kTextDim);
        QCOMPARE(look("small", false, false, false, true).paddingH, 6);
        QCOMPARE(look("small", false, false, false, true).pointSize, 8.0);
        // Device header: transparent; a light wash under the mouse; ACCENT checked; dim disabled.
        QCOMPARE(look("device-header", false, false, false, true).background.alpha(), 0);
        QCOMPARE(look("device-header", true, false, false, true).background, Theme::kDeviceHeaderHover);
        QCOMPARE(look("device-header", true, false, true, true).background, Theme::kAccent);
        const L headerOff = look("device-header", false, false, true, false);
        QCOMPARE(headerOff.background.alpha(), 0);
        QCOMPARE(headerOff.text, Theme::kTextDisabled);
        // For QML: the same as a map.
        Theme theme;
        const QVariantMap map = theme.buttonStyle(QStringLiteral("solo"), false, false, true, true);
        QCOMPARE(map.value(QStringLiteral("background")).value<QColor>(), Theme::kSoloOn);
        QCOMPARE(map.value(QStringLiteral("radius")).toInt(), 2);
    }

    void textHelpers() {
        QCOMPARE(Theme::withoutMnemonics(QStringLiteral("&File")), QStringLiteral("File"));
        QCOMPARE(Theme::withoutMnemonics(QStringLiteral("Save &As\u2026")), QStringLiteral("Save As\u2026"));
        QCOMPARE(Theme::withoutMnemonics(QStringLiteral("Rock && Roll")), QStringLiteral("Rock & Roll"));
        Theme theme;
        QCOMPARE(theme.shortcutText(QStringLiteral("Ctrl+Shift+S")), QStringLiteral("Ctrl+Shift+S"));
        QCOMPARE(theme.shortcutText(int(QKeySequence::Copy)),
                 QKeySequence(QKeySequence::Copy).toString(QKeySequence::NativeText));
        QCOMPARE(theme.shortcutText(QVariant()), QString());
        QCOMPARE(theme.automationColor(QStringLiteral("on")), Theme::kAutomationOn);
        QCOMPARE(theme.automationColor(QStringLiteral("off")), Theme::kAutomationOff);
        QCOMPARE(theme.automationColor(QString()).alpha(), 0);
    }

    void everyIconDraws() {
        const QStringList names = Icons::names();
        const QStringList fromPython = {
            QStringLiteral("play"),      QStringLiteral("stop"),       QStringLiteral("record"),
            QStringLiteral("metronome"), QStringLiteral("loop"),       QStringLiteral("follow"),
            QStringLiteral("re_enable_automation"), QStringLiteral("lock_envelopes"), QStringLiteral("headphones"),
            QStringLiteral("folder"),    QStringLiteral("waveform"),   QStringLiteral("plugin"),
            QStringLiteral("preset"),    QStringLiteral("plugin_window"), QStringLiteral("sidechain"),
            QStringLiteral("snowflake"), QStringLiteral("save"),       QStringLiteral("link"),
            QStringLiteral("infinity"),  QStringLiteral("expand"),     QStringLiteral("sliders"),
            QStringLiteral("fold"),      QStringLiteral("search"),     QStringLiteral("app_icon")};
        QCOMPARE(names.size(), fromPython.size());
        for (const QString& name : fromPython) {
            QVERIFY2(names.contains(name), qPrintable(name));
            for (int size : {14, 64}) {
                const QImage image = Icons::image(name, size);
                QCOMPARE(image.size(), QSize(size, size));
                QVERIFY2(coverage(image) > size * size / 30, qPrintable(name));
            }
        }
        QVERIFY(Icons::image(QStringLiteral("nothing"), 14).isNull());
    }

    void iconColors() {
        // Their own colours by default.
        QCOMPARE(strongest(Icons::image(QStringLiteral("play"), 32)).rgb(), Theme::kText.rgb());
        QCOMPARE(strongest(Icons::image(QStringLiteral("folder"), 32)).rgb(), Theme::kTextDim.rgb());
        QCOMPARE(strongest(Icons::image(QStringLiteral("record"), 32)).rgb(), QColor("#ff5a4d").rgb());
        QCOMPARE(strongest(Icons::image(QStringLiteral("snowflake"), 32)).rgb(), Theme::kFrozen.rgb());
        QCOMPARE(Icons::defaultColor(QStringLiteral("search")), Theme::kTextDim);
        // Asked for, or disabled.
        QCOMPARE(strongest(Icons::image(QStringLiteral("play"), 32, Qt::red)).rgb(), QColor(Qt::red).rgb());
        QCOMPARE(strongest(Icons::image(QStringLiteral("play"), 32, Qt::red, false, true)).rgb(),
                 Theme::kTextDisabled.rgb());
        // The app icon keeps its colours.
        const QImage app = Icons::image(QStringLiteral("app_icon"), 64, QColor(), false, true);
        QCOMPARE(app.pixelColor(8, 32).rgb(), Theme::kPanelAlt.rgb());
    }

    void iconStates() {
        // Lock Envelopes: closed (on) and open (off) differ in the shackle; fold points right or down.
        const QImage closed = Icons::image(QStringLiteral("lock_envelopes"), 64, QColor(), true);
        const QImage open = Icons::image(QStringLiteral("lock_envelopes"), 64, QColor(), false);
        QVERIFY(closed != open);
        QVERIFY(qAlpha(open.pixel(21, 6)) > 0);    // raised shackle
        QCOMPARE(qAlpha(closed.pixel(21, 6)), 0);
        const QImage folded = Icons::image(QStringLiteral("fold"), 64, QColor(), true);
        const QImage unfolded = Icons::image(QStringLiteral("fold"), 64, QColor(), false);
        QVERIFY(qAlpha(folded.pixel(44, 32)) > 0);   // pointing right
        QCOMPARE(qAlpha(unfolded.pixel(44, 32)), 0);
        QVERIFY(qAlpha(unfolded.pixel(32, 43)) > 0);  // pointing down
        QCOMPARE(qAlpha(folded.pixel(32, 43)), 0);
    }

    void iconProviderUrls() {
        QCOMPARE(Icons::url(QStringLiteral("play")), QStringLiteral("image://icons/play"));
        QCOMPARE(Icons::url(QStringLiteral("play"), QColor(Qt::red)), QStringLiteral("image://icons/play?color=%23ff0000"));
        QCOMPARE(Icons::url(QStringLiteral("lock_envelopes"), QVariant(), true, true),
                 QStringLiteral("image://icons/lock_envelopes?state=on&mode=disabled"));
        QCOMPARE(Icons::url(QStringLiteral("play"), QColor(255, 0, 0, 128)),
                 QStringLiteral("image://icons/play?color=%2380ff0000"));
        IconProvider provider;
        QSize size;
        QImage image = provider.requestImage(QStringLiteral("play?color=%23ff0000"), &size, QSize(20, 20));
        QCOMPARE(size, QSize(20, 20));
        QCOMPARE(strongest(image).rgb(), QColor(Qt::red).rgb());
        image = provider.requestImage(QStringLiteral("play?color=00ff00&mode=disabled"), &size, QSize());
        QCOMPARE(size, QSize(64, 64));  // no size asked: the drawing grid's
        QCOMPARE(strongest(image).rgb(), Theme::kTextDisabled.rgb());
        image = provider.requestImage(QStringLiteral("play?color=blue"), &size, QSize(0, 16));
        QCOMPARE(size, QSize(16, 16));
        QCOMPARE(strongest(image).rgb(), QColor(Qt::blue).rgb());
        image = provider.requestImage(QStringLiteral("fold?state=on"), &size, QSize(64, 64));
        QCOMPARE(image, Icons::image(QStringLiteral("fold"), 64, QColor(), true));
    }

    void iconsAreDrawnForTheScreen() {
        if (QGuiApplication::platformName() == QLatin1String("offscreen") ||
            QGuiApplication::platformName() == QLatin1String("minimal"))
            QSKIP("needs a display");
        sub::ui::setUpApplication();
        QQmlEngine engine;
        sub::ui::setUpEngine(engine);
        auto* probe = new ProbeProvider;
        engine.addImageProvider(QStringLiteral("probe"), probe);
        QQmlComponent component(&engine);
        // (No raw string literal here: moc stops reading the class at one.)
        component.setData("import QtQuick\n"
                          "import SUBstation\n"
                          "Window {\n"
                          "    width: 100; height: 60; visible: true\n"
                          "    Icon { objectName: \"icon\"; name: \"play\"; size: 14 }\n"
                          "    Image { objectName: \"probe\"; x: 30; source: \"image://probe/play\"\n"
                          "            sourceSize: Qt.size(14, 14); width: 14; height: 14 }\n"
                          "}\n",
                          QUrl());
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto* window = qobject_cast<QQuickWindow*>(root.get());
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* icon = window->findChild<QQuickItem*>(QStringLiteral("icon"));
        QTRY_COMPARE(icon->property("status").toInt(), 1);  // Image.Ready
        QCOMPARE(icon->width(), 14.0);
        // An icon is drawn at the size it shows at, in device pixels.
        QTRY_VERIFY(probe->requested.isValid());
        const qreal dpr = window->effectiveDevicePixelRatio();
        QCOMPARE(probe->requested, QSize(qRound(14 * dpr), qRound(14 * dpr)));
    }
};

QTEST_MAIN(TestUiTheme)
#include "test_ui_theme.moc"
