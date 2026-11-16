
import std;
import mcpplibs.mtest;
import mcpplibs.os.stdio;
import mcpp.ui;

using namespace mcpplibs::mtest;

// Closing notices are advisories about the run as a whole. They are printed
// once, after the command's own output, as `tip:` lines on stderr; a command
// that writes an envelope takes them first and reports them as notes.

UTest are_deduplicated_and_taken_once{"ClosingNotices/AreDeduplicatedAndTakenOnce", [] {
    (void)mcpp::ui::take_closing_notices();
    mcpp::ui::add_closing_notice("CODE_A", "the index needs a newer mcpp");
    mcpp::ui::add_closing_notice("CODE_A", "the index needs a newer mcpp");
    mcpp::ui::add_closing_notice("CODE_B", "a second notice");
    auto taken = mcpp::ui::take_closing_notices();
    expect(fatal(eq(taken.size(), 2u)));
    expect(eq(taken[0].code, "CODE_A"));
    expect(eq(taken[1].message, "a second notice"));
    expect(mcpp::ui::take_closing_notices().empty());
}};

UTest print_as_tip_lines_on_stderr_and_then_are_gone{"ClosingNotices/PrintAsTipLinesOnStderrAndThenAreGone", [] {
    (void)mcpp::ui::take_closing_notices();
    mcpp::ui::disable_color();
    mcpp::ui::add_closing_notice("CODE", "upgrade to see newer packages");
    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    mcpp::ui::print_closing_notices();
    mcpp::ui::print_closing_notices();
    auto err = stderrCapture.text();
    expect(eq(err, "tip: upgrade to see newer packages\n"));
}};

UTest quiet_prints_nothing_but_still_consumes{"ClosingNotices/QuietPrintsNothingButStillConsumes", [] {
    (void)mcpp::ui::take_closing_notices();
    mcpp::ui::add_closing_notice("CODE", "not shown under --quiet");
    mcpp::ui::set_quiet(true);
    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    mcpp::ui::print_closing_notices();
    auto err = stderrCapture.text();
    mcpp::ui::set_quiet(false);
    expect(err.empty()) << err;
    expect(mcpp::ui::take_closing_notices().empty());
}};
