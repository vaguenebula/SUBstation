#pragma once
// Extension point for third-party plugin formats (VST3 via the MIT-licensed
// VST 3.8 SDK, CLAP via its MIT headers). Nothing implements this yet; a format
// adapter only has to produce `Processor` instances, which the renderer already
// hosts in each track's insert chain.
//
// Hosting notes for the implementation:
//  * Scanning runs out-of-process so a crashing plugin cannot take the DAW down.
//  * Editors attach to an HWND supplied by the Qt UI (QWidget::winId()).
//  * Main-thread callbacks (CLAP request_callback, timers) are pumped from
//    Engine::idle(), which the UI already calls on a timer.

#include <memory>
#include <string>
#include <vector>

#include "Processor.h"

namespace gil {

struct PluginDescription {
    std::string format;     // "VST3" or "CLAP"
    std::string path;       // bundle / file on disk
    std::string uid;        // VST3 class id or CLAP plugin id
    std::string name;
    std::string vendor;
    std::string category;
    bool isInstrument = false;
};

class PluginFormat {
public:
    virtual ~PluginFormat() = default;

    virtual std::string name() const = 0;
    virtual std::vector<std::string> defaultSearchPaths() const = 0;

    // Lists the plugins contained in one file or bundle.
    virtual std::vector<PluginDescription> scanFile(const std::string& path) = 0;

    // Creates a prepared processor ready to be inserted into a track.
    virtual std::shared_ptr<Processor> instantiate(const PluginDescription& description, double sampleRate,
                                                   int maxBlockSize) = 0;
};

}  // namespace gil
