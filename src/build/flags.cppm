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

std::filesystem::path derive_c_compiler(const std::filesystem::path& cxxPath) {
    auto stem = cxxPath.stem().string();
    auto parent = cxxPath.parent_path();
    auto ext = cxxPath.extension();

    // g++ → gcc, clang++ → clang, x86_64-linux-musl-g++ → x86_64-linux-musl-gcc
    std::string cc_stem;
    if (stem.ends_with("++")) {
        cc_stem = stem.substr(0, stem.size() - 2);
        // g++ → gcc; clang++ → clang
        if (cc_stem.ends_with("g"))
            cc_stem += "cc";  // g → gcc
        // else clang++ → clang (already correct after stripping ++)
    } else {
        cc_stem = stem;  // fallback: same as cxx
    }
    return parent / (cc_stem + ext.string());
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
    f.ccBinary = derive_c_compiler(plan.toolchain.binaryPath);

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
    bool isMuslTc = plan.toolchain.targetTriple.find("-musl") != std::string::npos;
    std::filesystem::path binutilsBin;
    if (!isMuslTc) {
        auto bp = plan.toolchain.binaryPath;
        std::filesystem::path xpkgsDir;
        for (auto p = bp.parent_path(); p.has_parent_path() && p != p.root_path();
             p = p.parent_path()) {
            if (p.filename() == "xpkgs") {
                xpkgsDir = p;
                break;
            }
        }
        if (!xpkgsDir.empty()) {
            auto root = xpkgsDir / "xim-x-binutils";
            std::error_code ec;
            if (std::filesystem::exists(root, ec)) {
                for (auto& v : std::filesystem::directory_iterator(root, ec)) {
                    auto candidate = v.path() / "bin";
                    if (std::filesystem::exists(candidate / "ar", ec)) {
                        binutilsBin = candidate;
                        break;
                    }
                }
            }
        }
    }
    std::string b_flag;
    if (!binutilsBin.empty()) {
        b_flag = " -B" + escape_path(binutilsBin);
        f.bFlag = b_flag;
    }

    // AR binary
    if (!binutilsBin.empty()) {
        f.arBinary = binutilsBin / "ar";
    } else if (isMuslTc) {
        auto muslAr = plan.toolchain.binaryPath.parent_path() / "x86_64-linux-musl-ar";
        if (std::filesystem::exists(muslAr))
            f.arBinary = muslAr;
    }

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
    f.cxx = std::format("-std=c++23 -fmodules{}{}{}{}{}{}", opt_flag, pic_flag, sysroot_flag,
                        b_flag, include_flags, user_cxxflags);
    f.cc = std::format("-std={}{}{}{}{}{}{}", c_std, opt_flag, pic_flag, sysroot_flag, b_flag,
                       include_flags, user_cflags);

    // Link flags
    f.staticStdlib = plan.manifest.buildConfig.staticStdlib;
    f.linkage = plan.manifest.buildConfig.linkage;
    std::string full_static = (f.linkage == "static") ? " -static" : "";
    std::string static_stdlib = f.staticStdlib ? " -static-libstdc++" : "";
    f.ld = std::format("{}{}{}{}", full_static, static_stdlib, sysroot_flag, b_flag);

    return f;
}

}  // namespace mcpp::build
