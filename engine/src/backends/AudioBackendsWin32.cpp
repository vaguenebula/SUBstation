// The audio driver types on Windows (see AudioBackends.h): WASAPI through
// miniaudio, and ASIO when the engine is built with the ASIO SDK.

#include "backends/AudioBackends.h"

#include "backends/MiniaudioBackend.h"
#if SUBSTATION_HAS_ASIO
#include "backends/AsioBackend.h"
#endif

namespace sub {

std::vector<std::string> audioDriverNames() {
#if SUBSTATION_HAS_ASIO
    return {MiniaudioBackend::kName, "ASIO"};
#else
    return {MiniaudioBackend::kName};
#endif
}

std::vector<std::unique_ptr<AudioBackend>> makeAudioBackends() {
#if SUBSTATION_HAS_ASIO
    // First: ASIO needs this thread in a single-threaded COM apartment, which
    // miniaudio would otherwise make multithreaded.
    auto asio = createAsioBackend();
#endif
    std::vector<std::unique_ptr<AudioBackend>> backends;
    backends.push_back(std::make_unique<MiniaudioBackend>());
#if SUBSTATION_HAS_ASIO
    backends.push_back(std::move(asio));
#endif
    return backends;
}

}  // namespace sub
