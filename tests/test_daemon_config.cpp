// Host-side tests for the autopass.conf parser.

#include "daemon_config.hpp"

#include <cstdio>

using namespace autopass;

namespace {
int failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL line %d: %s\n", __LINE__, #expr); ++failures; } } while (0)
} // namespace

int main() {
    DaemonConfig c;
    CHECK(parse_daemon_config("", &c).empty());
    CHECK(c.sensors.colour_min_light == 0.6 && c.sensors.colour_dark_light == 0.35 && !c.observe_only);
    CHECK(c.policy.confirm_s == 0.5 && c.policy.urgent_confirm_s == 0.25 && c.policy.cooldown_base_s == 2.0);

    CHECK(parse_daemon_config("# comment\n\n colour_min_light = 0.5  # inline\nverify_s=4\nobserve_only = true\n", &c).empty());
    CHECK(c.sensors.colour_min_light == 0.5 && c.sensors.verify_s == 4.0 && c.observe_only);
    CHECK(parse_daemon_config("cooldown_base_s = 1\ncooldown_max_s = 20\n", &c).empty());
    CHECK(c.policy.cooldown_base_s == 1.0 && c.policy.cooldown_max_s == 20.0);

    // Errors leave the config untouched and name the line.
    DaemonConfig d;
    const auto e1 = parse_daemon_config("confirm_s = 2\nverfy_s = 5\n", &d);
    CHECK(e1.find("line 2") != std::string::npos && e1.find("unknown key") != std::string::npos);
    CHECK(d.policy.confirm_s == 0.5);
    // Removed keys are rejected, not silently ignored.
    CHECK(parse_daemon_config("ir_max_grain = 7\n", &d).find("unknown key") != std::string::npos);
    CHECK(parse_daemon_config("grain_lockout_s = 60\n", &d).find("unknown key") != std::string::npos);
    CHECK(parse_daemon_config("ir_min_light = 0.2\nir_light_hold_s = 0.5\nconfirm_to_colour_s = 0.5\n", &d).empty());
    CHECK(d.policy.confirm_to_colour_s == 0.5);
    CHECK(d.sensors.ir_min_light == 0.2 && d.sensors.ir_light_hold_s == 0.5);
    CHECK(!parse_daemon_config("ir_min_light = 0\n", &d).empty());
    CHECK(!parse_daemon_config("ir_light_lockout_max_s = 10\n", &d).empty());  // below ir_light_lockout_s
    // colour_dark_light above colour_min_light makes no sense.
    CHECK(!parse_daemon_config("colour_dark_light = 0.7\n", &d).empty());
    CHECK(parse_daemon_config("cooldown_base_s = ten\n", &d).find("not a number") != std::string::npos);
    CHECK(parse_daemon_config("cooldown_base_s = 10s\n", &d).find("not a number") != std::string::npos);
    CHECK(parse_daemon_config("min_dwell_s = 10\n", &d).find("unknown key") != std::string::npos);
    CHECK(!parse_daemon_config("cooldown_base_s = 40\n", &d).empty());  // above cooldown_max_s
    CHECK(parse_daemon_config("observe_only = yes\n", &d).find("true or false") != std::string::npos);
    CHECK(parse_daemon_config("just words\n", &d).find("key = value") != std::string::npos);
    // Values that parse but fail policy validation are rejected too.
    CHECK(!parse_daemon_config("colour_min_light = 2\n", &d).empty());
    CHECK(d.sensors.colour_min_light == 0.6);

    if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
    std::printf("all autopassd config tests passed\n");
    return 0;
}
