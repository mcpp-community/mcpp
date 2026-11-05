#include <gtest/gtest.h>
#include <cerrno>
#include <cstdio>
#include <fstream>

import std;
import mcpp.run;
import mcpp.invocation;

namespace process = mcpp::run;

// These exercise the POSIX launch path with POSIX program paths (/bin/true,
// /bin/sh, /bin/echo) — Linux and macOS share the direct posix_spawn path
// (#248 launcher-unify): extraEnv goes into the child's envp only, so the
// parent environment is never mutated. The Windows path is covered by the
// integration build (ninja launched via capture_exec).
#if !defined(_WIN32)

// The regression that matters: launching with an injected loader var must NOT
// change mcpp's own environment — that mutation is exactly what leaked into
// /bin/sh and crashed it on newer-glibc hosts. The addition is the started
// program's alone; this invocation's statement is what it was.
TEST(RunExec, OutputProducer) {
    if (mcpp::invocation::get("MCPP_TEST_OUTPUT_PRODUCER") != "1")
        GTEST_SKIP() << "subprocess producer";
    std::fputs("mcpp-stderr-ready\n", stdout);
    std::fflush(stdout);
    const bool error = mcpp::invocation::get("MCPP_TEST_OUTPUT_STREAM") != "stdout";
    if (error) std::fclose(stdout);
    FILE* stream = error ? stderr : stdout;
    const int count = std::stoi(mcpp::invocation::get("MCPP_TEST_OUTPUT_TICKS").value_or("24"));
    auto next = std::chrono::steady_clock::now();
    for (int i = 0; i < count; ++i) {
        const auto tick = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::fprintf(stream, "%lld\n", static_cast<long long>(tick));
        std::fflush(stream);
        next += std::chrono::milliseconds(30);
        std::this_thread::sleep_until(next);
    }
    std::_Exit(7);
}

TEST(RunExec, QueuedOutputRenewsIdleAfterALineHandler) {
    std::vector<long long> ticks;
    const auto result = process::execute({mcpp::invocation::self_exe_path().string(),
        "--gtest_filter=RunExec.OutputProducer"}, {
        .env = {{"MCPP_TEST_OUTPUT_PRODUCER", "1"}, {"MCPP_TEST_OUTPUT_STREAM", "stdout"}, {"MCPP_TEST_OUTPUT_TICKS", "48"}},
        .streams = process::Streams::merged,
        .total = std::chrono::seconds(3), .idle = std::chrono::milliseconds(500),
        .on_line = [&](std::string_view line) {
            if (line == "mcpp-stderr-ready")
                std::this_thread::sleep_for(std::chrono::milliseconds(750));
            long long tick;
            if (std::from_chars(line.data(), line.data() + line.size(), tick).ec == std::errc{})
                ticks.push_back(tick);
        }});
    EXPECT_EQ(result.exit_code, 7);
    EXPECT_FALSE(result.timed_out);
    EXPECT_EQ(ticks.size(), 48u);
}

TEST(RunExec, QueuedErrorOutputRenewsIdleAfterALineHandler) {
    const auto result = process::execute({mcpp::invocation::self_exe_path().string(),
        "--gtest_filter=RunExec.OutputProducer"}, {
        .env = {{"MCPP_TEST_OUTPUT_PRODUCER", "1"}, {"MCPP_TEST_OUTPUT_TICKS", "48"}},
        .streams = process::Streams::output_error_kept,
        .total = std::chrono::seconds(3), .idle = std::chrono::milliseconds(500),
        .on_line = [](std::string_view line) {
            if (line == "mcpp-stderr-ready")
                std::this_thread::sleep_for(std::chrono::milliseconds(750));
        }});
    EXPECT_EQ(result.exit_code, 7) << result.error_output;
    EXPECT_FALSE(result.timed_out);
    std::istringstream input(result.error_output);
    int ticks = 0;
    for (long long tick; input >> tick;) ++ticks;
    EXPECT_EQ(ticks, 48);
}

TEST(RunExec, StreamCompletionAndChildWaitKeepTheirBounds) {
    struct Case {
        const char* name;
        const char* command;
        int total = 0;
        int idle = 0;
        bool stop = false;
        int exit = -1;
    };
    const std::vector<Case> cases{
        {"exit", "exit 7", 0, 0, false, 7},
        {"signal", "kill -TERM $$", 0, 0, false, 143},
        {"already-exited-bounded", "exit 7", 3000, 0, true, 7},
        {"open-stop", "sleep 2", 0, 0, true},
        {"both-EOF-stop", "exec 1>&- 2>&-; sleep 2", 0, 0, true},
        {"stdout-EOF-stop", "exec 1>&-; sleep 2", 0, 0, true},
        {"both-EOF-total", "exec 1>&- 2>&-; sleep 2", 50},
        {"stdout-EOF-total", "exec 1>&-; sleep 2", 50},
    };
    auto exercise = [](process::Streams streams, const Case& test) {
        SCOPED_TRACE(std::format("stream={} case={}", static_cast<int>(streams), test.name));
        const auto started = std::chrono::steady_clock::now();
        process::Options options;
        options.streams = streams;
        options.total = std::chrono::milliseconds(test.total);
        options.idle = std::chrono::milliseconds(test.idle);
        if (test.stop) options.should_stop = [&] {
            return std::chrono::steady_clock::now() - started > std::chrono::milliseconds(50);
        };
        const bool producer = std::string_view(test.command) == "@stderr-producer";
        if (producer) options.env.emplace_back("MCPP_TEST_OUTPUT_PRODUCER", "1");
        const std::vector<std::string> argv = producer
            ? std::vector<std::string>{mcpp::invocation::self_exe_path().string(),
                                      "--gtest_filter=RunExec.OutputProducer"}
            : std::vector<std::string>{"/bin/sh", "-c", test.command};
        const auto result = process::execute(argv, options);
        if (test.exit >= 0) {
            EXPECT_EQ(result.exit_code, test.exit);
            EXPECT_FALSE(result.timed_out);
        } else {
            EXPECT_NE(result.exit_code, 0);
            EXPECT_TRUE(result.timed_out);
        }
        const auto elapsed = std::chrono::steady_clock::now() - started;
        if (producer) {
            // One process emits across the idle window. Forking an external
            // sleep for every tick would add platform-dependent launch gaps.
            SCOPED_TRACE(result.error_output);
            std::istringstream input(result.error_output);
            std::vector<long long> ticks;
            for (long long tick; input >> tick;) ticks.push_back(tick);
            ASSERT_EQ(ticks.size(), 24u);
            for (std::size_t i = 1; i < ticks.size(); ++i)
                EXPECT_LT(std::chrono::nanoseconds(ticks[i] - ticks[i - 1]), options.idle);
            EXPECT_GT(elapsed, options.idle);
            EXPECT_LT(elapsed, std::chrono::seconds(3));
        } else {
            EXPECT_LT(elapsed, std::chrono::seconds(1));
        }
    };
    for (const auto streams : {process::Streams::inherit, process::Streams::discard,
            process::Streams::merged, process::Streams::output,
            process::Streams::output_error_shown, process::Streams::output_error_kept}) {
        for (const auto& test : cases) exercise(streams, test);
        if (streams != process::Streams::inherit && streams != process::Streams::discard)
            exercise(streams, {"both-EOF-idle", "exec 1>&- 2>&-; sleep 2", 0, 50});
    }
    exercise(process::Streams::output_error_kept,
        {"stderr-activity", "@stderr-producer", 3000, 500, false, 7});
}

TEST(RunExec, StopPredicateAppliesAfterCapturedStreamsClose) {
    const auto started = std::chrono::steady_clock::now();
    const auto result = process::execute({"/bin/sh", "-c", "exec 1>&- 2>&-; sleep 2"}, {
        .streams = process::Streams::merged,
        .should_stop = [&] {
            return std::chrono::steady_clock::now() - started > std::chrono::milliseconds(50);
        }});
    EXPECT_TRUE(result.timed_out);
    EXPECT_NE(result.exit_code, 0);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
}

TEST(RunExec, StopPredicateAppliesWithoutCapturedStreams) {
    for (const auto streams : {process::Streams::inherit, process::Streams::discard}) {
        const auto started = std::chrono::steady_clock::now();
        int checks = 0;
        const auto result = process::execute({"/bin/sh", "-c", "sleep 60"}, {
            .streams = streams, .total = std::chrono::seconds(3),
            .should_stop = [&] {
                ++checks;
                return std::chrono::steady_clock::now() - started > std::chrono::milliseconds(50);
            }});
        EXPECT_GT(checks, 0);
        EXPECT_TRUE(result.timed_out);
        EXPECT_NE(result.exit_code, 0);
        EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
    }
}

TEST(RunExec, DoesNotMutateParentEnvironment) {
    mcpp::invocation::ScopedEnv leak("MCPP_TEST_LEAK", "sentinel");
    // Use /bin/sh (present on Linux AND macOS) — /bin/true lives at
    // /usr/bin/true on macOS, so it is not a portable launch target.
    int rc = process::run_exec({"/bin/sh", "-c", "[ \"$MCPP_TEST_LEAK\" = injected ]"},
                               {{"MCPP_TEST_LEAK", "injected"}});
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(mcpp::invocation::get("MCPP_TEST_LEAK"), "sentinel");   // unchanged → no leak
}

TEST(RunExec, ChildSeesInjectedEnv) {
    // `sh -c '[ "$X" = injected ]'` exits 0 only if the child received X.
    int rc = process::run_exec(
        {"/bin/sh", "-c", "[ \"$MCPP_TEST_INJECT\" = injected ]"},
        {{"MCPP_TEST_INJECT", "injected"}});
    EXPECT_EQ(rc, 0);
}

TEST(RunExec, PropagatesChildExitCode) {
    EXPECT_EQ(process::run_exec({"/bin/sh", "-c", "exit 7"}), 7);
}

TEST(RunExec, ReturnsErrorWhenProgramMissing) {
    EXPECT_NE(process::run_exec({"/no/such/program/mcpp-xyz"}), 0);
}

TEST(CaptureExec, MissingProgramReportsSpawnFailure) {
    constexpr std::string_view missing = "/no/such/program/mcpp-capture-xyz";
    auto r = process::capture_exec({std::string(missing)});
    EXPECT_EQ(r.exit_code, 127);
    EXPECT_NE(r.output.find(missing), std::string::npos) << r.output;
    EXPECT_NE(r.output.find("error 2"), std::string::npos) << r.output;
}

TEST(CaptureExec, CapturesStdoutWithoutShell) {
    auto r = process::capture_exec({"/bin/echo", "hello world"});
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.output, "hello world\n");
}

TEST(CaptureExec, CapturesStderrCombined) {
    // stderr must be merged into output (replaces the old `2>&1`), otherwise
    // ninja's error text would be lost from the fast-path stale detector.
    auto r = process::capture_exec({"/bin/sh", "-c", "echo oops 1>&2"});
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.output, "oops\n");
}

// #248 regression: extraEnv AND cwd BOTH non-empty — the previously untested
// combination (every prior call site passed at most one of the two; the only
// env+cwd caller is the build.mcpp child). The old macOS shell fallback built
// `KEY='v' cd <cwd> && prog 2>&1`, and POSIX binds the env assignments to the
// first command (`cd`) across `&&` — so the real program received NO extra env
// and build.mcpp lost the whole MCPP_* contract. Linux and macOS now share the
// posix_spawn path, so this locks the contract on both.
TEST(CaptureExec, EnvAndCwdCombinedBothReachChild) {
    auto dir = std::filesystem::temp_directory_path() / "mcpp_248_env_cwd";
    std::filesystem::create_directories(dir);
    auto r = process::capture_exec(
        {"/bin/sh", "-c", "printf '%s|%s' \"$MCPP_TEST_CONTRACT\" \"$PWD\""},
        {{"MCPP_TEST_CONTRACT", "hello"}},
        dir.string());
    EXPECT_EQ(r.exit_code, 0);
    // Env value must reach the child (the old prefix bound it to `cd` only).
    EXPECT_NE(r.output.find("hello|"), std::string::npos) << r.output;
    // Child must actually run in `cwd`. Compare the leaf name, not the full
    // path: macOS resolves /var → /private/var in $PWD.
    EXPECT_NE(r.output.find("mcpp_248_env_cwd"), std::string::npos) << r.output;
}

// Companion check for run_exec (no cwd parameter there, but extraEnv must
// still land in the child via the same merged_environ posix_spawn path).
TEST(RunExec, InjectedEnvSurvivesShellChildChain) {
    int rc = process::run_exec(
        {"/bin/sh", "-c", "[ \"$MCPP_TEST_CONTRACT\" = hello ]"},
        {{"MCPP_TEST_CONTRACT", "hello"}});
    EXPECT_EQ(rc, 0);
}

// ── spawn failures are typed, reported once, and never retried (#544) ──────
//
// The invariant the bounded launcher's DeadlineRun comment states ("could not
// spawn" and "ran and failed" must not share an exit code) was defeated one
// layer down: run_exec turned every posix_spawnp failure into a bare 127 with
// nothing printed, and both deadline wrappers fell back to it, spawning a
// second time and discarding the first errno. `mcpp run --target
// aarch64-linux-musl` on a host without an emulator printed "Running …", a
// blank line, and exited 1.

TEST(RunExec, MissingProgramReportsOnStderrWhenCallerDoesNotAsk) {
    testing::internal::CaptureStderr();
    int rc = process::run_exec({"/no/such/program/mcpp-run-xyz"});
    auto err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, 127);
    EXPECT_NE(err.find("/no/such/program/mcpp-run-xyz"), std::string::npos) << err;
    EXPECT_NE(err.find("error 2"), std::string::npos) << err;
}

TEST(RunExec, MissingProgramTypesErrnoWhenCallerAsks) {
    int spawnErr = 0;
    testing::internal::CaptureStderr();
    int rc = process::run_exec({"/no/such/program/mcpp-run-xyz"}, {}, &spawnErr);
    auto err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, 127);
    EXPECT_EQ(spawnErr, ENOENT);
    EXPECT_TRUE(err.empty()) << err;   // the caller owns the report
}

#if defined(__linux__)
// Linux only, and that is a MEASUREMENT (CI, macOS 14 ARM64, 2026-09-02): on
// macOS posix_spawnp handles ENOEXEC the way execvp does — it runs the file
// through /bin/sh — so the child IS spawned (spawn_error stays 0) and it is the
// shell that exits non-zero. The kernel's own refusal of a wrong-architecture
// Mach-O (EBADARCH) is a different reading and is not what this file
// synthesises. `mcpp run`'s "this host cannot execute" typing therefore fires
// on Linux for any unloadable file and on macOS only for a real foreign
// binary; see the design's open question 1.
TEST(RunExec, UnloadableArtifactIsENOEXEC) {
    // An executable file with no loader magic: the kernel refuses it with
    // ENOEXEC. glibc's posix_spawnp does not retry through /bin/sh the way
    // execvp does, so the errno reaches the caller untouched. This is the
    // reading `mcpp run` turns into "this host cannot execute the artifact".
    auto dir = std::filesystem::temp_directory_path() / "mcpp-enoexec-test";
    std::filesystem::create_directories(dir);
    auto f = dir / "not-an-elf";
    { std::ofstream o(f, std::ios::binary); o << "\x7f" "NOT" "\x00\x00\x00\x00"; }
    std::filesystem::permissions(f, std::filesystem::perms::owner_all);
    int spawnErr = 0;
    int rc = process::run_exec({f.string()}, {}, &spawnErr);
    EXPECT_EQ(rc, 127);
    EXPECT_EQ(spawnErr, ENOEXEC);
}
#endif

TEST(RunExec, SpawnedChildLeavesSpawnErrorZero) {
    int spawnErr = -1;
    EXPECT_EQ(process::run_exec({"/bin/sh", "-c", "exit 7"}, {}, &spawnErr), 7);
    EXPECT_EQ(spawnErr, 0);
}

TEST(CaptureExec, MissingProgramTypesErrnoWhenCallerAsks) {
    int spawnErr = 0;
    auto r = process::capture_exec({"/no/such/program/mcpp-capture-typed"}, {}, {}, &spawnErr);
    EXPECT_EQ(r.exit_code, 127);
    EXPECT_EQ(spawnErr, ENOENT);
    EXPECT_TRUE(r.output.empty()) << r.output;
}

TEST(RunExecDeadline, SpawnFailureIsTypedAndNotRetried) {
    int spawnErr = 0;
    bool timedOut = true;
    testing::internal::CaptureStderr();
    int rc = process::run_exec_deadline({"/no/such/program/mcpp-dl-xyz"}, {},
                                        std::chrono::milliseconds(5000), &timedOut,
                                        &spawnErr);
    auto err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, 127);
    EXPECT_EQ(spawnErr, ENOENT);
    EXPECT_FALSE(timedOut);
    EXPECT_TRUE(err.empty()) << err;
}

TEST(RunExecDeadline, SpawnFailureIsReportedWhenCallerDoesNotAsk) {
    bool timedOut = true;
    testing::internal::CaptureStderr();
    int rc = process::run_exec_deadline({"/no/such/program/mcpp-dl-untyped"}, {},
                                        std::chrono::milliseconds(5000), &timedOut);
    auto err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, 127);
    EXPECT_FALSE(timedOut);
    EXPECT_NE(err.find("mcpp-dl-untyped"), std::string::npos) << err;
    EXPECT_NE(err.find("error 2"), std::string::npos) << err;
}

TEST(CaptureExecDeadline, SpawnFailureIsTypedInOutcome) {
    int spawnErr = 0;
    bool timedOut = true;
    auto r = process::capture_exec_deadline({"/no/such/program/mcpp-cdl-xyz"}, {},
                                            std::chrono::milliseconds(5000), &timedOut,
                                            {}, &spawnErr);
    EXPECT_EQ(r.exit_code, 127);
    EXPECT_EQ(spawnErr, ENOENT);
    EXPECT_FALSE(timedOut);
    EXPECT_TRUE(r.output.empty()) << r.output;
}

TEST(CaptureExecDeadline, SpawnFailureIsInOutputWhenCallerDoesNotAsk) {
    bool timedOut = true;
    auto r = process::capture_exec_deadline({"/no/such/program/mcpp-cdl-untyped"}, {},
                                            std::chrono::milliseconds(5000), &timedOut);
    EXPECT_EQ(r.exit_code, 127);
    EXPECT_NE(r.output.find("mcpp-cdl-untyped"), std::string::npos) << r.output;
    EXPECT_NE(r.output.find("error 2"), std::string::npos) << r.output;
}

// ── capture_stdout: the argument-vector form of `<program> 2>/dev/null` ──

TEST(CaptureStdout, CapturesStandardOutputOnly) {
    auto r = process::capture_stdout(
        {"/bin/sh", "-c", "echo out; echo 9.9.9-on-stderr 1>&2"});
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.output, "out\n");
}

TEST(CaptureStdout, PropagatesExitCode) {
    auto r = process::capture_stdout({"/bin/sh", "-c", "echo partial; exit 3"});
    EXPECT_EQ(r.exit_code, 3);
    EXPECT_EQ(r.output, "partial\n");
}

// A probe parses what it captured, so a launcher message must never be read
// as the program's answer: a missing program leaves the output empty.
TEST(CaptureStdout, MissingProgramLeavesOutputEmpty) {
    int spawnErr = 0;
    auto r = process::capture_stdout({"/no/such/program/mcpp-probe-2026.1.2.3"},
                                     {}, &spawnErr);
    EXPECT_EQ(r.exit_code, 127);
    EXPECT_EQ(spawnErr, ENOENT);
    EXPECT_TRUE(r.output.empty()) << r.output;
}

TEST(CaptureStdout, StandardInputIsEmpty) {
    // `cat` returns at once with nothing when stdin is /dev/null; with an
    // inherited terminal or pipe it would block or echo the parent's input.
    auto r = process::capture_stdout({"/bin/sh", "-c", "cat; echo done"});
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.output, "done\n");
}

TEST(CaptureStdout, ExtraEnvironmentReachesTheChild) {
    auto r = process::capture_stdout({"/bin/sh", "-c", "printf %s \"$MCPP_PROBE_ENV\""},
                                     {{"MCPP_PROBE_ENV", "reached"}});
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.output, "reached");
}

#else  // _WIN32

TEST(RunExec, WindowsCoveredByIntegration) {
    // The Windows path uses _spawnvpe (no /bin/* programs to point at here);
    // it is exercised by the integration build that launches ninja.
    SUCCEED();
}

#endif

