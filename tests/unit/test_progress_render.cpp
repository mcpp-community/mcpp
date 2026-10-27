#include <gtest/gtest.h>

import std;
import mcpp.ui;
import mcpp.fetcher.progress;
import mcpp.run;
import mcpplibs.os.platform;

// W11: one renderer for every acquisition, with a terminal mode (redrawn in
// place) and a plain mode; either prints one line per item (#734). The renderer
// narrates, and narration is on standard error (output streams plan
// 2026-10-01, R3), which is what these tests capture.

TEST(GitProgress, ParsesThePhaseAndThePercentage) {
    auto r = mcpp::fetcher::parse_git_progress(
        "Receiving objects:  45% (450/1000), 1.20 MiB | 800.00 KiB/s");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->phase, "Receiving objects");
    EXPECT_EQ(r->percent, 45u);

    auto remote = mcpp::fetcher::parse_git_progress("remote: Counting objects: 100% (5/5), done.");
    ASSERT_TRUE(remote.has_value());
    EXPECT_EQ(remote->phase, "Counting objects");
    EXPECT_EQ(remote->percent, 100u);

    EXPECT_FALSE(mcpp::fetcher::parse_git_progress("Cloning into 'x'...").has_value());
    EXPECT_FALSE(mcpp::fetcher::parse_git_progress("fatal: repository not found").has_value());
    EXPECT_FALSE(mcpp::fetcher::parse_git_progress("").has_value());
}

TEST(ProgressBarPlain, OneCompletionLineWithoutRepaints) {
    mcpp::ui::disable_color();
    mcpp::ui::set_live_progress(false);
    testing::internal::CaptureStderr();
    {
        mcpp::ui::ProgressBar bar("Fetching", "example");
        bar.update_bytes(10, 100, 0.1);
        bar.update_bytes(50, 100, 0.5);
        bar.update_bytes(100, 100, 1.0);
        bar.finish();
    }
    auto out = testing::internal::GetCapturedStderr();
    EXPECT_EQ(out.find('\r'), std::string::npos) << out;
    EXPECT_EQ(out.find('\x1b'), std::string::npos) << out;
    // #734: one line per item, stating the size and the time.
    EXPECT_EQ(std::ranges::count(out, '\n'), 1) << out;
    EXPECT_NE(out.find("Fetching example done, 100 B in"), std::string::npos) << out;
}

TEST(ProgressBarPlain, AFailedItemIsNotReportedDone) {
    mcpp::ui::disable_color();
    mcpp::ui::set_live_progress(false);
    testing::internal::CaptureStderr();
    {
        mcpp::ui::ProgressBar bar("Installing", "xim:example");
        bar.update_indeterminate(0, 0.2);
        bar.finish_failed("xim:example");
    }
    auto out = testing::internal::GetCapturedStderr();
    EXPECT_NE(out.find("did not complete"), std::string::npos) << out;
    EXPECT_EQ(out.find(" done"), std::string::npos) << out;
}

TEST(DownloadProgressPlain, AnInterruptedDownloadIsNotReportedDone) {
    mcpp::ui::disable_color();
    mcpp::ui::set_live_progress(false);
    testing::internal::CaptureStderr();
    {
        mcpp::ui::DownloadProgress dl;
        const mcpp::ui::DownloadFile f{"xim-index.tar.gz", 10, 100, true, false};
        dl.update(std::span{&f, 1}, 0.2);
        dl.finish_failed();
    }
    auto out = testing::internal::GetCapturedStderr();
    EXPECT_NE(out.find("xim-index.tar.gz did not complete"), std::string::npos) << out;
    EXPECT_EQ(out.find(" done"), std::string::npos) << out;
}

TEST(ProgressBarLive, RedrawsInPlace) {
    mcpp::ui::disable_color();
    mcpp::ui::set_live_progress(true);
    testing::internal::CaptureStderr();
    {
        mcpp::ui::ProgressBar bar("Fetching", "example");
        bar.update(100);
        bar.finish();
    }
    auto out = testing::internal::GetCapturedStderr();
    mcpp::ui::set_live_progress(false);
    EXPECT_NE(out.find('\r'), std::string::npos) << out;
}

TEST(StreamingBounded, ACarriageReturnEndsALineOnlyWhenAsked) {
    if constexpr (mcpplibs::os::is_windows) GTEST_SKIP() << "printf is a POSIX program";
    const std::vector<std::string> argv{ "printf", "a\\rb\\r\\nc\\n" };
    std::vector<std::string> plain, split;
    (void)mcpp::run::execute(argv, { .idle = std::chrono::seconds{30},
        .on_line = [&](std::string_view l) { plain.emplace_back(l); } });
    (void)mcpp::run::execute(argv, { .idle = std::chrono::seconds{30},
        .on_line = [&](std::string_view l) { split.emplace_back(l); }, .split_on_cr = true });
    EXPECT_EQ(plain, (std::vector<std::string>{"a\rb", "c"}));
    EXPECT_EQ(split, (std::vector<std::string>{"a", "b", "c"}));
}
