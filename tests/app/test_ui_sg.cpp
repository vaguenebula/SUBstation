// The scene-graph painting toolkit: SgCanvas and SgPainter, rendered for real
// (the GPU path: run on a display, xvfb with the xcb platform here) and checked
// pixel by pixel in grabWindow(); and a benchmark of a big arrangement's worth of rects.

#include <QElapsedTimer>
#include <QImage>
#include <QLinearGradient>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QTest>

#include <functional>
#include <vector>

#include "sg/SgCanvas.h"
#include "sg/SgPainter.h"
#include "sg/SgTextureCache.h"
#include "theme/Theme.h"

using sub::ui::SgCanvas;
using sub::ui::SgPainter;

namespace {

class TestCanvas : public SgCanvas {
public:
    std::function<void(SgPainter&)> draw;

protected:
    void paint(SgPainter& p) override {
        if (draw)
            draw(p);
    }
};

// A window showing one canvas over black, its frames counted.
struct Scene {
    QQuickWindow window;
    TestCanvas* canvas = nullptr;
    int frames = 0;

    explicit Scene(QSize size = QSize(200, 160)) {
        window.setColor(Qt::black);
        window.resize(size);
        canvas = new TestCanvas;
        canvas->setParentItem(window.contentItem());
        canvas->setSize(QSizeF(size));
        QObject::connect(&window, &QQuickWindow::frameSwapped, &window, [this] { ++frames; });
    }

    bool show() {
        window.show();
        return QTest::qWaitForWindowExposed(&window);
    }

    // Draws `draw` and returns the window's pixels (device pixels).
    QImage render(std::function<void(SgPainter&)> draw) {
        canvas->draw = std::move(draw);
        canvas->update();
        const int before = frames;
        (void)QTest::qWaitFor([&] { return frames > before; }, 2000);
        QImage image = window.grabWindow();
        image.setDevicePixelRatio(1.0);
        return image.convertToFormat(QImage::Format_ARGB32);
    }

    qreal dpr() const { return window.effectiveDevicePixelRatio(); }
};

QRgb at(const QImage& image, qreal x, qreal y, qreal dpr) { return image.pixel(int(x * dpr), int(y * dpr)); }

bool near(QRgb a, QRgb b, int tolerance = 3) {
    return qAbs(qRed(a) - qRed(b)) <= tolerance && qAbs(qGreen(a) - qGreen(b)) <= tolerance &&
           qAbs(qBlue(a) - qBlue(b)) <= tolerance;
}

QString rgb(QRgb c) { return QStringLiteral("#%1").arg(c & 0xffffff, 6, 16, QLatin1Char('0')); }

#define CHECK_PIXEL(image, x, y, expected)                                                                  \
    do {                                                                                                    \
        const QRgb got_ = at(image, x, y, scene.dpr());                                                     \
        QVERIFY2(near(got_, QColor(expected).rgb()),                                                        \
                 qPrintable(QStringLiteral("(%1, %2): %3, expected %4")                                     \
                                .arg(x)                                                                     \
                                .arg(y)                                                                     \
                                .arg(rgb(got_), rgb(QColor(expected).rgb()))));                             \
    } while (false)

// How many pixels in `area` (device pixels) are not black.
int lit(const QImage& image, const QRect& area) {
    int count = 0;
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x)
            if ((image.pixel(x, y) & 0xffffff) != 0)
                ++count;
    return count;
}

QRect device(const QRectF& r, qreal dpr) {
    return QRect(int(r.x() * dpr), int(r.y() * dpr), int(r.width() * dpr), int(r.height() * dpr));
}

}  // namespace

class TestUiSg : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() {
        if (QGuiApplication::platformName() == QLatin1String("offscreen") ||
            QGuiApplication::platformName() == QLatin1String("minimal"))
            QSKIP("needs a display: the offscreen platform renders Qt Quick in software, without this geometry");
    }

    void rectsAreBatched() {
        Scene scene;
        QVERIFY(scene.show());
        const QImage image = scene.render([](SgPainter& p) {
            for (int i = 0; i < 10; ++i)
                p.fillRect(QRectF(10 + i * 15, 10, 10, 10), i % 2 ? QColor(Qt::red) : QColor(Qt::green));
            p.fillRect(QRectF(10, 40, 50, 30), QColor(0, 0, 255));
        });
        CHECK_PIXEL(image, 15, 15, Qt::green);
        CHECK_PIXEL(image, 30, 15, Qt::red);
        CHECK_PIXEL(image, 22, 15, Qt::black);  // between two
        CHECK_PIXEL(image, 10, 40, QColor(0, 0, 255));
        CHECK_PIXEL(image, 59, 69, QColor(0, 0, 255));
        CHECK_PIXEL(image, 60, 70, Qt::black);  // the far edges are outside, as QPainter's fillRect
        const SgCanvas::Stats stats = scene.canvas->lastStats();
        QCOMPARE(stats.solidNodes, 1);  // eleven rects, one batch
        QCOMPARE(stats.textureNodes, 0);
        QCOMPARE(stats.vertices, 11 * 6);
    }

    void alignedLinesLandOnWholePixels() {
        // Without antialiasing, as QPainter's aliased rendering: a 1 px line at
        // x = 10 fills the column from 10 to 11; square caps reach half a pixel past the ends.
        Scene scene;
        QVERIFY(scene.show());
        const QImage image = scene.render([](SgPainter& p) {
            p.drawLine(QPointF(10, 20), QPointF(10, 40), Qt::white);
            p.drawLine(QPointF(30, 20), QPointF(60, 20), Qt::red, 1.0, Qt::FlatCap);
            p.drawLine(QPointF(30, 30), QPointF(60, 30), Qt::green, 3.0);
        });
        CHECK_PIXEL(image, 10, 20, Qt::white);
        CHECK_PIXEL(image, 10, 40, Qt::white);  // the square cap
        CHECK_PIXEL(image, 10, 41, Qt::black);
        CHECK_PIXEL(image, 9, 30, Qt::black);
        CHECK_PIXEL(image, 11, 30, Qt::black);
        CHECK_PIXEL(image, 30, 20, Qt::red);
        CHECK_PIXEL(image, 59, 20, Qt::red);
        CHECK_PIXEL(image, 60, 20, Qt::black);  // flat: stops at the end
        CHECK_PIXEL(image, 29, 20, Qt::black);
        CHECK_PIXEL(image, 45, 29, Qt::green);  // 3 px: rows 29 to 31
        CHECK_PIXEL(image, 45, 31, Qt::green);
        CHECK_PIXEL(image, 45, 28, Qt::black);
        CHECK_PIXEL(image, 45, 32, Qt::black);
    }

    void rectOutlines() {
        Scene scene;
        QVERIFY(scene.show());
        const QImage image = scene.render([](SgPainter& p) {
            p.drawRect(QRectF(10, 10, 20, 20), Qt::white);  // covers 10 to 30 inclusive, as QPainter's
            p.drawRect(QRectF(50, 10, 20, 20), Qt::red, 3.0);
        });
        CHECK_PIXEL(image, 10, 10, Qt::white);
        CHECK_PIXEL(image, 30, 30, Qt::white);
        CHECK_PIXEL(image, 30, 20, Qt::white);
        CHECK_PIXEL(image, 31, 20, Qt::black);
        CHECK_PIXEL(image, 20, 20, Qt::black);  // hollow
        CHECK_PIXEL(image, 11, 20, Qt::black);
        CHECK_PIXEL(image, 49, 20, Qt::red);  // 3 px: from 49 to 51
        CHECK_PIXEL(image, 51, 20, Qt::red);
        CHECK_PIXEL(image, 52, 20, Qt::black);
        CHECK_PIXEL(image, 48, 20, Qt::black);
    }

    void antialiasedLinesFeather() {
        Scene scene;
        QVERIFY(scene.show());
        const QImage image = scene.render([](SgPainter& p) {
            p.setAntialiasing(true);
            p.drawLine(QPointF(10, 10), QPointF(150, 100), Qt::white, 2.0);
            // On a pixel's centre: one crisp row. Between two rows: two half-lit ones.
            p.drawLine(QPointF(20, 120.5), QPointF(120, 120.5), Qt::white, 1.0, Qt::FlatCap);
            p.drawLine(QPointF(20, 140), QPointF(120, 140), Qt::white, 1.0, Qt::FlatCap);
        });
        int partial = 0;  // along the diagonal, pixels partly covered
        for (int x = 20; x < 140; ++x) {
            for (int y = 0; y < 110; ++y) {
                const int v = qRed(at(image, x, y, scene.dpr()));
                if (v > 20 && v < 235)
                    ++partial;
            }
        }
        QVERIFY2(partial > 50, qPrintable(QString::number(partial)));
        CHECK_PIXEL(image, 80, 55, Qt::white);  // on the line: y = 10 + (80 - 10) * 90 / 140
        CHECK_PIXEL(image, 70, 120, Qt::white);
        CHECK_PIXEL(image, 70, 119, Qt::black);
        CHECK_PIXEL(image, 70, 121, Qt::black);
        const QRgb upper = at(image, 70, 139, scene.dpr()), lower = at(image, 70, 140, scene.dpr());
        QVERIFY2(qRed(upper) > 90 && qRed(upper) < 170, qPrintable(rgb(upper)));
        QVERIFY2(qRed(lower) > 90 && qRed(lower) < 170, qPrintable(rgb(lower)));
    }

    void polylinesJoinWithoutGaps() {
        Scene scene;
        QVERIFY(scene.show());
        const QImage image = scene.render([](SgPainter& p) {
            const QPointF zigzag[] = {{10, 100}, {40, 20}, {70, 100}, {100, 20}, {130, 100}};
            p.drawPolyline(zigzag, 5, Qt::white, 4.0);
            p.setAntialiasing(true);
            const QPointF corner[] = {{150, 20}, {190, 20}, {190, 60}};
            p.drawPolyline(corner, 3, Qt::red, 6.0, Qt::FlatCap);
        });
        // A mitred square corner: filled out to the outer corner.
        CHECK_PIXEL(image, 192, 18, Qt::red);
        CHECK_PIXEL(image, 189, 21, Qt::red);
        CHECK_PIXEL(image, 170, 20, Qt::red);
        CHECK_PIXEL(image, 190, 40, Qt::red);
        // Sharp turns are bevelled: their tips are there.
        CHECK_PIXEL(image, 40, 21, Qt::white);
        CHECK_PIXEL(image, 70, 99, Qt::white);
        CHECK_PIXEL(image, 25, 60, Qt::white);  // mid-segment
    }

    void clipping() {
        Scene scene;
        QVERIFY(scene.show());
        const QFont font = sub::ui::uiFont(14, true);
        const QImage image = scene.render([&](SgPainter& p) {
            p.save();
            p.setClipRect(QRectF(20, 20, 40, 40));
            p.fillRect(QRectF(0, 0, 200, 200), Qt::blue);  // only the clip shows
            p.setAntialiasing(true);
            p.fillEllipse(QRectF(40, 40, 60, 60), Qt::red);  // a corner of it shows
            p.drawText(QPointF(30, 50), QStringLiteral("WWWWWW"), Qt::white, font);
            p.restore();
            p.fillRect(QRectF(100, 100, 10, 10), Qt::green);  // unclipped again
            p.save();
            p.translate(120, 20);
            p.setClipRect(QRectF(0, 0, 30, 30));
            p.setClipRect(QRectF(10, 10, 40, 40));  // intersects: 10 to 30
            p.fillRect(QRectF(0, 0, 100, 100), Qt::yellow);
            p.restore();
        });
        CHECK_PIXEL(image, 20, 20, Qt::blue);
        CHECK_PIXEL(image, 59, 25, Qt::blue);
        CHECK_PIXEL(image, 19, 20, Qt::black);
        CHECK_PIXEL(image, 60, 25, Qt::black);
        CHECK_PIXEL(image, 25, 60, Qt::black);
        CHECK_PIXEL(image, 57, 57, Qt::red);
        CHECK_PIXEL(image, 70, 70, Qt::black);  // the ellipse outside the clip
        CHECK_PIXEL(image, 105, 105, Qt::green);
        CHECK_PIXEL(image, 135, 35, Qt::yellow);
        CHECK_PIXEL(image, 125, 35, Qt::black);
        CHECK_PIXEL(image, 149, 35, Qt::yellow);
        CHECK_PIXEL(image, 151, 35, Qt::black);
        // The text shows inside the clip and is cut at its right edge.
        const qreal dpr = scene.dpr();
        int white = 0;
        const QRect inside = device(QRectF(30, 36, 29, 14), dpr);
        for (int y = inside.top(); y <= inside.bottom(); ++y)
            for (int x = inside.left(); x <= inside.right(); ++x)
                if (near(image.pixel(x, y), qRgb(255, 255, 255), 40))
                    ++white;
        QVERIFY(white > 10);
        QCOMPARE(lit(image, device(QRectF(61, 30, 39, 9), dpr)), 0);  // above the ellipse, right of the clip
    }

    void textAndPaintOrder() {
        Scene scene(QSize(240, 120));
        QVERIFY(scene.show());
        const QFont font = sub::ui::uiFont(12, true);
        const QImage image = scene.render([&](SgPainter& p) {
            p.fillRect(QRectF(0, 0, 240, 40), QColor(0, 0, 128));
            p.drawText(QRectF(0, 0, 240, 40), Qt::AlignCenter, QStringLiteral("SUBSTATION"), Qt::white, font);
            p.drawText(QRectF(0, 50, 240, 30), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Covered"), Qt::white,
                       font);
            p.fillRect(QRectF(0, 50, 240, 30), Qt::red);  // drawn after the text: hides it
            p.drawText(QPointF(5, 110), QStringLiteral("Baseline"), Qt::yellow, font);
        });
        const qreal dpr = scene.dpr();
        // The text is over the rect drawn before it: white pixels inside the blue band.
        int white = 0;
        for (int y = 0; y < int(40 * dpr); ++y)
            for (int x = 0; x < int(240 * dpr); ++x)
                if (qRed(image.pixel(x, y)) > 200 && qBlue(image.pixel(x, y)) > 200)
                    ++white;
        QVERIFY2(white > 40, qPrintable(QString::number(white)));
        // Centred: nothing near the band's ends.
        for (int y = 0; y < int(40 * dpr); ++y)
            QVERIFY(qRed(image.pixel(int(4 * dpr), y)) < 10);
        // The rect drawn after the text covers it: all red.
        for (int y = int(51 * dpr); y < int(79 * dpr); ++y)
            for (int x = 0; x < int(240 * dpr); ++x)
                QVERIFY(near(image.pixel(x, y), qRgb(255, 0, 0)));
        // On a baseline: glyphs above y = 110, nothing much below.
        QVERIFY(lit(image, device(QRectF(5, 95, 80, 15), dpr)) > 20);
        QCOMPARE(lit(image, device(QRectF(5, 114, 80, 5), dpr)), 0);
        const SgCanvas::Stats stats = scene.canvas->lastStats();
        QCOMPARE(stats.textureNodes, 3);
        QCOMPARE(stats.solidNodes, 2);  // the rect before the texts, and the one after
    }

    void shapes() {
        Scene scene;
        QVERIFY(scene.show());
        const QImage image = scene.render([](SgPainter& p) {
            const QPointF l[] = {{10, 10}, {40, 10}, {40, 30}, {25, 30}, {25, 60}, {10, 60}};  // concave
            p.fillPolygon(l, 6, Qt::green);
            p.setAntialiasing(true);
            const QPointF triangle[] = {{60, 60}, {100, 10}, {140, 60}};
            p.fillPolygon(triangle, 3, Qt::white);
            p.fillEllipse(QRectF(150, 10, 40, 40), Qt::red);
            p.fillRoundedRect(QRectF(10, 80, 60, 30), 8, 8, Qt::blue);
            p.drawEllipse(QRectF(90, 80, 40, 40), Qt::yellow, 2.0);
            p.drawArc(QRectF(140, 80, 40, 40), 225, -270, Qt::cyan, 3.0);  // a knob's track
        });
        CHECK_PIXEL(image, 15, 15, Qt::green);
        CHECK_PIXEL(image, 35, 25, Qt::green);
        CHECK_PIXEL(image, 15, 55, Qt::green);
        CHECK_PIXEL(image, 35, 45, Qt::black);  // the L's notch
        CHECK_PIXEL(image, 100, 45, Qt::white);
        CHECK_PIXEL(image, 65, 20, Qt::black);
        CHECK_PIXEL(image, 170, 30, Qt::red);
        CHECK_PIXEL(image, 152, 12, Qt::black);  // outside the circle, inside its box
        CHECK_PIXEL(image, 40, 95, Qt::blue);
        CHECK_PIXEL(image, 10, 80, Qt::black);  // a rounded-off corner
        CHECK_PIXEL(image, 110, 100, Qt::black);  // the ring is hollow
        CHECK_PIXEL(image, 110, 80, Qt::yellow);
        CHECK_PIXEL(image, 160, 80, Qt::cyan);    // the arc's top (90 degrees)
        CHECK_PIXEL(image, 160, 119, Qt::black);  // its gap at the bottom (270 degrees)
    }

    void aBandIsAntialiased() {
        // A waveform's band: solid between its outlines, soft at their edges
        // (some pixels partly covered, not a pixel's staircase), and two
        // halves clipped side by side the same as the whole.
        Scene scene;
        QVERIFY(scene.show());
        const std::vector<float> top{40, 40, 20.5f, 40, 40}, bottom{44, 44, 60, 44, 44};
        const QImage image = scene.render([&](SgPainter& p) {
            p.fillBand(10, 10, top.data(), bottom.data(), 5, Qt::white);
            for (const auto& [x0, x1] : {std::pair{110.0, 135.0}, std::pair{135.0, 160.0}}) {
                p.save();
                p.setClipRect(QRectF(x0, 0, x1 - x0, 100));
                p.fillBand(110, 10, top.data(), bottom.data(), 5, Qt::white);
                p.restore();
            }
        });
        const qreal dpr = scene.dpr();
        CHECK_PIXEL(image, 30, 40, Qt::white);
        CHECK_PIXEL(image, 15, 42, Qt::white);
        CHECK_PIXEL(image, 30, 10, Qt::black);
        CHECK_PIXEL(image, 30, 70, Qt::black);
        int partial = 0;
        for (int y = 0; y < int(40 * dpr); ++y) {
            const int gray = qGray(image.pixel(int(25 * dpr), y));
            if (gray > 30 && gray < 225) ++partial;
        }
        QVERIFY2(partial >= 1, "no soft edge");
        for (int y = 0; y < int(80 * dpr); ++y) {
            for (int x = int(11 * dpr); x < int(49 * dpr); ++x) {
                const QRgb whole = image.pixel(x, y), halves = image.pixel(x + int(100 * dpr), y);
                if (!near(whole, halves, 2))
                    QFAIL(qPrintable(QStringLiteral("(%1, %2): %3 vs %4").arg(x).arg(y).arg(rgb(whole), rgb(halves))));
            }
        }
    }

    void columnsGradientImageOpacity() {
        Scene scene;
        QVERIFY(scene.show());
        QImage picture(4, 4, QImage::Format_ARGB32_Premultiplied);
        picture.fill(Qt::magenta);
        const QImage image = scene.render([&](SgPainter& p) {
            const std::vector<float> top{10, 20, 30, 40.25f}, bottom{50, 40, 31, 40.25f};
            p.fillColumns(10, 5, top.data(), bottom.data(), 4, Qt::white, 1.0);
            QLinearGradient gradient(QPointF(0, 60), QPointF(0, 100));
            gradient.setColorAt(0.0, Qt::red);
            gradient.setColorAt(0.5, Qt::red);
            gradient.setColorAt(1.0, Qt::blue);
            p.fillRect(QRectF(40, 60, 20, 40), gradient);
            p.drawImage(QRectF(80, 60, 40, 40), picture, QRectF(), false);
            p.setOpacity(0.5);
            p.fillRect(QRectF(130, 60, 20, 20), Qt::white);
            p.drawImage(QRectF(130, 90, 20, 20), picture);
        });
        CHECK_PIXEL(image, 12, 30, Qt::white);
        CHECK_PIXEL(image, 12, 9, Qt::black);
        CHECK_PIXEL(image, 17, 15, Qt::black);
        CHECK_PIXEL(image, 17, 25, Qt::white);
        CHECK_PIXEL(image, 22, 30, Qt::white);  // 30 to 31: one pixel
        CHECK_PIXEL(image, 27, 40, Qt::white);  // empty: made a pixel tall about its middle
        CHECK_PIXEL(image, 50, 65, Qt::red);
        CHECK_PIXEL(image, 50, 79, Qt::red);
        QVERIFY(near(at(image, 50, 99, scene.dpr()), qRgb(0, 0, 255), 8));  // 98.75 % of the way to blue
        const QRgb middle = at(image, 50, 90, scene.dpr());
        QVERIFY2(qRed(middle) > 80 && qBlue(middle) > 80, qPrintable(rgb(middle)));
        CHECK_PIXEL(image, 100, 80, Qt::magenta);
        CHECK_PIXEL(image, 140, 70, QColor(128, 128, 128));
        CHECK_PIXEL(image, 140, 100, QColor(128, 0, 128));
    }

    void nodesAreReused() {
        Scene scene;
        QVERIFY(scene.show());
        const QFont font = sub::ui::uiFont();
        auto draw = [&](QColor color) {
            return [=](SgPainter& p) {
                p.fillRect(QRectF(0, 0, 20, 20), color);
                p.drawText(QPointF(5, 40), QStringLiteral("x"), Qt::white, font);
                p.fillRect(QRectF(30, 0, 20, 20), color);
            };
        };
        scene.render(draw(Qt::red));
        QCOMPARE(scene.canvas->lastStats().nodesMade, 3);
        const QImage image = scene.render(draw(Qt::green));
        CHECK_PIXEL(image, 10, 10, Qt::green);
        CHECK_PIXEL(image, 40, 10, Qt::green);
        QCOMPARE(scene.canvas->lastStats().nodesMade, 3);  // none new
        QCOMPARE(scene.canvas->lastStats().solidNodes, 2);
        QCOMPARE(scene.canvas->lastStats().textureNodes, 1);
    }

    void textTexturesAreCached() {
        Scene scene;
        QVERIFY(scene.show());
        const QFont font = sub::ui::uiFont();
        auto labels = [&](int count) {
            return [=](SgPainter& p) {
                for (int i = 0; i < count; ++i)
                    p.drawText(QPointF(5, 15 + (i % 8) * 16), QStringLiteral("Bar %1").arg(i), Qt::white, font);
            };
        };
        scene.render(labels(8));
        // The same texts again: drawn from the cache. (Hits and misses are per window.)
        scene.render(labels(8));
        QCOMPARE(scene.canvas->lastStats().textureNodes, 8);
        QVERIFY(sub::ui::SgTextureCache::windowCount() >= 1);
    }

    // A big arrangement's worth: how long painting and building take, and frames.
    void benchmark_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<int>("count");
        QTest::newRow("5k rects") << QStringLiteral("rects") << 5000;
        QTest::newRow("20k rects") << QStringLiteral("rects") << 20000;
        QTest::newRow("50k rects") << QStringLiteral("rects") << 50000;
        QTest::newRow("100k rects") << QStringLiteral("rects") << 100000;
        QTest::newRow("2k labels") << QStringLiteral("labels") << 2000;  // 200 different texts, cached
        QTest::newRow("20k-point antialiased polyline") << QStringLiteral("polyline") << 20000;
    }

    void benchmark() {
        QFETCH(QString, kind);
        QFETCH(int, count);
        Scene scene(QSize(1600, 900));
        QVERIFY(scene.show());
        struct Rect {
            QRectF r;
            QColor c;
        };
        std::vector<Rect> items;
        std::vector<QPointF> curve;
        QStringList texts;
        QRandomGenerator random(42);
        for (int i = 0; i < count; ++i)
            items.push_back({QRectF(random.bounded(1600.0), random.bounded(900.0), 2 + random.bounded(60.0),
                                    2 + random.bounded(20.0)),
                             QColor::fromRgb(random.bounded(256), random.bounded(256), random.bounded(256))});
        for (int i = 0; i < count; ++i)
            curve.emplace_back(i * 1600.0 / count, 450 + 300 * std::sin(i * 0.01) * std::cos(i * 0.0007));
        for (int i = 0; i < 200; ++i)
            texts << QStringLiteral("Clip %1").arg(i);
        const QFont font = sub::ui::uiFont(8);
        int shift = 0;
        auto draw = [&](SgPainter& p) {
            p.save();
            p.setClipRect(QRectF(0, 0, 1600, 880));
            p.translate(shift % 7, 0);  // a scroll: every frame differs
            if (kind == QLatin1String("rects")) {
                for (const Rect& item : items)
                    p.fillRect(item.r, item.c);
            } else if (kind == QLatin1String("labels")) {
                for (int i = 0; i < count; ++i)
                    p.drawText(items[size_t(i)].r.topLeft(), texts[i % texts.size()], Qt::white, font);
            } else {
                p.setAntialiasing(true);
                p.drawPolyline(curve.data(), int(curve.size()), Qt::white, 1.5);
            }
            p.restore();
        };
        scene.render(draw);  // warm up (and fill the text cache)
        const int frames = 30;
        qint64 paintNs = 0, buildNs = 0;
        QElapsedTimer wall;
        wall.start();
        for (int i = 0; i < frames; ++i) {
            ++shift;
            scene.canvas->update();
            const int before = scene.frames;
            QVERIFY(QTest::qWaitFor([&] { return scene.frames > before; }, 5000));
            paintNs += scene.canvas->lastStats().paintNs;
            buildNs += scene.canvas->lastStats().buildNs;
        }
        const double frameMs = wall.nsecsElapsed() / 1e6 / frames;
        const double paintMs = paintNs / 1e6 / frames, buildMs = buildNs / 1e6 / frames;
        qInfo("%s: paint %.2f ms + nodes %.2f ms = %.2f ms of CPU a frame; %.1f ms a frame in all "
              "(here the GPU is Mesa's llvmpipe, on the CPU too)",
              QTest::currentDataTag(), paintMs, buildMs, paintMs + buildMs, frameMs);
        if (kind == QLatin1String("rects")) {
            // As few nodes as the renderer draws: at most 65535 vertices each (six a rect).
            QVERIFY(scene.canvas->lastStats().solidNodes <= (count * 6 + 65531) / 65532);
            // The CPU side of 50k rects stays well inside a 60 Hz frame.
            if (count <= 50000)
                QVERIFY2(paintMs + buildMs < 16.0, qPrintable(QString::number(paintMs + buildMs)));
        }
    }
};

QTEST_MAIN(TestUiSg)
#include "test_ui_sg.moc"
