
import std;
import mcpplibs.mtest;
import mcpplibs.os.stdio;
import mcpp.ui;
import mcpp.fetcher.progress;
import mcpp.run;
import mcpplibs.os.platform;

using namespace mcpplibs::mtest;

// W11: one renderer for every acquisition, with a terminal mode (redrawn in
// place) and a plain mode; either prints one line per item (#734). The renderer
// narrates, and narration is on standard error (output streams plan
// 2026-10-01, R3), which is what these tests capture.

UTest parses_the_phase_and_the_percentage{"GitProgress/ParsesThePhaseAndThePercentage", [] {
    auto r = mcpp::fetcher::parse_git_progress(
        "Receiving objects:  45% (450/1000), 1.20 MiB | 800.00 KiB/s");
    expect(fatal(r.has_value()));
    expect(eq(r->phase, "Receiving objects"));
    expect(eq(r->percent, 45u));

    auto remote = mcpp::fetcher::parse_git_progress("remote: Counting objects: 100% (5/5), done.");
    expect(fatal(remote.has_value()));
    expect(eq(remote->phase, "Counting objects"));
    expect(eq(remote->percent, 100u));

    expect(!(mcpp::fetcher::parse_git_progress("Cloning into 'x'...").has_value()));
    expect(!(mcpp::fetcher::parse_git_progress("fatal: repository not found").has_value()));
    expect(!(mcpp::fetcher::parse_git_progress("").has_value()));
}};

UTest one_completion_line_without_repaints{"ProgressBarPlain/OneCompletionLineWithoutRepaints", [] {
    mcpp::ui::disable_color();
    mcpp::ui::set_live_progress(false);
    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    {
        mcpp::ui::ProgressBar bar("Fetching", "example");
        bar.update_bytes(10, 100, 0.1);
        bar.update_bytes(50, 100, 0.5);
        bar.update_bytes(100, 100, 1.0);
        bar.finish();
    }
    auto out = stderrCapture.text();
    expect(eq(out.find('\r'), std::string::npos)) << out;
    expect(eq(out.find('\x1b'), std::string::npos)) << out;
    // #734: one line per item, stating the size and the time.
    expect(eq(std::ranges::count(out, '\n'), 1)) << out;
    expect(ne(out.find("Fetching example done, 100 B in"), std::string::npos)) << out;
}};

UTest a_failed_item_is_not_reported_done{"ProgressBarPlain/AFailedItemIsNotReportedDone", [] {
    mcpp::ui::disable_color();
    mcpp::ui::set_live_progress(false);
    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    {
        mcpp::ui::ProgressBar bar("Installing", "xim:example");
        bar.update_indeterminate(0, 0.2);
        bar.finish_failed("xim:example");
    }
    auto out = stderrCapture.text();
    expect(ne(out.find("did not complete"), std::string::npos)) << out;
    expect(eq(out.find(" done"), std::string::npos)) << out;
}};

UTest an_interrupted_download_is_not_reported_done{"DownloadProgressPlain/AnInterruptedDownloadIsNotReportedDone", [] {
    mcpp::ui::disable_color();
    mcpp::ui::set_live_progress(false);
    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    {
        mcpp::ui::DownloadProgress dl;
        const mcpp::ui::DownloadFile f{"xim-index.tar.gz", 10, 100, true, false};
        dl.update(std::span{&f, 1}, 0.2);
        dl.finish_failed();
    }
    auto out = stderrCapture.text();
    expect(ne(out.find("xim-index.tar.gz did not complete"), std::string::npos)) << out;
    expect(eq(out.find(" done"), std::string::npos)) << out;
}};

UTest redraws_in_place{"ProgressBarLive/RedrawsInPlace", [] {
    mcpp::ui::disable_color();
    mcpp::ui::set_live_progress(true);
    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    {
        mcpp::ui::ProgressBar bar("Fetching", "example");
        bar.update(100);
        bar.finish();
    }
    auto out = stderrCapture.text();
    mcpp::ui::set_live_progress(false);
    expect(ne(out.find('\r'), std::string::npos)) << out;
}};

UTest a_carriage_return_ends_a_line_only_when_asked{"StreamingBounded/ACarriageReturnEndsALineOnlyWhenAsked", [] {
    if constexpr (mcpplibs::os::is_windows) skip("printf is a POSIX program");
    const std::vector<std::string> argv{ "printf", "a\\rb\\r\\nc\\n" };
    std::vector<std::string> plain, split;
    (void)mcpp::run::execute(argv, { .idle = std::chrono::seconds{30},
        .on_line = [&](std::string_view l) { plain.emplace_back(l); } });
    (void)mcpp::run::execute(argv, { .idle = std::chrono::seconds{30},
        .on_line = [&](std::string_view l) { split.emplace_back(l); }, .split_on_cr = true });
    expect(eq(plain, (std::vector<std::string>{"a\rb", "c"})));
    expect(eq(split, (std::vector<std::string>{"a", "b", "c"})));
}};
