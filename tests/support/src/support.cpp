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
// The homes of the cases, made under the system's temporary directory: a payload may hold its
// install directory in a fixed space (glibc's binaries hold 255 bytes), and a case's own directory
// is too long for it, where the scripts' `mktemp -d` was not. Removed when the program ends.
struct homes {
    std::mutex m;
    std::map<fs::path, fs::path> of_case;
    ~homes() {
        std::error_code ec;
        for (const auto& [_, h] : of_case) fs::remove_all(h, ec);
    }
};
homes& case_homes() {
    static homes h;
    return h;
}
}  // namespace

fs::path case_home(home kind) {
    auto& all = case_homes();
    const auto key = mcpplibs::mtest::temp_dir();
    std::scoped_lock lock(all.m);
    if (auto it = all.of_case.find(key); it != all.of_case.end())
        return it->second;
    std::random_device rd;
    fs::path h;
    std::error_code ec;
    do h = fs::temp_directory_path() / std::format("mcpp-e2e-home-{:08x}", rd());
    while (!fs::create_directory(h, ec) && !ec);
    all.of_case.emplace(key, h);
    if (kind == home::linked && fs::is_directory(home_dir() / "registry", ec))
        link_or_copy(home_dir() / "registry", h / "registry");
    else if (kind == home::inherited || kind == home::payloads)
        inherit(h, kind == home::inherited);
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

project::project(files contents) : project(mcpplibs::mtest::temp_dir() / "project", contents) {}

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
    const auto parent = mcpplibs::mtest::temp_dir() / "new";
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
    std::uint64_t runpath = 0;
    bool has_runpath = false;
    for (std::uint64_t at = dyn_off; at + entry <= dyn_off + dyn_size; at += entry) {
        const std::int64_t tag = h.wide ? get<std::int64_t>(b, at) : get<std::int32_t>(b, at);
        const std::uint64_t val = h.wide ? get<std::uint64_t>(b, at + 8) : get<std::uint32_t>(b, at + 4);
        if (tag == 0) break;
        if (tag == 5) strtab = val;
        else if (tag == 1) needed.push_back(val);
        else if (tag == 29 || tag == 15) { runpath = val; has_runpath = true; }
    }
    // The string table's address, as a file offset.
    std::uint64_t str_off = strtab;
    for (const auto& l : loads)
        if (strtab >= l.vaddr && strtab < l.vaddr + l.filesz)
            str_off = strtab - l.vaddr + l.offset;
    for (auto n : needed)
        h.needed.push_back(cstring(b, static_cast<std::size_t>(str_off + n)));
    if (has_runpath)
        h.runpath = cstring(b, static_cast<std::size_t>(str_off + runpath));
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

}  // namespace mcpp::test::support
