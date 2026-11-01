#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <poll.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace cleaner {
extern volatile std::sig_atomic_t interrupted;
constexpr int dirFlags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
struct Fd {
    int n;
    explicit Fd(int value) : n(value) { if (n < 0) throw std::runtime_error(std::strerror(errno)); }
    ~Fd() { if (n >= 0) close(n); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : n(other.n) { other.n = -1; }
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) { if (n >= 0) close(n); n = other.n; other.n = -1; }
        return *this;
    }
};
struct Rule { std::string id, label; std::vector<std::string> roots; };
const std::vector<Rule> rules = {
    {"firefox", "Firefox cache", {".cache/mozilla/firefox"}},
    {"chrome", "Chrome cache", {}}, {"brave", "Brave cache", {}},
    {"code", "VS Code cache", {".config/Code/Cache", ".config/Code/CachedData",
        ".config/Code/GPUCache", ".config/Code/DawnCache", ".config/Code/Code Cache"}},
    {"graphics", "Thumbnails and shader cache", {".cache/thumbnails", ".cache/mesa_shader_cache",
        ".cache/mesa_shader_cache_db", ".cache/mesa_shader_cache_sf", ".cache/gnome-desktop-thumbnailer"}},
    {"npm", "npm download cache", {".npm/_cacache"}},
    {"pip", "pip download cache", {".cache/pip"}},
    {"flatpak", "Flatpak app caches", {}}
};
using Bytes = unsigned long long;
std::string human(Bytes bytes) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes); int unit = 0;
    while (value >= 1024 && unit < 4) { value /= 1024; ++unit; }
    std::ostringstream out; out << std::fixed << std::setprecision(1) << value << ' ' << units[unit];
    return out.str();
}
std::string display(std::string text) {
    for (char& c : text) if (static_cast<unsigned char>(c) < 32 || c == 127) c = '?';
    return text;
}
struct Entry { std::string path; struct stat info; };
struct Target {
    std::string group, path; dev_t device; ino_t inode;
    std::vector<Entry> files; Bytes size = 0;
};
struct Result { size_t count = 0, skipped = 0; Bytes bytes = 0; };
bool sameFile(const struct stat& a, const struct stat& b) {
    return S_ISREG(b.st_mode) && b.st_nlink == 1 && a.st_uid == b.st_uid &&
        a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
        a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
        a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
class Engine {
    Fd homeFd;
    dev_t device;
    bool allowed(const struct stat& s) const { return s.st_dev == device && s.st_uid == getuid(); }
public:
    std::string home;
    size_t skipped = 0;
    explicit Engine(const std::string& path) : homeFd(open(path.c_str(), dirFlags)), home(path) {
        struct stat s{};
        if (path.empty() || path[0] != '/' || fstat(homeFd.n, &s) || s.st_uid != getuid())
            throw std::runtime_error("HOME must be an absolute directory owned by you.");
        struct stat root{};
        if (stat("/", &root) || (s.st_dev == root.st_dev && s.st_ino == root.st_ino))
            throw std::runtime_error("Refusing to use / as HOME.");
        device = s.st_dev;
    }
    Fd openDir(const std::string& path, int base = -1) const {
        Fd fd(dup(base < 0 ? homeFd.n : base));
        if (path.empty()) return fd;
        std::istringstream in(path); std::string part;
        if (path.front() == '/' || path.back() == '/') throw std::runtime_error("Invalid cache path");
        while (std::getline(in, part, '/')) {
            if (part.empty() || part == "." || part == "..") throw std::runtime_error("Invalid cache path");
            Fd next(openat(fd.n, part.c_str(), dirFlags));
            struct stat s{};
            if (fstat(next.n, &s) || !allowed(s)) throw std::runtime_error("Foreign owner/filesystem");
            fd = std::move(next);
        }
        return fd;
    }
    static std::vector<std::string> names(int fd) {
        int copy = dup(fd);
        if (copy < 0) throw std::runtime_error("Cannot duplicate directory");
        DIR* dir = fdopendir(copy);
        if (!dir) { close(copy); throw std::runtime_error("Cannot read directory"); }
        std::vector<std::string> list;
        rewinddir(dir);
        errno = 0;
        while (dirent* item = readdir(dir)) {
            std::string name(item->d_name);
            if (name != "." && name != "..") list.push_back(name);
            errno = 0;
        }
        int failure = errno; closedir(dir);
        if (failure) throw std::runtime_error("Directory read failed");
        std::sort(list.begin(), list.end()); return list;
    }
    std::vector<std::string> children(const std::string& path) const {
        try { Fd fd = openDir(path); return names(fd.n); }
        catch (const std::runtime_error&) { return {}; }
    }
    std::vector<std::pair<std::string, std::string>> paths() const {
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto& r : rules) for (const auto& p : r.roots) out.emplace_back(r.id, p);
        for (const auto& browser : std::vector<std::pair<std::string, std::string>>{
            {"chrome", ".config/google-chrome"}, {"brave", ".config/BraveSoftware/Brave-Browser"},
            {"chrome", ".cache/google-chrome"}, {"brave", ".cache/BraveSoftware/Brave-Browser"}}) {
            for (const auto& profile : children(browser.second)) {
                if (profile != "Default" && profile.rfind("Profile ", 0) != 0) continue;
                for (const auto& name : {"Cache", "Code Cache", "GPUCache", "DawnCache"})
                    out.emplace_back(browser.first, browser.second + '/' + profile + '/' + name);
            }
            for (const auto& name : {"ShaderCache", "GrShaderCache"})
                out.emplace_back(browser.first, browser.second + '/' + name);
        }
        for (const auto& app : children(".var/app")) out.emplace_back("flatpak", ".var/app/" + app + "/cache");
        return out;
    }
    void walk(int fd, const std::string& prefix, Target& target, unsigned depth = 0) {
        if (depth > 128) { ++skipped; return; }
        for (const auto& name : names(fd)) {
            struct stat s{};
            std::string rel = prefix.empty() ? name : prefix + '/' + name;
            if (fstatat(fd, name.c_str(), &s, AT_SYMLINK_NOFOLLOW) || !allowed(s)) { ++skipped; continue; }
            if (S_ISREG(s.st_mode) && s.st_nlink == 1) {
                target.files.push_back({rel, s}); target.size += static_cast<Bytes>(s.st_blocks) * 512;
            } else if (S_ISDIR(s.st_mode)) {
                try { Fd child = openDir(name, fd); walk(child.n, rel, target, depth + 1); }
                catch (const std::runtime_error&) { ++skipped; }
            } else ++skipped;
        }
    }
    std::vector<Target> scan() {
        skipped = 0; std::vector<Target> out;
        for (const auto& pair : paths()) {
            try {
                Fd fd = openDir(pair.second); struct stat s{};
                if (fstat(fd.n, &s)) { ++skipped; continue; }
                Target t{pair.first, pair.second, s.st_dev, s.st_ino, {}, 0};
                walk(fd.n, "", t);
                if (!t.files.empty()) out.push_back(std::move(t));
            } catch (const std::runtime_error&) { ++skipped; }
        }
        return out;
    }
    Result clean(const std::vector<Target>& targets) const {
        Result result;
        for (const auto& t : targets) {
            try {
                Fd root = openDir(t.path); struct stat s{};
                if (fstat(root.n, &s) || s.st_dev != t.device || s.st_ino != t.inode) {
                    result.skipped += t.files.size(); continue;
                }
                for (const auto& entry : t.files) {
                    if (interrupted) return result;
                    try {
                        size_t slash = entry.path.rfind('/');
                        std::string parent = slash == std::string::npos ? "" : entry.path.substr(0, slash);
                        std::string name = slash == std::string::npos ? entry.path : entry.path.substr(slash + 1);
                        Fd fd = openDir(parent, root.n); struct stat current{};
                        if (fstatat(fd.n, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) ||
                            !sameFile(entry.info, current) || unlinkat(fd.n, name.c_str(), 0)) {
                            ++result.skipped; continue;
                        }
                        ++result.count; result.bytes += static_cast<Bytes>(entry.info.st_blocks) * 512;
                    } catch (const std::runtime_error&) { ++result.skipped; }
                }
            } catch (const std::runtime_error&) { result.skipped += t.files.size(); }
        }
        return result;
    }
    Bytes freeBytes() const {
        struct statvfs s{};
        return fstatvfs(homeFd.n, &s) ? 0 : static_cast<Bytes>(s.f_bavail) * s.f_frsize;
    }
};
std::map<std::string, Bytes> totals(const std::vector<Target>& targets) {
    std::map<std::string, Bytes> out;
    for (const auto& r : rules) out[r.id] = 0;
    for (const auto& t : targets) out[t.group] += t.size;
    return out;
}
void report(const Engine& engine, const std::vector<Target>& targets) {
    std::cout << "TrpCleaner / 42 | " << display(engine.home) << "\nFilesystem free: "
        << human(engine.freeBytes()) << " (account quota may differ)\nRead-only preview; allocated-block estimates.\n\n";
    auto sizes = totals(targets);
    for (const auto& r : rules) std::cout << std::left << std::setw(10) << r.id << std::right
        << std::setw(13) << human(sizes[r.id]) << "  " << r.label << '\n';
    Bytes sum = 0;
    for (const auto& t : targets) { sum += t.size; std::cout << "  " << human(t.size) << "  ~/" << display(t.path) << '\n'; }
    std::cout << "\nCache total: " << human(sum) << "\n";
}
class Terminal {
    struct termios old{};
public:
    Terminal() {
        if (tcgetattr(STDIN_FILENO, &old)) throw std::runtime_error("Use --plain in this terminal");
        auto raw = old;
        raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
        // Keep ISIG enabled so Ctrl-C works; restore via a signal-safe handler.
        raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw)) throw std::runtime_error("Cannot set terminal mode");
        std::cout << "\033[?1049h\033[?25l" << std::flush;
    }
    ~Terminal() { tcsetattr(STDIN_FILENO, TCSAFLUSH, &old); std::cout << "\033[?25h\033[?1049l" << std::flush; }
};
} // namespace cleaner

// Signal handlers only set a flag; normal C++ unwinding restores the terminal.
namespace cleaner {
volatile std::sig_atomic_t interrupted = 0;
void onSignal(int) { interrupted = 1; }
int readKey() {
    while (!interrupted) {
        struct pollfd p{STDIN_FILENO, POLLIN, 0};
        int rc = poll(&p, 1, 100);
        if (rc < 0 && errno != EINTR) return -1;
        if (rc > 0) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1) return static_cast<unsigned char>(c);
            return -1;
        }
    }
    return -1;
}
int menuKey() {
    int key = readKey();
    if (key != 27) return key;
    struct pollfd p{STDIN_FILENO, POLLIN, 0};
    if (poll(&p, 1, 100) <= 0) return 27;
    int next = readKey();
    if (next != '[' && next != 'O') return 0;
    int arrow = readKey();
    if (arrow == 'A') return 'k';
    if (arrow == 'B') return 'j';
    if (arrow == 'C') return ']';
    if (arrow == 'D') return '[';
    return 0;
}
struct Canvas {
    int rows = 24, cols = 80;
    Canvas() {
        struct winsize size{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) {
            if (size.ws_row) rows = size.ws_row;
            if (size.ws_col) cols = size.ws_col;
        }
        std::cout << "\033[H\033[2J";
    }
    void line(int row, const std::string& text, const char* color = "") const {
        if (row < 1 || row > rows) return;
        auto safe = display(text);
        std::cout << "\033[" << row << ";1H\033[2K" << color
            << safe.substr(0, static_cast<size_t>(std::max(0, cols - 2))) << "\033[0m";
    }
    bool ready() const {
        if (rows >= 18 && cols >= 64) return true;
        line(1, "Resize terminal to at least 64 x 18. q quits.");
        std::cout << std::flush; return false;
    }
};
constexpr const char* orange = "\033[1;38;5;208m";
constexpr const char* muted = "\033[90m";
constexpr const char* highlight = "\033[48;5;236m\033[38;5;208m";
bool showPaths(const std::vector<Target>& targets, const std::string& title, bool confirm) {
    size_t offset = 0;
    Bytes sum = 0; size_t count = 0;
    for (const auto& t : targets) { sum += t.size; count += t.files.size(); }
    while (!interrupted) {
        Canvas screen;
        if (screen.ready()) {
            screen.line(1, " TrpCleaner / 42                         v3.0", orange);
            screen.line(3, " " + title, "\033[1m");
            screen.line(4, " " + human(sum) + " / " + std::to_string(count) + " files / "
                + std::to_string(targets.size()) + " cache directories", muted);
            size_t visible = static_cast<size_t>(std::max(1, screen.rows - 12));
            size_t maxOffset = targets.size() > visible ? targets.size() - visible : 0;
            offset = std::min(offset, maxOffset);
            for (size_t i = 0; i < visible && offset + i < targets.size(); ++i) {
                const auto& t = targets[offset + i];
                screen.line(6 + static_cast<int>(i), " " + human(t.size) + "  ~/" + t.path);
            }
            if (targets.empty()) screen.line(6, " No supported cache files found.", muted);
            screen.line(screen.rows - 5, " Protected: projects, Downloads, extensions and profile data.", muted);
            screen.line(screen.rows - 4, confirm ? " Close selected apps. Cache deletion is permanent." : " Details only. No files have been deleted.", orange);
            screen.line(screen.rows - 2, confirm ? " y delete these cache files    n / Esc cancel" : " Enter / Esc return");
            screen.line(screen.rows - 1, " j/k or arrows scroll cache paths", muted);
            std::cout << std::flush;
        }
        int key = menuKey();
        if (key == -1 || key == 'q' || key == 'n' || key == 27) return false;
        if (confirm && key == 'y' && screen.rows >= 18 && screen.cols >= 64) return true;
        if (!confirm && (key == '\n' || key == '\r')) return false;
        if (key == 'j' && offset + 1 < targets.size()) ++offset;
        if (key == 'k' && offset) --offset;
    }
    return false;
}
std::vector<std::string> menuGroups(int tab) {
    if (tab == 1) return {"firefox", "chrome", "brave"};
    if (tab == 2) return {"code", "npm", "pip"};
    if (tab == 3) return {"graphics", "flatpak"};
    std::vector<std::string> out{"all"};
    for (const auto& r : rules) out.push_back(r.id);
    return out;
}
std::string label(const std::string& id) {
    if (id == "all") return "Clean all supported caches";
    for (const auto& r : rules) if (r.id == id) return r.label;
    return id;
}
void tui(Engine& engine) {
    Terminal terminal;
    int tab = 0; size_t cursor = 0;
    auto targets = engine.scan();
    std::string status = "Enter previews the highlighted row. a cleans all caches.";
    while (!interrupted) {
        auto groups = menuGroups(tab); cursor = std::min(cursor, groups.size() - 1);
        auto sizes = totals(targets); Bytes total = 0;
        for (const auto& t : targets) total += t.size;
        Canvas screen;
        if (screen.ready()) {
            screen.line(1, " TrpCleaner / 42                         v3.0", orange);
            std::string tabs;
            const char* titles[] = {"All caches", "Browsers", "Dev tools", "Apps"};
            for (int i = 0; i < 4; ++i) tabs += " [" + std::to_string(i + 1) + "] " + titles[i] + (i == tab ? " * " : "   ");
            screen.line(3, tabs, "\033[1m");
            screen.line(4, " Cache: " + human(total) + "  /  Filesystem free: " + human(engine.freeBytes()), muted);
            size_t visible = static_cast<size_t>(screen.rows - 13);
            size_t start = cursor >= visible ? cursor - visible + 1 : 0;
            for (size_t n = 0; n < visible && start + n < groups.size(); ++n) {
                size_t i = start + n; const auto& id = groups[i];
                std::ostringstream row;
                row << (i == cursor ? " > " : "   ") << std::left << std::setw(32) << label(id)
                    << std::right << std::setw(12) << human(id == "all" ? total : sizes[id]);
                screen.line(6 + static_cast<int>(n), row.str(), i == cursor ? highlight : "");
            }
            screen.line(screen.rows - 7, groups[cursor] == "all" ? " All supported cache categories / one confirmation" : " " + groups[cursor] + " / Enter shows paths / c cleans this category", muted);
            screen.line(screen.rows - 6, " " + std::to_string(cursor + 1) + "/" + std::to_string(groups.size()) + "  " + label(groups[cursor]));
            screen.line(screen.rows - 4, " " + status, orange);
            screen.line(screen.rows - 2, " Enter details  c clean row  a clean all  r rescan  q quit");
            screen.line(screen.rows - 1, " 1-4 tabs  arrows/j/k navigate  h help", muted);
            std::cout << std::flush;
        }
        int key = menuKey();
        if (key == -1 || key == 'q' || key == 27) return;
        if (screen.rows < 18 || screen.cols < 64) continue;
        if (key == 'j' && cursor + 1 < groups.size()) ++cursor;
        else if (key == 'k' && cursor) --cursor;
        else if (key >= '1' && key <= '4') { tab = key - '1'; cursor = 0; }
        else if (key == ']' || key == '[') { tab = (tab + (key == ']' ? 1 : 3)) % 4; cursor = 0; }
        else if (key == 'r') { targets = engine.scan(); status = "Scan complete. Cache sizes refreshed."; }
        else if (key == 'h') {
            status = "Enter details / c clean row / a ALL tabs / r refresh / q quit.";
        } else if (key == 'a' || key == 'c' || key == '\r' || key == '\n') {
            bool all = key == 'a' || groups[cursor] == "all";
            bool cleanAction = key == 'a' || key == 'c' || groups[cursor] == "all";
            std::string id = all ? "all" : groups[cursor];
            // Every action takes a fresh snapshot, even in a long-running TUI.
            targets = engine.scan(); std::vector<Target> chosen;
            for (const auto& t : targets) if (all || t.group == id) chosen.push_back(t);
            if (chosen.empty()) { status = "No cache files to clean in " + label(id) + "."; continue; }
            if (showPaths(chosen, label(id), cleanAction) && !interrupted) {
                auto result = engine.clean(chosen);
                status = "Cleaned " + human(result.bytes) + " / " + std::to_string(result.count)
                    + " files. Skipped " + std::to_string(result.skipped) + ".";
                targets = engine.scan();
            } else status = cleanAction ? "Cleanup cancelled. Nothing deleted." : "Returned from details.";
        }
    }
}
void help() {
    std::cout << "Usage: TrpCleaner [--scan | --plain | --clean GROUP...]\n"
        << "No arguments: campus-style action menu. --scan: read-only report.\n"
        << "--plain: numbered menu. --clean: preselect groups, preview and confirm.\n"
        << "Groups:";
    for (const auto& r : rules) std::cout << ' ' << r.id;
    std::cout << "\nInstall: make install (defaults to ~/.local/bin)\n";
}
int run(int argc, char** argv) {
    std::string mode; std::set<std::string> selected;
    if (argc > 1) {
        mode = argv[1];
        if (mode == "--help" || mode == "-h") { help(); return 0; }
        if (mode != "--scan" && mode != "--plain" && mode != "--clean") { help(); return 2; }
        if (mode != "--clean" && argc > 2) { help(); return 2; }
        if (mode == "--clean") {
            if (argc < 3) { help(); return 2; }
            for (int i = 2; i < argc; ++i) {
                std::string id = argv[i];
                if (std::none_of(rules.begin(), rules.end(), [&](const Rule& r) { return r.id == id; })) {
                    std::cerr << "Unknown group: " << display(id) << '\n'; return 2;
                }
                selected.insert(id);
            }
        }
    }
    if (geteuid() == 0 || getuid() == 0) throw std::runtime_error("Run as your normal user, without sudo.");
    const char* home = std::getenv("HOME");
    if (!home || !*home) throw std::runtime_error("HOME is not set.");
    Engine engine(home); auto targets = engine.scan();
    if (mode == "--scan" || !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        report(engine, targets);
        if (mode != "--scan") std::cout << "An interactive terminal is required for cleanup; nothing deleted.\n";
        return 0;
    }
    if (mode.empty()) { tui(engine); return interrupted ? 130 : 0; }
    else if (mode == "--plain") {
        report(engine, targets);
        for (size_t i = 0; i < rules.size(); ++i) std::cout << i + 1 << ". " << rules[i].label << '\n';
        std::cout << "0. Clean all supported caches\nChoose one number (Enter cancels): " << std::flush;
        std::string line, token; std::getline(std::cin, line); std::istringstream in(line);
        if (in >> token) {
            std::string extra;
            if (token.size() != 1 || token[0] < '0' || token[0] > '8' || (in >> extra)) {
                std::cout << "Invalid selection; nothing deleted.\n"; return 2;
            }
            if (token == "0") for (const auto& r : rules) selected.insert(r.id);
            else selected.insert(rules[static_cast<size_t>(token[0] - '1')].id);
        }
    }
    if (interrupted) return 130;
    std::vector<Target> chosen; Bytes sum = 0;
    for (const auto& t : targets) if (selected.count(t.group)) { chosen.push_back(t); sum += t.size; }
    if (chosen.empty()) { std::cout << "No cache files selected; nothing deleted.\n"; return 0; }
    std::cout << "\nSelected cache directories:\n";
    for (const auto& t : chosen) std::cout << "  " << human(t.size) << "  ~/" << display(t.path) << '\n';
    std::cout << "\nEstimated total: " << human(sum)
        << "\nClose these apps first. Deletion is permanent; caches rebuild.\nType CLEAN to delete these cache files: " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer) || interrupted || answer != "CLEAN") {
        std::cout << "Cancelled; nothing deleted.\n"; return 0;
    }
    auto result = engine.clean(chosen);
    std::cout << "Deleted " << result.count << " files (" << human(result.bytes)
        << " in allocated blocks).\nSkipped " << result.skipped << " changed or inaccessible files.\n"
        << "Empty cache directories kept. Actual reclaimed space may differ.\n";
    if (interrupted) {
        std::cout << "Stopped; completed deletions remain applied.\n";
        return 130;
    }
    return 0;
}
} // namespace cleaner
#ifndef CLEANER_TEST
int main(int argc, char** argv) {
    try {
        std::signal(SIGINT, cleaner::onSignal); std::signal(SIGTERM, cleaner::onSignal);
        return cleaner::run(argc, argv);
    } catch (const std::exception& e) { std::cerr << "Error: " << cleaner::display(e.what()) << '\n'; return 1; }
}
#endif
