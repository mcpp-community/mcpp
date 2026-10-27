// mcpp.toolchain.clang - Clang/libc++ compiler behavior.

export module mcpp.toolchain.clang;

import std;
import mcpp.toolchain.model;
import mcpp.run;

export namespace mcpp::toolchain::clang {

bool matches_version_output(std::string_view firstLineLower,
                            std::string_view fullOutputLower);

std::optional<std::filesystem::path> find_libcxx_std_module_source(
    const std::filesystem::path& cxx_binary,
    const mcpp::run::Env& env);

void enrich_toolchain(Toolchain& tc, const mcpp::run::Env& env);

std::filesystem::path std_bmi_path(const std::filesystem::path& cacheDir);
std::filesystem::path staged_std_bmi_path(const std::filesystem::path& outputDir);

// `crtFlag` (#718): empty off the MSVC ABI; on it, `-fms-runtime-lib=static`
// or `=dll` (`mcpp.toolchain.dialect::msvc_abi_crt_word`), applied to BOTH
// commands below — the precompile step needs it as much as the codegen step
// does, since the CRT choice also selects which `<vcruntime.h>` declarations
// this compile sees.
std::vector<std::string> std_module_build_commands(const Toolchain& tc,
                                                   const std::filesystem::path& cacheDir,
                                                   const std::filesystem::path& bmiPath,
                                                   std::string_view sysrootFlag,
                                                   std::string_view cppStandardFlag,
                                                   std::string_view crtFlag = {});

std::optional<std::filesystem::path> find_libcxx_std_compat_source(
    const std::filesystem::path& cxx_binary,
    const mcpp::run::Env& env);

std::filesystem::path std_compat_bmi_path(const std::filesystem::path& cacheDir);
std::filesystem::path staged_std_compat_bmi_path(const std::filesystem::path& outputDir);

std::vector<std::string> std_compat_build_commands(const Toolchain& tc,
                                                    const std::filesystem::path& cacheDir,
                                                    const std::filesystem::path& bmiPath,
                                                    const std::filesystem::path& stdBmiPath,
                                                    std::string_view sysrootFlag,
                                                    std::string_view cppStandardFlag,
                                                    std::string_view crtFlag = {});


// Locate clang-scan-deps in the same bin/ directory as clang++.
std::optional<std::filesystem::path> find_scan_deps(const Toolchain& tc);

// The file the driver links for `archive` (`libc++.a`, `libc++abi.a`,
// `libunwind.a`) under `driverFlags`, which carry the target the link names.
// Empty when the driver cannot place it. Asked once per process for each
// (compiler, flags, archive) triple.
std::filesystem::path runtime_archive_path(const Toolchain& tc,
                                           const std::vector<std::string>& driverFlags,
                                           std::string_view archive);

} // namespace mcpp::toolchain::clang
