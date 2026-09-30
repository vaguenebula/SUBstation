#include "plugins/Vst3Format.h"

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <stdexcept>

#include "PathUtils.h"
#include "plugins/Vst3Processor.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

namespace gil::vst3 {
namespace {

// What plug-ins see of us (IHostApplication, and the interfaces we support).
class GilHostApplication final : public Steinberg::Vst::HostApplication {
public:
    Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override {
        return Steinberg::Vst::StringConvert::convert("GIL Studio", name) ? Steinberg::kResultTrue
                                                                           : Steinberg::kInternalError;
    }
};

Steinberg::FUnknown* hostContext() {
    static auto* host = new GilHostApplication;  // never freed: plug-ins may keep it until they unload
    return host;
}

// Some plug-ins need COM on the thread that loads them. Qt has set it up on the
// UI thread already; the scanner process has not.
void ensureComInitialized() {
    thread_local bool initialized = false;
    if (!initialized) {
        OleInitialize(nullptr);
        initialized = true;
    }
}

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::string moduleKey(const std::string& path) {
    std::wstring wide = pathFromUtf8(path).lexically_normal().make_preferred().wstring();
    std::transform(wide.begin(), wide.end(), wide.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return utf8(std::filesystem::path(wide));
}

std::string knownFolder(REFKNOWNFOLDERID id) {
    PWSTR folder = nullptr;
    std::string result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &folder))) result = utf8(std::filesystem::path(folder));
    CoTaskMemFree(folder);
    return result;
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

std::vector<std::string> Vst3Format::defaultSearchPaths() const {
    std::vector<std::string> paths;
    for (const auto& folder : {knownFolder(FOLDERID_ProgramFilesCommon), knownFolder(FOLDERID_UserProgramFilesCommon)}) {
        if (!folder.empty()) paths.push_back(utf8(pathFromUtf8(folder) / "VST3"));
    }
    return paths;
}

VST3::Hosting::Module::Ptr Vst3Format::loadModule(const std::string& path) {
    std::lock_guard lock(mutex_);
    auto& slot = modules_[moduleKey(path)];
    if (auto module = slot.lock()) return module;
    std::string error;
    auto module = VST3::Hosting::Module::create(path, error);
    if (!module) throw std::runtime_error(error.empty() ? "Could not load " + path : error);
    module->getFactory().setHostContext(hostContext());
    slot = module;
    return module;
}

std::vector<PluginDescription> Vst3Format::scanFile(const std::string& path) {
    ensureComInitialized();
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
    ensureComInitialized();
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
    throw std::runtime_error(utf8(pathFromUtf8(path).filename()) + " does not contain this plug-in any more.");
}

}  // namespace gil::vst3
