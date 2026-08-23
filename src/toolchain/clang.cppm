// mcpp.toolchain.clang - Clang/libc++ compiler behavior.

export module mcpp.toolchain.clang;

import std;
import mcpp.toolchain.model;
import mcpp.toolchain.msvc;
import mcpp.toolchain.probe;
import mcpp.platform.xlings;
import mcpp.platform;

export namespace mcpp::toolchain::clang {

bool matches_version_output(std::string_view firstLineLower,
                            std::string_view fullOutputLower);

std::optional<std::filesystem::path> find_libcxx_std_module_source(
    const std::filesystem::path& cxx_binary,
    const std::string& envPrefix);

void enrich_toolchain(Toolchain& tc, const std::string& envPrefix);

std::filesystem::path std_bmi_path(const std::filesystem::path& cacheDir);
std::filesystem::path staged_std_bmi_path(const std::filesystem::path& outputDir);

std::vector<std::string> std_module_build_commands(const Toolchain& tc,
                                                   const std::filesystem::path& cacheDir,
                                                   const std::filesystem::path& bmiPath,
                                                   std::string_view sysrootFlag,
                                                   std::string_view cppStandardFlag);

std::optional<std::filesystem::path> find_libcxx_std_compat_source(
    const std::filesystem::path& cxx_binary,
    const std::string& envPrefix);

std::filesystem::path std_compat_bmi_path(const std::filesystem::path& cacheDir);
std::filesystem::path staged_std_compat_bmi_path(const std::filesystem::path& outputDir);

std::vector<std::string> std_compat_build_commands(const Toolchain& tc,
                                                    const std::filesystem::path& cacheDir,
                                                    const std::filesystem::path& bmiPath,
                                                    const std::filesystem::path& stdBmiPath,
                                                    std::string_view sysrootFlag,
                                                    std::string_view cppStandardFlag);


// Locate clang-scan-deps in the same bin/ directory as clang++.
std::optional<std::filesystem::path> find_scan_deps(const Toolchain& tc);

} // namespace mcpp::toolchain::clang

namespace mcpp::toolchain::clang {

namespace {

std::optional<std::string>
json_string_value_after(std::string_view body, std::size_t start, std::string_view key) {
    auto keyToken = std::string{"\""} + std::string(key) + "\"";
    auto keyPos = body.find(keyToken, start);
    if (keyPos == std::string_view::npos) return std::nullopt;

    auto colon = body.find(':', keyPos + keyToken.size());
    if (colon == std::string_view::npos) return std::nullopt;

    auto quote = body.find('"', colon + 1);
    if (quote == std::string_view::npos) return std::nullopt;

    std::string out;
    for (std::size_t i = quote + 1; i < body.size(); ++i) {
        char c = body[i];
        if (c == '"') return out;
        if (c == '\\' && i + 1 < body.size()) {
            out.push_back(body[++i]);
        } else {
            out.push_back(c);
        }
    }
    return std::nullopt;
}

} // namespace

bool matches_version_output(std::string_view firstLineLower,
                            std::string_view fullOutputLower) {
    return firstLineLower.find("clang version") != std::string::npos
        || firstLineLower.find("apple clang version") != std::string::npos
        || fullOutputLower.find("clang") != std::string::npos;
}

std::optional<std::filesystem::path> find_libcxx_std_module_source(
    const std::filesystem::path& cxx_binary,
    const std::string& envPrefix)
{
    auto manifest_r = mcpp::toolchain::run_capture(std::format(
        "{}{} -print-library-module-manifest-path {}",
        envPrefix,
        mcpp::xlings::shq(cxx_binary.string()),
        mcpp::platform::null_redirect));
    if (manifest_r) {
        auto manifestPath = std::filesystem::path(
            mcpp::toolchain::trim_line(*manifest_r));
        if (!manifestPath.empty() && std::filesystem::exists(manifestPath)) {
            std::ifstream is(manifestPath);
            std::stringstream ss;
            ss << is.rdbuf();
            auto body = ss.str();

            std::size_t cursor = 0;
            while (true) {
                auto logical = body.find("\"logical-name\"", cursor);
                if (logical == std::string::npos) break;
                auto name = json_string_value_after(body, logical, "logical-name");
                if (name && *name == "std") {
                    auto src = json_string_value_after(body, logical, "source-path");
                    if (src) {
                        std::filesystem::path p = *src;
                        if (p.is_relative())
                            p = manifestPath.parent_path() / p;
                        std::error_code ec;
                        auto canon = std::filesystem::weakly_canonical(p, ec);
                        if (!ec) p = canon;
                        if (std::filesystem::exists(p)) return p;
                    }
                }
                cursor = logical + 1;
            }
        }
    }

    auto root = cxx_binary.parent_path().parent_path();
    auto fallback = root / "share" / "libc++" / "v1" / "std.cppm";
    if (std::filesystem::exists(fallback)) return fallback;
    return std::nullopt;
}

void enrich_toolchain(Toolchain& tc, const std::string& envPrefix) {
    // Clang targeting MSVC uses MSVC STL, not libc++.
    bool msvTarget = is_msvc_target(tc);
    tc.stdlibId      = msvTarget ? "msvc-stl" : "libc++";
    tc.stdlibVersion = tc.version.empty() ? "unknown" : tc.version;
    tc.linkRuntimeDirs = mcpp::toolchain::discover_link_runtime_dirs(
        tc.binaryPath, tc.targetTriple);

    if (auto p = find_libcxx_std_module_source(tc.binaryPath, envPrefix)) {
        tc.stdModuleSource = *p;
        tc.hasImportStd    = true;
        // libc++ documents the std module for C++20 and later, and its
        // std.cppm carries no __cplusplus guard. Verified on clang 22.1.8 +
        // libc++ (std and std.compat) at -std=c++20 (design §2.2).
        tc.importStdMinLevel = 20;
    }

#if defined(_WIN32)
    // Fallback: if libc++ std.cppm not found, look for MSVC STL's std.ixx.
    // Uses msvc.cppm which searches via vswhere, env vars, and known paths.
    if (!tc.hasImportStd && msvTarget) {
        if (auto p = mcpp::toolchain::msvc::find_std_module_source()) {
            tc.stdModuleSource = *p;
            tc.hasImportStd    = true;
            // This is MSVC STL's std.ixx — the STL's own C++20 policy applies,
            // not libc++'s. tc.version is clang's here, so it cannot answer the
            // cl-banner question; stay strict (the STL is the binding side).
            tc.importStdMinLevel = 23;
        }
    }
#endif

    if (tc.hasImportStd) {
        if (auto p = find_libcxx_std_compat_source(tc.binaryPath, envPrefix)) {
            tc.stdCompatSource = *p;
        }
    }
}

std::filesystem::path std_bmi_path(const std::filesystem::path& cacheDir) {
    return cacheDir / "pcm.cache" / "std.pcm";
}

std::filesystem::path staged_std_bmi_path(const std::filesystem::path& outputDir) {
    return outputDir / "pcm.cache" / "std.pcm";
}

std::vector<std::string> std_module_build_commands(const Toolchain& tc,
                                                   const std::filesystem::path& cacheDir,
                                                   const std::filesystem::path& bmiPath,
                                                   std::string_view sysrootFlag,
                                                   std::string_view cppStandardFlag) {
    auto relBmi = std::filesystem::relative(bmiPath, cacheDir).string();
    // ⚠️ A PACKAGE-PROVIDED std MODULE REPLACES THE TOOLCHAIN'S SYSROOT FLAGS
    // RATHER THAN BEING APPENDED TO THEM.
    //
    // Those flags describe the standard library the COMPILER ships and the C
    // library the HOST has, and they lead with `-isystem' — so appending to them
    // puts the host's headers ahead of the package's, and no later flag can
    // undo it. Measured: the module then compiles the host C library's
    // <wchar.h> and stops on names that library expects the host compiler to
    // have supplied.
    //
    // The triple has to be restated for the same reason: it was in the flags
    // being replaced, and without it the module is built for whatever machine
    // is doing the building.
    // The replacement is complete: whoever set stdModuleFlags stated the target
    // as well, because the triple was in the string being replaced and a module
    // built without it is built for whatever machine is doing the building.
    if (!tc.stdModuleFlags.empty()) sysrootFlag = {};
    const std::string& extraFlags = tc.stdModuleFlags;
    // ⚠️ The codegen step compiles a BMI, which already carries what the
    // headers contributed; only the machine has to be restated. See
    // Toolchain::stdModuleTargetFlags.
    const std::string& codegenFlags = tc.stdModuleTargetFlags;
#if defined(_WIN32)
    // Windows: use absolute paths, raw binary path as first token
    // (cmd.exe strips leading quotes), shq for args with spaces.
    // -x c++-module is needed for MSVC STL's .ixx files (Clang doesn't
    // recognize the .ixx extension as a module source by default).
    auto absBmi = (cacheDir / relBmi).string();
    auto ext = tc.stdModuleSource.extension().string();
    // MSVC STL's std.ixx needs -x c++-module (Clang doesn't recognize .ixx)
    // and generates harmless warnings about #include in module purview and
    // the reserved 'std' module name — suppress both.
    std::string ixxFlags = (ext == ".ixx")
        ? " -x c++-module -Wno-include-angled-in-module-purview"
        : "";
    // ⚠️ AND THE RESERVED-NAME WARNING UNCONDITIONALLY, WHICH IS WHAT THE OTHER
    // BRANCH DOES.
    //
    // `export module std;` is a reserved identifier and every standard library
    // that ships one triggers the warning; the non-Windows command has carried
    // the suppression since it was written. This branch tied it to `.ixx`,
    // which was correct while the only `std` module a Windows host ever saw was
    // the MSVC STL's — and stopped being correct when a package could supply
    // its own. Measured 2026-08-23, a Windows host building the openkal
    // runtime's `llvm-generated/std.cppm`:
    //
    //     std.cppm:167:15: warning: 'std' is a reserved name for a module
    //       [-Wreserved-module-identifier]
    //
    // A warning that is correct, unavoidable, and printed on every build is
    // noise of the kind that hides the next one.
    ixxFlags += " -Wno-reserved-module-identifier";
    // ⚠️ `extraFlags` IS ON BOTH COMMANDS HERE, AND IT WAS ON NEITHER.
    //
    // This branch was written when a Windows host built for itself against the
    // MSVC STL, and `stdModuleFlags` did not exist — so the omission was not
    // visible: there was nothing to omit. It became a defect when a package
    // could supply its own `std` module, because that string is where the
    // package's own headers, `-nostdinc` and the target triple live.
    //
    // ⚠️ Measured 2026-08-23, a Windows host cross-building for
    // `x86_64-linux-gnu` over openkal — the command it produced carried FIVE
    // tokens:
    //
    //     clang++.exe -std=c++23 --precompile "…/std.cppm" -o "…/std.pcm"
    //     …/llvm-generated/std.cppm:16:10: fatal error: '__config' file not found
    //
    // The same build from a Linux host had `--target=`, `--no-default-config`,
    // `-nostdinc`, `-nostdinc++` and eight `-I`s. The error names a header, and
    // the cause is a branch keyed on which machine is doing the building.
    return {
        std::format(
            "{} {}{}{}{} "
            "--precompile {} -o {}",
            tc.binaryPath.string(),
            cppStandardFlag,
            ixxFlags,
            sysrootFlag,
            extraFlags,
            mcpp::xlings::shq(tc.stdModuleSource.string()),
            mcpp::xlings::shq(absBmi)),
        std::format(
            "{} {}{}{} "
            "{} -c -o {}",
            tc.binaryPath.string(),
            cppStandardFlag,
            sysrootFlag,
            codegenFlags,
            mcpp::xlings::shq(absBmi),
            mcpp::xlings::shq((cacheDir / "std.o").string()))
    };
#else
    return {
        std::format(
            "cd {} && {}{} {} -Wno-reserved-module-identifier{}{} "
            "--precompile {} -o {} 2>&1",
            mcpp::xlings::shq(cacheDir.string()),
            mcpp::toolchain::compiler_env_prefix(tc),
            mcpp::xlings::shq(tc.binaryPath.string()),
            cppStandardFlag,
            sysrootFlag,
            extraFlags,
            mcpp::xlings::shq(tc.stdModuleSource.string()),
            mcpp::xlings::shq(relBmi)),
        std::format(
            "cd {} && {}{} {} -Wno-reserved-module-identifier{}{} "
            "{} -c -o std.o 2>&1",
            mcpp::xlings::shq(cacheDir.string()),
            mcpp::toolchain::compiler_env_prefix(tc),
            mcpp::xlings::shq(tc.binaryPath.string()),
            cppStandardFlag,
            sysrootFlag,
            codegenFlags,
            mcpp::xlings::shq(relBmi))
    };
#endif
}

std::optional<std::filesystem::path> find_scan_deps(const Toolchain& tc) {
    auto p = tc.binaryPath.parent_path() /
        (std::string("clang-scan-deps") + std::string(mcpp::platform::exe_suffix));
    if (std::filesystem::exists(p)) return p;
    return std::nullopt;
}

std::optional<std::filesystem::path> find_libcxx_std_compat_source(
    const std::filesystem::path& cxx_binary,
    const std::string& envPrefix)
{
    // Same search strategy as find_libcxx_std_module_source but for std.compat
    auto root = cxx_binary.parent_path().parent_path();
    auto p = root / "share" / "libc++" / "v1" / "std.compat.cppm";
    if (std::filesystem::exists(p)) return p;
    return std::nullopt;
}

std::filesystem::path std_compat_bmi_path(const std::filesystem::path& cacheDir) {
    return cacheDir / "pcm.cache" / "std.compat.pcm";
}

std::filesystem::path staged_std_compat_bmi_path(const std::filesystem::path& outputDir) {
    return outputDir / "pcm.cache" / "std.compat.pcm";
}

std::vector<std::string> std_compat_build_commands(const Toolchain& tc,
                                                    const std::filesystem::path& cacheDir,
                                                    const std::filesystem::path& bmiPath,
                                                    const std::filesystem::path& stdBmiPath,
                                                    std::string_view sysrootFlag,
                                                    std::string_view cppStandardFlag)
{
    auto relBmi = std::filesystem::relative(bmiPath, cacheDir).string();
    auto relStdBmi = std::filesystem::relative(stdBmiPath, cacheDir).string();
    // ⚠️ THE SAME REPLACEMENT THE `std` BUILDER MAKES, FOR THE SAME REASON.
    //
    // `std.compat` is a second module over the SAME library, and it therefore
    // needs the same headers, the same target and the same configuration. This
    // used to take `sysrootFlag` unconditionally while its sibling above
    // replaced it — so a package-provided pair had one module built against its
    // own libc++ and the other against the toolchain's.
    //
    // ⚠️ It does not fail where the two are chosen. Measured on a macOS cross:
    //
    //   error: std module precompile failed (rc=1):
    //     …/openkal-llvm-runtime/llvm-generated/std.compat.cppm:16
    //     …/xim-x-llvm/22.1.8/include/c++/v1/__config:13
    //         fatal error: '__config_site' file not found
    //
    // The SOURCE named is the package's; the header it opened is the
    // toolchain's, whose per-installation configuration was never generated for
    // this target. Reading that message, the mixture is invisible.
    if (!tc.stdModuleFlags.empty()) sysrootFlag = {};
    const std::string& extraFlags = tc.stdModuleFlags;
    // Same split as the `std` builder above: the second command compiles a BMI
    // and needs the machine restated, not the include paths.
    const std::string& codegenFlags = tc.stdModuleTargetFlags;
    // std.compat depends on std, so we need -fmodule-file=std=<std.pcm>
    // Note: the path after = must NOT be shell-quoted separately; the
    // entire -fmodule-file flag is a single token to the compiler.
    //
    // ⚠️⚠️ ABSOLUTE PATHS AND NO `cd`, AND ONE FORM RATHER THAN TWO.
    //
    // This used to be `cd <cacheDir> && … pcm.cache/std.pcm …`. `cd X && …`
    // DOES NOT CHANGE THE DRIVE in cmd.exe — the build cache lives under the
    // user's profile and a checkout lives wherever the runner put it, so on CI
    // those are `C:` and `D:`. The `cd` succeeds, the drive stays where it was,
    // and every relative path resolves against the wrong root. Measured
    // 2026-08-23, a Windows host cross-building for `x86_64-linux-gnu`:
    //
    //     std.compat.cppm:84:8: fatal error: module file 'pcm.cache\std.pcm'
    //       not found: module file not found
    //
    // — and `std.pcm` had been built successfully one command earlier.
    //
    // ⚠️ The obvious repair was a `#if defined(_WIN32)` branch, which is what
    // the `std` builder above has. It was written and then withdrawn: a branch
    // that only compiles on one platform is a branch this machine cannot check,
    // and every defect this session found in the host dimension had exactly
    // that shape — code shaped by which machine was doing the building. Naming
    // absolute paths is correct everywhere, so there is one form.
    auto absBmi    = (cacheDir / relBmi).string();
    auto absStdBmi = (cacheDir / relStdBmi).string();
    auto absObj    = (cacheDir / "std.compat.o").string();
    return {
        std::format("{}{} {} -Wno-reserved-module-identifier{}{} "
                    "-fmodule-file=std={} "
                    "--precompile {} -o {} 2>&1",
                    mcpp::toolchain::compiler_env_prefix(tc),
                    mcpp::xlings::shq(tc.binaryPath.string()),
                    cppStandardFlag,
                    sysrootFlag,
                    extraFlags,
                    absStdBmi,
                    mcpp::xlings::shq(tc.stdCompatSource.string()),
                    mcpp::xlings::shq(absBmi)),
        std::format("{}{} {} -Wno-reserved-module-identifier{}{} "
                    "-fmodule-file=std={} "
                    "{} -c -o {} 2>&1",
                    mcpp::toolchain::compiler_env_prefix(tc),
                    mcpp::xlings::shq(tc.binaryPath.string()),
                    cppStandardFlag,
                    sysrootFlag,
                    codegenFlags,
                    absStdBmi,
                    mcpp::xlings::shq(absBmi),
                    mcpp::xlings::shq(absObj))
    };
}

} // namespace mcpp::toolchain::clang
