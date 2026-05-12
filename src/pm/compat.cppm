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

// ─── xpkg .lua filename candidates (namespace-aware) ────────────────
//
// Given a structured (namespace, shortName), return the list of candidate
// filenames to search for in the index's pkgs/<letter>/ directories.
//
// The FIRST candidate is the canonical form; subsequent candidates are
// backward-compatibility fallbacks.
//
// DEPRECATION SCHEDULE (fallback candidates):
//   Fallback candidates are slated for removal in mcpp 1.0.0.
//   Package indices should use the canonical filename form by then.

inline std::vector<std::string> xpkg_lua_candidates(std::string_view ns,
                                                     std::string_view shortName)
{
    std::vector<std::string> candidates;
    auto qname = qualified_name(ns, shortName);

    auto fqname = ns.empty() ? std::string(shortName)
                             : std::format("{}.{}", ns, shortName);

    // Canonical: for default namespace → "<shortName>.lua" (e.g. "cmdline.lua")
    //            for non-default     → "<ns>.<shortName>.lua" (e.g. "compat.mbedtls.lua")
    if (ns.empty() || ns == mcpp::pm::kDefaultNamespace) {
        candidates.push_back(std::string(shortName) + ".lua");
    } else {
        candidates.push_back(fqname + ".lua");
    }

    // ── Fallback candidates (COMPAT, remove in 1.0.0) ──────────────

    // Fallback: full qualified name when it differs from qname
    // (e.g. default-ns "mcpplibs": qname="cmdline", fqname="mcpplibs.cmdline")
    auto qnameFile = qname + ".lua";
    auto fqnameFile = fqname + ".lua";
    if (fqnameFile != qnameFile &&
        fqnameFile != candidates.front()) {
        candidates.push_back(fqnameFile);
    }

    // Fallback: bare short name — covers mcpplibs packages whose
    // index files are named "<shortName>.lua" without namespace prefix.
    if (!ns.empty() && ns != mcpp::pm::kDefaultNamespace) {
        candidates.push_back(std::string(shortName) + ".lua");
    }

    // Fallback: compat.<shortName>.lua — covers compat packages
    // when the caller didn't specify the "compat" namespace.
    if (ns.empty() || ns == mcpp::pm::kDefaultNamespace) {
        candidates.push_back("compat." + std::string(shortName) + ".lua");
    }

    // Fallback: compat variants for non-default/non-compat namespaces.
    if (!ns.empty() && ns != mcpp::pm::kDefaultNamespace && ns != "compat") {
        candidates.push_back("compat." + fqname + ".lua");
        candidates.push_back("compat." + std::string(shortName) + ".lua");
    }

    return candidates;
}

// ─── install_path directory candidates (namespace-aware) ─────────────
//
// Given a structured (namespace, shortName) and index name, return the
// list of candidate directory names under <xpkgs>/ to search for.
//
// DEPRECATION SCHEDULE (fallback candidates):
//   Fallback candidates are slated for removal in mcpp 1.0.0.
//   By then xlings should use a consistent directory layout.

inline std::vector<std::string> install_dir_candidates(std::string_view ns,
                                                        std::string_view shortName,
                                                        std::string_view indexName)
{
    std::vector<std::string> candidates;
    auto qname = qualified_name(ns, shortName);
    auto fqname = ns.empty() ? std::string(shortName)
                             : std::format("{}.{}", ns, shortName);

    // Canonical: <index>-x-<ns>.<shortName>  (e.g. "mcpp-index-x-compat.mbedtls")
    // For default namespace: <index>-x-<shortName>  (e.g. "mcpp-index-x-cmdline")
    candidates.push_back(std::format("{}-x-{}", indexName, qname));

    // Fallback: xlings always installs with the full qualified name
    // (ns.shortName), even for the default namespace. So we also try
    // <index>-x-<ns>.<shortName> when qname != fqname.
    if (qname != fqname) {
        candidates.push_back(std::format("{}-x-{}", indexName, fqname));
    }

    // ── Fallback candidates (COMPAT, remove in 1.0.0) ──────────────

    // Namespace-prefixed dir (xlings uses namespace as prefix, not index name)
    // e.g. "compat-x-mbedtls", "mcpplibs-x-mcpplibs.tinyhttps"
    if (!ns.empty()) {
        candidates.push_back(std::format("{}-x-{}", ns, fqname));
        if (std::string(shortName) != fqname) {
            candidates.push_back(std::format("{}-x-{}", ns, shortName));
        }
    }

    // Index-prefixed with bare short name
    // e.g. "mcpp-index-x-mbedtls" (old pre-namespace layout)
    if (!ns.empty() && ns != mcpp::pm::kDefaultNamespace) {
        candidates.push_back(std::format("{}-x-{}", indexName, shortName));
    }

    return candidates;
}

} // namespace mcpp::pm::compat
