#include <gtest/gtest.h>

import std;
import mcpp.build.cmdlimits;

// The command-length budget, as data. Seven builds have died because a command
// outgrew a ceiling nobody tracked, and each was found by crashing at the end
// of a build with an error that named neither the edge nor the cause. These
// tests hold the two properties that make the table worth having: the numbers
// are the REAL ones, and the diagnostic says enough to act on.
//
// See .agents/docs/2026-08-06-command-length-architecture.md.

namespace cl = mcpp::build::cmdlimits;

namespace {

TEST(CmdLimits, EveryChannelIsInTheTable) {
    // A channel that is not in the table has no budget, which is exactly the
    // state that produced this defect family seven times. `limit_of` must find
    // a real row for each enumerator, not fall through to the placeholder.
    for (auto c : {cl::Channel::NinjaArgv, cl::Channel::PosixShell,
                   cl::Channel::CmdWrapper, cl::Channel::RspContent,
                   cl::Channel::RspLine}) {
        EXPECT_EQ(cl::limit_of(c).channel, c);
        EXPECT_FALSE(cl::limit_of(c).imposedBy.empty());
    }
}

TEST(CmdLimits, NumbersAreTheRealOnes) {
    // Each of these was learned the expensive way; a "tidier" number would be
    // wrong. MAX_ARG_STRLEN is 32 pages — NOT the 2 MiB ARG_MAX that #344
    // spent a release believing in.
    EXPECT_EQ(cl::limit_of(cl::Channel::PosixShell).bytes, 128u * 1024u);
    EXPECT_EQ(cl::limit_of(cl::Channel::NinjaArgv).bytes, 32u * 1024u);
    EXPECT_EQ(cl::limit_of(cl::Channel::CmdWrapper).bytes, 8191u);
    EXPECT_EQ(cl::limit_of(cl::Channel::RspLine).bytes, 128u * 1024u);
}

TEST(CmdLimits, EverySymptomIsRecorded) {
    // The costly part of this family has never been the fix — it has been
    // RECOGNISING it. `Argument list too long`, `LNK1170` and a bare 127 have
    // nothing in common, so each row has to carry what the user actually sees.
    for (auto c : {cl::Channel::NinjaArgv, cl::Channel::PosixShell,
                   cl::Channel::CmdWrapper, cl::Channel::RspLine}) {
        auto const& l = cl::limit_of(c);
        EXPECT_FALSE(l.symptom.empty()) << "channel has no symptom recorded";
        EXPECT_FALSE(l.remedy.empty()) << "channel has no remedy recorded";
    }
}

TEST(CmdLimits, ShellIsTighterThanDirectSpawnOnWindows) {
    // cmd.exe is a quarter of CreateProcess, which is why #261's redirection
    // (it forced a `cmd /c` wrapper) failed where the direct spawn would have
    // been fine.
    EXPECT_LT(cl::inline_budget(/*win=*/true, /*shell=*/true),
              cl::inline_budget(/*win=*/true, /*shell=*/false));
}

TEST(CmdLimits, UnderBudgetPasses) {
    EXPECT_FALSE(cl::check_inline(std::string(1000, 'x'), false, true));
    EXPECT_FALSE(cl::check_inline(std::string(1000, 'x'), true, false));
}

TEST(CmdLimits, OverBudgetIsReportedWithBothNumbers) {
    // POSIX: ninja wraps in `sh -c`, so the whole command is one argv entry.
    auto over = cl::check_inline(std::string(200u * 1024u, 'x'), false, true);
    ASSERT_TRUE(over);
    EXPECT_EQ(over->channel, cl::Channel::PosixShell);
    EXPECT_EQ(over->actual, 200u * 1024u);
    EXPECT_EQ(over->allowed, 128u * 1024u);
}

TEST(CmdLimits, WindowsPicksTheChannelItActuallyTravels) {
    // 50 781 bytes is #274's real measurement (FFmpeg's 2281 units): fine for
    // CreateProcess, fatal through cmd.exe.
    const std::string cmd(50781, 'x');
    auto direct = cl::check_inline(cmd, /*win=*/true, /*shell=*/false);
    auto shell  = cl::check_inline(cmd, /*win=*/true, /*shell=*/true);
    ASSERT_TRUE(direct);
    EXPECT_EQ(direct->channel, cl::Channel::NinjaArgv);
    ASSERT_TRUE(shell);
    EXPECT_EQ(shell->channel, cl::Channel::CmdWrapper);
}

TEST(CmdLimits, DiagnosticNamesTheEdgeTheNumbersAndTheWayOut) {
    // Every underlying error omits the edge; that omission is what made these
    // expensive. The message must supply it, plus enough to act without
    // reading the source.
    auto over = cl::check_inline(std::string(200u * 1024u, 'x'), false, true);
    ASSERT_TRUE(over);
    const std::string msg = cl::explain("build edge 'bin/app' (rule cxx_link)", *over);

    EXPECT_NE(msg.find("bin/app"), std::string::npos);
    EXPECT_NE(msg.find("cxx_link"), std::string::npos);
    EXPECT_NE(msg.find("204800"), std::string::npos);   // actual
    EXPECT_NE(msg.find("131072"), std::string::npos);   // allowed
    EXPECT_NE(msg.find("MAX_ARG_STRLEN"), std::string::npos);
    EXPECT_NE(msg.find("response file"), std::string::npos);
    EXPECT_NE(msg.find("2026-08-06-command-length-architecture"), std::string::npos);
}

}  // namespace
