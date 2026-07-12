// mcpp.toolchain.detect - compiler discovery facade.

export module mcpp.toolchain.detect;

export import mcpp.toolchain.model;
export import mcpp.toolchain.probe;

import std;
import mcpp.toolchain.clang;
import mcpp.toolchain.gcc;
import mcpp.toolchain.msvc;
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

    // MSVC cl.exe has no --version / -dumpmachine / -print-sysroot; classify
    // it by filename and take a dedicated enrich path (banner → version,
    // arch → triple, std.ixx lookup). No runtime dirs / sysroot / payloads —
    // those concepts are GCC/Clang-shaped.
    if (auto stem = lower_copy(tc.binaryPath.stem().string()); stem == "cl") {
        if (auto r = mcpp::toolchain::msvc::enrich_toolchain_from_cl(tc); !r)
            return std::unexpected(r.error());
        return tc;
    }

    tc.compilerRuntimeDirs = discover_compiler_runtime_dirs(tc.binaryPath);
    auto envPrefix = compiler_env_prefix(tc);

    auto ver_r = run_capture(std::format("{}{} --version 2>&1",
                                         envPrefix,
                                         mcpp::xlings::shq(tc.binaryPath.string())));
    if (!ver_r) return std::unexpected(ver_r.error());

    const auto& vstr = *ver_r;
    tc.driverIdent = normalize_driver_output(vstr);
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

#if defined(_WIN32)
    // On Windows, Clang targeting MSVC auto-detects the MSVC version at
    // compile time and bakes it into the module AST. The -dumpmachine triple
    // doesn't include this version, so fingerprints don't change when MSVC
    // patches (e.g. 19.44.35226 → 35227), causing stale BMI cache hits.
    // Query the effective triple which includes the actual MSVC version.
    if (tc.compiler == CompilerId::Clang
        && is_msvc_target(tc)) {
        auto vr = run_capture(std::format(
            "{}{} -print-effective-triple 2>NUL",
            envPrefix,
            mcpp::xlings::shq(tc.binaryPath.string())));
        if (vr) {
            auto effective = trim_line(*vr);
            if (!effective.empty() && effective != tc.targetTriple)
                tc.driverIdent += "\neffective-triple: " + effective;
        }
    }
#endif

    if (tc.compiler == CompilerId::GCC) {
        mcpp::toolchain::gcc::enrich_toolchain(tc);
    } else if (tc.compiler == CompilerId::Clang) {
        mcpp::toolchain::clang::enrich_toolchain(tc, envPrefix);
    }

    tc.sysroot = probe_sysroot(tc.binaryPath, envPrefix);

    // Probe fine-grained payload paths from sibling xpkgs (glibc, linux-headers).
    // When available, flags are assembled from these paths instead of --sysroot.
    tc.payloadPaths = probe_payload_paths(tc.binaryPath);

    // For GCC: ensure the probed sysroot has complete headers by symlinking
    // missing content (linux kernel headers, glibc) from payload xpkgs.
    // This makes mcpp self-sufficient — not dependent on xlings subos init.
    if (tc.payloadPaths && !tc.sysroot.empty())
        ensure_sysroot_complete(tc.sysroot, *tc.payloadPaths);

    return tc;
}

} // namespace mcpp::toolchain
