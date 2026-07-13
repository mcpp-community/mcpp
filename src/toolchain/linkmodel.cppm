// mcpp.toolchain.linkmodel — the single resolver for "how do we compile and
// link against this toolchain's C library" on Linux (glibc payload / sysroot
// worlds) plus the Clang cfg-bypass driver model.
//
// Motivation (issue #195, .agents/docs/2026-07-07-hermetic-toolchain-link-model-design.md):
// this knowledge used to live in four divergent copies (flags.cppm,
// stdmod.cppm, build_program.cppm host_base_flags, post_install.cppm
// fixup_clang_cfg) — the link side of one copy lost the CRT discovery prefix
// (-B) and every copy hardcoded the x86_64 loader name. All consumers now
// derive their flags from ToolchainLinkModel / ClangDriverModel so they can
// never diverge again, and the loader comes from data (per-arch triple map,
// then a glob of the payload contents), never from a hardcoded string.
//
// Scope: the C-library axis only — CRT dir, libc lib dirs, dynamic linker,
// libc/kernel headers. Driver-level flags that are not about the C library
// (opt level, modules, macOS deployment target, …) stay with the consumers.

export module mcpp.toolchain.linkmodel;

import std;
import mcpp.platform;
import mcpp.toolchain.model;

export namespace mcpp::toolchain {

enum class CLibMode {
    None,          // nothing usable found — driver defaults (host) apply;
                   // the hermeticity check reports what actually leaked in.
    PayloadFirst,  // fine-grained glibc/linux-headers xpkg payloads
    Sysroot,       // --sysroot (GCC include-fixed world, musl, macOS SDK)
};

// Escaping differs per consumer (ninja `$`-escaping vs shell quoting), so the
// renderers take an escape callback instead of baking one in. The DEFAULT
// (identity) is only safe for paths already known to be quote-free.
using PathEscape = std::function<std::string(const std::filesystem::path&)>;

struct ToolchainLinkModel {
    CLibMode mode = CLibMode::None;

    // PayloadFirst fields.
    std::filesystem::path crtDir;    // -B: Scrt1.o / crti.o / crtn.o discovery
    std::vector<std::filesystem::path> libDirs;  // -L (+ -rpath for clang)
    std::filesystem::path loader;    // -Wl,--dynamic-linker (clang only; GCC's
                                     // specs fixup owns the loader there)

    // Sysroot fields.
    std::filesystem::path sysroot;

    // Compile-side C library / kernel headers (payload dirs, or the
    // linux-headers supplement for a sysroot that lacks them).
    std::vector<std::filesystem::path> systemIncludes;

    // Rendering knobs derived from the toolchain at resolve time.
    bool clangDriver   = false;  // clang: -isystem + rpath/loader on link
                                 // gcc:   -idirafter (…#include_next), -B/-L only
    bool clangWithCfg  = false;  // sibling <driver>.cfg exists (bundled LLVM)

    // Render the compile-side flags (leading-space separated, matching the
    // historical assembly style of flags.cppm/stdmod.cppm).
    std::string compile_flags(const PathEscape& esc) const {
        std::string out;
        if (mode == CLibMode::Sysroot)
            out += " --sysroot=" + esc(sysroot);
        // PayloadFirst headers: clang takes -isystem; GCC needs -idirafter so
        // libstdc++'s #include_next wrappers (which only search *after* the
        // current dir, and GCC's built-ins are last) can still reach libc.
        // A Sysroot-mode supplement (kernel headers missing from the sysroot)
        // is -isystem for both: the libc headers come from the sysroot there.
        const char* incFlag = (mode == CLibMode::Sysroot || clangDriver)
                            ? " -isystem" : " -idirafter";
        for (auto& inc : systemIncludes)
            out += incFlag + esc(inc);
        return out;
    }

    // Render the link-side flags. `-B` is the CRT-discovery fix for #195:
    // the driver resolves crt objects through -B prefixes and sysroot paths,
    // never through -L.
    std::string link_flags(const PathEscape& esc) const {
        std::string out;
        if (mode == CLibMode::Sysroot) {
            out += " --sysroot=" + esc(sysroot);
            return out;
        }
        if (mode != CLibMode::PayloadFirst) return out;
        if (!crtDir.empty()) out += " -B" + esc(crtDir);
        for (auto& dir : libDirs) {
            out += " -L" + esc(dir);
            if (clangDriver) out += " -Wl,-rpath," + esc(dir);
        }
        if (clangDriver && !loader.empty())
            out += " -Wl,--dynamic-linker=" + esc(loader);
        return out;
    }
};

// Clang cfg-bypass driver model: everything a consumer needs to emit so that
// `--no-default-config` (reproducible builds, no dependence on the
// install-time-generated cfg) still yields a working libc++ toolchain.
struct ClangDriverModel {
    bool hasCfg = false;                       // sibling <driver>.cfg exists
    std::filesystem::path cfgPath;
    std::filesystem::path llvmRoot;            // <bin>/../
    std::vector<std::filesystem::path> cxxIncludes;  // libc++ header dirs
    std::vector<std::filesystem::path> libDirs;      // libc++/compiler-rt libs

    // " --no-default-config -nostdinc++ -isystem<...>" (compile side).
    // -stdlib=libc++ is deliberately left to callers: compile commands for
    // C files must not carry it.
    std::string compile_flags(const PathEscape& esc) const {
        std::string out = " --no-default-config -nostdinc++";
        for (auto& inc : cxxIncludes) out += " -isystem" + esc(inc);
        return out;
    }

    // Link-side driver selection, matching the cfg xlings generates.
    static constexpr std::string_view kLinkDriverFlags =
        " -stdlib=libc++ -fuse-ld=lld --rtlib=compiler-rt --unwindlib=libunwind";
};

// ── loader resolution: data over hardcodes ───────────────────────────────
//
// Priority:
//   1. Triple map (x86_64/aarch64/riscv64/loongarch64 glibc + musl) —
//      loader file names are platform-ABI-level stable conventions.
//   2. Glob: first `ld-*.so*` regular file in the lib dir (covers arches
//      the map doesn't know yet).
//
// A third, declared-metadata source (a persisted `.xpkg-exports.json`
// written by the installer) was evaluated and removed: its only consumer
// would have been this resolver, while the two sources above already cover
// every real payload — see docs/08-toolchain-internals.md for the record.
//
// Returns the loader's absolute path, or empty when none was found (callers
// then omit --dynamic-linker and the hermeticity check reports the gap).

// Loader *file name* for a target triple; empty when the arch is unknown.
std::string loader_filename(std::string_view targetTriple) {
    const bool musl = targetTriple.find("musl") != std::string_view::npos;
    struct ArchLoader { std::string_view arch, gnuName, muslArch; };
    static constexpr std::array<ArchLoader, 5> kMap{{
        {"x86_64",      "ld-linux-x86-64.so.2",          "x86_64"},
        {"aarch64",     "ld-linux-aarch64.so.1",         "aarch64"},
        {"riscv64",     "ld-linux-riscv64-lp64d.so.1",   "riscv64"},
        {"loongarch64", "ld-linux-loongarch-lp64d.so.1", "loongarch64"},
        {"i686",        "ld-linux.so.2",                 "i386"},
    }};
    for (auto& m : kMap) {
        if (targetTriple.starts_with(m.arch)) {
            if (musl) return std::format("ld-musl-{}.so.1", m.muslArch);
            return std::string(m.gnuName);
        }
    }
    return {};
}

// The distro-side loader path a *shipped* binary's PT_INTERP should point at
// (LSB layout), by target triple. Used by `mcpp pack`.
std::string distro_loader_path(std::string_view targetTriple) {
    auto name = loader_filename(targetTriple);
    if (name.empty()) return {};
    if (targetTriple.starts_with("x86_64"))
        return "/lib64/" + name;   // LSB-mandated symlink on glibc distros
    return "/lib/" + name;
}

std::filesystem::path resolve_loader(const std::filesystem::path& libDir,
                                     std::string_view targetTriple) {
    if (libDir.empty()) return {};
    std::error_code ec;

    // 1. Triple map.
    if (auto name = loader_filename(targetTriple); !name.empty()) {
        auto p = libDir / name;
        if (std::filesystem::exists(p, ec)) return p;
    }

    // 2. Glob fallback: ld-*.so*
    for (auto it = std::filesystem::directory_iterator(libDir, ec);
         !ec && it != std::filesystem::directory_iterator{}; it.increment(ec)) {
        auto name = it->path().filename().string();
        if (name.starts_with("ld-") && name.find(".so") != std::string::npos
            && it->is_regular_file(ec))
            return it->path();
    }
    return {};
}

// Locate a glibc payload's lib dir (lib64 preferred, then lib) that actually
// carries a dynamic loader. Replaces the hand-rolled
// `exists(lib64/ld-linux-x86-64.so.2)` probes scattered through
// lifecycle/post_install.
std::filesystem::path payload_lib_dir_with_loader(
    const std::filesystem::path& payloadVersionRoot,
    std::string_view targetTriple = {}) {
    for (auto sub : {"lib64", "lib"}) {
        auto candidate = payloadVersionRoot / sub;
        if (!resolve_loader(candidate, targetTriple).empty())
            return candidate;
    }
    return {};
}

ClangDriverModel resolve_clang_driver(const Toolchain& tc) {
    ClangDriverModel dm;
    if (!is_clang(tc) || tc.binaryPath.empty()) return dm;
    dm.cfgPath = tc.binaryPath.parent_path()
               / (tc.binaryPath.stem().string() + ".cfg");
    dm.hasCfg = std::filesystem::exists(dm.cfgPath);
    if (!dm.hasCfg) return dm;
    dm.llvmRoot = tc.binaryPath.parent_path().parent_path();
    auto libcxxInclude = dm.llvmRoot / "include" / "c++" / "v1";
    dm.cxxIncludes.push_back(libcxxInclude);
    if (!tc.targetTriple.empty()) {
        auto targetInclude = dm.llvmRoot / "include" / tc.targetTriple / "c++" / "v1";
        if (std::filesystem::exists(targetInclude))
            dm.cxxIncludes.push_back(targetInclude);
        auto targetLib = dm.llvmRoot / "lib" / tc.targetTriple;
        if (std::filesystem::exists(targetLib))
            dm.libDirs.push_back(targetLib);
    }
    return dm;
}

ToolchainLinkModel resolve_link_model(const Toolchain& tc) {
    ToolchainLinkModel lm;
    lm.clangDriver  = is_clang(tc);
    lm.clangWithCfg = resolve_clang_driver(tc).hasCfg;

    // PE targets: no ELF loader, no rpath, no glibc payload/sysroot model.
    // MSVC-ABI Clang gets STL+SDK via the driver, MinGW is self-contained —
    // both want CLibMode::None. Keyed on the TARGET (not the host) so the
    // ELF resolution below stays testable anywhere and a future
    // cross-compile resolves by what it builds FOR.
    if (is_msvc_target(tc) || is_mingw_target(tc)) return lm;

    auto payload_first = [&] {
        auto& pp = *tc.payloadPaths;
        lm.mode   = CLibMode::PayloadFirst;
        lm.crtDir = pp.glibcLib;
        lm.libDirs.push_back(pp.glibcLib);
        lm.systemIncludes.push_back(pp.glibcInclude);
        if (!pp.linuxInclude.empty())
            lm.systemIncludes.push_back(pp.linuxInclude);
        if (lm.clangDriver)
            lm.loader = resolve_loader(pp.glibcLib, tc.targetTriple);
    };
    auto sysroot_mode = [&](const std::filesystem::path& root) {
        lm.mode    = CLibMode::Sysroot;
        lm.sysroot = root;
        // Supplement kernel headers when the sysroot lacks them (glibc's
        // local_lim.h needs <linux/limits.h>). Self-contained musl sysroots
        // ship their own; a cross target must not see host-arch headers.
        if (!is_musl_target(tc) && tc.payloadPaths
            && !tc.payloadPaths->linuxInclude.empty()
            && !std::filesystem::exists(root / "usr" / "include" / "linux" / "limits.h"))
            lm.systemIncludes.push_back(tc.payloadPaths->linuxInclude);
    };

    if (lm.clangWithCfg) {
        // Bundled LLVM: payload first (PR #62 principle — the sysroot comes
        // from the toolchain payload, not from an environment directory),
        // then the macOS SDK, then a probed sysroot.
        if (tc.payloadPaths) payload_first();
        else if (auto sdk = mcpp::platform::macos::sdk_path()) {
            lm.mode = CLibMode::Sysroot;
            lm.sysroot = *sdk;
        }
        else if (!tc.sysroot.empty()) sysroot_mode(tc.sysroot);
    } else if (!tc.sysroot.empty()) {
        // GCC (or clang without cfg): --sysroot is required for GCC's
        // include-fixed headers (stdlib.h wrapper).
        sysroot_mode(tc.sysroot);
    } else if (tc.payloadPaths) {
        payload_first();
    }
    return lm;
}

} // namespace mcpp::toolchain
