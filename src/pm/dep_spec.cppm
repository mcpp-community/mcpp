// mcpp.pm.dep_spec — package-management subsystem: dependency data model.
//
// Owns `DependencySpec` and the default-namespace constant. Pure value
// types — no IO, no parsing here. Parsing currently lives in
// `mcpp.manifest`; a follow-up PR-R5 will move the `[dependencies]` /
// `[dev-dependencies]` parsing here, alongside the namespaced subtable
// + flat + legacy-dotted forms already established in PR-A.
//
// See `.agents/docs/2026-05-08-pm-subsystem-architecture.md` for the
// full pm/ subsystem layout.

export module mcpp.pm.dep_spec;

import std;

export namespace mcpp::pm {

struct DependencyCoordinate {
    std::string namespace_;
    std::string shortName;

    auto operator<=>(const DependencyCoordinate&) const = default;
};

// One declared dependency. Path-based deps refer to a sibling mcpp package
// on disk; version-based deps come from a registry; git-based deps clone
// a remote at a fixed ref.
struct DependencySpec {
    // xpkg-style namespace. Defaults to `kDefaultNamespace` ("mcpp") for
    // the root index. Carried alongside the existing fully-qualified name
    // (which the dependencies map keys on) so callers that want the
    // structured form — registry lookup, lockfile entries, error
    // messages — can pull it out without re-splitting strings.
    std::string                 namespace_;     // "mcpp" / "mcpplibs" / ...
    std::string                 shortName;      // package name without namespace prefix
    std::string                 version;        // "0.0.1" / "^1.2" / "" (req string)
    std::string                 path;           // filesystem path, or empty
    std::string                 git;            // "https://..." or empty
    std::string                 gitRev;         // commit / tag / branch (any one)
    std::string                 gitRefKind;     // "rev" / "tag" / "branch" (for clarity)
    std::string                 visibility = "public"; // public / private / interface
    std::vector<std::string>    features;       // requested feature set (long-form dep spec)
    // #355: HOST tools this consumer wants from the dependency — the names of
    // its `kind = "bin"` targets. Requesting one makes mcpp build that target
    // for the BUILD MACHINE (never the --target triple) and hand its path to
    // build.mcpp as MCPP_DEP_<PKG>_BIN_<TOOL>.
    //
    // Declared here, on the dependency edge, rather than in build.mcpp: asking
    // for an extra artifact from the graph is a graph-level request, and the
    // graph stays statically analysable (lockfile / LSP / audit). It is also
    // where the industry converged — vcpkg's `"host": true`, Conan's
    // `tool_requires`, xmake's `add_deps(..., {host = true})`, Cargo's
    // `[build-dependencies]` — all put it on the consumer's edge.
    //
    // Empty by default: the cost (e.g. protobuf's libprotoc is ~157 extra TUs)
    // is paid by the consumer, so nothing is built unless someone asks.
    std::vector<std::string>    tools;
    // #355 step 5: compile this dependency's lib-root module interface FOR THE
    // HOST and make it importable from the consumer's build.mcpp — the
    // mechanism behind reusable build rules distributed as ordinary packages
    // (`import mcpp.rules.protobuf;`), instead of a second, non-C++ rule DSL.
    //
    // Compiled ALONGSIDE build.mcpp with the SAME flags, not by a separate
    // sub-build. That is not an optimisation: a BMI is only usable by a
    // compile that agrees with it on standard, dialect flags and compiler
    // identity, and two independently-resolved builds have no reason to. The
    // shared-compile construction makes that agreement structural rather than
    // something to verify — the same class of failure as `module X CRC
    // mismatch`, which this project has paid for before.
    bool                        hostModule = false;
    bool                        defaultFeatures = true; // consumer opt-out: `default-features = false`
                                                        // suppresses the dep's own [features].default seed
                                                        // (Cargo parity). Explicit `features = [...]` still apply.
    std::vector<DependencyCoordinate> candidates; // ordered lookup candidates

    bool                        inheritWorkspace = false;  // .workspace = true
    bool                        legacyDottedKey = false;   // parsed from legacy "ns.name" flat key

    bool isPath()    const { return !path.empty(); }
    bool isGit()     const { return !git.empty(); }
    bool isVersion() const { return !isPath() && !isGit() && !version.empty(); }
};

// Default namespace for packages declared without an explicit one — the
// mcpplibs "root". Bare `gtest = "1.15.2"` becomes `(mcpp, gtest)`.
inline constexpr std::string_view kDefaultNamespace = "mcpplibs";

// The `compat` namespace holds upstream-library wrappers (compat.zlib,
// compat.gtest, …). It is the one non-default namespace that an unqualified
// (default-namespace) dependency name reaches: bare `gtest` → `compat.gtest`.
// Centralized here so the candidate generator (xpkg_lua_candidates) and the
// identity gate (xpkg_lua_identity_matches) share one source of truth instead
// of each hard-coding the literal "compat". See the design doc's §4.1 for the
// fuller "unqualified namespace search path" direction this is a seed of.
inline constexpr std::string_view kCompatNamespace = "compat";

} // namespace mcpp::pm
