#pragma once

// Where autopassd and autopass keep their files. Everything lives in the user's
// home directory; nothing is written to system locations.

#include <string>

namespace autopass {

// ~/.config/frame-autopass (honours XDG_CONFIG_HOME).
std::string config_dir();
// ~/.local/share/frame-autopass (honours XDG_DATA_HOME): the
// installed binaries (autopassd, autopass).
std::string install_dir();

inline std::string status_path() { return config_dir() + "/status.json"; }
inline std::string log_path() { return config_dir() + "/autopassd.log"; }
inline std::string conf_path() { return config_dir() + "/autopass.conf"; }
inline std::string lock_path() { return config_dir() + "/autopassd.lock"; }
// Crash guard: XRService restarts soon after an autopassd switch, one line each.
// Passthrough mode chosen in the settings panel or `autopass mode`: one
// word, auto (default), colour or ir. autopassd watches it with inotify.
inline std::string mode_path() { return config_dir() + "/mode"; }
inline std::string crash_guard_path() { return config_dir() + "/crash_guard"; }

// Creates the directory and its parents. Returns false on failure.
bool make_dirs(const std::string& path);
// Writes a file atomically (temporary file, then rename).
bool write_file_atomic(const std::string& path, const std::string& contents);
// Reads a whole small file; returns false if it cannot be read.
bool read_file(const std::string& path, std::string* contents);

// Whether the Arcturus Vision colour module is attached: its sensors
// (arcimx616) are listed by name in /sys/class/video4linux. Only reads
// sysfs names; never opens a camera device.
bool colour_module_present();

// The release version, set at build time (Makefile VERSION).
const char* version();

} // namespace autopass
