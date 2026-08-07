// mcpp.pm.resolver — turn a SemVer constraint into a concrete version,
// using the package's xpkg lua descriptor as the version inventory.
//
// WHAT THIS RETURNS IS AN INDEX KEY, NOT A RENDERING (mcpp#363)
//
// The resolved string flows into the xlings wire address, the store directory
// and mcpp.lock, so it must be a key the index literally holds. This function
// used to return `parsed[i].str()` — the parsed numbers re-rendered — which
// cannot reproduce `1.92.8-docking` (prerelease), `b10069` (not a number) or
// `25.0.4.7.1` (five segments, truncated to four). The literal was already in
// hand and was thrown away. It now travels alongside the order, and
// `version_req` is only ever asked to SORT.
//
// Part of the package-management subsystem refactor (PR-R4 in
// `.agents/docs/2026-05-08-pm-subsystem-architecture.md`), originally
// pulled out of `cli.cppm` verbatim.
//
// The descriptor is reached through `mcpp.pm.index_route`, never through a
// bare `Fetcher`. A `Fetcher` only ever reads the shared registry, so while
// these functions took one, a dependency served by a project
// `[indices]` entry resolved fine as an exact version and then failed the
// moment the same dependency carried a SemVer constraint — candidate
// selection could see the package, version resolution could not (#308).
//
// Implementation note: `resolve_semver` is **not** declared inline on
// purpose. Inlining it across modules makes every importer
// (cli.cppm, ...) instantiate `std::_Vector_base<vr::Version>`'s
// destructor locally. With musl-gcc 15.1's libstdc++ that triggers
// `undefined reference to std::_Vector_base<...>::_Vector_impl::~_Vector_impl()`
// at link time. Single-definition-point sidesteps the bug.

export module mcpp.pm.resolver;

import std;
import mcpp.manifest;
import mcpp.platform;
import mcpp.platform.axis;
import mcpp.pm.compat;
import mcpp.pm.dep_spec;
import mcpp.pm.index_route;
import mcpp.version_req;

export namespace mcpp::pm {

// xpkg.lua's `xpm.<key>` uses these names. (Distinct from
// `kCurrentPlatform` in cli.cppm, which is the [toolchain] table key —
// "macos" vs "macosx".)
inline constexpr std::string_view kXpkgPlatform = mcpp::platform::xpkg_platform;

// Returns true if `v` is a SemVer constraint (caret, tilde, range, glob,
// `*`, or empty) rather than a literal exact version. Empty counts as
// "constraint" so callers re-resolve via the index — bare `1.2.3` is
// treated as exact for back-compat with pre-SemVer pinning workflows;
// users opt into resolution by writing `^1.2.3` etc.
bool is_version_constraint(std::string_view v);

// ─── Namespace-aware overloads (canonical, 0.0.10+) ─────────────────

// Resolve a SemVer constraint against the index entry's available
// versions. Returns the chosen exact version string, or an error
// message. Uses structured (ns, shortName) for index lookup.
std::expected<std::string, std::string>
resolve_semver(std::string_view ns, std::string_view shortName,
               std::string_view constraint,
               const mcpp::pm::IndexRoute& route,
               const mcpp::platform::PlatformKey& platform);

// Try to AND-merge two version constraints and resolve to a single
// concrete version satisfying both. Uses structured (ns, shortName).
std::expected<std::string, std::string>
try_merge_semver(std::string_view ns, std::string_view shortName,
                 std::string_view a,
                 std::string_view b,
                 const mcpp::pm::IndexRoute& route,
                 const mcpp::platform::PlatformKey& platform);

// ─── Legacy overloads (COMPAT, remove in 1.0.0) ─────────────────────

std::expected<std::string, std::string>
resolve_semver(std::string_view name,
               std::string_view constraint,
               const mcpp::pm::IndexRoute& route);

std::expected<std::string, std::string>
try_merge_semver(std::string_view name,
                 std::string_view a,
                 std::string_view b,
                 const mcpp::pm::IndexRoute& route);

} // namespace mcpp::pm

namespace mcpp::pm {

bool is_version_constraint(std::string_view v) {
    if (v.empty()) return true;
    if (v == "*") return true;
    char c = v.front();
    if (c == '^' || c == '~' || c == '>' || c == '<' || c == '=') return true;
    if (v.find(',') != std::string_view::npos) return true;
    return false;
}

// ─── Namespace-aware resolve_semver (canonical, 0.0.10+) ─────────────

std::expected<std::string, std::string>
resolve_semver(std::string_view ns, std::string_view shortName,
               std::string_view constraint,
               const mcpp::pm::IndexRoute& route,
               const mcpp::platform::PlatformKey& platform)
{
    namespace vr = mcpp::version_req;
    auto qname = mcpp::pm::compat::qualified_name(ns, shortName);

    auto luaContent = route.read(mcpp::pm::DependencyCoordinate{
        .namespace_ = std::string(ns), .shortName = std::string(shortName) });
    if (!luaContent) {
        // `mcpp index update` is only advice worth giving when the shared
        // registry (or a not-yet-cloned git index) is what would have answered.
        // A project `[indices] path = …` is whatever the user has on disk, and
        // telling them to refresh it sends them nowhere.
        auto* idx = route.find_for_ns(ns);
        const bool refreshable = !idx || idx->is_builtin() || !idx->is_local();
        return std::unexpected(std::format(
            "dependency '{}' has SemVer constraint '{}' but no readable index "
            "entry for it{}",
            qname, constraint,
            refreshable ? " — run `mcpp index update` first" : ""));
    }

    auto entries = mcpp::manifest::list_xpkg_version_entries(*luaContent, platform);

    // An exact constraint naming a published key IS the answer, before any
    // parsing. Two things depend on this short-circuit:
    //
    //   * `= pre-v0.0.5` has to work. It is the documented remedy the
    //     unorderable-key error below hands out, and it reaches here through
    //     try_merge_semver (which canonicalises a literal pin to `=<literal>`).
    //     Routing it through the SemVer grammar would reject the very form the
    //     error message just told the user to write.
    //   * `= 1.0.0+a` has to select `1.0.0+a`. SemVer excludes build metadata
    //     from precedence, so comparing by order alone makes it ambiguous with
    //     `1.0.0+b` — while the two literals are not ambiguous at all.
    //
    // Aliases are eligible here: pinning `latest` or `25.0.4` exactly is a
    // legitimate address, and only RANGE selection has to ignore pointers.
    //
    // The `=` prefix is REQUIRED, and the guard is load-bearing rather than
    // decorative. A bare `1.2.3` is caret-default in this grammar (see
    // version_req.cppm), so matching it literally here would turn every
    // `dep = "1.2.3"` into an exact pin the moment the index happens to publish
    // that key — silently disabling `^`. Callers never send a bare literal
    // (`prepare`, `index_refresh` and `mcpp add` all gate on
    // `is_version_constraint`, and `try_merge_semver` canonicalises a literal
    // pin to `=<literal>`), but that invariant lives in the callers, so this
    // function refuses to depend on it.
    if (constraint.starts_with('=')) {
        auto exact = constraint.substr(1);
        while (!exact.empty() && (exact.front() == ' ' || exact.front() == '\t'))
            exact.remove_prefix(1);
        while (!exact.empty() && (exact.back() == ' ' || exact.back() == '\t'))
            exact.remove_suffix(1);
        if (!exact.empty() && exact != "*")
            for (auto const& e : entries)
                if (e.version == exact) return e.version;
    }

    auto req = vr::parse_req(constraint);
    if (!req) {
        return std::unexpected(std::format(
            "dependency '{}': invalid version constraint '{}': {}",
            qname, constraint, req.error()));
    }

    if (entries.empty()) {
        return std::unexpected(std::format(
            "dependency '{}': index entry has no versions for platform '{}'",
            qname, platform.key()));
    }

    // Split the published keys three ways. The LITERAL travels with the order:
    // what this function returns has to be a key the index actually holds, and
    // no rendering of the parsed numbers can promise that (mcpp#363).
    std::vector<std::string> literals;       // candidate keys, index-aligned with `parsed`
    std::vector<vr::Version> parsed;
    std::vector<std::string> unorderable;    // real entries whose key has no order
    std::vector<std::string> aliases;        // `{ ref = "..." }` pointers
    for (auto& e : entries) {
        if (e.alias) { aliases.push_back(e.version); continue; }
        auto v = vr::parse_version(e.version);
        if (!v) { unorderable.push_back(e.version); continue; }
        literals.push_back(e.version);
        parsed.push_back(std::move(*v));
    }

    auto join = [](const std::vector<std::string>& v) {
        std::string s;
        for (auto& x : v) { if (!s.empty()) s += ", "; s += x; }
        return s;
    };
    // "Pin it exactly" is the whole remedy for an unorderable key, so the hint
    // carries a line the user can paste.
    auto pin_hint = [&](const std::vector<std::string>& keys) {
        return std::format(
            "\n  These keys are not ordered versions, so no range can address "
            "them — pin one exactly:\n    {} = \"{}\"",
            qname, keys.front());
    };

    if (parsed.empty()) {
        // Blaming the index for having "no valid versions" is what this used to
        // do, and it is false: `khistory` publishes exactly one release, keyed
        // `pre-v0.0.5`, which is perfectly installable — just not by a range.
        if (!unorderable.empty()) {
            return std::unexpected(std::format(
                "dependency '{}': constraint '{}' cannot be resolved. The index "
                "publishes [{}]{}",
                qname, constraint, join(unorderable), pin_hint(unorderable)));
        }
        if (!aliases.empty()) {
            return std::unexpected(std::format(
                "dependency '{}': the index entry for platform '{}' has only "
                "alias versions [{}] and no release to point at",
                qname, platform.key(), join(aliases)));
        }
        return std::unexpected(std::format(
            "dependency '{}': index entry has no versions for platform '{}'",
            qname, platform.key()));
    }

    auto best = vr::choose_all(*req, parsed);
    if (best.empty()) {
        auto msg = std::format(
            "dependency '{}': constraint '{}' matches none of: [{}]",
            qname, constraint, join(literals));
        if (!unorderable.empty())
            msg += std::format("\n  (also published, but not orderable: [{}]){}",
                               join(unorderable), pin_hint(unorderable));
        return std::unexpected(msg);
    }
    if (best.size() > 1) {
        // Distinct keys that compare equal — build metadata (`1.0.0+a` vs
        // `1.0.0+b`), which SemVer excludes from precedence. They are two
        // different tarballs with two different hashes, and nothing in the
        // ordering can say which one was meant. Choosing "the larger literal"
        // would be a guess wearing determinism's clothes, and picking by
        // whichever line came first in the descriptor (the old behaviour) makes
        // a cosmetic reordering of the index change what gets built.
        std::vector<std::string> tied;
        for (auto i : best) tied.push_back(literals[i]);
        return std::unexpected(std::format(
            "dependency '{}': constraint '{}' matches {} versions that compare "
            "EQUAL: [{}]. They differ only in build metadata, which SemVer "
            "excludes from precedence, so mcpp cannot tell which one you want "
            "— pin one exactly:\n    {} = \"{}\"",
            qname, constraint, tied.size(), join(tied), qname, tied.front()));
    }
    return literals[best.front()];
}

// ─── Namespace-aware try_merge_semver (canonical, 0.0.10+) ───────────

std::expected<std::string, std::string>
try_merge_semver(std::string_view ns, std::string_view shortName,
                 std::string_view a,
                 std::string_view b,
                 const mcpp::pm::IndexRoute& route,
                 const mcpp::platform::PlatformKey& platform)
{
    auto canon = [](std::string_view v) -> std::string {
        if (v.empty() || v == "*") return std::string{};
        if (is_version_constraint(v)) return std::string(v);
        return "=" + std::string(v);
    };

    std::string ca = canon(a);
    std::string cb = canon(b);
    std::string merged;
    // Two consumers asking for the SAME thing is not a merge. Defensive rather
    // than a live fix — prepare only reaches here when the two RESOLVED versions
    // differ, and identical constraints resolve identically — but the failure it
    // prevents is silent and total: `=pre-v0.0.5,=pre-v0.0.5` routes an
    // unorderable key through the SemVer grammar, which rejects the very form
    // the unorderable-key error tells users to write.
    if (ca == cb) merged = ca.empty() ? "*" : ca;
    else if (!ca.empty() && !cb.empty()) merged = ca + "," + cb;
    else if (!ca.empty())            merged = ca;
    else if (!cb.empty())            merged = cb;
    else                              merged = "*";

    return resolve_semver(ns, shortName, merged, route, platform);
}

// ─── Legacy overloads (COMPAT, remove in 1.0.0) ─────────────────────

std::expected<std::string, std::string>
resolve_semver(std::string_view name,
               std::string_view constraint,
               const mcpp::pm::IndexRoute& route)
{
    auto resolved = mcpp::pm::compat::resolve_package_name(name, "");
    // Legacy overload: no target is threaded through it, so it names the host
    // axis explicitly rather than inheriting a silent default (#254).
    return resolve_semver(resolved.namespace_, resolved.shortName,
                          constraint, route,
                          mcpp::platform::HostPlatform::current());
}

std::expected<std::string, std::string>
try_merge_semver(std::string_view name,
                 std::string_view a,
                 std::string_view b,
                 const mcpp::pm::IndexRoute& route)
{
    auto resolved = mcpp::pm::compat::resolve_package_name(name, "");
    // Legacy overload — see the resolve_semver note above.
    return try_merge_semver(resolved.namespace_, resolved.shortName,
                            a, b, route,
                            mcpp::platform::HostPlatform::current());
}

} // namespace mcpp::pm
