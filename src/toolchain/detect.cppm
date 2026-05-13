// mcpp.toolchain.detect - compiler discovery facade.

export module mcpp.toolchain.detect;

export import mcpp.toolchain.model;
export import mcpp.toolchain.probe;

import std;
import mcpp.toolchain.clang;
import mcpp.toolchain.gcc;
import mcpp.xlings;

export namespace mcpp::toolchain {

// Detect toolchain. If explicit_compiler is given, use that binary path
// directly. Otherwise fall back to $CXX, then PATH g++.
std::expected<Toolchain, DetectError>
detect(const std::filesystem::path& explicit_compiler = {});

// Compatibility helper for older call sites/tests: GCC std module lookup now
// lives in the GCC provider.
std::optional<std::filesystem::path> find_std_module_source(
    const std::filesystem::path& cxx_binary, std::string_view version);

} // namespace mcpp::toolchain

namespace mcpp::toolchain {

std::optional<std::filesystem::path> find_std_module_source(
    const std::filesystem::path& cxx_binary, std::string_view version) {
    return mcpp::toolchain::gcc::find_std_module_source(cxx_binary, version);
}

std::expected<Toolchain, DetectError>
detect(const std::filesystem::path& explicit_compiler) {
    auto bin_r = probe_compiler_binary(explicit_compiler);
    if (!bin_r) return std::unexpected(bin_r.error());

    Toolchain tc;
    tc.binaryPath = *bin_r;
    tc.compilerRuntimeDirs = discover_compiler_runtime_dirs(tc.binaryPath);
    auto envPrefix = compiler_env_prefix(tc);

    auto ver_r = run_capture(std::format("{}{} --version 2>&1",
                                         envPrefix,
                                         mcpp::xlings::shq(tc.binaryPath.string())));
    if (!ver_r) return std::unexpected(ver_r.error());

    const auto& vstr = *ver_r;
    auto head = first_line_of(vstr);
    auto headLower = lower_copy(head);
    auto fullLower = lower_copy(vstr);

    if (mcpp::toolchain::clang::matches_version_output(headLower, fullLower)) {
        tc.compiler = CompilerId::Clang;
        tc.version  = extract_version(head.empty()
            ? std::string_view(vstr)
            : std::string_view(head));
    } else if (mcpp::toolchain::gcc::matches_version_output(headLower)) {
        tc.compiler = CompilerId::GCC;
        tc.version  = mcpp::toolchain::gcc::parse_version(head);
    } else {
        return std::unexpected(DetectError{
            std::format("unrecognized compiler output:\n{}", vstr)});
    }

    if (auto triple = probe_target_triple(tc.binaryPath, envPrefix)) {
        tc.targetTriple = *triple;
    }

    if (tc.compiler == CompilerId::GCC) {
        mcpp::toolchain::gcc::enrich_toolchain(tc);
    } else if (tc.compiler == CompilerId::Clang) {
        mcpp::toolchain::clang::enrich_toolchain(tc, envPrefix);
    }

    tc.sysroot = probe_sysroot(tc.binaryPath, envPrefix);

    return tc;
}

} // namespace mcpp::toolchain
