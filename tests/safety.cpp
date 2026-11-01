#define CLEANER_TEST
#include "../src/main.cpp"
#include <filesystem>
#include <fstream>
#include <functional>

namespace fs = std::filesystem;
void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
struct Fixture {
    fs::path home;
    Fixture() {
        char pattern[] = "/tmp/session-cleaner-test-XXXXXX";
        char* path = mkdtemp(pattern);
        if (!path) throw std::runtime_error("mkdtemp failed");
        home = path;
    }
    ~Fixture() { std::error_code ec; fs::remove_all(home, ec); }
    fs::path put(const std::string& relative, const std::string& text = "cache") {
        auto p = home / relative; fs::create_directories(p.parent_path());
        std::ofstream out(p); out << text; out.close();
        require(!out.fail(), "Fixture write failed"); return p;
    }
};
int main(int argc, char** argv) {
    std::signal(SIGINT, cleaner::onSignal); std::signal(SIGTERM, cleaner::onSignal);
    // Optional pseudo-terminal fixture entry point; never opens real HOME.
    if (argc == 2 && std::string(argv[1]) == "--tui") {
        Fixture f; f.put(".cache/pip/data"); cleaner::Engine engine(f.home.string());
        auto outside = f.put("Downloads/keep");
        cleaner::tui(engine);
        std::cout << "Fixture cache remaining: " << fs::exists(f.home / ".cache/pip/data") << '\n';
        std::cout << "Protected fixture preserved: " << fs::exists(outside) << '\n';
        return cleaner::interrupted ? 130 : 0;
    }
    std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"preview and protected files", [] {
            Fixture f; auto cache = f.put(".config/google-chrome/Default/Cache/data");
            std::vector<fs::path> protectedFiles;
            for (auto p : {".config/google-chrome/Default/Cookies", ".config/google-chrome/Default/Bookmarks",
                ".config/Code/User/settings.json", ".vscode/extensions/tool", "Downloads/file",
                "project/.git/objects/object", ".codex/sessions/chat"}) protectedFiles.push_back(f.put(p));
            cleaner::Engine engine(f.home.string()); auto targets = engine.scan();
            require(fs::exists(cache), "scan deleted a cache file");
            auto r = engine.clean(targets);
            require(r.count == 1 && !fs::exists(cache), "cache was not cleaned");
            for (const auto& p : protectedFiles) require(fs::exists(p), "protected file deleted");
        }},
        {"symlinked cache ancestor", [] {
            Fixture f; auto keep = f.put("goinfre/firefox/data"); fs::create_directory(f.home / ".cache");
            fs::create_directory_symlink(f.home / "goinfre", f.home / ".cache/mozilla");
            cleaner::Engine engine(f.home.string()); require(engine.scan().empty(), "followed symlink");
            require(fs::exists(keep), "removed external data");
        }},
        {"nested symlinks and hardlinks", [] {
            Fixture f; auto keep = f.put("Downloads/keep"); auto cache = f.home / ".cache/pip";
            fs::create_directories(cache); fs::create_symlink(keep, cache / "link");
            fs::create_directory_symlink(keep.parent_path(), cache / "folder");
            fs::create_hard_link(keep, cache / "hardlink");
            cleaner::Engine engine(f.home.string()); require(engine.scan().empty(), "included links");
            require(fs::exists(keep), "removed external data");
        }},
        {"changed and newly created files", [] {
            Fixture f; auto changed = f.put(".cache/pip/data"); cleaner::Engine engine(f.home.string());
            auto targets = engine.scan(); f.put(".cache/pip/data", "changed since preview");
            auto added = f.put(".cache/pip/new"); auto r = engine.clean(targets);
            require(r.count == 0 && r.skipped == 1, "removed changed file");
            require(fs::exists(changed) && fs::exists(added), "changed/new file missing");
        }},
        {"root cache swapped for symlink", [] {
            Fixture f; f.put(".cache/pip/data"); auto keep = f.put("goinfre/data");
            cleaner::Engine engine(f.home.string()); auto targets = engine.scan();
            auto cache = f.home / ".cache/pip"; fs::rename(cache, f.home / ".cache/old");
            fs::create_directory_symlink(keep.parent_path(), cache);
            require(engine.clean(targets).count == 0 && fs::exists(keep), "followed replacement root");
        }},
        {"nested cache swapped for symlink", [] {
            Fixture f; f.put(".cache/pip/http/data"); auto keep = f.put("goinfre/data");
            cleaner::Engine engine(f.home.string()); auto targets = engine.scan();
            auto sub = f.home / ".cache/pip/http"; fs::rename(sub, f.home / ".cache/pip/old");
            fs::create_directory_symlink(keep.parent_path(), sub);
            require(engine.clean(targets).count == 0 && fs::exists(keep), "followed replacement ancestor");
        }},
        {"replacement cache inode rejected", [] {
            Fixture f; f.put(".cache/pip/data"); cleaner::Engine engine(f.home.string()); auto targets = engine.scan();
            fs::rename(f.home / ".cache/pip", f.home / ".cache/old");
            auto keep = f.put(".cache/pip/data", "replacement");
            require(engine.clean(targets).count == 0 && fs::exists(keep), "accepted replacement root");
        }},
        {"Flatpak cache scope", [] {
            Fixture f; f.put(".var/app/com.spotify.Client/cache/data");
            auto keep = f.put(".var/app/com.spotify.Client/config/settings");
            cleaner::Engine engine(f.home.string()); auto targets = engine.scan();
            require(targets.size() == 1 && targets[0].group == "flatpak", "wrong scope");
            require(engine.clean(targets).count == 1 && fs::exists(keep), "Flatpak scope violated");
        }},
        {"invalid relative paths rejected", [] {
            Fixture f; cleaner::Engine engine(f.home.string());
            for (auto p : {"../elsewhere", "/tmp", "a/../b", "a//b", "a/"}) {
                bool rejected = false;
                try { auto fd = engine.openDir(p); } catch (const std::runtime_error&) { rejected = true; }
                require(rejected, "invalid path accepted");
            }
        }}
    };
    size_t passed = 0;
    for (const auto& test : tests) {
        try { test.second(); ++passed; std::cout << "PASS " << test.first << '\n'; }
        catch (const std::exception& e) { std::cerr << "FAIL " << test.first << ": " << e.what() << '\n'; return 1; }
    }
    std::cout << passed << " safety tests passed.\n";
}
