#include "private_camera.hpp"

#include <openvr.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace autopass {
namespace {

constexpr const char* kInterface = "IVRCameraPassthroughInternal_001";
constexpr std::size_t kGetConfig = 9;
constexpr std::size_t kSetConfig = 10;

// First instructions of CVRCameraPassthroughInternal's GetConfig and
// SetConfig in SteamVR 2.17.10 (vrclient.so 0x1a49d8 and 0x1a4bf8). Only
// position-independent words are included: the prologue, argument moves,
// the `add x1, x0, #0x18` that reaches the shared-state accessor, and a
// short relative branch. A call instruction would change whenever the
// library is relinked, so the fingerprints stop before the first `bl`.
constexpr std::array<std::uint32_t, 6> kGetFingerprint = {
    0xa9bc7bfd,  // stp x29, x30, [sp, #-0x40]!
    0x910003fd,  // mov x29, sp
    0xa90153f3,  // stp x19, x20, [sp, #0x10]
    0xaa0103f3,  // mov x19, x1          (cfg)
    0x91006001,  // add x1, x0, #0x18    (shared state)
    0xb40002b3,  // cbz x19, ...         (null cfg returns only the flag)
};
constexpr std::array<std::uint32_t, 7> kSetFingerprint = {
    0xa9ba7bfd,  // stp x29, x30, [sp, #-0x60]!
    0x910003fd,  // mov x29, sp
    0xa90153f3,  // stp x19, x20, [sp, #0x10]
    0xaa0103f3,  // mov x19, x1          (cfg)
    0x91006001,  // add x1, x0, #0x18    (shared state)
    0x910083f4,  // add x20, sp, #0x20   (lock guard)
    0xaa1403e0,  // mov x0, x20
};

template <std::size_t N>
bool matches(const void* fn, const std::array<std::uint32_t, N>& expected) {
    std::uint32_t words[N];
    std::memcpy(words, fn, sizeof words);
    return std::memcmp(words, expected.data(), sizeof words) == 0;
}

using GetConfigFn = bool (*)(void* self, std::uint8_t* cfg);
using SetConfigFn = void (*)(void* self, const std::uint8_t* cfg);

} // namespace

PrivateCamera::PrivateCamera() {
    vr::EVRInitError err = vr::VRInitError_None;
    void* instance = vr::VR_GetGenericInterface(kInterface, &err);
    if (!instance || err != vr::VRInitError_None) {
        error_ = std::string("interface unavailable: ") + vr::VR_GetVRInitErrorAsEnglishDescription(err);
        return;
    }
    auto** methods = *static_cast<void***>(instance);
    if (!methods || !methods[kGetConfig] || !methods[kSetConfig]) {
        error_ = "interface has no config methods";
        return;
    }
    if (!matches(methods[kGetConfig], kGetFingerprint) || !matches(methods[kSetConfig], kSetFingerprint)) {
        error_ = "SteamVR's camera interface does not match the verified build (2.17.10); refusing to call it";
        return;
    }
    instance_ = instance;
    methods_ = methods;
}

std::optional<CameraConfig> PrivateCamera::get() {
    if (!ok()) return std::nullopt;
    std::array<std::uint8_t, 5> raw;
    raw.fill(0xff);
    reinterpret_cast<GetConfigFn>(methods_[kGetConfig])(instance_, raw.data());
    const auto config = CameraConfig::from(raw.data());
    if (!config.valid()) {
        error_ = "config read back non-boolean bytes: " + config.describe();
        return std::nullopt;
    }
    return config;
}

bool PrivateCamera::set(const CameraConfig& config) {
    if (!ok()) return false;
    if (!config.valid()) {
        error_ = "refusing to write non-boolean config";
        return false;
    }
    const auto raw = config.bytes();
    reinterpret_cast<SetConfigFn>(methods_[kSetConfig])(instance_, raw.data());
    const auto back = get();
    if (!back || back->bytes() != raw) {
        error_ = "config did not read back as written";
        return false;
    }
    return true;
}

} // namespace autopass
