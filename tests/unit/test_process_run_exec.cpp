#include <gtest/gtest.h>
#include <cerrno>
#include <cstdlib>
#include <fstream>

import std;
import mcpp.platform.process;

using namespace mcpp::platform;

// These exercise the POSIX launch path with POSIX program paths (/bin/true,
// /bin/sh, /bin/echo) — Linux and macOS share the direct posix_spawn path
// (#248 launcher-unify): extraEnv goes into the child's envp only, so the
// parent environment is never mutated. The Windows path is covered by the
// integration build (ninja launched via capture_exec).
#if !defined(_WIN32)

// The regression that matters: launching with an injected loader var must NOT
// mutate the parent (mcpp) environment — that mutation is exactly what leaked
// into /bin/sh and crashed it on newer-glibc hosts.
TEST(RunExec, DoesNotMutateParentEnvironment) {
    ::setenv("MCPP_TEST_LEAK", "sentinel", 1);
    // Use /bin/sh (present on Linux AND macOS) — /bin/true lives at
    // /usr/bin/true on macOS, so it is not a portable launch target.
    int rc = process::run_exec({"/bin/sh", "-c", "exit 0"},
                               {{"MCPP_TEST_LEAK", "injected"}});
    EXPECT_EQ(rc, 0);
    const char* v = ::getenv("MCPP_TEST_LEAK");
    ASSERT_NE(v, nullptr);
    EXPECT_STREQ(v, "sentinel");   // parent unchanged → no leak
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

TEST(RunExec, UnloadableArtifactIsENOEXEC) {
    // An executable file with no loader magic: the kernel refuses it with
    // ENOEXEC. posix_spawnp does not retry through /bin/sh the way execvp
    // does, so the errno reaches the caller untouched. This is the reading
    // `mcpp run` turns into "this host cannot execute the artifact".
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

#else  // _WIN32

TEST(RunExec, WindowsCoveredByIntegration) {
    // The Windows path uses _spawnvpe (no /bin/* programs to point at here);
    // it is exercised by the integration build that launches ninja.
    SUCCEED();
}

#endif
