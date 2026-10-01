#pragma once

// Access to SteamVR's private IVRCameraPassthroughInternal_001 interface,
// limited to reading and writing the 5-byte camera config. Requires an
// initialised OpenVR client (VR_Init) in the calling process.
//
// The interface has no public header. Its layout was mapped from the
// headset's own vrclient.so (FINDINGS.md sections 9 and 11). Before any
// call, the code of the two methods is compared with the instructions
// seen in that binary; if SteamVR has changed them, nothing is called.

#include "passthrough_state.hpp"

#include <optional>
#include <string>

namespace autopass {

class PrivateCamera {
public:
    // Looks up the interface and verifies the method fingerprints. On
    // failure, error() says why and every other call returns failure.
    PrivateCamera();

    bool ok() const { return methods_ != nullptr; }
    const std::string& error() const { return error_; }

    std::optional<CameraConfig> get();
    // Writes the config and reads it back. Returns false (with error())
    // on any mismatch.
    bool set(const CameraConfig& config);

private:
    void* instance_ = nullptr;
    void** methods_ = nullptr;
    std::string error_;
};

} // namespace autopass
