// mcpp.manifest:types — shared manifest data model.
//
// Everything both descriptor formats (mcpp.toml, xpkg .lua) synthesize
// into, plus format-agnostic helpers. No parsing lives here.

export module mcpp.manifest.types;

import std;
import mcpp.pm.dep_spec;     // M5.x pm/ subsystem refactor: DependencySpec lives here
import mcpp.pm.compat;       // Legacy dependency-key compatibility helpers
import mcpp.pm.index_spec;   // IndexSpec for [indices] section
import mcpp.platform;

export namespace mcpp::manifest {


// PR-R1 transitional: the dependency data model has moved into
// `mcpp.pm.dep_spec`. The aliases below keep `mcpp::manifest::DependencySpec`
// and `mcpp::manifest::kDefaultNamespace` available as before so existing
// callers (`cli.cppm`, `fetcher.cppm`, ...) compile unchanged. A later
// refactor PR will migrate call sites to reference `mcpp::pm::` directly
// and these aliases can disappear.
using DependencySpec = mcpp::pm::DependencySpec;
inline constexpr auto kDefaultNamespace = mcpp::pm::kDefaultNamespace;
inline constexpr auto kCompatNamespace  = mcpp::pm::kCompatNamespace;

struct CppStandardConfig {
    std::string                 canonical = "c++23";
    std::string                 flag = "-std=c++23";
    int                         level = 23;
    bool                        gnuDialect = false;
};

struct Package {
    std::string                 name;
    std::string                 namespace_;    // xpkg V1 namespace field (0.0.6+); empty = infer from name
    std::string                 version;
    std::string                 standard   = "c++23";   // C++ standard (M5.0: moved from [language])
    std::string                 description;
    std::string                 license;
    std::vector<std::string>    authors;
    std::string                 repo;
    std::vector<std::string>    platforms;     // declared supported platforms (CI matrix hint)
};

struct Language {
    std::string                 standard   = "c++23";
    bool                        modules    = true;
    bool                        importStd = true;
};

// Author-asserted scan result for one source glob (scan_overrides).
// Files matched by the glob bypass the M1 text scan entirely; the declared
// (provides, imports) enter the module graph directly. Sound because the
// declaration is verified against the compiler's own P1689 (.ddi) output
// at build time — assertion + verification instead of computation.
// Design: .agents/docs/2026-07-08-scanner-backend-abstraction-design.md §3-pre.
struct ScanOverride {
    std::vector<std::string> provides;   // module logical names the file exports
    std::vector<std::string> imports;    // module logical names the file imports
};

struct Modules {
    std::vector<std::string>    sources;        // glob patterns
    std::vector<std::string>    exports_;       // declared module names (optional)
    bool                        strict = false;
    // glob → declared scan result; every glob must match ≥1 source file.
    std::map<std::string, ScanOverride> scanOverrides;
};

struct Target {
    std::string                 name;
    enum Kind { Library, Binary, SharedLibrary, TestBinary } kind;
    std::string                 main;           // for binary / test
    std::string                 soname;         // ABI name for shared libraries, e.g. libfoo.so.1
    // Per-target compile flags. SCOPE: applied ONLY to this target's exclusive
    // entry source (its `main`) — never to shared module/impl objects, which are
    // compiled once and linked into every target (the build's compile-once model;
    // see src/build/plan.cppm). `defines` are sugar desugared to `-D<x>` at plan
    // time and applied to both the C and C++ entry compile. Use these for flags
    // that are private to a binary's own entry (e.g. `-DBUILD_SERVER=1`,
    // `-Wno-deprecated`); for divergence that must reach shared code, use a
    // workspace member or a [features] knob instead.
    std::vector<std::string>    cflags;
    std::vector<std::string>    cxxflags;
    std::vector<std::string>    defines;
    // Build gate: this target is emitted ONLY when every listed feature is
    // active in the current build (otherwise it is silently skipped). Gate
    // only — it does not activate features (use --features / [features].default).
    std::vector<std::string>    requiredFeatures;
};

// `DependencySpec` and `kDefaultNamespace` have moved to mcpp.pm.dep_spec.
// Aliases at the top of this file keep `mcpp::manifest::DependencySpec`
// resolvable for unchanged call sites.

// `[toolchain]` section per docs/21-toolchain-and-tools.md
//   linux   = "gcc@15.1.0"
//   macos   = "llvm@20"
//   windows = "msvc@system"
//   default = "gcc@15.1.0"   (used when current platform isn't listed)
struct Toolchain {
    std::map<std::string, std::string> byPlatform;   // platform -> "pkg@ver"

    // Returns the toolchain spec for a platform, falling back to "default".
    std::optional<std::string> for_platform(std::string_view platform) const {
        if (auto it = byPlatform.find(std::string(platform)); it != byPlatform.end()) {
            return it->second;
        }
        if (auto it = byPlatform.find("default"); it != byPlatform.end()) {
            return it->second;
        }
        return std::nullopt;
    }
};

// `[build]` section — tunables for the build backend.
//
// M5.0: now also carries `sources` (moved from [modules]) and `include_dirs`
// (new). Defaults are injected by load() after parse if these are empty.
struct BuildConfig {
    std::vector<std::string>           sources;        // glob patterns
    // feature name → extra source globs gated by that feature. A glob listed
    // here is EXCLUDED from the default build and only compiled/linked when the
    // feature is active for this package (resolved in prepare_build). Lets a
    // dependency expose an optional component (e.g. gtest's gtest_main.cc behind
    // the "main" feature) without it being linked by default — see
    // .agents/docs/2026-06-25-gtest-main-feature-and-add-dev-design.md.
    std::map<std::string, std::vector<std::string>> featureSources;
    // feature name → package-owned preprocessor defines (e.g. "-DEIGEN_USE_BLAS").
    // Feature System v2 Stage 1: when the feature is active these are appended to
    // the package's compile flags alongside the automatic -DMCPP_FEATURE_<NAME>
    // (resolved in prepare_build). Restricted by convention to the package's own
    // namespaced macros — features do NOT inject free-form cflags/ldflags, which
    // would break feature-union composition. See
    // .agents/docs/2026-06-29-feature-capability-model-design.md.
    std::map<std::string, std::vector<std::string>> featureDefines;
    std::vector<std::filesystem::path> includeDirs;    // relative to package root
    std::map<std::filesystem::path, std::string> generatedFiles; // Form B package-owned support files
    bool                                staticStdlib = true;
    // "" (default = dynamic), "static", "dynamic" — chosen at resolve
    // time from --static / --target / [target.<triple>].linkage. Wired
    // through to ninja backend as the `-static` link flag.
    std::string                         linkage;
    // M5.x C-language support. `cflags` / `cxxflags` are appended verbatim
    // to the per-rule baseline (see `ninja_backend` cflags / cxxflags).
    // `cStandard` controls -std= for the C compile rule (.c files).
    // Empty cStandard → backend default ("c11" today).
    std::vector<std::string>           cflags;
    std::vector<std::string>           cxxflags;
    std::vector<std::string>           ldflags;
    std::string                         cStandard;
    // Escape hatch for the hermetic link check: a sandbox toolchain whose
    // CRT/loader resolve OUTSIDE the sandbox is a hard error by default
    // (silent host contamination, or issue #195's bare-CRT link failure);
    // set true to deliberately link against host libraries.
    // MCPP_ALLOW_HOST_LIBS=1 is the per-invocation equivalent.
    bool                                allowHostLibs = false;
    // macOS minimum supported OS version for produced binaries
    // (LC_BUILD_VERSION minos), e.g. "14.0". Mirrors the ecosystem
    // conventions around deployment targets (the MACOSX_DEPLOYMENT_TARGET
    // env var that cargo/rustc/cc honor; SwiftPM's `platforms:` manifest
    // field; CMAKE_OSX_DEPLOYMENT_TARGET). Precedence: the env var (an
    // explicit per-invocation override) wins over this manifest default;
    // empty + no env = toolchain/SDK default. No effect off macOS.
    std::string                         macosDeploymentTarget;
    // Resolved build-profile knobs (from [profile.<name>] + built-in defaults).
    std::string                         optLevel = "2";  // -O level
    bool                                debug    = false; // -g
    bool                                lto      = false; // -flto
    bool                                strip    = false; // link -s
    // `[build].default-profile` (alias: `profile`) — the project's DEFAULT
    // profile when no --profile/--dev/--release is passed. The global convention
    // default stays "release"; this lets a project opt its plain `mcpp build`
    // into e.g. "dev" without typing --profile. Precedence: --profile/--dev/
    // --release flag > [build].default-profile > "release". NOTE (distribution
    // footgun): a project that defaults to dev should pass `--profile release`
    // when producing a distributable (a pack-time release guard is a follow-up).
    std::string                         defaultProfile;
};

// `[runtime]` — requirements needed when launching built binaries.
struct RuntimeConfig {
    std::vector<std::filesystem::path> libraryDirs;   // relative to package root
    std::vector<std::string>           dlopenLibs;    // runtime-loaded sonames
    std::vector<std::string>           capabilities;  // host/system capabilities REQUIRED
    // Capabilities this package explicitly FULFILS (strong provider claim).
    // Packages that merely list a capability in `capabilities` are weak
    // providers (back-compat); provides-declarers win provider selection.
    std::vector<std::string>           provides;
    // [runtime.<capability>] provider = "<pkg>" — explicit provider selection
    // (the three-tier knob: default/auto → explicit override).
    std::map<std::string, std::string> providerOverrides;
};

// `[xlings]` — the project's build ENVIRONMENT (L-1). The subsection names mirror
// xlings' own `.xlings.json` schema 1:1, so mcpp materializes them verbatim into
// `<proj>/.mcpp/.xlings.json` (no translation layer): `deps` (host build-tools
// installed by xlings), `[xlings.workspace]` (tool→version pins, the general form
// of `[toolchain]`), `subos` (a named per-project sandbox), `[xlings.envs]`
// (env vars applied by xvm shims). See
// .agents/docs/2026-06-29-manifest-environment-and-platform-design.md (L-1).
struct XlingsConfig {
    std::vector<std::string>           deps;       // → .xlings.json "deps"
    std::map<std::string, std::string> workspace;  // → "workspace" (tool → version)
    std::string                        subos;      // → "subos" (named project sandbox)
    std::map<std::string, std::string> envs;       // → "envs" (env var → value)

    bool empty() const {
        return deps.empty() && workspace.empty() && subos.empty() && envs.empty();
    }
};

// `[target.<triple>]` — per-target overrides.
// Picked up when caller passes --target <triple> to build/run/test.
struct TargetEntry {
    std::string                         toolchain;     // e.g. "gcc@15.1.0-musl"; empty = inherit [toolchain]
    std::string                         linkage;       // "static" | "dynamic" | "" (= auto by libc)
};

// `[target.'cfg(...)'.build]` — platform-conditional build flags (L1). The
// predicate is the raw `[target.<predicate>]` key (e.g. `cfg(windows)`,
// `cfg(all(linux, not(arch="aarch64")))`, or a bare triple). It is stored
// DEFERRED here because manifest parsing is target-agnostic; prepare_build
// evaluates it against the RESOLVED target (host triple for a native build,
// the --target triple for a cross build) and merges matching flags into
// buildConfig. See .agents/docs/2026-06-29-manifest-environment-and-platform-design.md.
struct ConditionalConfig {
    std::string                         predicate;     // the [target.<predicate>] key
    std::vector<std::string>            cflags;
    std::vector<std::string>            cxxflags;
    std::vector<std::string>            ldflags;
    // Conditional dependencies (Phase 1b): merged into the corresponding
    // manifest maps in prepare_build when the predicate matches the resolved
    // target — before dependency resolution, so they resolve like any dep.
    std::map<std::string, DependencySpec> dependencies;
    std::map<std::string, DependencySpec> devDependencies;
    std::map<std::string, DependencySpec> buildDependencies;
};

// `[lib]` — library "root" interface convention.
//
// Convention-over-configuration: a library package's primary module
// interface lives at `src/<package-tail>.cppm`, where `<package-tail>` is
// the last dotted segment of `[package].name` (e.g. `mcpplibs.tinyhttps`
// → `src/tinyhttps.cppm`). That file declares `export module
// <full-package-name>;` and re-exports the public partitions. The lib
// root then drives:
//   * `[modules].exports` default (the lib root's module = the only
//     externally-visible base module),
//   * `mcpp publish` xpkg generation (consumer just `import <name>;`),
//   * downstream tooling (docs / explain) entry point.
//
// Override the convention with `[lib].path = "src/foo.cppm"` (cargo-style)
// — the file must still `export module <package-name>;` (no partition).
//
// Lib-root is only meaningful for projects that ship a `kind = "lib"`
// target. Pure-binary projects (mcpp itself, scaffolded `mcpp new`)
// don't trigger any lib-root checks.
struct LibConfig {
    std::filesystem::path               path;          // explicit override; empty = use convention
};

// `[pack]` — `mcpp pack` configuration. See docs/35-pack-design.md.
//
// `default_mode` picks the bundling strategy when the user runs bare
// `mcpp pack` (no `--mode` flag):
//   "static"          — full musl static, no PT_INTERP / RUNPATH
//   "bundle-project"  — bundle only project's third-party .so (default)
//   "bundle-all"      — bundle every dynamic dep including libc / libstdc++
struct PackConfig {
    std::string                         defaultMode;   // empty → "bundle-project"
    std::vector<std::string>            include;       // extra files/globs to ship
    std::vector<std::string>            exclude;       // patterns to drop from include
    // Mode C overrides — let the user expand or contract the PEP 600
    // skip list when their target distros differ from the default
    // assumption ("modern desktop Linux").
    std::vector<std::string>            alsoSkip;      // libs to ALSO skip on top of PEP 600
    std::vector<std::string>            forceBundle;   // libs to bundle even if PEP 600 says skip
};

// `[workspace]` — multi-package workspace support (0.0.11+).
//
// A workspace root mcpp.toml declares member packages. Members share
// a unified lock file, target directory, and can inherit dependency
// versions via `.workspace = true`.
//
// Virtual workspace (no [package]): pure management node.
// Rooted workspace ([package] + [workspace]): root is also a package.
struct WorkspaceConfig {
    std::vector<std::string>                            members;       // relative paths to member dirs
    std::vector<std::string>                            exclude;       // paths to exclude
    std::map<std::string, DependencySpec>               dependencies;  // [workspace.dependencies]
    bool                                                present = false;
};

// [profile.<name>] — bundled build settings (opt level, debug, lto, strip).
struct Profile {
    std::string optLevel = "2";
    bool        debug    = false;
    bool        lto      = false;
    bool        strip    = false;
    // Passthrough escape hatch (fixed keys, open values — I6 completeness):
    std::vector<std::string> cflags;
    std::vector<std::string> cxxflags;
    std::vector<std::string> ldflags;
};

struct Manifest {
    std::filesystem::path       sourcePath;    // mcpp.toml's filesystem path

    // Unknown top-level keys silently skipped while synthesizing from an
    // xpkg mcpp segment — surfaced as warnings by `mcpp xpkg parse` so
    // schema evolution is loud in lint instead of invisible.
    std::vector<std::string>    xpkgUnknownKeys;

    Package                     package;
    Language                    language;
    Modules                     modules;
    std::vector<Target>         targets;

    // version-string keyed dependencies (M2 short form only).
    std::map<std::string, DependencySpec> dependencies;
    std::map<std::string, DependencySpec> devDependencies;
    std::map<std::string, DependencySpec> buildDependencies;   // host-side tools (M5+ behavior)

    Toolchain                   toolchain;     // optional; empty == fallback
    BuildConfig                 buildConfig;
    RuntimeConfig               runtimeConfig;
    XlingsConfig                xlings;             // [xlings] build environment (L-1)
    std::vector<ConditionalConfig> conditionalConfigs;  // [target.'cfg(...)'.build], deferred
    std::map<std::string, Profile> profiles;   // [profile.<name>]
    // [features] — feature name → implied features ("default" = default set).
    std::map<std::string, std::vector<std::string>> featuresMap;

    // Feature System v2 Stage 3 — capabilities (provides/requires). A capability
    // is just a shared string. A package satisfies one via package-level
    // `provides` or via a feature's `provides`; a feature `requires` an abstract
    // capability instead of a concrete package, and the resolver binds exactly
    // one provider from the graph. See
    // .agents/docs/2026-06-29-feature-capability-model-design.md.
    std::vector<std::string>                        provides;        // package-level
    std::map<std::string, std::vector<std::string>> featureProvides; // feature → caps
    std::map<std::string, std::vector<std::string>> featureRequires; // feature → caps
    // Feature System v2 Stage 2a — dependencies activated by a feature. A dep
    // declared ONLY here is optional: pulled into the resolution worklist only
    // when its feature is active (root --features or a dep spec's features=[...]).
    // Each value is a full DependencySpec, so a feature-dep may itself request
    // features. See .agents/docs/2026-06-29-feature-optional-dependencies-s2-design.md.
    std::map<std::string, std::map<std::string, DependencySpec>> featureDeps;
    // Root-only: [capabilities] cap = "provider" pins (also fed by --cap).
    std::map<std::string, std::string>              capabilityPins;

    // [target.<triple>] tables — empty if user didn't declare any.
    std::map<std::string, TargetEntry> targetOverrides;

    // [pack] — `mcpp pack` config (see docs/35-pack-design.md).
    PackConfig                         packConfig;

    // [lib] — library root interface convention (M5.x+).
    LibConfig                          lib;

    // [workspace] — multi-package workspace.
    WorkspaceConfig                    workspace;

    // [indices] — custom package index repositories (index-name → IndexSpec).
    std::map<std::string, mcpp::pm::IndexSpec> indices;

    // M5.0: post-parse computed/inferred state
    CppStandardConfig           cppStandard;
    bool                        usesModules    = true;   // refined by scanner
    bool                        usesImportStd  = true;   // refined by scanner
    std::vector<std::string>    inferredNotes;           // for `Inferred ...` banner

    // Non-fatal schema warnings collected during parse (e.g. unsupported keys
    // under [targets.<name>]). The caller (prepare_build) prints these and, under
    // --strict, escalates them to errors — mirroring the feature/platform path.
    std::vector<std::string>    schemaWarnings;
};

struct ManifestError {
    std::string                 message;
    std::filesystem::path       file;
    std::size_t                 line   = 0;
    std::size_t                 column = 0;

    std::string format() const {
        if (line)
            return std::format("{}:{}:{}: error: {}", file.string(), line, column, message);
        return std::format("{}: error: {}", file.string(), message);
    }
};

std::expected<CppStandardConfig, std::string> normalize_cpp_standard(std::string_view raw);

std::filesystem::path resolve_lib_root_path(const Manifest& manifest);

// True if the manifest declares at least one `kind = "lib"` target.
// Lib-root convention only applies when this returns true.
bool has_lib_target(const Manifest& manifest);


// GCC 15 cross-link workaround anchor — see definition below.
void force_template_instantiations();

} // namespace mcpp::manifest

// ── helpers shared by the toml and xpkg parsers (exported: separate
//    modules need them reachable; they live in the manifest namespace) ──
export namespace mcpp::manifest {

bool starts_with_std_flag(std::string_view flag) {
    return flag == "-std" || flag.starts_with("-std=");
}

bool is_basename(std::string_view value) {
    return !value.empty()
        && value.find('/') == std::string_view::npos
        && value.find('\\') == std::string_view::npos;
}

std::optional<std::string> validate_target_soname(const Target& t,
                                                  std::string_view targetPath) {
    if (t.soname.empty()) return std::nullopt;
    if (t.kind != Target::SharedLibrary) {
        return std::format("{}soname is only valid for shared targets", targetPath);
    }
    if (!is_basename(t.soname)) {
        return std::format("{}soname must be a library basename, got '{}'",
                           targetPath, t.soname);
    }
    return std::nullopt;
}


std::expected<CppStandardConfig, std::string> normalize_cpp_standard(std::string_view raw) {
    auto trim_copy = [](std::string_view input) {
        std::size_t begin = 0;
        while (begin < input.size()
               && std::isspace(static_cast<unsigned char>(input[begin]))) {
            ++begin;
        }
        std::size_t end = input.size();
        while (end > begin
               && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
            --end;
        }
        return std::string(input.substr(begin, end - begin));
    };

    std::string s = trim_copy(raw);
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    CppStandardConfig out;
    if (s.empty() || s == "c++23" || s == "c++2b") {
        out.canonical = "c++23";
        out.flag = "-std=c++23";
        out.level = 23;
        out.gnuDialect = false;
        return out;
    }
    if (s == "gnu++23" || s == "gnu++2b") {
        out.canonical = "gnu++23";
        out.flag = "-std=gnu++23";
        out.level = 23;
        out.gnuDialect = true;
        return out;
    }
    if (s == "c++26" || s == "c++2c") {
        out.canonical = "c++26";
        out.flag = "-std=c++26";
        out.level = 26;
        out.gnuDialect = false;
        return out;
    }
    if (s == "gnu++26" || s == "gnu++2c") {
        out.canonical = "gnu++26";
        out.flag = "-std=gnu++26";
        out.level = 26;
        out.gnuDialect = true;
        return out;
    }
    if (s == "c++latest") {
        out.canonical = "c++latest";
        out.flag = "-std=c++26";
        out.level = 999;
        out.gnuDialect = false;
        return out;
    }

    return std::unexpected(std::format(
        "unsupported C++ standard '{}'; expected c++23, c++26, c++2c, gnu++23, gnu++26, or c++latest",
        raw));
}

bool has_lib_target(const Manifest& manifest) {
    for (auto& t : manifest.targets) {
        if (t.kind == Target::Library || t.kind == Target::SharedLibrary) {
            return true;
        }
    }
    return false;
}

std::filesystem::path resolve_lib_root_path(const Manifest& manifest) {
    if (!manifest.lib.path.empty()) {
        return manifest.lib.path;
    }
    // Convention: src/<package-tail>.cppm
    std::string tail = manifest.package.name;
    if (auto p = tail.rfind('.'); p != std::string::npos) {
        tail = tail.substr(p + 1);
    }
    return std::filesystem::path("src") / (tail + ".cppm");
}



// ── GCC 15 cross-link workaround ────────────────────────────────────
// GCC 15 (aarch64-linux-musl cross, xim gcc 15.1.0) does not emit
// implicit template instantiations for std::map/... members of
// module-attached structs in IMPORTER object files — it expects the
// owning module to provide them. The old single-file mcpp.manifest
// emitted them by accident (its parser code constructed every struct);
// this non-inline, exported definition recreates that guarantee
// deliberately. Remove once the cross toolchain floor is GCC 16.
void force_template_instantiations() {
    Manifest          m;
    WorkspaceConfig   w;
    XlingsConfig      x;
    Modules           mo;
    BuildConfig       bc;
    RuntimeConfig     rc;
    TargetEntry       te;
    ConditionalConfig cc;
    LibConfig         lc;
    PackConfig        pc;
    Profile           pr;
    Toolchain         tc;
    (void)m; (void)w; (void)x; (void)mo; (void)bc; (void)rc;
    (void)te; (void)cc; (void)lc; (void)pc; (void)pr; (void)tc;
}

} // namespace mcpp::manifest
