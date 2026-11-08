#include <cerrno>
#include <cstdio>
#include <fstream>

import std;
import mcpplibs.mtest;
import mcpplibs.os.stdio;
import mcpp.run;
import mcpp.invocation;

using namespace mcpplibs::mtest;

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
UTest output_producer{"RunExec/OutputProducer", [] {
    if (mcpp::invocation::get("MCPP_TEST_OUTPUT_PRODUCER") != "1")
        skip("subprocess producer");
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
}};

UTest queued_output_renews_idle_after_a_line_handler{"RunExec/QueuedOutputRenewsIdleAfterALineHandler", [] {
    std::vector<long long> ticks;
    const auto result = process::execute({mcpp::invocation::self_exe_path().string(),
        "RunExec/OutputProducer"}, {
        .env = {{"MTEST_PROTOCOL", ""}, {"MCPP_TEST_OUTPUT_PRODUCER", "1"}, {"MCPP_TEST_OUTPUT_STREAM", "stdout"}, {"MCPP_TEST_OUTPUT_TICKS", "48"}},
        .streams = process::Streams::merged,
        .total = std::chrono::seconds(3), .idle = std::chrono::milliseconds(500),
        .on_line = [&](std::string_view line) {
            if (line == "mcpp-stderr-ready")
                std::this_thread::sleep_for(std::chrono::milliseconds(750));
            long long tick;
            if (std::from_chars(line.data(), line.data() + line.size(), tick).ec == std::errc{})
                ticks.push_back(tick);
        }});
    expect(eq(result.exit_code, 7));
    expect(!result.timed_out);
    expect(eq(ticks.size(), 48u));
}};

UTest queued_error_output_renews_idle_after_a_line_handler{"RunExec/QueuedErrorOutputRenewsIdleAfterALineHandler", [] {
    const auto result = process::execute({mcpp::invocation::self_exe_path().string(),
        "RunExec/OutputProducer"}, {
        .env = {{"MTEST_PROTOCOL", ""}, {"MCPP_TEST_OUTPUT_PRODUCER", "1"}, {"MCPP_TEST_OUTPUT_TICKS", "48"}},
        .streams = process::Streams::output_error_kept,
        .total = std::chrono::seconds(3), .idle = std::chrono::milliseconds(500),
        .on_line = [](std::string_view line) {
            if (line == "mcpp-stderr-ready")
                std::this_thread::sleep_for(std::chrono::milliseconds(750));
        }});
    expect(eq(result.exit_code, 7)) << result.error_output;
    expect(!result.timed_out);
    std::istringstream input(result.error_output);
    int ticks = 0;
    for (long long tick; input >> tick;) ++ticks;
    expect(eq(ticks, 48));
}};

UTest stream_completion_and_child_wait_keep_their_bounds{"RunExec/StreamCompletionAndChildWaitKeepTheirBounds", [] {
    struct Case {
        const char* name;
        const char* command;
        int total = 0;
        int idle = 0;
        bool stop = false;
        int exit = -1;
        // When `should_stop` begins to answer true. An exited child is waited
        // for however soon it is asked to stop; the case of one that exits by
        // itself gives the shell a start no runner is slower than.
        int stopAfter = 50;
    };
    const std::vector<Case> cases{
        {"exit", "exit 7", 0, 0, false, 7},
        {"signal", "kill -TERM $$", 0, 0, false, 143},
        {"already-exited-bounded", "exit 7", 3000, 0, true, 7, 2000},
        {"open-stop", "sleep 2", 0, 0, true},
        {"both-EOF-stop", "exec 1>&- 2>&-; sleep 2", 0, 0, true},
        {"stdout-EOF-stop", "exec 1>&-; sleep 2", 0, 0, true},
        {"both-EOF-total", "exec 1>&- 2>&-; sleep 2", 50},
        {"stdout-EOF-total", "exec 1>&-; sleep 2", 50},
    };
    auto exercise = [](process::Streams streams, const Case& test) {
        /* trace: std::format("stream={} case={}", static_cast<int>(streams), test.name) */;
        const auto started = std::chrono::steady_clock::now();
        process::Options options;
        options.streams = streams;
        options.total = std::chrono::milliseconds(test.total);
        options.idle = std::chrono::milliseconds(test.idle);
        if (test.stop) options.should_stop = [&] {
            return std::chrono::steady_clock::now() - started > std::chrono::milliseconds(test.stopAfter);
        };
        const bool producer = std::string_view(test.command) == "@stderr-producer";
        if (producer) options.env.emplace_back("MCPP_TEST_OUTPUT_PRODUCER", "1");
        if (producer) options.env.emplace_back("MTEST_PROTOCOL", "");
        const std::vector<std::string> argv = producer
            ? std::vector<std::string>{mcpp::invocation::self_exe_path().string(),
                                      "RunExec/OutputProducer"}
            : std::vector<std::string>{"/bin/sh", "-c", test.command};
        const auto result = process::execute(argv, options);
        if (test.exit >= 0) {
            expect(eq(result.exit_code, test.exit));
            expect(!result.timed_out);
        } else {
            expect(ne(result.exit_code, 0));
            expect(bool(result.timed_out));
        }
        const auto elapsed = std::chrono::steady_clock::now() - started;
        if (producer) {
            // One process emits across the idle window. Forking an external
            // sleep for every tick would add platform-dependent launch gaps.
            /* trace: result.error_output */;
            std::istringstream input(result.error_output);
            std::vector<long long> ticks;
            for (long long tick; input >> tick;) ticks.push_back(tick);
            expect(fatal(eq(ticks.size(), 24u)));
            for (std::size_t i = 1; i < ticks.size(); ++i)
                expect(lt(std::chrono::nanoseconds(ticks[i] - ticks[i - 1]), options.idle));
            expect(gt(elapsed, options.idle));
            expect(lt(elapsed, std::chrono::seconds(3)));
        } else {
            expect(lt(elapsed, std::chrono::seconds(1)));
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
}};

UTest stop_predicate_applies_after_captured_streams_close{"RunExec/StopPredicateAppliesAfterCapturedStreamsClose", [] {
    const auto started = std::chrono::steady_clock::now();
    const auto result = process::execute({"/bin/sh", "-c", "exec 1>&- 2>&-; sleep 2"}, {
        .streams = process::Streams::merged,
        .should_stop = [&] {
            return std::chrono::steady_clock::now() - started > std::chrono::milliseconds(50);
        }});
    expect(bool(result.timed_out));
    expect(ne(result.exit_code, 0));
    expect(lt(std::chrono::steady_clock::now() - started, std::chrono::seconds(1)));
}};

UTest stop_predicate_applies_without_captured_streams{"RunExec/StopPredicateAppliesWithoutCapturedStreams", [] {
    for (const auto streams : {process::Streams::inherit, process::Streams::discard}) {
        const auto started = std::chrono::steady_clock::now();
        int checks = 0;
        const auto result = process::execute({"/bin/sh", "-c", "sleep 60"}, {
            .streams = streams, .total = std::chrono::seconds(3),
            .should_stop = [&] {
                ++checks;
                return std::chrono::steady_clock::now() - started > std::chrono::milliseconds(50);
            }});
        expect(gt(checks, 0));
        expect(bool(result.timed_out));
        expect(ne(result.exit_code, 0));
        expect(lt(std::chrono::steady_clock::now() - started, std::chrono::seconds(1)));
    }
}};

UTest does_not_mutate_parent_environment{"RunExec/DoesNotMutateParentEnvironment", [] {
    mcpp::invocation::ScopedEnv leak("MCPP_TEST_LEAK", "sentinel");
    // Use /bin/sh (present on Linux AND macOS) — /bin/true lives at
    // /usr/bin/true on macOS, so it is not a portable launch target.
    int rc = process::run_exec({"/bin/sh", "-c", "[ \"$MCPP_TEST_LEAK\" = injected ]"},
                               {{"MCPP_TEST_LEAK", "injected"}});
    expect(eq(rc, 0));
    expect(eq(mcpp::invocation::get("MCPP_TEST_LEAK"), "sentinel"));   // unchanged → no leak
}};

UTest child_sees_injected_env{"RunExec/ChildSeesInjectedEnv", [] {
    // `sh -c '[ "$X" = injected ]'` exits 0 only if the child received X.
    int rc = process::run_exec(
        {"/bin/sh", "-c", "[ \"$MCPP_TEST_INJECT\" = injected ]"},
        {{"MCPP_TEST_INJECT", "injected"}});
    expect(eq(rc, 0));
}};

UTest propagates_child_exit_code{"RunExec/PropagatesChildExitCode", [] {
    expect(eq(process::run_exec({"/bin/sh", "-c", "exit 7"}), 7));
}};

UTest returns_error_when_program_missing{"RunExec/ReturnsErrorWhenProgramMissing", [] {
    expect(ne(process::run_exec({"/no/such/program/mcpp-xyz"}), 0));
}};

UTest missing_program_reports_spawn_failure{"CaptureExec/MissingProgramReportsSpawnFailure", [] {
    constexpr std::string_view missing = "/no/such/program/mcpp-capture-xyz";
    auto r = process::capture_exec({std::string(missing)});
    expect(eq(r.exit_code, 127));
    expect(ne(r.output.find(missing), std::string::npos)) << r.output;
    expect(ne(r.output.find("error 2"), std::string::npos)) << r.output;
}};

UTest captures_stdout_without_shell{"CaptureExec/CapturesStdoutWithoutShell", [] {
    auto r = process::capture_exec({"/bin/echo", "hello world"});
    expect(eq(r.exit_code, 0));
    expect(eq(r.output, "hello world\n"));
}};

UTest captures_stderr_combined{"CaptureExec/CapturesStderrCombined", [] {
    // stderr must be merged into output (replaces the old `2>&1`), otherwise
    // ninja's error text would be lost from the fast-path stale detector.
    auto r = process::capture_exec({"/bin/sh", "-c", "echo oops 1>&2"});
    expect(eq(r.exit_code, 0));
    expect(eq(r.output, "oops\n"));
}};

// #248 regression: extraEnv AND cwd BOTH non-empty — the previously untested
// combination (every prior call site passed at most one of the two; the only
// env+cwd caller is the build.mcpp child). The old macOS shell fallback built
// `KEY='v' cd <cwd> && prog 2>&1`, and POSIX binds the env assignments to the
// first command (`cd`) across `&&` — so the real program received NO extra env
// and build.mcpp lost the whole MCPP_* contract. Linux and macOS now share the
// posix_spawn path, so this locks the contract on both.
UTest env_and_cwd_combined_both_reach_child{"CaptureExec/EnvAndCwdCombinedBothReachChild", [] {
    auto dir = std::filesystem::temp_directory_path() / "mcpp_248_env_cwd";
    std::filesystem::create_directories(dir);
    auto r = process::capture_exec(
        {"/bin/sh", "-c", "printf '%s|%s' \"$MCPP_TEST_CONTRACT\" \"$PWD\""},
        {{"MCPP_TEST_CONTRACT", "hello"}},
        dir.string());
    expect(eq(r.exit_code, 0));
    // Env value must reach the child (the old prefix bound it to `cd` only).
    expect(ne(r.output.find("hello|"), std::string::npos)) << r.output;
    // Child must actually run in `cwd`. Compare the leaf name, not the full
    // path: macOS resolves /var → /private/var in $PWD.
    expect(ne(r.output.find("mcpp_248_env_cwd"), std::string::npos)) << r.output;
}};

// Companion check for run_exec (no cwd parameter there, but extraEnv must
// still land in the child via the same merged_environ posix_spawn path).
UTest injected_env_survives_shell_child_chain{"RunExec/InjectedEnvSurvivesShellChildChain", [] {
    int rc = process::run_exec(
        {"/bin/sh", "-c", "[ \"$MCPP_TEST_CONTRACT\" = hello ]"},
        {{"MCPP_TEST_CONTRACT", "hello"}});
    expect(eq(rc, 0));
}};

// ── spawn failures are typed, reported once, and never retried (#544) ──────
//
// The invariant the bounded launcher's DeadlineRun comment states ("could not
// spawn" and "ran and failed" must not share an exit code) was defeated one
// layer down: run_exec turned every posix_spawnp failure into a bare 127 with
// nothing printed, and both deadline wrappers fell back to it, spawning a
// second time and discarding the first errno. `mcpp run --target
// aarch64-linux-musl` on a host without an emulator printed "Running …", a
// blank line, and exited 1.

UTest missing_program_reports_on_stderr_when_caller_does_not_ask{"RunExec/MissingProgramReportsOnStderrWhenCallerDoesNotAsk", [] {
    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    int rc = process::run_exec({"/no/such/program/mcpp-run-xyz"});
    auto err = stderrCapture.text();
    expect(eq(rc, 127));
    expect(ne(err.find("/no/such/program/mcpp-run-xyz"), std::string::npos)) << err;
    expect(ne(err.find("error 2"), std::string::npos)) << err;
}};

UTest missing_program_types_errno_when_caller_asks{"RunExec/MissingProgramTypesErrnoWhenCallerAsks", [] {
    int spawnErr = 0;
    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    int rc = process::run_exec({"/no/such/program/mcpp-run-xyz"}, {}, &spawnErr);
    auto err = stderrCapture.text();
    expect(eq(rc, 127));
    expect(eq(spawnErr, ENOENT));
    expect(err.empty()) << err;   // the caller owns the report
}};

#if defined(__linux__)
// Linux only, and that is a MEASUREMENT (CI, macOS 14 ARM64, 2026-09-02): on
// macOS posix_spawnp handles ENOEXEC the way execvp does — it runs the file
// through /bin/sh — so the child IS spawned (spawn_error stays 0) and it is the
// shell that exits non-zero. The kernel's own refusal of a wrong-architecture
// Mach-O (EBADARCH) is a different reading and is not what this file
// synthesises. `mcpp run`'s "this host cannot execute" typing therefore fires
// on Linux for any unloadable file and on macOS only for a real foreign
// binary; see the design's open question 1.
UTest unloadable_artifact_is_enoexec{"RunExec/UnloadableArtifactIsENOEXEC", [] {
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
    expect(eq(rc, 127));
    expect(eq(spawnErr, ENOEXEC));
}};
#endif

UTest spawned_child_leaves_spawn_error_zero{"RunExec/SpawnedChildLeavesSpawnErrorZero", [] {
    int spawnErr = -1;
    expect(eq(process::run_exec({"/bin/sh", "-c", "exit 7"}, {}, &spawnErr), 7));
    expect(eq(spawnErr, 0));
}};

UTest capture_exec_missing_program_types_errno_when_caller_asks{"CaptureExec/MissingProgramTypesErrnoWhenCallerAsks", [] {
    int spawnErr = 0;
    auto r = process::capture_exec({"/no/such/program/mcpp-capture-typed"}, {}, {}, &spawnErr);
    expect(eq(r.exit_code, 127));
    expect(eq(spawnErr, ENOENT));
    expect(r.output.empty()) << r.output;
}};

UTest spawn_failure_is_typed_and_not_retried{"RunExecDeadline/SpawnFailureIsTypedAndNotRetried", [] {
    int spawnErr = 0;
    bool timedOut = true;
    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    int rc = process::run_exec_deadline({"/no/such/program/mcpp-dl-xyz"}, {},
                                        std::chrono::milliseconds(5000), &timedOut,
                                        &spawnErr);
    auto err = stderrCapture.text();
    expect(eq(rc, 127));
    expect(eq(spawnErr, ENOENT));
    expect(!timedOut);
    expect(err.empty()) << err;
}};

UTest spawn_failure_is_reported_when_caller_does_not_ask{"RunExecDeadline/SpawnFailureIsReportedWhenCallerDoesNotAsk", [] {
    bool timedOut = true;
    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    int rc = process::run_exec_deadline({"/no/such/program/mcpp-dl-untyped"}, {},
                                        std::chrono::milliseconds(5000), &timedOut);
    auto err = stderrCapture.text();
    expect(eq(rc, 127));
    expect(!timedOut);
    expect(ne(err.find("mcpp-dl-untyped"), std::string::npos)) << err;
    expect(ne(err.find("error 2"), std::string::npos)) << err;
}};

UTest spawn_failure_is_typed_in_outcome{"CaptureExecDeadline/SpawnFailureIsTypedInOutcome", [] {
    int spawnErr = 0;
    bool timedOut = true;
    auto r = process::capture_exec_deadline({"/no/such/program/mcpp-cdl-xyz"}, {},
                                            std::chrono::milliseconds(5000), &timedOut,
                                            {}, &spawnErr);
    expect(eq(r.exit_code, 127));
    expect(eq(spawnErr, ENOENT));
    expect(!timedOut);
    expect(r.output.empty()) << r.output;
}};

UTest spawn_failure_is_in_output_when_caller_does_not_ask{"CaptureExecDeadline/SpawnFailureIsInOutputWhenCallerDoesNotAsk", [] {
    bool timedOut = true;
    auto r = process::capture_exec_deadline({"/no/such/program/mcpp-cdl-untyped"}, {},
                                            std::chrono::milliseconds(5000), &timedOut);
    expect(eq(r.exit_code, 127));
    expect(ne(r.output.find("mcpp-cdl-untyped"), std::string::npos)) << r.output;
    expect(ne(r.output.find("error 2"), std::string::npos)) << r.output;
}};

// ── capture_stdout: the argument-vector form of `<program> 2>/dev/null` ──

UTest captures_standard_output_only{"CaptureStdout/CapturesStandardOutputOnly", [] {
    auto r = process::capture_stdout(
        {"/bin/sh", "-c", "echo out; echo 9.9.9-on-stderr 1>&2"});
    expect(eq(r.exit_code, 0));
    expect(eq(r.output, "out\n"));
}};

UTest propagates_exit_code{"CaptureStdout/PropagatesExitCode", [] {
    auto r = process::capture_stdout({"/bin/sh", "-c", "echo partial; exit 3"});
    expect(eq(r.exit_code, 3));
    expect(eq(r.output, "partial\n"));
}};

// A probe parses what it captured, so a launcher message must never be read
// as the program's answer: a missing program leaves the output empty.
UTest missing_program_leaves_output_empty{"CaptureStdout/MissingProgramLeavesOutputEmpty", [] {
    int spawnErr = 0;
    auto r = process::capture_stdout({"/no/such/program/mcpp-probe-2026.1.2.3"},
                                     {}, &spawnErr);
    expect(eq(r.exit_code, 127));
    expect(eq(spawnErr, ENOENT));
    expect(r.output.empty()) << r.output;
}};

UTest standard_input_is_empty{"CaptureStdout/StandardInputIsEmpty", [] {
    // `cat` returns at once with nothing when stdin is /dev/null; with an
    // inherited terminal or pipe it would block or echo the parent's input.
    auto r = process::capture_stdout({"/bin/sh", "-c", "cat; echo done"});
    expect(eq(r.exit_code, 0));
    expect(eq(r.output, "done\n"));
}};

UTest extra_environment_reaches_the_child{"CaptureStdout/ExtraEnvironmentReachesTheChild", [] {
    auto r = process::capture_stdout({"/bin/sh", "-c", "printf %s \"$MCPP_PROBE_ENV\""},
                                     {{"MCPP_PROBE_ENV", "reached"}});
    expect(eq(r.exit_code, 0));
    expect(eq(r.output, "reached"));
}};

#else  // _WIN32

UTest windows_covered_by_integration{"RunExec/WindowsCoveredByIntegration", [] {
    // The Windows path uses _spawnvpe (no /bin/* programs to point at here);
    // it is exercised by the integration build that launches ninja.
    /* passes */;
}};

#endif

