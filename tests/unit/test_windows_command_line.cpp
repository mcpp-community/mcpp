#include <gtest/gtest.h>

import std;
import mcpp.platform.process;
import mcpp.platform.shell;

// The cmd.exe quoting rules are the easiest thing in mcpp to get wrong and
// the hardest to notice: on Linux and macOS the Windows branch is not even
// compiled, so nothing here is exercised by an ordinary local run. These
// tests drive the host-independent shapers directly, so a regression fails
// on every platform instead of only on a Windows runner.

namespace proc = mcpp::platform::process;

// #331: argv[0] used to be emitted RAW to survive cmd.exe's /c stripping.
// That made every payload under `C:\Program Files\...` — and every machine
// whose user name has a space — fail with `'C:\Program' is not recognized`.
TEST(WindowsCommandLine, ProgramPathWithSpacesIsQuoted) {
    auto cmd = proc::windows_command_from_argv(
        {"C:\\Program Files\\mcpp\\g++.exe", "-c", "main.cpp"});
    EXPECT_TRUE(cmd.starts_with("\"C:\\Program Files\\mcpp\\g++.exe\"")) << cmd;
}

TEST(WindowsCommandLine, EveryArgumentIsQuoted) {
    auto cmd = proc::windows_command_from_argv(
        {"g++.exe", "-I", "C:\\my dir\\inc", "src\\main.cpp"});
    EXPECT_NE(cmd.find("\"C:\\my dir\\inc\""), std::string::npos) << cmd;
    EXPECT_NE(cmd.find("\"src\\main.cpp\""), std::string::npos) << cmd;
}

TEST(WindowsCommandLine, EmptyArgvIsEmpty) {
    EXPECT_EQ(proc::windows_command_from_argv({}), "");
}

// The outer pair is what cmd.exe consumes under its "strip the first
// character and the last quote character" rule, so the inner quoting is
// what actually reaches the program. Without it, quoting argv[0] makes
// things worse rather than better.
TEST(WindowsCommandLine, WrapAddsTheOuterPairCmdConsumes) {
    auto inner = proc::windows_command_from_argv(
        {"C:\\Program Files\\mcpp\\g++.exe", "-c", "main.cpp"});
    auto wrapped = proc::windows_wrap_for_cmd_c(inner);
    ASSERT_GE(wrapped.size(), inner.size() + 2);
    EXPECT_EQ(wrapped.front(), '"');
    EXPECT_EQ(wrapped.back(),  '"');
    EXPECT_EQ(wrapped.substr(1, wrapped.size() - 2), inner);

    // Simulate what cmd.exe does with `/c <wrapped>`: drop the first
    // character and the last quote character. What remains must be exactly
    // the command we meant to run.
    auto stripped = wrapped.substr(1);
    stripped.erase(stripped.rfind('"'), 1);
    EXPECT_EQ(stripped, inner);
}

// Redirects appended after the command must end up INSIDE the wrap, so cmd
// still parses them once it has stripped the outer pair.
TEST(WindowsCommandLine, RedirectStaysInsideTheWrap) {
    auto wrapped = proc::windows_wrap_for_cmd_c(
        proc::windows_command_from_argv({"prog.exe", "arg"}) + " <NUL");
    EXPECT_TRUE(wrapped.ends_with(" <NUL\"")) << wrapped;
}

TEST(WindowsCommandLine, EmbeddedQuotesAreEscaped) {
    auto q = mcpp::platform::shell::quote_windows("a\"b");
    EXPECT_EQ(q, "\"a\\\"b\"");
}

// ─── A user-authored shell command (project `[hooks]`, #496) ─────────────
//
// What cmd.exe does with `/c <tail>` when /S is given: strip the first
// character and the last quote character, run the rest. Modelling it here is
// the whole point — the assertion is "the author's command arrives verbatim",
// not "the string looks plausible".
static std::string cmd_c_tail_under_slash_s(std::string_view line) {
    constexpr std::string_view kPrefix = "cmd.exe /d /s /c ";
    EXPECT_TRUE(line.starts_with(kPrefix)) << line;
    std::string tail(line.substr(kPrefix.size()));
    if (tail.empty() || tail.front() != '"') return tail;   // rule does not fire
    tail.erase(0, 1);
    tail.erase(tail.rfind('"'), 1);
    return tail;
}

// The switches must arrive BARE. Quoted (`"cmd.exe" "/d" "/s" "/c" "..."`,
// which is what windows_command_from_argv produces for the same argv) they
// are no longer switches, and the command tail keeps a quote pair cmd never
// consumes — the CI failure this shape exists to prevent.
TEST(WindowsCommandLine, ShellCommandKeepsCmdSwitchesBare) {
    auto line = proc::windows_shell_command_line("echo hi");
    EXPECT_TRUE(line.starts_with("cmd.exe /d /s /c ")) << line;
    EXPECT_EQ(line.find("\"/c\""), std::string::npos) << line;
}

// A redirect is the ordinary case for a hook that appends to a log, and it is
// also the case argv quoting destroys: `>` inside a quoted argument is a
// literal, not a redirect.
TEST(WindowsCommandLine, ShellCommandDeliversARedirectVerbatim) {
    constexpr std::string_view command = "echo start>>hooks.log";
    EXPECT_EQ(cmd_c_tail_under_slash_s(proc::windows_shell_command_line(command)),
              command);
}

// More than one interior quote pair is exactly where the /C rule bites, and
// where the "wrap once" shape earns its keep: whatever the author wrote comes
// back byte for byte.
TEST(WindowsCommandLine, ShellCommandDeliversQuotedPathsVerbatim) {
    constexpr std::string_view command =
        R"("C:\Program Files\notify\notify.exe" --title "build done")";
    EXPECT_EQ(cmd_c_tail_under_slash_s(proc::windows_shell_command_line(command)),
              command);
}

// The POSIX half must keep its own convention — a shared helper that
// silently applied Windows quoting on Linux would break every sh command.
TEST(WindowsCommandLine, PosixQuotingIsUnaffected) {
    EXPECT_EQ(mcpp::platform::shell::quote_posix("/home/my dir"),
              "'/home/my dir'");
    EXPECT_EQ(mcpp::platform::shell::quote_posix("it's"), "'it'\\''s'");
}
