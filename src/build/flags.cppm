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
    std::string toolEnv;              // env prefix for private toolchain executables
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
    f.toolEnv = mcpp::toolchain::compiler_env_prefix(plan.toolchain);

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

    // Sysroot + config override for macOS.
    // On macOS, xlings LLVM's clang++.cfg contains hardcoded --sysroot and
    // -isystem paths from the original install location. When the package is
    // copied to mcpp's sandbox, these paths become stale. We pass
    // --no-default-config to ignore the cfg and provide correct paths.
    std::string sysroot_flag;
    bool is_macos_clang = mcpp::toolchain::is_clang(plan.toolchain)
        && (plan.toolchain.targetTriple.find("apple") != std::string::npos
         || plan.toolchain.targetTriple.find("darwin") != std::string::npos);
    if (is_macos_clang) {
        auto llvmRoot = plan.toolchain.binaryPath.parent_path().parent_path();
        auto libcxxInclude = llvmRoot / "include" / "c++" / "v1";
        sysroot_flag = " --no-default-config";
        sysroot_flag += " -isystem" + escape_path(libcxxInclude);
        if (auto sdk = mcpp::platform::macos::sdk_path())
            sysroot_flag += " --sysroot=" + escape_path(*sdk);
        f.sysroot = sysroot_flag;
    } else if (!plan.toolchain.sysroot.empty()) {
        sysroot_flag = " --sysroot=" + escape_path(plan.toolchain.sysroot);
        f.sysroot = sysroot_flag;
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
        prebuilt_module_flag = std::format(" -fprebuilt-module-path={}", traits.bmiDir);
    }
    f.cxx = std::format("-std=c++23{}{}{}{}{}{}{}{}{}{}", module_flag, std_module_flag,
                        std_compat_module_flag, prebuilt_module_flag,
                        opt_flag, pic_flag, sysroot_flag, b_flag, include_flags, user_cxxflags);
    f.cc = std::format("-std={}{}{}{}{}{}{}", c_std, opt_flag, pic_flag, sysroot_flag, b_flag,
                       include_flags, user_cflags);

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

    if constexpr (mcpp::platform::is_windows) {
        f.ld = "";
    } else if constexpr (mcpp::platform::needs_explicit_libcxx) {
        f.ld = std::format("{}{}{} -lc++", full_static, static_stdlib, b_flag);
    } else {
        f.ld = std::format("{}{}{}{}{}", full_static, static_stdlib, sysroot_flag, b_flag, runtime_dirs);
    }

    return f;
}

}  // namespace mcpp::build
