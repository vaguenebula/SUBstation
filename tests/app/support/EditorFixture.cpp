#include "EditorFixture.h"

namespace sub::app::test {

Envelope env(std::initializer_list<std::pair<double, double>> points) {
    Envelope envelope;
    for (const auto& [beat, value] : points) envelope.push_back(AutomationPoint{beat, value, 0.0});
    return envelope;
}

Clip audioClip(const QString& id, double start, double durationSec, const QString& path) {
    return Clip::audio(id, path, id, start, durationSec, 0.0, durationSec);
}

}  // namespace sub::app::test
