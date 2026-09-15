// mcpp.pm.index_refresh — the single source of truth for "should mcpp touch
// the network to refresh a package index right now?"
//
// WHY THIS MODULE EXISTS
//
// The answer used to be derived independently in five places, and two of them
// disagreed. `mcpp.xlings`' xim gate (ensure_official_package_index_fresh) had
// been offline-first for a while — it refreshes only when a requested package
// is genuinely absent locally, and its comment says in so many words that a
// TTL must NOT trigger a network sync, because that is what hangs a build on a
// slow or blocked network. `mcpp.build.prepare` meanwhile refreshed on exactly
// that: a marker older than an hour, whether or not anything was missing. So
// the offline-first policy was real but unreachable — the TTL fired first,
// every hour, on every build with a registry dependency (#315).
//
// THE AXIS WAS WRONG, NOT JUST THE THRESHOLD
//
// "Is this index fresh enough?" is unanswerable, and mtime is a bad proxy for
// it: a restored CI cache, a clock skew, or a tar that preserved timestamps all
// produce a marker that lies in either direction. The answerable question is
// "can the resolver do its job with what is on disk?" — and every resolution
// input IS on disk (descriptors are files; `resolve_semver` parses a local
// xpkg.lua). So the decision below is a pure, offline, deterministic function
// of local state, and the marker is demoted to a debounce timer.
//
// THE TRAP THIS MODULE IS MOSTLY WRITTEN AROUND
//
// "Not found locally" must NEVER be read as "needs a refresh" on its own. A
// miss only means something when the index that would have answered is both
// readable and authoritative — `IndexRoute::authoritative_for` (#307) is what
// decides that. Notably, xim descriptors declare no `namespace`, so `(xim, x)`
// can never match the identity gate: treat that miss as real and every build
// with a toolchain-ish dependency refreshes EVERY TIME, which is strictly worse
// than the TTL this change removes. See `SuppressedInconclusive`.

export module mcpp.pm.index_refresh;

import std;
import mcpp.config;
import mcpp.log;
import mcpp.platform;
import mcpp.platform.axis;
import mcpp.pm.dep_spec;
import mcpp.pm.dependency_selector;   // legacy_bare_candidates
import mcpp.pm.index_contract;
import mcpp.pm.index_route;
export import mcpp.pm.refresh_policy;   // the policy half, shared with the fetcher
import mcpp.pm.resolver;
import mcpp.ui;
import mcpp.xlings;

export namespace mcpp::pm {

// Pure: no network, no filesystem writes, no side effects. The judgement table
// is the contract — `tests/unit/test_pm_index_refresh.cpp` locks it row by row.
RefreshDecision decide_for_dependency(const IndexRoute&               route,
                                      std::string_view                depKey,
                                      const DependencySpec&           spec,
                                      const mcpp::xlings::Env&        env,
                                      const mcpp::platform::PlatformKey& platform,
                                      const RefreshPolicy&            policy);

// Is this dependency served by the shared registry (as opposed to a project
// `[indices]` entry, a path, or a git URL)? Refreshing the global index does
// nothing for the others, so callers use this to skip the sync entirely.
bool routes_to_builtin(const IndexRoute& route, const DependencySpec& spec);

} // namespace mcpp::pm

namespace mcpp::pm {

namespace {

// Echo back what the USER wrote (the `[dependencies]` key), not the resolved
// coordinate. `xim.nasm` is parsed into the candidate `(mcpplibs.xim, nasm)`,
// and reporting that spelling in a diagnostic sends the reader looking for a
// namespace they never typed.
std::string subject_of(std::string_view depKey, const DependencySpec& spec) {
    std::string name{depKey};
    if (name.empty()) {
        name = spec.shortName;
        if (!spec.namespace_.empty())
            name = std::format("{}:{}", spec.namespace_, name);
    }
    return spec.version.empty() ? name : std::format("{}@{}", name, spec.version);
}

// Does ANY candidate for this dependency resolve through the shared registry?
// A project `[indices] path = …` is whatever the user has on disk and a custom
// git index is synced by its own project-scoped path — refreshing the global
// index would do nothing for either, so those keep today's behaviour exactly.
bool coords_route_to_builtin(const IndexRoute& route,
                             const std::vector<DependencyCoordinate>& coords) {
    for (auto& c : coords) {
        auto* idx = route.find_for_ns(c.namespace_);
        if (!idx || idx->is_builtin()) return true;
    }
    return false;
}

std::vector<DependencyCoordinate> coords_of(std::string_view depKey,
                                            const DependencySpec& spec) {
    if (!spec.candidates.empty()) return spec.candidates;
    // Specs built by hand (tests, older call sites) carry no candidate list.
    return { DependencyCoordinate{
        .namespace_ = spec.namespace_,
        .shortName  = spec.shortName.empty() ? std::string(depKey) : spec.shortName,
    } };
}

} // namespace

RefreshDecision decide_for_dependency(const IndexRoute&               route,
                                      std::string_view                depKey,
                                      const DependencySpec&           spec,
                                      const mcpp::xlings::Env&        env,
                                      const mcpp::platform::PlatformKey& platform,
                                      const RefreshPolicy&            policy)
{
    RefreshDecision d;
    d.subject = subject_of(depKey, spec);

    // 1. Sources that never consult an index.
    if (spec.isPath() || spec.isGit()) return d;

    // 2. Only the shared registry is refreshable from here.
    auto coords = coords_of(depKey, spec);
    if (!coords_route_to_builtin(route, coords)) return d;

    // 3. INV-3: a miss only counts when the index could have refuted it.
    auto found = lookup_descriptor(route, coords);

    // 3b. THE RESOLVER'S LADDER, NOT A SHORTER ONE. After the exact coordinate
    //     misses, the resolver tries the deprecated bare-name rung for a
    //     version selector whose namespace was omitted (`ftxui = "6.1.9"`
    //     reaches `compat.ftxui`, prepare.cppm). A decision that stopped at the
    //     exact coordinate called that dependency missing while the resolver
    //     found it on disk, so every build of such a manifest started a network
    //     refresh once the debounce had passed (mcpp-community/mcpp#648: an
    //     editor planning in the background waited on one for 11 minutes). The
    //     same condition as the resolver's, for as long as the rung exists.
    if (!found.hit && found.error.empty() && found.conclusive
        && spec.isVersion() && spec.namespaceOmitted && !coords.empty()) {
        auto legacy = lookup_descriptor(
            route, mcpp::pm::legacy_bare_candidates(coords.front()));
        if (legacy.hit) found = std::move(legacy);
    }

    if (!found.error.empty()) {
        d.reason = RefreshReason::SuppressedMalformedDescriptor;
        return d;
    }
    if (!found.hit && !found.conclusive) {
        d.reason = RefreshReason::SuppressedInconclusive;
        return d;
    }

    auto status = mcpp::xlings::default_index_status(env, policy.debounceSeconds);

    // 4-6. The three ways local state fails to answer.
    //
    // Deliberately evaluated BEFORE the opt-outs below: this is all local file
    // I/O, and knowing whether the answer was actually available is what makes
    // the diagnostic worth reading. Short-circuiting on `--offline` first would
    // report "offline mode" for a dependency that resolved perfectly well —
    // which is precisely the case the user is trying to confirm.
    if (!status.present) {
        d.reason = RefreshReason::IndexAbsent;
        d.shouldRefresh = true;
    } else if (!found.hit) {
        d.reason = RefreshReason::DescriptorMiss;
        d.shouldRefresh = true;
    } else if (is_version_constraint(spec.version)) {
        // Only CONSTRAINTS are checked here. An exact pin is deliberately not:
        // version tables are per-OS, so "this host publishes no 1.2.3" is not
        // "1.2.3 does not exist" (the same judgement `mcpp add` reaches — it
        // flags an unpublished exact version rather than refusing it). Treating
        // it as a miss would refresh on every build for a dependency that is
        // simply not built for this platform. The install layer already covers
        // the real version-miss case: it refreshes once when an install fails.
        auto resolved = resolve_semver(found.hit->coord.namespace_,
                                       found.hit->coord.shortName,
                                       spec.version, route, platform);
        if (!resolved) {
            d.reason = RefreshReason::VersionMiss;
            d.shouldRefresh = true;
        }
    }

    if (!d.shouldRefresh) return d;      // nothing to suppress

    // An index this binary cannot READ makes every descriptor lookup in it come
    // back empty, and that miss is indistinguishable from "the package is not
    // there" at the call site. Refreshing would fetch the same unreadable tree
    // again — so the miss would repeat, and the unusable state would drive
    // repeated refreshes of itself. Stop here and let the E0006 error stand as
    // the explanation.
    if (mcpp::pm::any_index_unusable()) {
        d.shouldRefresh = false;
        d.reason = RefreshReason::SuppressedIndexUnusable;
        return d;
    }

    // 7. Opt-outs, in order of authority: the user's flag, the machine's
    //    config, then the "we just did this" guard.
    if (policy.offline)      { d.shouldRefresh = false; d.reason = RefreshReason::SuppressedOffline;  return d; }
    if (!policy.autoRefresh) { d.shouldRefresh = false; d.reason = RefreshReason::SuppressedDisabled; return d; }

    // 8. Debounce — but never for a cold start, where there is nothing to
    //    debounce against and the build cannot proceed without the data.
    if (d.reason != RefreshReason::IndexAbsent
        && status.ageSeconds >= 0 && status.ageSeconds < policy.debounceSeconds) {
        d.shouldRefresh = false;
        d.reason = RefreshReason::SuppressedDebounce;
    }

    return d;
}

bool routes_to_builtin(const IndexRoute& route, const DependencySpec& spec) {
    if (spec.isPath() || spec.isGit()) return false;
    return coords_route_to_builtin(route, coords_of(spec.shortName, spec));
}

} // namespace mcpp::pm
