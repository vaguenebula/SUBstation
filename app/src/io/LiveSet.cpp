#include "io/LiveSet.h"

#include "model/Errors.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QXmlStreamReader>

#include <array>
#include <new>

extern "C" {
#include "puff.h"
}

namespace sub::app::live {

namespace {

// What the importer never reads, left out as the file is read (a third of a
// Live Set): Session View clips, frozen tracks' clips, MIDI controllers'
// targets, take lanes, views, the groove pool, presets' and browsers' records.
const QSet<QString>& skippedElements() {
    static const QSet<QString> skipped{QStringLiteral("ClipSlotList"),
                                       QStringLiteral("FreezeSequencer"),
                                       QStringLiteral("MidiControllers"),
                                       QStringLiteral("TakeLanes"),
                                       QStringLiteral("ViewStates"),
                                       QStringLiteral("GroovePool"),
                                       QStringLiteral("Scenes"),
                                       QStringLiteral("PreHearTrack"),
                                       QStringLiteral("SourceContext"),
                                       QStringLiteral("LastPresetRef"),
                                       QStringLiteral("ScrollerTimePreserver"),
                                       QStringLiteral("ExpressionGrid"),
                                       QStringLiteral("FollowAction"),
                                       QStringLiteral("NoteProbabilityGroups"),
                                       QStringLiteral("AutomationLanes"),
                                       QStringLiteral("ClipEnvelopeChooserViewState"),
                                       QStringLiteral("Onsets"),
                                       QStringLiteral("SavedWarpMarkersForStretched"),
                                       QStringLiteral("SequencerNavigator"),
                                       QStringLiteral("DetailClipKeyMidis"),
                                       QStringLiteral("MidiCCOnOffThresholds"),
                                       QStringLiteral("MidiControllerRange"),
                                       QStringLiteral("SlicePoints"),
                                       QStringLiteral("ManualSlicePoints"),
                                       QStringLiteral("BeatSlicePoints"),
                                       QStringLiteral("RegionSlicePoints"),
                                       QStringLiteral("InitialSlicePointsFromOnsets")};
    return skipped;
}

uint32_t readLe32(const QByteArray& bytes, qsizetype at) {
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.constData() + at);
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint32_t crc32(const QByteArray& bytes) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (const char byte : bytes) crc = table[(crc ^ static_cast<unsigned char>(byte)) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

[[noreturn]] void damaged(const QString& name, const QString& why) {
    throw ProjectFileError(QStringLiteral("%1 is damaged: %2").arg(name, why));
}

}  // namespace

// --- Element ----------------------------------------------------------------------------------

QString Element::attribute(QStringView name) const {
    for (const auto& [key, value] : attributes) {
        if (key == name) return value;
    }
    return {};
}

bool Element::hasAttribute(QStringView name) const {
    for (const auto& attribute : attributes) {
        if (attribute.first == name) return true;
    }
    return false;
}

const Element* Element::child(QStringView name) const {
    for (const auto& c : children) {
        if (c->tag == name) return c.get();
    }
    return nullptr;
}

const Element* Element::at(QStringView path) const {
    const Element* element = this;
    qsizetype start = 0;
    while (element && start < path.size()) {
        qsizetype end = path.indexOf(u'/', start);
        if (end < 0) end = path.size();
        element = element->child(path.mid(start, end - start));
        start = end + 1;
    }
    return element;
}

std::vector<const Element*> Element::all(QStringView name) const {
    std::vector<const Element*> found;
    for (const auto& c : children) {
        if (name.isEmpty() || c->tag == name) found.push_back(c.get());
    }
    return found;
}

std::vector<const Element*> Element::descendants(QStringView name) const {
    std::vector<const Element*> found;
    collect(name, found);
    return found;
}

void Element::collect(QStringView name, std::vector<const Element*>& found) const {
    for (const auto& c : children) {
        if (c->tag == name) found.push_back(c.get());
        c->collect(name, found);
    }
}

std::optional<QString> Element::value(QStringView path) const {
    const Element* element = at(path);
    if (!element || !element->hasAttribute(u"Value")) return std::nullopt;
    return element->attribute(u"Value");
}

double Element::number(QStringView path, double fallback) const {
    const std::optional<QString> text = value(path);
    if (!text) return fallback;
    bool ok = false;
    const double parsed = text->toDouble(&ok);
    return ok ? parsed : fallback;
}

int Element::integer(QStringView path, int fallback) const {
    const std::optional<QString> text = value(path);
    if (!text) return fallback;
    bool ok = false;
    const double parsed = text->toDouble(&ok);  // (Live writes some as "3.0")
    return ok ? static_cast<int>(parsed) : fallback;
}

bool Element::flag(QStringView path, bool fallback) const {
    const std::optional<QString> text = value(path);
    if (!text) return fallback;
    if (*text == u"true" || *text == u"1") return true;
    if (*text == u"false" || *text == u"0") return false;
    return fallback;
}

double Element::numberAttribute(QStringView name, double fallback) const {
    if (!hasAttribute(name)) return fallback;
    bool ok = false;
    const double parsed = attribute(name).toDouble(&ok);
    return ok ? parsed : fallback;
}

// --- Reading ----------------------------------------------------------------------------------

bool isGzip(const QByteArray& bytes) {
    return bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0x1F &&
           static_cast<unsigned char>(bytes[1]) == 0x8B;
}

QByteArray gunzip(const QByteArray& gzip, const QString& name) {
    // The header (RFC 1952): magic, method (8: deflate), flags, time, extra flags, system.
    if (!isGzip(gzip) || gzip.size() < 18 || static_cast<unsigned char>(gzip[2]) != 8) {
        damaged(name, QStringLiteral("it isn't compressed as a Live Set is"));
    }
    const auto flags = static_cast<unsigned char>(gzip[3]);
    qsizetype at = 10;
    if (flags & 0x04) {  // FEXTRA
        if (at + 2 > gzip.size()) damaged(name, QStringLiteral("it ends early"));
        at += 2 + (static_cast<unsigned char>(gzip[at]) | (static_cast<unsigned char>(gzip[at + 1]) << 8));
    }
    for (const int field : {0x08, 0x10}) {  // FNAME, FCOMMENT: zero-terminated
        if (!(flags & field)) continue;
        while (at < gzip.size() && gzip[at] != '\0') ++at;
        ++at;
    }
    if (flags & 0x02) at += 2;  // FHCRC
    if (at + 8 > gzip.size()) damaged(name, QStringLiteral("it ends early"));
    // Its length (modulo 4 GB: no Live Set is near that) is the last 4 bytes.
    // Taken on trust only as far as deflate can go (1032 bytes from one at
    // most): a cut file's last bytes are anything, and asking for gigabytes of
    // memory for them would fail where it should say the file is damaged.
    const uint32_t size = readLe32(gzip, gzip.size() - 4);
    if (static_cast<quint64>(size) > static_cast<quint64>(gzip.size() - at) * 1032 + 1024) {
        damaged(name, QStringLiteral("its compressed data is broken"));
    }
    QByteArray out;
    try {
        out = QByteArray(static_cast<qsizetype>(size), Qt::Uninitialized);
    } catch (const std::bad_alloc&) {
        throw ProjectFileError(QStringLiteral("%1 is too large to read (%2 MB)").arg(name).arg(size >> 20));
    }
    unsigned long outLength = size;
    unsigned long inLength = static_cast<unsigned long>(gzip.size() - at);
    const int result = puff(size > 0 ? reinterpret_cast<unsigned char*>(out.data()) : nullptr, &outLength,
                            reinterpret_cast<const unsigned char*>(gzip.constData() + at), &inLength);
    if (result != 0 || outLength != size) damaged(name, QStringLiteral("its compressed data is broken"));
    const qsizetype trailer = at + static_cast<qsizetype>(inLength);
    if (trailer + 8 > gzip.size() || readLe32(gzip, trailer + 4) != size || readLe32(gzip, trailer) != crc32(out)) {
        damaged(name, QStringLiteral("its data doesn't check out"));
    }
    return out;
}

std::unique_ptr<Element> parseLiveSet(const QByteArray& bytes, const QString& name) {
    const QByteArray xml = isGzip(bytes) ? gunzip(bytes, name) : bytes;
    QXmlStreamReader reader(xml);
    // Element and attribute names, shared: looked up by the reader's view of
    // them, each kept once (its key a view of the kept string).
    QHash<QStringView, QString> names;
    const auto shared = [&names](QStringView view) {
        auto it = names.constFind(view);
        if (it == names.constEnd()) {
            const QString kept = view.toString();
            it = names.insert(QStringView(kept), kept);
        }
        return *it;
    };
    const QSet<QString>& skipped = skippedElements();
    std::unique_ptr<Element> root;
    std::vector<Element*> open;
    int skipping = 0;  // depth inside an element left out
    while (!reader.atEnd()) {
        switch (reader.readNext()) {
        case QXmlStreamReader::StartElement: {
            if (skipping > 0) {
                ++skipping;
                break;
            }
            const QString tag = shared(reader.name());
            if (!open.empty() && skipped.contains(tag)) {
                skipping = 1;
                break;
            }
            auto element = std::make_unique<Element>();
            element->tag = tag;
            const QXmlStreamAttributes attributes = reader.attributes();
            element->attributes.reserve(static_cast<size_t>(attributes.size()));
            for (const QXmlStreamAttribute& a : attributes) {
                element->attributes.emplace_back(shared(a.name()), a.value().toString());
            }
            Element* raw = element.get();
            if (open.empty()) {
                if (root) damaged(name, QStringLiteral("it has more than one root"));
                root = std::move(element);
            } else {
                open.back()->children.push_back(std::move(element));
            }
            open.push_back(raw);
            break;
        }
        case QXmlStreamReader::EndElement:
            if (skipping > 0) {
                --skipping;
            } else if (!open.empty()) {
                open.pop_back();
            }
            break;
        case QXmlStreamReader::Characters:
            if (skipping == 0 && !open.empty() && !reader.isWhitespace()) {
                open.back()->text += reader.text().trimmed();
            }
            break;
        default: break;
        }
    }
    if (reader.hasError()) {
        damaged(name, QStringLiteral("%1 (line %2)").arg(reader.errorString()).arg(reader.lineNumber()));
    }
    if (!root || root->tag != u"Ableton" || !root->child(u"LiveSet")) {
        throw ProjectFileError(QStringLiteral("%1 is not an Ableton Live Set").arg(name));
    }
    return root;
}

std::unique_ptr<Element> readLiveSet(const QString& path) {
    QFile file(path);
    const QString name = QFileInfo(path).fileName();
    if (!file.open(QIODevice::ReadOnly)) {
        throw ProjectFileError(QStringLiteral("Could not read %1: %2").arg(name, file.errorString()));
    }
    return parseLiveSet(file.readAll(), name);
}

}  // namespace sub::app::live
