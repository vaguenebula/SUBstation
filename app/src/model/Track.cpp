#include "model/Track.h"

#include <cmath>
#include <stdexcept>

namespace sub::app {

namespace {

struct FieldName {
    TrackField field;
    const char* name;
};

constexpr FieldName kFieldNames[] = {
    {TrackField::Name, "name"},          {TrackField::Color, "color"},
    {TrackField::VolumeDb, "volume_db"}, {TrackField::Pan, "pan"},
    {TrackField::Mute, "mute"},          {TrackField::Solo, "solo"},
    {TrackField::Height, "height"},      {TrackField::Input, "input"},
    {TrackField::InputTrack, "input_track"}, {TrackField::MidiInput, "midi_input"},
    {TrackField::Monitor, "monitor"},    {TrackField::Armed, "armed"},
    {TrackField::Folded, "folded"},      {TrackField::Sends, "sends"},
    {TrackField::InputTap, "input_tap"}, {TrackField::Output, "output"},
};

double asDouble(const TrackValue& value) {
    if (const auto* d = std::get_if<double>(&value)) return *d;
    if (const auto* i = std::get_if<int>(&value)) return *i;
    if (const auto* b = std::get_if<bool>(&value)) return *b ? 1.0 : 0.0;
    throw std::bad_variant_access();
}

int asInt(const TrackValue& value) {
    if (const auto* i = std::get_if<int>(&value)) return *i;
    if (const auto* d = std::get_if<double>(&value)) return static_cast<int>(std::lround(*d));
    throw std::bad_variant_access();
}

}  // namespace

QString trackFieldName(TrackField field) {
    for (const FieldName& entry : kFieldNames) {
        if (entry.field == field) return QString::fromLatin1(entry.name);
    }
    return {};
}

std::optional<TrackField> trackFieldFromName(const QString& name) {
    for (const FieldName& entry : kFieldNames) {
        if (name == QLatin1String(entry.name)) return entry.field;
    }
    return std::nullopt;
}

Clip Freeze::clip(const QString& trackId, const QString& name) const {
    Clip played = segment(QStringLiteral("frozen-") + trackId, 0.0, 0.0, durationSec);
    played.name = name;
    return played;
}

Clip Freeze::segment(const QString& id, double startBeat, double offsetSec, double length) const {
    Clip played = Clip::audio(id, path, QString(), startBeat, length, offsetSec, durationSec);
    played.warp = true;
    played.warpMode = kDefaultWarpMode;
    played.segmentBpm = tempo;
    return played;
}

std::vector<Clip> Freeze::playing(const QString& trackId) const {
    if (segments) return *segments;
    return {clip(trackId, QString())};
}

bool Track::hasInput() const {
    if (!hasClips()) return false;
    return isMidi() ? midiInput.has_value() : (!input.empty() || inputTrack.has_value());
}

TrackValue Track::value(TrackField field) const {
    switch (field) {
    case TrackField::Name: return nameSource();
    case TrackField::Color: return color;
    case TrackField::VolumeDb: return volumeDb;
    case TrackField::Pan: return pan;
    case TrackField::Mute: return mute;
    case TrackField::Solo: return solo;
    case TrackField::Height: return height;
    case TrackField::Input: return input;
    case TrackField::InputTrack: return inputTrack;
    case TrackField::InputTap: return inputTap;
    case TrackField::MidiInput: return midiInput;
    case TrackField::Monitor: return monitor;
    case TrackField::Armed: return armed;
    case TrackField::Folded: return folded;
    case TrackField::Sends: return sends;
    case TrackField::Output: return output;
    }
    return {};
}

void Track::setValue(TrackField field, const TrackValue& value) {
    switch (field) {
    case TrackField::Name: name = nameTemplate = std::get<QString>(value); break;  // (numbered by Project)
    case TrackField::Color: color = std::get<QString>(value); break;
    case TrackField::VolumeDb: volumeDb = asDouble(value); break;
    case TrackField::Pan: pan = asDouble(value); break;
    case TrackField::Mute: mute = std::get<bool>(value); break;
    case TrackField::Solo: solo = std::get<bool>(value); break;
    case TrackField::Height: height = asInt(value); break;
    case TrackField::Input: input = std::get<std::vector<int>>(value); break;
    case TrackField::InputTrack: inputTrack = std::get<std::optional<QString>>(value); break;
    case TrackField::InputTap: inputTap = std::get<QString>(value); break;
    case TrackField::MidiInput: midiInput = std::get<std::optional<MidiInput>>(value); break;
    case TrackField::Monitor: monitor = std::get<QString>(value); break;
    case TrackField::Armed: armed = std::get<bool>(value); break;
    case TrackField::Folded: folded = std::get<bool>(value); break;
    case TrackField::Sends: sends = std::get<SendMap>(value); break;
    case TrackField::Output: output = std::get<Output>(value); break;
    }
}

Track newMaster() {
    Track master;
    master.id = kMaster;
    master.name = QStringLiteral("Master");
    master.color = kMasterColor;
    master.kind = kMasterKind;
    return master;
}

QString returnLetter(int index) {
    QString letters;
    index += 1;
    while (index > 0) {
        const int rest = (index - 1) % 26;
        index = (index - 1) / 26;
        letters.prepend(QChar(static_cast<char16_t>(u'A' + rest)));
    }
    return letters;
}

}  // namespace sub::app
