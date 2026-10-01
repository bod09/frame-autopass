// Host-side test for XrServiceLog against a fake Steam logs directory:
// initial state from an existing log, appended lines (including one split
// across two writes), unrelated noise, and an XRService restart that
// replaces the xrservice.txt symlink.

#include "xrservice_log.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <ctime>
#include <string>
#include <unistd.h>

using autopass::XrServiceLog;

namespace {

int failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL line %d: %s\n", __LINE__, #expr); ++failures; } } while (0)

const char* kAuto = "Wed Sep 30 2026 22:39:15.067651 INFO: SLAMConsole: [IREmitters] IR emitters mode changed to Auto\n";
const char* kOn = "Wed Sep 30 2026 22:40:49.267834 INFO: SLAMConsole: [IREmitters] IR Emitters Turned On\n";
const char* kOff = "Wed Sep 30 2026 22:41:24.230440 INFO: SLAMConsole: [IREmitters] IR Emitters Turned Off\n";
const char* kNoise = "Wed Sep 30 2026 22:39:23.152963 WARNING: [DeckardCaptureSource] Max number of iteration reached\n";

void append(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::app | std::ios::binary);
    f << text;
}

} // namespace

int main() {
    char tmpl[] = "/tmp/autopass_emitter_XXXXXX";
    const std::string dir = ::mkdtemp(tmpl);
    const std::string log1 = dir + "/XRService-1.log", log2 = dir + "/XRService-2.log", link = dir + "/xrservice.txt";
    const std::string log3 = dir + "/XRService-3.log";

    append(log1, std::string(kAuto) + kNoise + kOn + kNoise);
    CHECK(::symlink(log1.c_str(), link.c_str()) == 0);

    XrServiceLog w(dir);
    CHECK(w.start());
    CHECK(w.state().emitters == std::optional<bool>(true));  // last line in the existing log
    CHECK(!w.update());                               // nothing new
    CHECK(!w.take_restarted());

    append(log1, kNoise);
    CHECK(!w.update());
    CHECK(w.state().emitters == std::optional<bool>(true));

    // A line written in two parts is only parsed once complete.
    const std::string off = kOff;
    append(log1, off.substr(0, 40));
    w.update();
    CHECK(w.state().emitters == std::optional<bool>(true));
    append(log1, off.substr(40));
    CHECK(w.update());
    CHECK(w.state().emitters == std::optional<bool>(false));

    // XRService restarts: a new session log, symlink replaced atomically.
    append(log2, std::string(kAuto) + kNoise);
    const std::string tmp_link = dir + "/xrservice.txt.new";
    CHECK(::symlink(log2.c_str(), tmp_link.c_str()) == 0);
    CHECK(std::rename(tmp_link.c_str(), link.c_str()) == 0);
    w.update();
    CHECK(w.state().emitters == std::optional<bool>(false));  // "mode changed to Auto" with no On: off
    CHECK(w.take_restarted());
    CHECK(!w.take_restarted());
    append(log2, kOn);
    CHECK(w.update());
    CHECK(w.state().emitters == std::optional<bool>(true));
    // The old log is no longer followed.
    append(log1, kOff);
    w.update();
    CHECK(w.state().emitters == std::optional<bool>(true));

    // A restart while the watcher is stopped (passthrough off) is still
    // noticed on the next start.
    w.stop();
    CHECK(!w.running());
    append(log3, kAuto);
    const std::string tmp3 = dir + "/xrservice.txt.3";
    CHECK(::symlink(log3.c_str(), tmp3.c_str()) == 0);
    CHECK(std::rename(tmp3.c_str(), link.c_str()) == 0);
    CHECK(w.start());
    CHECK(w.take_restarted());
    // ...but not when it is the same session.
    w.stop();
    CHECK(w.start());
    CHECK(!w.take_restarted());
    w.stop();
    std::remove(log3.c_str());
    std::remove(link.c_str());
    std::remove(log1.c_str());
    std::remove(log2.c_str());
    ::rmdir(dir.c_str());

    // Session start from the real path layout.
    const long long t0 = XrServiceLog::parse_session_start(
        "/home/steamos/.local/share/Steam/logs/XRService-2026.09.30/XRService-23-49-58.log");
    CHECK(t0 > 0);
    {
        const std::time_t tt = static_cast<std::time_t>(t0);
        const std::tm* lt = std::localtime(&tt);
        CHECK(lt->tm_year == 126 && lt->tm_mon == 8 && lt->tm_mday == 30 && lt->tm_hour == 23 && lt->tm_min == 49 &&
              lt->tm_sec == 58);
    }
    CHECK(XrServiceLog::parse_session_start("/tmp/whatever.log") == 0);

    // Passthrough and standby lines in the same stream.
    {
        char tmpl2[] = "/tmp/autopass_xrlog_XXXXXX";
        const std::string d2 = ::mkdtemp(tmpl2);
        const std::string lg = d2 + "/XRService-1.log", lk = d2 + "/xrservice.txt";
        append(lg, "x INFO: [DeckardCaptureSource] Passthrough cameras resumed\n");
        CHECK(::symlink(lg.c_str(), lk.c_str()) == 0);
        XrServiceLog x(d2);
        CHECK(x.start());
        CHECK(x.state().passthrough == std::optional<bool>(true));
        append(lg, "x INFO: [UserPresence] Received onEnterStandby from SteamVR.\nx INFO: [DeckardCaptureSource] Passthrough cameras paused\n");
        CHECK(x.update());
        CHECK(x.state().passthrough == std::optional<bool>(false) && x.state().standby == std::optional<bool>(true));
        x.stop();
        std::remove(lk.c_str());
        std::remove(lg.c_str());
        ::rmdir(d2.c_str());
    }

    // No log at all: start() fails cleanly.
    XrServiceLog missing("/nonexistent/autopass/logs");
    CHECK(!missing.start());
    CHECK(!missing.error().empty());

    if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
    std::printf("all XRService log tests passed\n");
    return 0;
}
