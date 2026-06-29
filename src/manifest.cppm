// mcpp.manifest — load and validate mcpp.toml

export module mcpp.manifest;

import std;
import mcpp.libs.toml;
import mcpp.pm.dep_spec;     // M5.x pm/ subsystem refactor: DependencySpec lives here
import mcpp.pm.compat;       // Legacy dependency-key compatibility helpers
import mcpp.pm.dependency_selector;
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

struct Modules {
    std::vector<std::string>    sources;        // glob patterns
    std::vector<std::string>    exports_;       // declared module names (optional)
    bool                        strict = false;
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

// `[target.<triple>]` — per-target overrides.
// Picked up when caller passes --target <triple> to build/run/test.
struct TargetEntry {
    std::string                         toolchain;     // e.g. "gcc@15.1.0-musl"; empty = inherit [toolchain]
    std::string                         linkage;       // "static" | "dynamic" | "" (= auto by libc)
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

std::expected<Manifest, ManifestError> parse_string(std::string_view content,
                                                    const std::filesystem::path& origin = "mcpp.toml");
std::expected<Manifest, ManifestError> load(const std::filesystem::path& path);
std::expected<CppStandardConfig, std::string> normalize_cpp_standard(std::string_view raw);

// For `mcpp new` scaffolding.
std::string default_template(std::string_view packageName);

// M6.x: `mcpp` field in xpkg.lua may be either:
//   - a string (path to mcpp.toml inside the extracted tarball, glob-able)
//   - a table (inline Form B descriptor)
// extract_mcpp_field discriminates and returns the right kind.
struct McppField {
    enum Kind { Absent, StringPath, TableBody } kind = Absent;
    std::string                 value;   // glob path (StringPath) or table body (TableBody)
};
McppField extract_mcpp_field(std::string_view luaContent);

// Extract the list of available versions for `platform` (e.g. "linux", "macosx",
// "windows") from an xpkg .lua's xpm.<platform> = { ["X.Y.Z"] = {...}, ... }.
// Returns an empty vector if the platform table is missing or has no entries.
std::vector<std::string>
list_xpkg_versions(std::string_view luaContent, std::string_view platform);

// Extract the `namespace` field from an xpkg .lua's `package = { ... }` block.
// Returns empty string if the field is absent (legacy descriptors).
std::string extract_xpkg_namespace(std::string_view luaContent);

// Extract the `name` field from an xpkg .lua's `package = { ... }` block.
// Returns empty string if the field is absent.
std::string extract_xpkg_name(std::string_view luaContent);

// Canonical package identity — the unified (ns, name) model (design doc §4.2).
//
// A package's identity is a 2-tuple: `ns` is a hierarchical namespace path
// (sub-namespaces, dotted: `compat`, `a.b.c`), `name` is a single atomic
// segment. Surface spellings (dotted name, embedded prefix, missing namespace)
// all normalize to this tuple. Normalization:
//   1. If `declaredNs` is empty, inherit `indexDefaultNs` (owning-index ns).
//   2. Fully-qualified name: if `declaredName` already starts with `ns.`, it is
//      the FQN; otherwise FQN = `ns.declaredName` (or just `declaredName` when
//      there is no namespace at all).
//   3. Split the FQN on its LAST dot: prefix → `ns`, final segment → `name`.
struct XpkgIdentity {
    std::string ns;
    std::string name;
    bool operator==(const XpkgIdentity&) const = default;
};
XpkgIdentity canonical_xpkg_identity(std::string_view declaredNs,
                                     std::string_view declaredName,
                                     std::string_view indexDefaultNs = {});

// Convenience: the canonical identity of an xpkg .lua, read from its declared
// `package.{namespace,name}`. Empty `name` field → empty identity.
XpkgIdentity canonical_xpkg_identity_from_lua(std::string_view luaContent,
                                              std::string_view indexDefaultNs = {});

// Identity gate: does this xpkg .lua actually DECLARE the package the caller
// asked for? Compares the descriptor's declared `package.{name,namespace}`
// against the requested (ns, shortName) coordinate. This is the invariant that
// makes filename-based lookup safe — a file found by a candidate filename is
// only accepted when it *is* the requested package, so a bare `zlib.lua` from a
// foreign index never satisfies a request for `compat.zlib`.
//
// A descriptor that declares no `name` is accepted (cannot verify → lenient).
// `allowLegacyBareDefault` governs only the default-namespace case: whether a
// no-namespace descriptor whose bare name matches counts as the default-ns
// package (preserves legacy bare-named mcpplibs descriptors).
//
// `indexDefaultNs` is the namespace OWNED BY the index the descriptor was found
// in. When the read is scoped to a single known index (e.g. a `[indices]` local
// path index owns namespace `local-dev`), a descriptor that declares no
// namespace inherits the index's — so `tinycfg.lua` (name only) in the
// `local-dev` index matches a request for `(local-dev, tinycfg)`. Empty for
// multi-index scans where the owning index isn't known per-file (the builtin
// global scan); see the design doc §4.1.
bool xpkg_lua_identity_matches(std::string_view luaContent,
                               std::string_view ns,
                               std::string_view shortName,
                               bool allowLegacyBareDefault = true,
                               std::string_view indexDefaultNs = {});

// Resolve the lib-root path for a manifest:
//   1. `[lib].path` if explicitly set (cargo-style override),
//   2. otherwise the convention `src/<package-tail>.cppm`, where
//      `<package-tail>` is the last `.`-segment of [package].name
//      (e.g. `mcpplibs.tinyhttps` → `src/tinyhttps.cppm`).
// The returned path is relative to the package root unless the user
// passed an absolute path in `[lib].path`.
std::filesystem::path resolve_lib_root_path(const Manifest& manifest);

// True if the manifest declares at least one `kind = "lib"` target.
// Lib-root convention only applies when this returns true.
bool has_lib_target(const Manifest& manifest);

// Synthesize a Manifest from an xpkg .lua file's `mcpp = {}` segment.
// Used when a fetched dep has no source/mcpp.toml — the index entry's
// `mcpp = {}` workaround block carries the missing build info.
//
// The resulting Manifest is in-memory only; sourcePath is set to the
// supplied package name + version so error messages can refer to it.
std::expected<Manifest, ManifestError>
synthesize_from_xpkg_lua(std::string_view luaContent,
                         std::string_view packageName,
                         std::string_view packageVersion);

} // namespace mcpp::manifest

// =====================================================================
// Implementation
// =====================================================================

namespace mcpp::manifest {

namespace t = mcpp::libs::toml;

namespace {

bool starts_with_std_flag(std::string_view flag) {
    return flag == "-std" || flag.starts_with("-std=");
}

ManifestError error(const std::filesystem::path& origin,
                    const std::string& msg,
                    t::Position pos = {0, 0}) {
    return ManifestError{msg, origin, pos.line, pos.column};
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

} // namespace

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

std::expected<Manifest, ManifestError> parse_string(std::string_view content,
                                                    const std::filesystem::path& origin) {
    auto doc = t::parse(content);
    if (!doc) {
        return std::unexpected(error(origin, doc.error().message, doc.error().where));
    }

    Manifest m;
    m.sourcePath = origin;

    // [package] — required unless [workspace] is present (virtual workspace).
    auto* pkg_t = doc->get_table("package");
    bool has_workspace = (doc->get_table("workspace") != nullptr);
    if (!pkg_t && !has_workspace)
        return std::unexpected(error(origin, "missing required [package] section"));

    auto name = doc->get_string("package.name");
    if (!name && !has_workspace)
        return std::unexpected(error(origin, "missing required field 'package.name'"));
    if (name) m.package.name = *name;

    // 0.0.6+: explicit namespace field (xpkg V1 style).
    // If present, [package].name is the short name.
    // If absent, compat.cppm::resolve_package_name infers from dotted name.
    if (auto v = doc->get_string("package.namespace")) m.package.namespace_ = *v;

    auto version = doc->get_string("package.version");
    if (!version && !has_workspace)
        return std::unexpected(error(origin, "missing required field 'package.version'"));
    if (version) m.package.version = *version;

    if (auto v = doc->get_string("package.description")) m.package.description = *v;
    if (auto v = doc->get_string("package.license"))     m.package.license     = *v;
    if (auto v = doc->get_string("package.repo"))        m.package.repo        = *v;
    if (auto v = doc->get_string_array("package.authors")) m.package.authors  = *v;
    if (auto v = doc->get_string_array("package.platforms")) m.package.platforms = *v;

    // [package].standard (M5.0 new home)
    if (auto v = doc->get_string("package.standard"))    m.package.standard    = *v;

    // [language] (M5.0: deprecated, kept for backward compat — drop in M6)
    // Reads to old fields AND mirrors to new package.standard if [package].standard not set.
    bool had_language_section = (doc->get_table("language") != nullptr);
    if (auto v = doc->get_string("language.standard")) {
        m.language.standard = *v;
        // mirror to new home only if [package].standard wasn't explicitly set
        if (!doc->get_string("package.standard")) m.package.standard = *v;
    } else {
        m.language.standard = m.package.standard;   // keep old field consistent with new
    }
    if (auto v = doc->get_bool("language.modules"))      m.language.modules    = *v;
    if (auto v = doc->get_bool("language.import_std"))   m.language.importStd = *v;

    // Validation on the unified standard. Store the canonical spelling so all
    // downstream build surfaces consume one active value.
    auto stdCfg = normalize_cpp_standard(m.package.standard);
    if (!stdCfg) return std::unexpected(error(origin, stdCfg.error()));
    m.cppStandard = *stdCfg;
    m.package.standard = m.cppStandard.canonical;
    m.language.standard = m.cppStandard.canonical;
    if (had_language_section && !m.language.modules) {
        return std::unexpected(error(origin,
            "language.modules must be true (mcpp is modules-only)"));
    }

    // [build].sources (M5.0 new home) + [modules].sources (deprecated, compat)
    if (auto v = doc->get_string_array("build.sources"))   m.buildConfig.sources = *v;
    if (auto v = doc->get_string_array("modules.sources")) {
        m.modules.sources = *v;
        // If [build].sources wasn't set, mirror legacy field into new field.
        if (m.buildConfig.sources.empty()) m.buildConfig.sources = *v;
    }
    // Mirror new → legacy so existing code reading manifest.modules.sources keeps working.
    if (m.modules.sources.empty()) m.modules.sources = m.buildConfig.sources;

    if (auto v = doc->get_string_array("modules.exports")) m.modules.exports_ = *v;
    if (auto v = doc->get_bool("modules.strict"))          m.modules.strict   = *v;

    // [build].include_dirs (M5.0 new field)
    if (auto v = doc->get_string_array("build.include_dirs")) {
        for (auto& s : *v) m.buildConfig.includeDirs.emplace_back(s);
    }

    // [targets.*] — M5.0: now optional. If absent, defer to auto-inference (in load()).
    // [profile.<name>] — bundled build settings.
    if (auto* profile_table = doc->get_table("profile");
        profile_table && !profile_table->empty()) {
        for (auto& [pname, pval] : *profile_table) {
            if (!pval.is_table()) continue;
            auto& tt = pval.as_table();
            Profile pr;
            if (auto it = tt.find("opt"); it != tt.end()) {
                if      (it->second.is_string()) pr.optLevel = it->second.as_string();
                else if (it->second.is_int())    pr.optLevel = std::to_string(it->second.as_int());
            }
            if (auto it = tt.find("debug"); it != tt.end() && it->second.is_bool()) pr.debug = it->second.as_bool();
            if (auto it = tt.find("lto");   it != tt.end() && it->second.is_bool()) pr.lto   = it->second.as_bool();
            if (auto it = tt.find("strip"); it != tt.end() && it->second.is_bool()) pr.strip = it->second.as_bool();
            auto read_list = [&](const char* key, std::vector<std::string>& out) {
                if (auto it = tt.find(key); it != tt.end() && it->second.is_array())
                    for (auto& v : it->second.as_array())
                        if (v.is_string()) out.push_back(v.as_string());
            };
            read_list("cflags",   pr.cflags);
            read_list("cxxflags", pr.cxxflags);
            read_list("ldflags",  pr.ldflags);
            m.profiles[pname] = pr;
        }
    }

    // [features] — feature name → implied features. "default" lists the
    // default-active set. Two accepted shapes (Feature System v2):
    //   array form (shorthand):  name = ["implied", ...]
    //   table form (full):       name = { implies = [...], defines = [...] }
    // The table form lets a feature contribute package-owned defines (Stage 1);
    // `requires`/`provides`/`deps` keys are reserved for later stages.
    if (auto* features_table = doc->get_table("features");
        features_table && !features_table->empty()) {
        auto read_str_array = [](const auto& tbl, std::string_view key,
                                 std::vector<std::string>& out) {
            if (auto it = tbl.find(std::string(key));
                it != tbl.end() && it->second.is_array())
                for (auto& v : it->second.as_array())
                    if (v.is_string()) out.push_back(v.as_string());
        };
        for (auto& [fname, fval] : *features_table) {
            std::vector<std::string> implied;
            if (fval.is_array()) {
                for (auto& v : fval.as_array())
                    if (v.is_string()) implied.push_back(v.as_string());
            } else if (fval.is_table()) {
                auto& ft = fval.as_table();
                read_str_array(ft, "implies", implied);
                std::vector<std::string> defs;
                read_str_array(ft, "defines", defs);
                if (!defs.empty()) m.buildConfig.featureDefines[fname] = std::move(defs);
                std::vector<std::string> reqs, provs;
                read_str_array(ft, "requires", reqs);
                read_str_array(ft, "provides", provs);
                if (!reqs.empty())  m.featureRequires[fname] = std::move(reqs);
                if (!provs.empty()) m.featureProvides[fname] = std::move(provs);
            }
            m.featuresMap[fname] = std::move(implied);
        }
    }

    // [package] provides — package-level capabilities (Feature System v2 S3).
    if (auto v = doc->get_string_array("package.provides")) m.provides = *v;

    // [capabilities] cap = "provider" — root-only provider pins.
    if (auto* caps = doc->get_table("capabilities"); caps && !caps->empty()) {
        for (auto& [cap, cval] : *caps)
            if (cval.is_string()) m.capabilityPins[cap] = cval.as_string();
    }

    auto* targets_table = doc->get_table("targets");
    if (targets_table && !targets_table->empty()) {
    for (auto& [tname, tval] : *targets_table) {
        if (!tval.is_table()) {
            return std::unexpected(error(origin,
                std::format("[targets.{}] must be a table", tname)));
        }
        Target t;
        t.name = tname;
        auto& tt = tval.as_table();

        auto kit = tt.find("kind");
        if (kit == tt.end() || !kit->second.is_string()) {
            return std::unexpected(error(origin,
                std::format("targets.{}.kind missing or not a string", tname)));
        }
        const auto& kind_s = kit->second.as_string();
        if      (kind_s == "lib"    || kind_s == "library")  t.kind = Target::Library;
        else if (kind_s == "bin"    || kind_s == "binary")   t.kind = Target::Binary;
        else if (kind_s == "shared" || kind_s == "dylib"
              || kind_s == "so"     || kind_s == "shlib")    t.kind = Target::SharedLibrary;
        else return std::unexpected(error(origin,
            std::format("targets.{}.kind must be 'bin', 'lib' or 'shared'; got '{}'", tname, kind_s)));

        if (t.kind == Target::Binary) {
            auto mit = tt.find("main");
            if (mit == tt.end() || !mit->second.is_string()) {
                return std::unexpected(error(origin,
                    std::format("targets.{} (kind=bin) requires 'main' field", tname)));
            }
            t.main = mit->second.as_string();
        }
        if (auto sit = tt.find("soname"); sit != tt.end()) {
            if (!sit->second.is_string()) {
                return std::unexpected(error(origin,
                    std::format("targets.{}.soname must be a string", tname)));
            }
            t.soname = sit->second.as_string();
        }
        if (auto msg = validate_target_soname(t, std::format("targets.{}.", tname))) {
            return std::unexpected(error(origin, *msg));
        }

        // Per-target flags (entry-scoped) + required-features gate.
        auto read_list = [&](const char* key, std::vector<std::string>& out) {
            if (auto it = tt.find(key); it != tt.end() && it->second.is_array())
                for (auto& v : it->second.as_array())
                    if (v.is_string()) out.push_back(v.as_string());
        };
        read_list("cflags",            t.cflags);
        read_list("cxxflags",          t.cxxflags);
        read_list("defines",           t.defines);
        read_list("required_features", t.requiredFeatures);
        // Guard: -std=... belongs to [package].standard, not per-target flags
        // (same rule as [build].cxxflags). Reject early with a clear message.
        for (auto const& flag : t.cxxflags) {
            if (starts_with_std_flag(flag)) {
                return std::unexpected(error(origin, std::format(
                    "targets.{}.cxxflags contains '{}'; use [package].standard to "
                    "configure the C++ language standard", tname, flag)));
            }
        }

        // Surface unsupported keys instead of silently dropping them — the
        // historic footgun behind issue #131 (a `[targets.x] cxxflags` typo on
        // an older mcpp just vanished). Per-target arbitrary build config that
        // must reach SHARED code is intentionally not a target key; point users
        // at the right axis (workspace / features / profile).
        static constexpr std::string_view kKnownTargetKeys[] = {
            "kind", "main", "soname",
            "cflags", "cxxflags", "defines", "required_features",
        };
        for (auto& [key, _] : tt) {
            bool known = false;
            for (auto k : kKnownTargetKeys) if (key == k) { known = true; break; }
            if (!known) {
                m.schemaWarnings.push_back(std::format(
                    "[targets.{}] has unsupported key '{}' (ignored). Per-target keys: "
                    "kind, main, soname, cflags, cxxflags, defines, required_features. "
                    "For config that must affect shared code, split into a workspace "
                    "member or use [features]; for a whole-build mode use [profile.*].",
                    tname, key));
            }
        }
        m.targets.push_back(std::move(t));
    }
    } // close `if (targets_table && !targets_table->empty())`

    // [dependencies] / [dev-dependencies]
    //
    // Three accepted forms (M5.x):
    //
    //   (1) flat / default-ns
    //         [dependencies]
    //         gtest = "1.15.2"             ⇒ (mcpp, gtest)
    //         frob  = { path = "..." }     ⇒ (mcpp, frob) inline spec
    //
    //   (2) namespaced subtable (TOML-native, no quotes)
    //         [dependencies.mcpplibs]
    //         cmdline = "0.0.2"            ⇒ (mcpplibs, cmdline)
    //         tmpl    = { version = "0.0.1", features = [...] }
    //
    //   (3) legacy quoted dotted form (deprecated, still parsed)
    //         [dependencies]
    //         "mcpplibs.cmdline" = "0.0.2" ⇒ (mcpplibs, cmdline) + warning
    //
    // The map key remains the fully-qualified `<ns>.<name>` for non-default
    // namespaces (so existing fetcher / lockfile lookups by composite name
    // keep working) and the bare `<name>` for the default namespace (so the
    // common case stays unchanged).
    auto is_dep_spec_key = [](std::string_view k) {
        return k == "path"   || k == "version" || k == "git"
            || k == "rev"    || k == "tag"     || k == "branch"
            || k == "features" || k == "workspace" || k == "visibility"
            || k == "backend";
    };
    auto looks_like_inline_dep_spec = [&](const t::Table& sub) {
        if (sub.empty()) return false;
        for (auto& [sk, sv] : sub) {
            if (!is_dep_spec_key(sk)) return false;
        }
        return true;
    };

    auto fill_inline_spec = [&](DependencySpec& spec,
                                std::string_view section,
                                std::string_view fqName,
                                const t::Table& sub) -> std::expected<void, ManifestError>
    {
        if (auto it = sub.find("path");    it != sub.end() && it->second.is_string()) spec.path    = it->second.as_string();
        if (auto it = sub.find("version"); it != sub.end() && it->second.is_string()) spec.version = it->second.as_string();
        if (auto it = sub.find("git");     it != sub.end() && it->second.is_string()) spec.git     = it->second.as_string();
        if (auto it = sub.find("visibility"); it != sub.end() && it->second.is_string()) {
            spec.visibility = it->second.as_string();
            if (spec.visibility != "public"
                && spec.visibility != "private"
                && spec.visibility != "interface") {
                return std::unexpected(error(origin, std::format(
                    "[{}.\"{}\"] visibility must be 'public', 'private', or 'interface'",
                    section, fqName)));
            }
        }
        if (auto it = sub.find("features"); it != sub.end() && it->second.is_array()) {
            for (auto& fv : it->second.as_array())
                if (fv.is_string()) spec.features.push_back(fv.as_string());
        }
        // `backend = "<impl>"` — sugar for requesting the dependency's
        // `backend-<impl>` feature (library-level backend selection knob).
        if (auto it = sub.find("backend"); it != sub.end() && it->second.is_string()) {
            spec.features.push_back("backend-" + it->second.as_string());
        }
        if (auto it = sub.find("rev");     it != sub.end() && it->second.is_string()) {
            spec.gitRev     = it->second.as_string();
            spec.gitRefKind = "rev";
        } else if (auto it = sub.find("tag");    it != sub.end() && it->second.is_string()) {
            spec.gitRev     = it->second.as_string();
            spec.gitRefKind = "tag";
        } else if (auto it = sub.find("branch"); it != sub.end() && it->second.is_string()) {
            spec.gitRev     = it->second.as_string();
            spec.gitRefKind = "branch";
        }
        if (auto it = sub.find("workspace"); it != sub.end() && it->second.is_bool() && it->second.as_bool()) {
            spec.inheritWorkspace = true;
            return {};  // version will be filled in by workspace merge
        }
        if (spec.path.empty() && spec.version.empty() && spec.git.empty()) {
            return std::unexpected(error(origin, std::format(
                "[{}.\"{}\"] must specify 'path', 'version', or 'git'", section, fqName)));
        }
        if (!spec.git.empty() && spec.gitRev.empty()) {
            return std::unexpected(error(origin, std::format(
                "[{}.\"{}\"] git dep requires one of: rev / tag / branch", section, fqName)));
        }
        return {};
    };

    auto assign_dep = [&](std::string_view section,
                          std::map<std::string, DependencySpec>& out,
                          const mcpp::pm::DependencySelector& selector,
                          const t::Value& value,
                          bool legacyDottedKey)
        -> std::expected<void, ManifestError>
    {
        if (selector.candidates.empty()) {
            return std::unexpected(error(origin, std::format(
                "[{}] dependency selector '{}' has no candidates",
                section, selector.stableMapKey)));
        }

        DependencySpec spec;
        spec.namespace_ = selector.candidates.front().namespace_;
        spec.shortName = selector.candidates.front().shortName;
        spec.candidates = selector.candidates;
        spec.legacyDottedKey = legacyDottedKey;

        auto key = selector.stableMapKey;
        if (value.is_string()) {
            spec.version = value.as_string();
        } else if (value.is_table()) {
            auto& sub = value.as_table();
            if (!looks_like_inline_dep_spec(sub)) {
                return std::unexpected(error(origin, std::format(
                    "[{}.{}] must be a version string or table of "
                    "(path/version/git/rev/tag/branch/features/visibility)",
                    section, key)));
            }
            if (auto r = fill_inline_spec(spec, section, key, sub); !r) return r;
        } else {
            return std::unexpected(error(origin, std::format(
                "[{}].{} must be a string (version) or table (path/version/...)",
                section, key)));
        }

        out[key] = std::move(spec);
        return {};
    };

    auto is_namespace_table = [&](std::string_view section,
                                  std::string_view key) {
        auto path = std::format("{}.{}", section, key);
        return doc->has_explicit_table(path)
            || key == kDefaultNamespace;
    };

    std::function<std::expected<void, ManifestError>(
        std::string_view,
        std::map<std::string, DependencySpec>&,
        std::string,
        std::string,
        const t::Table&)> load_nested_dep_table;

    load_nested_dep_table =
        [&](std::string_view section,
            std::map<std::string, DependencySpec>& out,
            std::string ns,
            std::string mapPrefix,
            const t::Table& table) -> std::expected<void, ManifestError>
    {
        for (auto& [k, v] : table) {
            if (v.is_string() ||
                (v.is_table() && looks_like_inline_dep_spec(v.as_table()))) {
                auto mapKey = mapPrefix.empty()
                    ? k
                    : std::format("{}.{}", mapPrefix, k);
                auto selector = mcpp::pm::make_direct_dependency_selector(
                    ns, k, mapKey);
                if (auto r = assign_dep(section, out, selector, v, false); !r)
                    return r;
                continue;
            }
            if (!v.is_table()) {
                return std::unexpected(error(origin, std::format(
                    "[{}].{}.{} must be a string, inline dep table, or nested table",
                    section, ns, k)));
            }
            auto childNs = std::format("{}.{}", ns, k);
            auto childMapPrefix = mapPrefix.empty()
                ? k
                : std::format("{}.{}", mapPrefix, k);
            if (auto r = load_nested_dep_table(
                    section, out, childNs, childMapPrefix, v.as_table()); !r)
                return r;
        }
        return {};
    };

    std::function<std::expected<void, ManifestError>(
        std::string_view,
        std::map<std::string, DependencySpec>&,
        std::string,
        const t::Table&)> load_selector_dep_table;

    load_selector_dep_table =
        [&](std::string_view section,
            std::map<std::string, DependencySpec>& out,
            std::string selectorPrefix,
            const t::Table& table) -> std::expected<void, ManifestError>
    {
        for (auto& [k, v] : table) {
            auto selectorText = selectorPrefix.empty()
                ? k
                : std::format("{}.{}", selectorPrefix, k);
            if (v.is_string() ||
                (v.is_table() && looks_like_inline_dep_spec(v.as_table()))) {
                auto selector = mcpp::pm::resolve_dependency_selector(
                    selectorText,
                    mcpp::pm::DependencySelectorMode::OmittedMcpplibsPriority);
                if (auto r = assign_dep(section, out, selector, v, false); !r)
                    return r;
                continue;
            }
            if (!v.is_table()) {
                return std::unexpected(error(origin, std::format(
                    "[{}].{} must be a string, inline dep table, or nested table",
                    section, selectorText)));
            }
            if (auto r = load_selector_dep_table(
                    section, out, selectorText, v.as_table()); !r)
                return r;
        }
        return {};
    };

    auto load_deps = [&](std::string_view section, std::map<std::string, DependencySpec>& out)
        -> std::expected<void, ManifestError>
    {
        auto* tt = doc->get_table(section);
        if (!tt) return {};
        for (auto& [k, v] : *tt) {
            // (1) string value → flat default-ns short version, or
            // (3) legacy "ns.name" = "ver" (dotted key).
            if (v.is_string()) {
                if (k.find('.') != std::string::npos) {
                    auto legacyKey = mcpp::pm::compat::split_legacy_dependency_key(k);
                    auto selector = mcpp::pm::make_direct_dependency_selector(
                        legacyKey.namespace_, legacyKey.shortName, k);
                    if (auto r = assign_dep(section, out, selector, v,
                                            legacyKey.legacyDottedKey); !r)
                        return r;
                    continue;
                }
                auto selector = mcpp::pm::resolve_dependency_selector(
                    k, mcpp::pm::DependencySelectorMode::OmittedMcpplibsPriority);
                if (auto r = assign_dep(section, out, selector, v, false); !r)
                    return r;
                continue;
            }

            if (!v.is_table()) {
                return std::unexpected(error(origin, std::format(
                    "[{}].{} must be a string (version) or table (path/version/...)", section, k)));
            }

            auto& sub = v.as_table();

            // (1') inline dep spec under the default namespace, e.g.
            //         frob = { path = "..." }     or
            //         "mcpplibs.cmdline" = { version = "0.0.2" }
            // The latter is the legacy dotted-key form; same treatment as (3).
            if (looks_like_inline_dep_spec(sub)) {
                if (k.find('.') != std::string::npos) {
                    auto legacyKey = mcpp::pm::compat::split_legacy_dependency_key(k);
                    auto selector = mcpp::pm::make_direct_dependency_selector(
                        legacyKey.namespace_, legacyKey.shortName, k);
                    if (auto r = assign_dep(section, out, selector, v,
                                            legacyKey.legacyDottedKey); !r)
                        return r;
                    continue;
                }
                auto selector = mcpp::pm::resolve_dependency_selector(
                    k, mcpp::pm::DependencySelectorMode::OmittedMcpplibsPriority);
                if (auto r = assign_dep(section, out, selector, v, false); !r)
                    return r;
                continue;
            }

            // (2) namespaced or nested subtable.
            //
            // Explicit tables such as `[dependencies.acme]` are namespace
            // roots. Dotted keys written inside the single dependency table,
            // such as `[dependencies] capi.lua = "0.0.3"`, are ordered
            // selectors: mcpplibs.capi/lua first, then capi/lua.
            if (is_namespace_table(section, k)) {
                if (auto r = load_nested_dep_table(section, out, k, k, sub); !r)
                    return r;
            } else if (auto r = load_selector_dep_table(section, out, k, sub); !r) {
                return r;
            }
        }
        return {};
    };
    if (auto r = load_deps("dependencies",       m.dependencies);       !r) return std::unexpected(r.error());
    if (auto r = load_deps("dev-dependencies",   m.devDependencies);    !r) return std::unexpected(r.error());
    if (auto r = load_deps("build-dependencies", m.buildDependencies);  !r) return std::unexpected(r.error());

    // [toolchain] — platform → "pkg@version" map (docs/21)
    if (auto* tt = doc->get_table("toolchain")) {
        for (auto& [platform, val] : *tt) {
            if (!val.is_string()) {
                return std::unexpected(error(origin,
                    std::format("[toolchain].{} must be a string like \"gcc@15.1.0\"", platform)));
            }
            m.toolchain.byPlatform[platform] = val.as_string();
        }
    }

    // [build] — backend tunables
    if (auto v = doc->get_bool("build.static_stdlib")) m.buildConfig.staticStdlib = *v;
    if (auto v = doc->get_string_array("build.cflags"))   m.buildConfig.cflags   = *v;
    if (auto v = doc->get_string_array("build.cxxflags")) m.buildConfig.cxxflags = *v;
    if (auto v = doc->get_string_array("build.ldflags"))  m.buildConfig.ldflags  = *v;
    if (auto v = doc->get_string("build.c_standard"))     m.buildConfig.cStandard = *v;
    if (auto v = doc->get_string("build.macos_deployment_target"))
        m.buildConfig.macosDeploymentTarget = *v;
    for (auto const& flag : m.buildConfig.cxxflags) {
        if (starts_with_std_flag(flag)) {
            return std::unexpected(error(origin,
                std::format("build.cxxflags contains '{}'; use [package].standard to configure the C++ language standard",
                            flag)));
        }
    }

    // [runtime] — launch-time requirements.
    if (auto v = doc->get_string_array("runtime.library_dirs")) {
        for (auto& s : *v) m.runtimeConfig.libraryDirs.emplace_back(s);
    }
    if (auto v = doc->get_string_array("runtime.dlopen_libs"))
        m.runtimeConfig.dlopenLibs = *v;
    if (auto v = doc->get_string_array("runtime.capabilities"))
        m.runtimeConfig.capabilities = *v;
    if (auto v = doc->get_string_array("runtime.provides"))
        m.runtimeConfig.provides = *v;
    // [runtime.<capability>] provider = "<pkg>" — explicit provider override.
    if (auto* rt = doc->get_table("runtime"); rt && !rt->empty()) {
        for (auto& [rk, rv] : *rt) {
            if (!rv.is_table()) continue;  // flat keys handled above
            auto& tt = rv.as_table();
            if (auto it = tt.find("provider"); it != tt.end() && it->second.is_string())
                m.runtimeConfig.providerOverrides[rk] = it->second.as_string();
        }
    }

    // [lib] — library root convention (cargo-style).
    if (auto v = doc->get_string("lib.path")) {
        m.lib.path = *v;
    }

    // [pack] — `mcpp pack` configuration. See docs/35-pack-design.md.
    if (auto v = doc->get_string("pack.default_mode")) {
        const auto& s = *v;
        if (s != "static" && s != "bundle-project" && s != "bundle-all") {
            return std::unexpected(error(origin, std::format(
                "[pack].default_mode = '{}' invalid; expected "
                "'static' | 'bundle-project' | 'bundle-all'", s)));
        }
        m.packConfig.defaultMode = s;
    }
    if (auto v = doc->get_string_array("pack.include"))
        m.packConfig.include = *v;
    if (auto v = doc->get_string_array("pack.exclude"))
        m.packConfig.exclude = *v;
    // [pack.bundle-project] sub-table for fine-grained PEP 600 overrides.
    if (auto v = doc->get_string_array("pack.bundle-project.also_skip"))
        m.packConfig.alsoSkip = *v;
    if (auto v = doc->get_string_array("pack.bundle-project.force_bundle"))
        m.packConfig.forceBundle = *v;

    // [target.<triple>] — per-target overrides. We accept both GCC
    // (x86_64-linux-musl) and Rust-style (x86_64-unknown-linux-musl)
    // triple forms; the latter is canonicalised by stripping the
    // `-unknown-` segment so both keys map to the same entry.
    auto canon_triple = [](std::string s) {
        constexpr std::string_view kUnknown = "-unknown-";
        if (auto p = s.find(kUnknown); p != std::string::npos)
            s.replace(p, kUnknown.size(), "-");
        return s;
    };
    if (auto* tt = doc->get_table("target")) {
        for (auto& [triple, val] : *tt) {
            if (!val.is_table()) continue;
            auto& body = val.as_table();
            TargetEntry e;
            if (auto it = body.find("toolchain"); it != body.end() && it->second.is_string())
                e.toolchain = it->second.as_string();
            if (auto it = body.find("linkage"); it != body.end() && it->second.is_string()) {
                e.linkage = it->second.as_string();
                if (e.linkage != "static" && e.linkage != "dynamic") {
                    return std::unexpected(error(origin, std::format(
                        "[target.{}].linkage = '{}' is invalid; expected 'static' or 'dynamic'",
                        triple, e.linkage)));
                }
            }
            m.targetOverrides[canon_triple(triple)] = std::move(e);
        }
    }

    // [workspace] — multi-package workspace support (0.0.11+).
    if (doc->get_table("workspace")) {
        m.workspace.present = true;
        if (auto v = doc->get_string_array("workspace.members"))
            m.workspace.members = *v;
        if (auto v = doc->get_string_array("workspace.exclude"))
            m.workspace.exclude = *v;

        // [workspace.dependencies] — versions that members inherit via .workspace = true.
        if (auto* wdeps = doc->get_table("workspace.dependencies")) {
            for (auto& [k, v] : *wdeps) {
                if (v.is_string()) {
                    if (k.find('.') != std::string::npos) {
                        auto depKey = mcpp::pm::compat::split_legacy_dependency_key(k);
                        auto selector = mcpp::pm::make_direct_dependency_selector(
                            depKey.namespace_, depKey.shortName, k);
                        if (auto r = assign_dep("workspace.dependencies",
                                                m.workspace.dependencies,
                                                selector, v,
                                                depKey.legacyDottedKey); !r) {
                            return std::unexpected(r.error());
                        }
                        continue;
                    }
                    auto selector = mcpp::pm::resolve_dependency_selector(
                        k, mcpp::pm::DependencySelectorMode::OmittedMcpplibsPriority);
                    if (auto r = assign_dep("workspace.dependencies",
                                            m.workspace.dependencies,
                                            selector, v, false); !r) {
                        return std::unexpected(r.error());
                    }
                    continue;
                }
                if (!v.is_table()) continue;
                if (is_namespace_table("workspace.dependencies", k)) {
                    if (auto r = load_nested_dep_table("workspace.dependencies",
                                                       m.workspace.dependencies,
                                                       k, k, v.as_table()); !r) {
                        return std::unexpected(r.error());
                    }
                } else {
                    if (auto r = load_selector_dep_table("workspace.dependencies",
                                                         m.workspace.dependencies,
                                                         k, v.as_table()); !r) {
                        return std::unexpected(r.error());
                    }
                }
            }
        }
    }

    // [indices] — custom package index repositories.
    //
    // Accepted forms:
    //   acme = "git@gitlab.example.com:platform/mcpp-index.git"       # short: value = url
    //   acme-stable = { url = "git@...", tag = "v2.0" }               # long: inline table
    //   local-dev = { path = "<path>/my-packages" }                  # local path
    //   mcpplibs = { url = "https://...", rev = "abc123" }            # pin built-in
    if (auto* indices_t = doc->get_table("indices")) {
        for (auto& [k, v] : *indices_t) {
            mcpp::pm::IndexSpec spec;
            spec.name = k;

            if (v.is_string()) {
                // Short form: key = "url"
                spec.url = v.as_string();
            } else if (v.is_table()) {
                auto& sub = v.as_table();
                if (auto it = sub.find("url");    it != sub.end() && it->second.is_string()) spec.url    = it->second.as_string();
                if (auto it = sub.find("rev");    it != sub.end() && it->second.is_string()) spec.rev    = it->second.as_string();
                if (auto it = sub.find("tag");    it != sub.end() && it->second.is_string()) spec.tag    = it->second.as_string();
                if (auto it = sub.find("branch"); it != sub.end() && it->second.is_string()) spec.branch = it->second.as_string();
                if (auto it = sub.find("path");   it != sub.end() && it->second.is_string()) spec.path   = it->second.as_string();
                if (spec.url.empty() && spec.path.empty()) {
                    return std::unexpected(error(origin, std::format(
                        "[indices].{} must specify 'url' or 'path'", k)));
                }
            } else {
                return std::unexpected(error(origin, std::format(
                    "[indices].{} must be a string (url) or inline table", k)));
            }

            m.indices[k] = std::move(spec);
        }
    }

    return m;
}

// M5.0: inject defaults and auto-infer targets when fields are absent.
// Mutates manifest in-place; called from load() with the project root.
namespace {

void apply_defaults_and_infer(Manifest& m, const std::filesystem::path& root) {
    // Default sources glob (covers .cppm/.cpp/.cc/.c under src/).
    if (m.buildConfig.sources.empty()) {
        m.buildConfig.sources = {
            "src/**/*.cppm",
            "src/**/*.cpp",
            "src/**/*.cc",
            "src/**/*.c",
        };
        m.modules.sources = m.buildConfig.sources;   // legacy mirror
        m.inferredNotes.push_back("sources [src/**/*.{cppm,cpp,cc,c}]");
    }

    // Default include_dirs: ["include"] iff <root>/include/ exists.
    if (m.buildConfig.includeDirs.empty()) {
        std::error_code ec;
        if (std::filesystem::is_directory(root / "include", ec)) {
            m.buildConfig.includeDirs.push_back("include");
            m.inferredNotes.push_back("include_dirs [include]");
        }
    }

    // Auto-target inference (only when no [targets] declared).
    if (m.targets.empty()) {
        std::error_code ec;
        auto mainCpp = root / "src" / "main.cpp";
        bool hasMain   = std::filesystem::exists(mainCpp, ec);

        bool hasCppm = false;
        if (std::filesystem::is_directory(root / "src", ec)) {
            for (auto& e : std::filesystem::recursive_directory_iterator(root / "src", ec)) {
                if (ec) break;
                if (e.is_regular_file(ec) && !ec
                    && e.path().extension() == ".cppm") {
                    hasCppm = true; break;
                }
            }
        }

        if (hasMain) {
            Target t;
            t.name = m.package.name;
            t.kind = Target::Binary;
            t.main = "src/main.cpp";
            m.targets.push_back(std::move(t));
            m.inferredNotes.push_back(
                std::format("target {} (bin from src/main.cpp)", m.package.name));
        } else if (hasCppm) {
            Target t;
            t.name = m.package.name;
            t.kind = Target::Library;
            m.targets.push_back(std::move(t));
            m.inferredNotes.push_back(
                std::format("target {} (lib from .cppm in src/)", m.package.name));
        }
        // If neither, no auto-target — caller will error if it needs one.
    }
}

} // namespace

std::expected<Manifest, ManifestError> load(const std::filesystem::path& path) {
    std::ifstream is(path);
    if (!is) {
        return std::unexpected(ManifestError{
            std::format("cannot open '{}'", path.string()),
            path, 0, 0});
    }
    std::stringstream ss;
    ss << is.rdbuf();
    auto m = parse_string(ss.str(), path);
    if (!m) return m;

    // M5.0: defaults + target inference (uses filesystem context relative to mcpp.toml).
    apply_defaults_and_infer(*m, path.parent_path());
    return m;
}

// =====================================================================
//  synthesize_from_xpkg_lua — parse mcpp = {} segment from an xpkg .lua
// =====================================================================
//
//  Scope: tiny Lua-subset reader specialised for our `mcpp = { ... }`
//  workaround block. We don't run real Lua; we just locate the mcpp
//  table and read a short list of typed fields out of it.

namespace {

struct LuaCursor {
    std::string_view text;
    std::size_t      pos = 0;

    bool eof() const { return pos >= text.size(); }
    char peek() const { return pos < text.size() ? text[pos] : '\0'; }

    void skip_ws_and_comments() {
        while (!eof()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' || c == ';') {
                ++pos;
            } else if (c == '-' && pos + 1 < text.size() && text[pos + 1] == '-') {
                while (!eof() && peek() != '\n') ++pos;
            } else {
                break;
            }
        }
    }

    bool consume(char c) {
        skip_ws_and_comments();
        if (peek() == c) { ++pos; return true; }
        return false;
    }

    std::string read_string() {
        skip_ws_and_comments();
        if (peek() != '"' && peek() != '\'') return {};
        char q = text[pos++];
        std::string out;
        while (!eof() && peek() != q) {
            if (peek() == '\\' && pos + 1 < text.size()) {
                ++pos;
                char e = text[pos++];
                switch (e) {
                    case 'n':  out.push_back('\n'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'r':  out.push_back('\r'); break;
                    case '"':  out.push_back('"');  break;
                    case '\'': out.push_back('\''); break;
                    case '\\': out.push_back('\\'); break;
                    default:   out.push_back(e);
                }
            } else {
                out.push_back(text[pos++]);
            }
        }
        if (!eof()) ++pos;     // closing quote
        return out;
    }

    std::string read_ident() {
        skip_ws_and_comments();
        std::string out;
        while (!eof() && (std::isalnum(static_cast<unsigned char>(peek())) ||
                          peek() == '_'))
        {
            out.push_back(text[pos++]);
        }
        return out;
    }

    // Read either a bare ident or `["string"]`.
    std::string read_key() {
        skip_ws_and_comments();
        if (peek() == '[') {
            ++pos;
            auto s = read_string();
            skip_ws_and_comments();
            if (peek() == ']') ++pos;
            return s;
        }
        return read_ident();
    }

    // Read a Lua barewordy value: number/true/false/nil up to delimiter.
    std::string read_bareword() {
        skip_ws_and_comments();
        std::string out;
        while (!eof() && !std::isspace(static_cast<unsigned char>(peek())) &&
               peek() != ',' && peek() != '}' && peek() != ';')
        {
            out.push_back(text[pos++]);
        }
        return out;
    }

    // Skip an entire balanced { ... } block, string-aware.
    void skip_table() {
        if (!consume('{')) return;
        int depth = 1;
        while (!eof() && depth > 0) {
            char c = peek();
            if (c == '"' || c == '\'') {
                read_string();
                continue;
            } else if (c == '-' && pos + 1 < text.size() && text[pos+1] == '-') {
                while (!eof() && peek() != '\n') ++pos;
                continue;
            } else if (c == '{') { ++depth; ++pos; }
            else if (c == '}') { --depth; ++pos; }
            else { ++pos; }
        }
    }

    // Read and consume a balanced { ... } block, returning the inner text.
    std::string read_table_body() {
        if (!consume('{')) return {};
        auto start = pos;
        int depth = 1;
        while (!eof() && depth > 0) {
            char c = peek();
            if (c == '"' || c == '\'') {
                read_string();
                continue;
            }
            if (c == '-' && pos + 1 < text.size() && text[pos + 1] == '-') {
                while (!eof() && peek() != '\n') ++pos;
                continue;
            }
            if (c == '{') {
                ++depth;
                ++pos;
                continue;
            }
            if (c == '}') {
                --depth;
                if (depth == 0) {
                    auto end = pos;
                    ++pos;
                    return std::string(text.substr(start, end - start));
                }
                ++pos;
                continue;
            }
            ++pos;
        }
        return {};
    }
};

std::string top_level_table_body_for_key(std::string_view body, std::string_view wantedKey) {
    LuaCursor cur { body };
    cur.skip_ws_and_comments();
    while (!cur.eof()) {
        auto key = cur.read_key();
        if (key.empty()) {
            cur.skip_ws_and_comments();
            if (cur.eof()) break;
            ++cur.pos;
            continue;
        }
        cur.skip_ws_and_comments();
        if (!cur.consume('=')) {
            cur.skip_ws_and_comments();
            continue;
        }
        cur.skip_ws_and_comments();
        if (key == wantedKey) {
            return cur.read_table_body();
        }
        if (cur.peek() == '{') cur.skip_table();
        else if (cur.peek() == '"' || cur.peek() == '\'') (void)cur.read_string();
        else (void)cur.read_bareword();
        cur.skip_ws_and_comments();
    }
    return {};
}

std::string top_level_string_value_for_key(std::string_view body, std::string_view wantedKey) {
    LuaCursor cur { body };
    cur.skip_ws_and_comments();
    while (!cur.eof()) {
        auto key = cur.read_key();
        if (key.empty()) {
            cur.skip_ws_and_comments();
            if (cur.eof()) break;
            ++cur.pos;
            continue;
        }
        cur.skip_ws_and_comments();
        if (!cur.consume('=')) {
            cur.skip_ws_and_comments();
            continue;
        }
        cur.skip_ws_and_comments();
        if (key == wantedKey && (cur.peek() == '"' || cur.peek() == '\'')) {
            return cur.read_string();
        }
        if (cur.peek() == '{') cur.skip_table();
        else if (cur.peek() == '"' || cur.peek() == '\'') (void)cur.read_string();
        else (void)cur.read_bareword();
        cur.skip_ws_and_comments();
    }
    return {};
}

// Strip Lua line comments (`-- ...\n`) and string contents from text,
// replacing them with spaces of the same length so positions are
// preserved. This is a simple-but-correct way to make the scanner
// in extract_mcpp_segment_body() ignore comments and strings without
// re-implementing a full Lua tokenizer.
std::string strip_lua_comments_and_strings(std::string_view text) {
    std::string out(text.size(), ' ');
    std::size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        // Line comment
        if (c == '-' && i + 1 < text.size() && text[i+1] == '-') {
            while (i < text.size() && text[i] != '\n') {
                // keep newlines for line-number fidelity
                out[i] = (text[i] == '\n') ? '\n' : ' ';
                ++i;
            }
            continue;
        }
        // String literal
        if (c == '"' || c == '\'') {
            char q = c;
            out[i] = c;        // keep opening quote so structure-aware search still sees it
            ++i;
            while (i < text.size() && text[i] != q) {
                if (text[i] == '\\' && i + 1 < text.size()) {
                    out[i]   = ' ';
                    out[i+1] = ' ';
                    i += 2;
                    continue;
                }
                out[i] = (text[i] == '\n') ? '\n' : ' ';
                ++i;
            }
            if (i < text.size()) {
                out[i] = q;     // closing quote
                ++i;
            }
            continue;
        }
        out[i] = c;
        ++i;
    }
    return out;
}

// Locate the body of `mcpp = { ... }` and return the inner content (no
// surrounding braces). Returns empty string if not found.
// M6.x: locate the `mcpp = ...` field at top level of an xpkg.lua and
// classify it as either a table body or a string path. Operates on a
// comment-/string-stripped copy so literal "mcpp = ..." inside Lua
// comments doesn't false-match.
McppField extract_mcpp_field_impl(std::string_view raw_text) {
    auto sanitized = strip_lua_comments_and_strings(raw_text);
    std::string_view text { sanitized };

    std::size_t p = 0;
    while ((p = text.find("mcpp", p)) != std::string_view::npos) {
        bool word_start = (p == 0 || (!std::isalnum(static_cast<unsigned char>(text[p-1]))
                                       && text[p-1] != '_'));
        if (!word_start) { ++p; continue; }
        std::size_t q = p + 4;
        if (q < text.size() && (std::isalnum(static_cast<unsigned char>(text[q])) ||
                                text[q] == '_')) { ++p; continue; }
        while (q < text.size() && (text[q] == ' ' || text[q] == '\t')) ++q;
        if (q >= text.size() || text[q] != '=') { ++p; continue; }
        ++q;
        while (q < text.size() && (text[q] == ' ' || text[q] == '\t' ||
                                    text[q] == '\n' || text[q] == '\r')) ++q;
        if (q >= text.size()) { ++p; continue; }

        // Discriminate: { → table body, " → string path
        if (text[q] == '{') {
            ++q;
            std::size_t body_start = q;
            int depth = 1;
            while (q < text.size() && depth > 0) {
                char c = text[q];
                if (c == '{') ++depth;
                else if (c == '}') {
                    --depth;
                    if (depth == 0) {
                        return McppField{
                            McppField::TableBody,
                            std::string(raw_text.substr(body_start, q - body_start))};
                    }
                }
                ++q;
            }
            return {};
        }
        if (text[q] == '"') {
            // string literal — but the sanitizer blanks string contents, so
            // re-locate the same `"..."` in raw_text and take its body.
            // Find the opening `"` at offset q in raw_text (offsets align
            // because sanitizer keeps positions).
            std::size_t s = q;
            if (s >= raw_text.size() || raw_text[s] != '"') { ++p; continue; }
            ++s;
            std::string val;
            while (s < raw_text.size() && raw_text[s] != '"') {
                if (raw_text[s] == '\\' && s + 1 < raw_text.size()) {
                    char nc = raw_text[s + 1];
                    switch (nc) {
                        case 'n': val.push_back('\n'); break;
                        case 't': val.push_back('\t'); break;
                        case '"': val.push_back('"');  break;
                        case '\\': val.push_back('\\'); break;
                        default: val.push_back(nc);
                    }
                    s += 2;
                } else {
                    val.push_back(raw_text[s++]);
                }
            }
            return McppField{ McppField::StringPath, std::move(val) };
        }
        ++p;
    }
    return {};
}

// Backward-compat: old API; prefer extract_mcpp_field for new callers.
std::string extract_mcpp_segment_body(std::string_view raw_text) {
    auto f = extract_mcpp_field_impl(raw_text);
    return f.kind == McppField::TableBody ? std::move(f.value) : std::string{};
}

} // namespace

McppField extract_mcpp_field(std::string_view luaContent) {
    return extract_mcpp_field_impl(luaContent);
}

std::string extract_xpkg_namespace(std::string_view luaContent) {
    auto packageBody = top_level_table_body_for_key(luaContent, "package");
    if (packageBody.empty()) return {};
    return top_level_string_value_for_key(packageBody, "namespace");
}

std::string extract_xpkg_name(std::string_view luaContent) {
    auto packageBody = top_level_table_body_for_key(luaContent, "package");
    if (packageBody.empty()) return {};
    return top_level_string_value_for_key(packageBody, "name");
}

XpkgIdentity canonical_xpkg_identity(std::string_view declaredNs,
                                     std::string_view declaredName,
                                     std::string_view indexDefaultNs) {
    // Step 1 — owning-index namespace: a descriptor that declares no namespace
    // inherits the namespace of the index it lives in.
    std::string ns(declaredNs.empty() ? indexDefaultNs : declaredNs);
    std::string name(declaredName);

    // Step 2 — fully-qualified name. A declared name already prefixed with the
    // namespace IS the FQN; otherwise the namespace is prepended. With no
    // namespace at all, the declared name stands alone.
    std::string fqn;
    if (ns.empty()) {
        fqn = name;
    } else {
        std::string prefix = ns + ".";
        fqn = name.starts_with(prefix) ? name : ns + "." + name;
    }

    // Step 3 — split the FQN on its LAST dot: prefix → ns, final segment → name.
    auto pos = fqn.rfind('.');
    if (pos == std::string::npos) return XpkgIdentity{ /*ns=*/{}, /*name=*/fqn };
    return XpkgIdentity{ fqn.substr(0, pos), fqn.substr(pos + 1) };
}

XpkgIdentity canonical_xpkg_identity_from_lua(std::string_view luaContent,
                                              std::string_view indexDefaultNs) {
    auto name = extract_xpkg_name(luaContent);
    if (name.empty()) return {};
    return canonical_xpkg_identity(extract_xpkg_namespace(luaContent), name,
                                   indexDefaultNs);
}

bool xpkg_lua_identity_matches(std::string_view luaContent,
                               std::string_view ns,
                               std::string_view shortName,
                               bool allowLegacyBareDefault,
                               std::string_view indexDefaultNs) {
    auto luaName = extract_xpkg_name(luaContent);
    if (luaName.empty()) return true;   // no declared name → cannot verify, accept

    // Reduce the descriptor to its canonical (ns, name) tuple (design doc §4.2),
    // then match per the unified model.
    auto id = canonical_xpkg_identity(extract_xpkg_namespace(luaContent),
                                      luaName, indexDefaultNs);

    // The single atomic name must equal the requested short name in every mode.
    if (id.name != shortName) return false;

    // Discovery (empty request ns, e.g. `mcpp new --template X`): the caller
    // derives the namespace from the descriptor, so a name match is enough.
    if (ns.empty()) return true;

    // Unqualified / default-namespace request: resolve the name against the
    // default namespace search path — the default namespace itself, then the
    // `compat` wrapper namespace (`kCompatNamespace`, shared with the candidate
    // generator). A legacy no-namespace descriptor is admitted under the flag.
    if (ns == kDefaultNamespace) {
        return id.ns == kDefaultNamespace
            || id.ns == kCompatNamespace
            || (allowLegacyBareDefault && id.ns.empty());
    }

    // Qualified request (concrete namespace: compat, xim, a custom/nested ns):
    // exact namespace equality. Cross-namespace collisions are structurally
    // impossible — a foreign `(xim, zlib)` never equals `(compat, zlib)`.
    return id.ns == ns;
}

std::vector<std::string>
list_xpkg_versions(std::string_view luaContent, std::string_view platform) {
    // Locate `xpm = { ... <platform> = { ["X.Y.Z"] = {...}, ... } ... }`.
    // We work on a sanitized copy so quoted version keys remain locatable
    // by their offsets in the original text.
    auto sanitized = strip_lua_comments_and_strings(luaContent);
    std::string_view text { sanitized };
    std::vector<std::string> versions;

    auto find_word_at_lhs = [&](std::string_view name, std::size_t from)
        -> std::size_t
    {
        std::size_t p = from;
        while ((p = text.find(name, p)) != std::string_view::npos) {
            bool word_start = (p == 0 ||
                (!std::isalnum(static_cast<unsigned char>(text[p-1])) && text[p-1] != '_'));
            std::size_t after = p + name.size();
            bool word_end = (after >= text.size() ||
                (!std::isalnum(static_cast<unsigned char>(text[after])) && text[after] != '_'));
            if (!word_start || !word_end) { ++p; continue; }
            std::size_t q = after;
            while (q < text.size() && (text[q] == ' ' || text[q] == '\t' ||
                                       text[q] == '\n' || text[q] == '\r')) ++q;
            if (q < text.size() && text[q] == '=') return p;
            ++p;
        }
        return std::string_view::npos;
    };

    auto skip_to_open_brace = [&](std::size_t from) -> std::size_t {
        std::size_t q = from;
        while (q < text.size() && text[q] != '{') ++q;
        return q < text.size() ? q : std::string_view::npos;
    };

    // Match braces to find table extent.
    auto find_table_end = [&](std::size_t open) -> std::size_t {
        int depth = 1;
        std::size_t q = open + 1;
        while (q < text.size() && depth > 0) {
            char c = text[q];
            if (c == '{') ++depth;
            else if (c == '}') {
                --depth;
                if (depth == 0) return q;
            }
            ++q;
        }
        return std::string_view::npos;
    };

    auto xpm_pos = find_word_at_lhs("xpm", 0);
    if (xpm_pos == std::string_view::npos) return versions;
    auto xpm_open = skip_to_open_brace(xpm_pos);
    if (xpm_open == std::string_view::npos) return versions;
    auto xpm_end = find_table_end(xpm_open);
    if (xpm_end == std::string_view::npos) return versions;

    auto plat_pos = find_word_at_lhs(platform, xpm_open + 1);
    if (plat_pos == std::string_view::npos || plat_pos >= xpm_end) return versions;
    auto plat_open = skip_to_open_brace(plat_pos);
    if (plat_open == std::string_view::npos || plat_open >= xpm_end) return versions;
    auto plat_end = find_table_end(plat_open);
    if (plat_end == std::string_view::npos) return versions;

    // Inside platform table: scan for ["X.Y.Z"] = { ... }
    std::size_t q = plat_open + 1;
    while (q < plat_end) {
        if (text[q] == '[') {
            std::size_t r = q + 1;
            while (r < plat_end && (text[r] == ' ' || text[r] == '\t')) ++r;
            if (r < plat_end && text[r] == '"') {
                ++r;
                std::size_t key_start = r;
                while (r < plat_end && text[r] != '"' && text[r] != '\n') ++r;
                if (r < plat_end && text[r] == '"') {
                    versions.emplace_back(luaContent.substr(key_start, r - key_start));
                }
            }
        }
        ++q;
    }
    return versions;
}

std::expected<Manifest, ManifestError>
synthesize_from_xpkg_lua(std::string_view luaContent,
                         std::string_view packageName,
                         std::string_view packageVersion)
{
    auto body = extract_mcpp_segment_body(luaContent);
    if (body.empty()) {
        return std::unexpected(ManifestError{
            std::format(
                "package '{}' has no `mcpp = {{}}` segment in its index entry "
                "and the source has no mcpp.toml — cannot derive a manifest.",
                packageName),
            std::format("xpkg-lua of {}@{}", packageName, packageVersion),
            0, 0});
    }
    if (auto platformBody = top_level_table_body_for_key(body, mcpp::platform::xpkg_platform);
        !platformBody.empty()) {
        body += "\n";
        body += platformBody;
    }

    Manifest m;
    m.sourcePath  = std::format("xpkg-lua://{}@{}", packageName, packageVersion);
    m.package.name    = std::string(packageName);
    m.package.version = std::string(packageVersion);
    m.package.standard = "c++23";
    m.language.standard   = "c++23";
    m.language.modules    = true;
    m.language.importStd  = true;

    LuaCursor cur { body };
    cur.skip_ws_and_comments();

    while (!cur.eof()) {
        cur.skip_ws_and_comments();
        if (cur.eof()) break;
        auto key = cur.read_key();
        if (key.empty()) {
            cur.skip_ws_and_comments();
            if (cur.eof()) break;
            ++cur.pos;            // unknown char — advance and retry
            continue;
        }
        cur.skip_ws_and_comments();
        if (!cur.consume('=')) {
            return std::unexpected(ManifestError{
                std::format("malformed mcpp segment near key '{}'", key),
                m.sourcePath, 0, 0});
        }
        cur.skip_ws_and_comments();

        if      (key == "language") {
            auto v = cur.read_string();
            if (!v.empty()) {
                m.language.standard = v;
                m.package.standard = v;
            }
        }
        else if (key == "import_std") {
            auto v = cur.read_bareword();
            m.language.importStd = (v == "true");
        }
        else if (key == "modules") {
            // `{ "a", "b", ... }`
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    "expected '{' after `modules =`", m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            while (!cur.eof() && cur.peek() != '}') {
                auto s = cur.read_string();
                if (!s.empty()) m.modules.exports_.push_back(std::move(s));
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "sources") {
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    "expected '{' after `sources =`", m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            while (!cur.eof() && cur.peek() != '}') {
                auto s = cur.read_string();
                if (!s.empty()) {
                    m.modules.sources.push_back(s);
                    m.buildConfig.sources.push_back(std::move(s));   // M5.0 mirror
                }
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "include_dirs") {
            // M5.0: shipped headers exposed to dependents AND used by this
            // package's own compile (mcpp's symmetric include_dirs semantics).
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    "expected '{' after `include_dirs =`", m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            while (!cur.eof() && cur.peek() != '}') {
                auto s = cur.read_string();
                if (!s.empty()) m.buildConfig.includeDirs.emplace_back(s);
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "provides") {
            // Package-level capabilities (Feature System v2 S3): this package
            // satisfies the listed abstract capability names for any dependent
            // that `requires` them. `{ "blas", "lapack", ... }`.
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    "expected '{' after `provides =`", m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            while (!cur.eof() && cur.peek() != '}') {
                auto s = cur.read_string();
                if (!s.empty()) m.provides.push_back(std::move(s));
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "generated_files") {
            // `{ ["relative/path"] = "contents", ... }`
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    "expected '{' after `generated_files =`", m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            while (!cur.eof() && cur.peek() != '}') {
                auto path = cur.read_key();
                if (path.empty()) break;
                cur.skip_ws_and_comments();
                if (!cur.consume('=')) {
                    return std::unexpected(ManifestError{
                        "expected '=' in `generated_files` entry", m.sourcePath, 0, 0});
                }
                auto content = cur.read_string();
                m.buildConfig.generatedFiles.emplace(path, std::move(content));
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "targets") {
            // `{ ["name"] = { kind = "lib" }, ... }`
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    "expected '{' after `targets =`", m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            while (!cur.eof() && cur.peek() != '}') {
                auto tname = cur.read_key();
                if (tname.empty()) { cur.skip_ws_and_comments(); break; }
                cur.skip_ws_and_comments();
                if (!cur.consume('=')) break;
                cur.skip_ws_and_comments();
                if (!cur.consume('{')) break;

                Target t;
                t.name = tname;
                t.kind = Target::Library;     // default
                cur.skip_ws_and_comments();
                while (!cur.eof() && cur.peek() != '}') {
                    auto sub = cur.read_key();
                    cur.skip_ws_and_comments();
                    if (!cur.consume('=')) break;
                    cur.skip_ws_and_comments();
                    if (sub == "kind") {
                        auto k = cur.read_string();
                        if      (k == "lib" || k == "library")     t.kind = Target::Library;
                        else if (k == "bin" || k == "binary")      t.kind = Target::Binary;
                        else if (k == "shared" || k == "dylib"
                              || k == "so" || k == "shlib")        t.kind = Target::SharedLibrary;
                    } else if (sub == "main") {
                        t.main = cur.read_string();
                    } else if (sub == "soname") {
                        t.soname = cur.read_string();
                    } else {
                        // unknown subfield — skip its value
                        cur.skip_ws_and_comments();
                        if (cur.peek() == '{') cur.skip_table();
                        else (void)cur.read_bareword();
                    }
                    cur.skip_ws_and_comments();
                }
                cur.consume('}');
                if (auto msg = validate_target_soname(t, std::format("targets.{}.", tname))) {
                    return std::unexpected(ManifestError{*msg, m.sourcePath, 0, 0});
                }
                m.targets.push_back(std::move(t));
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "features") {
            // `{ ["main"] = { sources = { "*/gtest_main.cc" } }, ... }`
            // Registers the feature (so it's a known feature) and, when it
            // carries `sources`, records them as feature-gated source globs
            // (excluded by default; included only when the feature is active —
            // resolved in prepare_build). A feature with no `sources` is still
            // registered (empty implied set) so it can be requested/validated.
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    "expected '{' after `features =`", m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            while (!cur.eof() && cur.peek() != '}') {
                auto fname = cur.read_key();
                if (fname.empty()) { cur.skip_ws_and_comments(); break; }
                cur.skip_ws_and_comments();
                if (!cur.consume('=')) break;
                cur.skip_ws_and_comments();
                if (!cur.consume('{')) break;
                // register the feature (no implied features for now)
                m.featuresMap.try_emplace(fname, std::vector<std::string>{});
                cur.skip_ws_and_comments();
                while (!cur.eof() && cur.peek() != '}') {
                    auto sub = cur.read_key();
                    cur.skip_ws_and_comments();
                    if (!cur.consume('=')) break;
                    cur.skip_ws_and_comments();
                    // Feature subfields that carry a string array. `sources`
                    // gates source globs; `defines` carries package-owned macros
                    // (Stage 1); `requires`/`provides` declare capabilities
                    // (Stage 3). All share the `{ "...", ... }` shape.
                    std::vector<std::string>* arr =
                        sub == "sources"  ? &m.buildConfig.featureSources[fname]
                      : sub == "defines"  ? &m.buildConfig.featureDefines[fname]
                      : sub == "requires" ? &m.featureRequires[fname]
                      : sub == "provides" ? &m.featureProvides[fname]
                      : nullptr;
                    if (arr && cur.peek() == '{') {
                        cur.consume('{');
                        cur.skip_ws_and_comments();
                        while (!cur.eof() && cur.peek() != '}') {
                            auto s = cur.read_string();
                            if (!s.empty()) arr->push_back(std::move(s));
                            cur.skip_ws_and_comments();
                        }
                        cur.consume('}');
                    } else {
                        // unknown subfield — skip its value
                        if (cur.peek() == '{') cur.skip_table();
                        else (void)cur.read_bareword();
                    }
                    cur.skip_ws_and_comments();
                }
                cur.consume('}');
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "deps") {
            // `{ ["name"] = "version", ["ns.name"] = "version", ... }`
            // The mcpp segment uses the flat / dotted form only — namespaced
            // subtables would require a richer Lua parser than we have here,
            // and the same expressivity is reachable by writing
            //     ["mcpplibs.cmdline"] = "0.0.2"
            // which the consumer side accepts identically.
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    "expected '{' after `deps =`", m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            while (!cur.eof() && cur.peek() != '}') {
                auto dname = cur.read_key();
                if (dname.empty()) break;
                cur.skip_ws_and_comments();
                if (!cur.consume('=')) break;
                cur.skip_ws_and_comments();
                auto dver = cur.read_string();
                if (!dname.empty()) {
                    DependencySpec spec;
                    spec.version = dver;
                    auto selector = mcpp::pm::resolve_dependency_selector(
                        dname,
                        mcpp::pm::DependencySelectorMode::OmittedMcpplibsPriority);
                    if (!selector.candidates.empty()) {
                        spec.namespace_ = selector.candidates.front().namespace_;
                        spec.shortName = selector.candidates.front().shortName;
                        spec.candidates = std::move(selector.candidates);
                        m.dependencies[selector.stableMapKey] = std::move(spec);
                    }
                }
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "cflags" || key == "cxxflags" || key == "ldflags") {
            // `{ "-Dfoo", "-Wall", ... }` — appended to the per-rule baseline
            // by ninja_backend. cflags goes to the C rule (.c files), cxxflags
            // to C++ rule (.cpp/.cc/.cxx/.cppm), ldflags to link commands.
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    std::format("expected '{{' after `{} =`", key),
                    m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            auto& target = (key == "cflags")
                ? m.buildConfig.cflags
                : (key == "cxxflags" ? m.buildConfig.cxxflags : m.buildConfig.ldflags);
            while (!cur.eof() && cur.peek() != '}') {
                auto s = cur.read_string();
                if (key == "cxxflags" && starts_with_std_flag(s)) {
                    return std::unexpected(ManifestError{
                        std::format("cxxflags contains '{}'; use language/package standard to configure the C++ language standard",
                                    s),
                        m.sourcePath, 0, 0});
                }
                if (!s.empty()) target.push_back(std::move(s));
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "c_standard") {
            auto v = cur.read_string();
            if (!v.empty()) m.buildConfig.cStandard = v;
        }
        else if (key == "runtime") {
            auto runtimeBody = cur.read_table_body();
            LuaCursor rc { runtimeBody };
            rc.skip_ws_and_comments();
            while (!rc.eof()) {
                auto sub = rc.read_key();
                if (sub.empty()) {
                    rc.skip_ws_and_comments();
                    if (rc.eof()) break;
                    ++rc.pos;
                    continue;
                }
                rc.skip_ws_and_comments();
                if (!rc.consume('=')) {
                    return std::unexpected(ManifestError{
                        std::format("malformed runtime segment near key '{}'", sub),
                        m.sourcePath, 0, 0});
                }
                rc.skip_ws_and_comments();
                auto read_string_list = [&](std::vector<std::string>& out)
                    -> std::expected<void, ManifestError>
                {
                    if (!rc.consume('{')) {
                        return std::unexpected(ManifestError{
                            std::format("expected '{{' after `runtime.{} =`", sub),
                            m.sourcePath, 0, 0});
                    }
                    rc.skip_ws_and_comments();
                    while (!rc.eof() && rc.peek() != '}') {
                        auto s = rc.read_string();
                        if (!s.empty()) out.push_back(std::move(s));
                        rc.skip_ws_and_comments();
                    }
                    rc.consume('}');
                    return {};
                };
                if (sub == "library_dirs") {
                    std::vector<std::string> dirs;
                    if (auto r = read_string_list(dirs); !r) return std::unexpected(r.error());
                    for (auto& d : dirs) m.runtimeConfig.libraryDirs.emplace_back(std::move(d));
                } else if (sub == "dlopen_libs") {
                    if (auto r = read_string_list(m.runtimeConfig.dlopenLibs); !r)
                        return std::unexpected(r.error());
                } else if (sub == "capabilities") {
                    if (auto r = read_string_list(m.runtimeConfig.capabilities); !r)
                        return std::unexpected(r.error());
                } else if (sub == "provides") {
                    if (auto r = read_string_list(m.runtimeConfig.provides); !r)
                        return std::unexpected(r.error());
                } else {
                    rc.skip_ws_and_comments();
                    if      (rc.peek() == '"' || rc.peek() == '\'') (void)rc.read_string();
                    else if (rc.peek() == '{') rc.skip_table();
                    else                        (void)rc.read_bareword();
                }
                rc.skip_ws_and_comments();
            }
        }
        else {
            // Unknown key — skip the value (string / bareword / table).
            cur.skip_ws_and_comments();
            if      (cur.peek() == '"' || cur.peek() == '\'') (void)cur.read_string();
            else if (cur.peek() == '{') cur.skip_table();
            else                        (void)cur.read_bareword();
        }
    }

    // Validate minimum
    if (m.modules.sources.empty()) {
        return std::unexpected(ManifestError{
            "synthesised manifest missing sources (mcpp segment must declare `sources = { ... }`)",
            m.sourcePath, 0, 0});
    }
    if (m.targets.empty()) {
        // Default to a library target with the same name as the package.
        Target t;
        t.name = m.package.name;
        // For dotted names like mcpplibs.cmdline, take the last segment.
        auto dot = t.name.find_last_of('.');
        if (dot != std::string::npos) t.name = t.name.substr(dot + 1);
        t.kind = Target::Library;
        m.targets.push_back(std::move(t));
    }

    auto stdCfg = normalize_cpp_standard(m.package.standard);
    if (!stdCfg) {
        return std::unexpected(ManifestError{stdCfg.error(), m.sourcePath, 0, 0});
    }
    m.cppStandard = *stdCfg;
    m.package.standard = m.cppStandard.canonical;
    m.language.standard = m.cppStandard.canonical;

    return m;
}

std::string default_template(std::string_view packageName) {
    // M5.0: minimal mcpp.toml — convention over configuration.
    // sources / target / standard are all auto-inferred. Users add fields as
    // they grow out of the defaults.
    return std::format(R"([package]
name        = "{}"
version     = "0.1.0"
description = "A modular C++23 package"
license     = "Apache-2.0"
)", packageName);
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

} // namespace mcpp::manifest
