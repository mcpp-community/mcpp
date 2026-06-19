// mcpp.pm.compat - compatibility facade for evolving package conventions.
// Legacy-only behavior lives under src/pm/compat/legacy.cppm so it has an
// explicit retirement boundary.
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

export import mcpp.pm.compat.legacy;

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

// ─── Descriptor-derived coordinates ──────────────────────────────────
//
// Derive the structured (namespace, shortName) for a package whose index
// descriptor was located by filename candidates from an unqualified spec
// (`mcpp new --template <pkg>`). The filename hit alone is not enough:
// pkgs/l/llmapi.lua is found by the bare name "llmapi" while the
// descriptor declares `namespace = "mcpplibs"` (and possibly a legacy
// dotted `name = "mcpplibs.llmapi"`), and xlings resolves install targets
// by the descriptor's qualified name.
//
// Unlike resolve_package_name(), a bare name with no namespace field
// stays in the index root (namespace "") — root packages such as "imgui"
// install by their bare name.

inline ResolvedName descriptor_coordinates(std::string_view specPkg,
                                           std::string_view luaNs,
                                           std::string_view luaName)
{
    ResolvedName r;
    std::string name(luaName.empty() ? specPkg : luaName);
    r.namespace_ = std::string(luaNs);

    if (!r.namespace_.empty()) {
        // Legacy descriptors embed the namespace in the name too
        // (namespace = "mcpplibs", name = "mcpplibs.llmapi").
        auto prefix = r.namespace_ + ".";
        if (name.starts_with(prefix)) {
            name = name.substr(prefix.size());
            r.usedLegacySplit = true;
        }
    } else if (auto dot = name.find('.'); dot != std::string::npos) {
        // Legacy dotted name without a namespace field.
        r.namespace_ = name.substr(0, dot);
        name         = name.substr(dot + 1);
        r.usedLegacySplit = true;
    }

    r.shortName = std::move(name);
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
// xlings layout: <ns>-x-<ns>.<short>/<version>/
//   e.g. mcpplibs-x-mcpplibs.cmdline/0.0.2/
//        compat-x-compat.mbedtls/3.6.1/

inline std::string xpkg_dir_name(std::string_view indexName,
                                  std::string_view ns,
                                  std::string_view shortName)
{
    auto fqname = ns.empty() ? std::string(shortName)
                             : std::format("{}.{}", ns, shortName);
    if (!ns.empty()) return std::format("{}-x-{}", ns, fqname);
    return std::format("{}-x-{}", indexName, fqname);
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
    // when the caller didn't specify the compat namespace.
    auto compatPrefix = std::string(mcpp::pm::kCompatNamespace) + ".";
    if (ns.empty() || ns == mcpp::pm::kDefaultNamespace) {
        candidates.push_back(compatPrefix + std::string(shortName) + ".lua");
    }

    // Fallback: compat variants for non-default/non-compat namespaces.
    if (!ns.empty() && ns != mcpp::pm::kDefaultNamespace
        && ns != mcpp::pm::kCompatNamespace) {
        candidates.push_back(compatPrefix + fqname + ".lua");
        candidates.push_back(compatPrefix + std::string(shortName) + ".lua");
    }

    return candidates;
}

// ─── install_path directory candidates (namespace-aware) ─────────────
//
// Given a structured (namespace, shortName) and index name, return the
// list of candidate directory names under <xpkgs>/ to search for.
//
// xlings directory layout: <ns>-x-<ns>.<shortName>/<version>/
//   e.g. mcpplibs-x-mcpplibs.tinyhttps/0.2.2/
//        compat-x-compat.mbedtls/3.6.1/

inline std::vector<std::string> install_dir_candidates(std::string_view ns,
                                                        std::string_view shortName,
                                                        std::string_view indexName)
{
    std::vector<std::string> candidates;
    auto fqname = ns.empty() ? std::string(shortName)
                             : std::format("{}.{}", ns, shortName);

    // Canonical: <ns>-x-<ns>.<shortName> (xlings current layout)
    if (!ns.empty()) {
        candidates.push_back(std::format("{}-x-{}", ns, fqname));
    }

    // ── Fallback candidates (COMPAT, remove in 1.0.0) ──────────────

    // Old index-prefixed layout: <index>-x-<ns>.<shortName>
    candidates.push_back(std::format("{}-x-{}", indexName, fqname));

    // Fallback: compat.<shortName> packages serving default-namespace deps
    // such as bare `gtest = "1.15.2"`.
    if (ns.empty() || ns == mcpp::pm::kDefaultNamespace) {
        candidates.push_back(std::format("compat-x-compat.{}", shortName));
    }

    // Bare short name variants
    if (!ns.empty()) {
        candidates.push_back(std::format("{}-x-{}", ns, shortName));
        candidates.push_back(std::format("{}-x-{}", indexName, shortName));
    }

    // Legacy dotted dependency keys such as "mcpplibs.capi.lua" are parsed as
    // ns="mcpplibs", shortName="capi.lua". Some xlings indices store these as
    // nested namespaces: mcpplibs.capi-x-mcpplibs.capi.lua.
    if (!ns.empty()) {
        auto dot = shortName.rfind('.');
        if (dot != std::string_view::npos) {
            auto nestedNs = std::format("{}.{}", ns, shortName.substr(0, dot));
            candidates.push_back(std::format("{}-x-{}", nestedNs, fqname));
        }
    }

    return candidates;
}

} // namespace mcpp::pm::compat
