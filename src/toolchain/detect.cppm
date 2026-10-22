// mcpp.toolchain.detect - compiler discovery facade.

export module mcpp.toolchain.detect;

export import mcpp.toolchain.model;
export import mcpp.toolchain.probe;

import std;
import mcpp.toolchain.clang;
import mcpp.toolchain.gcc;
import mcpp.toolchain.msvc;
import mcpp.provider;
import mcpp.provider.select;
import mcpp.platform;
import mcpp.fallback.probe_sysroot;

export namespace mcpp::toolchain {

// Detect toolchain. If explicit_compiler is given, use that binary path
// directly. Otherwise fall back to $CXX, then PATH g++.
std::expected<Toolchain, DetectError>
detect(const std::filesystem::path& explicit_compiler = {},
       std::string_view runtimeBinding = {},
       std::string_view runtimeContractHash = {});

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
       std::string_view runtimeBinding,
       std::string_view runtimeContractHash) {
    auto bin_r = probe_compiler_binary(explicit_compiler);
    if (!bin_r) return std::unexpected(bin_r.error());

    Toolchain tc;
    tc.binaryPath = *bin_r;
    // Set before provider-specific early returns (notably native MSVC), so
    // every platform carries the same RuntimeBinding cache identity.
    tc.runtimeBinding = std::string(runtimeBinding);
    tc.runtimeContractHash = std::string(runtimeContractHash);

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
                                         mcpp::platform::shell::quote(tc.binaryPath.string())));
    if (!ver_r) return std::unexpected(ver_r.error());

    const auto& vstr = *ver_r;
    tc.driverIdent = normalize_driver_output(vstr);
    // The packaging revision of the payload this compiler is in (design
    // 2026-10-17 D8). A build keeps an installed revision while the provider
    // calls it usable, so one version can be present in two packagings -- two
    // homes, or one home before and after `mcpp toolchain install` -- whose
    // `--version` reads the same; their artifacts and caches must not mix.
    // Revision 0 adds nothing: a payload from before revisions keeps the
    // identity, and the caches, it had.
    if (const auto entry = mcpp::provider::layout().entry_of(tc.binaryPath); !entry.dir.empty())
        if (const int r = mcpp::provider::revision_of(entry.dir).value_or(0); r > 0)
            tc.driverIdent += std::format("\npayload-revision: {}", r);
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
            mcpp::platform::shell::quote(tc.binaryPath.string())));
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

    tc.base.root = probe_sysroot(tc.binaryPath, envPrefix);

    // Probe fine-grained payload paths from sibling xpkgs (glibc, linux-headers).
    // When available, flags are assembled from these paths instead of --sysroot.
    tc.base.payload = probe_payload_paths(tc.binaryPath, tc.runtimeBinding);

    // For GCC: ensure the probed sysroot has complete headers by symlinking
    // missing content (linux kernel headers, glibc) from payload xpkgs.
    // This makes mcpp self-sufficient — not dependent on xlings subos init.
    //
    // ONLY A VIEW THIS ECOSYSTEM OWNS. A compiler named by path reports the
    // sysroot it was built with, which is the vendor's tree, and mcpp does not
    // write into a tree it does not own (SPEC-006 §2.2.1; design 2026-10-23
    // D10): its headers are completed on the command line instead. The view
    // is owned when it lives in the same home as the payloads it is completed
    // from.
    if (tc.base.payload && !tc.base.root.empty()) {
        auto store = mcpp::provider::store_of(tc.base.payload->glibcInclude);
        if (store && mcpp::fallback::path_is_under(
                         tc.base.root, mcpp::provider::layout().home_of_store(*store)))
            ensure_sysroot_complete(tc.base.root, *tc.base.payload);
    }

    return tc;
}

} // namespace mcpp::toolchain
