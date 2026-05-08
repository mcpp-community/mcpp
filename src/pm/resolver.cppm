// mcpp.pm.resolver — turn a SemVer constraint into a concrete version,
// using the package's xpkg lua descriptor as the version inventory.
//
// Part of the package-management subsystem refactor (PR-R4 in
// `.agents/docs/2026-05-08-pm-subsystem-architecture.md`). Strictly
// pulled out of `cli.cppm` with no behavior change; the same
// signatures, the same error strings, the same platform key picking.
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
import mcpp.pm.package_fetcher;
import mcpp.version_req;

export namespace mcpp::pm {

// xpkg.lua's `xpm.<key>` uses these names. (Distinct from
// `kCurrentPlatform` in cli.cppm, which is the [toolchain] table key —
// "macos" vs "macosx".)
inline constexpr std::string_view kXpkgPlatform =
#if defined(__linux__)
    "linux";
#elif defined(__APPLE__)
    "macosx";
#elif defined(_WIN32)
    "windows";
#else
    "linux";
#endif

// Returns true if `v` is a SemVer constraint (caret, tilde, range, glob,
// `*`, or empty) rather than a literal exact version. Empty counts as
// "constraint" so callers re-resolve via the index — bare `1.2.3` is
// treated as exact for back-compat with pre-SemVer pinning workflows;
// users opt into resolution by writing `^1.2.3` etc.
bool is_version_constraint(std::string_view v);

// Resolve a SemVer constraint against the index entry's available
// versions. Returns the chosen exact version string, or an error
// message. The fetcher is used to read the lua descriptor for the
// requested package name.
std::expected<std::string, std::string>
resolve_semver(std::string_view name,
               std::string_view constraint,
               mcpp::pm::Fetcher& fetcher);

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

std::expected<std::string, std::string>
resolve_semver(std::string_view name,
               std::string_view constraint,
               mcpp::pm::Fetcher& fetcher)
{
    namespace vr = mcpp::version_req;

    auto luaContent = fetcher.read_xpkg_lua(name);
    if (!luaContent) {
        return std::unexpected(std::format(
            "dependency '{}' has SemVer constraint '{}' but the index entry "
            "isn't cloned locally yet — run `mcpp index update` first",
            name, constraint));
    }

    auto req = vr::parse_req(constraint);
    if (!req) {
        return std::unexpected(std::format(
            "dependency '{}': invalid version constraint '{}': {}",
            name, constraint, req.error()));
    }

    auto rawVersions = mcpp::manifest::list_xpkg_versions(*luaContent, kXpkgPlatform);
    if (rawVersions.empty()) {
        return std::unexpected(std::format(
            "dependency '{}': index entry has no versions for platform '{}'",
            name, kXpkgPlatform));
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
            "dependency '{}': no valid versions in index", name));
    }

    auto idx = vr::choose(*req, parsed);
    if (!idx) {
        std::string avail;
        for (auto& s : rawVersions) { if (!avail.empty()) avail += ", "; avail += s; }
        return std::unexpected(std::format(
            "dependency '{}': constraint '{}' matches none of: [{}]",
            name, constraint, avail));
    }
    return parsed[*idx].str();
}

} // namespace mcpp::pm
