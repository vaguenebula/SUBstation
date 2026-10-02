#pragma once
// VST3 hosting: the host context plug-ins see ("SUBstation"), loaded modules
// (shared by all instances from the same file and unloaded with the last one),
// scanning and instantiation.

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "plugins/PluginFormat.h"
#include "public.sdk/source/vst/hosting/module.h"

namespace sub::vst3 {

class Vst3Format final : public PluginFormat {
public:
    static Vst3Format& instance();

    std::string name() const override { return "VST3"; }
    std::vector<std::string> defaultSearchPaths() const override;
    std::vector<PluginDescription> scanFile(const std::string& path) override;
    std::shared_ptr<Processor> instantiate(const std::string& path, const std::string& uid, double sampleRate,
                                           int maxBlockSize) override;

private:
    Vst3Format() = default;
    VST3::Hosting::Module::Ptr loadModule(const std::string& path);

    std::mutex mutex_;
    std::map<std::string, std::weak_ptr<VST3::Hosting::Module>> modules_;  // by normalised path
};

}  // namespace sub::vst3
