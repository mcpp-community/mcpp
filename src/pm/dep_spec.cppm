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
