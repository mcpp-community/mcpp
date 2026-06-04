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
    // macOS versions), TestBinary targets get the system -lc++ — they
    // only ever run on the build host, and statically linked libc++
    // SIGABRTs during static destruction unless the entry point guards
    // with _Exit (mcpp/xlings do; gtest main does not). Empty on other
    // platforms (stdlib handled by their existing paths).
    std::string ldStdlibDefault;
    std::string ldStdlibTest;
};

CompileFlags compute_flags(const BuildPlan& plan);

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

CompileFlags compute_flags(const BuildPlan& plan) {
    CompileFlags f;

    // ProviderCapabilities: centralised query point for per-toolchain decisions.
    // Prefer caps.* checks over ad-hoc is_clang()/is_musl_target() calls for
    // any new branching added to this function.
    auto caps = mcpp::toolchain::capabilities_for(plan.toolchain);

    // macOS minimum supported OS version for produced binaries.
    // Precedence: MACOSX_DEPLOYMENT_TARGET env (explicit per-invocation
    // override, the convention cargo/rustc/cc honor) > the manifest's
    // [build] macos_deployment_target (project default, SwiftPM-style) >
    // empty (toolchain/SDK default).
    std::string macosDeploymentTarget;
    if (const char* dt = std::getenv("MACOSX_DEPLOYMENT_TARGET"); dt && *dt) {
        macosDeploymentTarget = dt;
    } else {
        macosDeploymentTarget = plan.manifest.buildConfig.macosDeploymentTarget;
    }

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
        include_flags += " -I" + escape_path(abs);
    }

    // Sysroot / payload paths.
    //
    // Payload-first: when PayloadPaths are available (glibc + linux-headers
    // xpkgs found), use -isystem for each payload include dir. This avoids
    // dependency on xlings subos.
    //
    // For Clang with a cfg file: use --no-default-config to bypass
    // potentially-stale paths, then provide all flags explicitly.
    //
    // Fallback: if no PayloadPaths, use --sysroot from probe_sysroot().
    std::string compile_toolchain_flags;
    std::string link_toolchain_flags;
    bool isClangWithCfg = false;
    std::filesystem::path cfgPath;
    // LLVM root of a clang-with-cfg toolchain — used by the macOS link
    // path below to locate libc++.a/libc++abi.a for staticStdlib.
    std::filesystem::path llvmRootForStdlib;
    if (mcpp::toolchain::is_clang(plan.toolchain)) {
        cfgPath = plan.toolchain.binaryPath.parent_path()
                  / (plan.toolchain.binaryPath.stem().string() + ".cfg");
        isClangWithCfg = std::filesystem::exists(cfgPath);
    }

    if (isClangWithCfg) {
        // Clang with cfg: bypass cfg and provide all paths explicitly.
        auto llvmRoot = plan.toolchain.binaryPath.parent_path().parent_path();
        auto libcxxInclude = llvmRoot / "include" / "c++" / "v1";
        compile_toolchain_flags = " --no-default-config -nostdinc++";
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
        llvmRootForStdlib = llvmRoot;
        // libc++ headers
        compile_toolchain_flags += " -isystem" + escape_path(libcxxInclude);
        if (!plan.toolchain.targetTriple.empty()) {
            auto targetInclude = llvmRoot / "include"
                                 / plan.toolchain.targetTriple / "c++" / "v1";
            if (std::filesystem::exists(targetInclude))
                compile_toolchain_flags += " -isystem" + escape_path(targetInclude);
        }
        // C library + kernel headers from payload
        if (plan.toolchain.payloadPaths) {
            auto& pp = *plan.toolchain.payloadPaths;
            compile_toolchain_flags += " -isystem" + escape_path(pp.glibcInclude);
            if (!pp.linuxInclude.empty())
                compile_toolchain_flags += " -isystem" + escape_path(pp.linuxInclude);
        } else if (auto sdk = mcpp::platform::macos::sdk_path()) {
            auto sysroot_flag = " --sysroot=" + escape_path(*sdk);
            compile_toolchain_flags += sysroot_flag;
            link_toolchain_flags += sysroot_flag;
        } else if (!plan.toolchain.sysroot.empty()) {
            auto sysroot_flag = " --sysroot=" + escape_path(plan.toolchain.sysroot);
            compile_toolchain_flags += sysroot_flag;
            link_toolchain_flags += sysroot_flag;
        }
        // Linker flags that cfg normally provides
        link_toolchain_flags = " --no-default-config" + link_toolchain_flags
            + " -stdlib=libc++ -fuse-ld=lld --rtlib=compiler-rt --unwindlib=libunwind";
        f.sysroot = link_toolchain_flags;
    } else if (!plan.toolchain.sysroot.empty()) {
        // GCC (or Clang without cfg): use --sysroot from probe.
        // GCC requires --sysroot for include-fixed headers (stdlib.h wrapper).
        // Supplement with -isystem for linux kernel headers from payload
        // if the probed sysroot is missing them.
        auto sysroot_flag = " --sysroot=" + escape_path(plan.toolchain.sysroot);
        compile_toolchain_flags = sysroot_flag;
        link_toolchain_flags = sysroot_flag;
        if (plan.toolchain.payloadPaths && !plan.toolchain.payloadPaths->linuxInclude.empty()) {
            auto sysrootLinux = plan.toolchain.sysroot / "usr" / "include" / "linux" / "limits.h";
            if (!std::filesystem::exists(sysrootLinux))
                compile_toolchain_flags += " -isystem" + escape_path(plan.toolchain.payloadPaths->linuxInclude);
        }
        f.sysroot = link_toolchain_flags;
    } else if (plan.toolchain.payloadPaths) {
        // No usable sysroot: wire the C library headers from the payload.
        // For GCC use -idirafter (appended after the built-in dirs) so that
        // libstdc++'s #include_next wrappers can reach them; -isystem would
        // place them BEFORE the built-ins, invisible to #include_next.
        auto& pp = *plan.toolchain.payloadPaths;
        const bool clangTc = mcpp::toolchain::is_clang(plan.toolchain);
        auto inc_flag = [&](const std::filesystem::path& p) {
            return (clangTc ? " -isystem" : " -idirafter") + escape_path(p);
        };
        compile_toolchain_flags += inc_flag(pp.glibcInclude);
        if (!pp.linuxInclude.empty())
            compile_toolchain_flags += inc_flag(pp.linuxInclude);
        // Link-time C runtime: a usable --sysroot would have provided the
        // startup objects and core libs implicitly. Without one, point the
        // driver at the glibc payload lib dir: -B for crt1.o/crti.o discovery,
        // -L for -lm/-lc resolution.
        link_toolchain_flags += " -B" + escape_path(pp.glibcLib);
        link_toolchain_flags += " -L" + escape_path(pp.glibcLib);
        f.sysroot = link_toolchain_flags;
    }

    // Binutils -B flag
    bool isMuslTc = mcpp::toolchain::is_musl_target(plan.toolchain);
    bool isClang = mcpp::toolchain::is_clang(plan.toolchain);
    std::filesystem::path binutilsBin;
    if (!isMuslTc && !isClang) {
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
        ? " -Og" : (" -O" + prof.optLevel);
    if (prof.debug) opt_flag += " -g";
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
    // -fmodules is a GCC-only flag; Clang uses a different module ABI and does
    // not need it.  caps.stdlib_id distinguishes GCC (libstdc++) from Clang
    // (libc++ / msvc-stl) without an extra is_clang() call.
    std::string module_flag = (caps.stdlib_id == "libstdc++") ? " -fmodules" : "";
    std::string std_module_flag;
    if (isClang && !plan.stdBmiPath.empty()) {
        std_module_flag = " -fmodule-file=std=" + escape_path(staged_std_bmi_path(plan));
    }
    std::string std_compat_module_flag;
    if (isClang && !plan.stdCompatBmiPath.empty()) {
        auto compatDst = mcpp::toolchain::clang::staged_std_compat_bmi_path(plan.outputDir);
        std_compat_module_flag = " -fmodule-file=std.compat=" + escape_path(compatDst);
    }
    auto traits = mcpp::toolchain::bmi_traits(plan.toolchain);
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
        prebuilt_module_flag = std::format(" -fprebuilt-module-path={}",
            escape_path(plan.outputDir / traits.bmiDir));
    }
    std::string cxx_std_flag =
        plan.cppStandardFlag.empty() ? std::string("-std=c++23") : plan.cppStandardFlag;
    f.cxx = std::format("{}{}{}{}{}{}{}{}{}{}", cxx_std_flag, module_flag, std_module_flag,
                        std_compat_module_flag, prebuilt_module_flag,
                        opt_flag, pic_flag, compile_toolchain_flags, b_flag, include_flags);
    f.cc = std::format("-std={}{}{}{}{}{}", c_std, opt_flag, pic_flag, compile_toolchain_flags,
                       b_flag, include_flags);

    // Link flags
    f.staticStdlib = plan.manifest.buildConfig.staticStdlib;
    f.linkage = plan.manifest.buildConfig.linkage;
    std::string full_static = (mcpp::platform::supports_full_static && f.linkage == "static") ? " -static" : "";
    std::string static_stdlib = (f.staticStdlib && !isClang && !mcpp::platform::is_windows) ? " -static-libstdc++" : "";
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

    // For Clang with payload paths: add glibc lib + dynamic linker to link flags.
    std::string payload_ld;
    if (isClangWithCfg && plan.toolchain.payloadPaths) {
        auto& pp = *plan.toolchain.payloadPaths;
        payload_ld += " -L" + escape_path(pp.glibcLib);
        payload_ld += " -Wl,-rpath," + escape_path(pp.glibcLib);
        auto loader = pp.glibcLib / "ld-linux-x86-64.so.2";
        if (std::filesystem::exists(loader))
            payload_ld += " -Wl,--dynamic-linker=" + escape_path(loader);
    }

    std::string link_extra;
    if (prof.lto)   link_extra += " -flto";
    if (prof.strip) link_extra += " -s";

    if constexpr (mcpp::platform::is_windows) {
        f.ld = user_ldflags + link_extra;
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
        // Static libc++ is tied to an EXPLICIT deployment floor: when the
        // user (or the release pipeline) declares a minimum macOS via the
        // env var or [build] macos_deployment_target, the static LLVM
        // libc++ is what makes that floor real (the system libc++ caps it
        // at the build host's OS). With no declared floor, keep the
        // 0.0.49 behavior — dynamic system libc++, host-coupled.
        //
        // TODO(macos-static-default): flip static to the unconditional
        // default (rust-style "portable by default") once two tracked
        // issues are fixed — (1) mixed C/C++ static binaries SIGSEGV at
        // runtime (e2e 36_llvm_toolchain: answer.c + std::cout main.cpp,
        // exit 139; root cause not yet isolated), (2) the std-module
        // staging/fingerprint boundary (see canonical_compile_flags).
        // TODO(macos-floor-11): the official LLVM archives are built for
        // macOS 14; supporting 11-13 needs a custom libc++ build shipped
        // via xlings-res (data-only change — swap the archive source).
        // Both tracked in xlings
        // .agents/docs/2026-06-05-macos-min-version-support.md §5.
        if (f.staticStdlib && !macosDeploymentTarget.empty()
            && !llvmRootForStdlib.empty()) {
            auto libDir     = llvmRootForStdlib / "lib";
            auto libcxxA    = libDir / "libc++.a";
            auto libcxxAbiA = libDir / "libc++abi.a";
            if (std::filesystem::exists(libcxxA)
                && std::filesystem::exists(libcxxAbiA)) {
                // Link the archives BY PATH. (-Wl,-hidden-l looked like
                // the canonical choice, but lld resolves it like a plain
                // -l and picks the sibling dylib in the same directory —
                // the binary then carries @rpath/libc++.1.dylib with no
                // rpath and dies at load. Observed on macos CI; path
                // form verified end-to-end incl. macos-14.)
                f.ldStdlibDefault = " -nostdlib++ " + escape_path(libcxxA)
                                  + " " + escape_path(libcxxAbiA);
            }
        }
        std::string version_min;
        if (!macosDeploymentTarget.empty()) {
            version_min = " -mmacosx-version-min=" + macosDeploymentTarget;
        }
        f.ld = std::format("{}{}{} -fuse-ld=lld{}{}{}", full_static, static_stdlib,
                           b_flag, version_min, user_ldflags, link_extra);
    } else {
        f.ld = std::format("{}{}{}{}{}{}{}{}", full_static, static_stdlib, link_toolchain_flags, b_flag,
                           runtime_dirs, payload_ld, user_ldflags, link_extra);
    }

    return f;
}

}  // namespace mcpp::build
