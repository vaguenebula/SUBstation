#include "model/TrackNames.h"

#include "model/Devices.h"
#include "model/Track.h"

#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

namespace sub::app {

namespace {

// A file's stem, as it names a track: a take's without the time it was
// recorded at (takePath: "Kick 2026-10-08 141500", " 2" or "_" after it).
QString fileLabel(const QString& path) {
    static const QRegularExpression take(QStringLiteral("^(.+) \\d{4}-\\d{2}-\\d{2} \\d{6}(?: \\d+)?_*$"));
    const QString stem = QFileInfo(path).completeBaseName();
    const QRegularExpressionMatch match = take.match(stem);
    return match.hasMatch() ? match.captured(1) : stem;
}

}  // namespace

QString numberedName(const QString& nameTemplate, int number) {
    QString name = nameTemplate;
    return name.replace(QChar(kNumberMark), QString::number(number));
}

QString contentsLabel(const Track& track) {
    if (track.isAudio()) {
        QHash<QString, int> count;
        QString best;
        for (const Clip& clip : track.clips) {  // (in time order: a tie goes to the first played)
            if (!clip.isAudio()) continue;
            const QString label = fileLabel(clip.reversedFrom.isEmpty() ? clip.path : clip.reversedFrom);
            if (label.isEmpty()) continue;
            const int n = ++count[label];
            if (best.isEmpty() || n > count.value(best)) best = label;
        }
        return best.isEmpty() ? QStringLiteral("Audio") : best;
    }
    if (track.isMidi()) {
        for (const Device& device : track.devices) {
            if (deviceIsInstrument(device)) return deviceName(device);
        }
        return QStringLiteral("MIDI");
    }
    return {};
}

QString contentsName(const Track& track) { return QChar(kNumberMark) + u' ' + contentsLabel(track); }

bool namedByContents(const Track& track) { return track.hasClips() && track.nameSource() == contentsName(track); }

bool hasPlainName(const Track& track) {
    static const QRegularExpression plain(QStringLiteral("^\\d+ (Audio|MIDI|Group)$"));
    const QRegularExpressionMatch match = plain.match(track.nameSource());
    if (!match.hasMatch()) return false;
    const QString word = match.captured(1);
    return (track.isAudio() && word == u"Audio") || (track.isMidi() && word == u"MIDI") ||
           (track.isGroup() && word == u"Group");
}

QString plainNameTemplate(const Track& track) {
    return track.isGroup() ? QStringLiteral("# Group") : contentsName(track);
}

QString takeName(const Track& track) { return namedByContents(track) ? contentsLabel(track) : track.name; }

}  // namespace sub::app
