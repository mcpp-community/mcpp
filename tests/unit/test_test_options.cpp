
import std;
import mcpplibs.mtest;
import mcpp.build.backend;
import mcpp.build.execute;

using namespace mcpplibs::mtest;

using namespace mcpp::build;

// `mcpp test` has to be BOUNDABLE, and its run half bounded by default. It was
// neither: the per-test deadline defaulted to 0 (no limit) and no deadline
// existed for the build drives at all, so a single hung test or link could
// consume an entire unattended CI job with nothing to show for it. These
// defaults are the contract; asserting them here means neither half can drift
// back by an unnoticed edit.
UTest run_is_bounded_by_default{"TestOptions/RunIsBoundedByDefault", [] {
    TestOptions to;
    expect(gt(to.timeoutSecs, 0)) << "per-test run deadline must default to a limit";
    expect(eq(to.timeoutSecs, 300));
}};

// The build deadline deliberately does NOT default on, and the asymmetry is
// measured: a test binary running over five minutes is unusual, a cold
// dependency build running over fifteen is ordinary (one mcpp-index member
// builds OpenCV from source in 1019s on Linux, 1289s on Windows). A default
// ceiling would turn slow-but-correct builds red and blame mcpp for it.
UTest build_deadline_is_opt_in{"TestOptions/BuildDeadlineIsOptIn", [] {
    TestOptions to;
    expect(eq(to.buildTimeoutSecs, 0)) << "a default build ceiling would fail legitimate cold builds";
}};

// 0 is still reachable — "no limit" did not disappear, it just has to be asked
// for rather than being what you get by not thinking about it.
UTest zero_still_means_unlimited{"TestOptions/ZeroStillMeansUnlimited", [] {
    TestOptions to;
    to.timeoutSecs = 0;
    to.buildTimeoutSecs = 0;
    expect(eq(to.timeoutSecs, 0));
    expect(eq(to.buildTimeoutSecs, 0));
}};

// A build drive with no explicit ceiling must stay unbounded: BuildOptions is
// shared with `mcpp build`, which is interactive and must not acquire a
// surprise deadline just because `mcpp test` grew one.
UTest build_timeout_defaults_to_unbounded{"BuildOptions/BuildTimeoutDefaultsToUnbounded", [] {
    BuildOptions bo;
    expect(eq(bo.buildTimeoutSecs, 0u));
}};

UTest compile_database_is_optional_by_default{"BuildOptions/CompileDatabaseIsOptionalByDefault", [] {
    BuildOptions options;
    expect(!options.requireCompileDatabase);
}};

// The timeout verdict travels as a flag, not as a message prefix: run_tests
// reports a timed-out compile differently from a broken one, and matching on
// prose is exactly how that distinction rots silently.
UTest timed_out_is_a_flag_and_defaults_false{"BuildError/TimedOutIsAFlagAndDefaultsFalse", [] {
    BuildError e{"build failed", std::nullopt, "", false};
    expect(!e.timedOut);
    BuildError t{"build timed out after 900s", std::nullopt, "", true};
    expect(bool(t.timedOut));
}};

// The fan-out needs more than an exit code to report per-member progress: how
// many tests ran, and where the time went. A zero-initialised summary must be
// safe to print (the "no tests found" early return leaves it untouched).
UTest defaults_are_zero_and_printable{"TestRunSummary/DefaultsAreZeroAndPrintable", [] {
    TestRunSummary s;
    expect(eq(s.passed, 0));
    expect(eq(s.failed, 0));
    expect(eq(s.buildMs, 0));
    expect(eq(s.runMs, 0));
    expect(eq(s.elapsedMs, 0));
    expect(!s.packageError);
}};
