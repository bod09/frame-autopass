#pragma once

// Decides which passthrough source to show. Pure logic: no SteamVR, no
// clock, no I/O, so it can be tested on the host with synthetic signals.
//
// Sensors (see sensors.hpp) turn their readings into Evidence each tick:
// Dark (too dark for colour passthrough), Bright (bright enough for
// colour), Neutral (no reason to change), or Unknown (no reading). This
// class owns the timing rules: the first switch is fast (a short
// confirmation), and an adaptive cooldown stops flicker. The cooldown
// before the next automatic switch starts short and doubles each time
// switches follow each other quickly, resetting after a quiet period.
// There is also a settle window after any switch and holding when
// readings go stale.

#include <cstdint>
#include <string>

namespace autopass {

enum class Source { Color, Ir };
enum class Mode { Auto, ForceColor, ForceIr };
enum class Evidence { Unknown, Neutral, Dark, Bright };

const char* to_string(Source source);
const char* to_string(Mode mode);
const char* to_string(Evidence evidence);
bool parse_mode(const std::string& text, Mode* mode);

struct PolicyConfig {
    // Evidence for a change must persist this long before a switch fires,
    // so a hand passing over the cameras does not. Urgent evidence (undoing
    // a switch that turned out wrong) needs only urgent_confirm_s.
    // Switching to colour is checked by the colour camera straight away
    // (sensors.hpp), so a mistake there costs under a second: it can be
    // quicker (confirm_to_colour_s) than the switch to IR, where confirm_s
    // keeps a hand over the cameras from counting. 0.5 s is enough there
    // since switching to IR also needs XRService's emitters on, which a
    // hand over the colour camera does not cause (FINDINGS.md 45).
    double confirm_s = 0.5;
    double confirm_to_colour_s = 0.25;
    double urgent_confirm_s = 0.25;
    // Cooldown after an automatic switch before the next automatic one. It
    // only grows for flicker: a switch that comes within flicker_slack_s of
    // the moment the cooldown allowed it (the light changing back as soon
    // as it could, as a flashing light or a fooled sensor does) doubles it,
    // up to cooldown_max_s. Anything slower, such as someone turning lights
    // on and off, steps it back down one level per switch, and
    // cooldown_reset_s of quiet clears it. Manual switches do not count:
    // the cooldown exists to stop automatic flip-flopping, not to delay the
    // user.
    double cooldown_base_s = 2.0;
    double cooldown_max_s = 30.0;
    double cooldown_reset_s = 30.0;
    double flicker_slack_s = 1.5;
    // Evidence this soon after a switch is ignored while the other
    // camera's pipeline settles. The colour camera's light value is valid
    // on its first frame (FINDINGS.md 21), so after a switch to colour a
    // much shorter settle lets a mistaken switch be caught quickly.
    double settle_s = 1.5;
    double settle_to_colour_s = 0.3;
    // With no reading for this long, hold the current source and forget
    // partial confirmation.
    double stale_s = 3.0;
};

// Returns an empty string when the config is usable, otherwise the reason.
std::string validate(const PolicyConfig& config);

struct Decision {
    Source source;
    bool changed;        // source differs from the previous decision
    std::string reason;  // human-readable, for logs and the status file
};

class LightPolicy {
public:
    LightPolicy(PolicyConfig config, Source initial);

    // Feed the evidence for this tick. Timestamps are monotonic
    // milliseconds. `why` is appended to the reason of an automatic switch.
    // `urgent` evidence (a switch that turned out to be a mistake) still
    // needs confirmation but skips the cooldown; it counts towards the
    // cooldown's escalation like any automatic switch.
    Decision update(std::uint64_t now_ms, Evidence evidence, const std::string& why = {}, bool urgent = false);

    // Manual override. Leaving a forced mode for Auto keeps the current
    // source and requires fresh confirmation before any switch.
    Decision set_mode(std::uint64_t now_ms, Mode mode);

    // The source actually shown changed without us. Adopt it without
    // counting it as a switch.
    void adopt(Source actual);

    Mode mode() const { return mode_; }
    Source source() const { return source_; }
    // True within settle_s of the last switch (sensors should reset).
    bool settling(std::uint64_t now_ms) const;
    // Current cooldown after the last automatic switch, in milliseconds.
    std::uint64_t cooldown_ms() const;

private:
    Decision decide(std::uint64_t now_ms, Source target, std::string reason, bool automatic);
    void reset_confirmation();

    PolicyConfig config_;
    Mode mode_ = Mode::Auto;
    Source source_;

    std::uint64_t last_tick_ms_ = 0;
    std::uint64_t last_known_ms_ = 0;

    bool switched_ = false;           // any switch (drives the settle window)
    std::uint64_t last_switch_ms_ = 0;
    bool auto_switched_ = false;      // automatic switch (drives the cooldown)
    std::uint64_t last_auto_switch_ms_ = 0;
    int flicker_level_ = 0;           // cooldown doublings in effect

    bool pending_ = false;
    std::uint64_t pending_since_ms_ = 0;
};

} // namespace autopass
