// mcpp.pm.compat — backward-compatibility shims for evolving package
// conventions. Centralises all migration logic so it can be retired
// cleanly in a future major version.
//
// DEPRECATION SCHEDULE:
//   The shims in this module are slated for removal in mcpp 1.0.0.
//   Projects should migrate to the canonical forms before that point:
//
//   ┌─────────────────────────────────────┬──────────────────────────────┐
//   │ Deprecated (works until 1.0)        │ Canonical (use now)          │
//   ├─────────────────────────────────────┼──────────────────────────────┤
//   │ name = "mcpplibs.cmdline"           │ namespace = "mcpplibs"       │
//   │  (namespace embedded in name)       │ name      = "cmdline"        │
//   ├─────────────────────────────────────┼──────────────────────────────┤
//   │ [dependencies]                      │ [dependencies.mcpplibs]      │
//   │ "mcpplibs.cmdline" = "0.0.2"       │ cmdline = "0.0.2"            │
//   └─────────────────────────────────────┴──────────────────────────────┘
//
// See also: mcpp.pm.dep_spec (kDefaultNamespace definition).

export module mcpp.pm.compat;

import std;
import mcpp.pm.dep_spec;

export namespace mcpp::pm::compat {

// ─── Package name normalisation ──────────────────────────────────────
//
// Given a raw `package.name` and an optional `package.namespace` from
// an xpkg descriptor or mcpp.toml, produce the canonical (namespace,
// shortName) pair.
//
// Resolution order:
//   1. If `ns` is non-empty → use it directly; `name` is the short name.
//   2. If `name` contains a dot → split on the FIRST dot:
//      - prefix = namespace, suffix = short name.
//      (COMPAT: this is the legacy convention; warns on stderr.)
//   3. Otherwise → namespace = kDefaultNamespace ("mcpp"), name as-is.

struct ResolvedName {
    std::string namespace_;
    std::string shortName;
    bool        usedLegacySplit = false;   // true when rule (2) fired
};

inline ResolvedName resolve_package_name(std::string_view name,
                                         std::string_view ns)
{
    ResolvedName r;

    if (!ns.empty()) {
        // Rule 1: explicit namespace field — canonical path.
        r.namespace_ = std::string(ns);
        r.shortName  = std::string(name);
        return r;
    }

    auto dot = name.find('.');
    if (dot != std::string_view::npos) {
        // Rule 2: legacy dotted name — split on first dot.
        r.namespace_ = std::string(name.substr(0, dot));
        r.shortName  = std::string(name.substr(dot + 1));
        r.usedLegacySplit = true;
        return r;
    }

    // Rule 3: bare name → default namespace.
    r.namespace_ = std::string(mcpp::pm::kDefaultNamespace);
    r.shortName  = std::string(name);
    return r;
}

// Reconstruct the fully-qualified name from (namespace, shortName).
// Default-namespace packages use the bare short name; others use
// "ns.short".
inline std::string qualified_name(std::string_view ns,
                                   std::string_view shortName)
{
    if (ns.empty() || ns == mcpp::pm::kDefaultNamespace)
        return std::string(shortName);
    return std::format("{}.{}", ns, shortName);
}

// ─── Index directory naming ──────────────────────────────────────────
//
// Maps (indexName, namespace, shortName) → the xpkgs subdirectory name
// that xlings places the extracted tarball under.
//
// Current layout (compat):
//     <xpkgs>/<index>-x-<ns>.<short>/<version>/
//     e.g.  mcpp-index-x-mcpplibs.cmdline/0.0.2/
//
// The function encapsulates this so a future layout change (e.g.
//     <xpkgs>/<index>-x-<short>/<version>/   with ns in metadata)
// only touches one place.

inline std::string xpkg_dir_name(std::string_view indexName,
                                  std::string_view ns,
                                  std::string_view shortName)
{
    auto qname = qualified_name(ns, shortName);
    return std::format("{}-x-{}", indexName, qname);
}

} // namespace mcpp::pm::compat
