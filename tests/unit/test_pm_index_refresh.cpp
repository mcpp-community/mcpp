// The judgement table of mcpp.pm.index_refresh, row by row.
//
// This file is the contract for #315: a build refreshes the package index when
// — and only when — the local copy cannot answer. The rows worth staring at:
//
//   InconclusiveNamespaceNeverRefreshes — the one that decides whether this
//     whole change is an improvement or a regression. xim descriptors declare
//     no `namespace`, so `(xim, nasm)` can never satisfy the identity gate; if
//     that miss counted, every build with a xim dependency would sync EVERY
//     TIME, which is worse than the hourly TTL being removed.
//   ExactVersionAbsentIsNotAVersionMiss — version tables are per-OS, so "this
//     host publishes no 1.2.3" is not "1.2.3 does not exist".
//   ColdStartIgnoresDebounce — an absent index has nothing to debounce against.

#include <gtest/gtest.h>

import std;
import mcpp.config;
import mcpp.pm.dep_spec;
import mcpp.pm.index_refresh;
import mcpp.pm.index_route;
import mcpp.pm.index_spec;
import mcpp.platform.axis;
import mcpp.xlings;

namespace {

using mcpp::pm::RefreshReason;

// A throwaway MCPP_HOME whose layout matches what the builtin registry reader
// walks: <home>/data/<index>/pkgs/<initial>/<name>.lua
struct FakeRegistry {
    std::filesystem::path home;

    explicit FakeRegistry(std::string_view tag) {
        home = std::filesystem::temp_directory_path()
             / std::format("mcpp-index-refresh-{}-{}", tag,
                           std::filesystem::hash_value(
                               std::filesystem::path(std::string(tag))));
        std::filesystem::remove_all(home);
        std::filesystem::create_directories(home / "data" / "mcpplibs" / "pkgs");
    }
    ~FakeRegistry() { std::error_code ec; std::filesystem::remove_all(home, ec); }

    std::filesystem::path index_dir() const { return home / "data" / "mcpplibs"; }

    void publish(std::string_view name, std::string_view versions) {
        auto dir = index_dir() / "pkgs" / std::string(1, name.front());
        std::filesystem::create_directories(dir);
        std::ofstream(dir / std::format("{}.lua", name)) << std::format(R"(
package = {{
    spec      = "1",
    namespace = "mcpplibs",
    name      = "{}",
    type      = "package",
    xpm = {{
        linux   = {{ {} }},
        macosx  = {{ {} }},
        windows = {{ {} }},
    }},
}}
)", name, versions, versions, versions);
    }

    // Age the refresh marker by writing it and back-dating it.
    void mark_refreshed(std::chrono::seconds ago = std::chrono::seconds{0}) {
        auto marker = index_dir() / ".mcpp-index-updated";
        std::ofstream(marker) << "ok\n";
        std::error_code ec;
        std::filesystem::last_write_time(
            marker, std::filesystem::file_time_type::clock::now() - ago, ec);
    }

    void remove_index() { std::filesystem::remove_all(index_dir()); }

    mcpp::xlings::Env env() const { return mcpp::xlings::Env{ .home = home }; }

    mcpp::config::GlobalConfig config() const {
        mcpp::config::GlobalConfig cfg;
        cfg.registryDir = home;
        return cfg;
    }
};

mcpp::pm::DependencySpec version_dep(std::string_view ns,
                                     std::string_view shortName,
                                     std::string_view version) {
    mcpp::pm::DependencySpec spec;
    spec.namespace_ = std::string(ns);
    spec.shortName  = std::string(shortName);
    spec.version    = std::string(version);
    spec.candidates.push_back(mcpp::pm::DependencyCoordinate{
        .namespace_ = std::string(ns), .shortName = std::string(shortName) });
    return spec;
}

RefreshReason decide(const mcpp::pm::IndexRoute& route,
                     const mcpp::pm::DependencySpec& spec,
                     const mcpp::xlings::Env& env,
                     mcpp::pm::RefreshPolicy policy = {}) {
    return mcpp::pm::decide_for_dependency(
        route, spec.shortName, spec, env,
        mcpp::platform::TargetPlatform::for_os("linux"), policy).reason;
}

bool refreshes(const mcpp::pm::IndexRoute& route,
               const mcpp::pm::DependencySpec& spec,
               const mcpp::xlings::Env& env,
               mcpp::pm::RefreshPolicy policy = {}) {
    return mcpp::pm::decide_for_dependency(
        route, spec.shortName, spec, env,
        mcpp::platform::TargetPlatform::for_os("linux"), policy).shouldRefresh;
}

constexpr std::string_view kOneVersion = R"(["1.2.0"] = { url = "u", sha256 = "s" },)";

} // namespace

// ─── Rows that never reach the filesystem ────────────────────────────────

TEST(PmIndexRefresh, PathAndGitDependenciesNeverRefresh) {
    FakeRegistry reg("srcdeps");
    mcpp::pm::IndexMap indices;
    auto cfg = reg.config();
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", &cfg };

    mcpp::pm::DependencySpec pathDep;
    pathDep.path = "../sibling";
    EXPECT_EQ(decide(route, pathDep, reg.env()), RefreshReason::None);

    mcpp::pm::DependencySpec gitDep;
    gitDep.git = "https://example.invalid/x.git";
    EXPECT_EQ(decide(route, gitDep, reg.env()), RefreshReason::None);
}

TEST(PmIndexRefresh, OfflineSuppressesEvenAGenuineMiss) {
    FakeRegistry reg("offline");
    reg.mark_refreshed(std::chrono::hours{99});
    mcpp::pm::IndexMap indices;
    auto cfg = reg.config();
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", &cfg };

    auto spec = version_dep("mcpplibs", "absent", "1.0.0");
    mcpp::pm::RefreshPolicy offline; offline.offline = true;

    EXPECT_EQ(decide(route, spec, reg.env(), offline), RefreshReason::SuppressedOffline);
    EXPECT_FALSE(refreshes(route, spec, reg.env(), offline));
}

TEST(PmIndexRefresh, OfflineDoesNotMisreportAResolvableDependency) {
    // The opt-outs are applied AFTER the local analysis, so a dependency that
    // resolved fine is reported as such even offline. Short-circuiting on the
    // flag first would answer "offline mode" for every dependency — hiding
    // exactly what an offline user is checking.
    FakeRegistry reg("offlinesteady");
    reg.publish("widget", kOneVersion);
    reg.mark_refreshed(std::chrono::hours{99});
    mcpp::pm::IndexMap indices;
    auto cfg = reg.config();
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", &cfg };

    mcpp::pm::RefreshPolicy offline; offline.offline = true;
    EXPECT_EQ(decide(route, version_dep("mcpplibs", "widget", "1.2.0"), reg.env(), offline),
              RefreshReason::None);
}

TEST(PmIndexRefresh, AutoRefreshOptOutSuppressesAGenuineMiss) {
    FakeRegistry reg("optout");
    reg.mark_refreshed(std::chrono::hours{99});
    mcpp::pm::IndexMap indices;
    auto cfg = reg.config();
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", &cfg };

    auto spec = version_dep("mcpplibs", "absent", "1.0.0");
    mcpp::pm::RefreshPolicy off; off.autoRefresh = false;

    EXPECT_EQ(decide(route, spec, reg.env(), off), RefreshReason::SuppressedDisabled);
}

// ─── INV-3: a miss must be refutable to mean anything ────────────────────

TEST(PmIndexRefresh, InconclusiveNamespaceNeverRefreshes) {
    FakeRegistry reg("inconclusive");
    reg.mark_refreshed(std::chrono::hours{99});
    mcpp::pm::IndexMap indices;
    auto cfg = reg.config();
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", &cfg };

    // `xim` and any third-party namespace: the builtin registry is what would
    // answer, but it is not authoritative for identities mcpp does not define,
    // so "absent" here is not evidence of anything.
    for (auto ns : { "xim", "somevendor" }) {
        auto spec = version_dep(ns, "thing", "1.0.0");
        EXPECT_EQ(decide(route, spec, reg.env()), RefreshReason::SuppressedInconclusive)
            << "namespace: " << ns;
        EXPECT_FALSE(refreshes(route, spec, reg.env())) << "namespace: " << ns;
    }
}

TEST(PmIndexRefresh, LazyGitIndexNeverRefreshesTheGlobalOne) {
    FakeRegistry reg("lazygit");
    reg.mark_refreshed(std::chrono::hours{99});
    mcpp::pm::IndexMap indices;
    indices["acme"] = mcpp::pm::IndexSpec{ .name = "acme", .url = "https://x/y" };
    auto cfg = reg.config();
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", &cfg };

    // Served by a custom index; syncing the shared registry would not help.
    auto spec = version_dep("acme", "widget", "1.0.0");
    EXPECT_EQ(decide(route, spec, reg.env()), RefreshReason::None);
}

// ─── The three ways local state fails to answer ──────────────────────────

TEST(PmIndexRefresh, PresentDescriptorIsTheSteadyStateAndCostsNothing) {
    FakeRegistry reg("steady");
    reg.publish("widget", kOneVersion);
    reg.mark_refreshed(std::chrono::hours{99});   // old, and irrelevant
    mcpp::pm::IndexMap indices;
    auto cfg = reg.config();
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", &cfg };

    EXPECT_EQ(decide(route, version_dep("mcpplibs", "widget", "1.2.0"), reg.env()),
              RefreshReason::None);
    EXPECT_EQ(decide(route, version_dep("mcpplibs", "widget", "^1.2"), reg.env()),
              RefreshReason::None);
}

TEST(PmIndexRefresh, MissingDescriptorTriggersARefresh) {
    FakeRegistry reg("descmiss");
    reg.publish("widget", kOneVersion);
    reg.mark_refreshed(std::chrono::hours{99});
    mcpp::pm::IndexMap indices;
    auto cfg = reg.config();
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", &cfg };

    auto spec = version_dep("mcpplibs", "absent", "1.0.0");
    EXPECT_EQ(decide(route, spec, reg.env()), RefreshReason::DescriptorMiss);
    EXPECT_TRUE(refreshes(route, spec, reg.env()));
}

TEST(PmIndexRefresh, UnsatisfiableConstraintTriggersARefresh) {
    FakeRegistry reg("vermiss");
    reg.publish("widget", kOneVersion);          // only 1.2.0 exists
    reg.mark_refreshed(std::chrono::hours{99});
    mcpp::pm::IndexMap indices;
    auto cfg = reg.config();
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", &cfg };

    auto spec = version_dep("mcpplibs", "widget", "^9.9");
    EXPECT_EQ(decide(route, spec, reg.env()), RefreshReason::VersionMiss);
}

TEST(PmIndexRefresh, ExactVersionAbsentIsNotAVersionMiss) {
    FakeRegistry reg("exactver");
    reg.publish("widget", kOneVersion);          // only 1.2.0 exists
    reg.mark_refreshed(std::chrono::hours{99});
    mcpp::pm::IndexMap indices;
    auto cfg = reg.config();
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", &cfg };

    // Version tables are per-OS: an exact pin the local table does not list may
    // simply not be built for this host. Refreshing on it would fire on EVERY
    // build for such a dependency. The install layer covers the real case.
    auto spec = version_dep("mcpplibs", "widget", "9.9.9");
    EXPECT_EQ(decide(route, spec, reg.env()), RefreshReason::None);
}

// ─── Debounce ────────────────────────────────────────────────────────────

TEST(PmIndexRefresh, DebounceSuppressesTheSecondMissInAWindow) {
    FakeRegistry reg("debounce");
    reg.publish("widget", kOneVersion);
    reg.mark_refreshed(std::chrono::seconds{5});   // just synced
    mcpp::pm::IndexMap indices;
    auto cfg = reg.config();
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", &cfg };

    auto spec = version_dep("mcpplibs", "absent", "1.0.0");
    EXPECT_EQ(decide(route, spec, reg.env()), RefreshReason::SuppressedDebounce);
    EXPECT_FALSE(refreshes(route, spec, reg.env()));
}

TEST(PmIndexRefresh, ColdStartIgnoresDebounce) {
    FakeRegistry reg("coldstart");
    reg.mark_refreshed(std::chrono::seconds{5});
    reg.remove_index();      // marker gone with it; no local index at all
    mcpp::pm::IndexMap indices;
    auto cfg = reg.config();
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", &cfg };

    auto spec = version_dep("mcpplibs", "widget", "1.0.0");
    EXPECT_EQ(decide(route, spec, reg.env()), RefreshReason::IndexAbsent);
    EXPECT_TRUE(refreshes(route, spec, reg.env()));
}

// ─── decide_for_miss (the `mcpp add` half) ───────────────────────────────

TEST(PmIndexRefresh, DecideForMissHonoursTheSameOptOutsAndDebounce) {
    FakeRegistry reg("formiss");
    reg.mark_refreshed(std::chrono::hours{99});
    auto env = reg.env();

    mcpp::pm::RefreshPolicy allowed;
    EXPECT_TRUE(mcpp::pm::decide_for_miss(allowed, env, "pkg").shouldRefresh);

    mcpp::pm::RefreshPolicy offline; offline.offline = true;
    EXPECT_EQ(mcpp::pm::decide_for_miss(offline, env, "pkg").reason,
              RefreshReason::SuppressedOffline);

    mcpp::pm::RefreshPolicy disabled; disabled.autoRefresh = false;
    EXPECT_EQ(mcpp::pm::decide_for_miss(disabled, env, "pkg").reason,
              RefreshReason::SuppressedDisabled);

    // Two `mcpp add` typos in a row must not buy two multi-repo syncs.
    reg.mark_refreshed(std::chrono::seconds{5});
    EXPECT_EQ(mcpp::pm::decide_for_miss(allowed, env, "pkg").reason,
              RefreshReason::SuppressedDebounce);
}

TEST(PmIndexRefresh, SubjectEchoesWhatTheUserWrote) {
    FakeRegistry reg("subject");
    reg.mark_refreshed(std::chrono::hours{99});
    mcpp::pm::IndexMap indices;
    auto cfg = reg.config();
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", &cfg };

    // The manifest key `xim.nasm` parses into the candidate (mcpplibs.xim,
    // nasm); a diagnostic naming that spelling sends the reader hunting for a
    // namespace they never typed.
    auto spec = version_dep("mcpplibs.xim", "nasm", "2.16.03");
    auto d = mcpp::pm::decide_for_dependency(
        route, "xim.nasm", spec, reg.env(),
        mcpp::platform::TargetPlatform::for_os("linux"), {});
    EXPECT_EQ(d.subject, "xim.nasm@2.16.03");
}
