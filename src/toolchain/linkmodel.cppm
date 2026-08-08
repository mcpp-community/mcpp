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

// Identity escape — for the argv consumers, which hand tokens straight to
// exec and must NOT carry ninja `$` escapes or shell quotes.
inline std::string no_escape(const std::filesystem::path& p) { return p.string(); }

// Render argv tokens as the leading-space-separated string the ninja and
// shell channels want.
//
// Tokens are the source of truth and the string is derived, not the other way
// round. Historically only the string form existed, so the one consumer that
// needs argv (build.mcpp, which execs directly with no shell) could not use
// this seam at all and hand-wrote the whole assembly a second time — the
// duplication that 2026-08-02-host-compile-single-producer-design.md exists to
// remove. Splitting a rendered string back into tokens is not a substitute:
// it cannot tell a space *inside* a token from a separator.
inline std::string render_tokens(const std::vector<std::string>& tokens) {
    std::string out;
    for (auto const& t : tokens) { out += ' '; out += t; }
    return out;
}

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
    bool clangDriver   = false;  // clang: -isystem headers; gcc: -idirafter
                                 // gcc:   -idirafter (…#include_next), -B/-L only
    bool clangWithCfg  = false;  // sibling <driver>.cfg exists (bundled LLVM)

    // Compile-side flags as argv tokens. Each entry is ONE argv word.
    std::vector<std::string> compile_tokens(const PathEscape& esc) const {
        std::vector<std::string> out;
        if (mode == CLibMode::Sysroot)
            out.push_back("--sysroot=" + esc(sysroot));
        // PayloadFirst headers: clang takes -isystem; GCC needs -idirafter so
        // libstdc++'s #include_next wrappers (which only search *after* the
        // current dir, and GCC's built-ins are last) can still reach libc.
        // A Sysroot-mode supplement (kernel headers missing from the sysroot)
        // is -isystem for both: the libc headers come from the sysroot there.
        const char* incFlag = (mode == CLibMode::Sysroot || clangDriver)
                            ? "-isystem" : "-idirafter";
        for (auto& inc : systemIncludes)
            out.push_back(incFlag + esc(inc));
        return out;
    }

    // The string channel, derived from the tokens above (leading-space
    // separated, matching the historical assembly style of
    // flags.cppm/stdmod.cppm byte for byte).
    std::string compile_flags(const PathEscape& esc) const {
        return render_tokens(compile_tokens(esc));
    }

    // Link-side flags as argv tokens. `-B` is the CRT-discovery fix for #195:
    // the driver resolves crt objects through -B prefixes and sysroot paths,
    // never through -L.
    std::vector<std::string> link_tokens(const PathEscape& esc) const {
        std::vector<std::string> out;
        if (mode == CLibMode::Sysroot) {
            out.push_back("--sysroot=" + esc(sysroot));
            // Sysroot mode still needs the interpreter named explicitly when
            // we know it. mcpp replaces gcc's `*link:` to stop the payload's
            // accumulated rpath entries reaching artifacts, and the pristine
            // spec it restores carries the HOST's default loader — so removing
            // the addressing without supplying a replacement would silently
            // hand every artifact /lib64/ld-linux. The hermeticity check
            // catches that, which is how it was found.
            if (!loader.empty())
                out.push_back("-Wl,--dynamic-linker=" + esc(loader));
            // ...and the runtime search path, for the same reason. Replacing
            // `*link:` removes the rpath the payload's specs used to inject
            // along with the loader; supplying one without the other produces
            // a binary whose interpreter is right and whose libm is not found.
            for (auto& dir : libDirs) {
                out.push_back("-L" + esc(dir));
                out.push_back("-Wl,-rpath," + esc(dir));
            }
            return out;
        }
        if (mode != CLibMode::PayloadFirst) return out;
        if (!crtDir.empty()) out.push_back("-B" + esc(crtDir));
        for (auto& dir : libDirs) {
            out.push_back("-L" + esc(dir));
            out.push_back("-Wl,-rpath," + esc(dir));
        }
        // Emitted for BOTH drivers.
        //
        // GCC used to be left to its specs here, on the theory that the
        // install-time fixup owned the loader. That made the RUN side a
        // per-toolchain-install decision while the COMPILE side stayed
        // per-build, and the two named different glibc versions the moment a
        // second one was installed.
        //
        // mcpp already refuses to depend on clang's install-time cfg
        // (`--no-default-config`, "reproducible builds, no dependence on the
        // install-time-generated cfg"). The same reasoning had simply never
        // been applied to gcc. Measured: an explicit -Wl,--dynamic-linker
        // overrides what the specs inject.
        if (!loader.empty())
            out.push_back("-Wl,--dynamic-linker=" + esc(loader));
        return out;
    }

    std::string link_flags(const PathEscape& esc) const {
        return render_tokens(link_tokens(esc));
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

    // "--no-default-config" "-nostdinc++" "-isystem<...>" (compile side), as
    // argv tokens. -stdlib=libc++ is deliberately left to callers: compile
    // commands for C files must not carry it.
    // `stdlibSelect` inserts `-stdlib=libc++` immediately after `-nostdinc++`,
    // where the std module build has always put it. Position is not free
    // here: the rendered string is part of the std cache identity, so moving
    // the flag would invalidate every user's std BMIs for no behavioural gain.
    std::vector<std::string> compile_tokens(const PathEscape& esc,
                                            bool stdlibSelect = false) const {
        std::vector<std::string> out{"--no-default-config", "-nostdinc++"};
        if (stdlibSelect) out.push_back("-stdlib=libc++");
        for (auto& inc : cxxIncludes) out.push_back("-isystem" + esc(inc));
        return out;
    }

    std::string compile_flags(const PathEscape& esc) const {
        return render_tokens(compile_tokens(esc));
    }

    // Link-side driver selection, matching the cfg xlings generates.
    static constexpr std::string_view kLinkDriverFlags =
        " -stdlib=libc++ -fuse-ld=lld --rtlib=compiler-rt --unwindlib=libunwind";

    // Same, as argv tokens, WITHOUT `-stdlib=libc++`: a driver invocation that
    // both compiles and links (build.mcpp) already carries it on the compile
    // side via HostFlagOptions::clangStdlibSelect, and repeating it is noise.
    // Plus the libc++/compiler-rt library dirs, which a self-contained host
    // helper needs to both link against and find at run time.
    std::vector<std::string> link_tokens(const PathEscape& esc) const {
        std::vector<std::string> out{"-fuse-ld=lld", "--rtlib=compiler-rt",
                                     "--unwindlib=libunwind"};
        for (auto& d : libDirs) {
            out.push_back("-L" + esc(d));
            out.push_back("-Wl,-rpath," + esc(d));
        }
        return out;
    }
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

    // The compiler's OWN runtime lives beside it, not in the C library:
    // libgcc_s.so.1 for GCC. A produced binary links it whether or not the
    // build ever mentions it, so its directory has to be on the artifact's
    // RUNPATH -- and gcc's patched specs used to put it there, which is
    // exactly why removing that rewrite has to put it back.
    //
    // Both modes need this, and only one got it at first. PayloadFirst
    // artifacts came out with a single RUNPATH entry (the glibc payload) and
    // resolved libgcc_s.so.1 from the HOST -- or, on a machine without one,
    // not at all: `error while loading shared libraries: libgcc_s.so.1`. This
    // developer machine has a usable sysroot, so every local build took the
    // other branch and the gap only surfaced on CI (e2e 29).
    //
    // Not for musl (self-contained sysroot, static world) and not for clang,
    // which brings compiler-rt and libunwind instead.
    auto add_compiler_runtime_dir = [&] {
        if (is_musl_target(tc) || lm.clangDriver || tc.binaryPath.empty()) return;
        std::error_code lec;
        auto gccLib = tc.binaryPath.parent_path().parent_path() / "lib64";
        if (std::filesystem::exists(gccLib, lec))
            lm.libDirs.push_back(gccLib);
    };

    auto payload_first = [&] {
        auto& pp = *tc.payloadPaths;
        lm.mode   = CLibMode::PayloadFirst;
        lm.crtDir = pp.glibcLib;
        lm.libDirs.push_back(pp.glibcLib);
        add_compiler_runtime_dir();
        lm.systemIncludes.push_back(pp.glibcInclude);
        if (!pp.linuxInclude.empty())
            lm.systemIncludes.push_back(pp.linuxInclude);
        // Resolved for both drivers now that both emit it.
        lm.loader = resolve_loader(pp.glibcLib, tc.targetTriple);
    };
    auto sysroot_mode = [&](const std::filesystem::path& root) {
        lm.mode    = CLibMode::Sysroot;
        lm.sysroot = root;
        // Name the interpreter even here: see link_tokens. The payload is the
        // address (R6 — artifacts bind the payload, never the mutable view),
        // and it is known whenever a runtime binding resolved.
        //
        // The pair mirrors exactly what fixup_gcc_specs used to bake in
        // (`<glibcLib>:<gccLib>`): the C library, and the compiler's own
        // runtime (libgcc_s, libstdc++). Emitting it per build is the whole
        // point — the baked copy was a per-toolchain-install decision that no
        // longer matched the per-build one, and it accumulated one dead entry
        // per home that ever touched the shared payload.
        //
        // Never for a musl target. Its sysroot is self-contained -- it brings
        // its own libc, CRT and loader -- and the glibc payload here belongs
        // to the HOST toolchain, which merely happens to be probed alongside.
        // Putting it on the link path let ld pull glibc's static libc.a into a
        // musl link: `undefined reference to _DYNAMIC`, `hidden symbol
        // _DYNAMIC isn't defined`, from dl-reloc-static-pie.o. The musl branch
        // below already says this about headers; it holds for libraries and
        // the loader too.
        if (!is_musl_target(tc)
            && tc.payloadPaths && !tc.payloadPaths->glibcLib.empty()) {
            lm.loader = resolve_loader(tc.payloadPaths->glibcLib, tc.targetTriple);
            lm.libDirs.push_back(tc.payloadPaths->glibcLib);
        }
        add_compiler_runtime_dir();
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
