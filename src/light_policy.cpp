#include "light_policy.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace autopass {

const char* to_string(Source source) {
    return source == Source::Color ? "colour" : "ir";
}

const char* to_string(Mode mode) {
    switch (mode) {
    case Mode::Auto: return "auto";
    case Mode::ForceColor: return "colour";
    case Mode::ForceIr: return "ir";
    }
    return "unknown";
}

const char* to_string(Evidence e) {
    switch (e) {
    case Evidence::Unknown: return "unknown";
    case Evidence::Neutral: return "neutral";
    case Evidence::Dark: return "dark";
    case Evidence::Bright: return "bright";
    }
    return "unknown";
}

bool parse_mode(const std::string& text, Mode* mode) {
    if (text == "auto") *mode = Mode::Auto;
    else if (text == "colour" || text == "color") *mode = Mode::ForceColor;
    else if (text == "ir" || text == "mono") *mode = Mode::ForceIr;
    else return false;
    return true;
}

std::string validate(const PolicyConfig& c) {
    for (double v : {c.confirm_s, c.confirm_to_colour_s, c.urgent_confirm_s, c.cooldown_base_s, c.cooldown_max_s, c.cooldown_reset_s, c.flicker_slack_s, c.settle_s,
                     c.settle_to_colour_s, c.stale_s})
        if (!std::isfinite(v) || v < 0) return "durations must be finite and non-negative";
    if (c.stale_s <= 0) return "stale_s must be positive";
    if (c.cooldown_max_s < c.cooldown_base_s) return "cooldown_max_s must be at least cooldown_base_s";
    return {};
}

namespace {
std::uint64_t to_ms(double seconds) {
    return static_cast<std::uint64_t>(std::llround(seconds * 1000.0));
}
} // namespace

LightPolicy::LightPolicy(PolicyConfig config, Source initial)
    : config_(config), source_(initial) {}

void LightPolicy::reset_confirmation() {
    pending_ = false;
    pending_since_ms_ = 0;
}

bool LightPolicy::settling(std::uint64_t now_ms) const {
    const double settle = source_ == Source::Color ? config_.settle_to_colour_s : config_.settle_s;
    return switched_ && now_ms - last_switch_ms_ < to_ms(settle);
}

std::uint64_t LightPolicy::cooldown_ms() const {
    double s = config_.cooldown_base_s;
    for (int i = 0; i < flicker_level_ && s < config_.cooldown_max_s; ++i) s *= 2;
    return to_ms(std::min(s, config_.cooldown_max_s));
}

void LightPolicy::adopt(Source actual) {
    if (actual == source_) return;
    source_ = actual;
    reset_confirmation();
}

Decision LightPolicy::decide(std::uint64_t now_ms, Source target, std::string reason, bool automatic) {
    const bool changed = target != source_;
    if (changed) {
        source_ = target;
        switched_ = true;
        last_switch_ms_ = now_ms;
        if (automatic) {
            // Only a switch that came about as soon as the cooldown allowed
            // it is flicker; slower ones step the cooldown back down.
            if (auto_switched_) {
                const std::uint64_t gap = now_ms - last_auto_switch_ms_, allowed = cooldown_ms();
                if (gap < allowed + to_ms(config_.flicker_slack_s))
                    flicker_level_ = std::min(flicker_level_ + 1, 16);
                else if (gap >= allowed + to_ms(config_.cooldown_reset_s))
                    flicker_level_ = 0;
                else if (flicker_level_ > 0)
                    --flicker_level_;
            }
            auto_switched_ = true;
            last_auto_switch_ms_ = now_ms;
        }
        reset_confirmation();
    }
    return {source_, changed, std::move(reason)};
}

Decision LightPolicy::set_mode(std::uint64_t now_ms, Mode mode) {
    mode_ = mode;
    reset_confirmation();
    switch (mode) {
    case Mode::ForceColor: return decide(now_ms, Source::Color, "manual: force colour", false);
    case Mode::ForceIr: return decide(now_ms, Source::Ir, "manual: force IR", false);
    case Mode::Auto: break;
    }
    return decide(now_ms, source_, "manual: auto, holding current source until light confirms a change", false);
}

Decision LightPolicy::update(std::uint64_t now_ms, Evidence evidence, const std::string& why, bool urgent) {
    if (now_ms < last_tick_ms_) now_ms = last_tick_ms_;  // never run time backwards
    last_tick_ms_ = now_ms;

    if (settling(now_ms)) return {source_, false, "settling after switch"};

    if (evidence == Evidence::Unknown) {
        if (last_known_ms_ == 0 || now_ms - last_known_ms_ >= to_ms(config_.stale_s)) {
            reset_confirmation();
            return {source_, false, "no reading, holding"};
        }
        return {source_, false, "missing reading"};
    }
    last_known_ms_ = now_ms;

    if (mode_ != Mode::Auto) return {source_, false, std::string("manual: ") + to_string(mode_)};

    const bool want_ir = source_ == Source::Color && evidence == Evidence::Dark;
    const bool want_color = source_ == Source::Ir && evidence == Evidence::Bright;
    if (!want_ir && !want_color) {
        reset_confirmation();
        return {source_, false, "no change needed"};
    }

    if (!pending_) {
        pending_ = true;
        pending_since_ms_ = now_ms;
    }
    const double confirm = urgent ? config_.urgent_confirm_s : want_color ? config_.confirm_to_colour_s : config_.confirm_s;
    if (now_ms - pending_since_ms_ < to_ms(confirm)) return {source_, false, "confirming"};
    if (!urgent && auto_switched_ && now_ms - last_auto_switch_ms_ < cooldown_ms())
        return {source_, false, "confirmed, waiting for cooldown"};

    const std::string suffix = why.empty() ? "" : " (" + why + ")";
    return want_ir ? decide(now_ms, Source::Ir, "auto: too dark for colour" + suffix, true)
                   : decide(now_ms, Source::Color, "auto: bright enough for colour" + suffix, true);
}

} // namespace autopass
