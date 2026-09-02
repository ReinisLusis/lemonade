#include <lemon/job/discovery.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <lemon/job/record.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <afunix.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace lemon {
namespace job {

namespace {

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

// The per-user configuration location each OS already designates. Inventing a
// dotfile somewhere else is the habit this project keeps complaining about in
// other tools.
fs::path user_config_dir() {
#ifdef _WIN32
    const std::string appdata = env("APPDATA");
    if (!appdata.empty()) return fs::path(appdata);
#elif defined(__APPLE__)
    const std::string home = env("HOME");
    if (!home.empty()) return fs::path(home) / "Library" / "Application Support";
#else
    const std::string xdg = env("XDG_CONFIG_HOME");
    if (!xdg.empty()) return fs::path(xdg);
    const std::string home = env("HOME");
    if (!home.empty()) return fs::path(home) / ".config";
#endif
    return {};
}

Json read_json(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream body;
    body << in.rdbuf();
    try {
        return Json::parse(body.str());
    } catch (const std::exception&) {
        return {};  // a malformed config must never take the server down
    }
}

std::string json_string(const Json& j, const char* key) {
    if (!j.is_object() || !j.contains(key) || !j[key].is_string()) return {};
    return j[key].get<std::string>();
}

// "30s", "5m", "1m30s" -> seconds. Zero when it cannot be read, which the
// caller turns into a conservative default rather than treating as instant.
double parse_duration(const std::string& s) {
    double total = 0, value = 0;
    std::string digits;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.') {
            digits.push_back(s[i]);
            continue;
        }
        if (digits.empty()) continue;
        value = std::atof(digits.c_str());
        digits.clear();
        switch (s[i]) {
            case 'h': total += value * 3600; break;
            case 'm': total += (i + 1 < s.size() && s[i + 1] == 's') ? value / 1000 : value * 60; break;
            case 's': total += value; break;
            default: break;
        }
    }
    return total;
}

}  // namespace

std::string machine_store() {
    // An environment override first, so a test or a container can redirect one
    // run without editing a file other processes are reading.
    const std::string from_env = env("ABSTRACTION_STORE");
    if (!from_env.empty()) return from_env;

    const fs::path dir = user_config_dir();
    if (dir.empty()) return {};

    const Json cfg = read_json(dir / "abstraction" / "config.json");
    const std::string store = json_string(cfg, "store");
    if (!store.empty()) return store;

    // Nothing configured. The default is where a supervisor would put one, so a
    // machine with jobd running and no config file still works.
    const std::string home = env(
#ifdef _WIN32
        "USERPROFILE"
#else
        "HOME"
#endif
    );
    if (home.empty()) return {};
    const fs::path fallback = fs::path(home) / ".abstraction";
    return fs::exists(fallback) ? fallback.string() : std::string();
}

Supervisor supervisor_of(const std::string& store_root) {
    Supervisor out;
    if (store_root.empty()) return out;

    const Json hb = read_json(fs::path(store_root) / "supervisor.json");
    if (!hb.is_object()) return out;

    out.owner = json_string(hb, "owner");
    out.tier = json_string(hb, "tier");

    const std::string seen = json_string(hb, "seen");
    if (seen.empty()) return out;

    double every = parse_duration(json_string(hb, "every"));
    if (every <= 0) every = 30.0;

    // Three intervals: enough to survive a slow sweep or a jittering clock,
    // short enough that a killed supervisor stops attracting work in a minute
    // or two rather than forever.
    const auto age = Clock::now() - parse_rfc3339(seen);
    const double seconds = std::chrono::duration<double>(age).count();
    out.alive = seconds >= 0 && seconds <= 3 * every;
    return out;
}

void nudge(const std::string& store_root) {
    if (store_root.empty()) return;
    const std::string path = (fs::path(store_root) / "supervisor.sock").string();
    if (path.size() >= sizeof(sockaddr_un{}.sun_path)) return;

#ifdef _WIN32
    WSADATA wsa;
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
    SOCKET fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) {
        ::WSACleanup();
        return;
    }
#else
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
#endif

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());

    // Every failure here is fine. Nobody listening, a stale socket, a platform
    // that will not do this: the supervisor sweeps on its own timer regardless.
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        const char msg[] = "look\n";
        ::send(fd, msg, static_cast<int>(sizeof(msg) - 1), 0);
    }

#ifdef _WIN32
    ::closesocket(fd);
    ::WSACleanup();
#else
    ::close(fd);
#endif
}

}  // namespace job
}  // namespace lemon
