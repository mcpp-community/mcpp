// mcpp.toolchain.provider — provider capabilities dispatch.
//
// Documents the "provider concept": each toolchain variant (GCC/libstdc++,
// Clang/libc++, Clang/MSVC-STL) has a distinct set of capabilities.
// Previously these decisions were scattered as ad-hoc is_clang(tc) /
// is_gcc(tc) / targetTriple.find("msvc") checks. This module centralises
// them into a single query point.
//
// Usage:
//   auto caps = mcpp::toolchain::capabilities_for(tc);
//   if (caps.has_import_std) { ... }

export module mcpp.toolchain.provider;

import std;
import mcpp.toolchain.model;

export namespace mcpp::toolchain {

// ─── ProviderCapabilities ────────────────────────────────────────────────────
//
// Describes what a particular toolchain instance can do.  All fields have
// safe defaults (false / empty) so callers that only care about one flag
// do not need to guard the rest.

struct ProviderCapabilities {
    // True when the toolchain ships a prebuilt `std` module source
    // (bits/std.cc for GCC, std.cppm / std.ixx for Clang variants) and
    // Toolchain::stdModuleSource has been populated by enrich_toolchain().
    bool has_import_std = false;

    // True when clang-scan-deps (or an equivalent dep-scanner) is available
    // alongside the compiler binary.  Currently only Clang provides this.
    bool has_scan_deps = false;

    // True when the compiler itself emits P1689 during preprocessing
    // (GCC -fdeps-format=p1689r5). Clang needs the external clang-scan-deps;
    // MSVC's /scanDependencies is compiler-integrated but not wired yet.
    bool has_builtin_p1689_scan = false;

    // True when the compiler supports C++ named modules at all.
    // All three supported compilers do; kept for future use when we add
    // compilers that don't (e.g. old MSVC versions, ICC).
    bool has_modules = true;

    // Canonical stdlib identifier:
    //   "libstdc++"  — GCC, or Clang targeting a non-MSVC triple on Linux/macOS
    //   "libc++"     — Clang with libc++ (xim:llvm toolchain, or Apple Clang)
    //   "msvc-stl"   — Clang targeting x86_64-pc-windows-msvc
    //   ""           — Unknown / not yet detected
    std::string stdlib_id;

    // Archive tool name used for static libraries:
    //   "ar"       — GCC / system binutils
    //   "llvm-ar"  — Clang (llvm-ar is preferred; falls back to system ar)
    //   "lib.exe"  — MSVC (future)
    //   ""         — Unknown
    std::string archive_format;
};

// Determine provider capabilities from an already-detected toolchain.
// All fields are derived from tc.compiler + tc.targetTriple + tc.hasImportStd
// so the result is deterministic and has no side-effects.
ProviderCapabilities capabilities_for(const Toolchain& tc);

} // namespace mcpp::toolchain

// ─── Implementation ──────────────────────────────────────────────────────────

namespace mcpp::toolchain {

ProviderCapabilities capabilities_for(const Toolchain& tc) {
    ProviderCapabilities caps;

    caps.has_import_std = tc.hasImportStd;
    caps.has_modules    = true;   // all supported compilers handle modules

    switch (tc.compiler) {
        case CompilerId::GCC: {
            caps.has_scan_deps   = false;   // GCC has no clang-scan-deps equivalent
            caps.has_builtin_p1689_scan = true;
            caps.stdlib_id       = "libstdc++";
            caps.archive_format  = "ar";
            break;
        }

        case CompilerId::Clang: {
            // Clang targeting MSVC uses MSVC STL, not libc++.
            bool msvc_target = is_msvc_target(tc);

            caps.has_scan_deps  = true;     // clang-scan-deps lives beside clang++
            caps.stdlib_id      = msvc_target ? "msvc-stl" : "libc++";
            caps.archive_format = "llvm-ar";
            break;
        }

        case CompilerId::MSVC: {
            caps.has_scan_deps  = false;
            // cl.exe emits P1689 itself via /scanDependencies.
            caps.has_builtin_p1689_scan = true;
            caps.stdlib_id      = "msvc-stl";
            caps.archive_format = "lib.exe";
            break;
        }

        case CompilerId::Unknown:
        default:
            // Leave all caps at their safe defaults (false / "").
            break;
    }

    return caps;
}

} // namespace mcpp::toolchain
