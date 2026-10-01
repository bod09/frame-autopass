#include "daemon_config.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace autopass {
namespace {

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

bool parse_number(const std::string& text, double* out) {
    if (text.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(text.c_str(), &end);
    if (errno || !end || *end != '\0' || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

} // namespace

std::string parse_daemon_config(const std::string& text, DaemonConfig* config) {
    DaemonConfig result = *config;
    std::istringstream in(text);
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        const auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;
        const auto eq = line.find('=');
        const std::string where = "line " + std::to_string(number) + ": ";
        if (eq == std::string::npos) return where + "expected key = value";
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));

        if (key == "observe_only") {
            if (value == "true") result.observe_only = true;
            else if (value == "false") result.observe_only = false;
            else return where + "observe_only must be true or false";
            continue;
        }
        double* target = nullptr;
        if (key == "confirm_s") target = &result.policy.confirm_s;
        else if (key == "urgent_confirm_s") target = &result.policy.urgent_confirm_s;
        else if (key == "confirm_to_colour_s") target = &result.policy.confirm_to_colour_s;
        else if (key == "cooldown_base_s") target = &result.policy.cooldown_base_s;
        else if (key == "cooldown_max_s") target = &result.policy.cooldown_max_s;
        else if (key == "cooldown_reset_s") target = &result.policy.cooldown_reset_s;
        else if (key == "flicker_slack_s") target = &result.policy.flicker_slack_s;
        else if (key == "settle_s") target = &result.policy.settle_s;
        else if (key == "settle_to_colour_s") target = &result.policy.settle_to_colour_s;
        else if (key == "stale_s") target = &result.policy.stale_s;
        else if (key == "colour_min_light") target = &result.sensors.colour_min_light;
        else if (key == "colour_dark_light") target = &result.sensors.colour_dark_light;
        else if (key == "verify_s") target = &result.sensors.verify_s;
        else if (key == "ir_min_light") target = &result.sensors.ir_min_light;
        else if (key == "ir_light_hold_s") target = &result.sensors.ir_light_hold_s;
        else if (key == "ir_light_lockout_s") target = &result.sensors.ir_light_lockout_s;
        else if (key == "ir_light_lockout_max_s") target = &result.sensors.ir_light_lockout_max_s;
        else if (key == "emitters_off_distrust_s") target = &result.sensors.emitters_off_distrust_s;
        else if (key == "emitters_off_distrust_max_s") target = &result.sensors.emitters_off_distrust_max_s;
        else if (key == "smoothing_s") target = &result.sensors.smoothing_s;
        else return where + "unknown key '" + key + "'";
        if (!parse_number(value, target)) return where + "'" + value + "' is not a number";
    }
    const std::string problem = validate(result.policy);
    if (!problem.empty()) return problem;
    const auto& s = result.sensors;
    if (!(s.colour_min_light > 0 && s.colour_min_light <= 1) ||
        !(s.colour_dark_light > 0 && s.colour_dark_light <= s.colour_min_light))
        return "colour light thresholds must be in (0, 1], colour_dark_light <= colour_min_light";
    if (!(s.verify_s >= 0) || !(s.smoothing_s >= 0) || !(s.ir_light_hold_s >= 0) ||
        !(s.ir_min_light > 0 && s.ir_min_light <= 1) || !(s.ir_light_lockout_s >= 0) ||
        !(s.ir_light_lockout_max_s >= s.ir_light_lockout_s) || !(s.emitters_off_distrust_s >= 0) ||
        !(s.emitters_off_distrust_max_s >= s.emitters_off_distrust_s))
        return "sensor durations must be non-negative (each _max_s at least its base)";
    *config = result;
    return {};
}

} // namespace autopass
