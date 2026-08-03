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
import mcpp.pm.index_contract;
import mcpp.pm.index_route;
import mcpp.pm.resolver;
import mcpp.ui;
import mcpp.xlings;

export namespace mcpp::pm {

// Why a refresh was, or was not, triggered. Every value is user-visible under
// `-v`, and the Suppressed* ones are the interesting half when diagnosing
// "why did/didn't mcpp go to the network".
enum class RefreshReason {
    None,                   // resolvable locally — the steady state, zero network
    IndexAbsent,            // no local index at all (cold start)
    DescriptorMiss,         // package unknown locally, and the index can say so
    VersionMiss,            // package known, constraint unsatisfiable locally
    SuppressedOffline,      // --offline / MCPP_OFFLINE
    SuppressedDisabled,     // [index] auto_refresh = false
    SuppressedDebounce,     // refreshed moments ago; upstream simply lacks it
    SuppressedInconclusive, // a miss here proves nothing (see header)
    SuppressedIndexUnusable,// the index that would answer is too new to read
};

struct RefreshPolicy {
    bool         offline     = false;
    bool         autoRefresh = true;
    // Shared with the xim install gate — one constant, one rationale, in the
    // leaf module both layers can see (mcpp::xlings).
    std::int64_t debounceSeconds = mcpp::xlings::kIndexRefreshDebounceSeconds;
};

struct RefreshDecision {
    bool          shouldRefresh = false;
    RefreshReason reason        = RefreshReason::None;
    std::string   subject;      // "mcpplibs:fmt@^1.3" — for logs and errors
};

// Human tail for the status line: "package index — <subject> <reason_text>".
std::string_view reason_text(RefreshReason r);

// flag > env > config. `offlineFlag` is the parsed `--offline`.
RefreshPolicy policy_for(const mcpp::config::GlobalConfig& cfg);

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

// For a caller that has ALREADY established a conclusive miss through
// `lookup_descriptor` (that is `mcpp add`): only the policy half of the
// judgement is left to make. Keeps the opt-outs in one place rather than
// re-tested at each call site.
RefreshDecision decide_for_miss(const RefreshPolicy&     policy,
                                const mcpp::xlings::Env& env,
                                std::string_view         subject);

// Run the sync a decision asked for. At most ONE per process: a build whose
// deps miss for the same reason should pay one sync, not one per dep.
// Returns an error only when the sync itself failed; callers decide whether
// that is fatal (it is not, if the build can still resolve locally).
std::expected<void, std::string> apply(const RefreshDecision& d,
                                       const mcpp::xlings::Env& env);

// Explicit user intent (`mcpp update`): ignores debounce and the once-per-
// process guard. Still refuses when offline, and says so.
std::expected<void, std::string> force_refresh(const mcpp::xlings::Env& env);

// One line of advisory context for a resolution failure: what the index is and
// how old it is. Never a gate — only ever appended to an error the user is
// already seeing.
std::string staleness_note(const mcpp::xlings::Env& env);

} // namespace mcpp::pm

namespace mcpp::pm {

namespace {

// One sync per process, however many dependencies ask for one.
bool g_refreshed_this_process = false;

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

std::string age_phrase(std::int64_t s) {
    if (s < 0)      return "never refreshed";
    if (s < 90)     return std::format("refreshed {}s ago", s);
    if (s < 5400)   return std::format("refreshed {}m ago", s / 60);
    if (s < 172800) return std::format("refreshed {}h ago", s / 3600);
    return std::format("refreshed {}d ago", s / 86400);
}

} // namespace

std::string_view reason_text(RefreshReason r) {
    switch (r) {
        case RefreshReason::None:                   return "resolvable locally";
        case RefreshReason::IndexAbsent:            return "no local package index";
        case RefreshReason::DescriptorMiss:         return "not found locally";
        case RefreshReason::VersionMiss:            return "not satisfiable locally";
        case RefreshReason::SuppressedOffline:      return "offline mode";
        case RefreshReason::SuppressedDisabled:     return "[index] auto_refresh = false";
        case RefreshReason::SuppressedDebounce:     return "index was just refreshed";
        case RefreshReason::SuppressedInconclusive: return "no index can refute this";
        case RefreshReason::SuppressedIndexUnusable:
            return "an index requires a newer mcpp — refreshing cannot help";
    }
    return "";
}

RefreshPolicy policy_for(const mcpp::config::GlobalConfig& cfg) {
    RefreshPolicy p;
    p.offline     = mcpp::platform::env::offline_mode();
    p.autoRefresh = cfg.indexAutoRefresh;
    return p;
}

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

RefreshDecision decide_for_miss(const RefreshPolicy&     policy,
                                const mcpp::xlings::Env& env,
                                std::string_view         subject) {
    RefreshDecision d;
    d.subject = std::string(subject);
    // See decide(): a refresh cannot fix an index this binary cannot read.
    if (mcpp::pm::any_index_unusable()) {
        d.reason = RefreshReason::SuppressedIndexUnusable;
        return d;
    }
    if (policy.offline)      { d.reason = RefreshReason::SuppressedOffline;  return d; }
    if (!policy.autoRefresh) { d.reason = RefreshReason::SuppressedDisabled; return d; }
    // Same debounce as the build path: two `mcpp add` typos in a row should not
    // buy two multi-repo syncs, for the same reason a build with two missing
    // packages does not.
    auto status = mcpp::xlings::default_index_status(env, policy.debounceSeconds);
    if (status.present && status.ageSeconds >= 0
        && status.ageSeconds < policy.debounceSeconds) {
        d.reason = RefreshReason::SuppressedDebounce;
        return d;
    }
    d.reason = RefreshReason::DescriptorMiss;
    d.shouldRefresh = true;
    return d;
}

namespace {

std::expected<void, std::string> run_sync(const mcpp::xlings::Env& env,
                                          std::string_view banner) {
    auto before = mcpp::xlings::default_index_status(env, 0).rev;
    mcpp::ui::status("Refreshing", banner);
    int rc = mcpp::xlings::update_index(env, /*quiet=*/true);
    if (rc != 0)
        return std::unexpected(std::format("package index refresh failed (rc {})", rc));
    auto after = mcpp::xlings::default_index_status(env, 0).rev;
    if (before && after && *before != *after)
        mcpp::ui::status("Updated", std::format("package index {} → {}", *before, *after));
    else if (after)
        mcpp::log::verbose("index", std::format("package index still at {}", *after));
    return {};
}

} // namespace

std::expected<void, std::string> apply(const RefreshDecision& d,
                                       const mcpp::xlings::Env& env)
{
    if (!d.shouldRefresh) {
        mcpp::log::verbose("index", std::format(
            "skip refresh for {}: {}", d.subject, reason_text(d.reason)));
        return {};
    }
    if (g_refreshed_this_process) {
        mcpp::log::verbose("index", std::format(
            "skip refresh for {}: already refreshed in this run", d.subject));
        return {};
    }
    g_refreshed_this_process = true;
    return run_sync(env, std::format("package index — `{}` {} (one-time)",
                                     d.subject, reason_text(d.reason)));
}

std::expected<void, std::string> force_refresh(const mcpp::xlings::Env& env) {
    if (mcpp::platform::env::offline_mode())
        return std::unexpected(
            "offline mode is on — cannot refresh the package index "
            "(unset MCPP_OFFLINE or drop --offline)");
    g_refreshed_this_process = true;
    return run_sync(env, "package index (requested)");
}

std::string staleness_note(const mcpp::xlings::Env& env) {
    auto st = mcpp::xlings::default_index_status(env, 0);
    if (!st.present) return "no local package index";
    return st.rev
        ? std::format("local index {} ({})", *st.rev, age_phrase(st.ageSeconds))
        : std::format("local index {}", age_phrase(st.ageSeconds));
}

} // namespace mcpp::pm
