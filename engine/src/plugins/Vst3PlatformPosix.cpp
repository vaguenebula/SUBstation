// VST3 hosting elsewhere than Windows (see Vst3Platform.h): Linux, and macOS's
// folders and bundles.

#include "plugins/Vst3Platform.h"

#include <cstdlib>

#include "platform/Paths.h"

namespace sub::vst3 {

namespace {

// "Name.vst3" without its extension.
std::string stem(const std::string& bundleName) {
    return platform::fromPath(platform::toPath(bundleName).stem());
}

}  // namespace

void prepareThreadForModules() {}

std::vector<std::string> systemPluginFolders() {
    // The user's, then the system's.
    std::vector<std::string> paths;
    const char* home = std::getenv("HOME");
#ifdef __APPLE__
    if (home && *home) paths.push_back(platform::fromPath(platform::toPath(home) / "Library/Audio/Plug-Ins/VST3"));
    paths.push_back("/Library/Audio/Plug-Ins/VST3");
#else
    if (home && *home) paths.push_back(platform::fromPath(platform::toPath(home) / ".vst3"));
    paths.push_back("/usr/lib/vst3");
    paths.push_back("/usr/local/lib/vst3");
#endif
    return paths;
}

std::string binaryInBundle(const std::string& bundleName) {
#if defined(__APPLE__)
    return "Contents/MacOS/" + stem(bundleName);
#elif defined(__aarch64__)
    return "Contents/aarch64-linux/" + stem(bundleName) + ".so";
#else
    return "Contents/x86_64-linux/" + stem(bundleName) + ".so";
#endif
}

}  // namespace sub::vst3
