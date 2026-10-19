#include <gtest/gtest.h>

import std;
import mcpp.provider;
import mcpp.provider.xlings;
import mcpp.platform;

namespace {
struct NetworkFixture {
    std::filesystem::path dir = std::filesystem::temp_directory_path()
        / std::format("mcpp-network-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    mcpp::provider::Location env;
    NetworkFixture(std::string_view script) {
        std::filesystem::create_directories(dir);
        env = {dir / "provider", dir, {}};
        std::ofstream(env.binary) << R"SH(#!/bin/sh
touch "$XLINGS_HOME/invoked"
if [ "$2" = "--version" ]; then
    echo '{"version":"9999.1.1.1","features":["network_diagnostics","read_only_queries"]}'
    exit 0
fi
)SH" << script;
        std::filesystem::permissions(env.binary, std::filesystem::perms::owner_all);
    }
    ~NetworkFixture() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
};
}

TEST(NetworkCheck, OfflineNeverInvokesTheProvider) {
    if constexpr (mcpp::platform::is_windows) GTEST_SKIP() << "POSIX subprocess fixture";
    NetworkFixture f(R"SH(touch "$XLINGS_HOME/invoked"
exit 1
)SH");
    {
        mcpp::platform::env::ScopedEnv online("MCPP_OFFLINE", "0");
        EXPECT_EQ(mcpp::provider::xlings::network_check(f.env, std::chrono::seconds(1)).status, "unsupported");
        ASSERT_TRUE(std::filesystem::exists(f.dir / "invoked"));
        std::filesystem::remove(f.dir / "invoked");
    }
    mcpp::platform::env::ScopedEnv offline("MCPP_OFFLINE", "1");
    const auto result = mcpp::provider::xlings::network_check(f.env, std::chrono::milliseconds(100));
    EXPECT_EQ(result.status, "offline");
    EXPECT_FALSE(std::filesystem::exists(f.dir / "invoked"));
}

TEST(NetworkCheck, DiscoveryAndProbesShareOneDeadline) {
    if constexpr (mcpp::platform::is_windows) GTEST_SKIP() << "POSIX subprocess fixture";
    NetworkFixture f(R"SH(
if [ "$2" = "network_endpoints" ]; then
    echo '{"kind":"data","dataKind":"network_endpoints","payload":{"endpoints":[{"url":"https://one.invalid"},{"url":"https://two.invalid"},{"url":"https://three.invalid"},{"url":"https://four.invalid"},{"url":"https://five.invalid"}]}}'
else
    sleep 1000
fi
)SH");
    mcpp::platform::env::ScopedEnv offline("MCPP_OFFLINE", "0");
    const auto start = std::chrono::steady_clock::now();
    const auto result = mcpp::provider::xlings::network_check(f.env, std::chrono::milliseconds(400));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
    ASSERT_EQ(result.endpoints.size(), 5u);
    for (const auto& e : result.endpoints) EXPECT_EQ(e.status, "timeout");
}

TEST(NetworkCheck, MalformedProviderDataDoesNotEscapeDiagnosis) {
    if constexpr (mcpp::platform::is_windows) GTEST_SKIP() << "POSIX subprocess fixture";
    NetworkFixture f(R"SH(
if [ "$2" = "network_endpoints" ]; then
    echo '{"kind":"data","dataKind":"network_endpoints","payload":{"endpoints":[{"url":"https://one.invalid"}]}}'
else
    echo '{"kind":"data","dataKind":"network_probe","payload":{"reachable":"wrong-type"}}'
fi
)SH");
    mcpp::platform::env::ScopedEnv offline("MCPP_OFFLINE", "0");
    const auto result = mcpp::provider::xlings::network_check(f.env, std::chrono::seconds(1));
    ASSERT_EQ(result.endpoints.size(), 1u);
    EXPECT_EQ(result.endpoints.front().status, "invalid_response");
}

TEST(NetworkCheck, HeartbeatsCannotKeepAByteStalledDownloadAlive) {
    using namespace std::chrono;
    mcpp::provider::xlings::DownloadWatchdog watchdog(seconds(30));
    const auto start = steady_clock::now();
    mcpp::provider::BootstrapProgress p;
    p.files.push_back({.name = "tool", .started = true, .sourceAttempt = 1});
    watchdog.observe(p, start);
    watchdog.observe(p, start + seconds(29)); // repeated protocol sample
    EXPECT_TRUE(watchdog.expired(start + seconds(30)));
    p.files.front().downloadedBytes = 1;
    watchdog.observe(p, start + seconds(30));
    EXPECT_FALSE(watchdog.expired(start + seconds(59)));
    p.files.front().sourceAttempt = 2;
    p.files.front().downloadedBytes = 0;
    watchdog.observe(p, start + seconds(60));
    EXPECT_FALSE(watchdog.expired(start + seconds(89)));
    p.files.front().finished = true;
    watchdog.observe(p, start + seconds(90));
    EXPECT_FALSE(watchdog.expired(start + hours(3)));
}

TEST(NetworkProcess, ProgressCancellationIsPolledWhileOutputContinues) {
    using namespace std::chrono;
    const auto start = steady_clock::now();
    const auto cmd = mcpp::platform::is_windows
        ? "for /L %i in (1,1,10000000) do @echo heartbeat"
        : "while :; do echo heartbeat; done";
    int lines = 0;
    bool stopped = false;
    mcpp::platform::process::run_streaming_bounded(cmd,
        [&](std::string_view) { ++lines; }, seconds(5), seconds(2), &stopped, false,
        [&] { return steady_clock::now() - start >= milliseconds(200); });
    EXPECT_TRUE(stopped);
    EXPECT_GT(lines, 0);
    EXPECT_LT(steady_clock::now() - start, seconds(3));
}

TEST(NetworkCheck, IndexPreparationIsBoundedBeforeTheFirstDownloadEvent) {
    using namespace std::chrono;
    const auto start = steady_clock::now();
    mcpp::provider::xlings::InstallDeadline deadline(seconds(120), seconds(30), start);
    deadline.observe(R"({"kind":"heartbeat","ts":100})", start + seconds(119));
    EXPECT_TRUE(deadline.expired(start + seconds(120)));
    deadline.observe(R"({"kind":"progress","phase":"extract","percent":0})", start + seconds(119));
    EXPECT_FALSE(deadline.expired(start + minutes(10)));
}

TEST(NetworkCheck, LegacyStatusAndHandshakeDoNotWriteTheSelectedHome) {
    const auto dir = std::filesystem::temp_directory_path()
        / std::format("mcpp-legacy-probe-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    struct Cleanup {
        std::filesystem::path dir, previousCwd;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::current_path(previousCwd, ec);
            std::filesystem::remove_all(dir, ec);
        }
    } cleanup{dir, std::filesystem::current_path()};
    const auto home = dir / "home";
    std::filesystem::create_directories(home);
    const auto binary = dir / (mcpp::platform::is_windows ? "provider.cmd" : "provider");
    {
        std::ofstream script(binary, std::ios::binary);
        if constexpr (mcpp::platform::is_windows)
            script << "@echo off\r\necho invoked>>\"%XLINGS_HOME%\\invoked\"\r\necho invoked>metadata-cwd\r\n"
                      "echo {\"version\":\"9999.1.1.1\",\"features\":[]}\r\n";
        else
            script << "#!/bin/sh\necho invoked >> \"$XLINGS_HOME/invoked\"\necho invoked > metadata-cwd\n"
                      "echo '{\"version\":\"9999.1.1.1\",\"features\":[]}'\n";
    }
    std::filesystem::permissions(binary, std::filesystem::perms::owner_all);
    const auto project = dir / "project";
    std::filesystem::create_directories(project);
    std::filesystem::current_path(project);
    mcpp::provider::Location env{binary, home, {}};
    const auto metadata = mcpp::provider::xlings::describe(std::filesystem::relative(binary, project));
    ASSERT_TRUE(metadata.answered) << "the legacy handshake fixture never ran";
    EXPECT_EQ(metadata.version, "9999.1.1.1");
    for (const auto* capability : {"system_status", "payload_status"}) {
        const auto result = mcpp::provider::xlings::call(env, capability, "{}");
        ASSERT_FALSE(result);
        EXPECT_NE(result.error().find("read_only_queries"), std::string::npos);
    }
    mcpp::platform::env::ScopedEnv online("MCPP_OFFLINE", "0");
    EXPECT_EQ(mcpp::provider::xlings::network_check(env, std::chrono::seconds(1)).status, "unsupported");
    EXPECT_TRUE(std::filesystem::is_empty(home)) << "legacy metadata or status touched the selected home";
    EXPECT_TRUE(std::filesystem::is_empty(project)) << "legacy metadata touched the current project";
}

TEST(NetworkCheck, BootstrapRequiresDownloadBudgetsAndReadOnlyQueries) {
    mcpp::provider::ProviderInfo info;
    info.answered = true;
    info.version = "9999.1.1.1";
    info.capabilities = {"install_packages"};
    info.features = {"index_dir", "index_store", "upstreams", "payload_status", "sync_index",
                     "payload_currency", "binding_state"};
    for (const auto* feature : {"network_diagnostics", "download_socket_budgets", "read_only_queries"}) {
        EXPECT_NE(mcpp::provider::xlings::unmet(info).find(feature), std::string::npos);
        info.features.push_back(feature);
    }
    EXPECT_TRUE(mcpp::provider::xlings::unmet(info).empty());
}


TEST(NetworkProcess, MetadataAndResourceDeadlinesStopDescendants) {
    if constexpr (mcpp::platform::is_windows) GTEST_SKIP() << "POSIX subprocess fixture";
    const std::string writer = R"SH(
(file="${0%/*}/ticks"; i=0
while [ "$i" -lt 50 ]; do
    printf x >> "$file"
    sleep 0.05
    i=$((i+1))
done) >/dev/null 2>&1 &
wait
)SH";
    mcpp::platform::env::ScopedEnv online("MCPP_OFFLINE", "0");
    for (bool metadata : {true, false}) {
        NetworkFixture f("if [ \"$2\" = \"network_endpoints\" ]; then\n"
            "echo '{\"kind\":\"data\",\"dataKind\":\"network_endpoints\",\"payload\":{\"endpoints\":[{\"url\":\"https://one.invalid\"}]}}'\n"
            "else\n" + writer + "fi\n");
        if (metadata) std::ofstream(f.env.binary) << "#!/bin/sh\n" << writer;
        const auto start = std::chrono::steady_clock::now();
        if (metadata)
            EXPECT_FALSE(mcpp::provider::xlings::describe(f.env.binary, std::chrono::milliseconds(500)).answered);
        else {
            const auto report = mcpp::provider::xlings::network_check(f.env, std::chrono::milliseconds(500));
            ASSERT_EQ(report.endpoints.size(), 1u);
            EXPECT_EQ(report.endpoints.front().status, "timeout");
        }
        EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
        const auto ticks = f.dir / "ticks";
        ASSERT_TRUE(std::filesystem::exists(ticks)) << "the descendant fixture never ran";
        const auto before = std::filesystem::file_size(ticks);
        ASSERT_GT(before, 0u);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        EXPECT_EQ(std::filesystem::file_size(ticks), before)
            << (metadata ? "metadata" : "resource") << " deadline left a descendant writing";
    }
}
