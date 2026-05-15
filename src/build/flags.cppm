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
import mcpp.toolchain.detect;
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

    // Sysroot
    std::string sysroot_flag;
    if (!plan.toolchain.sysroot.empty()) {
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
    std::string module_flag = isClang ? "" : " -fmodules";
    std::string std_module_flag;
    if (isClang && !plan.stdBmiPath.empty()) {
        std_module_flag = " -fmodule-file=std=" + escape_path(staged_std_bmi_path(plan));
    }
    auto traits = mcpp::toolchain::bmi_traits(plan.toolchain);
    std::string prebuilt_module_flag;
    if (traits.needsPrebuiltModulePath) {
        prebuilt_module_flag = std::format(" -fprebuilt-module-path={}", traits.bmiDir);
    }
    f.cxx = std::format("-std=c++23{}{}{}{}{}{}{}{}{}", module_flag, std_module_flag,
                        prebuilt_module_flag,
                        opt_flag, pic_flag, sysroot_flag, b_flag, include_flags, user_cxxflags);
    f.cc = std::format("-std={}{}{}{}{}{}{}", c_std, opt_flag, pic_flag, sysroot_flag, b_flag,
                       include_flags, user_cflags);

    // Link flags
    f.staticStdlib = plan.manifest.buildConfig.staticStdlib;
    f.linkage = plan.manifest.buildConfig.linkage;
    std::string full_static = (f.linkage == "static") ? " -static" : "";
    std::string static_stdlib = (f.staticStdlib && !isClang) ? " -static-libstdc++" : "";
    std::string runtime_dirs;
    for (auto& dir : plan.toolchain.linkRuntimeDirs) {
        runtime_dirs += " -L" + escape_path(dir);
        runtime_dirs += " -Wl,-rpath," + escape_path(dir);
    }
    f.ld = std::format("{}{}{}{}{}", full_static, static_stdlib, sysroot_flag, b_flag,
                       runtime_dirs);

    return f;
}

}  // namespace mcpp::build
