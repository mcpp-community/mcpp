// mcpp.pm.compat.registry - every deprecated spelling or reading a user can
// write, in one table.
//
// docs/51 states that `mcpp.toml` keys are additive: an existing key keeps its
// meaning. A reading that is to be removed is an exception to that promise,
// and the exceptions are exactly the rows of `kDeprecations`. Each row states
// the release that deprecated the reading (`since`) and the CalVer month from
// which it is removed (`floor`). Everything a user sees about a deprecation is
// derived from the row:
//
//   the warning        deprecation_clause(): "deprecated since mcpp <since>
//                      and removed from mcpp <floor>"
//   `mcpp why compat`  record_use() collects each use with its location
//   docs/51            .github/tools/check_compat_registry.py checks that the
//                      table in docs/51 (en and zh) lists every row
//   the code           every `COMPAT(<id>)` marker names a row, and every row
//                      has a marker (the same tool)
//   the removal        tests/unit/test_pm_compat.cpp fails once MCPP_VERSION
//                      reaches a row's floor, naming the id
//
// A COMPAT marker of the form `COMPAT(internal:<topic>)` names engine-internal
// compatibility that no manifest can spell (legacy index file names, older
// call signatures). It is not a promise to anyone and has no row.

export module mcpp.pm.compat.registry;

import std;

export namespace mcpp::pm::compat {

struct Deprecation {
    std::string_view id;         // stable, kebab-case; the marker's argument
    std::string_view since;      // the first release that warns
    std::string_view floor;      // CalVer `YYYY.M`: absent from every release >= floor
    std::string_view what;       // the reading, as a noun phrase
    std::string_view canonical;  // what to write instead
    std::string_view domain;     // the diagnostic domain of the warning
    std::string_view docs;       // where the reading and its replacement are described
};

// The month from which the readings deprecated in 2026.10 are removed. A row
// may name a later floor; docs/51 requires at least two whole calendar months
// between `since` and `floor`.
inline constexpr std::string_view kRemovalFloor = "2027.1";

inline constexpr Deprecation kDeprecations[] = {
    {
        "workspace-position", "2026.10.10.1", kRemovalFloor,
        "a workspace root's own [toolchain], [indices], [profile.<name>], "
        "[target.<triple>] scalars or [xlings] reaching members by position",
        "[workspace.toolchain], [workspace.indices], [workspace.profile.<name>], "
        "[workspace.target.<selector>], [workspace.xlings]",
        "manifest/workspace-position", "docs/07-workspace.md",
    },
    {
        "host-module-in-dependencies", "2026.10.11.1", kRemovalFloor,
        "a [dependencies] entry that asks for `host-module = true` or for a "
        "feature declaring `rule_module`, read as a build dependency",
        "the same entry in [build-dependencies]",
        "manifest/deprecated", "docs/05-dependencies.md",
    },
    {
        "plugin-imports-from-build-dependencies", "2026.10.11.1", kRemovalFloor,
        "a build plugin's library importing a package it lists in its own "
        "[build-dependencies] with `host-module = true`",
        "the package in the plugin's [dependencies]",
        "manifest/deprecated", "docs/31-authoring-a-rule-package.md",
    },
};

// The row named `id`, or null.
inline const Deprecation* find_deprecation(std::string_view id) {
    for (auto const& d : kDeprecations)
        if (d.id == id) return &d;
    return nullptr;
}

// "deprecated since mcpp 2026.10.10.1 and removed from mcpp 2027.1"
inline std::string deprecation_clause(const Deprecation& d) {
    return std::format("deprecated since mcpp {} and removed from mcpp {}", d.since, d.floor);
}

// The `(year, month)` of a version or of a floor: the first two dot-separated
// numbers, a four-digit year and a month 1-12. Both are CalVer, so their order
// is the order of these pairs; anything else (a SemVer string) has none.
inline std::optional<std::pair<int, int>> calver_month(std::string_view v) {
    int parts[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        const auto dot = v.find('.');
        const auto head = v.substr(0, dot);
        if (head.empty()) return std::nullopt;
        auto [p, ec] = std::from_chars(head.data(), head.data() + head.size(), parts[i]);
        if (ec != std::errc{} || p != head.data() + head.size()) return std::nullopt;
        if (i == 0 && dot == std::string_view::npos) return std::nullopt;
        v = dot == std::string_view::npos ? std::string_view{} : v.substr(dot + 1);
    }
    if (parts[0] < 1000 || parts[0] > 9999 || parts[1] < 1 || parts[1] > 12) return std::nullopt;
    return std::pair{parts[0], parts[1]};
}

// Whether a release of `version` is at or past the floor of `d`.
inline bool past_floor(const Deprecation& d, std::string_view version) {
    auto v = calver_month(version), f = calver_month(d.floor);
    return v && f && *v >= *f;
}

// ── Uses of a deprecated reading in this process (`mcpp why compat`) ──────

struct DeprecationUse {
    std::string id;
    std::string where;    // the manifest that wrote it
    std::string detail;   // the table, key or entry, as the warning names it
    bool        warned = false;   // false: in a manifest the user cannot edit
};

// Recorded by every reader that applies a deprecated reading, warned or not.
void record_use(std::string_view id, std::string where, std::string detail, bool warned);
// Every use recorded so far, de-duplicated, in the order first seen.
std::vector<DeprecationUse> recorded_uses();

}  // namespace mcpp::pm::compat

namespace mcpp::pm::compat {

namespace {
std::mutex& uses_mutex() { static std::mutex m; return m; }
std::vector<DeprecationUse>& uses_store() { static std::vector<DeprecationUse> v; return v; }
}  // namespace

void record_use(std::string_view id, std::string where, std::string detail, bool warned) {
    std::lock_guard lock(uses_mutex());
    auto& v = uses_store();
    for (auto& u : v)
        if (u.id == id && u.where == where && u.detail == detail) {
            u.warned = u.warned || warned;
            return;
        }
    v.push_back({std::string(id), std::move(where), std::move(detail), warned});
}

std::vector<DeprecationUse> recorded_uses() {
    std::lock_guard lock(uses_mutex());
    return uses_store();
}

}  // namespace mcpp::pm::compat
