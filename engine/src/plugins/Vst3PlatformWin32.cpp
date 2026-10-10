// VST3 hosting on Windows (see Vst3Platform.h).

#include "plugins/Vst3Platform.h"

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include "platform/Paths.h"

namespace sub::vst3 {

namespace {

std::string knownFolder(REFKNOWNFOLDERID id) {
    PWSTR folder = nullptr;
    std::string result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &folder))) result = platform::fromNative(folder);
    CoTaskMemFree(folder);
    return result;
}

}  // namespace

void prepareThreadForModules() {
    thread_local bool initialized = false;
    if (!initialized) {
        OleInitialize(nullptr);
        initialized = true;
    }
}

std::vector<std::string> systemPluginFolders() {
    // C:\Program Files\Common Files\VST3, then %LOCALAPPDATA%\Programs\Common\VST3.
    std::vector<std::string> paths;
    for (const auto& folder : {knownFolder(FOLDERID_ProgramFilesCommon), knownFolder(FOLDERID_UserProgramFilesCommon)}) {
        if (!folder.empty()) paths.push_back(platform::fromPath(platform::toPath(folder) / "VST3"));
    }
    return paths;
}

std::string binaryInBundle(const std::string& bundleName) { return "Contents/x86_64-win/" + bundleName; }

}  // namespace sub::vst3
