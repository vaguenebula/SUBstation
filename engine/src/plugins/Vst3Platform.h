#pragma once
// What VST3 hosting does differently on each system, one file per system:
// Vst3PlatformWin32.cpp, Vst3PlatformPosix.cpp (Linux, and macOS's folders and
// bundles). The rest of the hosting is the same everywhere.

#include <string>
#include <vector>

namespace sub::vst3 {

// Before loading a module on this thread: COM on Windows (some plug-ins need
// it; Qt has set it up on the UI thread already, the scanner process has not).
void prepareThreadForModules();

// The system's VST3 folders, the user's and the system's (VST 3's "plug-in
// locations").
std::vector<std::string> systemPluginFolders();

// Where a bundle's code is, inside the bundle, on this system (VST 3's bundle
// format): "Contents/x86_64-win/<name>.vst3" on Windows,
// "Contents/<arch>-linux/<name>.so" on Linux, "Contents/MacOS/<name>" on macOS.
// `bundleName` is the bundle's file name ("Name.vst3").
std::string binaryInBundle(const std::string& bundleName);

}  // namespace sub::vst3
