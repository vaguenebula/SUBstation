#pragma once
// VST3 class ids as other programs write them, and as SUBstation's plug-in
// descriptions have them (PluginDescription::uid: the TUID's 16 bytes in hex,
// VST3::UID::toString(false)). A class id is four 32-bit words (FUID's
// getLong1() to getLong4(): what Ableton Live sets store, and what a .vstpreset
// file spells out as 32 hex digits); on Windows the TUID's bytes are those of a
// COM GUID made of them, elsewhere the words' bytes in order, so its hex differs
// between platforms. No SDK types here: the application layer uses it.

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace sub::vst3 {

// The class id PluginDescription::uid has for these four words.
std::string classIdFromWords(uint32_t l1, uint32_t l2, uint32_t l3, uint32_t l4);
// The words as a .vstpreset's header spells them: "%08X%08X%08X%08X".
std::string presetClassId(uint32_t l1, uint32_t l2, uint32_t l3, uint32_t l4);
// The four words of a class id as PluginDescription::uid has it; none if it isn't one.
std::optional<std::array<uint32_t, 4>> wordsFromClassId(const std::string& uid);

}  // namespace sub::vst3
