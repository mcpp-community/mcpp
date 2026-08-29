// mcpp.pm.compat.legacy - legacy dependency-key compatibility.
//
// COMPAT: This module exists only to keep pre-namespace dependency syntax
// working during migration. It is slated for removal in mcpp 1.0.0.
// TODO(mcpp 1.0.0): remove this module and the legacy dotted-key parser.
//
// Deprecated:
//
//     [dependencies]
//     "mcpplibs.cmdline" = "0.0.2"
//
// Canonical:
//
//     [dependencies.mcpplibs]
//     cmdline = "0.0.2"

export module mcpp.pm.compat.legacy;

import std;
import mcpp.pm.dep_spec;

export namespace mcpp::pm::compat {

struct LegacyDependencyKey {
    std::string namespace_;
    std::string shortName;
    bool        legacyDottedKey = false;
};

inline LegacyDependencyKey split_legacy_dependency_key(std::string_view key)
{
    auto dot = key.find('.');
    if (dot == std::string_view::npos) {
        return LegacyDependencyKey {
            .namespace_ = std::string(mcpp::pm::kDefaultNamespace),
            .shortName = std::string(key),
            .legacyDottedKey = false,
        };
    }

    return LegacyDependencyKey {
        .namespace_ = std::string(key.substr(0, dot)),
        .shortName = std::string(key.substr(dot + 1)),
        .legacyDottedKey = true,
    };
}

// Normalize legacy nested names after the first-dot split:
//   ns="mcpplibs", shortName="capi.lua" -> ns="mcpplibs.capi", shortName="lua".
//
// This preserves the fully qualified name while making dependency de-dup use
// the same structured key as canonical [dependencies.mcpplibs] capi.lua.
inline void normalize_nested_namespace(std::string& ns,
                                       std::string& shortName,
                                       bool legacyDottedKey)
{
    if (!legacyDottedKey) return;
    if (ns.empty()) return;
    auto dot = shortName.rfind('.');
    if (dot == std::string::npos || dot + 1 >= shortName.size()) return;

    ns += ".";
    ns += shortName.substr(0, dot);
    shortName = shortName.substr(dot + 1);
}

} // namespace mcpp::pm::compat
