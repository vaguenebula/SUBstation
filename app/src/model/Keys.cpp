#include "model/Keys.h"

#include "model/Clip.h"
#include "model/Numbers.h"

#include <QRegularExpression>

namespace sub::app {

namespace {

int tonicOf(QChar letter) {
    switch (letter.toUpper().unicode()) {
    case u'C': return 0;
    case u'D': return 2;
    case u'E': return 4;
    case u'F': return 5;
    case u'G': return 7;
    case u'A': return 9;
    case u'B': return 11;
    default: return 0;
    }
}

int pitchClass(int value) { return floorMod(value, 12); }

// --- Reading file names ---
// Not in the middle of a word or number.
#define SUB_SEP "(?<![A-Za-z0-9])"
#define SUB_END "(?![A-Za-z0-9])"
#define SUB_NUMBER "(\\d{2,3}(?:\\.\\d{1,2})?)"

// "128bpm", "128 BPM", "bpm128", "BPM_128"
const QRegularExpression& bpmTagged() {
    static const QRegularExpression re(QStringLiteral(SUB_SEP SUB_NUMBER "[\\s_-]?bpm" SUB_END "|" SUB_SEP
                                                      "bpm[\\s_-]?" SUB_NUMBER SUB_END),
                                       QRegularExpression::CaseInsensitiveOption);
    return re;
}

// A number standing on its own: "Loop_128_Am"
const QRegularExpression& bpmBare() {
    static const QRegularExpression re(QStringLiteral(SUB_SEP SUB_NUMBER SUB_END));
    return re;
}

// A tonic with a spelled-out mode: "A minor", "Ebmaj", "f# min", "Cmaj7". The letter may be lower case.
const QRegularExpression& keySpelled() {
    static const QRegularExpression re(
        QString::fromUtf8(SUB_SEP "([A-Ga-g])(#|♯|b|♭|sharp|flat|s(?=[\\s_-]*(?:maj|min|m)))?"
                                  "[\\s_-]?(major|minor|maj|min)(?![A-Za-z])"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// Short forms, upper-case tonic only (so words like "am" don't count): "Am", "F#m", "C#", "D".
const QRegularExpression& keyShort() {
    static const QRegularExpression re(QString::fromUtf8(SUB_SEP "([A-G])(#|♯|b|♭)?(m)?" SUB_END));
    return re;
}

// Words for a key: "key of A", "Key_Am"
const QRegularExpression& keyWord() {
    static const QRegularExpression re(QStringLiteral("key"), QRegularExpression::CaseInsensitiveOption);
    return re;
}

#undef SUB_SEP
#undef SUB_END
#undef SUB_NUMBER

std::optional<double> bpmOf(const QString& number) {
    bool ok = false;
    const double value = number.toDouble(&ok);
    if (!ok || value < kMinBpm || value > kMaxBpm) return std::nullopt;
    return value;
}

Key keyOf(const QString& letter, const QString& accidental, bool minor) {
    int shift = 0;
    if (!accidental.isEmpty()) {
        const QString lower = accidental.toLower();
        shift = (lower == u"#" || lower == QString(QChar(0x266F)) || lower == u"sharp" || lower == u"s") ? 1 : -1;
    }
    return Key{pitchClass(tonicOf(letter.at(0)) + shift), minor};
}

}  // namespace

QString Key::name() const { return kKeyNoteNames.at(tonic) + (minor ? QStringLiteral("m") : QString()); }

QString Key::label() const {
    return kKeyNoteNames.at(tonic) + (minor ? QStringLiteral(" Minor") : QStringLiteral(" Major"));
}

int Key::relativeMajor() const { return minor ? (tonic + 3) % 12 : tonic; }

std::vector<Key> allKeys() {
    std::vector<Key> keys;
    for (int tonic = 0; tonic < 12; ++tonic) {
        keys.push_back({tonic, false});
        keys.push_back({tonic, true});
    }
    return keys;
}

std::optional<Key> keyFromName(const QString& text) {
    if (text.isEmpty()) return std::nullopt;
    static const QRegularExpression re(QStringLiteral("\\A([A-G])([#b]?)(m?)\\z"));
    const auto match = re.match(text.trimmed());
    if (!match.hasMatch()) return std::nullopt;
    const QString accidental = match.captured(2);
    const int shift = accidental == u"#" ? 1 : accidental == u"b" ? -1 : 0;
    return Key{pitchClass(tonicOf(match.captured(1).at(0)) + shift), !match.captured(3).isEmpty()};
}

int transposeTo(const std::optional<Key>& source, const std::optional<Key>& target) {
    if (!source || !target) return 0;
    const int shift = pitchClass(target->relativeMajor() - source->relativeMajor());
    return shift >= 6 ? shift - 12 : shift;
}

FileInfo parseFilename(const QString& name) {
    static const QRegularExpression extension(QStringLiteral("\\.[A-Za-z0-9]{2,4}$"));
    QString stem = name;
    stem.replace(extension, QString());

    std::optional<double> bpm;
    qsizetype taggedStart = -1;
    qsizetype taggedEnd = -1;
    auto tagged = bpmTagged().globalMatch(stem);
    while (tagged.hasNext()) {
        const auto match = tagged.next();
        const QString number = !match.captured(1).isEmpty() ? match.captured(1) : match.captured(2);
        bpm = bpmOf(number);
        if (bpm) {
            taggedStart = match.capturedStart(0);
            taggedEnd = match.capturedEnd(0);
            break;
        }
    }
    if (!bpm) {
        // A bare number counts if it is the only one in tempo range, so a take
        // or version number next to it ("Loop 2 120") doesn't confuse it.
        std::vector<double> bare;
        auto matches = bpmBare().globalMatch(stem);
        while (matches.hasNext()) {
            if (const auto value = bpmOf(matches.next().captured(1))) bare.push_back(*value);
        }
        if (bare.size() == 1) bpm = bare.front();
    }

    std::optional<Key> key;
    QRegularExpressionMatch lastSpelled;
    auto spelled = keySpelled().globalMatch(stem);
    while (spelled.hasNext()) lastSpelled = spelled.next();
    if (lastSpelled.hasMatch()) {
        // Names put the key last more often than not.
        key = keyOf(lastSpelled.captured(1), lastSpelled.captured(2),
                    lastSpelled.captured(3).toLower().startsWith(u"min"));
    } else {
        const QString searchable =
            taggedStart < 0 ? stem : stem.left(taggedStart) + u' ' + stem.mid(taggedEnd);
        std::vector<std::pair<bool, Key>> candidates;
        auto shorts = keyShort().globalMatch(searchable);
        while (shorts.hasNext()) {
            const auto match = shorts.next();
            const QString accidental = match.captured(2);
            const bool minor = !match.captured(3).isEmpty();
            const bool explicitKey = !accidental.isEmpty() || minor;
            const qsizetype start = match.capturedStart(0);
            const qsizetype from = std::max<qsizetype>(0, start - 5);
            const bool afterKeyWord = keyWord().match(searchable.mid(from, start - from)).hasMatch();
            // A bare capital ("A", "D") is too common a word to trust alone: take it
            // only next to a tempo or after "key", and never as the first word ("A Day").
            if (!explicitKey && !afterKeyWord && (!bpm || start == 0)) continue;
            candidates.emplace_back(explicitKey, keyOf(match.captured(1), accidental, minor));
        }
        if (!candidates.empty()) {
            std::optional<Key> lastExplicit;
            for (const auto& [isExplicit, candidate] : candidates) {
                if (isExplicit) lastExplicit = candidate;
            }
            key = lastExplicit ? lastExplicit : candidates.back().second;
        }
    }
    return {bpm, key};
}

void ClipSettings::applyTo(Clip& clip) const {
    if (warp) clip.warp = *warp;
    if (segmentBpm) clip.segmentBpm = *segmentBpm;
    if (transpose) clip.transpose = *transpose;
}

ClipSettings clipSettings(const QString& name, double durationSec, double tempo,
                          const std::optional<Key>& projectKey) {
    const FileInfo info = parseFilename(name);
    ClipSettings settings;
    if (info.bpm || durationSec >= kAutoWarpMinSec) {
        settings.warp = true;
        settings.segmentBpm = info.bpm ? *info.bpm : tempo;
    }
    if (const int transpose = transposeTo(info.key, projectKey); transpose != 0) settings.transpose = transpose;
    return settings;
}

}  // namespace sub::app
