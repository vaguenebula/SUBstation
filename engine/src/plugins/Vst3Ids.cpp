#include "plugins/Vst3Ids.h"

#include "pluginterfaces/base/funknown.h"
#include "public.sdk/source/vst/utility/uid.h"

namespace sub::vst3 {

std::string classIdFromWords(uint32_t l1, uint32_t l2, uint32_t l3, uint32_t l4) {
    const Steinberg::FUID fuid(l1, l2, l3, l4);
    Steinberg::TUID tuid;
    fuid.toTUID(tuid);
    return VST3::UID::fromTUID(tuid).toString(false);
}

std::string presetClassId(uint32_t l1, uint32_t l2, uint32_t l3, uint32_t l4) {
    Steinberg::char8 text[33] = {};
    Steinberg::FUID(l1, l2, l3, l4).toString(text);
    return text;
}

std::optional<std::array<uint32_t, 4>> wordsFromClassId(const std::string& uid) {
    const auto parsed = VST3::UID::fromString(uid, false);
    if (!parsed) return std::nullopt;
    const Steinberg::FUID fuid = Steinberg::FUID::fromTUID(parsed->data());
    return std::array<uint32_t, 4>{fuid.getLong1(), fuid.getLong2(), fuid.getLong3(), fuid.getLong4()};
}

}  // namespace sub::vst3
