// Parses real VR_CameraPassthroughState snapshots captured on the headset
// (tests/fixtures, taken from the 21:32 live capture: one lit, one dark).

#include "passthrough_state.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

using namespace autopass;

namespace {

int failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL line %d: %s\n", __LINE__, #expr); ++failures; } } while (0)

std::vector<std::uint8_t> load(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

} // namespace

int main() {
    const auto lit = load("tests/fixtures/state_lit.bin");
    const auto dark = load("tests/fixtures/state_dark.bin");
    CHECK(lit.size() == PassthroughState::kSize);
    CHECK(dark.size() == PassthroughState::kSize);
    if (failures) return 1;

    const auto a = PassthroughState::parse(lit.data());
    CHECK(a.config.valid());
    CHECK(a.config.enabled == 1 && a.config.rgb == 1);
    CHECK(a.colour.has_value());
    if (a.colour) CHECK(a.colour->light > 0.7f && a.colour->timestamp > 0);

    const auto b = PassthroughState::parse(dark.data());
    CHECK(b.config.rgb == 1);
    CHECK(b.colour.has_value());
    if (b.colour) CHECK(b.colour->light < 0.05f);

    // Liveness timestamps: in RGB mode the colour stream is the newest;
    // in mono mode (capture 2, t=60 s) the mono stream is newer and the
    // colour records are frozen at the moment of the switch.
    CHECK(a.colour_timestamp > 0 && a.colour_timestamp > a.mono_timestamp);
    if (a.colour) CHECK(a.colour_timestamp >= a.colour->timestamp);
    const auto mono = load("tests/fixtures/state_mono.bin");
    CHECK(mono.size() == PassthroughState::kSize);
    if (mono.size() == PassthroughState::kSize) {
        const auto m = PassthroughState::parse(mono.data());
        CHECK(m.config.enabled == 1 && m.config.rgb == 0);
        CHECK(m.mono_timestamp > m.colour_timestamp);
        CHECK(m.mono_timestamp - m.colour_timestamp > 10.0);
    }

    // The IR camera's light value (FINDINGS.md 41), captured with IR shown
    // in room B 2026-10-01: light on 0.219, light off (emitters on) 0.
    for (const auto& [file, lit] : {std::pair{"tests/fixtures/state_mono_lit.bin", true},
                                    std::pair{"tests/fixtures/state_mono_dark.bin", false}}) {
        const auto raw = load(file);
        CHECK(raw.size() == PassthroughState::kSize);
        if (raw.size() != PassthroughState::kSize) continue;
        const auto m = PassthroughState::parse(raw.data());
        CHECK(m.config.rgb == 0 && m.mono_light.has_value());
        if (m.mono_light) CHECK(lit ? *m.mono_light > 0.2f && *m.mono_light < 0.25f : *m.mono_light == 0.0f);
    }

    // An all-zero buffer has no records and an all-off config.
    std::vector<std::uint8_t> zero(PassthroughState::kSize, 0);
    const auto z = PassthroughState::parse(zero.data());
    CHECK(!z.colour.has_value());
    CHECK(z.config.valid() && !z.config.enabled);
    CHECK(z.mono_timestamp == 0 && z.colour_timestamp == 0);
    CHECK(!z.mono_light.has_value());

    if (a.colour && b.colour)
        std::printf("lit light=%.3f dark light=%.3f\n", a.colour->light, b.colour->light);
    if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
    std::printf("all passthrough state tests passed\n");
    return 0;
}
