#include "xrservice_log.hpp"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace autopass {
namespace {

constexpr const char* kLink = "xrservice.txt";
// The session log is a few hundred KB; read at most this much of its tail
// when looking for the latest state.
constexpr long long kMaxInitialRead = 16LL * 1024 * 1024;

} // namespace

std::string XrServiceLog::default_logs_dir() {
    const char* home = std::getenv("HOME");
    return std::string(home && *home ? home : "/tmp") + "/.local/share/Steam/logs";
}

XrServiceLog::XrServiceLog(std::string logs_dir) : logs_dir_(std::move(logs_dir)) {}

XrServiceLog::~XrServiceLog() { stop(); }

bool apply_xrservice_line(const std::string& line, XrState* s) {
    const auto has = [&line](const char* text) { return line.find(text) != std::string::npos; };
    if (has("[DeckardCaptureSource] Passthrough cameras resumed")) s->passthrough = true;
    else if (has("[DeckardCaptureSource] Passthrough cameras paused")) s->passthrough = false;
    else if (has("[UserPresence] Received onEnterStandby")) s->standby = true;
    else if (has("[UserPresence] Received onLeaveStandby")) s->standby = false;
    else if (has("[IREmitters] IR Emitters Turned On")) s->emitters = true;
    else if (has("[IREmitters] IR Emitters Turned Off")) s->emitters = false;
    else if (has("[IREmitters] IR emitters mode changed to Auto")) s->emitters = false;
    else if (has("Transition to IMUFallback")) s->tracking_lost = true;
    else if (has("[IMUFallback] Disabled")) s->tracking_lost = false;
    else return false;
    return true;
}

void apply_xrservice_text(const std::string& text, XrState* state) {
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        apply_xrservice_line(text.substr(start, end - start), state);
        start = end + 1;
    }
}

long long XrServiceLog::parse_session_start(const std::string& path) {
    int y, mo, d, h, mi, s;
    const auto slash = path.rfind("/XRService-");
    if (slash == std::string::npos || slash < 11) return 0;
    const auto dir = path.rfind("XRService-", slash - 1);
    if (dir == std::string::npos) return 0;
    if (std::sscanf(path.c_str() + dir, "XRService-%d.%d.%d/XRService-%d-%d-%d.log", &y, &mo, &d, &h, &mi, &s) != 6)
        return 0;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    tm.tm_isdst = -1;
    return static_cast<long long>(std::mktime(&tm));
}

void XrServiceLog::stop() {
    if (file_fd_ >= 0) ::close(file_fd_);
    if (inotify_fd_ >= 0) ::close(inotify_fd_);
    file_fd_ = inotify_fd_ = -1;
    dir_watch_ = file_watch_ = -1;
    partial_.clear();
}

bool XrServiceLog::start() {
    stop();
    inotify_fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd_ < 0) {
        error_ = std::string("inotify: ") + std::strerror(errno);
        return false;
    }
    dir_watch_ = ::inotify_add_watch(inotify_fd_, logs_dir_.c_str(), IN_CREATE | IN_MOVED_TO);
    if (!open_log()) {
        stop();
        return false;
    }
    return true;
}

bool XrServiceLog::open_log() {
    if (file_fd_ >= 0) ::close(file_fd_);
    if (file_watch_ >= 0) ::inotify_rm_watch(inotify_fd_, file_watch_);
    file_fd_ = file_watch_ = -1;
    partial_.clear();

    char resolved[PATH_MAX];
    const std::string link = logs_dir_ + "/" + kLink;
    if (!::realpath(link.c_str(), resolved)) {
        error_ = "cannot resolve " + link + ": " + std::strerror(errno);
        return false;
    }
    // A different session log than the last one seen (even across a
    // stop/start) means XRService restarted in between.
    if (!log_path_.empty() && log_path_ != resolved) restarted_ = true;
    log_path_ = resolved;
    session_start_ = parse_session_start(log_path_);
    file_fd_ = ::open(log_path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (file_fd_ < 0) {
        error_ = "cannot open " + log_path_ + ": " + std::strerror(errno);
        return false;
    }
    file_watch_ = ::inotify_add_watch(inotify_fd_, log_path_.c_str(), IN_MODIFY);

    // Initial state from the tail of the log.
    struct stat st{};
    ::fstat(file_fd_, &st);
    const long long size = st.st_size;
    const long long from = size > kMaxInitialRead ? size - kMaxInitialRead : 0;
    std::string text(static_cast<std::size_t>(size - from), '\0');
    long long got = 0;
    while (got < size - from) {
        const auto n = ::pread(file_fd_, text.data() + got, static_cast<std::size_t>(size - from - got), from + got);
        if (n <= 0) break;
        got += n;
    }
    text.resize(static_cast<std::size_t>(got));
    // Everything we track comes from this session's log.
    state_ = {};
    apply_xrservice_text(text, &state_);
    offset_ = from + got;
    error_.clear();
    return true;
}

void XrServiceLog::read_new_bytes() {
    struct stat st{};
    if (file_fd_ < 0 || ::fstat(file_fd_, &st) != 0) return;
    if (st.st_size < offset_) offset_ = 0;  // truncated
    std::vector<char> buf(64 * 1024);
    for (;;) {
        const auto n = ::pread(file_fd_, buf.data(), buf.size(), offset_);
        if (n <= 0) break;
        offset_ += n;
        partial_.append(buf.data(), static_cast<std::size_t>(n));
        std::size_t start = 0, nl;
        while ((nl = partial_.find('\n', start)) != std::string::npos) {
            apply_xrservice_line(partial_.substr(start, nl - start), &state_);
            start = nl + 1;
        }
        partial_.erase(0, start);
    }
}

bool XrServiceLog::update() {
    if (inotify_fd_ < 0) return false;
    const XrState before = state_;
    bool file_changed = false, relink = false;
    alignas(inotify_event) char buf[4096];
    for (;;) {
        const auto n = ::read(inotify_fd_, buf, sizeof buf);
        if (n <= 0) break;
        for (char* p = buf; p < buf + n;) {
            const auto* e = reinterpret_cast<const inotify_event*>(p);
            if (e->wd == file_watch_) file_changed = true;
            if (e->wd == dir_watch_ && e->len && std::strcmp(e->name, kLink) == 0) relink = true;
            p += sizeof(inotify_event) + e->len;
        }
    }
    if (relink) {
        char resolved[PATH_MAX];
        const std::string link = logs_dir_ + "/" + kLink;
        if (::realpath(link.c_str(), resolved) && log_path_ != resolved) open_log();
    }
    if (file_changed) read_new_bytes();
    return state_.passthrough != before.passthrough || state_.standby != before.standby ||
           state_.emitters != before.emitters || state_.tracking_lost != before.tracking_lost;
}

} // namespace autopass
