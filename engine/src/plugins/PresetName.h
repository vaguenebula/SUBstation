#pragma once
// Which preset (patch, instrument) a plug-in has loaded.
//
// VST3 has no call for it. A plug-in may publish a program list (IUnitInfo) and
// a program-change parameter, which Vst3Processor reads first; but many give
// placeholders there ("Prog 12") or nothing, and keep the name in their saved
// state instead, in a text header (Serum 2: JSON; Valhalla, Spitfire, Nuro
// Audio: XML). These functions find it there. They take bytes and nothing else,
// so they can be tested without a plug-in. (docs/engine/plugins.md#preset-names)

#include <cstddef>
#include <cstdint>
#include <string>

namespace sub::vst3 {

// The preset name in a plug-in's saved state, its component's or its
// controller's ("" if none is recognised): the first of the known keys found in
// the text near the start of `data` (JSON "presetName":"x", XML presetName="x",
// Spitfire's <META name="x">, <PRESET name="x">).
std::string presetNameFromState(const uint8_t* data, size_t size);

// The same in a .vstpreset (Vst3Processor::getState(), a project's saved
// device): its component's state, then its controller's. Bytes that aren't a
// .vstpreset are scanned as they are.
std::string presetNameFromPreset(const uint8_t* data, size_t size);

// A program name that says nothing: empty, or a slot number ("Prog 12", "preset 3").
bool isPlaceholderPresetName(const std::string& name);

}  // namespace sub::vst3
