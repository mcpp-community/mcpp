// mcpp.build.flags — shared compile/link flag computation.
//
// Extracts all flag logic from ninja_backend.cppm into a single point
// of truth so both the ninja backend and compile_commands.json emitter
// (and future backends) share identical flag sets.
//
// See .agents/docs/2026-05-12-compile-commands-design.md.

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

}  // namespace

CompileFlags compute_flags(const BuildPlan& plan) {
    CompileFlags f;

    // ProviderCapabilities: centralised query point for per-toolchain decisions.
    // Prefer caps.* checks over ad-hoc is_clang()/is_musl_target() calls for
    // any new branching added to this function.
    auto caps = mcpp::toolchain::capabilities_for(plan.toolchain);

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
        // No sysroot but have payload paths: use -isystem.
        auto& pp = *plan.toolchain.payloadPaths;
        compile_toolchain_flags += " -isystem" + escape_path(pp.glibcInclude);
        if (!pp.linuxInclude.empty())
            compile_toolchain_flags += " -isystem" + escape_path(pp.linuxInclude);
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

    // Opt level (musl ICE workaround)
    std::string opt_flag = isMuslTc ? " -Og" : " -O2";

    // User flags
    auto join = [](const std::vector<std::string>& v) {
        std::string s;
        for (auto& f : v) {
            s += ' ';
            s += f;
        }
        return s;
    };
    std::string user_cxxflags = join(plan.manifest.buildConfig.cxxflags);
    std::string user_cflags = join(plan.manifest.buildConfig.cflags);

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
    f.cxx = std::format("-std=c++23{}{}{}{}{}{}{}{}{}{}", module_flag, std_module_flag,
                        std_compat_module_flag, prebuilt_module_flag,
                        opt_flag, pic_flag, compile_toolchain_flags, b_flag, include_flags, user_cxxflags);
    f.cc = std::format("-std={}{}{}{}{}{}{}", c_std, opt_flag, pic_flag, compile_toolchain_flags,
                       b_flag, include_flags, user_cflags);

    // Link flags
    f.staticStdlib = plan.manifest.buildConfig.staticStdlib;
    f.linkage = plan.manifest.buildConfig.linkage;
    std::string full_static = (mcpp::platform::supports_full_static && f.linkage == "static") ? " -static" : "";
    std::string static_stdlib = (f.staticStdlib && !isClang && !mcpp::platform::is_windows) ? " -static-libstdc++" : "";
    std::string runtime_dirs;
    if constexpr (mcpp::platform::supports_rpath) {
        for (auto& dir : plan.toolchain.linkRuntimeDirs) {
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

    if constexpr (mcpp::platform::is_windows) {
        f.ld = "";
    } else if constexpr (mcpp::platform::needs_explicit_libcxx) {
        f.ld = std::format("{}{}{} -lc++", full_static, static_stdlib, b_flag);
    } else {
        f.ld = std::format("{}{}{}{}{}{}", full_static, static_stdlib, link_toolchain_flags, b_flag,
                           runtime_dirs, payload_ld);
    }

    return f;
}

}  // namespace mcpp::build
