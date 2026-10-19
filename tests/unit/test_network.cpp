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
        std::ofstream(env.binary) << "#!/bin/sh\n" << script;
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
