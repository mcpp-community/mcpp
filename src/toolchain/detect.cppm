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
//
// `cross` retargets the driver (clang only): the resolved toolchain then
// reports the REQUESTED triple rather than `-dumpmachine`, takes its C
// library from the cross sysroot, and every probe that would have answered
// for the host is skipped rather than answered wrongly. Building the
// CrossTarget is the caller's job — this module must not learn platforms.
std::expected<Toolchain, DetectError>
detect(const std::filesystem::path& explicit_compiler = {},
       const std::optional<CrossTarget>& cross = std::nullopt);

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
detect(const std::filesystem::path& explicit_compiler,
       const std::optional<CrossTarget>& cross) {
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

    // ── Driver retargeting ────────────────────────────────────────────────
    // Applied here, between classification and enrichment: everything below
    // this point reads tc.targetTriple / tc.crossTarget, so the override has
    // to land before them, and the compiler-identity probes above are the
    // driver's own business and unaffected by the target.
    if (cross) {
        if (tc.compiler != CompilerId::Clang) {
            return std::unexpected(DetectError{std::format(
                "target '{}' needs a clang toolchain: only clang can be "
                "retargeted with --target, and this driver is {} ({}).\n"
                "       Remove the [target.{}] toolchain override to take the "
                "target's LLVM convention pin, or point it at an LLVM one:\n"
                "\n"
                "         [target.{}]\n"
                "         toolchain = \"llvm@<version>\"",
                cross->triple, tc.compiler_name(), tc.binaryPath.string(),
                cross->triple, cross->triple)});
        }
        // `-dumpmachine` said "x86_64-unknown-linux-gnu"; keep it as
        // provenance (it participates in the BMI fingerprint through
        // driverIdent, which is right — the same target built by a different
        // host driver is a different BMI) but let the REQUEST be the truth.
        tc.driverIdent += "\nretargeted-from: " + tc.targetTriple;
        tc.targetTriple = cross->triple;
        tc.crossTarget  = *cross;
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

    if (cross) {
        // The target's C library, stated rather than probed. `probe_sysroot`
        // would ask the driver, and `probe_payload_paths` would find the
        // HOST's glibc xpkg sitting next to the compiler — either one silently
        // aims a cross build at the build machine's libc, which is the exact
        // failure mode CLibMode::PayloadFirst exists to produce on purpose for
        // host builds. payloadPaths stays empty: linkmodel treats a cross
        // target as Sysroot mode unconditionally, and leaving a host payload
        // reachable would only give a future edit something wrong to find.
        tc.sysroot = cross->sysroot;
        return tc;
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
