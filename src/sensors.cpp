#include "sensors.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace autopass {
namespace {

std::uint64_t to_ms(double seconds) { return static_cast<std::uint64_t>(std::llround(seconds * 1000.0)); }

} // namespace

double Ema::add(std::uint64_t now_ms, double value) {
    if (!has_ || tau_s_ <= 0) {
        value_ = value;
        has_ = true;
    } else {
        const double dt = static_cast<double>(now_ms > last_ms_ ? now_ms - last_ms_ : 0) / 1000.0;
        value_ += (1.0 - std::exp(-dt / tau_s_)) * (value - value_);
    }
    last_ms_ = now_ms;
    return value_;
}

const char* to_string(Cause c) {
    switch (c) {
    case Cause::None: return "none";
    case Cause::EmittersOff: return "IR emitters off";
    case Cause::IrLight: return "IR camera sees light";
    case Cause::EmittersOnDim: return "IR emitters on, colour dim";
    case Cause::VerifyFailed: return "colour dark right after switching";
    }
    return "?";
}

void EvidenceBuilder::on_switched(std::uint64_t now_ms, Source to, bool automatic, Cause cause) {
    reset();
    if (to == Source::Color && automatic) {
        verify_until_ms_ = now_ms + to_ms(config_.verify_s);
        verify_cause_ = cause;
    } else {
        verify_until_ms_ = 0;
        verify_cause_ = Cause::None;
    }
}

EvidenceResult EvidenceBuilder::ir_evidence(std::uint64_t now_ms, std::optional<bool> emitters_on,
                                            std::optional<double> ir_light) {
    EvidenceResult r;
    char buf[128];
    if (!*emitters_on && now_ms >= distrust_until_ms_) {
        r.evidence = Evidence::Bright;
        r.cause = Cause::EmittersOff;
        r.why = "IR emitters off";
        return r;
    }
    r.why = *emitters_on ? "IR emitters on" : "IR emitters off, not trusted for a while after a failed colour check";
    if (now_ms < ir_light_lockout_until_ms_) {
        r.evidence = Evidence::Neutral;
        return r;
    }
    if (!ir_light) return r;  // Unknown between samples
    if (*ir_light >= config_.ir_min_light) {
        if (!ir_lit_since_ms_) ir_lit_since_ms_ = now_ms;
    } else {
        ir_lit_since_ms_.reset();
    }
    std::snprintf(buf, sizeof buf, "IR camera light %.2f", *ir_light);
    r.why = buf;
    if (ir_lit_since_ms_ && now_ms - *ir_lit_since_ms_ >= to_ms(config_.ir_light_hold_s)) {
        r.evidence = Evidence::Bright;
        r.cause = Cause::IrLight;
    } else {
        r.evidence = Evidence::Neutral;
    }
    return r;
}

EvidenceResult EvidenceBuilder::evaluate(std::uint64_t now_ms, Source shown, std::optional<bool> emitters_on,
                                         std::optional<double> colour_light, std::optional<double> ir_light) {
    EvidenceResult r;
    char buf[128];
    if (colour_light) colour_.add(now_ms, *colour_light);
    // Without the emitter signal there is no reliable way back from IR, so
    // nothing switches either way.
    if (!emitters_on) {
        r.evidence = Evidence::Neutral;
        r.why = "IR emitter state unknown";
        return r;
    }

    if (shown == Source::Ir) return ir_evidence(now_ms, emitters_on, ir_light);

    if (!colour_.has()) {
        if (*emitters_on) {
            r.evidence = Evidence::Dark;
            r.cause = Cause::EmittersOnDim;
            r.why = "IR emitters on, no colour reading";
        }
        return r;
    }
    // No fresh colour sample this tick is "unknown", not "neutral": neutral
    // would reset the policy's confirmation between samples.
    if (!colour_light) return r;
    const double v = colour_.value();

    // XRService's emitters decide what is dark (FINDINGS.md 38, 39): its
    // tracking cameras turn them on only at their longest exposure. With
    // the emitters off, a low colour reading is a dim room (dusk), not a
    // dark one, and the IR view would be no better.
    if (!*emitters_on) {
        std::snprintf(buf, sizeof buf, "IR emitters off, colour light %.2f", v);
        r.evidence = Evidence::Neutral;
        r.why = buf;
        return r;
    }
    // Right after an automatic switch to colour, emitters on and colour
    // clearly dark: that switch was a mistake. Go back at once and stop
    // believing whatever misled it for a while (doubling each time).
    if (now_ms < verify_until_ms_ && v < config_.colour_dark_light) {
        std::snprintf(buf, sizeof buf, "colour light %.2f right after switching (was: %s)", v, to_string(verify_cause_));
        if (verify_cause_ == Cause::EmittersOff) {
            const double s = next_distrust_s_ > 0 ? next_distrust_s_ : config_.emitters_off_distrust_s;
            distrust_until_ms_ = now_ms + to_ms(s);
            next_distrust_s_ = std::min(s * 2, config_.emitters_off_distrust_max_s);
        } else if (verify_cause_ == Cause::IrLight) {
            const double s = next_ir_light_lockout_s_ > 0 ? next_ir_light_lockout_s_ : config_.ir_light_lockout_s;
            ir_light_lockout_until_ms_ = now_ms + to_ms(s);
            next_ir_light_lockout_s_ = std::min(s * 2, config_.ir_light_lockout_max_s);
        }
        verify_cause_ = Cause::None;  // count this mistake once
        r.evidence = Evidence::Dark;
        r.cause = Cause::VerifyFailed;
        r.urgent = true;
        r.why = buf;
        return r;
    }
    if (v < config_.colour_min_light) {
        std::snprintf(buf, sizeof buf, "IR emitters on, colour light %.2f", v);
        r.evidence = Evidence::Dark;
        r.cause = Cause::EmittersOnDim;
        r.why = buf;
        return r;
    }
    std::snprintf(buf, sizeof buf, "colour light %.2f", v);
    r.evidence = Evidence::Neutral;
    r.why = buf;
    return r;
}

} // namespace autopass
