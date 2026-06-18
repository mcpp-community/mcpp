#include <gtest/gtest.h>
#include <cstdlib>

import std;
import mcpp.platform.process;

using namespace mcpp::platform;

// These exercise the POSIX launch path with POSIX program paths (/bin/true,
// /bin/sh, /bin/echo) — the env is applied as a `KEY='val' cmd` shell prefix,
// so the parent environment is never mutated. The Windows path is covered by
// the integration build (ninja launched via capture_exec).
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

#else  // _WIN32

TEST(RunExec, WindowsCoveredByIntegration) {
    // The Windows path uses _spawnvpe (no /bin/* programs to point at here);
    // it is exercised by the integration build that launches ninja.
    SUCCEED();
}

#endif
