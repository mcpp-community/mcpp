// mcpp.pm.refresh_policy -- the policy half of "may this run refresh a package
// index now?", in a module the package fetcher can import.
//
// WHY THIS IS NOT PART OF mcpp.pm.index_refresh. That module answers the whole
// question for a dependency, and to do so it reads descriptors through
// mcpp.pm.index_route, which imports the fetcher. The fetcher could therefore
// not ask it, and it asked a second derivation instead:
// `ensure_official_package_index_fresh` before an install, and a bare
// `update_index` before a retry. Neither consulted `[index] auto_refresh`,
// which docs/05 documents as "never refresh the index automatically"
// (mcpp-community/mcpp#648 A5). The opt-outs, the debounce, the one-sync-per-
// process guard and the sync itself live here, and every caller that refreshes
// an index goes through `decide_for_miss` or `decide_for_dependency` and then
// `apply`.

export module mcpp.pm.refresh_policy;

import std;
import mcpp.config;
import mcpp.log;
import mcpp.platform;
import mcpp.pm.index_contract;
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
    SuppressedMalformedDescriptor, // local bytes exist but violate identity
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
        case RefreshReason::SuppressedMalformedDescriptor:
            return "descriptor identity is malformed — refreshing cannot help";
    }
    return "";
}

RefreshPolicy policy_for(const mcpp::config::GlobalConfig& cfg) {
    RefreshPolicy p;
    p.offline     = mcpp::platform::env::offline_mode();
    p.autoRefresh = cfg.indexAutoRefresh;
    return p;
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
