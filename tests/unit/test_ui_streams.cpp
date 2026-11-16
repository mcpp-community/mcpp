
import std;
import mcpplibs.mtest;
import mcpplibs.os.stdio;
import mcpp.ui;
import mcpp.ui.terminal;

using namespace mcpplibs::mtest;

// Two streams, one rule (output streams plan 2026-10-01, R3): a line that
// narrates what the command is doing is written to standard error, and a line
// that states its result stays on standard output. `mcpp.ui` makes the choice
// in one place, `narration_stream()`, and every function below follows it.

namespace {

struct Captured {
    std::string out;
    std::string err;
};

// What `body` wrote to each of the two streams.
template <class F>
Captured capture(F&& body) {
    auto stdoutCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::output).value();
    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    body();
    Captured c;
    c.err = stderrCapture.text();
    c.out = stdoutCapture.text();
    return c;
}

void plain_mode() {
    mcpp::ui::disable_color();
    mcpp::ui::set_quiet(false);
    mcpp::ui::set_live_progress(false);
}

} // namespace

UTest narration_is_standard_error{"UiStreams/NarrationIsStandardError", [] {
    expect(eq(mcpp::ui::narration_stream(), mcpp::ui::terminal::Stream::Err));
}};

UTest a_step_is_narration{"UiStreams/AStepIsNarration", [] {
    plain_mode();
    auto c = capture([] {
        mcpp::ui::status("Compiling", "app v0.1.0 (.)");
        mcpp::ui::info("Downloading", "xim:gcc");
        mcpp::ui::line("   a line of a step");
    });
    expect(eq(c.out, "")) << c.out;
    expect(eq(c.err, "   Compiling app v0.1.0 (.)\n"
              " Downloading xim:gcc\n"
              "   a line of a step\n"));
}};

UTest finished_is_narration_and_follows_the_lines_before_it_with_a_blank_line{"UiStreams/FinishedIsNarrationAndFollowsTheLinesBeforeItWithABlankLine", [] {
    plain_mode();
    auto c = capture([] {
        mcpp::ui::status("Compiling", "app v0.1.0 (.)");
        mcpp::ui::finished("dev", std::chrono::milliseconds(840), "unoptimized + debuginfo");
    });
    expect(eq(c.out, "")) << c.out;
    expect(ne(c.err.find("    Finished dev [unoptimized + debuginfo] in 0.84s\n"), std::string::npos)) << c.err;
    // The blank line answers to a narrated line, and the line before it is one.
    expect(ne(c.err.find("(.)\n\n    Finished"), std::string::npos)) << c.err;
}};

UTest a_result_stays_on_standard_output{"UiStreams/AResultStaysOnStandardOutput", [] {
    plain_mode();
    auto c = capture([] {
        mcpp::ui::plain("tests/smoke ... ok (0.01s)");
        mcpp::ui::result("test result", "ok. 1 passed; 0 failed; finished in 0.02s");
        mcpp::ui::result("workspace result", "ok. 2 member(s); 2 passed");
    });
    expect(eq(c.err, "")) << c.err;
    expect(eq(c.out, "tests/smoke ... ok (0.01s)\n"
              " test result ok. 1 passed; 0 failed; finished in 0.02s\n"
              "workspace result ok. 2 member(s); 2 passed\n"));
}};

// The stream is the caller's statement, never the verb's spelling: `status`
// narrates whatever its verb reads.
UTest the_stream_is_not_inferred_from_the_verb{"UiStreams/TheStreamIsNotInferredFromTheVerb", [] {
    plain_mode();
    auto c = capture([] { mcpp::ui::status("test result", "ok. 1 passed"); });
    expect(eq(c.out, "")) << c.out;
    expect(ne(c.err.find("test result ok. 1 passed"), std::string::npos)) << c.err;
}};

UTest warnings_and_errors_are_standard_error{"UiStreams/WarningsAndErrorsAreStandardError", [] {
    plain_mode();
    auto c = capture([] {
        mcpp::ui::warning("a warning");
        mcpp::ui::error("an error");
        mcpp::ui::note("a note");
        mcpp::ui::block("diagnostic text");
    });
    expect(eq(c.out, "")) << c.out;
    expect(eq(c.err, "warning: a warning\nerror: an error\nnote: a note\ndiagnostic text\n"));
}};

UTest quiet_suppresses_the_narration_and_the_blank_line_that_separates_it{"UiStreams/QuietSuppressesTheNarrationAndTheBlankLineThatSeparatesIt", [] {
    plain_mode();
    mcpp::ui::set_quiet(true);
    auto c = capture([] {
        mcpp::ui::status("Running", "`target/bin/app`");
        mcpp::ui::line("");
        mcpp::ui::info("Downloading", "xim:gcc");
        mcpp::ui::finished("dev", std::chrono::milliseconds(1));
    });
    mcpp::ui::set_quiet(false);
    expect(eq(c.out, "")) << c.out;
    expect(eq(c.err, "")) << c.err;
}};

UTest the_blank_line_after_running_is_on_the_stream_of_the_running_line{"UiStreams/TheBlankLineAfterRunningIsOnTheStreamOfTheRunningLine", [] {
    plain_mode();
    auto c = capture([] {
        mcpp::ui::status("Running", "`target/bin/app`");
        mcpp::ui::line("");
    });
    expect(eq(c.out, "")) << c.out;
    expect(eq(c.err, "     Running `target/bin/app`\n\n"));
}};

UTest a_progress_bar_completion_line_is_narration{"UiStreams/AProgressBarCompletionLineIsNarration", [] {
    plain_mode();
    auto c = capture([] {
        mcpp::ui::ProgressBar bar("Fetching", "example");
        bar.update_bytes(100, 100, 1.0);
        bar.finish();
    });
    expect(eq(c.out, "")) << c.out;
    expect(ne(c.err.find("Fetching example done"), std::string::npos)) << c.err;
}};
