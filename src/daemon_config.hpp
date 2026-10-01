#pragma once

// autopass.conf: optional `key = value` lines overriding the policy defaults.
// Blank lines and lines starting with '#' are ignored. Unknown keys and
// malformed values are errors, reported with their line number, so a typo
// never silently leaves a default in place.
//
//   confirm_s = 0.5                  # evidence must persist this long (to IR)
//   confirm_to_colour_s = 0.25       # ...to colour (checked by the colour camera)
//   urgent_confirm_s = 0.25          # ...when undoing a mistaken switch
//   cooldown_base_s = 2              # anti-flicker cooldown: starts at base,
//   cooldown_max_s = 30              #   doubles for a switch that comes within
//   flicker_slack_s = 1.5            #   flicker_slack_s of being allowed,
//   cooldown_reset_s = 30            #   steps down for slower ones, clears after quiet
//   settle_s = 1.5                   # after a switch to IR
//   settle_to_colour_s = 0.3         # after a switch to colour
//   stale_s = 3.0
//   colour_min_light = 0.6           # with IR emitters on: dark below this
//   colour_dark_light = 0.35         # emitters on and colour this dark right after a
//                                    #   switch to colour: that switch was a mistake
//   verify_s = 3                     # check colour right after switching to it
//   ir_min_light = 0.15              # IR shown: the IR camera's light value says lit
//   ir_light_hold_s = 0              #   for this long (on top of confirm_to_colour_s)
//   ir_light_lockout_s = 60          # ignore the IR light value after it misled us, doubling
//   ir_light_lockout_max_s = 600     #   up to this
//   emitters_off_distrust_s = 60     # ignore "emitters off" after it misled us, doubling
//   emitters_off_distrust_max_s = 600 #  up to this
//   smoothing_s = 0                  # extra smoothing of the colour light value (the camera already smooths it)
//   observe_only = false             # log decisions but never switch

#include "light_policy.hpp"
#include "sensors.hpp"

#include <string>

namespace autopass {

struct DaemonConfig {
    PolicyConfig policy;
    SensorConfig sensors;
    bool observe_only = false;
};

// Parses `text` over `config` (so absent keys keep their current value).
// Returns an empty string on success, otherwise "line N: reason".
std::string parse_daemon_config(const std::string& text, DaemonConfig* config);

} // namespace autopass
