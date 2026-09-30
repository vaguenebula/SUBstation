#pragma once
// Plug-in formats: scanning files for plug-ins and instantiating them as
// `Processor`s, which the renderer hosts in each track's insert chain. VST3 is
// implemented (Vst3Format); CLAP would be another implementation.
//
// Hosting notes:
//  * Scanning loads plug-in code, so the UI runs it in a child process: a
//    crashing plug-in cannot take the DAW down (gilstudio/plugins/scanner.py).
//  * Plug-ins are created, configured and destroyed on the main thread.
//  * Main-thread work plug-ins ask for (restarts, parameter updates, editor
//    events) is done in Engine::idle(), which the UI calls on a timer.

#include <memory>
#include <string>
#include <vector>

#include "Processor.h"

namespace gil {

struct PluginDescription {
    std::string format;     // "VST3"
    std::string path;       // bundle or file on disk
    std::string uid;        // VST3 class id (32 hex digits)
    std::string name;
    std::string vendor;
    std::string version;
    std::string category;   // VST3 sub-categories, e.g. "Instrument|Synth" or "Fx|Delay"
    bool isInstrument = false;
};

class PluginFormat {
public:
    virtual ~PluginFormat() = default;

    virtual std::string name() const = 0;
    virtual std::vector<std::string> defaultSearchPaths() const = 0;

    // Lists the plug-ins in one file or bundle. Loads its code. Throws on failure.
    virtual std::vector<PluginDescription> scanFile(const std::string& path) = 0;

    // Creates a prepared processor, ready to be inserted into a track. Main
    // thread only. Throws std::runtime_error with a message for the user.
    virtual std::shared_ptr<Processor> instantiate(const std::string& path, const std::string& uid,
                                                   double sampleRate, int maxBlockSize) = 0;
};

}  // namespace gil
