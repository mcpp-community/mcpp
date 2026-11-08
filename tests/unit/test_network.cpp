
import std;
import mcpplibs.mtest;
import mcpp.provider;
import mcpp.provider.xlings;
import mcpplibs.os.platform;
import mcpp.invocation;
import mcpp.run;

using namespace mcpplibs::mtest;

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

UTest offline_never_invokes_the_provider{"NetworkCheck/OfflineNeverInvokesTheProvider", [] {
    if constexpr (mcpplibs::os::is_windows) skip("POSIX subprocess fixture");
    NetworkFixture f(R"SH(touch "$XLINGS_HOME/invoked"
exit 1
)SH");
    {
        mcpp::invocation::ScopedEnv online("MCPP_OFFLINE", "0");
        expect(eq(mcpp::provider::xlings::network_check(f.env, std::chrono::seconds(1)).status, "unsupported"));
        expect(fatal(std::filesystem::exists(f.dir / "invoked")));
        std::filesystem::remove(f.dir / "invoked");
    }
    mcpp::invocation::ScopedEnv offline("MCPP_OFFLINE", "1");
    const auto result = mcpp::provider::xlings::network_check(f.env, std::chrono::milliseconds(100));
    expect(eq(result.status, "offline"));
    expect(!std::filesystem::exists(f.dir / "invoked"));
}};

UTest discovery_and_probes_share_one_deadline{"NetworkCheck/DiscoveryAndProbesShareOneDeadline", [] {
    if constexpr (mcpplibs::os::is_windows) skip("POSIX subprocess fixture");
    NetworkFixture f(R"SH(
if [ "$2" = "network_endpoints" ]; then
    echo '{"kind":"data","dataKind":"network_endpoints","payload":{"endpoints":[{"url":"https://one.invalid"},{"url":"https://two.invalid"},{"url":"https://three.invalid"},{"url":"https://four.invalid"},{"url":"https://five.invalid"}]}}'
else
    sleep 1000
fi
)SH");
    mcpp::invocation::ScopedEnv offline("MCPP_OFFLINE", "0");
    const auto start = std::chrono::steady_clock::now();
    const auto result = mcpp::provider::xlings::network_check(f.env, std::chrono::milliseconds(400));
    expect(lt(std::chrono::steady_clock::now() - start, std::chrono::seconds(2)));
    expect(fatal(eq(result.endpoints.size(), 5u)));
    for (const auto& e : result.endpoints) expect(eq(e.status, "timeout"));
}};

UTest malformed_provider_data_does_not_escape_diagnosis{"NetworkCheck/MalformedProviderDataDoesNotEscapeDiagnosis", [] {
    if constexpr (mcpplibs::os::is_windows) skip("POSIX subprocess fixture");
    NetworkFixture f(R"SH(
if [ "$2" = "network_endpoints" ]; then
    echo '{"kind":"data","dataKind":"network_endpoints","payload":{"endpoints":[{"url":"https://one.invalid"}]}}'
else
    echo '{"kind":"data","dataKind":"network_probe","payload":{"reachable":"wrong-type"}}'
fi
)SH");
    mcpp::invocation::ScopedEnv offline("MCPP_OFFLINE", "0");
    const auto result = mcpp::provider::xlings::network_check(f.env, std::chrono::seconds(1));
    expect(fatal(eq(result.endpoints.size(), 1u)));
    expect(eq(result.endpoints.front().status, "invalid_response"));
}};

UTest heartbeats_cannot_keep_a_byte_stalled_download_alive{"NetworkCheck/HeartbeatsCannotKeepAByteStalledDownloadAlive", [] {
    using namespace std::chrono;
    mcpp::provider::xlings::DownloadWatchdog watchdog(seconds(30));
    const auto start = steady_clock::now();
    mcpp::provider::BootstrapProgress p;
    p.files.push_back({.name = "tool", .started = true, .sourceAttempt = 1});
    watchdog.observe(p, start);
    watchdog.observe(p, start + seconds(29)); // repeated protocol sample
    expect(bool(watchdog.expired(start + seconds(30))));
    p.files.front().downloadedBytes = 1;
    watchdog.observe(p, start + seconds(30));
    expect(!(watchdog.expired(start + seconds(59))));
    p.files.front().sourceAttempt = 2;
    p.files.front().downloadedBytes = 0;
    watchdog.observe(p, start + seconds(60));
    expect(!(watchdog.expired(start + seconds(89))));
    p.files.front().finished = true;
    watchdog.observe(p, start + seconds(90));
    expect(!(watchdog.expired(start + hours(3))));
}};

// xlings leaves a source that sent no byte for kSourceIdleTimeout and reports
// the next one; mcpp's own bound is longer, so the next source is reached.
// With the two equal, a source that connected and then sent nothing stopped the
// install at attempt 1 (CI, 2026-11-02).
UTest a_silent_source_is_left_before_the_install_stops{"NetworkCheck/ASilentSourceIsLeftBeforeTheInstallStops", [] {
    using namespace std::chrono;
    namespace x = mcpp::provider::xlings;
    static_assert(x::kSourceConnectTimeout < x::kSourceIdleTimeout);
    static_assert(x::kSourceIdleTimeout < x::kDownloadStallTimeout);
    x::DownloadWatchdog watchdog;
    const auto start = steady_clock::now();
    const auto left = start + x::kSourceIdleTimeout + seconds(1);
    mcpp::provider::BootstrapProgress p;
    p.files.push_back({.name = "pkg", .started = true, .sourceAttempt = 1});
    watchdog.observe(p, start);
    expect(!watchdog.expired(left));
    p.files.front().sourceAttempt = 2;
    watchdog.observe(p, left);
    expect(!watchdog.expired(left + x::kSourceIdleTimeout));
    expect(bool(watchdog.expired(left + x::kDownloadStallTimeout)));
}};

UTest another_download_cannot_renew_a_stalled_files_budget{"NetworkCheck/AnotherDownloadCannotRenewAStalledFilesBudget", [] {
    using namespace std::chrono;
    namespace x = mcpp::provider::xlings;
    const auto start = steady_clock::now();
    x::InstallDeadline deadline(seconds(120), seconds(45), start);
    deadline.observe(R"({"kind":"data","dataKind":"download_progress","payload":{"stream":"install:1","files":[{"name":"stalled","started":true,"finished":false,"sourceAttempt":1,"downloadedBytes":0},{"name":"healthy","started":true,"finished":false,"sourceAttempt":1,"downloadedBytes":1}]}})", start);
    deadline.observe(R"({"kind":"data","dataKind":"download_progress","payload":{"stream":"install:1","files":[{"name":"stalled","started":true,"finished":false,"sourceAttempt":1,"downloadedBytes":0},{"name":"healthy","started":true,"finished":false,"sourceAttempt":1,"downloadedBytes":100}]}})", start + seconds(44));
    expect(!(deadline.expired(start + seconds(44))));
    expect(eq(deadline.failure_reason(start + seconds(45)), "download 'stalled' made no byte progress or source change for 45s (attempt 1)"));
    deadline.observe(R"({"kind":"data","dataKind":"download_progress","payload":{"stream":"install:1","files":[{"name":"stalled","started":true,"finished":false,"sourceAttempt":2,"downloadedBytes":0},{"name":"healthy","started":true,"finished":true,"sourceAttempt":1,"downloadedBytes":100}]}})", start + seconds(46));
    expect(deadline.failure_reason(start + seconds(90)).empty());
    expect(eq(deadline.failure_reason(start + seconds(91)), "download 'stalled' made no byte progress or source change for 45s (attempt 2)"));
    deadline.observe(R"({"kind":"data","dataKind":"download_progress","payload":{"stream":"install:1","files":[{"name":"stalled","started":true,"finished":true,"sourceAttempt":2,"downloadedBytes":1}]}})", start + seconds(92));
    expect(deadline.failure_reason(start + hours(1)).empty());
}};

UTest a_queued_files_budget_starts_when_it_becomes_active{"NetworkCheck/AQueuedFilesBudgetStartsWhenItBecomesActive", [] {
    using namespace std::chrono;
    namespace x = mcpp::provider::xlings;
    const auto start = steady_clock::now();
    x::DownloadWatchdog watchdog(seconds(45));
    mcpp::provider::BootstrapProgress progress;
    progress.files.push_back({.name = "queued", .started = false});
    watchdog.observe(progress, start);
    expect(!(watchdog.expired(start + seconds(96))));
    // Neither bytes nor sourceAttempt changes when the worker obtains a slot.
    progress.files.front().started = true;
    watchdog.observe(progress, start + seconds(96));
    expect(!(watchdog.expired(start + seconds(140))));
    expect(eq(watchdog.failure_reason(start + seconds(141)), "download 'queued' made no byte progress or source change for 45s (attempt 0)"));
    progress.files.front().sourceAttempt = 1;
    watchdog.observe(progress, start + seconds(142));
    expect(!(watchdog.expired(start + seconds(186))));
    progress.files.front().finished = true;
    watchdog.observe(progress, start + seconds(187));
    expect(!(watchdog.expired(start + hours(1))));
}};

UTest progress_cancellation_is_polled_while_output_continues{"NetworkProcess/ProgressCancellationIsPolledWhileOutputContinues", [] {
    using namespace std::chrono;
    const auto start = steady_clock::now();
    const auto cmd = mcpplibs::os::is_windows
        ? "for /L %i in (1,1,10000000) do @echo heartbeat"
        : "while :; do echo heartbeat; done";
    int lines = 0;
    bool stopped = false;
    const auto sh = mcpp::run::shell(cmd);
    stopped = mcpp::run::execute(sh.argv, {
        .total = seconds(5), .idle = seconds(2),
        .on_line = [&](std::string_view) { ++lines; },
        .should_stop = [&] { return steady_clock::now() - start >= milliseconds(200); },
        .one_string = sh.one_string }).timed_out;
    expect(bool(stopped));
    expect(gt(lines, 0));
    expect(lt(steady_clock::now() - start, seconds(3)));
}};

UTest index_preparation_is_bounded_before_the_first_download_event{"NetworkCheck/IndexPreparationIsBoundedBeforeTheFirstDownloadEvent", [] {
    using namespace std::chrono;
    const auto start = steady_clock::now();
    mcpp::provider::xlings::InstallDeadline deadline(seconds(120), seconds(30), start);
    deadline.observe(R"({"kind":"heartbeat","ts":100})", start + seconds(119));
    expect(bool(deadline.expired(start + seconds(120))));
    deadline.observe(R"({"kind":"progress","phase":"extract","percent":0})", start + seconds(119));
    expect(!(deadline.expired(start + minutes(10))));
}};

UTest legacy_status_and_handshake_do_not_write_the_selected_home{"NetworkCheck/LegacyStatusAndHandshakeDoNotWriteTheSelectedHome", [] {
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
    const auto binary = dir / (mcpplibs::os::is_windows ? "provider.cmd" : "provider");
    {
        std::ofstream script(binary, std::ios::binary);
        if constexpr (mcpplibs::os::is_windows)
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
    expect(fatal(bool(metadata.answered))) << "the legacy handshake fixture never ran";
    expect(eq(metadata.version, "9999.1.1.1"));
    for (const auto* capability : {"system_status", "payload_status"}) {
        const auto result = mcpp::provider::xlings::call(env, capability, "{}");
        expect(fatal(!result));
        expect(ne(result.error().find("read_only_queries"), std::string::npos));
    }
    mcpp::invocation::ScopedEnv online("MCPP_OFFLINE", "0");
    expect(eq(mcpp::provider::xlings::network_check(env, std::chrono::seconds(1)).status, "unsupported"));
    expect(std::filesystem::is_empty(home)) << "legacy metadata or status touched the selected home";
    expect(std::filesystem::is_empty(project)) << "legacy metadata touched the current project";
}};

UTest bootstrap_requires_download_budgets_and_read_only_queries{"NetworkCheck/BootstrapRequiresDownloadBudgetsAndReadOnlyQueries", [] {
    mcpp::provider::ProviderInfo info;
    info.answered = true;
    info.version = "9999.1.1.1";
    info.capabilities = {"install_packages"};
    info.features = {"index_dir", "index_store", "upstreams", "payload_status", "sync_index",
                     "payload_currency", "binding_state"};
    for (const auto* feature : {"network_diagnostics", "download_socket_budgets", "read_only_queries"}) {
        expect(ne(mcpp::provider::xlings::unmet(info).find(feature), std::string::npos));
        info.features.push_back(feature);
    }
    expect(mcpp::provider::xlings::unmet(info).empty());
}};


UTest metadata_and_resource_deadlines_stop_descendants{"NetworkProcess/MetadataAndResourceDeadlinesStopDescendants", [] {
    if constexpr (mcpplibs::os::is_windows) skip("POSIX subprocess fixture");
    const std::string writer = R"SH(
(file="${0%/*}/ticks"; i=0
while [ "$i" -lt 50 ]; do
    printf x >> "$file"
    sleep 0.05
    i=$((i+1))
done) >/dev/null 2>&1 &
wait
)SH";
    mcpp::invocation::ScopedEnv online("MCPP_OFFLINE", "0");
    for (bool metadata : {true, false}) {
        NetworkFixture f("if [ \"$2\" = \"network_endpoints\" ]; then\n"
            "echo '{\"kind\":\"data\",\"dataKind\":\"network_endpoints\",\"payload\":{\"endpoints\":[{\"url\":\"https://one.invalid\"}]}}'\n"
            "else\n" + writer + "fi\n");
        if (metadata) std::ofstream(f.env.binary) << "#!/bin/sh\n" << writer;
        const auto start = std::chrono::steady_clock::now();
        if (metadata)
            expect(!(mcpp::provider::xlings::describe(f.env.binary, std::chrono::milliseconds(500)).answered));
        else {
            const auto report = mcpp::provider::xlings::network_check(f.env, std::chrono::milliseconds(500));
            expect(fatal(eq(report.endpoints.size(), 1u)));
            expect(eq(report.endpoints.front().status, "timeout"));
        }
        expect(lt(std::chrono::steady_clock::now() - start, std::chrono::seconds(2)));
        const auto ticks = f.dir / "ticks";
        expect(fatal(std::filesystem::exists(ticks))) << "the descendant fixture never ran";
        const auto before = std::filesystem::file_size(ticks);
        expect(fatal(gt(before, 0u)));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        expect(eq(std::filesystem::file_size(ticks), before))
            << (metadata ? "metadata" : "resource") << " deadline left a descendant writing";
    }
}};
