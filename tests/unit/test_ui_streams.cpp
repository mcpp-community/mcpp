#include <gtest/gtest.h>

import std;
import mcpp.ui;
import mcpp.ui.terminal;

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
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    body();
    Captured c;
    c.err = testing::internal::GetCapturedStderr();
    c.out = testing::internal::GetCapturedStdout();
    return c;
}

void plain_mode() {
    mcpp::ui::disable_color();
    mcpp::ui::set_quiet(false);
    mcpp::ui::set_live_progress(false);
}

} // namespace

TEST(UiStreams, NarrationIsStandardError) {
    EXPECT_EQ(mcpp::ui::narration_stream(), mcpp::ui::terminal::Stream::Err);
}

TEST(UiStreams, AStepIsNarration) {
    plain_mode();
    auto c = capture([] {
        mcpp::ui::status("Compiling", "app v0.1.0 (.)");
        mcpp::ui::info("Downloading", "xim:gcc");
        mcpp::ui::line("   a line of a step");
    });
    EXPECT_EQ(c.out, "") << c.out;
    EXPECT_EQ(c.err,
              "   Compiling app v0.1.0 (.)\n"
              " Downloading xim:gcc\n"
              "   a line of a step\n");
}

TEST(UiStreams, FinishedIsNarrationAndFollowsTheLinesBeforeItWithABlankLine) {
    plain_mode();
    auto c = capture([] {
        mcpp::ui::status("Compiling", "app v0.1.0 (.)");
        mcpp::ui::finished("dev", std::chrono::milliseconds(840), "unoptimized + debuginfo");
    });
    EXPECT_EQ(c.out, "") << c.out;
    EXPECT_NE(c.err.find("    Finished dev [unoptimized + debuginfo] in 0.84s\n"),
              std::string::npos) << c.err;
    // The blank line answers to a narrated line, and the line before it is one.
    EXPECT_NE(c.err.find("(.)\n\n    Finished"), std::string::npos) << c.err;
}

TEST(UiStreams, AResultStaysOnStandardOutput) {
    plain_mode();
    auto c = capture([] {
        mcpp::ui::plain("tests/smoke ... ok (0.01s)");
        mcpp::ui::result("test result", "ok. 1 passed; 0 failed; finished in 0.02s");
        mcpp::ui::result("workspace result", "ok. 2 member(s); 2 passed");
    });
    EXPECT_EQ(c.err, "") << c.err;
    EXPECT_EQ(c.out,
              "tests/smoke ... ok (0.01s)\n"
              " test result ok. 1 passed; 0 failed; finished in 0.02s\n"
              "workspace result ok. 2 member(s); 2 passed\n");
}

// The stream is the caller's statement, never the verb's spelling: `status`
// narrates whatever its verb reads.
TEST(UiStreams, TheStreamIsNotInferredFromTheVerb) {
    plain_mode();
    auto c = capture([] { mcpp::ui::status("test result", "ok. 1 passed"); });
    EXPECT_EQ(c.out, "") << c.out;
    EXPECT_NE(c.err.find("test result ok. 1 passed"), std::string::npos) << c.err;
}

TEST(UiStreams, WarningsAndErrorsAreStandardError) {
    plain_mode();
    auto c = capture([] {
        mcpp::ui::warning("a warning");
        mcpp::ui::error("an error");
        mcpp::ui::note("a note");
        mcpp::ui::block("diagnostic text");
    });
    EXPECT_EQ(c.out, "") << c.out;
    EXPECT_EQ(c.err, "warning: a warning\nerror: an error\nnote: a note\ndiagnostic text\n");
}

TEST(UiStreams, QuietSuppressesTheNarrationAndTheBlankLineThatSeparatesIt) {
    plain_mode();
    mcpp::ui::set_quiet(true);
    auto c = capture([] {
        mcpp::ui::status("Running", "`target/bin/app`");
        mcpp::ui::line("");
        mcpp::ui::info("Downloading", "xim:gcc");
        mcpp::ui::finished("dev", std::chrono::milliseconds(1));
    });
    mcpp::ui::set_quiet(false);
    EXPECT_EQ(c.out, "") << c.out;
    EXPECT_EQ(c.err, "") << c.err;
}

TEST(UiStreams, TheBlankLineAfterRunningIsOnTheStreamOfTheRunningLine) {
    plain_mode();
    auto c = capture([] {
        mcpp::ui::status("Running", "`target/bin/app`");
        mcpp::ui::line("");
    });
    EXPECT_EQ(c.out, "") << c.out;
    EXPECT_EQ(c.err, "     Running `target/bin/app`\n\n");
}

TEST(UiStreams, AProgressBarCompletionLineIsNarration) {
    plain_mode();
    auto c = capture([] {
        mcpp::ui::ProgressBar bar("Fetching", "example");
        bar.update_bytes(100, 100, 1.0);
        bar.finish();
    });
    EXPECT_EQ(c.out, "") << c.out;
    EXPECT_NE(c.err.find("Fetching example done"), std::string::npos) << c.err;
}
