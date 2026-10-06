// autopass: install, inspect and support autopassd.
//
//   autopass install          install the systemd user unit that starts autopassd
//                           with SteamVR and stops it with SteamVR; start it
//   autopass uninstall [--purge]
//                           stop and remove the unit and the launcher entry,
//                           switch back to colour; --purge also deletes the
//                           programs, settings and logs
//   autopass mode [auto|colour|ir]
//                           show or set the passthrough mode
//   autopass status           autopassd's status and whether the unit is active
//   autopass update           download and install the latest release
//   autopass report           write ~/frame-autopass-report.txt for a bug report
//   autopass version          print the version
//   autopass guard [reset]    show or clear the crash guard (XRService restarts
//                           soon after autopassd switches)
//   autopass state [SECONDS]  passive: camera config and colour light value
//                           from shared memory, no SteamVR calls
//   autopass get              read the camera config (private interface)
//   autopass set rgb|mono [--quiet]
//                           switch the camera source (used by autopassd). Exit
//                           codes: 0 done or already so, 1 error, 3 SteamVR's
//                           interface changed, 4 camera not enabled, 5 no
//                           Arcturus colour module.
//
// `get` and `set` connect to SteamVR as a background client for a few
// milliseconds; that never starts SteamVR and does not wake the headset.

#include "app_paths.hpp"
#include "light_policy.hpp"
#include "passthrough_state.hpp"
#include "private_camera.hpp"

#include <openvr.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

constexpr const char* kUnit = "frame-autopass.service";
constexpr const char* kInstallScript = "https://github.com/bod09/frame-autopass/releases/latest/download/install.sh";

int usage() {
    std::fprintf(stderr,
        "usage: autopass install | uninstall [--purge] | status | mode [auto|colour|ir] |\n"
        "                update | report | version | guard [reset] | state [SECONDS] | get |\n"
        "                set rgb|mono [--quiet]\n");
    return 2;
}

std::string data_home() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    return xdg && *xdg == '/' ? xdg : std::string(home ? home : "/tmp") + "/.local/share";
}

// The settings panel's entry in the Frame's app launcher. The launcher hides
// entries with Terminal=true, and needs an absolute Exec path. The file is
// named after the panel's application ID so KDE's taskbar can match the
// running window to it (and show its icon).
constexpr const char* kAppId = "io.github.bod09.FrameAutopass";
std::string desktop_path() { return data_home() + "/applications/" + kAppId + ".desktop"; }
// Name used before 0.2.0, removed on install and uninstall.
std::string old_desktop_path() { return data_home() + "/applications/frame-autopass.desktop"; }

// The icon, installed into the user's icon theme under the app ID: shells
// that look icons up by name (the Frame's VR taskbar) cannot use a path.
std::string theme_icon_path() { return data_home() + "/icons/hicolor/scalable/apps/" + kAppId + ".svg"; }

// Asks KDE to re-read app entries so new icons show without a restart.
void refresh_app_cache() {
    std::system("command -v kbuildsycoca6 >/dev/null 2>&1 && kbuildsycoca6 >/dev/null 2>&1");
}

std::string desktop_text() {
    const std::string dir = autopass::install_dir();
    return "[Desktop Entry]\n"
           "Type=Application\n"
           "Name=Frame Autopass\n"
           "Comment=Settings only: switching runs in the background with SteamVR\n"
           "Exec=" + dir + "/autopass-settings\n"
           "Icon=" + std::string(kAppId) + "\n"
           "Terminal=false\n"
           "StartupWMClass=" + std::string(kAppId) + "\n"
           "Categories=Settings;Utility;\n";
}

std::string unit_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    const std::string base = xdg && *xdg == '/' ? xdg : std::string(home ? home : "/tmp") + "/.config";
    return base + "/systemd/user/" + kUnit;
}

// Runs `systemctl --user ARGS...`, returns its exit code. Always against
// the login session's user manager: Desktop Mode on the Frame runs Konsole
// in a nested session whose XDG_RUNTIME_DIR and D-Bus cannot reach it.
int systemctl(std::initializer_list<const char*> args, bool quiet = false) {
    const std::string run = "/run/user/" + std::to_string(::getuid());
    std::string cmd = "XDG_RUNTIME_DIR=" + run + " DBUS_SESSION_BUS_ADDRESS=unix:path=" + run + "/bus systemctl --user";
    for (const char* a : args) cmd += std::string(" ") + a;
    if (quiet) cmd += " >/dev/null 2>&1";
    const int rc = std::system(cmd.c_str());
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
}

std::string unit_text() {
    return std::string(
        "# Installed by `autopass install`. Starts autopassd with SteamVR and stops it\n"
        "# with SteamVR. Remove with `autopass uninstall`.\n"
        "[Unit]\n"
        "Description=Adaptive passthrough (colour in good light, IR in the dark)\n"
        "After=steamvr.service\n"
        "BindsTo=steamvr.service\n"
        "\n"
        "[Service]\n"
        "Type=simple\n"
        "ExecStart=") + autopass::install_dir() + "/autopassd\n"
        "Restart=on-failure\n"
        "RestartSec=5\n"
        "Slice=session.slice\n"
        "Nice=10\n"
        "\n"
        "[Install]\n"
        "WantedBy=steamvr.service\n";
}

int install() {
    const std::string dir = autopass::install_dir();
    if (::access((dir + "/autopassd").c_str(), X_OK) != 0 || ::access((dir + "/autopass").c_str(), X_OK) != 0) {
        std::fprintf(stderr, "autopassd and autopass must be in %s first\n", dir.c_str());
        return 1;
    }
    if (!autopass::colour_module_present()) {
        std::fprintf(stderr,
            "The Arcturus Vision colour module was not found (no arcimx616 camera in\n"
            "/sys/class/video4linux). frame-autopass switches between that module and the\n"
            "built-in IR cameras, so it needs the module attached. Not installing.\n");
        return 1;
    }
    const std::string path = unit_path();
    if (!autopass::make_dirs(path.substr(0, path.rfind('/'))) || !autopass::write_file_atomic(path, unit_text())) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return 1;
    }
    if (systemctl({"daemon-reload"}) != 0 || systemctl({"enable", "--now", kUnit}) != 0) {
        std::fprintf(stderr, "systemctl failed; see `systemctl --user status %s`\n", kUnit);
        return 1;
    }
    std::printf("installed %s: autopassd now starts and stops with SteamVR\n", path.c_str());
    if (::access((dir + "/autopass-settings").c_str(), X_OK) == 0) {
        const std::string entry = desktop_path();
        std::remove(old_desktop_path().c_str());
        std::string icon;
        const std::string theme_icon = theme_icon_path();
        if (!autopass::read_file(dir + "/autopass.svg", &icon) ||
            !autopass::make_dirs(theme_icon.substr(0, theme_icon.rfind('/'))) ||
            !autopass::write_file_atomic(theme_icon, icon))
            std::fprintf(stderr, "could not install the icon to %s\n", theme_icon.c_str());
        if (autopass::make_dirs(entry.substr(0, entry.rfind('/'))) && autopass::write_file_atomic(entry, desktop_text())) {
            refresh_app_cache();
            std::printf("added Frame Autopass to the app launcher\n");
        }
        else
            std::fprintf(stderr, "could not write %s (the service is installed anyway)\n", entry.c_str());
    }
    return 0;
}

int switch_source(bool want_rgb, bool quiet);

// Deletes a directory tree we created (rm -rf on a fixed path of ours).
void remove_tree(const std::string& path) {
    if (path.size() < 10 || path.find("frame-autopass") == std::string::npos) return;  // never anything else
    const std::string cmd = "rm -rf '" + path + "'";
    if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "could not remove %s\n", path.c_str());
}

int uninstall(bool purge) {
    systemctl({"disable", "--now", kUnit}, true);
    std::remove(unit_path().c_str());
    systemctl({"daemon-reload"}, true);
    std::remove(desktop_path().c_str());
    std::remove(old_desktop_path().c_str());
    std::remove(theme_icon_path().c_str());
    refresh_app_cache();
    std::printf("removed %s and the launcher entry\n", kUnit);
    // Leave the headset showing colour, the stock behaviour with the module.
    switch_source(true, true);
    if (purge) {
        remove_tree(autopass::config_dir());
        remove_tree(autopass::install_dir());
        std::printf("removed %s and %s: frame-autopass is gone\n", autopass::install_dir().c_str(),
                    autopass::config_dir().c_str());
    }
    return 0;
}

int mode(const char* value) {
    std::string text;
    if (!value) {
        const bool set = autopass::read_file(autopass::mode_path(), &text);
        std::printf("%s\n", set && !text.empty() ? text.substr(0, text.find('\n')).c_str() : "auto");
        return 0;
    }
    autopass::Mode m;
    if (!autopass::parse_mode(value, &m)) {
        std::fprintf(stderr, "mode must be auto, colour or ir\n");
        return 2;
    }
    if (!autopass::make_dirs(autopass::config_dir()) ||
        !autopass::write_file_atomic(autopass::mode_path(), std::string(autopass::to_string(m)) + "\n")) {
        std::fprintf(stderr, "cannot write %s\n", autopass::mode_path().c_str());
        return 1;
    }
    std::printf("mode: %s\n", autopass::to_string(m));
    return 0;
}

int status() {
    std::string text;
    std::printf("unit: %s, %s\n", systemctl({"is-enabled", "--quiet", kUnit}, true) == 0 ? "installed" : "not installed",
                systemctl({"is-active", "--quiet", kUnit}, true) == 0 ? "running" : "not running");
    std::printf("version: %s\n", autopass::version());
    if (autopass::read_file(autopass::status_path(), &text)) {
        std::printf("status: %s", text.c_str());
        if (text.find("\"blocked\": \"steamvr-changed\"") != std::string::npos)
            std::printf("\nSteamVR has changed the camera interface frame-autopass uses, so it has\n"
                        "stopped switching (passthrough itself works normally). Run `autopass update`;\n"
                        "if that does not help, a new release is needed.\n");
        else if (text.find("\"blocked\": \"no-colour-module\"") != std::string::npos)
            std::printf("\nThe Arcturus Vision colour module is not attached, so there is nothing to\n"
                        "switch to; it resumes when the module is back.\n");
        else if (text.find("\"blocked\": \"crash-guard\"") != std::string::npos)
            std::printf("\nXRService restarted twice soon after a switch, so switching is paused as a\n"
                        "precaution. `autopass guard reset`, then restart SteamVR, to resume.\n");
    }
    if (autopass::read_file(autopass::crash_guard_path(), &text) && !text.empty())
        std::printf("crash guard entries (switching stops at 2):\n%s", text.c_str());
    return 0;
}

int guard(bool reset) {
    std::string text;
    const bool present = autopass::read_file(autopass::crash_guard_path(), &text) && !text.empty();
    if (reset) {
        std::remove(autopass::crash_guard_path().c_str());
        std::printf("crash guard cleared; restart autopassd (or SteamVR) to resume switching\n");
        return 0;
    }
    std::printf(present ? "crash guard entries:\n%s" : "crash guard: no entries\n", text.c_str());
    return 0;
}

int state(double seconds) {
    const autopass::PassthroughState st;
    if (st.path().empty()) {
        std::fprintf(stderr, "passthrough state file not found in /dev/shm\n");
        return 1;
    }
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    do {
        const auto s = st.read();
        if (!s) {
            std::fprintf(stderr, "cannot read %s\n", st.path().c_str());
            return 1;
        }
        std::printf("config: %s", s->config.describe().c_str());
        if (s->colour) std::printf(" | colour light %.3f", s->colour->light);
        std::printf(" | newest mono %.3f colour %.3f\n", s->mono_timestamp, s->colour_timestamp);
        if (seconds > 0) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } while (std::chrono::steady_clock::now() < end);
    return 0;
}

int update() {
    std::printf("fetching %s\n", kInstallScript);
    const std::string cmd = std::string("curl -fsSL ") + kInstallScript + " | bash";
    const int rc = std::system(cmd.c_str());
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
}

// Appends a command's output to the report.
void section(std::string* out, const char* title, const std::string& cmd) {
    *out += std::string("\n== ") + title + " ==\n";
    if (FILE* p = ::popen((cmd + " 2>&1").c_str(), "r")) {
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) out->append(buf, n);
        ::pclose(p);
    }
}

int report() {
    const char* home = std::getenv("HOME");
    const std::string path = std::string(home ? home : "/tmp") + "/frame-autopass-report.txt";
    std::string out = std::string("frame-autopass report, version ") + autopass::version() + "\n";
    out += std::string("Arcturus colour module: ") + (autopass::colour_module_present() ? "found" : "NOT found") + "\n";
    section(&out, "date", "date");
    section(&out, "SteamOS", "grep -E '^(VERSION_ID|BUILD_ID)=' /etc/os-release");
    section(&out, "SteamVR", "cat /opt/steamvr/bin/version.txt; grep -m1 -E 'cv: version [0-9]' ~/.local/share/Steam/logs/vrserver.txt");
    section(&out, "unit", "XDG_RUNTIME_DIR=/run/user/" + std::to_string(::getuid()) +
                              " systemctl --user status --no-pager " + kUnit + " | head -5");
    section(&out, "status", "cat " + autopass::status_path());
    section(&out, "config", "cat " + autopass::conf_path() + " 2>/dev/null || echo '(defaults)'");
    section(&out, "crash guard", "cat " + autopass::crash_guard_path() + " 2>/dev/null || echo '(empty)'");
    section(&out, "autopassd log (last 300 lines)", "tail -n 300 " + autopass::log_path());
    section(&out, "XRService (last 150 relevant lines)",
            "grep -hE 'IREmitters|Passthrough cameras|UserPresence|IMUFallback\\] (En|Dis)|Transition to IMU' "
            "~/.local/share/Steam/logs/xrservice.txt | cut -c1-140 | tail -n 150");
    if (!autopass::write_file_atomic(path, out)) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return 1;
    }
    std::printf("wrote %s\nIt contains your SteamOS and SteamVR versions and recent logs (no personal\n"
                "data that I know of; have a look before sharing it). Attach it to a GitHub issue.\n",
                path.c_str());
    return 0;
}

struct Session {
    bool ok = false;
    Session() {
        vr::EVRInitError err = vr::VRInitError_None;
        vr::VR_Init(&err, vr::VRApplication_Background);
        ok = err == vr::VRInitError_None;
        if (!ok) std::fprintf(stderr, "SteamVR unavailable: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
    }
    ~Session() {
        if (ok) vr::VR_Shutdown();
    }
};

int get_config() {
    Session session;
    if (!session.ok) return 1;
    autopass::PrivateCamera camera;
    if (!camera.ok()) {
        std::fprintf(stderr, "%s\n", camera.error().c_str());
        return 3;
    }
    const auto c = camera.get();
    if (!c) {
        std::fprintf(stderr, "%s\n", camera.error().c_str());
        return 1;
    }
    std::printf("%s\n", c->describe().c_str());
    return 0;
}

int switch_source(bool want_rgb, bool quiet) {
    if (!autopass::colour_module_present()) {
        if (!quiet) std::fprintf(stderr, "Arcturus colour module not found; not changing anything\n");
        return 5;
    }
    Session session;
    if (!session.ok) return 1;
    autopass::PrivateCamera camera;
    if (!camera.ok()) {
        std::fprintf(stderr, "%s\n", camera.error().c_str());
        return 3;
    }
    auto c = camera.get();
    if (!c) {
        std::fprintf(stderr, "%s\n", camera.error().c_str());
        return 1;
    }
    if (!c->enabled) {
        if (!quiet) std::fprintf(stderr, "passthrough camera is not enabled; not changing anything\n");
        return 4;
    }
    if (c->rgb == static_cast<std::uint8_t>(want_rgb)) {
        if (!quiet) std::printf("already %s\n", want_rgb ? "colour" : "IR");
        return 0;
    }
    c->rgb = want_rgb ? 1 : 0;
    if (!camera.set(*c)) {
        std::fprintf(stderr, "%s\n", camera.error().c_str());
        return 1;
    }
    if (!quiet) std::printf("switched to %s\n", want_rgb ? "colour" : "IR");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];
    if (cmd == "install" && argc == 2) return install();
    if (cmd == "uninstall" && argc == 2) return uninstall(false);
    if (cmd == "uninstall" && argc == 3 && std::string(argv[2]) == "--purge") return uninstall(true);
    if (cmd == "mode" && argc <= 3) return mode(argc == 3 ? argv[2] : nullptr);
    if (cmd == "status" && argc == 2) return status();
    if (cmd == "update" && argc == 2) return update();
    if (cmd == "report" && argc == 2) return report();
    if ((cmd == "version" || cmd == "--version") && argc == 2) return std::printf("%s\n", autopass::version()) < 0;
    if (cmd == "guard" && argc == 2) return guard(false);
    if (cmd == "guard" && argc == 3 && std::string(argv[2]) == "reset") return guard(true);
    if (cmd == "state" && argc <= 3) return state(argc == 3 ? std::atof(argv[2]) : 0);
    if (cmd == "get" && argc == 2) return get_config();
    if (cmd == "set" && (argc == 3 || (argc == 4 && std::string(argv[3]) == "--quiet"))) {
        const std::string which = argv[2];
        if (which != "rgb" && which != "mono") return usage();
        return switch_source(which == "rgb", argc == 4);
    }
    return usage();
}
