// Musical time: positions, bar labels, dB and pan text (tests/test_timebase.py).

#include "TestSupport.h"

#include "model/Timebase.h"

#include <QTest>

using namespace sub::app;

namespace {
const TimeSignature kFourFour{4, 4};
const TimeSignature kSixEight{6, 8};
}  // namespace

class TestTimebase : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void timeSignatureLengths() {
        QCOMPARE(kFourFour.beatsPerBar(), 4.0);
        QCOMPARE(kSixEight.beatsPerBar(), 3.0);
        QCOMPARE(kSixEight.beatLength(), 0.5);
        QCOMPARE(kSixEight.toString(), QStringLiteral("6/8"));
    }

    void secondsAndBeats() {
        QCOMPARE(beatsToSeconds(4.0, 120.0), 2.0);
        QCOMPARE(secondsToBeats(2.0, 120.0), 4.0);
    }

    void formatPosition_data() {
        QTest::addColumn<double>("beats");
        QTest::addColumn<int>("numerator");
        QTest::addColumn<int>("denominator");
        QTest::addColumn<QString>("expected");
        QTest::newRow("start") << 0.0 << 4 << 4 << QStringLiteral("1.1.1");
        QTest::newRow("beat 2") << 1.0 << 4 << 4 << QStringLiteral("1.2.1");
        QTest::newRow("bar 2") << 4.25 << 4 << 4 << QStringLiteral("2.1.2");
        QTest::newRow("end of bar 4") << 15.99 << 4 << 4 << QStringLiteral("4.4.4");
        QTest::newRow("6/8 bar 2") << 3.0 << 6 << 8 << QStringLiteral("2.1.1");
        QTest::newRow("6/8 beat 2") << 3.5 << 6 << 8 << QStringLiteral("2.2.1");
    }

    void formatPosition() {
        QFETCH(double, beats);
        QFETCH(int, numerator);
        QFETCH(int, denominator);
        QFETCH(QString, expected);
        QCOMPARE(sub::app::formatPosition(beats, TimeSignature{numerator, denominator}), expected);
    }

    void parsePositionRoundTrip() {
        for (double beats : {0.0, 1.0, 4.25, 13.75}) {
            QCOMPARE(parsePosition(sub::app::formatPosition(beats, kFourFour), kFourFour), std::optional<double>(beats));
        }
        QCOMPARE(parsePosition(QStringLiteral("3"), kFourFour), std::optional<double>(8.0));
        QVERIFY(!parsePosition(QStringLiteral("nonsense"), kFourFour));
        QVERIFY(!parsePosition(QStringLiteral("0.1.1"), kFourFour));
        QCOMPARE(parsePosition(QStringLiteral(" 2:2 "), kFourFour), std::optional<double>(5.0));  // colons too
        QVERIFY(!parsePosition(QStringLiteral("1.1.1.1"), kFourFour));
    }

    void barLabels() {
        QCOMPARE(formatBarLabel(8.0, kFourFour), QStringLiteral("3"));
        QCOMPARE(formatBarLabel(9.0, kFourFour), QStringLiteral("3.2"));
        QCOMPARE(formatBarLabel(9.25, kFourFour), QStringLiteral("3.2.2"));
    }

    void valueFormatting() {
        QCOMPARE(formatDb(-80.0), QStringLiteral("-inf dB"));
        QCOMPARE(formatDb(-6.02), QStringLiteral("-6.0 dB"));
        QCOMPARE(formatPan(0.0), QStringLiteral("C"));
        QCOMPARE(formatPan(-1.0), QStringLiteral("50L"));
        QCOMPARE(formatPan(0.5), QStringLiteral("25R"));
        QCOMPARE(formatPan(0.01), QStringLiteral("C"));  // half a step rounds to even
    }

    void panText() {
        QCOMPARE(parsePan(QStringLiteral("25L")), std::optional<double>(-0.5));
        QCOMPARE(parsePan(QStringLiteral("30r")), std::optional<double>(0.6));
        QCOMPARE(parsePan(QStringLiteral(" Centre ")), std::optional<double>(0.0));
        QCOMPARE(parsePan(QStringLiteral("-100")), std::optional<double>(-1.0));  // clamped
        QVERIFY(!parsePan(QStringLiteral("left")));
    }

    void gains() {
        QCOMPARE(dbToGain(-70.0), 0.0);
        QVERIFY(qFuzzyCompare(dbToGain(-6.0), 0.501187233627272));
        QVERIFY(std::isinf(gainToDb(0.0)));
        QVERIFY(qFuzzyCompare(gainToDb(10.0), 20.0));
    }
};

QTEST_GUILESS_MAIN(TestTimebase)
#include "test_timebase.moc"
