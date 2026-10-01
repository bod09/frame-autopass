#pragma once

// Turn raw readings into Evidence for LightPolicy (FINDINGS.md 29, 34, 38-43).
//
// Three signals, none of them needing an image:
// - XRService's IR emitter state (from its log, xrservice_log.hpp). Its
//   tracking cameras' auto-exposure turns the emitters on only at their
//   longest exposure with gain to spare, within ~0.1 s of darkness, and off
//   only after 5 s of comfortable exposure (often much later, as the
//   emitters light the room themselves). So "on" means dark, and "off"
//   means the room has light, even if a dim one.
// - The colour camera's light value g4 (shared memory, only while colour
//   is shown). Low in a dark room, but also at dusk, when the room is
//   still fine to see in (2026-10-01 18:41), so on its own it never
//   switches.
// - The IR camera's light value (shared memory, the same record field as
//   g4, only while IR is shown). 0 in the dark even with the emitters on,
//   ~0.2-0.35 within half a second of a light coming on, also while
//   tracking is lost (FINDINGS.md 41).
//
// Colour shown, Dark when the emitters are on and:
// - g4 < `colour_min_light`;
// - within `verify_s` of an automatic switch to colour, g4 <
//   `colour_dark_light`: that switch was a mistake. Urgent (skips the
//   cooldown), and what misled it is not believed for a while:
//   "emitters off" for `emitters_off_distrust_s`, the IR light value for
//   `ir_light_lockout_s`, each doubling per repeat up to its maximum.
// With the emitters off nothing in colour switches to IR. The cost: if the
// emitters are fooled off in a dark room (a wall centimetres away,
// 00:13:33), colour stays until they come back on, which they do as soon
// as the wall is no longer lit by them.
//
// IR shown, Bright when either:
// - the emitters are off (and not distrusted). XRService keeps them on for
//   at least 5 s after the light returns, often longer;
// - the IR light value is at least `ir_min_light` for `ir_light_hold_s`:
//   what normally brings colour back, about a second after the light. A
//   wall lit by the emitters from close by read 0.10 (22:04:09), below the
//   threshold; if one fools it anyway, the colour check above undoes it.
//
// Without the emitter signal (for example a SteamVR update changed the log
// line) nothing switches in either direction.

#include "light_policy.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace autopass {

// Exponential moving average over irregular samples.
class Ema {
public:
    explicit Ema(double time_constant_s = 1.0) : tau_s_(time_constant_s) {}
    double add(std::uint64_t now_ms, double value);
    void reset() { has_ = false; }
    bool has() const { return has_; }
    double value() const { return value_; }

private:
    double tau_s_;
    bool has_ = false;
    double value_ = 0;
    std::uint64_t last_ms_ = 0;
};

struct SensorConfig {
    double colour_min_light = 0.6;     // with emitters on: dim for colour below this
    double colour_dark_light = 0.35;   // clearly dark: a switch to colour was a mistake
    double verify_s = 3.0;             // check colour right after switching to it
    double ir_min_light = 0.15;        // IR shown: the IR camera's light value says lit...
    double ir_light_hold_s = 0.0;      // ...for this long (on top of the policy's confirmation)
    double ir_light_lockout_s = 60.0;  // ignore the IR light value after it misled us, doubling...
    double ir_light_lockout_max_s = 600.0;
    double emitters_off_distrust_s = 60.0;  // ignore "emitters off" after it misled us, doubling...
    double emitters_off_distrust_max_s = 600.0;
    // Extra smoothing of the colour light value. 0: the camera already
    // smooths it (0.89 to 0.44 over ~1.5 s when the light goes out).
    double smoothing_s = 0.0;
};

// Which signal produced a Bright or Dark.
enum class Cause { None, EmittersOff, IrLight, EmittersOnDim, VerifyFailed };
const char* to_string(Cause cause);

struct EvidenceResult {
    Evidence evidence = Evidence::Unknown;
    Cause cause = Cause::None;
    bool urgent = false;   // may skip the cooldown (a mistaken switch)
    std::string why;       // short description for logs
};

// Combines the signals into one Evidence for the source being shown.
class EvidenceBuilder {
public:
    explicit EvidenceBuilder(SensorConfig config = {})
        : config_(config), colour_(config.smoothing_s) {}

    // Forget smoothed readings (passthrough hidden, source changed).
    void reset() {
        colour_.reset();
        ir_lit_since_ms_.reset();
    }

    // Tell the builder about a switch that happened, so it can verify an
    // automatic switch to colour and learn from a mistaken one.
    void on_switched(std::uint64_t now_ms, Source to, bool automatic, Cause cause);

    // `emitters_on`: known emitter state, or nullopt. `colour_light` /
    // `ir_light`: fresh readings for this tick, if any.
    EvidenceResult evaluate(std::uint64_t now_ms, Source shown, std::optional<bool> emitters_on,
                            std::optional<double> colour_light, std::optional<double> ir_light = std::nullopt);

    bool distrusting_emitters_off(std::uint64_t now_ms) const { return now_ms < distrust_until_ms_; }
    // Whether autopassd should poll the IR light value (IR shown, and it could
    // matter right now).
    bool wants_ir_light(std::uint64_t now_ms, std::optional<bool> emitters_on) const {
        return now_ms >= ir_light_lockout_until_ms_ && (emitters_on != false || now_ms < distrust_until_ms_);
    }

private:
    EvidenceResult ir_evidence(std::uint64_t now_ms, std::optional<bool> emitters_on,
                               std::optional<double> ir_light);

    SensorConfig config_;
    Ema colour_;
    std::optional<std::uint64_t> ir_lit_since_ms_;
    std::uint64_t ir_light_lockout_until_ms_ = 0;
    double next_ir_light_lockout_s_ = 0;  // 0: use ir_light_lockout_s
    std::uint64_t verify_until_ms_ = 0;
    Cause verify_cause_ = Cause::None;
    std::uint64_t distrust_until_ms_ = 0;
    double next_distrust_s_ = 0;       // 0: use emitters_off_distrust_s
};

} // namespace autopass
