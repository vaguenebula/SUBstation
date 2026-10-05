#pragma once
// The list of built-in devices. A device registers itself where it is defined:
// each .cpp in builtin/devices/ ends with
//
//     SUB_REGISTER_BUILTIN(MyProcessor, AudioEffect);
//
// and that's all: the engine creates it by id, and the UI's device list, names,
// categories and parameter defaults all come from here (see builtin_devices() in
// the bindings). A device's id is its typeId() without the "builtin:" prefix.
//
// The engine is a static library, and a linker takes only the files of one that
// something refers to: nothing refers to a device's file but its own registrar,
// so a program linking the engine would have no built-in devices. So the macro
// also defines an empty function named after the file (SUB_BUILTIN_ANCHOR, which
// engine/CMakeLists.txt sets for each file in builtin/devices/), and
// BuiltinRegistry::instance() calls every one of them (BuiltinDevices.cpp, which
// CMake writes from the same list).

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Processor.h"

namespace sub {

enum class BuiltinCategory { Instrument, AudioEffect };

// What the UI needs to know about a device without instantiating one per use.
struct BuiltinInfo {
    std::string id;    // "utility"
    std::string name;  // "Utility"
    BuiltinCategory category;
    std::vector<ParamInfo> params;

    bool isInstrument() const noexcept { return category == BuiltinCategory::Instrument; }
};

class BuiltinRegistry {
public:
    using Factory = std::function<std::shared_ptr<Processor>()>;

    static BuiltinRegistry& instance();

    void add(BuiltinCategory category, Factory factory);

    // A new device of this id. Throws std::invalid_argument for an unknown one.
    std::shared_ptr<Processor> create(const std::string& id) const;

    // Every device, instruments first, then by name.
    const std::vector<BuiltinInfo>& devices() const;

private:
    struct Entry {
        BuiltinCategory category;
        Factory factory;
    };
    std::vector<Entry> entries_;
    mutable std::once_flag described_;
    mutable std::vector<BuiltinInfo> infos_;
    mutable std::vector<size_t> entryOf_;  // infos_[i] was made by entries_[entryOf_[i]]
};

struct BuiltinRegistrar {
    BuiltinRegistrar(BuiltinCategory category, BuiltinRegistry::Factory factory) {
        BuiltinRegistry::instance().add(category, std::move(factory));
    }
};

}  // namespace sub

#ifdef SUB_BUILTIN_ANCHOR
#define SUB_BUILTIN_ANCHOR_DEFINITION \
    void SUB_BUILTIN_ANCHOR() {}
#else
#define SUB_BUILTIN_ANCHOR_DEFINITION
#endif

#define SUB_REGISTER_BUILTIN(Class, category)                                                  \
    SUB_BUILTIN_ANCHOR_DEFINITION                                                               \
    static const ::sub::BuiltinRegistrar subBuiltin##Class {                                    \
        ::sub::BuiltinCategory::category, [] { return std::make_shared<Class>(); }              \
    }
