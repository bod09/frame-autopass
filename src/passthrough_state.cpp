#include "passthrough_state.hpp"

#include <cmath>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace autopass {
namespace {

constexpr std::size_t kConfigOffset = 2;
// Stream blocks: mono at 0x14, colour at 0x20e8. Inside each, per-camera
// blocks 0x1040 apart hold per-frame records 0x104 apart starting at +0x38;
// a record's frame timestamp (float64) is at +0x10.
constexpr std::size_t kMonoStream = 0x14;
constexpr std::size_t kFirstRecord = 0x38;
constexpr std::size_t kCameraStride = 0x1040;
constexpr std::size_t kRecordStride = 0x104;
constexpr int kCameras = 2;
constexpr int kRecordsPerCamera = 4;
// The colour stream block starts here; everything before is the mono one.
constexpr std::size_t kColourStream = 0x20e8;
// Per-frame records are found by a float32 1/2.2 gamma marker.
constexpr std::size_t kRecordBeforeMarker = 0xd8;
constexpr std::size_t kTimestampInRecord = 0x10;
constexpr std::size_t kLightAfterMarker = 0x14;
// The same light value, by fixed offset: mono records carry no gamma marker.
constexpr std::size_t kLightInRecord = kRecordBeforeMarker + kLightAfterMarker;

float read_f32(const std::uint8_t* p) {
    float v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

double read_f64(const std::uint8_t* p) {
    double v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

double newest_timestamp(const std::uint8_t* buf, std::size_t stream) {
    double newest = 0;
    for (int cam = 0; cam < kCameras; ++cam)
        for (int rec = 0; rec < kRecordsPerCamera; ++rec) {
            const std::size_t off = stream + kFirstRecord + cam * kCameraStride + rec * kRecordStride + kTimestampInRecord;
            const double ts = read_f64(buf + off);
            if (std::isfinite(ts) && ts > newest) newest = ts;
        }
    return newest;
}

} // namespace

bool CameraConfig::valid() const {
    for (auto b : bytes())
        if (b > 1) return false;
    return true;
}

std::string CameraConfig::describe() const {
    std::string s;
    s += "enabled=" + std::to_string(enabled);
    s += " stereo=" + std::to_string(stereo);
    s += " rgb=" + std::to_string(rgb);
    s += " disable_devignetting=" + std::to_string(disable_devignetting);
    s += " sharpening=" + std::to_string(sharpening);
    return s;
}

std::string PassthroughState::locate() {
    std::string found;
    int matches = 0;
    DIR* dir = ::opendir("/dev/shm");
    if (!dir) return {};
    while (auto* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.rfind("u", 0) != 0 || name.find("-Shm_") == std::string::npos) continue;
        const std::string path = "/dev/shm/" + name;
        struct stat st{};
        if (::stat(path.c_str(), &st) == 0 && static_cast<std::size_t>(st.st_size) == kSize) {
            found = path;
            ++matches;
        }
    }
    ::closedir(dir);
    return matches == 1 ? found : std::string();
}

PassthroughState::PassthroughState(std::string path) : path_(std::move(path)) {}

std::optional<PassthroughSnapshot> PassthroughState::read() const {
    if (path_.empty()) return std::nullopt;
    const int fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    std::vector<std::uint8_t> buf(kSize);
    std::size_t got = 0;
    while (got < kSize) {
        const auto n = ::pread(fd, buf.data() + got, kSize - got, static_cast<off_t>(got));
        if (n <= 0) break;
        got += static_cast<std::size_t>(n);
    }
    ::close(fd);
    if (got != kSize) return std::nullopt;
    return parse(buf.data());
}

PassthroughSnapshot PassthroughState::parse(const std::uint8_t* buf) {
    PassthroughSnapshot snap;
    snap.config = CameraConfig::from(buf + kConfigOffset);
    snap.mono_timestamp = newest_timestamp(buf, kMonoStream);
    snap.colour_timestamp = newest_timestamp(buf, kColourStream);
    double newest_mono = 0;
    for (int cam = 0; cam < kCameras; ++cam)
        for (int rec = 0; rec < kRecordsPerCamera; ++rec) {
            const std::uint8_t* record = buf + kMonoStream + kFirstRecord + cam * kCameraStride + rec * kRecordStride;
            const double ts = read_f64(record + kTimestampInRecord);
            const float light = read_f32(record + kLightInRecord);
            if (std::isfinite(ts) && ts > newest_mono && light >= 0.0f && light <= 1.0f) {
                newest_mono = ts;
                snap.mono_light = light;
            }
        }

    // 1/2.2 computed in double and rounded to float by the writer. A float
    // division (1.0f / 2.2f) rounds to 0x3ee8ba2e and would never match.
    const std::uint32_t gamma_bits = 0x3ee8ba2f;
    std::uint8_t marker[4];
    std::memcpy(marker, &gamma_bits, sizeof marker);

    for (std::size_t pos = kColourStream + kRecordBeforeMarker; pos + kLightAfterMarker + 4 <= kSize; pos += 4) {
        if (std::memcmp(buf + pos, marker, 4) != 0) continue;
        const std::uint8_t* record = buf + pos - kRecordBeforeMarker;
        ColourFrameInfo info;
        info.timestamp = read_f64(record + kTimestampInRecord);
        info.light = read_f32(buf + pos + kLightAfterMarker);
        if (!(info.timestamp > 0) || !(info.light >= 0.0f && info.light <= 1.0f)) continue;
        if (!snap.colour || info.timestamp > snap.colour->timestamp) snap.colour = info;
    }
    return snap;
}

} // namespace autopass
