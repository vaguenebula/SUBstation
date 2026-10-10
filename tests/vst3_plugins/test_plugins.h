#pragma once
// What the test plug-ins' two translation units share. The single-component
// effect has one of its own: the SDK's SingleComponentEffect renames
// IEditController::setState with a macro, which only works if its header comes
// first (vstsinglecomponenteffect.h).

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

namespace sub_test {

inline const Steinberg::FUID kSynthProcessorUID(0x6A1C0D5E, 0x3B7F4E21, 0x9C8D2A10, 0x5E4F7B01);
inline const Steinberg::FUID kSynthControllerUID(0x6A1C0D5E, 0x3B7F4E21, 0x9C8D2A10, 0x5E4F7B02);
inline const Steinberg::FUID kEffectUID(0x6A1C0D5E, 0x3B7F4E21, 0x9C8D2A10, 0x5E4F7B03);
inline const Steinberg::FUID kMonoUID(0x6A1C0D5E, 0x3B7F4E21, 0x9C8D2A10, 0x5E4F7B04);
inline const Steinberg::FUID kSidechainUID(0x6A1C0D5E, 0x3B7F4E21, 0x9C8D2A10, 0x5E4F7B05);
inline const Steinberg::FUID kNoteEffectUID(0x6A1C0D5E, 0x3B7F4E21, 0x9C8D2A10, 0x5E4F7B06);

// The last value a parameter queue holds in this block, if any.
inline bool lastValue(Steinberg::Vst::IParamValueQueue* queue, Steinberg::Vst::ParamValue& value) {
    const Steinberg::int32 points = queue->getPointCount();
    Steinberg::int32 offset = 0;
    return points > 0 && queue->getPoint(points - 1, offset, value) == Steinberg::kResultTrue;
}

// SUB Test Effect (test_effect.cpp).
Steinberg::FUnknown* createTestEffect(void*);
extern const wchar_t* const kEffectViewClass;

// SUB Test Sidechain (test_sidechain.cpp).
Steinberg::FUnknown* createTestSidechain(void*);

}  // namespace sub_test
