// Pure clip maths: overlaps, cuts, trims, splits, tempo fitting, warping and
// reversing (the pure-function parts of tests/test_edits.py).

#include "TestSupport.h"

#include "model/Edits.h"
#include "model/Ids.h"

#include <QTest>

#include <cmath>

using namespace sub::app;

namespace {

constexpr double kTempo = 120.0;  // 1 beat = 0.5 s

Clip clip(double start, double beats, double offset = 0.0, double source = 100.0, const QString& id = {}) {
    return Clip::audio(id.isEmpty() ? newId() : id, QStringLiteral("a.wav"), QStringLiteral("a"), start, beats * 0.5,
                       offset, source);
}

// A warped clip `beats` long; at 60 BPM a beat is one second of audio.
Clip warped(double start, double beats, double segmentBpm = 60.0, const QString& id = {}, double source = 100.0) {
    Clip c = Clip::audio(id.isEmpty() ? newId() : id, QStringLiteral("a.wav"), QStringLiteral("a"), start,
                         beats * 60.0 / segmentBpm, 0.0, source);
    c.warp = true;
    c.segmentBpm = segmentBpm;
    return c;
}

double round6(double value) { return std::round(value * 1e6) / 1e6; }

using Spans = std::vector<std::pair<double, double>>;

Spans spans(const std::vector<Clip>& clips, double tempo = kTempo) {
    Spans result;
    for (const Clip& c : clips) result.emplace_back(round6(c.startBeat), round6(c.endBeat(tempo)));
    return result;
}

bool near(double a, double b, double tolerance = 1e-9) { return std::abs(a - b) <= tolerance; }

}  // namespace

class TestEdits : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { test::prepareApplication(); }

    void overlapTrimsPartiallyCoveredClips() {
        const auto result = edits::resolveOverlaps({clip(0, 4, 0, 100, "old"), clip(3, 4, 0, 100, "new")}, {"new"}, kTempo);
        QVERIFY((spans(result) == Spans{{0, 3}, {3, 7}}));
        QCOMPARE(result[0].id, QStringLiteral("old"));
    }

    void overlapTrimsStartAndShiftsOffset() {
        const auto result = edits::resolveOverlaps({clip(2, 4, 0, 100, "old"), clip(0, 3, 0, 100, "new")}, {"new"}, kTempo);
        QVERIFY((spans(result) == Spans{{0, 3}, {3, 6}}));
        QVERIFY(near(result[1].offsetSec, 0.5));  // one beat of audio skipped
    }

    void overlapSplitsEnclosingClip() {
        const auto result = edits::resolveOverlaps({clip(0, 8, 0, 100, "old"), clip(2, 2, 0, 100, "new")}, {"new"}, kTempo);
        QVERIFY((spans(result) == Spans{{0, 2}, {2, 4}, {4, 8}}));
        QCOMPARE(result[0].id, QStringLiteral("old"));
        QVERIFY(result[2].id != u"old" && result[2].id != u"new");
        QVERIFY(near(result[2].offsetSec, 2.0));
    }

    void overlapRemovesCoveredClipAndKeepsNeighbours() {
        const auto result = edits::resolveOverlaps(
            {clip(1, 1, 0, 100, "covered"), clip(4, 1, 0, 100, "touching"), clip(0, 4, 0, 100, "new")}, {"new"}, kTempo);
        QCOMPARE(result.size(), size_t(2));
        QCOMPARE(result[0].id, QStringLiteral("new"));
        QCOMPARE(result[1].id, QStringLiteral("touching"));
    }

    void removeRangeKeepsStartAndEndOfSpanningClip() {
        const Clip longClip = clip(0.0, 8.0, 0, 100, "long");
        auto after = edits::removeRange({longClip, clip(10.0, 1.0), clip(11.5, 2.0, 0, 100, "across")}, 2.0, 12.0, kTempo);
        after = edits::removeRange(after, 3.0, 5.0, kTempo);
        QVERIFY((spans(after) == Spans{{0.0, 2.0}, {12.0, 13.5}}));
        // A middle cut leaves both ends, each still playing its own part of the audio.
        const auto pieces = edits::removeRange({longClip}, 3.0, 5.0, kTempo);
        QVERIFY((spans(pieces) == Spans{{0.0, 3.0}, {5.0, 8.0}}));
        QVERIFY(pieces[0].id == u"long" && pieces[1].id != u"long");
        QVERIFY(near(pieces[1].offsetSec, 2.5));
        QVERIFY(edits::removeRange({longClip}, 9.0, 10.0, kTempo) == std::vector<Clip>{longClip});  // untouched
    }

    void splitClip() {
        const auto parts = edits::splitClip(clip(0, 4, 1.0), 1.0, kTempo);
        QVERIFY(parts);
        QVERIFY((spans({parts->first, parts->second}) == Spans{{0, 1}, {1, 4}}));
        QVERIFY(near(parts->second.offsetSec, 1.5));
        QVERIFY(!edits::splitClip(clip(0, 4), 4.0, kTempo));
        QVERIFY(!edits::splitClip(clip(0, 4), -1.0, kTempo));
    }

    void trimStartKeepsAudioInPlace() {
        const Clip c = clip(4, 4, 1.0);
        Clip t = edits::trimStart(c, 5.0, kTempo);
        QCOMPARE(t.startBeat, 5.0);
        QCOMPARE(t.offsetSec, 1.5);
        QCOMPARE(t.durationSec, 1.5);
        // Cannot reveal audio before the start of the file.
        t = edits::trimStart(c, 0.0, kTempo);
        QVERIFY(near(t.startBeat, 2.0));
        QCOMPARE(t.offsetSec, 0.0);
    }

    void trimEndLimitedBySource() {
        const Clip c = clip(0, 2, 1.0, 3.0);
        QVERIFY(near(edits::trimEnd(c, 1.0, kTempo).durationSec, 0.5));
        QVERIFY(near(edits::trimEnd(c, 100.0, kTempo).durationSec, 2.0));
        QVERIFY(near(edits::trimEnd(c, -5.0, kTempo).durationSec, edits::kMinClipSec));
    }

    void fitToTempoLeavesNonOverlappingClipsAlone() {
        const std::vector<Clip> clips{clip(0.0, 2.0), clip(4.0, 2.0)};
        bool changed = true;
        QVERIFY(edits::fitToTempo(clips, kTempo, &changed) == clips);
        QVERIFY(!changed);
        for (const auto& [factor, end] : {std::pair{2.0, 8.0}, std::pair{3.0, 10.0}}) {  // the first would grow to 4 or 6 beats
            const auto fitted = edits::fitToTempo(clips, kTempo * factor, &changed);
            QCOMPARE(changed, factor == 3.0);  // (at twice the tempo the first just reaches the second)
            QVERIFY((spans(fitted, kTempo * factor) == Spans{{0.0, 4.0}, {4.0, end}}));
        }
    }

    void fitToTempoDropsCoveredClips() {
        // Two clips starting together: the first is cut to nothing and goes.
        const std::vector<Clip> clips{clip(0.0, 2.0, 0, 100, "a"), clip(0.0, 1.0, 0, 100, "b")};
        const auto fitted = edits::fitToTempo(clips, kTempo);
        QCOMPARE(fitted.size(), size_t(1));
        QCOMPARE(fitted[0].id, QStringLiteral("b"));
    }

    void warpedClipLengthIsFixedInBeats() {
        const Clip c = warped(0.0, 4.0);
        for (double tempo : {60.0, 120.0, 175.0}) QVERIFY(near(c.lengthBeats(tempo), 4.0));
        // Unwarped (or warped without a segment BPM) it follows the tempo again.
        Clip unwarped = c;
        unwarped.warp = false;
        QVERIFY(near(unwarped.lengthBeats(120.0), 8.0));
        Clip unset = c;
        unset.segmentBpm = 0.0;
        QVERIFY(near(unset.lengthBeats(120.0), 8.0));
    }

    void editingWarpedClipsMeasuresSourceAtSegmentBpm() {
        const Clip c = warped(2.0, 4.0, 60.0, "w");  // 4 s of audio over beats 2..6
        const auto parts = edits::splitClip(c, 3.0, kTempo);
        QVERIFY(parts);
        QVERIFY(near(parts->first.durationSec, 1.0) && near(parts->second.offsetSec, 1.0) &&
                near(parts->second.durationSec, 3.0));
        Clip offset = c;
        offset.offsetSec = 5.0;
        const Clip trimmed = edits::trimStart(offset, 1.0, kTempo);
        QVERIFY(near(trimmed.startBeat, 1.0) && near(trimmed.offsetSec, 4.0) && near(trimmed.durationSec, 5.0));
        QVERIFY(near(edits::trimEnd(c, 5.0, kTempo).durationSec, 3.0));
        const auto pieces = edits::removeRange({c}, 3.0, 4.0, kTempo);
        QVERIFY((spans(pieces) == Spans{{2.0, 3.0}, {4.0, 6.0}}));
        QVERIFY(near(pieces[1].offsetSec, 2.0));
    }

    void sliceRangeTakesJustTheRange() {
        const Clip whole = clip(1.0, 2.0, 0, 100, "whole");
        const Clip across = clip(2.0, 4.0, 0, 100, "across");
        const auto slices = edits::sliceRange({whole, across}, 0.0, 4.0, kTempo);
        QVERIFY((spans(slices) == Spans{{1.0, 3.0}, {2.0, 4.0}}));
        QVERIFY(slices[0].id != u"whole" && slices[1].id != u"across");
        QCOMPARE(edits::sliceRange({whole}, 0.0, 4.0, kTempo, true)[0].id, QStringLiteral("whole"));
    }

    // (The pure part of test_reversing_plays_a_stretch_backwards_and_again_forwards.)
    void reverseClip() {
        Clip c = Clip::audio(QStringLiteral("c"), QStringLiteral("a.wav"), QStringLiteral("a"), 0.0, 4.0, 0.0, 4.0);
        c.offsetSec = 0.5;
        c.durationSec = 2.0;
        const Clip flipped = edits::reverseClip(c, QStringLiteral("a R.wav"), 4.0);
        // It plays seconds 0.5-2.5 of the file backwards: seconds 1.5-3.5 of the reversed copy.
        QCOMPARE(flipped.path, QStringLiteral("a R.wav"));
        QCOMPARE(flipped.offsetSec, 1.5);
        QCOMPARE(flipped.durationSec, 2.0);
        QCOMPARE(flipped.reversedFrom, QStringLiteral("a.wav"));
        QCOMPARE(flipped.startBeat, c.startBeat);
        // Reversed again, it plays its file again (forwards), and forgets it was reversed.
        const Clip back = edits::reverseClip(flipped, QStringLiteral("a.wav"), 4.0);
        QCOMPARE(back.path, QStringLiteral("a.wav"));
        QCOMPARE(back.offsetSec, 0.5);
        QCOMPARE(back.reversedFrom, QString());
    }

    void subtractIntervals() {
        using Intervals = std::vector<edits::Interval>;
        QVERIFY((edits::subtractIntervals(0, 10, {{2, 3}, {5, 6}}) == Intervals{{0, 2}, {3, 5}, {6, 10}}));
        QVERIFY((edits::subtractIntervals(0, 10, {{-1, 11}}) == Intervals{}));
        QVERIFY((edits::subtractIntervals(0, 10, {{10, 12}}) == Intervals{{0, 10}}));  // touching: nothing cut
    }
};

QTEST_GUILESS_MAIN(TestEdits)
#include "test_edits.moc"
