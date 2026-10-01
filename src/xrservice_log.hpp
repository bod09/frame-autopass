#pragma once

// Follows XRService's session log for the few facts autopassd needs
// (FINDINGS.md sections 29 and 33). Lines, as logged on the headset:
//
//   [DeckardCaptureSource] Passthrough cameras resumed | paused
//       passthrough is being shown | not shown (every toggle, and standby)
//   [UserPresence] Received onEnterStandby | onLeaveStandby
//   [IREmitters] IR Emitters Turned On | Off
//       XRService's own "too dark for the cameras" judgement
//   Transition to IMUFallback | [IMUFallback] Disabled
//       visual tracking lost | back
//
// ~/.local/share/Steam/logs/xrservice.txt is a symlink to the current
// session log. start() reads the log once for the latest state, then
// inotify delivers only appended bytes. XRService writes nothing while the
// headset is idle, so an idle autopassd is never woken. A new session log (the
// symlink changes, also across stop/start) means XRService restarted.

#include <optional>
#include <string>

namespace autopass {

struct XrState {
    std::optional<bool> passthrough;     // cameras resumed (shown) / paused
    std::optional<bool> standby;         // headset in standby
    std::optional<bool> emitters;        // IR emitters on
    std::optional<bool> tracking_lost;   // IMU fallback active
};

// Applies one log line to `state`. Returns true if it was a line we track.
bool apply_xrservice_line(const std::string& line, XrState* state);
// Applies every complete or trailing line of `text` in order.
void apply_xrservice_text(const std::string& text, XrState* state);

class XrServiceLog {
public:
    explicit XrServiceLog(std::string logs_dir = default_logs_dir());
    ~XrServiceLog();
    XrServiceLog(const XrServiceLog&) = delete;
    XrServiceLog& operator=(const XrServiceLog&) = delete;

    static std::string default_logs_dir();

    bool start();  // false with error() if the log cannot be opened
    void stop();
    bool running() const { return inotify_fd_ >= 0; }
    int fd() const { return inotify_fd_; }
    const std::string& error() const { return error_; }

    // Handles pending inotify events without blocking. Returns true if any
    // tracked state changed.
    bool update();
    const XrState& state() const { return state_; }

    // True once after XRService started a new session log since the one
    // seen before, including across stop()/start(). Cleared by reading.
    bool take_restarted() {
        const bool r = restarted_;
        restarted_ = false;
        return r;
    }
    // Session start (Unix time) parsed from the log path, or 0.
    long long session_start() const { return session_start_; }
    static long long parse_session_start(const std::string& path);

private:
    bool open_log();
    void read_new_bytes();

    std::string logs_dir_;
    std::string log_path_;
    int inotify_fd_ = -1;
    int dir_watch_ = -1;
    int file_watch_ = -1;
    int file_fd_ = -1;
    long long offset_ = 0;
    std::string partial_;
    XrState state_;
    bool restarted_ = false;
    long long session_start_ = 0;
    std::string error_;
};

} // namespace autopass
