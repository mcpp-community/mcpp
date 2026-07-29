// mcpp.pm.resolver — turn a SemVer constraint into a concrete version,
// using the package's xpkg lua descriptor as the version inventory.
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

    auto req = vr::parse_req(constraint);
    if (!req) {
        return std::unexpected(std::format(
            "dependency '{}': invalid version constraint '{}': {}",
            qname, constraint, req.error()));
    }

    auto rawVersions = mcpp::manifest::list_xpkg_versions(*luaContent, platform);
    if (rawVersions.empty()) {
        return std::unexpected(std::format(
            "dependency '{}': index entry has no versions for platform '{}'",
            qname, platform.key()));
    }

    std::vector<vr::Version> parsed;
    parsed.reserve(rawVersions.size());
    for (auto& s : rawVersions) {
        auto v = vr::parse_version(s);
        if (!v) continue;     // ignore unparseable entries
        parsed.push_back(*v);
    }
    if (parsed.empty()) {
        return std::unexpected(std::format(
            "dependency '{}': no valid versions in index", qname));
    }

    auto idx = vr::choose(*req, parsed);
    if (!idx) {
        std::string avail;
        for (auto& s : rawVersions) { if (!avail.empty()) avail += ", "; avail += s; }
        return std::unexpected(std::format(
            "dependency '{}': constraint '{}' matches none of: [{}]",
            qname, constraint, avail));
    }
    return parsed[*idx].str();
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
    if (!ca.empty() && !cb.empty()) merged = ca + "," + cb;
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
