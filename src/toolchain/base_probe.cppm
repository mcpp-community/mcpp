// mcpp.toolchain.base_probe — locating and measuring a target base (design
// 2026-10-23 §4.3-§4.4, §4.8, §4.9). The data it fills is
// `mcpp.toolchain.base`; the decision of which statement applies is prepare's.
//
// LAYOUT IS ASKED OF THE DRIVER, IMPLEMENTATION AND VERSION OF THE
// PREPROCESSOR (D10). Nothing here recognises a vendor's directory tree. The
// one piece of layout knowledge is Debian multiarch, which is specified and
// which a gcc built without multiarch support needs to be told (scenario J).
// Nothing here writes into the directory it measures.

export module mcpp.toolchain.base_probe;

import std;
import mcpplibs.os.platform;
import mcpp.run;
import mcpp.toolchain.base;
import mcpp.toolchain.linkmodel;
import mcpp.toolchain.model;
import mcpp.toolchain.triple;
import mcpp.runtime.elf;
import mcpp.toolchain.apple_sdk;
import mcpp.invocation;

export namespace mcpp::toolchain::base {

namespace fs = std::filesystem;

// The ecosystem's payload layout (`<root>/include/features.h`, `<root>/lib64`
// or `<root>/lib`) rather than a sysroot's (`<root>/usr/include`). A glibc
// package is the first; a crosstool-ng or Debian root is the second.
inline bool is_payload_layout(const fs::path& root) {
    std::error_code ec;
    if (fs::exists(root / "usr" / "include", ec)) return false;
    if (!fs::exists(root / "include" / "features.h", ec)) return false;
    return fs::exists(root / "lib64" / "libc.so.6", ec) || fs::exists(root / "lib" / "libc.so.6", ec)
        || fs::exists(root / "lib64" / "libc.so", ec)   || fs::exists(root / "lib" / "libc.so", ec);
}

inline PayloadPaths payload_layout_paths(const fs::path& root, const fs::path& linuxInclude) {
    PayloadPaths pp;
    std::error_code ec;
    pp.glibcInclude = root / "include";
    pp.glibcLib = fs::exists(root / "lib64" / "libc.so.6", ec) || fs::exists(root / "lib64" / "libc.so", ec)
                ? root / "lib64" : root / "lib";
    pp.linuxInclude = fs::exists(root / "include" / "linux" / "limits.h", ec) ? fs::path{} : linuxInclude;
    return pp;
}

// The directories of a gnu-sysroot a loader searches, in the order the
// layouts put them: crosstool-ng and Fedora (`lib64`, `usr/lib64`), the
// generic `lib`, then Debian multiarch.
inline std::vector<fs::path> library_dirs(const fs::path& root, std::string_view tuple) {
    std::vector<fs::path> out{root / "lib64", root / "usr" / "lib64", root / "lib", root / "usr" / "lib"};
    if (!tuple.empty()) {
        out.push_back(root / "lib" / std::string(tuple));
        out.push_back(root / "usr" / "lib" / std::string(tuple));
    }
    return out;
}

namespace detail {

inline std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

// `-print-file-name=<name>`: an absolute path when the driver found it, the
// name echoed back when it did not.
inline fs::path print_file_name(const std::vector<std::string>& base, std::string_view name) {
    auto argv = base;
    argv.push_back(std::format("-print-file-name={}", name));
    auto r = mcpp::run::capture_stdout(argv);
    if (r.exit_code != 0) return {};
    fs::path p(trim(r.output));
    std::error_code ec;
    if (!p.is_absolute() || !fs::exists(p, ec)) return {};
    return p;
}

inline void mix(std::uint64_t& h, std::string_view s) {
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    h ^= 0x1f; h *= 1099511628211ull;
}

inline void stamp(std::uint64_t& h, const fs::path& p) {
    std::error_code ec;
    mix(h, p.generic_string());
    auto real = fs::canonical(p, ec);
    if (ec) { mix(h, "absent"); return; }
    mix(h, real.generic_string());
    mix(h, std::format("{}", static_cast<std::uint64_t>(fs::file_size(real, ec))));
    auto t = fs::last_write_time(real, ec);
    mix(h, std::format("{}", static_cast<std::int64_t>(t.time_since_epoch().count())));
}

} // namespace detail

// What a gnu-sysroot directory is, measured with the driver that will use it.
struct GnuMeasurement {
    bool                 cAbiFound = false;   // the driver found the C runtime
    bool                 multiarch = false;
    std::string          tuple;
    std::optional<LibcFact> fact;
    fs::path             loader;
    std::vector<fs::path> runtimeDirs;
    std::vector<fs::path> evidence;           // the files the identity stamps
};

// `driverBase` is the driver and the flags that select its target
// (`--target=`, `--no-default-config`), without any sysroot. `probeDir` is a
// writable directory for the probe translation unit.
inline GnuMeasurement measure_gnu(const std::vector<std::string>& driverBase,
                                  const triple::Triple& t,
                                  const fs::path& root,
                                  bool clangDriver,
                                  const fs::path& probeDir) {
    GnuMeasurement m;
    std::error_code ec;
    auto base = driverBase;
    base.push_back("--sysroot=" + root.string());
    // The C runtime, as the driver looks for it.
    auto crt = detail::print_file_name(base, "crt1.o");
    if (crt.empty()) crt = detail::print_file_name(base, "Scrt1.o");
    std::vector<std::string> extra;
    const auto tuple = multiarch_tuple(t);
    if (crt.empty() && !tuple.empty()
        && (fs::exists(root / "usr" / "lib" / tuple / "crt1.o", ec)
            || fs::exists(root / "usr" / "lib" / tuple / "Scrt1.o", ec))) {
        // Debian multiarch, and a driver that does not know it.
        m.multiarch = true;
        m.tuple = tuple;
        crt = root / "usr" / "lib" / tuple / "crt1.o";
        extra.push_back(std::string(clangDriver ? "-isystem" : "-idirafter")
                        + (root / "usr" / "include" / tuple).string());
    } else if (!tuple.empty() && fs::is_directory(root / "usr" / "lib" / tuple, ec)) {
        // The driver found it through multiarch itself (clang); the
        // directories still hold the libraries a loader is pointed at.
        m.tuple = tuple;
    }
    m.cAbiFound = !crt.empty();
    if (!crt.empty()) m.evidence.push_back(crt);

    // The implementation and its version, from the preprocessor.
    fs::create_directories(probeDir, ec);
    const auto probe = probeDir / "mcpp-base-probe.c";
    {
        std::ofstream os(probe, std::ios::binary | std::ios::trunc);
        os << "#include <stdio.h>\n";
    }
    auto pp = base;
    for (auto const& e : extra) pp.push_back(e);
    pp.insert(pp.end(), {"-E", "-dM", "-x", "c", probe.string()});
    auto r = mcpp::run::capture_stdout(pp);
    if (r.exit_code == 0) m.fact = classify_macros(parse_defines(r.output));
    if (!m.fact && m.cAbiFound) {
        // musl defines nothing that names it; a musl loader or a `libc.a`
        // with no glibc beside it is the answer (§4.3's last measurable row).
        for (auto const& d : library_dirs(root, m.tuple))
            for (auto it = fs::directory_iterator(d, ec); !ec && it != fs::directory_iterator{}; it.increment(ec))
                if (it->path().filename().string().starts_with("ld-musl-")) {
                    m.fact = LibcFact{"musl", "", "musl"};
                    break;
                }
        if (!m.fact && t.is_musl()) m.fact = LibcFact{"musl", "", "musl"};
    }
    for (auto name : {"usr/include/features.h", "usr/include/gnu/libc-version.h",
                      "usr/include/stdio.h"})
        if (fs::exists(root / name, ec)) m.evidence.push_back(root / name);

    // The loader and the directories a binding artifact's RUNPATH names.
    const bool glibc = m.fact && m.fact->impl == "glibc";
    for (auto const& d : library_dirs(root, m.tuple)) {
        if (m.loader.empty())
            if (auto l = resolve_loader(d, t.str()); !l.empty()) m.loader = l;
        const bool hasLibc = glibc ? fs::exists(d / "libc.so.6", ec)
                                   : (fs::exists(d / "libc.so", ec) || fs::exists(d / "libc.a", ec));
        if (hasLibc && std::ranges::find(m.runtimeDirs, d) == m.runtimeDirs.end()) {
            // A symlinked `lib64 -> usr/lib64` is one directory.
            auto real = fs::canonical(d, ec);
            bool dup = false;
            for (auto const& seen : m.runtimeDirs)
                if (!ec && fs::canonical(seen, ec) == real) dup = true;
            if (!dup) m.runtimeDirs.push_back(d);
        }
    }
    if (!m.loader.empty()) m.evidence.push_back(m.loader);
    for (auto const& d : m.runtimeDirs)
        m.evidence.push_back(d / (glibc ? "libc.so.6" : "libc.so"));
    return m;
}

// THE BASE'S CONTENT IDENTITY (§4.9): the statement, the root, and the path,
// size and modification time of each file the measurement rested on. A base
// rewritten in place gets a new identity, and so a new output directory,
// without hashing a gigabyte of libraries on every build.
inline std::string identity_of(std::string_view text, const fs::path& root,
                               const std::vector<fs::path>& evidence,
                               std::string_view measured) {
    std::uint64_t h = 1469598103934665603ull;
    detail::mix(h, text);
    detail::mix(h, root.generic_string());
    detail::mix(h, measured);
    for (auto const& f : evidence) detail::stamp(h, f);
    return std::format("{:016x}", h);
}

// THE C LIBRARY THE COMPILER'S OWN RUNTIME REQUIRES (§4.8): the highest
// `GLIBC_x.y` named by the version needs of the runtime libraries a produced
// artifact links. Asked of the driver without the base, because these are the
// compiler's own. Empty when none is found or none names glibc.
struct RuntimeFloor {
    std::string version;   // "2.38"
    fs::path    from;      // the library that requires it
};

inline RuntimeFloor compiler_runtime_glibc_floor(const std::vector<std::string>& driverBase) {
    RuntimeFloor best;
    for (auto name : {"libstdc++.so.6", "libgcc_s.so.1", "libc++.so.1", "libc++abi.so.1",
                      "libunwind.so.1", "libatomic.so.1"}) {
        auto p = detail::print_file_name(driverBase, name);
        if (p.empty()) continue;
        std::error_code ec;
        auto real = fs::canonical(p, ec);
        if (ec) continue;
        auto facts = mcpp::runtime::elf::inspect_elf_runtime(real);
        if (!facts) continue;
        auto v = highest_glibc(facts->requiredGlibcVersions);
        if (!v.empty() && (best.version.empty() || compare_versions(v, best.version) > 0))
            best = RuntimeFloor{v, p};
    }
    return best;
}

// ── Apple (§4.4): the platform's own conventions, on every host ────────────

// The SDK a statement names, or the reason there is none. `name` is the row's
// xcrun SDK name. `SDKROOT` is honoured on every host; `xcrun` and the
// developer directory only exist on an Apple one (inside `sdk_path`).
inline std::expected<AppleSdk, std::string>
locate_apple(const Statement& s, std::string_view name) {
    namespace mac = mcpp::toolchain::apple;
    AppleSdk sdk;
    sdk.name = std::string(name);
    std::error_code ec;
    if (s.form == Form::Path) {
        if (!fs::is_directory(s.path, ec))
            return std::unexpected(std::format("{} does not exist", s.path.string()));
        if (!fs::exists(s.path / "SDKSettings.json", ec) && !fs::exists(s.path / "usr" / "include", ec))
            return std::unexpected(std::format(
                "{} is not an Apple SDK: it has neither SDKSettings.json nor usr/include",
                s.path.string()));
        sdk.root = s.path;
    } else {
        auto found = mac::sdk_path(name);
        if (!found) {
            const auto env = mcpp::invocation::get("SDKROOT");
            return std::unexpected(std::format(
                "looked in: SDKROOT ({}){}", env.value_or("unset"),
                mcpplibs::os::is_macos ? ", xcrun, xcode-select and the standard locations" : ""));
        }
        sdk.root = *found;
    }
    sdk.version = mac::sdk_settings_version(sdk.root);
    if (sdk.version.empty())
        if (auto v = mac::sdk_version(name)) sdk.version = *v;
    if (s.form == Form::Locator && s.version != "system" && !s.version.empty()
        && !(sdk.version == s.version || sdk.version.starts_with(s.version + ".")))
        return std::unexpected(std::format(
            "the {} SDK found at {} is version {}, and {}@{} was asked for",
            name, sdk.root.string(), sdk.version.empty() ? "unknown" : sdk.version,
            name, s.version));
    return sdk;
}

} // namespace mcpp::toolchain::base
