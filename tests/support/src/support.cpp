module;
#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
extern char** environ;
#endif
module mcpp.test.support;

import std;

namespace mcpp::test::support {

// ---- the machine -------------------------------------------------------------------------------

bool on_windows() noexcept {
#if defined(_WIN32)
    return true;
#else
    return false;
#endif
}
bool on_macos() noexcept {
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}
bool on_linux() noexcept {
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

std::string exe(std::string_view name) {
    return on_windows() ? std::string(name) + ".exe" : std::string(name);
}

std::optional<std::string> env(std::string_view name) {
    if (const char* v = std::getenv(std::string(name).c_str()))
        return std::string(v);
    return std::nullopt;
}

std::optional<fs::path> on_path(std::string_view name) {
    const auto path = env("PATH");
    if (!path)
        return std::nullopt;
    const char sep = on_windows() ? ';' : ':';
    for (auto part : std::views::split(std::string_view(*path), sep)) {
        const fs::path dir(std::string(part.begin(), part.end()));
        if (dir.empty())
            continue;
        std::error_code ec;
        for (const auto& candidate : {dir / name, dir / (std::string(name) + ".exe")})
            if (fs::is_regular_file(candidate, ec))
                return candidate;
    }
    return std::nullopt;
}

// ---- capabilities (tests/e2e/run_all.sh, "Build the set of capabilities") ----------------------

namespace {

fs::path home_dir() {
    if (auto h = env("MCPP_HOME"); h && !h->empty())
        return *h;
    return fs::path(env(on_windows() ? "USERPROFILE" : "HOME").value_or("")) / ".mcpp";
}

fs::path user_dir() { return env(on_windows() ? "USERPROFILE" : "HOME").value_or(""); }

// A payload under the registry of the home or of the user's xlings: `<xpkgs>/<pattern>`.
bool payload(std::string_view pattern, bool xlings_too = true) {
    if (!glob(home_dir() / "registry/data/xpkgs", pattern).empty())
        return true;
    return xlings_too && !glob(user_dir() / ".xlings/data/xpkgs", pattern).empty();
}

bool probe(std::string_view c) {
    const bool unix_like = on_linux() || on_macos();
    if (c == "x86_64-host") {
#if defined(__x86_64__) || defined(_M_X64)
        return true;
#else
        return false;
#endif
    }
    if (c == "elf") return on_linux();
    if (c == "unix-shell" || c == "fresh-sandbox" || c == "symlink") return unix_like;
    if (c == "macos") return on_macos();
    if (c == "windows") return on_windows();
    if (c == "gcc") return on_linux() && on_path("g++");
    if (c == "patchelf" || c == "pack") return on_linux() && on_path("patchelf");
    if (c == "musl")
        return on_linux() && (on_path("x86_64-linux-musl-g++") || payload("xim-x-musl-gcc/*/bin/x86_64-linux-musl-g++"));
    if (c == "mingw-cross") return on_linux() && payload("xim-x-mingw-cross-gcc/*/bin/x86_64-w64-mingw32-g++");
    if (c == "llvm") return on_linux() && payload("xim-x-llvm/*/bin/clang++", false);
    if (c == "mingw-host-headers") {
        if (!on_linux()) return false;
        if (fs::is_directory("/usr/x86_64-w64-mingw32/include")) return true;
        auto root = env("MCPP_E2E_HOST_MINGW_ROOT");
        return root && !root->empty() && fs::is_regular_file(fs::path(*root) / "x86_64-w64-mingw32/include/io.h");
    }
    if (c == "wine") return on_linux() && on_path("wine");
    if (c == "qemu-riscv") return on_linux() && on_path("qemu-system-riscv64");
    if (c == "qemu-arm") return on_linux() && (on_path("qemu-system-arm") || payload("xim-x-qemu-arm/*/bin/qemu-system-arm"));
    if (c == "android-ndk") return on_linux() && payload("xim-x-android-ndk/*/toolchains/llvm/prebuilt/*/bin/clang++");
    if (c == "android-device") {
        // An arm64 device (not an emulator) answering the platform-tools' adb.
        if (!on_linux()) return false;
        std::vector<fs::path> adbs = glob(user_dir() / ".xlings/data/xpkgs", "xim-x-android-platform-tools/*/adb");
        for (auto& a : glob(home_dir() / "registry/data/xpkgs", "xim-x-android-platform-tools/*/adb")) adbs.push_back(a);
        for (const auto& adb : adbs) {
            auto r = e2e::run({.argv = {adb.string(), "devices"}});
            for (auto line : std::views::split(std::string_view(r.out), '\n')) {
                std::string_view l(line.begin(), line.end());
                const auto tab = l.find('\t');
                if (tab == std::string_view::npos || l.starts_with("emulator-") || !l.substr(tab + 1).starts_with("device"))
                    continue;
                auto abi = e2e::run({.argv = {adb.string(), "shell", "getprop", "ro.product.cpu.abi"},
                                     .env = {{"ANDROID_SERIAL", std::string(l.substr(0, tab))}}});
                if (abi.out.starts_with("arm64-v8a")) return true;
            }
        }
        return false;
    }
    if (c == "msvc" || c == "no-msvc") {
        if (!on_windows()) return false;
        const fs::path vswhere = "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe";
        bool msvc = false;
        if (fs::is_regular_file(vswhere)) {
            auto r = e2e::run({.argv = {vswhere.string(), "-latest", "-products", "*", "-requires",
                                        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property",
                                        "installationPath"}});
            msvc = r.out.find_first_not_of(" \r\n") != std::string::npos;
        }
        return c == "msvc" ? msvc : !msvc;
    }
    if (c == "mingw") return on_windows() && payload("xim-x-mingw-gcc/*/bin/g++.exe");
    if (c == "python3") return on_path("python3").has_value();
    if (c == "jq") return on_path("jq").has_value();
    if (c == "nasm") return on_path("nasm") || payload("xim-x-nasm/*/nasm", false) || payload("xim-x-nasm/*/bin/nasm", false);
    if (c == "scan-deps")
        return on_path("clang-scan-deps") || payload("xim-x-llvm/*/bin/clang-scan-deps", false) ||
               payload("xim-x-llvm/*/bin/clang-scan-deps.exe", false);
    if (c == "import-std-libcxx") return payload("xim-x-llvm/*/share/libc++/v1/std.cppm", false);
    if (c == "xlings-msvc") return env("MCPP_E2E_XLINGS_MSVC").value_or("") == "1";
    mcpplibs::mtest::fail(std::format("unknown capability '{}' (run_all.sh's KNOWN_CAPS)", c));
}

}  // namespace

namespace {
// `sort -V` order of two versions: numbers as numbers, the rest as text.
bool version_less(std::string_view a, std::string_view b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
            std::size_t ie = i, je = j;
            while (ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie]))) ++ie;
            while (je < b.size() && std::isdigit(static_cast<unsigned char>(b[je]))) ++je;
            const auto x = std::stoull(std::string(a.substr(i, ie - i)));
            const auto y = std::stoull(std::string(b.substr(j, je - j)));
            if (x != y) return x < y;
            i = ie; j = je;
        } else {
            if (a[i] != b[j]) return a[i] < b[j];
            ++i; ++j;
        }
    }
    return a.size() - i < b.size() - j;
}
}  // namespace

std::optional<std::string> newest_payload(std::string_view package, std::string_view probe) {
    for (const auto& root : {home_dir() / "registry/data/xpkgs", user_dir() / ".xlings/data/xpkgs"}) {
        std::vector<std::string> versions;
        for (const auto& v : glob(root / package, "*"))
            if (probe.empty() || !glob(v, probe).empty())
                versions.push_back(v.filename().string());
        if (!versions.empty())
            return *std::ranges::max_element(versions, version_less);
    }
    return std::nullopt;
}

std::string family_version(std::string_view package, std::string_view override, std::string_view fallback) {
    if (auto v = env(override); v && !v->empty())
        return *v;
    // _toolchain_env.sh reads the home's registry only (USERPROFILE's on Windows when unset).
    std::vector<std::string> versions;
    static const std::regex numeric{R"(^[0-9]+(\.[0-9]+)*$)"};
    for (const auto& v : glob(home_dir() / "registry/data/xpkgs" / package, "*")) {
        const auto name = v.filename().string();
        if (std::regex_match(name, numeric))
            versions.push_back(name);
    }
    if (versions.empty())
        return std::string(fallback);
    return *std::ranges::max_element(versions, version_less);
}

fs::path payload_root(std::string_view package, std::string_view version) {
    const auto mine = home_dir() / "registry/data/xpkgs" / package / version;
    if (std::error_code ec; fs::exists(mine, ec))
        return mine;
    return user_dir() / ".xlings/data/xpkgs" / package / version;
}

bool has(std::string_view capability) {
    static std::mutex m;
    static std::map<std::string, bool, std::less<>> known;
    std::scoped_lock lock(m);
    if (auto it = known.find(capability); it != known.end())
        return it->second;
    const bool held = probe(capability);
    known.emplace(std::string(capability), held);
    return held;
}

void needs(std::initializer_list<std::string_view> capabilities) {
    for (auto c : capabilities)
        if (!has(c))
            mcpplibs::mtest::skip(std::format("missing capability: {}", c));
}

// ---- mcpp ----------------------------------------------------------------------------------------

fs::path repository() {
    // The directory holding tests/support/mcpp.toml: above this source file when the compiler
    // named it absolutely, else above the working directory (`mcpp test` runs in the package).
    const auto holds = [](const fs::path& d) {
        std::error_code ec;
        return fs::is_regular_file(d / "tests/support/mcpp.toml", ec);
    };
    std::vector<fs::path> starts;
    if (const fs::path here = std::source_location::current().file_name(); here.is_absolute())
        starts.push_back(here.parent_path());
    starts.push_back(fs::current_path());
    for (auto d : starts)
        for (; !d.empty(); d = d.parent_path()) {
            if (holds(d))
                return d;
            if (d == d.parent_path())
                break;
        }
    mcpplibs::mtest::fail("the mcpp repository was not found above this program");
}

std::string mcpp_path() {
    if (auto v = env("MCPP_BIN_MCPP"); v && !v->empty())
        return *v;
    if (auto v = env("MCPP"); v && !v->empty())
        return *v;
    return e2e::program("mcpp");
}

e2e::outcome mcpp(std::vector<std::string> args, const fs::path& cwd, const options& o) {
    return e2e::run(mcpp_command(std::move(args), cwd, o));
}

e2e::command mcpp_command(std::vector<std::string> args, const fs::path& cwd, const options& o) {
    e2e::command c;
    c.argv.push_back(mcpp_path());
    for (auto& a : args) c.argv.push_back(std::move(a));
    c.cwd = cwd;
    if (o.where != home::shared) {
        c.env.emplace_back("MCPP_HOME", case_home(o.where).string());
    } else if (!env("MCPP_HOME")) {
        if (auto u = env(on_windows() ? "USERPROFILE" : "HOME"))
            c.env.emplace_back("MCPP_HOME", (fs::path(*u) / ".mcpp").string());
    }
    for (const auto& kv : o.env) c.env.push_back(kv);
    c.input = o.input;
    c.timeout = o.timeout;
    return c;
}

std::string text(const e2e::outcome& r) { return r.out + r.err; }

fs::path shared_home() { return home_dir(); }

namespace {
void link_or_copy(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (fs::exists(fs::symlink_status(to, ec)))
        return;
    fs::create_directory_symlink(from, to, ec);
    if (ec)
        fs::copy(from, to, fs::copy_options::recursive, ec);
}
// _inherit_toolchain.sh's link_xpkg_payloads: each package directory of `from` is a directory of
// `to` whose versions are links; anything else is linked whole.
void link_payloads(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (!fs::is_directory(from, ec))
        return;
    fs::create_directories(to);
    for (const auto& e : fs::directory_iterator(from, ec)) {
        const auto there = to / e.path().filename();
        if (!e.is_directory(ec) || fs::is_symlink(fs::symlink_status(there, ec))) {
            link_or_copy(e.path(), there);
            continue;
        }
        fs::create_directories(there);
        for (const auto& v : fs::directory_iterator(e.path(), ec))
            link_or_copy(v.path(), there / v.path().filename());
    }
}
void inherit(const fs::path& h, bool whole) {
    const auto user = home_dir();
    const auto xlings = user_dir() / ".xlings/data/xpkgs";
    link_payloads(user / "registry/data/xpkgs", h / "registry/data/xpkgs");
    link_payloads(user / "xpkgs", h / "xpkgs");
    link_payloads(xlings, h / "registry/data/xpkgs");
    std::error_code ec;
    for (auto name : {"xim-pkgindex", "xim-index-repos"})
        if (fs::is_directory(user / "registry/data" / name, ec)) {
            fs::create_directories(h / "registry/data");
            link_or_copy(user / "registry/data" / name, h / "registry/data" / name);
        }
    if (whole && env("MCPP_INHERIT_SUBOS").value_or("1") != "0" && fs::is_directory(user / "registry/subos", ec))
        link_or_copy(user / "registry/subos", h / "registry/subos");
    if (whole && env("MCPP_INHERIT_CONFIG").value_or("1") != "0" && fs::is_regular_file(user / "config.toml", ec))
        fs::copy_file(user / "config.toml", h / "config.toml", fs::copy_options::overwrite_existing, ec);
    if (fs::is_directory(user / "bin", ec))
        link_or_copy(user / "bin", h / "bin");
}
}  // namespace

namespace {
// The directories of the cases (their own directory and their homes), made under the system's
// temporary directory. Not the runner's case directory: that lies under the repository's target/,
// and mcpp run in a directory with no manifest of its own would find the repository's mcpp.toml
// and build it; and a payload may hold its install directory in a fixed space (glibc's binaries
// hold 255 bytes), which a case directory under target/ exceeds where `mktemp -d` did not.
//
// Each is filed under the runner's directory of the case that made it (mtest::temp_dir(), which
// mtest removes when the case ends): a directory whose case has ended is removed by the next use
// of this registry, and the rest when the program ends.
struct case_dirs {
    std::mutex m;
    std::vector<std::pair<fs::path, fs::path>> made;  // (the case's runner directory, the directory)
    std::map<fs::path, fs::path> own;                 // the case's own directory, by its runner directory
    std::map<fs::path, fs::path> home;                // the case's home, by its runner directory
    ~case_dirs() {
        std::error_code ec;
        for (const auto& [_, d] : made) fs::remove_all(d, ec);
    }
    // Removes the directories of the cases that have ended. Called with `m` held.
    void sweep() {
        std::error_code ec;
        std::erase_if(made, [&](const auto& e) {
            if (fs::exists(e.first, ec)) return false;
            fs::remove_all(e.second, ec);
            own.erase(e.first);
            home.erase(e.first);
            return true;
        });
    }
    // A new directory `<temp>/<prefix>-XXXXXXXX` of the current case. Called with `m` held.
    fs::path make(std::string_view prefix) {
        const auto key = mcpplibs::mtest::temp_dir();
        sweep();
        std::random_device rd;
        fs::path d;
        std::error_code ec;
        do d = fs::temp_directory_path() / std::format("{}-{:08x}", prefix, rd());
        while (!fs::create_directory(d, ec) && !ec);
        made.emplace_back(key, d);
        return d;
    }
};
case_dirs& registry_of_cases() {
    static case_dirs h;
    return h;
}

void populate_home(const fs::path& h, home kind) {
    std::error_code ec;
    if (kind == home::linked && fs::is_directory(home_dir() / "registry", ec))
        link_or_copy(home_dir() / "registry", h / "registry");
    else if (kind == home::inherited || kind == home::payloads)
        inherit(h, kind == home::inherited);
}
}  // namespace

fs::path case_dir() {
    auto& all = registry_of_cases();
    const auto key = mcpplibs::mtest::temp_dir();
    std::scoped_lock lock(all.m);
    if (auto it = all.own.find(key); it != all.own.end() && fs::exists(it->second))
        return it->second;
    const auto d = all.make("mcpp-e2e-case");
    all.own[key] = d;
    return d;
}

fs::path case_home(home kind) {
    auto& all = registry_of_cases();
    const auto key = mcpplibs::mtest::temp_dir();
    std::scoped_lock lock(all.m);
    if (auto it = all.home.find(key); it != all.home.end() && fs::exists(it->second))
        return it->second;
    const auto h = all.make("mcpp-e2e-home");
    all.home[key] = h;
    populate_home(h, kind);
    return h;
}

fs::path another_home(home kind) {
    auto& all = registry_of_cases();
    std::scoped_lock lock(all.m);
    const auto h = all.make("mcpp-e2e-home");
    populate_home(h, kind);
    return h;
}

// ---- a project -----------------------------------------------------------------------------------

namespace {
void write_into(const fs::path& dir, std::string_view relative, std::string_view content) {
    const auto file = dir / fs::path(std::u8string(relative.begin(), relative.end()));
    if (file.has_parent_path())
        fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary).write(content.data(), static_cast<std::streamsize>(content.size()));
}
}  // namespace

project::project(files contents) : project(case_dir() / "project", contents) {}

project::project(fs::path dir, files contents) : dir_(std::move(dir)) {
    fs::create_directories(dir_);
    for (const auto& [path, content] : contents)
        write_into(dir_, path, content);
}

fs::path project::at(std::string_view relative) const {
    return dir_ / fs::path(std::u8string(relative.begin(), relative.end()));
}
void project::write(std::string_view relative, std::string_view content) const { write_into(dir_, relative, content); }
std::string project::read(std::string_view relative) const { return read_file(at(relative)); }
bool project::exists(std::string_view relative) const { return fs::exists(at(relative)); }
void project::remove(std::string_view relative) const { fs::remove_all(at(relative)); }

e2e::outcome project::mcpp(std::vector<std::string> args, const options& o) const {
    return support::mcpp(std::move(args), dir_, o);
}
e2e::outcome project::mcpp_in(std::string_view sub, std::vector<std::string> args, const options& o) const {
    return support::mcpp(std::move(args), at(sub), o);
}

std::optional<fs::path> project::artifact(std::string_view name) const {
    return find_file(dir_ / "target", name);
}

project project::scaffold(std::string_view name, std::vector<std::string> args) {
    const auto parent = case_dir() / "new";
    fs::create_directories(parent);
    std::vector<std::string> argv{"new", std::string(name)};
    argv.insert(argv.end(), args.begin(), args.end());
    auto r = support::mcpp(std::move(argv), parent);
    if (!r)
        mcpplibs::mtest::fail(std::format("mcpp new {} failed: {}\n{}", name, r.line, text(r)));
    return project(parent / fs::path(std::u8string(name.begin(), name.end())), {});
}

std::optional<fs::path> find_file(const fs::path& root, std::string_view name) {
    std::error_code ec;
    if (!fs::is_directory(root, ec))
        return std::nullopt;
    std::vector<fs::path> found;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && it->path().filename() == name)
            found.push_back(it->path());
    if (found.empty())
        return std::nullopt;
    std::ranges::sort(found);  // the same answer on every run
    return found.front();
}

std::vector<fs::path> find_files(const fs::path& root, std::string_view pattern) {
    std::vector<fs::path> found;
    std::error_code ec;
    if (!fs::is_directory(root, ec))
        return found;
    const std::regex re{std::string(pattern)};
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && std::regex_match(it->path().filename().string(), re))
            found.push_back(it->path());
    std::ranges::sort(found);
    return found;
}

std::vector<std::string> listing(const fs::path& root) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
        out.push_back(it->path().lexically_relative(root).generic_string());
    std::ranges::sort(out);
    return out;
}

std::optional<fs::path> newest_file(const fs::path& root, std::string_view name) {
    std::optional<fs::path> best;
    fs::file_time_type when{};
    std::error_code ec;
    if (!fs::is_directory(root, ec))
        return best;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().filename() != name)
            continue;
        const auto t = fs::last_write_time(it->path(), ec);
        if (!best || t > when) { best = it->path(); when = t; }
    }
    return best;
}

void write_executable(const fs::path& file, std::string_view content) {
    write_into(file.parent_path(), file.filename().string(), content);
    std::error_code ec;
    fs::permissions(file, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                              fs::perms::others_read | fs::perms::others_exec, ec);
}

e2e::outcome run(const fs::path& program, std::vector<std::string> args, const fs::path& cwd,
                 std::vector<std::pair<std::string, std::optional<std::string>>> env) {
    e2e::command c;
    c.argv.push_back(program.string());
    for (auto& a : args) c.argv.push_back(std::move(a));
    c.cwd = cwd;
    c.env = std::move(env);
    return e2e::run(std::move(c));
}

namespace {
// A shell's `*` within one name; no leading dot unless the pattern has one.
bool wildcard(std::string_view pattern, std::string_view name) {
    if (name.starts_with('.') && !pattern.starts_with('.'))
        return false;
    std::size_t p = 0, n = 0, star = std::string_view::npos, mark = 0;
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == '*') { star = p++; mark = n; }
        else if (p < pattern.size() && pattern[p] == name[n]) { ++p; ++n; }
        else if (star != std::string_view::npos) { p = star + 1; n = ++mark; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}
}  // namespace

std::vector<fs::path> glob(const fs::path& root, std::string_view pattern) {
    std::vector<fs::path> level{root};
    for (auto part : std::views::split(pattern, '/')) {
        const std::string_view component(part.begin(), part.end());
        if (component.empty())
            continue;
        std::vector<fs::path> next;
        std::error_code ec;
        for (const auto& dir : level) {
            if (!component.contains('*')) {
                if (fs::exists(dir / component, ec))
                    next.push_back(dir / component);
                continue;
            }
            if (!fs::is_directory(dir, ec))
                continue;
            for (const auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec))
                if (wildcard(component, e.path().filename().string()))
                    next.push_back(e.path());
        }
        level = std::move(next);
    }
    std::ranges::sort(level);
    return level;
}

// ---- readings --------------------------------------------------------------------------------------

std::string read_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::string> lines_with(std::string_view text, std::string_view needle) {
    std::vector<std::string> out;
    for (auto line : std::views::split(text, '\n')) {
        std::string_view l(line.begin(), line.end());
        if (l.ends_with('\r')) l.remove_suffix(1);
        if (l.find(needle) != std::string_view::npos)
            out.emplace_back(l);
    }
    return out;
}

std::vector<std::string> file_lines_with(const fs::path& file, std::string_view needle) {
    return lines_with(read_file(file), needle);
}

nlohmann::json json(std::string_view text) {
    auto whole = nlohmann::json::parse(text, nullptr, false);
    if (!whole.is_discarded())
        return whole;
    for (auto line : std::views::split(text, '\n')) {
        auto one = nlohmann::json::parse(std::string_view(line.begin(), line.end()), nullptr, false);
        if (!one.is_discarded())
            return one;
    }
    return nlohmann::json{};
}

nlohmann::json json_file(const fs::path& file) { return json(read_file(file)); }

std::vector<nlohmann::json> json_lines(std::string_view text) {
    std::vector<nlohmann::json> out;
    for (auto line : std::views::split(text, '\n')) {
        std::string_view l(line.begin(), line.end());
        if (l.empty() || l.front() != '{')
            continue;
        auto one = nlohmann::json::parse(l, nullptr, false);
        if (!one.is_discarded() && one.is_object())
            out.push_back(std::move(one));
    }
    return out;
}

namespace {
std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> out;
    for (auto line : std::views::split(text, '\n')) {
        std::string_view l(line.begin(), line.end());
        if (l.ends_with('\r')) l.remove_suffix(1);
        out.push_back(l);
    }
    return out;
}
std::string tail_of(std::string_view text) {
    constexpr std::size_t keep = 3000;
    return std::string(text.size() > keep ? text.substr(text.size() - keep) : text);
}
std::string lower(std::string_view s) {
    std::string r(s);
    for (auto& ch : r) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return r;
}
bool any_line(std::string_view text, std::string_view pattern) {
    const std::regex re{std::string(pattern)};
    for (auto l : split_lines(text))
        if (std::regex_search(l.begin(), l.end(), re))
            return true;
    return false;
}
}  // namespace

mcpplibs::mtest::comparison has_line(std::string_view text, std::string_view pattern) {
    return {any_line(text, pattern), "has_line", std::format("a line matching /{}/", pattern), tail_of(text)};
}
mcpplibs::mtest::comparison no_line(std::string_view text, std::string_view pattern) {
    return {!any_line(text, pattern), "no_line", std::format("no line matching /{}/", pattern), tail_of(text)};
}
mcpplibs::mtest::comparison lacks(std::string_view text, std::string_view needle) {
    return {!text.contains(needle), "lacks", std::format("text without \"{}\"", needle), tail_of(text)};
}
mcpplibs::mtest::comparison contains_nocase(std::string_view text, std::string_view needle) {
    return {lower(text).contains(lower(needle)), "contains_nocase",
            std::format("text containing \"{}\" (any case)", needle), tail_of(text)};
}
mcpplibs::mtest::comparison has_line_nocase(std::string_view text, std::string_view pattern) {
    const std::regex re{std::string(pattern), std::regex::ECMAScript | std::regex::icase};
    bool held = false;
    for (auto l : split_lines(text))
        held = held || std::regex_search(l.begin(), l.end(), re);
    return {held, "has_line_nocase", std::format("a line matching /{}/i", pattern), tail_of(text)};
}
std::string last_line(std::string_view text) {
    auto lines = split_lines(text);
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines.empty() ? std::string{} : std::string(lines.back());
}
std::string last_starting(std::string_view text, std::string_view prefix) {
    std::string found;
    for (auto l : split_lines(text))
        if (l.starts_with(prefix)) found = std::string(l);
    return found;
}
std::size_t count_matching(std::string_view text, std::string_view pattern) {
    const std::regex re{std::string(pattern)};
    return static_cast<std::size_t>(std::ranges::count_if(split_lines(text), [&](auto l) { return std::regex_search(l.begin(), l.end(), re); }));
}
std::size_t count_lines(std::string_view text, std::string_view needle) {
    return static_cast<std::size_t>(std::ranges::count_if(split_lines(text), [&](auto l) { return l.contains(needle); }));
}

std::optional<std::vector<std::string>> cdb_arguments(const fs::path& cdb, std::string_view file_suffix) {
    std::error_code ec;
    if (!fs::is_regular_file(cdb, ec))
        return std::nullopt;
    const auto doc = nlohmann::json::parse(read_file(cdb), nullptr, false);
    if (doc.is_discarded() || !doc.is_array())
        return std::nullopt;
    for (const auto& e : doc) {
        auto file = e.value("file", std::string{});
        std::ranges::replace(file, '\\', '/');
        if (!file.ends_with(file_suffix))
            continue;
        std::vector<std::string> words;
        if (e.contains("arguments") && e["arguments"].is_array()) {
            for (const auto& w : e["arguments"]) words.push_back(w.get<std::string>());
        } else {
            for (auto w : std::views::split(std::string_view(e.value("command", std::string{})), ' '))
                if (!std::string_view(w.begin(), w.end()).empty()) words.emplace_back(w.begin(), w.end());
        }
        return words;
    }
    return std::nullopt;
}

std::vector<std::string> ninja_log_outputs(const fs::path& build_dir) {
    std::vector<std::string> out;
    for (auto l : split_lines(read_file(build_dir / ".ninja_log"))) {
        if (l.starts_with('#'))
            continue;
        std::vector<std::string_view> cols;
        for (auto c : std::views::split(l, '\t')) cols.emplace_back(c.begin(), c.end());
        if (cols.size() >= 4)
            out.emplace_back(cols[3]);
    }
    return out;
}

// ---- executables -------------------------------------------------------------------------------------

namespace {

template <class T>
T get(const std::string& b, std::size_t at, bool big = false) {
    T v{};
    if (at + sizeof(T) > b.size())
        return v;
    std::memcpy(&v, b.data() + at, sizeof(T));
    if (big) {
        T r{};
        for (std::size_t i = 0; i < sizeof(T); ++i)
            r = static_cast<T>((r << 8) | static_cast<unsigned char>(b[at + i]));
        return r;
    }
    return v;
}

std::string cstring(const std::string& b, std::size_t at) {
    if (at >= b.size())
        return {};
    const auto end = b.find('\0', at);
    return b.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

void read_elf(const std::string& b, header& h) {
    h.kind = format::elf;
    h.wide = b.size() > 4 && b[4] == 2;
    h.elf_type = get<std::uint16_t>(b, 16);
    h.machine = get<std::uint16_t>(b, 18);
    const std::uint64_t phoff = h.wide ? get<std::uint64_t>(b, 32) : get<std::uint32_t>(b, 28);
    const std::uint16_t phentsize = get<std::uint16_t>(b, h.wide ? 54 : 42);
    const std::uint16_t phnum = get<std::uint16_t>(b, h.wide ? 56 : 44);
    std::uint64_t dyn_off = 0, dyn_size = 0;
    struct load { std::uint64_t vaddr, offset, filesz; };
    std::vector<load> loads;
    for (std::uint16_t i = 0; i < phnum; ++i) {
        const std::size_t p = static_cast<std::size_t>(phoff) + std::size_t(i) * phentsize;
        const auto type = get<std::uint32_t>(b, p);
        const std::uint64_t offset = h.wide ? get<std::uint64_t>(b, p + 8) : get<std::uint32_t>(b, p + 4);
        const std::uint64_t vaddr = h.wide ? get<std::uint64_t>(b, p + 16) : get<std::uint32_t>(b, p + 8);
        const std::uint64_t filesz = h.wide ? get<std::uint64_t>(b, p + 32) : get<std::uint32_t>(b, p + 16);
        if (type == 3) {  // PT_INTERP
            h.elf_interpreter = true;
            h.interpreter = cstring(b, static_cast<std::size_t>(offset));
        } else if (type == 2) {  // PT_DYNAMIC
            dyn_off = offset;
            dyn_size = filesz;
        } else if (type == 1) {
            loads.push_back({vaddr, offset, filesz});
        }
    }
    if (!dyn_size)
        return;
    const std::size_t entry = h.wide ? 16 : 8;
    std::uint64_t strtab = 0;
    std::vector<std::uint64_t> needed;
    std::vector<std::uint64_t> paths;
    std::uint64_t soname = 0;
    bool has_soname = false;
    for (std::uint64_t at = dyn_off; at + entry <= dyn_off + dyn_size; at += entry) {
        const std::int64_t tag = h.wide ? get<std::int64_t>(b, at) : get<std::int32_t>(b, at);
        const std::uint64_t val = h.wide ? get<std::uint64_t>(b, at + 8) : get<std::uint32_t>(b, at + 4);
        if (tag == 0) break;
        if (tag == 5) strtab = val;
        else if (tag == 1) needed.push_back(val);
        else if (tag == 14) { soname = val; has_soname = true; }
        else if (tag == 29 || tag == 15) {
            paths.push_back(val);
            (tag == 15 ? h.elf_rpath : h.elf_runpath) = true;
        }
    }
    // The string table's address, as a file offset.
    std::uint64_t str_off = strtab;
    for (const auto& l : loads)
        if (strtab >= l.vaddr && strtab < l.vaddr + l.filesz)
            str_off = strtab - l.vaddr + l.offset;
    for (auto n : needed)
        h.needed.push_back(cstring(b, static_cast<std::size_t>(str_off + n)));
    for (auto p : paths) {
        if (!h.runpath.empty()) h.runpath += ':';
        h.runpath += cstring(b, static_cast<std::size_t>(str_off + p));
    }
    if (has_soname)
        h.soname = cstring(b, static_cast<std::size_t>(str_off + soname));
}

void read_pe(const std::string& b, header& h) {
    const auto pe = get<std::uint32_t>(b, 0x3c);
    if (b.compare(pe, 4, std::string("PE\0\0", 4)) != 0)
        return;  // an MZ program with no PE image (DOS)
    h.kind = format::pe;
    h.machine = get<std::uint16_t>(b, pe + 4);
    const auto magic = get<std::uint16_t>(b, pe + 24);
    h.wide = magic == 0x20b;
    h.subsystem = get<std::uint16_t>(b, pe + 24 + 68);
}

}  // namespace

header read_header(const fs::path& file) {
    header h;
    std::ifstream in(file, std::ios::binary);
    std::string b(64 * 1024, '\0');
    in.read(b.data(), static_cast<std::streamsize>(b.size()));
    b.resize(static_cast<std::size_t>(in.gcount()));
    if (b.size() >= 4 && b.compare(0, 4, "\x7f" "ELF") == 0) {
        // The dynamic section may lie past the first 64 KiB.
        h.kind = format::elf;
        b = read_file(file);
        read_elf(b, h);
    } else if (b.size() >= 2 && b[0] == 'M' && b[1] == 'Z') {
        read_pe(b, h);
    } else if (b.size() >= 4) {
        const auto m = get<std::uint32_t>(b, 0);
        if (m == 0xfeedface || m == 0xfeedfacf) {
            h.kind = format::macho;
            h.wide = m == 0xfeedfacf;
            h.machine = get<std::uint32_t>(b, 4);
        } else if (m == 0xbebafeca || m == 0xbfbafeca) {
            h.kind = format::macho_fat;
        } else if (b.compare(0, 4, std::string("\0asm", 4)) == 0) {
            h.kind = format::wasm;
        } else if (b.size() >= 8 && b.compare(0, 8, "!<arch>\n") == 0) {
            h.kind = format::archive;
        }
    }
    return h;
}

namespace {
// One member of an `ar` archive: its raw name field and its contents.
struct ar_member {
    std::string name;
    std::string data;
};

std::vector<ar_member> ar_members(const fs::path& file) {
    const auto bytes = read_file(file);
    std::vector<ar_member> out;
    if (!bytes.starts_with("!<arch>\n")) return out;
    std::size_t at = 8;
    while (at + 60 <= bytes.size()) {
        auto name = bytes.substr(at, 16);
        const auto size = std::stoull(bytes.substr(at + 48, 10));
        at += 60;
        if (at + size > bytes.size()) break;
        auto data = bytes.substr(at, size);
        while (!name.empty() && name.back() == ' ') name.pop_back();
        // BSD: `#1/<n>`, the name is the first n bytes of the data.
        if (name.starts_with("#1/")) {
            const auto n = std::stoull(name.substr(3));
            name = data.substr(0, n);
            data = data.substr(n);
            while (!name.empty() && name.back() == '\0') name.pop_back();
        }
        out.push_back({std::move(name), std::move(data)});
        at += size + (size % 2);
    }
    return out;
}
}  // namespace

std::vector<std::string> archive_members(const fs::path& file) {
    const auto all = ar_members(file);
    std::string long_names;
    for (const auto& m : all)
        if (m.name == "//") long_names = m.data;
    std::vector<std::string> out;
    for (const auto& m : all) {
        if (m.name == "/" || m.name == "//" || m.name == "/SYM64/" || m.name.starts_with("__.SYMDEF")) continue;
        std::string name = m.name;
        if (name.size() > 1 && name[0] == '/' && std::isdigit(static_cast<unsigned char>(name[1]))) {
            const auto off = std::stoull(name.substr(1));
            const auto end = long_names.find('\n', off);
            name = long_names.substr(off, end == std::string::npos ? std::string::npos : end - off);
        }
        if (name.ends_with("/")) name.pop_back();
        out.push_back(name);
    }
    return out;
}

std::vector<std::string> archive_symbols(const fs::path& file) {
    std::vector<std::string> out;
    for (const auto& m : ar_members(file)) {
        // System V/GNU: a big-endian count (4 bytes; 8 in /SYM64/), as many offsets, the names.
        const std::size_t width = m.name == "/" ? 4 : m.name == "/SYM64/" ? 8 : 0;
        if (width != 0 && m.data.size() >= width) {
            std::uint64_t n = 0;
            for (std::size_t i = 0; i < width; ++i) n = (n << 8) | static_cast<unsigned char>(m.data[i]);
            std::size_t at = width + n * width;
            for (std::uint64_t i = 0; i < n && at < m.data.size(); ++i) {
                const auto end = m.data.find('\0', at);
                out.push_back(m.data.substr(at, end - at));
                at = end == std::string::npos ? m.data.size() : end + 1;
            }
        }
        // BSD `__.SYMDEF`: ranlib entries (string offset, member offset) and a string table.
        if (m.name.starts_with("__.SYMDEF") && m.data.size() >= 4) {
            auto le32 = [&](std::size_t at) {
                std::uint32_t v = 0;
                for (int i = 3; i >= 0; --i) v = (v << 8) | static_cast<unsigned char>(m.data[at + i]);
                return v;
            };
            const auto ranlib = le32(0);
            const std::size_t strings = 4 + ranlib + 4;
            for (std::size_t e = 4; e + 8 <= 4 + ranlib && strings <= m.data.size(); e += 8) {
                const auto at = strings + le32(e);
                const auto end = m.data.find('\0', at);
                if (at < m.data.size()) out.push_back(m.data.substr(at, end - at));
            }
        }
    }
    return out;
}

namespace {
struct raw_section {
    std::string name;
    std::uint32_t type = 0;
    std::uint64_t offset = 0, size = 0, entsize = 0;
    std::uint32_t link = 0;
};

// The section headers of an ELF file (either class, little-endian), named.
std::vector<raw_section> raw_sections(const std::string& b) {
    std::vector<raw_section> out;
    if (b.size() < 64 || b.compare(0, 4, "\x7f" "ELF") != 0) return out;
    const bool wide = b[4] == 2;
    const std::uint64_t shoff = wide ? get<std::uint64_t>(b, 0x28) : get<std::uint32_t>(b, 0x20);
    const std::uint16_t shentsize = get<std::uint16_t>(b, wide ? 0x3a : 0x2e);
    const std::uint16_t shnum = get<std::uint16_t>(b, wide ? 0x3c : 0x30);
    const std::uint16_t shstrndx = get<std::uint16_t>(b, wide ? 0x3e : 0x32);
    std::vector<std::uint32_t> names;
    for (std::uint16_t i = 0; i < shnum; ++i) {
        const std::size_t o = static_cast<std::size_t>(shoff) + std::size_t(i) * shentsize;
        raw_section s;
        names.push_back(get<std::uint32_t>(b, o));
        s.type = get<std::uint32_t>(b, o + 4);
        s.offset = wide ? get<std::uint64_t>(b, o + 0x18) : get<std::uint32_t>(b, o + 0x10);
        s.size = wide ? get<std::uint64_t>(b, o + 0x20) : get<std::uint32_t>(b, o + 0x14);
        s.link = get<std::uint32_t>(b, o + (wide ? 0x28 : 0x18));
        s.entsize = wide ? get<std::uint64_t>(b, o + 0x38) : get<std::uint32_t>(b, o + 0x24);
        out.push_back(std::move(s));
    }
    if (shstrndx < out.size())
        for (std::size_t i = 0; i < out.size(); ++i)
            out[i].name = cstring(b, static_cast<std::size_t>(out[shstrndx].offset + names[i]));
    return out;
}
}  // namespace

std::vector<elf_section> elf_sections(const fs::path& file) {
    std::vector<elf_section> out;
    for (auto& s : raw_sections(read_file(file))) out.push_back({std::move(s.name), s.type});
    return out;
}

std::vector<std::string> elf_symbols(const fs::path& file, std::string_view table, bool defined_only) {
    const auto b = read_file(file);
    const auto all = raw_sections(b);
    std::vector<std::string> out;
    const bool wide = b.size() > 4 && b[4] == 2;
    for (const auto& s : all) {
        if (s.name != table || s.entsize == 0 || s.link >= all.size()) continue;
        const auto strings = all[s.link].offset;
        for (std::uint64_t k = 0; k < s.size / s.entsize; ++k) {
            const auto at = static_cast<std::size_t>(s.offset + k * s.entsize);
            const auto st_name = get<std::uint32_t>(b, at);  // the first word in both classes
            const auto st_shndx = get<std::uint16_t>(b, at + (wide ? 6 : 14));
            if (st_name && (!defined_only || st_shndx != 0))
                out.push_back(cstring(b, static_cast<std::size_t>(strings + st_name)));
        }
    }
    return out;
}

namespace {
e2e::outcome tar(std::vector<std::string> args, const fs::path& cwd = {}) {
    e2e::command c;
    c.argv.push_back("tar");
    for (auto& a : args) c.argv.push_back(std::move(a));
    c.cwd = cwd;
    return e2e::run(std::move(c));
}
}  // namespace

std::vector<std::string> tar_list(const fs::path& archive) {
    auto r = tar({"-tf", archive.string()});
    if (!r.started || r.status != 0)
        throw std::runtime_error("tar -tf " + archive.string() + " failed: " + r.err);
    std::vector<std::string> out;
    for (auto l : split_lines(r.out)) {
        if (l.ends_with('\r')) l.remove_suffix(1);
        if (!l.empty()) out.emplace_back(l);
    }
    return out;
}

void tar_extract(const fs::path& archive, const fs::path& into, std::vector<std::string> members) {
    fs::create_directories(into);
    std::vector<std::string> args{"-xf", fs::absolute(archive).string(), "-C", into.string()};
    for (auto& m : members) args.push_back(std::move(m));
    auto r = tar(std::move(args));
    if (!r.started || r.status != 0)
        throw std::runtime_error("tar -xf " + archive.string() + " failed: " + r.err);
}

std::vector<zip_entry> zip_entries(const fs::path& file) {
    const auto b = read_file(file);
    std::vector<zip_entry> out;
    // The end of central directory record: the last "PK\5\6".
    const auto eocd = b.rfind(std::string("PK\x05\x06", 4));
    if (eocd == std::string::npos) return out;
    const auto count = get<std::uint16_t>(b, eocd + 10);
    std::size_t at = get<std::uint32_t>(b, eocd + 16);
    for (std::uint16_t i = 0; i < count && at + 46 <= b.size(); ++i) {
        if (b.compare(at, 4, std::string("PK\x01\x02", 4)) != 0) break;
        const auto method = get<std::uint16_t>(b, at + 10);
        const auto name_len = get<std::uint16_t>(b, at + 28);
        const auto extra_len = get<std::uint16_t>(b, at + 30);
        const auto comment_len = get<std::uint16_t>(b, at + 32);
        out.push_back({b.substr(at + 46, name_len), method});
        at += 46 + name_len + extra_len + comment_len;
    }
    return out;
}

std::string sha256(std::string_view data) {
    static constexpr std::array<std::uint32_t, 64> k{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::array<std::uint32_t, 8> h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string m(data);
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    m.push_back(static_cast<char>(0x80));
    while (m.size() % 64 != 56) m.push_back('\0');
    for (int i = 7; i >= 0; --i) m.push_back(static_cast<char>((bits >> (i * 8)) & 0xff));
    auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (std::size_t chunk = 0; chunk < m.size(); chunk += 64) {
        std::array<std::uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i)
            for (int j = 0; j < 4; ++j)
                w[i] = (w[i] << 8) | static_cast<unsigned char>(m[chunk + i * 4 + j]);
        for (int i = 16; i < 64; ++i) {
            const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto [a, b, c, d, e, f, g, hh] = h;
        for (int i = 0; i < 64; ++i) {
            const auto t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const auto t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::string out;
    for (auto v : h) out += std::format("{:08x}", v);
    return out;
}

// ---- a process held by its pid ---------------------------------------------------------------------

std::string strip_escapes(std::string_view text) {
    static const std::regex csi("\x1b\\[[0-9;?]*[A-Za-z]");
    return std::regex_replace(std::string(text), csi, "");
}

std::vector<process_entry> processes() {
    std::vector<process_entry> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/proc", ec)) {
        const auto name = e.path().filename().string();
        if (name.empty() || !std::ranges::all_of(name, [](char c) { return c >= '0' && c <= '9'; }))
            continue;
        process_entry p;
        p.pid = std::stoi(name);
        std::ifstream(e.path() / "comm") >> p.name;
        std::error_code ec2;
        p.cwd = fs::read_symlink(e.path() / "cwd", ec2);
        std::ifstream cmd(e.path() / "cmdline", std::ios::binary);
        p.command_line.assign(std::istreambuf_iterator<char>(cmd), std::istreambuf_iterator<char>());
        std::ranges::replace(p.command_line, '\0', ' ');
        out.push_back(std::move(p));
    }
    return out;
}

std::string file_identity(const fs::path& file) {
#if defined(_WIN32)
    const auto t = std::chrono::duration_cast<std::chrono::seconds>(fs::last_write_time(file).time_since_epoch()).count();
    return std::format("0 {} {}", t, fs::file_size(file));
#else
    struct stat st {};
    if (::stat(file.c_str(), &st) != 0) return {};
    return std::format("{} {} {}", static_cast<unsigned long long>(st.st_ino), static_cast<long long>(st.st_mtime),
                       static_cast<long long>(st.st_size));
#endif
}

process::process(process&& o) noexcept
    : pid_(std::exchange(o.pid_, -1)), master_(std::exchange(o.master_, -1)), slave_(std::exchange(o.slave_, -1)),
      pipe_(std::exchange(o.pipe_, -1)), status_(o.status_), signal_(o.signal_), term_(std::move(o.term_)), piped_(std::move(o.piped_)) {}

process& process::operator=(process&& o) noexcept {
    if (this != &o) {
        this->~process();
        new (this) process(std::move(o));
    }
    return *this;
}

#if defined(_WIN32)
process process::start(spawn) { mcpplibs::mtest::fail("a process held by its pid needs a POSIX system"); }
process::~process() = default;
void process::pump(std::chrono::milliseconds) {}
bool process::wait_for(std::string_view, std::chrono::milliseconds) { return false; }
void process::send(std::string_view) {}
terminal_mode process::mode() const { return {}; }
void process::signal(int) const {}
std::optional<int> process::poll() { return status_; }
int process::wait(std::chrono::milliseconds) { return status_.value_or(-1); }
#else
namespace {
int shell_status(int raw) {
    if (WIFEXITED(raw)) return WEXITSTATUS(raw);
    if (WIFSIGNALED(raw)) return 128 + WTERMSIG(raw);
    return -1;
}
void cloexec(int fd) { ::fcntl(fd, F_SETFD, FD_CLOEXEC); }
}  // namespace

process process::start(spawn s) {
    process p;
    if (s.argv.empty()) mcpplibs::mtest::fail("process::start: no program");
    std::string program = s.argv.front();
    if (program.find('/') == std::string::npos) {
        auto found = on_path(program);
        if (!found) mcpplibs::mtest::fail(std::format("process::start: '{}' is not on PATH", program));
        program = found->string();
    }
    // The environment, made before the fork.
    std::map<std::string, std::string> vars;
    for (char** e = environ; e && *e; ++e) {
        std::string_view kv(*e);
        const auto eq = kv.find('=');
        if (eq != std::string_view::npos) vars[std::string(kv.substr(0, eq))] = std::string(kv.substr(eq + 1));
    }
    for (const auto& [k, v] : s.env) {
        if (v) vars[k] = *v;
        else vars.erase(k);
    }
    std::vector<std::string> env_words;
    for (const auto& [k, v] : vars) env_words.push_back(k + "=" + v);
    std::vector<char*> envp, argv;
    for (auto& w : env_words) envp.push_back(w.data());
    envp.push_back(nullptr);
    for (auto& a : s.argv) argv.push_back(a.data());
    argv.push_back(nullptr);
    const std::string cwd = s.cwd.empty() ? std::string() : s.cwd.string();

    const bool wants_terminal = s.in == stream::terminal || s.out == stream::terminal || s.err == stream::terminal;
    if (wants_terminal) {
        p.master_ = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (p.master_ < 0 || ::grantpt(p.master_) != 0 || ::unlockpt(p.master_) != 0)
            mcpplibs::mtest::fail("process::start: no pseudo-terminal");
        p.slave_ = ::open(::ptsname(p.master_), O_RDWR | O_NOCTTY);
        cloexec(p.master_);
        cloexec(p.slave_);
        winsize ws{};
        ws.ws_row = s.rows;
        ws.ws_col = s.cols;
        ::ioctl(p.slave_, TIOCSWINSZ, &ws);
    }
    int pipe_fds[2] = {-1, -1}, closed_fds[2] = {-1, -1};
    if (s.out == stream::pipe || s.err == stream::pipe) {
        if (::pipe(pipe_fds) != 0) mcpplibs::mtest::fail("process::start: no pipe");
        cloexec(pipe_fds[0]);
        cloexec(pipe_fds[1]);
    }
    if (s.out == stream::closed || s.err == stream::closed) {
        if (::pipe(closed_fds) != 0) mcpplibs::mtest::fail("process::start: no pipe");
        ::close(closed_fds[0]);
        cloexec(closed_fds[1]);
    }
    const auto open_file = [](const fs::path& f) { return ::open(f.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); };
    const int out_file = s.out == stream::file ? open_file(s.out_file) : -1;
    const int err_file = s.err == stream::file ? open_file(s.err_file) : -1;
    const int null_fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    const auto fd_for = [&](stream k, int file) {
        switch (k) {
        case stream::null: return null_fd;
        case stream::pipe: return pipe_fds[1];
        case stream::file: return file;
        case stream::terminal: return p.slave_;
        case stream::closed: return closed_fds[1];
        }
        return null_fd;
    };
    const int in_fd = fd_for(s.in, -1), out_fd = fd_for(s.out, out_file), err_fd = fd_for(s.err, err_file);

    p.pid_ = ::fork();
    if (p.pid_ == 0) {
        if (s.session) {
            ::setsid();
            if (p.slave_ >= 0) ::ioctl(p.slave_, TIOCSCTTY, 0);
        }
        struct sigaction dfl {};
        dfl.sa_handler = SIG_DFL;
        for (int n = 1; n < 32; ++n) ::sigaction(n, &dfl, nullptr);
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        ::dup2(in_fd, 0);
        ::dup2(out_fd, 1);
        ::dup2(err_fd, 2);
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) ::_exit(127);
        ::execve(program.c_str(), argv.data(), envp.data());
        ::_exit(127);
    }
    for (int fd : {pipe_fds[1], closed_fds[1], out_file, err_file, null_fd})
        if (fd >= 0) ::close(fd);
    p.pipe_ = pipe_fds[0];
    if (p.pid_ < 0) mcpplibs::mtest::fail("process::start: fork failed");
    return p;
}

process::~process() {
    if (pid_ > 0 && !status_) {
        ::kill(pid_, SIGKILL);
        int raw = 0;
        ::waitpid(pid_, &raw, 0);
    }
    for (int fd : {master_, slave_, pipe_})
        if (fd >= 0) ::close(fd);
}

void process::pump(std::chrono::milliseconds wait) {
    const auto deadline = std::chrono::steady_clock::now() + wait;
    for (;;) {
        std::vector<pollfd> fds;
        if (master_ >= 0) fds.push_back({master_, POLLIN, 0});
        if (pipe_ >= 0) fds.push_back({pipe_, POLLIN, 0});
        if (fds.empty()) return;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        const int n = ::poll(fds.data(), fds.size(), static_cast<int>(std::max<long long>(left.count(), 0)));
        if (n <= 0) return;
        for (const auto& f : fds) {
            if (!(f.revents & (POLLIN | POLLHUP | POLLERR))) continue;
            char buf[65536];
            const auto got = ::read(f.fd, buf, sizeof buf);
            auto& into = f.fd == master_ ? term_ : piped_;
            if (got > 0) {
                into.append(buf, static_cast<std::size_t>(got));
            } else if (f.fd == pipe_) {
                ::close(pipe_);
                pipe_ = -1;
            } else if (slave_ < 0) {
                ::close(master_);
                master_ = -1;
            } else {
                return;  // the terminal is held open by this process: nothing more now
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) return;
    }
}

bool process::wait_for(std::string_view pattern, std::chrono::milliseconds timeout) {
    const std::regex re{std::string(pattern)};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (std::regex_search(strip_escapes(term_), re) || std::regex_search(strip_escapes(piped_), re)) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        pump(std::chrono::milliseconds{100});
        if (poll() && master_ < 0 && pipe_ < 0) return std::regex_search(strip_escapes(term_ + piped_), re);
    }
}

void process::send(std::string_view bytes) {
    if (master_ >= 0) (void)::write(master_, bytes.data(), bytes.size());
}

terminal_mode process::mode() const {
    terminal_mode m;
    termios t{};
    if (slave_ < 0 || ::tcgetattr(slave_, &t) != 0) return m;
    m.iflag = t.c_iflag;
    m.oflag = t.c_oflag;
    m.cflag = t.c_cflag;
    // PENDIN is the kernel's own (macOS sets it while input is re-echoed): not a mode a program chose.
    m.lflag = t.c_lflag & ~static_cast<unsigned long>(PENDIN);
    m.cc.assign(std::begin(t.c_cc), std::end(t.c_cc));
    m.echo = (t.c_lflag & ECHO) != 0;
    m.canonical = (t.c_lflag & ICANON) != 0;
    return m;
}

void process::signal(int sig) const {
    if (pid_ > 0 && !status_) ::kill(pid_, sig);
}

std::optional<int> process::poll() {
    if (!status_ && pid_ > 0) {
        int raw = 0;
        if (::waitpid(pid_, &raw, WNOHANG) == pid_) {
            status_ = shell_status(raw);
            signal_ = WIFSIGNALED(raw) ? WTERMSIG(raw) : 0;
        }
    }
    return status_;
}

int process::wait(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!poll()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid_, SIGKILL);
            int raw = 0;
            ::waitpid(pid_, &raw, 0);
            status_ = shell_status(raw);
            signal_ = WIFSIGNALED(raw) ? WTERMSIG(raw) : 0;
            break;
        }
        if (master_ >= 0 || pipe_ >= 0) pump(std::chrono::milliseconds{50});
        else std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    // Drain: what the program wrote before it ended, and what a child of it still writes, until the
    // streams are quiet.
    for (int quiet = 0; quiet < 3 && (master_ >= 0 || pipe_ >= 0);) {
        const auto before = term_.size() + piped_.size();
        pump(std::chrono::milliseconds{100});
        quiet = term_.size() + piped_.size() == before ? quiet + 1 : 0;
    }
    return *status_;
}
#endif

}  // namespace mcpp::test::support
