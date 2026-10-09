// The audio driver types elsewhere than Windows (see AudioBackends.h):
// miniaudio's choice of the system's backends, as "System".

#include "backends/AudioBackends.h"

#include "backends/MiniaudioBackend.h"

namespace sub {

std::vector<std::string> audioDriverNames() { return {MiniaudioBackend::kName}; }

std::vector<std::unique_ptr<AudioBackend>> makeAudioBackends() {
    std::vector<std::unique_ptr<AudioBackend>> backends;
    backends.push_back(std::make_unique<MiniaudioBackend>());
    return backends;
}

}  // namespace sub
