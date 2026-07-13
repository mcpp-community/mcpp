// mcpp.build.flags — shared compile/link flag computation.
//
// Extracts all flag logic from ninja_backend.cppm into a single point
// of truth so both the ninja backend and compile_commands.json emitter
// (and future backends) share identical flag sets.
//
// See .agents/docs/2026-05-12-compile-commands-design.md.

module;
#include <cstdlib>

export module mcpp.build.flags;

import std;
import mcpp.build.plan;
import mcpp.platform;
import mcpp.toolchain.clang;
import mcpp.toolchain.detect;
import mcpp.toolchain.dialect;
import mcpp.toolchain.linkmodel;
import mcpp.toolchain.provider;
import mcpp.toolchain.registry;

export namespace mcpp::build {

struct CompileFlags {
    std::string cxx;                  // full cxxflags string
    std::string cc;                   // full cflags string
    std::string ld;                   // ldflags string
    std::filesystem::path cxxBinary;  // g++ / clang++
    std::filesystem::path ccBinary;   // gcc / clang (derived)
    std::filesystem::path arBinary;   // ar path (may be empty → use PATH)
    std::string sysroot;              // --sysroot=... (for ninja ldflags)
    std::string bFlag;                // -B<binutils> (for ninja ldflags)
    bool staticStdlib = true;
    std::string linkage;  // "static" or ""
    // macOS per-unit C++ stdlib link (appended via unit_ldflags):
    // distributable targets get the static LLVM libc++ (portable across
    // macOS versions); TestBinary targets get the toolchain's own libc++
    // DYNAMICALLY (-L + -lc++ + rpath into the toolchain) — host-only
    // binaries, so the rpath is fine, and it keeps headers and dylib the
    // same version (the system -lc++ they used before was a version split
    // that broke on libc++ 22's out-of-line __hash_memory). Empty on other
    // platforms (stdlib handled by their existing paths).
    std::string ldStdlibDefault;
    std::string ldStdlibTest;
};

CompileFlags compute_flags(const BuildPlan& plan);

// Return the linker flag that pulls in libatomic, or "" when it should be
// omitted. libatomic carries the out-of-line __atomic_* libcalls that
// 16-byte / oversized std::atomic lowers to (a GCC runtime lib — LLVM ships
// no equivalent, and compiler drivers don't auto-link it), so a genuine
// atomic user otherwise fails at link with `undefined __atomic_*`. We guard
// it with --as-needed so binaries that don't use it get no dependency. But
// --as-needed does NOT skip a missing library (the linker still has to open
// it), so the flag is emitted ONLY when a link-resolvable libatomic actually
// exists on one of the toolchain's link dirs — otherwise it would break
// toolchains that ship no libatomic at all. `staticLink` (a `-static` build,
// e.g. musl targets) narrows the resolvable form to `libatomic.a`; a dynamic
// link also accepts `libatomic.so`.
std::string atomic_link_flag(const std::vector<std::filesystem::path>& linkDirs,
                             bool staticLink);

}  // namespace mcpp::build

namespace mcpp::build {

namespace {

std::filesystem::path staged_std_bmi_path(const BuildPlan& plan) {
    return mcpp::toolchain::staged_std_bmi_path(plan.toolchain, plan.outputDir);
}

// Escape a path for embedding in ninja rule strings.
std::string escape_path(const std::filesystem::path& p) {
    auto s = p.string();
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == ' ' || c == '$' || c == ':')
            out.push_back('$');
        out.push_back(c);
    }
    return out;
}

std::string normalize_ldflag(const std::filesystem::path& root, const std::string& flag) {
    auto absolute_path = [&](std::string_view raw) {
        std::filesystem::path p{std::string(raw)};
        if (p.is_absolute() || raw.starts_with("$")) return p;
        return root / p;
    };

    if (flag.starts_with("-L") && flag.size() > 2) {
        return "-L" + escape_path(absolute_path(std::string_view(flag).substr(2)));
    }

    constexpr std::string_view rpathPrefix = "-Wl,-rpath,";
    if (flag.starts_with(rpathPrefix) && flag.size() > rpathPrefix.size()) {
        return std::string(rpathPrefix)
             + escape_path(absolute_path(std::string_view(flag).substr(rpathPrefix.size())));
    }

    return flag;
}

}  // namespace

std::string atomic_link_flag(const std::vector<std::filesystem::path>& linkDirs,
                             bool staticLink) {
    for (auto& dir : linkDirs) {
        std::error_code ec;
        if (std::filesystem::exists(dir / "libatomic.a", ec)
            || (!staticLink && std::filesystem::exists(dir / "libatomic.so", ec))) {
            return " -Wl,--push-state,--as-needed -latomic -Wl,--pop-state";
        }
    }
    return {};
}

CompileFlags compute_flags(const BuildPlan& plan) {
    CompileFlags f;

    // Central query points for per-toolchain decisions — prefer these over
    // ad-hoc is_clang()/is_gcc() calls:
    //   caps   — what the toolchain can do (scan-deps, stdlib id, …)
    //   d      — how a flag is SPELT (GNU "-I" vs MSVC "/I")
    //   traits — BMI mechanics + module-flag spellings
    auto caps = mcpp::toolchain::capabilities_for(plan.toolchain);
    const auto& d = mcpp::toolchain::dialect_for(plan.toolchain);

    // macOS minimum supported OS version for produced binaries.
    // Precedence: MACOSX_DEPLOYMENT_TARGET env (explicit per-invocation
    // override, the convention cargo/rustc/cc honor) > the manifest's
    // [build] macos_deployment_target (project default, SwiftPM-style) >
    // empty (toolchain/SDK default).
    std::string macosDeploymentTarget = mcpp::platform::macos::deployment_target(
        plan.manifest.buildConfig.macosDeploymentTarget);

    f.cxxBinary = plan.toolchain.binaryPath;
    f.ccBinary = mcpp::toolchain::derive_c_compiler(plan.toolchain);

    // PIC?
    bool need_pic = false;
    for (auto& lu : plan.linkUnits) {
        if (lu.kind == LinkUnit::SharedLibrary) {
            need_pic = true;
            break;
        }
    }
    std::string pic_flag = need_pic ? " -fPIC" : "";

    // Include dirs
    std::string include_flags;
    for (auto& inc : plan.manifest.buildConfig.includeDirs) {
        auto abs = inc.is_absolute() ? inc : (plan.projectRoot / inc);
        include_flags += std::format(" {}{}", d.includePrefix, escape_path(abs));
    }

    // Sysroot / payload paths — resolved ONCE by the toolchain link model
    // (mcpp.toolchain.linkmodel, the single source of truth shared with
    // stdmod / build_program / the cfg fixup; see
    // .agents/docs/2026-07-07-hermetic-toolchain-link-model-design.md).
    // Payload-first, --sysroot fallback; for Clang with a cfg file we bypass
    // the (install-time-generated, non-reproducible) cfg with
    // --no-default-config and provide everything explicitly.
    const auto dm = mcpp::toolchain::resolve_clang_driver(plan.toolchain);
    const auto lm = mcpp::toolchain::resolve_link_model(plan.toolchain);
    const mcpp::toolchain::PathEscape ninjaEsc =
        [](const std::filesystem::path& p) { return escape_path(p); };

    std::string compile_toolchain_flags;
    std::string link_toolchain_flags;
    const bool isClangWithCfg = dm.hasCfg;
    // LLVM root of a clang-with-cfg toolchain — used by the macOS link
    // path below to locate libc++.a/libc++abi.a for staticStdlib.
    std::filesystem::path llvmRootForStdlib;

    if (isClangWithCfg) {
        // --no-default-config -nostdinc++ + libc++ headers.
        compile_toolchain_flags = dm.compile_flags(ninjaEsc);
        // macOS deployment target: make the resolved value explicit on
        // the command line so (a) the ninja commands don't depend on env
        // propagation and (b) the value participates in the BMI
        // fingerprint via canonical flags — mixing targets in one sandbox
        // otherwise reuses a std.pcm built for a different
        // arm64-apple-macosxNN triple and dies with a config mismatch
        // (observed on macos CI). The link side is added to f.ld below
        // (the macOS link path doesn't consume link_toolchain_flags).
        if (mcpp::platform::is_macos && !macosDeploymentTarget.empty()) {
            compile_toolchain_flags +=
                " -mmacosx-version-min=" + macosDeploymentTarget;
        }
        llvmRootForStdlib = dm.llvmRoot;
        // C library headers (payload -isystem, or --sysroot fallback).
        compile_toolchain_flags += lm.compile_flags(ninjaEsc);
        // Linker flags that cfg normally provides. The payload C-runtime
        // flags (-B/-L/loader) are appended via payload_ld below.
        link_toolchain_flags = " --no-default-config";
        if (lm.mode == mcpp::toolchain::CLibMode::Sysroot)
            link_toolchain_flags += lm.link_flags(ninjaEsc);
        link_toolchain_flags +=
            mcpp::toolchain::ClangDriverModel::kLinkDriverFlags;
        f.sysroot = link_toolchain_flags;
    } else if (lm.mode != mcpp::toolchain::CLibMode::None) {
        // GCC (or Clang without cfg): --sysroot from probe, or the payload
        // headers + C runtime (-B for crt discovery, -L for -lc/-lm).
        compile_toolchain_flags = lm.compile_flags(ninjaEsc);
        link_toolchain_flags = lm.link_flags(ninjaEsc);
        f.sysroot = link_toolchain_flags;
    }

    // Binutils -B flag — a GCC/libstdc++ payload concern (musl bundles its
    // own as/ld; Clang and MSVC never take an external binutils).
    bool isMuslTc = mcpp::toolchain::is_musl_target(plan.toolchain);
    std::filesystem::path binutilsBin;
    if (!isMuslTc && caps.stdlib_id == "libstdc++") {
        auto ar = mcpp::toolchain::archive_tool(plan.toolchain);
        if (!ar.empty())
            binutilsBin = ar.parent_path();
    }
    std::string b_flag;
    if (!binutilsBin.empty()) {
        b_flag = " -B" + escape_path(binutilsBin);
        f.bFlag = b_flag;
    }

    // AR binary
    f.arBinary = mcpp::toolchain::archive_tool(plan.toolchain);

    // Opt level + debug come from the resolved build profile
    // ([profile.<name>] → buildConfig). musl keeps -Og as an ICE workaround
    // unless the profile pins -O0.
    auto& prof = plan.manifest.buildConfig;
    std::string opt_flag = isMuslTc && prof.optLevel != "0"
        ? " -Og" : std::format(" {}{}", d.optPrefix, prof.optLevel);
    if (prof.debug) opt_flag += std::format(" {}", d.debugFlags);
    if (prof.lto)   opt_flag += " -flto";

    // User link flags
    std::string user_ldflags;
    for (auto const& flag : plan.manifest.buildConfig.ldflags) {
        user_ldflags += ' ';
        user_ldflags += normalize_ldflag(plan.projectRoot, flag);
    }

    // C standard
    std::string c_std =
        plan.manifest.buildConfig.cStandard.empty() ? "c11" : plan.manifest.buildConfig.cStandard;

    // Assemble
    // Module-flag spellings come from BmiTraits: GCC needs -fmodules on every
    // TU (BMIs implicit); Clang/MSVC reference the staged std BMI and a BMI
    // search dir explicitly (spelled -fmodule-file=/-fprebuilt-module-path vs
    // /reference//ifcSearchDir).
    auto traits = mcpp::toolchain::bmi_traits(plan.toolchain);
    std::string module_flag{traits.compileModulesFlag};
    std::string std_module_flag;
    if (!traits.stdBmiUsePrefix.empty() && !plan.stdBmiPath.empty()) {
        std_module_flag = std::string(traits.stdBmiUsePrefix)
                        + escape_path(staged_std_bmi_path(plan));
    }
    std::string std_compat_module_flag;
    if (!traits.stdCompatBmiUsePrefix.empty() && !plan.stdCompatBmiPath.empty()) {
        // NOTE: staging path is Clang's today; registry-dispatch when the
        // MSVC backend lands (std.compat.ixx staging).
        auto compatDst = mcpp::toolchain::clang::staged_std_compat_bmi_path(plan.outputDir);
        std_compat_module_flag = std::string(traits.stdCompatBmiUsePrefix)
                               + escape_path(compatDst);
    }
    std::string prebuilt_module_flag;
    if (traits.needsPrebuiltModulePath) {
        // Absolute path: a bare `pcm.cache` / `gcm.cache` works at ninja
        // time because ninja runs commands with cwd = outputDir, but the
        // same flag ends up verbatim in `compile_commands.json` whose
        // `directory` field is the project root. clangd does `cd directory`
        // before resolving the flag, so a bare relative path points at
        // `<projectRoot>/pcm.cache` (which doesn't exist) and `import`
        // resolution fails with `module 'X' not found`. The other
        // `-fmodule-file=` flags in this block are already escape_path'd
        // (absolute) for the same reason — this one was a leftover.
        prebuilt_module_flag = std::string(traits.bmiSearchPrefix)
                             + escape_path(plan.outputDir / traits.bmiDir);
    }
    std::string cxx_std_flag =
        plan.cppStandardFlag.empty()
            ? std::format("{}c++23", d.stdPrefix) : plan.cppStandardFlag;
    f.cxx = std::format("{}{}{}{}{}{}{}{}{}{}", cxx_std_flag, module_flag, std_module_flag,
                        std_compat_module_flag, prebuilt_module_flag,
                        opt_flag, pic_flag, compile_toolchain_flags, b_flag, include_flags);
    f.cc = std::format("{}{}{}{}{}{}{}", d.stdPrefix, c_std, opt_flag, pic_flag,
                       compile_toolchain_flags, b_flag, include_flags);

    // Link flags
    f.staticStdlib = plan.manifest.buildConfig.staticStdlib;
    f.linkage = plan.manifest.buildConfig.linkage;
    std::string full_static = (mcpp::platform::supports_full_static && f.linkage == "static") ? " -static" : "";
    // Static C++ stdlib: a libstdc++ (GCC/MinGW) concern. On Windows a MinGW
    // toolchain also statically links libgcc, so produced binaries don't
    // depend on libstdc++-6.dll / libgcc_s DLLs from the toolchain dir
    // (portable-by-default, same spirit as the macOS static-libc++ path).
    std::string static_stdlib;
    if (f.staticStdlib && caps.stdlib_id == "libstdc++") {
        static_stdlib = " -static-libstdc++";
        if constexpr (mcpp::platform::is_windows)
            static_stdlib += " -static-libgcc";
    }
    std::string runtime_dirs;
    if constexpr (mcpp::platform::supports_rpath) {
        // Toolchain runtime dirs (glibc/gcc) as before...
        for (auto& dir : plan.toolchain.linkRuntimeDirs) {
            runtime_dirs += " -L" + escape_path(dir);
            runtime_dirs += " -Wl,-rpath," + escape_path(dir);
        }
        // ...plus dependency packages' [runtime] library_dirs (e.g.
        // compat.glx-runtime's host-GL passthrough), so dlopen()'d host libs
        // (libGL/libGLX) are reachable at run time. Only the dep dirs — NOT the
        // glibc payload dir — so static/musl links stay clean.
        for (auto& dir : plan.depRuntimeLibraryDirs) {
            runtime_dirs += " -L" + escape_path(dir);
            runtime_dirs += " -Wl,-rpath," + escape_path(dir);
        }
    }

    // For Clang with payload paths: the payload C runtime — -B so the driver
    // resolves Scrt1.o/crti.o/crtn.o inside the payload (the driver never
    // consults -L for CRT objects; without -B it silently falls back to the
    // host's /lib or, on hosts without a system toolchain, passes bare names
    // that lld cannot open — issue #195), -L/-rpath for -lc/-lm, and the
    // payload's dynamic linker.
    std::string payload_ld;
    if (isClangWithCfg && lm.mode == mcpp::toolchain::CLibMode::PayloadFirst)
        payload_ld = lm.link_flags(ninjaEsc);

    std::string link_extra;
    if (prof.lto)   link_extra += " -flto";
    if (prof.strip) link_extra += " -s";

    if constexpr (mcpp::platform::is_windows) {
        // PE link: no rpath/loader/payload model. MSVC-ABI Clang needs
        // nothing extra (MSVC STL/SDK via the driver); MinGW adds the static
        // libstdc++/libgcc pair (static_stdlib above) and -B so its own
        // binutils resolve, plus `-static` for full static when requested
        // (MinGW supports it, unlike MSVC-ABI links).
        std::string mingw_static;
        std::string mingw_stdexp;
        if (caps.stdlib_id == "libstdc++") {
            // `-static` for the whole link — winlibs' own recommendation for
            // standalone exes. The piecemeal recipe (-static-libstdc++ +
            // -Wl,-Bstatic -lwinpthread) verifiably loses to the driver's
            // implicit closing libs: CI import tables still showed
            // libwinpthread-1.dll. System DLLs (KERNEL32/UCRT) still resolve
            // via their import libs. Tied to staticStdlib so
            // [build] static_stdlib=false opts back into DLL-coupled links.
            if (f.staticStdlib || f.linkage == "static")
                mingw_static = " -static";
            // std::print's terminal probe (__open_terminal /
            // __write_to_terminal, bits/print.h) lives in libstdc++exp.a on
            // Windows targets — plain -lstdc++ leaves them undefined.
            mingw_stdexp = " -lstdc++exp";
        }
        f.ld = std::format("{}{}{}{}{}{}", mingw_static, static_stdlib, b_flag,
                           user_ldflags, mingw_stdexp, link_extra);
    } else if constexpr (mcpp::platform::needs_explicit_libcxx) {
        // macOS. Two min-version concerns (see xlings
        // .agents/docs/2026-06-05-macos-min-version-support.md):
        //
        // 1. stdlib linkage — `-lc++` resolves to the SYSTEM
        //    /usr/lib/libc++.1.dylib, which caps the deployment floor at
        //    the build host's OS: e.g. std::print's __is_posix_terminal
        //    support symbol only exists in macOS 15's libc++, so a
        //    minos-14 binary dies at launch on 14 (dyld missing-symbol
        //    abort; verified on macos-14 CI). With staticStdlib (the
        //    manifest default — previously silently ignored on the clang
        //    route), link LLVM's own libc++.a/libc++abi.a instead:
        //    runtime deps shrink to libSystem and the floor drops to
        //    14.0 — the floor of the official LLVM static archives;
        //    lower needs a custom libc++ build. Falls back to -lc++ when the
        //    archives are absent.
        // 2. deployment target — mirror MACOSX_DEPLOYMENT_TARGET onto the
        //    link command line so it doesn't depend on env propagation.
        // 3. linker — use LLVM's own lld (same as the Linux clang path)
        //    instead of Xcode's ld: the system ld's version floats with
        //    the host Xcode (observed: Xcode 15.4's ld aborting at launch
        //    on macos-14 CI when its libc++ resolution was diverted), and
        //    lld ships with the exact toolchain doing the compile.
        f.ldStdlibDefault = " -lc++";
        f.ldStdlibTest    = " -lc++";
        // Static libc++ + the deployment floor are the DEFAULT (rust-style
        // "portable by default"): the resolver always yields a floor on
        // macOS (built-in 14.0 unless env/manifest override), and the
        // static LLVM libc++ is what makes that floor real — the system
        // libc++ caps binaries at the build host's OS (a fresh user's
        // std::println hello on macOS 14 died at dyld against the system
        // libc++ before this). Opt-out: [build] static_stdlib = false
        // (host-coupled dynamic libc++, the pre-0.0.52 no-declaration
        // behavior). The two blockers that deferred this default are
        // resolved: (1) mixed C/C++ split-brain SIGSEGV — fixed by
        // -load_hidden (PR #117 forensics), (2) std-module staging /
        // fingerprint drift — fixed by the single resolver (PR #119).
        // TODO(macos-floor-11): the official LLVM archives are built for
        // macOS 14; supporting 11-13 needs a custom libc++ build shipped
        // via xlings-res (data-only change — swap the archive source).
        // Tracked in xlings
        // .agents/docs/2026-06-05-macos-min-version-support.md §5.
        if (f.staticStdlib && !macosDeploymentTarget.empty()
            && !llvmRootForStdlib.empty()) {
            auto libDir     = llvmRootForStdlib / "lib";
            auto libcxxA    = libDir / "libc++.a";
            auto libcxxAbiA = libDir / "libc++abi.a";
            if (std::filesystem::exists(libcxxA)
                && std::filesystem::exists(libcxxAbiA)) {
                // Link the archives via -Wl,-load_hidden,<path>: forces
                // the ARCHIVE (never a sibling dylib) and gives its
                // symbols hidden visibility. Both properties matter:
                //  - plain BY-PATH linking leaves default-visibility
                //    symbols that dyld then unifies with the system
                //    libc++ pulled in via the shared cache — a
                //    split-brain libc++ where ostream<<int crosses into
                //    the system copy's locale machinery and SIGSEGVs
                //    (CI forensics m1/m3 vs m6/m7).
                //  - -Wl,-hidden-l resolves like a plain -l under lld
                //    and picks the sibling DYLIB (load failure).
                f.ldStdlibDefault = " -nostdlib++"
                    " -Wl,-load_hidden," + escape_path(libcxxA)
                  + " -Wl,-load_hidden," + escape_path(libcxxAbiA);
            }
        }
        // TestBinary: SAME static -load_hidden libc++ as distributables.
        // Tests previously took the SYSTEM -lc++ while compiling against the
        // toolchain's libc++ HEADERS — a header/dylib version split that
        // detonated when libc++ 22 moved string hashing out of line
        // (undefined __hash_memory, 2026-07-08). The dynamic alternative
        // (toolchain libc++.dylib + rpath) is a dead end with this
        // distribution: its abi/unwind dylibs upward-link /usr/lib/libc++,
        // so the SYSTEM libc++ still loads next to the toolchain's and
        // gtest's initializers freed across the two copies
        // (BUG_IN_CLIENT_OF_LIBMALLOC, CI crash forensics rounds 5-6).
        // Static hidden archives keep exactly ONE libc++, inside the
        // binary — the same already-proven shape mcpp/xlings ship with.
        // Design: .agents/docs/2026-07-08-root-cause-remediation-design.md A1.
        if (!llvmRootForStdlib.empty()) {
            auto libDir     = llvmRootForStdlib / "lib";
            auto libcxxA    = libDir / "libc++.a";
            auto libcxxAbiA = libDir / "libc++abi.a";
            if (std::filesystem::exists(libcxxA)
                && std::filesystem::exists(libcxxAbiA)) {
                f.ldStdlibTest = " -nostdlib++"
                    " -Wl,-load_hidden," + escape_path(libcxxA)
                  + " -Wl,-load_hidden," + escape_path(libcxxAbiA);
            }
        }
        std::string version_min;
        if (!macosDeploymentTarget.empty()) {
            version_min = " -mmacosx-version-min=" + macosDeploymentTarget;
        }
        // Pass the macOS SDK to the LINKER explicitly. The link otherwise relies
        // on clang's implicit SDK detection (xcrun/SDKROOT → ld64 -syslibroot)
        // to resolve -lSystem and friends. On a clean Xcode (CI) that works, so
        // the gap is latent; but on a machine where that detection fails —
        // misconfigured `xcode-select`, Command-Line-Tools-only, or a freshly
        // installed bundled clang — ld64.lld dies with "library not found for
        // -lSystem". -isysroot makes it deterministic regardless of the host's
        // developer-tools state. (compile side already gets --sysroot above.)
        std::string macos_sdk;
        if (auto sdk = mcpp::platform::macos::sdk_path())
            macos_sdk = " -isysroot " + escape_path(*sdk);
        f.ld = std::format("{}{}{}{} -fuse-ld=lld{}{}{}", full_static, static_stdlib,
                           b_flag, macos_sdk, version_min, user_ldflags, link_extra);
    } else {
        // libatomic: 16-byte / oversized std::atomic needs the out-of-line
        // __atomic_* libcalls from libatomic, which the driver won't add on
        // its own. Inject `-latomic` (under --as-needed) after runtime_dirs
        // so its -L entries are on the search path; self-guards on the lib
        // actually being present (see atomic_link_flag).
        std::string atomic_ld = atomic_link_flag(plan.toolchain.linkRuntimeDirs,
                                                 !full_static.empty());
        f.ld = std::format("{}{}{}{}{}{}{}{}{}", full_static, static_stdlib, link_toolchain_flags, b_flag,
                           runtime_dirs, atomic_ld, payload_ld, user_ldflags, link_extra);
    }

    return f;
}

}  // namespace mcpp::build
