// autopassd: adaptive passthrough for the Steam Frame.
//
// Shows the Arcturus colour feed in good light and the built-in mono IR
// feed in low light, fully automatically: a fast switch, and an adaptive
// cooldown so it never flickers (light_policy.hpp).
//
// Designed to cost nothing when passthrough is not in use:
// - It does not connect to SteamVR. It follows XRService's session log
//   with inotify (xrservice_log.hpp): passthrough shown/hidden, standby,
//   IR emitters, tracking loss. XRService writes nothing while the headset
//   is idle, so autopassd is not woken at all then.
// - While passthrough is shown in colour it reads the colour camera's light
//   value from shared memory once a second (4 Hz while a decision is
//   pending). While IR is shown with the IR emitters on, it reads the IR
//   camera's light value from the same shared memory 4 times a second to
//   recognise a lit room (sensors.hpp), and logs it every 10 s. It never
//   captures images.
// - To switch, it runs `autopass set rgb|mono`, which connects to SteamVR as
//   a background client for ~15 ms (it does not wake the headset), checks
//   the private interface's fingerprint, switches and exits.
//
// Started and stopped with SteamVR by a systemd user unit (`autopass install`).
// Usage: autopassd [--observe] [--verbose]

#include "app_paths.hpp"
#include "daemon_config.hpp"
#include "light_policy.hpp"
#include "passthrough_state.hpp"
#include "sensors.hpp"
#include "xrservice_log.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <deque>
#include <optional>
#include <poll.h>
#include <sys/inotify.h>
#include <spawn.h>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace {

using autopass::Evidence;
using autopass::Source;

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

bool g_verbose = false;
std::FILE* g_log = nullptr;

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string wall_time() {
    char buf[32];
    const std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return buf;
}

void log(const std::string& msg) {
    const std::string line = wall_time() + " " + msg + "\n";
    if (g_log) {
        std::fputs(line.c_str(), g_log);
        std::fflush(g_log);
        struct stat st{};
        if (::stat(autopass::log_path().c_str(), &st) == 0 && st.st_size > 512 * 1024) {
            std::fclose(g_log);
            std::rename(autopass::log_path().c_str(), (autopass::log_path() + ".1").c_str());
            g_log = std::fopen(autopass::log_path().c_str(), "a");
        }
    } else {
        std::fputs(line.c_str(), stderr);
    }
}

std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

const char* tri(const std::optional<bool>& v, const char* yes, const char* no) {
    return !v ? "unknown" : *v ? yes : no;
}

// Exit codes of `autopass set` (tools/autopass.cpp).
constexpr int kSetOk = 0;
constexpr int kSetMismatch = 3;     // SteamVR's private interface changed
constexpr int kSetNotEnabled = 4;   // passthrough camera not enabled
constexpr int kSetNoModule = 5;     // no Arcturus colour module

// Runs `autopass set rgb|mono --quiet` and waits up to 5 s. Returns its exit
// code, or -1 if it could not be run or did not finish.
int run_switch_helper(bool rgb) {
    const std::string helper = autopass::install_dir() + "/autopass";
    char arg0[] = "autopass", arg1[] = "set", arg_rgb[] = "rgb", arg_mono[] = "mono", arg3[] = "--quiet";
    char* argv[] = {arg0, arg1, rgb ? arg_rgb : arg_mono, arg3, nullptr};
    pid_t pid = 0;
    if (::posix_spawn(&pid, helper.c_str(), nullptr, nullptr, argv, environ) != 0) return -1;
    for (int i = 0; i < 100; ++i) {
        int status = 0;
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (r < 0 && errno != EINTR) return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    return -1;
}

} // namespace

int main(int argc, char** argv) {
    bool observe_flag = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--observe") observe_flag = true;
        else if (a == "--verbose") g_verbose = true;
        else if (a == "--help") {
            std::printf("autopassd [--observe] [--verbose]\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument %s\n", a.c_str());
            return 2;
        }
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    if (!autopass::make_dirs(autopass::config_dir())) {
        std::fprintf(stderr, "cannot create %s\n", autopass::config_dir().c_str());
        return 1;
    }
    const int lock_fd = ::open(autopass::lock_path().c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd < 0 || ::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        std::fprintf(stderr, "autopassd is already running\n");
        return 1;
    }
    g_log = std::fopen(autopass::log_path().c_str(), "a");

    autopass::DaemonConfig cfg;
    std::string conf_text;
    if (autopass::read_file(autopass::conf_path(), &conf_text)) {
        const auto problem = autopass::parse_daemon_config(conf_text, &cfg);
        if (!problem.empty()) {
            log("autopass.conf rejected (" + problem + "); using defaults");
            cfg = {};
        }
    }

    // Crash guard (FINDINGS.md 31): two XRService restarts within 10 s of an
    // autopassd switch stop switching until `autopass guard reset`.
    std::string guard_text;
    int guard_hits = 0;
    if (autopass::read_file(autopass::crash_guard_path(), &guard_text))
        for (char c : guard_text) guard_hits += c == '\n';
    // Why switching is off, if it is (shown by `autopass status`): empty, or
    // observe-only, crash-guard, steamvr-changed, no-colour-module.
    std::string blocked = observe_flag || cfg.observe_only ? "observe-only" : guard_hits >= 2 ? "crash-guard" : "";
    if (blocked == "crash-guard") log("crash guard: XRService restarted twice soon after switches; observe-only until `autopass guard reset`");
    else if (!blocked.empty()) log("observe-only: decisions are logged, never applied");

    // Follow XRService's log. SteamVR may still be starting: retry quietly.
    autopass::XrServiceLog xr;
    bool warned = false;
    while (!g_stop && !xr.start()) {
        if (!warned) log("waiting for XRService's log (" + xr.error() + ")");
        warned = true;
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    if (g_stop) return 0;

    autopass::PassthroughState state;
    Source initial = Source::Color;
    if (const auto snap = state.read(); snap && !snap->config.rgb) initial = Source::Ir;
    autopass::LightPolicy policy(cfg.policy, initial);
    autopass::EvidenceBuilder evidence(cfg.sensors);
    log(std::string("autopassd ") + autopass::version() + " started: showing " + autopass::to_string(initial));

    // Passthrough mode (auto / always colour / always IR) from the mode
    // file, re-read whenever it changes.
    const auto read_mode = [] {
        std::string text;
        autopass::Mode m = autopass::Mode::Auto;
        if (autopass::read_file(autopass::mode_path(), &text)) {
            while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
            if (!autopass::parse_mode(text, &m)) m = autopass::Mode::Auto;
        }
        return m;
    };
    autopass::Mode mode = read_mode();
    if (mode != autopass::Mode::Auto) policy.set_mode(now_ms(), mode);
    log(std::string("mode: ") + autopass::to_string(mode));
    const int config_watch = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (config_watch >= 0) ::inotify_add_watch(config_watch, autopass::config_dir().c_str(), IN_CLOSE_WRITE | IN_MOVED_TO);
    // A forced mode switches once when it is chosen and each time passthrough
    // appears; it never polls in between.
    bool force_pending = true;

    double last_colour_ts = -1;
    double mono_ts_at_last_light = 0;
    std::uint64_t last_ir_log_ms = 0;
    // The last 10 s of IR light readings (4 a second), written to the log
    // when autopassd returns to colour, so a slow return shows what it read.
    struct LightNote {
        std::uint64_t ms;
        double light;
    };
    std::deque<LightNote> recent_ir;
    // When the cameras first saw the room go dark or lit, to log how long
    // XRService takes to turn its IR emitters on or off after that.
    std::optional<std::uint64_t> dark_seen_ms, lit_seen_ms;
    std::optional<bool> last_emitters;
    long long last_switch_wall = 0;
    std::uint64_t expect_source_until_ms = 0, last_state_retry_ms = 0;
    bool was_active = false;
    std::string last_reason, last_status;

    autopass::Cause last_cause = autopass::Cause::None;
    const auto apply = [&](Source target, const std::string& reason, std::uint64_t now, bool automatic = true) {
        if (!blocked.empty()) {
            log(std::string("would switch to ") + autopass::to_string(target) + " (" + reason + ") [" + blocked + "]");
            return;
        }
        const int rc = run_switch_helper(target == Source::Color);
        if (rc == kSetOk) {
            log(std::string("switched to ") + autopass::to_string(target) + ": " + reason);
            if (target == Source::Color && !recent_ir.empty()) {
                std::string notes = "IR light before the switch (s ago: value):";
                for (const auto& n : recent_ir) {
                    char item[32];
                    std::snprintf(item, sizeof item, " %.1f:%.2f", (now - n.ms) / 1000.0, n.light);
                    notes += item;
                }
                log(notes);
            }
            recent_ir.clear();
            expect_source_until_ms = now + 3000;
            last_switch_wall = static_cast<long long>(std::time(nullptr));
            evidence.on_switched(now, target, automatic, last_cause);
        } else if (rc == kSetMismatch) {
            log("SteamVR's camera interface changed (SteamVR update?); switching disabled until frame-autopass "
                "is updated (`autopass update`)");
            blocked = "steamvr-changed";
        } else if (rc == kSetNoModule) {
            log("Arcturus colour module not detected: not switching");
            blocked = "no-colour-module";
        } else if (rc == kSetNotEnabled) {
            log("passthrough camera not enabled; not switching");
        } else {
            log("switch helper failed (code " + std::to_string(rc) + ")");
        }
        // Whatever happened, follow what is actually shown.
        if (const auto snap = state.read()) policy.adopt(snap->config.rgb ? Source::Color : Source::Ir);
        if (rc != kSetOk) evidence.reset();
    };

    // First pass runs at once, so a passthrough already shown at start-up is
    // handled without waiting for XRService's next log line. Afterwards -1
    // means: sleep until XRService logs something.
    int wait_ms = 0;
    while (!g_stop) {
        pollfd pfds[2] = {{xr.fd(), POLLIN, 0}, {config_watch, POLLIN, 0}};
        if (::poll(pfds, config_watch >= 0 ? 2 : 1, wait_ms) < 0 && errno != EINTR) break;
        if (g_stop) break;
        const std::uint64_t now = now_ms();

        if (config_watch >= 0 && (pfds[1].revents & POLLIN)) {
            alignas(inotify_event) char events[4096];
            bool mode_touched = false;
            for (ssize_t n; (n = ::read(config_watch, events, sizeof events)) > 0;)
                for (char* p = events; p < events + n;) {
                    const auto* e = reinterpret_cast<const inotify_event*>(p);
                    if (e->len && std::string(e->name) == "mode") mode_touched = true;
                    p += sizeof(inotify_event) + e->len;
                }
            const autopass::Mode m = mode_touched ? read_mode() : mode;
            if (m != mode) {
                mode = m;
                log(std::string("mode: ") + autopass::to_string(mode));
                policy.set_mode(now, mode);
                evidence.reset();
                force_pending = true;
            }
        }

        const bool xr_changed = xr.update();
        const autopass::XrState& xs = xr.state();
        if (xr.take_restarted()) {
            const long long restart = xr.session_start();
            const long long since = last_switch_wall && restart ? restart - last_switch_wall : -1;
            log("XRService restarted" + (since >= 0 ? " " + std::to_string(since) + " s after autopassd's last switch"
                                                    : std::string(" (no recent autopassd switch)")));
            if (since >= 0 && since <= 10) {
                std::string text;
                autopass::read_file(autopass::crash_guard_path(), &text);
                text += wall_time() + " XRService restart " + std::to_string(since) + " s after a switch\n";
                autopass::write_file_atomic(autopass::crash_guard_path(), text);
                log("crash guard: recorded; switching stops after two such restarts");
            }
        }
        if (g_verbose && xr_changed)
            log(std::string("xrservice: passthrough ") + tri(xs.passthrough, "shown", "hidden") + ", standby " +
                tri(xs.standby, "yes", "no") + ", emitters " + tri(xs.emitters, "on", "off") + ", tracking " +
                tri(xs.tracking_lost, "lost", "ok"));

        if (xs.emitters != last_emitters) {
            if (last_emitters && xs.emitters) {
                const auto& seen = *xs.emitters ? dark_seen_ms : lit_seen_ms;
                char msg[160];
                if (seen)
                    std::snprintf(msg, sizeof msg, "IR emitters turned %s %.1f s after the cameras first saw the room %s",
                                  *xs.emitters ? "on" : "off", (now - *seen) / 1000.0, *xs.emitters ? "dark" : "lit");
                else
                    std::snprintf(msg, sizeof msg, "IR emitters turned %s (no light change seen before it)",
                                  *xs.emitters ? "on" : "off");
                log(msg);
            }
            last_emitters = xs.emitters;
        }

        const bool active = xs.passthrough == true && xs.standby != true;
        if (active != was_active) {
            log(active ? "passthrough shown: sensing" : "passthrough not shown: idle");
            was_active = active;
            // Without the Arcturus module there is no colour feed to switch to.
            // Checked each time passthrough appears (a sysfs read).
            if (active) {
                const bool module = autopass::colour_module_present();
                if (!module && blocked.empty()) {
                    log("Arcturus colour module not detected: not switching");
                    blocked = "no-colour-module";
                } else if (module && blocked == "no-colour-module") {
                    log("Arcturus colour module detected: switching again");
                    blocked.clear();
                }
            }
            evidence.reset();
            last_colour_ts = -1;
            dark_seen_ms.reset();
            lit_seen_ms.reset();
            force_pending = true;
        }

        Evidence ev = Evidence::Unknown;
        Source shown = policy.source();
        bool want_ir = false;
        if (active && mode != autopass::Mode::Auto) {
            // Always colour or always IR: switch if needed, then sleep.
            const Source target = mode == autopass::Mode::ForceColor ? Source::Color : Source::Ir;
            if (const auto snap = state.read()) shown = snap->config.rgb ? Source::Color : Source::Ir;
            if (force_pending && shown != target)
                apply(target, std::string("mode: always ") + autopass::to_string(target), now, false);
            force_pending = false;
            last_reason = std::string("mode: ") + autopass::to_string(mode);
        } else if (active) {
            if (state.path().empty() && now - last_state_retry_ms > 5000) {
                last_state_retry_ms = now;
                state = autopass::PassthroughState();
            }
            std::optional<double> colour_light;
            std::optional<double> ir_light;
            if (const auto snap = state.read()) {
                shown = snap->config.rgb ? Source::Color : Source::Ir;
                // The IR camera's light value counts only from a new frame:
                // a stalled XRService leaves the last one in place.
                if (shown == Source::Ir && snap->mono_light && snap->mono_timestamp > mono_ts_at_last_light) {
                    mono_ts_at_last_light = snap->mono_timestamp;
                    ir_light = *snap->mono_light;
                }
                if (shown != policy.source() && now > expect_source_until_ms) {
                    log(std::string("source changed outside autopassd to ") + autopass::to_string(shown));
                    policy.adopt(shown);
                    evidence.reset();
                }
                if (snap->colour) {
                    const bool fresh = last_colour_ts >= 0 && snap->colour->timestamp != last_colour_ts;
                    last_colour_ts = snap->colour->timestamp;
                    if (shown == Source::Color && fresh) colour_light = snap->colour->light;
                }
            }

            want_ir = shown == Source::Ir && evidence.wants_ir_light(now, xs.emitters);
            if (want_ir && ir_light) {
                recent_ir.push_back({now, *ir_light});
                while (!recent_ir.empty() && now - recent_ir.front().ms > 10000) recent_ir.pop_front();
                if (now - last_ir_log_ms >= 10000) {
                    last_ir_log_ms = now;
                    char msg[96];
                    std::snprintf(msg, sizeof msg, "IR light %.2f, emitters %s, tracking %s", *ir_light,
                                  tri(xs.emitters, "on", "off"), tri(xs.tracking_lost, "lost", "ok"));
                    log(msg);
                }
            }

            // Emitter timing (log only): the colour camera knows dark from lit;
            // in IR, the IR light value is the first sign of light.
            const auto saw = [&](bool lit) {
                auto& mark = lit ? lit_seen_ms : dark_seen_ms;
                if (!mark) mark = now;
                (lit ? dark_seen_ms : lit_seen_ms).reset();
            };
            if (colour_light) {
                if (*colour_light < cfg.sensors.colour_dark_light) saw(false);
                else if (*colour_light >= cfg.sensors.colour_min_light) saw(true);
            }
            if (ir_light) {
                if (*ir_light >= cfg.sensors.ir_min_light) saw(true);
                else lit_seen_ms.reset();  // not lit (yet); "dark" is the colour camera's call
            }

            // Tracking loss does not hold anything: in a dark room tracking
            // can drop the moment the light goes out and stay lost while the
            // headset is still (2026-10-01 21:43-21:53), and neither the
            // emitters nor the IR light value depend on it (FINDINGS.md 41).
            const autopass::EvidenceResult er = evidence.evaluate(now, shown, xs.emitters, colour_light, ir_light);
            ev = er.evidence;
            last_cause = er.cause;
            const auto d = policy.update(now, ev, er.why, er.urgent);
            if (d.changed) apply(d.source, d.reason, now);
            if (g_verbose && d.reason != last_reason && d.reason != "missing reading") log("decision: " + d.reason);
            last_reason = d.reason;
        }
        // Next wake-up. Idle, or nothing that could change our mind: sleep
        // until XRService logs something.
        const bool pending = last_reason == "confirming" || last_reason == "confirmed, waiting for cooldown" ||
                             last_reason == "settling after switch" || last_reason == "missing reading";

        // Colour shown: read the colour camera's light value once a second
        // (XRService's "emitters on" line wakes autopassd at once when it gets
        // dark). IR shown: read the IR camera's light value 4 times a second
        // while it matters, otherwise wait for XRService's log.
        if (!active || mode != autopass::Mode::Auto) wait_ms = -1;
        else if (pending) wait_ms = 250;
        else if (want_ir) wait_ms = 250;
        else if (shown == Source::Color) wait_ms = 1000;
        else wait_ms = -1;

        char buf[512];
        std::snprintf(buf, sizeof buf,
            "{\"pid\": %d, \"passthrough\": \"%s\", \"standby\": \"%s\", \"showing\": \"%s\", "
            "\"ir_emitters\": \"%s\", \"tracking\": \"%s\", \"evidence\": \"%s\", \"cooldown_s\": %.0f, "
            "\"can_switch\": %s, \"blocked\": \"%s\", \"reason\": \"%s\", \"version\": \"%s\", \"mode\": \"%s\"}\n",
            static_cast<int>(::getpid()), tri(xs.passthrough, "shown", "hidden"), tri(xs.standby, "yes", "no"),
            autopass::to_string(policy.source()), tri(xs.emitters, "on", "off"), tri(xs.tracking_lost, "lost", "ok"),
            autopass::to_string(ev), policy.cooldown_ms() / 1000.0, blocked.empty() ? "true" : "false",
            blocked.c_str(), json_escape(active ? last_reason : "").c_str(), autopass::version(),
            autopass::to_string(mode));
        if (buf != last_status) {
            autopass::write_file_atomic(autopass::status_path(), buf);
            last_status = buf;
        }
    }

    if (config_watch >= 0) ::close(config_watch);
    xr.stop();
    log("autopassd stopped");
    if (g_log) std::fclose(g_log);
    return 0;
}
