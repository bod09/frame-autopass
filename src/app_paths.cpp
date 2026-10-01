#include "app_paths.hpp"

#include <dirent.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace autopass {
namespace {

std::string home() {
    const char* h = std::getenv("HOME");
    return h && *h ? h : "/tmp";
}

std::string xdg(const char* var, const char* fallback) {
    const char* v = std::getenv(var);
    if (v && *v == '/') return v;
    return home() + fallback;
}

} // namespace

std::string config_dir() { return xdg("XDG_CONFIG_HOME", "/.config") + "/frame-autopass"; }
std::string install_dir() { return xdg("XDG_DATA_HOME", "/.local/share") + "/frame-autopass"; }

bool make_dirs(const std::string& path) {
    std::string partial;
    std::stringstream ss(path);
    std::string part;
    if (!path.empty() && path[0] == '/') partial = "/";
    while (std::getline(ss, part, '/')) {
        if (part.empty()) continue;
        partial += part + "/";
        if (::mkdir(partial.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

bool write_file_atomic(const std::string& path, const std::string& contents) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!(f << contents) || !f.flush()) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool read_file(const std::string& path, std::string* contents) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    *contents = ss.str();
    return true;
}

bool colour_module_present() {
    const std::string base = "/sys/class/video4linux";
    DIR* dir = ::opendir(base.c_str());
    if (!dir) return false;
    bool found = false;
    while (const dirent* e = ::readdir(dir)) {
        if (e->d_name[0] == '.') continue;
        std::string name;
        if (read_file(base + "/" + e->d_name + "/name", &name) && name.find("arcimx616") != std::string::npos) {
            found = true;
            break;
        }
    }
    ::closedir(dir);
    return found;
}

#ifndef AUTOPASS_VERSION
#define AUTOPASS_VERSION "dev"
#endif
const char* version() { return AUTOPASS_VERSION; }

} // namespace autopass
