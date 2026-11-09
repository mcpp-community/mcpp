
import std;
import mcpplibs.mtest;
import mcpplibs.os.stdio;
import mcpp.diag;

using namespace mcpplibs::mtest;

using namespace mcpp::diag;

namespace {

struct DiagTest {
    void SetUp() { reset(); }
    void TearDown() { reset(); }
};

} // namespace

UTest warning_and_degraded_are_counted_separately{"DiagTest/WarningAndDegradedAreCountedSeparately", [] {
    struct Case : DiagTest {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    warning("manifest/schema", "unsupported key 'bogus'");
    degraded("build/depfile", "no GNU depfile on this toolchain",
             "edits to purview-included files will not trigger a rebuild");

    expect(eq(count(Severity::Warning), 1u));
    expect(eq(count(Severity::Degraded), 1u));
    expect(eq(records().size(), 2u));
}
    } fixture;
    fixture.body();
}};

UTest identical_reports_are_deduplicated{"DiagTest/IdenticalReportsAreDeduplicated", [] {
    struct Case : DiagTest {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    for (int i = 0; i < 5; ++i)
        warning("modgraph/scan", "glob 'a/**' matched no source file");

    expect(eq(records().size(), 1u))
        << "a repeated report should render and record once";
}
    } fixture;
    fixture.body();
}};

UTest same_domain_different_payload_stays_distinct{"DiagTest/SameDomainDifferentPayloadStaysDistinct", [] {
    struct Case : DiagTest {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    // Two degradations in one domain that tell the user different things are
    // two records — dedup keys on the whole payload, not the domain.
    degraded("build/depfile", "no GNU depfile", "stale BMI possible");
    degraded("build/depfile", "no GNU depfile", "stale object possible");

    expect(eq(records().size(), 2u));
}
    } fixture;
    fixture.body();
}};

UTest format_keeps_what_on_the_first_line{"DiagTest/FormatKeepsWhatOnTheFirstLine", [] {
    struct Case : DiagTest {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    // e2e assertions grep for the `what` substring; elaboration must not get
    // in front of it.
    warning("modgraph/scan", "[build].flags glob 'x/**' matched no source file");
    auto rs = records();
    expect(fatal(eq(rs.size(), 1u)));
    expect(eq(rs[0].format(), "[build].flags glob 'x/**' matched no source file"));

    reset();
    degraded("build/depfile", "what happened", "the impact", "the hint");
    rs = records();
    expect(fatal(eq(rs.size(), 1u)));
    expect(rs[0].format().starts_with("what happened\n"));
    expect(ne(rs[0].format().find("impact: the impact"), std::string::npos));
    expect(ne(rs[0].format().find("help: the hint"), std::string::npos));
}
    } fixture;
    fixture.body();
}};

UTest strict_promotes_degradations_only{"DiagTest/StrictPromotesDegradationsOnly", [] {
    struct Case : DiagTest {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    warning("manifest/schema", "just a warning");
    expect(bool(flush(/*strict=*/true)))
        << "a plain warning must not fail a --strict run";

    reset();
    degraded("build/depfile", "w", "i");
    expect(!flush(/*strict=*/true));

    reset();
    degraded("build/depfile", "w", "i");
    expect(bool(flush(/*strict=*/false)));
}
    } fixture;
    fixture.body();
}};

UTest flush_clears_run_state{"DiagTest/FlushClearsRunState", [] {
    struct Case : DiagTest {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    warning("a", "b");
    degraded("c", "d", "e");
    expect(bool(flush(/*strict=*/false)));
    expect(eq(records().size(), 0u));
    expect(eq(count(Severity::Degraded), 0u));
}
    } fixture;
    fixture.body();
}};

// Regression guard for the review finding on this batch: flush() is the ONLY
// place the --strict policy is settled, and it was initially never called
// outside this file — the channel reported degradations and then everyone
// ignored them, which is precisely the failure mode it exists to prevent.
// run_build_plan now calls it; this pins the contract flush() must honour.
UTest flush_is_the_sole_policy_point_and_reports_failure_to_the_caller{"DiagTest/FlushIsTheSolePolicyPointAndReportsFailureToTheCaller", [] {
    struct Case : DiagTest {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    // No records at all: strict must not fail a clean build.
    expect(bool(flush(/*strict=*/true)));

    // A degradation under --strict must tell the caller to fail. The caller
    // (run_build_plan) turns this into a non-zero exit.
    degraded("build/depfile", "no depfile on this toolchain",
             "stale BMI possible after editing an included file");
    expect(!flush(/*strict=*/true));

    // Same degradation without --strict: reported, not fatal.
    degraded("build/depfile", "no depfile on this toolchain",
             "stale BMI possible after editing an included file");
    expect(bool(flush(/*strict=*/false)));
}
    } fixture;
    fixture.body();
}};

// WS3 of the 2026-09-28 design: one statement per fact per PROCESS. A
// `--workspace` build flushes after each member, and a fact every member
// shares -- a redundant word the workspace states -- was printed once per
// member. The terminal prints it once; each run's record still holds it, so
// `take()` gives every member's occurrence to a machine-readable envelope.
UTest a_fact_is_printed_once_per_process_and_recorded_per_run{"DiagTest/AFactIsPrintedOncePerProcessAndRecordedPerRun", [] {
    struct Case : DiagTest {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    const std::string fact = "the CRT word is redundant ([workspace.build] dialect_cxxflags)";
    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    for (int member = 0; member < 5; ++member) {
        warning("build/msvc-crt-word", fact);
        auto run = take();
        expect(fatal(eq(run.size(), 1u))) << "member " << member << " lost its own record";
        expect(eq(run[0].what, fact));
        (void)flush(/*strict=*/false);
    }
    const auto printed = stderrCapture.text();
    std::size_t count = 0;
    for (auto at = printed.find(fact); at != std::string::npos; at = printed.find(fact, at + 1))
        ++count;
    expect(eq(count, 1u)) << printed;
}
    } fixture;
    fixture.body();
}};

UTest notes_are_printed_as_notes_and_never_promoted{"DiagTest/NotesArePrintedAsNotesAndNeverPromoted", [] {
    struct Case : DiagTest {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    note("build/runtime-placement", "a dependency ships the MSVC C++ runtime");
    const auto printed = stderrCapture.text();
    expect(ne(printed.find("note:"), std::string::npos)) << printed;
    expect(eq(printed.find("warning:"), std::string::npos)) << printed;
    expect(eq(count(Severity::Degraded), 0u));
    expect(bool(flush(/*strict=*/true)));
}
    } fixture;
    fixture.body();
}};
