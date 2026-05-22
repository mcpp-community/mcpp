// mcpp.fallback.registry — compile-time fallback metadata registry.
//
// Every fallback/workaround in mcpp is registered here with its lifecycle,
// domain, and removal plan. This provides a single source of truth for
// auditing, logging, and the `mcpp self fallbacks` command.
//
// Usage:
//   auto* fb = mcpp::fallback::find("xpkg.copy_from_global");
//   mcpp::log::verbose("fetcher", std::format("[fallback:{}] {}", fb->id, fb->description));

export module mcpp.fallback.registry;

import std;

export namespace mcpp::fallback {

enum class Lifecycle {
    permanent,   // architecturally required (multi-platform, retry logic)
    compat,      // backward compatibility, remove by specified version
    workaround,  // works around external bug, remove when upstream fixed
};

struct Entry {
    std::string_view id;             // unique key, e.g. "xpkg.copy_from_global"
    std::string_view domain;         // "package" | "config" | "toolchain" | "build" | "dependency" | "manifest"
    std::string_view description;    // one-line human-readable description
    Lifecycle        lifecycle;
    std::string_view removeBy;       // version string "1.0" or "" (permanent/workaround)
    std::string_view upstreamIssue;  // "xlings#123" or "" or description of upstream issue
};

// ─── Registry entries ────────────────────────────────────────────────

inline constexpr Entry kEntries[] = {

    // ─── Package fetch & install ─────────────────────────────────────

    {   .id          = "xpkg.copy_from_global",
        .domain      = "package",
        .description = "copy xpkg from ~/.xlings/ when sandbox install fails",
        .lifecycle   = Lifecycle::workaround,
        .removeBy    = {},
        .upstreamIssue = "xlings XLINGS_HOME propagation / NDJSON large-package install",
    },
    {   .id          = "xpkg.install_direct_before_ndjson",
        .domain      = "package",
        .description = "try direct xlings install before NDJSON interface for large packages",
        .lifecycle   = Lifecycle::workaround,
        .removeBy    = {},
        .upstreamIssue = "xlings NDJSON interface unreliable for large packages",
    },
    {   .id          = "xpkg.install_dir_scan",
        .domain      = "package",
        .description = "last-resort scan xpkgs/ directory for matching package dir",
        .lifecycle   = Lifecycle::compat,
        .removeBy    = "1.0",
    },
    {   .id          = "xpkg.lua_candidates",
        .domain      = "package",
        .description = "multi-candidate xpkg .lua file lookup for legacy naming",
        .lifecycle   = Lifecycle::compat,
        .removeBy    = "1.0",
    },

    // ─── xlings binary acquisition ───────────────────────────────────

    {   .id          = "xlings_binary.vendored_env",
        .domain      = "config",
        .description = "MCPP_VENDORED_XLINGS env override (Windows runtime workaround)",
        .lifecycle   = Lifecycle::workaround,
        .removeBy    = {},
        .upstreamIssue = "Windows xlings binary may lack runtime after copy",
    },
    {   .id          = "xlings_binary.system_which",
        .domain      = "config",
        .description = "find xlings in PATH when bundled binary unavailable",
        .lifecycle   = Lifecycle::permanent,
    },

    // ─── Toolchain probing ───────────────────────────────────────────

    {   .id          = "probe.sysroot_compiler",
        .domain      = "toolchain",
        .description = "gcc -print-sysroot direct probe",
        .lifecycle   = Lifecycle::permanent,
    },
    {   .id          = "probe.sysroot_cfg",
        .domain      = "toolchain",
        .description = "parse clang++.cfg for --sysroot line",
        .lifecycle   = Lifecycle::permanent,
    },
    {   .id          = "probe.sysroot_xcrun",
        .domain      = "toolchain",
        .description = "macOS xcrun --show-sdk-path fallback",
        .lifecycle   = Lifecycle::permanent,
    },
    {   .id          = "probe.sysroot_xlings_remap",
        .domain      = "toolchain",
        .description = "remap xlings build-time sysroot path to mcpp registry",
        .lifecycle   = Lifecycle::workaround,
        .removeBy    = {},
        .upstreamIssue = "xlings bakes build-host absolute path into gcc specs",
    },
    {   .id          = "sysroot.symlink_kernel_headers",
        .domain      = "toolchain",
        .description = "symlink linux-headers xpkg into sysroot when missing",
        .lifecycle   = Lifecycle::workaround,
        .removeBy    = {},
        .upstreamIssue = "xlings sysroot may lack kernel headers after init",
    },
    {   .id          = "sysroot.symlink_glibc_headers",
        .domain      = "toolchain",
        .description = "symlink glibc xpkg headers into sysroot when missing",
        .lifecycle   = Lifecycle::workaround,
        .removeBy    = {},
        .upstreamIssue = "xlings sysroot may lack glibc headers after init",
    },

    // ─── Build system ────────────────────────────────────────────────

    {   .id          = "build.ninja_incremental_retry",
        .domain      = "build",
        .description = "full rebuild when incremental ninja build fails",
        .lifecycle   = Lifecycle::permanent,
    },
    {   .id          = "build.dyndep_opt_out",
        .domain      = "build",
        .description = "MCPP_NINJA_DYNDEP=0 disables P1689 dynamic deps",
        .lifecycle   = Lifecycle::permanent,
    },

    // ─── Dependency resolution ───────────────────────────────────────

    {   .id          = "deps.multi_version_mangle",
        .domain      = "dependency",
        .description = "cross-major version coexistence via module name mangling",
        .lifecycle   = Lifecycle::permanent,
    },

    // ─── Backward compatibility / migration ──────────────────────────

    {   .id          = "compat.dotted_package_name",
        .domain      = "manifest",
        .description = "split legacy 'ns.name' dotted package names",
        .lifecycle   = Lifecycle::compat,
        .removeBy    = "1.0",
    },
    {   .id          = "compat.config_index_migration",
        .domain      = "config",
        .description = "rename mcpp-index to mcpplibs in config files",
        .lifecycle   = Lifecycle::compat,
        .removeBy    = "1.0",
    },
};

inline constexpr std::size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

// ─── Query API ───────────────────────────────────────────────────────

constexpr const Entry* find(std::string_view id) {
    for (auto& e : kEntries)
        if (e.id == id) return &e;
    return nullptr;
}

inline constexpr const char* lifecycle_str(Lifecycle l) {
    switch (l) {
        case Lifecycle::permanent:  return "permanent";
        case Lifecycle::compat:     return "compat";
        case Lifecycle::workaround: return "workaround";
    }
    return "unknown";
}

} // namespace mcpp::fallback
