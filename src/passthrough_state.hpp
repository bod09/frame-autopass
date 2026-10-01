#pragma once

// Passive reader for SteamVR's VR_CameraPassthroughState shared memory
// (see FINDINGS.md sections 12 and 13). Reads a tmpfs file only: no
// SteamVR calls, no locks, no devices, so it cannot disturb passthrough.

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace autopass {

// The 5-byte passthrough camera config, as stored at state offset 2.
struct CameraConfig {
    std::uint8_t enabled = 0;
    std::uint8_t stereo = 0;
    std::uint8_t rgb = 0;
    std::uint8_t disable_devignetting = 0;
    std::uint8_t sharpening = 0;

    std::array<std::uint8_t, 5> bytes() const {
        return {enabled, stereo, rgb, disable_devignetting, sharpening};
    }
    static CameraConfig from(const std::uint8_t* p) { return {p[0], p[1], p[2], p[3], p[4]}; }
    bool valid() const;  // every byte is 0 or 1
    std::string describe() const;
};

struct ColourFrameInfo {
    double timestamp = 0;  // seconds, SteamVR clock
    float light = 0;       // "g4": ~1 bright, 0 when too dark for colour
};

struct PassthroughSnapshot {
    CameraConfig config;
    // Newest colour-stream frame, if any record is present.
    std::optional<ColourFrameInfo> colour;
    // Newest frame timestamp of each stream (0 if none). Only the stream
    // being displayed advances, so a timestamp that keeps moving means
    // that passthrough source is live (FINDINGS.md section 26).
    double mono_timestamp = 0;
    double colour_timestamp = 0;
    // Light value of the newest mono frame, at the record offset where the
    // colour stream keeps g4 (FINDINGS.md 41): 0 in the dark even with the
    // IR emitters on, about 0.2-0.35 in a lit room. Only advances while IR
    // is shown.
    std::optional<float> mono_light;
};

class PassthroughState {
public:
    static constexpr std::size_t kSize = 0x6200;

    // Finds the state file by its unique size under /dev/shm. Returns an
    // empty string when there is not exactly one candidate.
    static std::string locate();

    explicit PassthroughState(std::string path = locate());
    const std::string& path() const { return path_; }

    // Reads and parses one snapshot. Returns nullopt if the file cannot be
    // read or is not the expected size.
    std::optional<PassthroughSnapshot> read() const;

    // Exposed for tests: parse a raw buffer of kSize bytes.
    static PassthroughSnapshot parse(const std::uint8_t* buf);

private:
    std::string path_;
};

} // namespace autopass
