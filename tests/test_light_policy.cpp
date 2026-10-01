// Host-side tests for LightPolicy: evidence in, switching decisions out.
// Sensor behaviour is tested in test_sensors.cpp.

#include "light_policy.hpp"

#include <cstdio>
#include <functional>
#include <vector>

using namespace autopass;

namespace {

int failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(expr) check((expr), #expr, __LINE__)

// Feeds evidence(t) every `step_ms` from `from_ms` to `to_ms`; returns the
// switch times.
std::vector<std::uint64_t> run(LightPolicy& p, std::uint64_t from_ms, std::uint64_t to_ms,
                               const std::function<Evidence(std::uint64_t)>& evidence, std::uint64_t step_ms = 250) {
    std::vector<std::uint64_t> switches;
    for (std::uint64_t t = from_ms; t <= to_ms; t += step_ms)
        if (p.update(t, evidence(t)).changed) switches.push_back(t);
    return switches;
}

// Evidence for the source currently shown in a room that is dark when
// `dark(t)`: Dark while colour is shown in the dark, Bright while IR is
// shown in the light, otherwise Neutral.
std::function<Evidence(std::uint64_t)> room(LightPolicy& p, const std::function<bool(std::uint64_t)>& dark) {
    return [&p, dark](std::uint64_t t) {
        const bool d = dark(t);
        if (p.source() == Source::Color) return d ? Evidence::Dark : Evidence::Neutral;
        return d ? Evidence::Neutral : Evidence::Bright;
    };
}

// The timing rules are tested with a 0.5 s confirmation; the defaults are
// covered by test_sensors' end-to-end replays.
PolicyConfig cfg() {
    PolicyConfig c;
    c.confirm_s = 0.5;
    return c;
}

void test_validate_and_strings() {
    CHECK(validate(PolicyConfig{}).empty());
    PolicyConfig c;
    c.confirm_s = -1;
    CHECK(!validate(c).empty());
    c = {};
    c.stale_s = 0;
    CHECK(!validate(c).empty());
    c = {};
    c.cooldown_max_s = 1;  // below the 2 s base
    CHECK(!validate(c).empty());
    Mode m;
    CHECK(parse_mode("auto", &m) && m == Mode::Auto);
    CHECK(parse_mode("colour", &m) && m == Mode::ForceColor);
    CHECK(parse_mode("color", &m) && m == Mode::ForceColor);
    CHECK(parse_mode("ir", &m) && m == Mode::ForceIr);
    CHECK(!parse_mode("bright", &m));
}

void test_first_switch_is_fast() {
    // Lights off in a room: the switch comes after confirm_s (0.5 s), not
    // after any cooldown.
    LightPolicy p(cfg(), Source::Color);
    const auto s = run(p, 0, 20000, room(p, [](std::uint64_t t) { return t >= 5000; }));
    CHECK(s.size() == 1);
    if (!s.empty()) CHECK(s[0] >= 5500 && s[0] <= 5750);
}

void test_normal_off_then_on_is_fast_both_ways() {
    LightPolicy p(cfg(), Source::Color);
    const auto s = run(p, 0, 60000, room(p, [](std::uint64_t t) { return t >= 5000 && t < 30000; }));
    CHECK(s.size() == 2);
    if (s.size() == 2) {
        CHECK(s[0] <= 5750);
        CHECK(s[1] >= 30000 && s[1] <= 32250);  // settle 1.5 s already long past; confirm 0.5 s
    }
}

void test_quick_reversal_waits_for_base_cooldown() {
    // Light off, then straight back on after 1 s: the reversal waits for
    // the 2 s cooldown, not longer.
    LightPolicy p(cfg(), Source::Color);
    const auto s = run(p, 0, 20000, room(p, [](std::uint64_t t) { return t >= 5000 && t < 6000; }));
    CHECK(s.size() == 2);
    if (s.size() == 2) CHECK(s[1] - s[0] >= 2000 && s[1] - s[0] <= 2750);
}

void test_flickering_light_escalates_cooldown() {
    // A light flickering on/off every second for two minutes. Without the
    // cooldown this would switch ~120 times; with it, the gaps double.
    LightPolicy p(cfg(), Source::Color);
    const auto s = run(p, 0, 120000, room(p, [](std::uint64_t t) { return (t / 1000) % 2 == 1; }));
    CHECK(s.size() >= 3 && s.size() <= 10);
    for (std::size_t i = 2; i < s.size(); ++i) CHECK(s[i] - s[i - 1] >= s[i - 1] - s[i - 2] - 1000);
    // Gaps reach the 30 s cap and never exceed cap + one flicker period.
    if (s.size() >= 2) CHECK(s.back() - s[s.size() - 2] <= 32000);
    std::printf("flicker: %zu switches in 120 s:", s.size());
    for (auto t : s) std::printf(" %.1f", t / 1000.0);
    std::printf("\n");
}

void test_lights_toggled_by_hand_stay_fast() {
    // Someone switching the lights every 5 s for a minute (2026-10-01 18:55:
    // the old rule made each switch slower than the last). Every change is
    // followed within confirm time; the cooldown never grows.
    LightPolicy p(cfg(), Source::Color);
    const auto s = run(p, 0, 62000, room(p, [](std::uint64_t t) { return t >= 2000 && (t - 2000) / 5000 % 2 == 0; }));
    CHECK(s.size() == 12);
    for (std::size_t i = 0; i < s.size(); ++i) {
        const std::uint64_t change = 2000 + 5000 * i;
        CHECK(s[i] >= change && s[i] <= change + 1250);
    }
    CHECK(p.cooldown_ms() == 2000);
}

void test_cooldown_resets_after_quiet_period() {
    LightPolicy p(cfg(), Source::Color);
    // Four quick reversals escalate the cooldown...
    run(p, 0, 30000, room(p, [](std::uint64_t t) { return (t / 1000) % 2 == 1; }));
    CHECK(p.cooldown_ms() > 2000);
    // ...then a quiet minute in steady light, and a normal off/on is fast
    // again.
    const bool steady_dark = p.source() == Source::Ir;  // keep the light matching what is shown
    run(p, 30250, 100000, room(p, [steady_dark](std::uint64_t) { return steady_dark; }));
    const bool start_dark = p.source() == Source::Ir;
    const auto s = run(p, 100250, 110000, room(p, [start_dark](std::uint64_t) { return !start_dark; }));
    CHECK(s.size() == 1);
    if (!s.empty()) CHECK(s[0] <= 101000);
}

void test_brief_evidence_ignored() {
    // Dark evidence for less than confirm_s (a hand passing) never switches.
    LightPolicy p(cfg(), Source::Color);
    CHECK(run(p, 0, 30000, room(p, [](std::uint64_t t) { return t >= 5000 && t < 5250; })).empty());
    // Evidence flickering faster than confirm_s never switches either.
    LightPolicy q(cfg(), Source::Color);
    int changes = 0;
    for (std::uint64_t t = 0; t < 60000; t += 250)
        if (q.update(t, (t / 250) % 2 ? Evidence::Dark : Evidence::Neutral).changed) ++changes;
    CHECK(changes == 0);
}

void test_unknown_between_samples_keeps_confirming() {
    // A sensor sampling slower than the loop produces Unknown in between;
    // confirmation keeps accumulating across those ticks.
    LightPolicy p(cfg(), Source::Color);
    const auto s = run(p, 0, 5000, [](std::uint64_t t) { return (t / 250) % 2 ? Evidence::Unknown : Evidence::Dark; });
    CHECK(s.size() == 1);
}

void test_stale_readings_hold_and_reset() {
    LightPolicy p(cfg(), Source::Color);
    run(p, 0, 2000, [](auto) { return Evidence::Neutral; });
    // Dark for 0.25 s (not yet confirmed), then readings stop for 5 s.
    CHECK(run(p, 2250, 2500, [](auto) { return Evidence::Dark; }).empty());
    CHECK(run(p, 2750, 7750, [](auto) { return Evidence::Unknown; }).empty());
    CHECK(p.source() == Source::Color);
    // Dark again: confirmation restarts from zero.
    const auto s = run(p, 8000, 12000, [](auto) { return Evidence::Dark; });
    CHECK(s.size() == 1);
    if (!s.empty()) CHECK(s[0] >= 8500);
}

void test_manual_override() {
    LightPolicy p(cfg(), Source::Color);
    auto d = p.set_mode(1000, Mode::ForceIr);
    CHECK(d.changed && d.source == Source::Ir);
    CHECK(run(p, 1250, 60000, [](auto) { return Evidence::Bright; }).empty());
    d = p.set_mode(60000, Mode::Auto);
    CHECK(!d.changed && d.source == Source::Ir);
    d = p.set_mode(61000, Mode::ForceColor);
    CHECK(d.changed && d.source == Source::Color);
    CHECK(!p.set_mode(62000, Mode::ForceColor).changed);
}

void test_manual_switch_does_not_delay_auto() {
    // Seen live 23:27: force IR in bright light, back to auto; colour
    // returns after settle + confirm, with no cooldown. A long cooldown
    // makes the difference visible past the settle window.
    PolicyConfig c = cfg();
    c.cooldown_base_s = 10;
    LightPolicy p(c, Source::Color);
    CHECK(p.set_mode(5000, Mode::ForceIr).changed);
    p.set_mode(6000, Mode::Auto);
    const auto s = run(p, 6250, 20000, [](auto) { return Evidence::Bright; });
    CHECK(s.size() == 1);
    if (!s.empty()) CHECK(s[0] <= 7250);
}

void test_settle_ignores_transition_evidence() {
    LightPolicy p(cfg(), Source::Color);
    CHECK(p.set_mode(1000, Mode::ForceIr).changed);
    p.set_mode(1100, Mode::Auto);
    for (std::uint64_t t = 1250; t < 2500; t += 250)
        CHECK(p.update(t, Evidence::Bright).reason == "settling after switch");
    CHECK(p.update(2600, Evidence::Bright).reason == "confirming");
}

void test_adopt_is_not_a_switch() {
    LightPolicy p(cfg(), Source::Color);
    p.adopt(Source::Ir);
    CHECK(p.source() == Source::Ir && !p.settling(0));
    const auto s = run(p, 0, 5000, [](auto) { return Evidence::Bright; });
    CHECK(s.size() == 1);
    if (!s.empty()) CHECK(s[0] <= 750);
}

void test_urgent_skips_cooldown_with_short_confirm() {
    // A switch to colour turned out wrong (it is dark): undoing it must not
    // wait for the cooldown, and needs only urgent_confirm_s (0.25 s).
    PolicyConfig c = cfg();
    c.cooldown_base_s = 10;
    LightPolicy p(c, Source::Ir);
    std::uint64_t to_colour = 0, back = 0;
    for (std::uint64_t t = 0; t < 5000 && !to_colour; t += 250)
        if (p.update(t, Evidence::Bright).changed) to_colour = t;
    CHECK(to_colour > 0);
    for (std::uint64_t t = to_colour + 50; t < 20000 && !back; t += 50)
        if (p.update(t, Evidence::Dark, "", true).changed) back = t;
    CHECK(back && back - to_colour <= 650);  // settle 0.3 s + urgent confirm 0.25 s
    // Non-urgent evidence afterwards respects the escalated cooldown (the
    // quick reversal doubled it to 20 s).
    CHECK(p.cooldown_ms() == 20000);
    const auto s2 = run(p, back + 250, back + 40000, [](auto) { return Evidence::Bright; });
    CHECK(s2.size() == 1);
    if (!s2.empty()) CHECK(s2[0] - back >= 20000);
}

void test_time_going_backwards_is_safe() {
    LightPolicy p(cfg(), Source::Color);
    run(p, 0, 5000, [](auto) { return Evidence::Neutral; });
    CHECK(!p.update(4000, Evidence::Neutral).changed);
    CHECK(!p.update(100, Evidence::Dark).changed);
}

} // namespace

int main() {
    test_validate_and_strings();
    test_first_switch_is_fast();
    test_normal_off_then_on_is_fast_both_ways();
    test_quick_reversal_waits_for_base_cooldown();
    test_flickering_light_escalates_cooldown();
    test_lights_toggled_by_hand_stay_fast();
    test_cooldown_resets_after_quiet_period();
    test_brief_evidence_ignored();
    test_unknown_between_samples_keeps_confirming();
    test_stale_readings_hold_and_reset();
    test_manual_override();
    test_manual_switch_does_not_delay_auto();
    test_settle_ignores_transition_evidence();
    test_adopt_is_not_a_switch();
    test_urgent_skips_cooldown_with_short_confirm();
    test_time_going_backwards_is_safe();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all light policy tests passed\n");
    return 0;
}
