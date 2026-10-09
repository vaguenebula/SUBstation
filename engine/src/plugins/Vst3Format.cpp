#include "plugins/Vst3Format.h"

#include <algorithm>
#include <stdexcept>

#include "platform/Paths.h"
#include "plugins/Vst3Platform.h"
#include "plugins/Vst3Processor.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

namespace sub::vst3 {
namespace {

// What plug-ins see of us (IHostApplication, and the interfaces we support).
class SubHostApplication final : public Steinberg::Vst::HostApplication {
public:
    Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override {
        return Steinberg::Vst::StringConvert::convert("SUBstation", name) ? Steinberg::kResultTrue
                                                                           : Steinberg::kInternalError;
    }
};

Steinberg::FUnknown* hostContext() {
    static auto* host = new SubHostApplication;  // never freed: plug-ins may keep it until they unload
    return host;
}

bool isInstrument(const VST3::Hosting::ClassInfo& info) {
    const auto& categories = info.subCategories();
    return std::find(categories.begin(), categories.end(), Steinberg::Vst::PlugType::kInstrument) != categories.end();
}

}  // namespace

Vst3Format& Vst3Format::instance() {
    static Vst3Format format;
    return format;
}

std::vector<std::string> Vst3Format::defaultSearchPaths() const { return systemPluginFolders(); }

std::string Vst3Format::binaryInBundle(const std::string& bundleName) { return vst3::binaryInBundle(bundleName); }

VST3::Hosting::Module::Ptr Vst3Format::loadModule(const std::string& path) {
    std::lock_guard lock(mutex_);
    auto& slot = modules_[platform::fileKey(path)];  // one module per file
    if (auto module = slot.lock()) return module;
    std::string error;
    auto module = VST3::Hosting::Module::create(path, error);
    if (!module) throw std::runtime_error(error.empty() ? "Could not load " + path : error);
    module->getFactory().setHostContext(hostContext());
    slot = module;
    return module;
}

std::vector<PluginDescription> Vst3Format::scanFile(const std::string& path) {
    prepareThreadForModules();
    const auto module = loadModule(path);
    const auto& factory = module->getFactory();
    const std::string factoryVendor = factory.info().vendor();
    std::vector<PluginDescription> found;
    for (const auto& info : factory.classInfos()) {
        if (info.category() != kVstAudioEffectClass) continue;  // controllers and other helper classes
        PluginDescription description;
        description.format = "VST3";
        description.path = path;
        description.uid = info.ID().toString(false);
        description.name = info.name();
        description.vendor = info.vendor().empty() ? factoryVendor : info.vendor();
        description.version = info.version();
        description.category = info.subCategoriesString();
        description.isInstrument = isInstrument(info);
        found.push_back(std::move(description));
    }
    return found;
}

std::shared_ptr<Processor> Vst3Format::instantiate(const std::string& path, const std::string& uid, double sampleRate,
                                                   int maxBlockSize) {
    prepareThreadForModules();
    const auto id = VST3::UID::fromString(uid, false);
    if (!id) throw std::invalid_argument("Not a VST3 class id: " + uid);
    const auto module = loadModule(path);
    for (const auto& info : module->getFactory().classInfos()) {
        if (info.ID() == *id && info.category() == kVstAudioEffectClass) {
            auto processor = std::make_shared<Vst3Processor>(module, info, hostContext());
            processor->prepare(sampleRate, maxBlockSize);
            return processor;
        }
    }
    throw std::runtime_error(platform::fromPath(platform::toPath(path).filename()) + " does not contain this plug-in any more.");
}

}  // namespace sub::vst3
