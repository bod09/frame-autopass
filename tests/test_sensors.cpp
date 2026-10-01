// Host-side tests for the sensor layer: XRService log parsing and the
// evidence rules (FINDINGS.md sections 29, 34 and 39), plus an end-to-end
// replay of the scenarios seen live, run through LightPolicy.

#include "light_policy.hpp"
#include "sensors.hpp"
#include "xrservice_log.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace autopass;

namespace {

int failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL line %d: %s\n", __LINE__, #expr); ++failures; } } while (0)

// Real XRService lines from 2026-09-30.
const char* kModeAuto = "Wed Sep 30 2026 22:39:15.067651 INFO: SLAMConsole: [IREmitters] IR emitters mode changed to Auto";
const char* kOn = "Wed Sep 30 2026 22:40:49.267834 INFO: SLAMConsole: [IREmitters] IR Emitters Turned On";
const char* kOff = "Wed Sep 30 2026 22:41:24.230440 INFO: SLAMConsole: [IREmitters] IR Emitters Turned Off";
const char* kNoise = "Wed Sep 30 2026 22:39:23.152963 WARNING: [DeckardCaptureSource] Max number of iteration reached when estimating the CCT from grey world gains";

void test_parsing() {
    XrState s;
    CHECK(apply_xrservice_line(kOn, &s) && s.emitters == std::optional<bool>(true));
    CHECK(apply_xrservice_line(kOff, &s) && s.emitters == std::optional<bool>(false));
    CHECK(apply_xrservice_line(kModeAuto, &s) && s.emitters == std::optional<bool>(false));
    CHECK(!apply_xrservice_line(kNoise, &s));
    CHECK(!apply_xrservice_line("IR Emitters Turned On", &s));  // must carry the [IREmitters] tag

    // Real lines from 2026-09-30 for the other tracked facts.
    CHECK(apply_xrservice_line("Wed Sep 30 2026 22:39:15.083409 INFO: [DeckardCaptureSource] Passthrough cameras resumed", &s));
    CHECK(s.passthrough == std::optional<bool>(true));
    CHECK(apply_xrservice_line("Wed Sep 30 2026 22:44:35.826667 INFO: [DeckardCaptureSource] Passthrough cameras paused", &s));
    CHECK(s.passthrough == std::optional<bool>(false));
    CHECK(apply_xrservice_line("Wed Sep 30 2026 22:44:35.792844 INFO: [UserPresence] Received onEnterStandby from SteamVR. Setting UserPresenceDetected to 0.", &s));
    CHECK(s.standby == std::optional<bool>(true));
    CHECK(apply_xrservice_line("Wed Sep 30 2026 22:49:17.425068 INFO: [UserPresence] Received onLeaveStandby from SteamVR. Setting UserPresenceDetected to 1.", &s));
    CHECK(s.standby == std::optional<bool>(false));
    CHECK(apply_xrservice_line("Wed Sep 30 2026 22:44:35.852332 INFO: Transition to IMUFallback. Latest tracked pose: ref=Cam0", &s));
    CHECK(s.tracking_lost == std::optional<bool>(true));
    CHECK(apply_xrservice_line("Wed Sep 30 2026 22:39:15.336452 INFO: [DCU] [IMUFallback] Disabled with latest prediction:  ", &s));
    CHECK(s.tracking_lost == std::optional<bool>(false));
    // "Tracking cameras streaming paused" is not the passthrough line.
    XrState t;
    CHECK(!apply_xrservice_line("Wed Sep 30 2026 22:44:35.885592 INFO: [DeckardCaptureSource] Tracking cameras streaming paused", &t));
    CHECK(!t.passthrough);

    // Text: last line of each kind wins; a trailing partial line counts.
    const std::string nl = "\n";
    XrState u;
    apply_xrservice_text(std::string(kModeAuto) + nl + kOn + nl + kNoise + nl + kOff + nl + kOn, &u);
    CHECK(u.emitters == std::optional<bool>(true));
}

void test_ema() {
    Ema e(1.0);
    CHECK(!e.has());
    CHECK(e.add(0, 10) == 10);
    const double v = e.add(1000, 0);  // one time constant later: ~37% left
    CHECK(v > 3.5 && v < 3.9);
    e.reset();
    CHECK(!e.has());
}

void test_colour_rules() {
    EvidenceBuilder b;
    // Emitters on and colour dim: dark.
    auto r = b.evaluate(0, Source::Color, true, 0.435);
    CHECK(r.evidence == Evidence::Dark && r.cause == Cause::EmittersOnDim && !r.urgent);
    // Emitters on but colour bright (a hand just left the cameras; the
    // emitters lag ~5 s): not dark.
    b.reset();
    CHECK(b.evaluate(0, Source::Color, true, 0.95).evidence == Evidence::Neutral);
    // Emitters off, dim-looking view in good light (the 23:27:47 case): not dark.
    b.reset();
    CHECK(b.evaluate(0, Source::Color, false, 0.63).evidence == Evidence::Neutral);
    // Emitters off and colour reads dark (dusk, 2026-10-01 18:41): XRService's
    // cameras have light enough, so no switch however long it lasts.
    b.reset();
    for (std::uint64_t t = 0; t <= 60000; t += 1000)
        CHECK(b.evaluate(t, Source::Color, false, 0.01).evidence == Evidence::Neutral);
    // Emitters on and no colour reading ever: dark.
    b.reset();
    CHECK(b.evaluate(0, Source::Color, true, std::nullopt).evidence == Evidence::Dark);
    // Colour seen before but no fresh sample this tick: unknown.
    b.reset();
    b.evaluate(0, Source::Color, true, 0.9);
    CHECK(b.evaluate(250, Source::Color, true, std::nullopt).evidence == Evidence::Unknown);
    // Emitter state unknown: never switch either way.
    b.reset();
    CHECK(b.evaluate(0, Source::Color, std::nullopt, 0.0).evidence == Evidence::Neutral);
    CHECK(b.evaluate(0, Source::Ir, std::nullopt, std::nullopt).evidence == Evidence::Neutral);
}

void test_ir_rules() {
    EvidenceBuilder b;
    auto r = b.evaluate(0, Source::Ir, false, std::nullopt);
    CHECK(r.evidence == Evidence::Bright && r.cause == Cause::EmittersOff);
    CHECK(b.evaluate(0, Source::Ir, true, std::nullopt).evidence == Evidence::Unknown);

    // Emitters on and a dark IR light reading: not bright.
    CHECK(b.evaluate(500, Source::Ir, true, std::nullopt, 0.0).evidence == Evidence::Neutral);
}

void test_ir_light() {
    // The IR camera's own light value (FINDINGS.md 41): a lit reading is
    // bright at once (it steps 0 -> 0.33 within 0.1 s; the policy's
    // confirmation does the rest).
    EvidenceBuilder a;
    auto r = a.evaluate(0, Source::Ir, true, std::nullopt, 0.3);
    CHECK(r.evidence == Evidence::Bright && r.cause == Cause::IrLight);
    CHECK(a.evaluate(250, Source::Ir, true, std::nullopt, 0.14).evidence == Evidence::Neutral);
    // With a hold configured, it must last that long.
    SensorConfig held;
    held.ir_light_hold_s = 0.25;
    EvidenceBuilder b(held);
    CHECK(b.evaluate(0, Source::Ir, true, std::nullopt, 0.3).evidence == Evidence::Neutral);
    r = b.evaluate(250, Source::Ir, true, std::nullopt, 0.3);
    CHECK(r.evidence == Evidence::Bright && r.cause == Cause::IrLight);
    // Dark (0 with the emitters on, 70 s in room B): never.
    EvidenceBuilder c;
    for (std::uint64_t t = 0; t <= 70000; t += 500)
        CHECK(c.evaluate(t, Source::Ir, true, std::nullopt, 0.0).evidence != Evidence::Bright);
    // A dark reading resets the hold.
    EvidenceBuilder d(held);
    d.evaluate(0, Source::Ir, true, std::nullopt, 0.3);
    d.evaluate(125, Source::Ir, true, std::nullopt, 0.0);
    CHECK(d.evaluate(250, Source::Ir, true, std::nullopt, 0.3).evidence != Evidence::Bright);
    // If it misled us (colour dark right after), it is locked out for a while.
    EvidenceBuilder e;
    e.on_switched(0, Source::Color, true, Cause::IrLight);
    CHECK(e.evaluate(300, Source::Color, true, 0.0).cause == Cause::VerifyFailed);
    e.on_switched(600, Source::Ir, true, Cause::VerifyFailed);
    for (std::uint64_t t = 1000; t < 60000; t += 250)
        CHECK(e.evaluate(t, Source::Ir, true, std::nullopt, 0.3).evidence != Evidence::Bright);
    e.evaluate(61000, Source::Ir, true, std::nullopt, 0.3);
    CHECK(e.evaluate(61250, Source::Ir, true, std::nullopt, 0.3).evidence == Evidence::Bright);
}

void test_ir_light_lockout_escalates() {
    // The IR light value misled us (assumed: a surface at medium distance,
    // lit by the emitters, in the dark): after each failed colour check it
    // is ignored for 60 s, then 120 s, ...
    EvidenceBuilder b;
    b.on_switched(10000, Source::Color, true, Cause::IrLight);
    CHECK(b.evaluate(10300, Source::Color, true, 0.0).cause == Cause::VerifyFailed);
    b.on_switched(10600, Source::Ir, true, Cause::VerifyFailed);
    CHECK(!b.wants_ir_light(11000, true));
    for (std::uint64_t t = 11000; t < 70000; t += 500)
        CHECK(b.evaluate(t, Source::Ir, true, std::nullopt, 0.2).evidence != Evidence::Bright);
    CHECK(b.wants_ir_light(70400, true));
    b.on_switched(80000, Source::Color, true, Cause::IrLight);
    b.evaluate(80300, Source::Color, true, 0.0);
    b.on_switched(80600, Source::Ir, true, Cause::VerifyFailed);
    CHECK(!b.wants_ir_light(80300 + 119000, true));
    CHECK(b.wants_ir_light(80300 + 121000, true));
    // Emitters off never needs it unless "off" is distrusted.
    EvidenceBuilder c;
    CHECK(!c.wants_ir_light(0, false));
    CHECK(c.wants_ir_light(0, true));
}

void test_verify_and_distrust() {
    // "Emitters off" took us to colour, but they are back on and colour is
    // dark (a wall fooled XRService): back to IR at once, and "off" is not
    // believed for 60 s, then 120 s, ...
    EvidenceBuilder b;
    b.on_switched(10000, Source::Color, true, Cause::EmittersOff);
    auto r = b.evaluate(10300, Source::Color, true, 0.0);
    CHECK(r.evidence == Evidence::Dark && r.cause == Cause::VerifyFailed && r.urgent);
    CHECK(b.distrusting_emitters_off(10300));
    b.on_switched(10800, Source::Ir, true, Cause::VerifyFailed);
    CHECK(b.evaluate(11000, Source::Ir, false, std::nullopt).evidence != Evidence::Bright);
    CHECK(b.evaluate(70000, Source::Ir, false, std::nullopt).evidence != Evidence::Bright);
    CHECK(b.wants_ir_light(70000, false));  // the IR light value stands in meanwhile
    // It expires by itself: never stuck in IR while the emitters stay off.
    CHECK(!b.distrusting_emitters_off(70400));
    r = b.evaluate(70400, Source::Ir, false, std::nullopt);
    CHECK(r.evidence == Evidence::Bright && r.cause == Cause::EmittersOff);
    // A second mistake doubles it.
    b.on_switched(80000, Source::Color, true, Cause::EmittersOff);
    b.evaluate(80300, Source::Color, true, 0.0);
    CHECK(b.distrusting_emitters_off(80300 + 119000));
    CHECK(!b.distrusting_emitters_off(80300 + 120000));

    // Emitters off and colour dark right after switching (dusk): fine, no revert.
    EvidenceBuilder f;
    f.on_switched(0, Source::Color, true, Cause::EmittersOff);
    r = f.evaluate(300, Source::Color, false, 0.01);
    CHECK(r.evidence == Evidence::Neutral && !r.urgent && !f.distrusting_emitters_off(300));
    // A correct switch to colour (it is light) passes the check quietly,
    // also while the emitters lag behind.
    EvidenceBuilder c;
    c.on_switched(0, Source::Color, true, Cause::IrLight);
    r = c.evaluate(300, Source::Color, true, 0.9);
    CHECK(r.evidence == Evidence::Neutral && !r.urgent);
    // After the verify window, emitters on and dark colour take the normal
    // (not urgent) route.
    EvidenceBuilder d;
    d.on_switched(0, Source::Color, true, Cause::EmittersOff);
    d.evaluate(3500, Source::Color, true, 0.9);
    r = d.evaluate(4000, Source::Color, true, 0.0);
    CHECK(r.evidence == Evidence::Dark && r.cause == Cause::EmittersOnDim && !r.urgent);
    // A manual switch to colour is not verified.
    EvidenceBuilder e;
    e.on_switched(0, Source::Color, false, Cause::None);
    CHECK(!e.evaluate(300, Source::Color, true, 0.0).urgent);
}

// End to end: a simulated room, XRService's emitters as measured (on ~1 s
// after darkness, off ~5 s after light returns, fooled off after ~3 s with
// a wall close to the headset, off at dusk), the colour camera, sensors,
// policy.
enum class Wall { None, Close, Medium };

struct World {
    bool emitters = false;
    bool sticky = false;  // XRService keeps its emitters on once on (00:28-00:30)
    std::uint64_t dark_since = 0, bright_since = 0, wall_since = 0;
    void step(std::uint64_t t, double light, Wall wall_kind) {
        const bool wall = wall_kind == Wall::Close;
        if (wall) {
            if (!wall_since) wall_since = t ? t : 1;
            if (t - wall_since >= 3000) emitters = false;  // reflected IR fools XRService
            return;
        }
        wall_since = 0;
        if (light < 0.3 && !dusk(light)) {
            if (!dark_since) dark_since = t ? t : 1;
            bright_since = 0;
            if (t - dark_since >= 1000) emitters = true;
        } else if (light >= 0.6 || dusk(light)) {
            if (!bright_since) bright_since = t ? t : 1;
            dark_since = 0;
            if (t - bright_since >= 5000 && !sticky) emitters = false;
        } else {
            dark_since = bright_since = 0;
        }
    }
    // Dusk (light 0.1..0.3): the colour camera reads ~0 like a dark room,
    // but XRService's cameras manage without emitters.
    static bool dusk(double light) { return light >= 0.1 && light < 0.3; }
    static double colour(double light) { return light >= 0.6 ? 0.9 : light >= 0.3 ? 0.435 : 0.0; }
    // The IR camera's light value (FINDINGS.md 41-42): 0 dark, ~0.3 lit,
    // 0.10 at a close wall lit by the emitters (measured). A surface at
    // medium distance is assumed to fool it (not measured).
    static double ir_light(double light, Wall wall) {
        if (wall == Wall::Close) return 0.10;
        if (wall == Wall::Medium) return 0.2;
        return light >= 0.6 ? 0.3 : 0.0;
    }
};

struct Replay {
    std::vector<std::uint64_t> switches;
    std::vector<Source> to;
    std::uint64_t colour_in_dark_ms = 0;  // time spent showing colour while it was dark
};

Replay replay(double (*light)(std::uint64_t), Wall (*wall)(std::uint64_t), std::uint64_t end_ms,
              bool emitters_stuck_off = false, bool sticky = false) {
    LightPolicy p({}, Source::Color);
    EvidenceBuilder b;
    World w;
    w.sticky = sticky;
    Replay out;
    for (std::uint64_t t = 0; t <= end_ms; t += 250) {
        const double l = light(t);
        const Wall wk = wall(t);
        w.step(t, l, wk);
        if (emitters_stuck_off) w.emitters = false;
        const bool colour_shown = p.source() == Source::Color;
        if (colour_shown && l < 0.1) out.colour_in_dark_ms += 250;
        // autopassd reads the IR light value at 2 Hz, only when it could matter.
        std::optional<double> ir;
        if (!colour_shown && b.wants_ir_light(t, w.emitters) && t % 500 == 0) ir = World::ir_light(l, wk);
        const auto r = b.evaluate(t, p.source(), w.emitters,
                                  colour_shown ? std::optional<double>(World::colour(l)) : std::nullopt, ir);
        const auto d = p.update(t, r.evidence, r.why, r.urgent);
        if (d.changed) {
            out.switches.push_back(t);
            out.to.push_back(d.source);
            b.on_switched(t, d.source, true, r.cause);
        }
    }
    return out;
}

Wall no_wall(std::uint64_t) { return Wall::None; }

void print(const char* what, const Replay& r) {
    std::printf("%s: switches at", what);
    for (std::size_t i = 0; i < r.switches.size(); ++i)
        std::printf(" %.2f(%s)", r.switches[i] / 1000.0, r.to[i] == Source::Color ? "colour" : "ir");
    std::printf("; colour shown in the dark %.2f s\n", r.colour_in_dark_ms / 1000.0);
}

void test_end_to_end() {
    // Lights off at 10 s, on at 40 s: IR within ~2 s; colour within ~1 s
    // (the IR light value, held 0.25 s), before XRService's emitters go off.
    auto r = replay([](std::uint64_t t) { return t >= 10000 && t < 40000 ? 0.0 : 1.0; }, no_wall, 80000);
    print("off/on", r);
    CHECK(r.switches.size() == 2);
    if (r.switches.size() == 2) {
        CHECK(r.switches[0] >= 11000 && r.switches[0] <= 12000);
        CHECK(r.switches[1] >= 40500 && r.switches[1] <= 41750);
    }

    // The 00:28 walk-through: XRService keeps its emitters on through bright
    // rooms. Colour must still come back.
    r = replay([](std::uint64_t t) { return t >= 10000 && t < 40000 ? 0.0 : 1.0; }, no_wall, 80000, false, true);
    print("bright room, emitters stay on", r);
    CHECK(r.switches.size() == 2);
    if (r.switches.size() == 2) CHECK(r.switches[1] >= 40500 && r.switches[1] <= 41750);

    // Dark throughout with the emitters on: never back to colour.
    r = replay([](std::uint64_t t) { return t >= 5000 ? 0.0 : 1.0; }, no_wall, 180000);
    CHECK(r.switches.size() == 1);

    // A hand over the cameras for 1.5 s in a lit room: no switch.
    r = replay([](std::uint64_t t) { return t >= 10000 && t < 11500 ? 0.0 : 1.0; }, no_wall, 40000);
    CHECK(r.switches.empty());

    // Good light, changing views: never switches.
    r = replay([](std::uint64_t t) { return (t / 7000) % 2 ? 1.0 : 0.7; }, no_wall, 120000);
    CHECK(r.switches.empty());

    // Dimmed to 45%: nothing changes.
    r = replay([](std::uint64_t) { return 0.45; }, no_wall, 60000);
    CHECK(r.switches.empty());

    // Dusk (2026-10-01 18:41): the colour camera reads dark, XRService's
    // emitters stay off. Never switches.
    r = replay([](std::uint64_t t) { return t >= 10000 ? 0.2 : 1.0; }, no_wall, 180000);
    print("dusk", r);
    CHECK(r.switches.empty());

    // Lights off (IR), then back to dusk level: the emitters go off, colour
    // returns and stays (the dark colour reading is not a failed check).
    r = replay([](std::uint64_t t) { return t >= 10000 && t < 40000 ? 0.0 : 0.2; }, no_wall, 180000);
    print("dark then dusk", r);
    CHECK(r.switches.size() == 2 && !r.to.empty() && r.to.back() == Source::Color);

    // Dark room, face near a wall from 20 s to 40 s. XRService turns its
    // emitters off at ~23 s: colour (dark) until they come back on after
    // the wall, then IR stays. The accepted cost of trusting the emitters.
    r = replay([](std::uint64_t) { return 0.0; },
               [](std::uint64_t t) { return t >= 20000 && t < 40000 ? Wall::Close : Wall::None; }, 120000);
    print("close wall in the dark", r);
    CHECK(!r.to.empty() && r.to.back() == Source::Ir);
    CHECK(r.switches.size() <= 3);
    CHECK(r.colour_in_dark_ms <= 22000);  // the initial ~1.5 s, plus the time at the wall

    // A wall at medium distance in the dark for three minutes: the view
    // is assumed to fool the IR light value. Each mistake is a sub-second
    // colour flash, then it is ignored for 60 s, 120 s, ...: a few flashes.
    r = replay([](std::uint64_t) { return 0.0; },
               [](std::uint64_t t) { return t >= 20000 && t < 200000 ? Wall::Medium : Wall::None; }, 240000);
    print("medium wall in the dark", r);
    CHECK(!r.to.empty() && r.to.back() == Source::Ir);
    CHECK(r.switches.size() <= 7);
    CHECK(r.colour_in_dark_ms <= 5000);
}

} // namespace

int main() {
    test_parsing();
    test_ema();
    test_colour_rules();
    test_ir_rules();
    test_ir_light();
    test_ir_light_lockout_escalates();
    test_verify_and_distrust();
    test_end_to_end();
    if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
    std::printf("all sensor tests passed\n");
    return 0;
}
