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


// ── An argument that must survive cmd.exe AND the child's argv parser ───────
//
// THE DEFECT. mcpp hands xlings its provisioning request as a JSON argument on
// a shell command line. `shell::quote` answers the CHILD's parser -- MSVCRT,
// whose escape for an embedded quote is `\"` -- and cmd.exe does not know that
// escape: to cmd every `"` toggles a quote state. A JSON payload therefore
// arrives at a `>` with an EVEN number of quotes behind it, cmd reads the `>`
// as a redirection, and the redirection target is the rest of the JSON:
//
//   Provisioning [xlings.workspace] entries declared by dependencies
//       (xim:shaderc@>=2026.3)
//   The filename, directory name, or volume label syntax is incorrect.
//
// Measured on windows-2022. The `>=` shape is what every rule package uses to
// state a floor, and no declaration reachable on Windows had ever carried one,
// so the whole shape was unexercised on that host.
//
// The two simulators below are the criterion. Neither asserts on the SPELLING
// of the escape -- they replay what each parser does and compare the argument
// the child would receive against the one that was meant.
namespace {

// cmd.exe, from `/d /s /c "<line>"` to what CreateProcess receives.
// Returns the command line, and reports whether any redirection or piping
// metacharacter survived unquoted -- which is what actually broke.
struct CmdParse {
    std::string passedOn;
    bool sawActiveMetacharacter = false;
};

CmdParse simulate_cmd_c(std::string_view wrapped) {
    // `/s`: strip the first character and the last quote character.
    std::string line{wrapped};
    if (!line.empty() && line.front() == '"') line.erase(0, 1);
    if (auto last = line.rfind('"'); last != std::string::npos) line.erase(last, 1);

    CmdParse out;
    bool inQuotes = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        // `^` escapes the next character OUTSIDE a quoted region; inside one
        // it is an ordinary character. Modelling only the first half would let
        // this simulator accept a shape cmd does not.
        if (c == '^' && !inQuotes) {
            if (i + 1 < line.size()) out.passedOn.push_back(line[++i]);
            continue;
        }
        if (c == '"') { inQuotes = !inQuotes; out.passedOn.push_back(c); continue; }
        if (!inQuotes && (c == '<' || c == '>' || c == '&' || c == '|'))
            out.sawActiveMetacharacter = true;
        out.passedOn.push_back(c);
    }
    return out;
}

// The MSVCRT argv rules, over the command line cmd passed on.
std::vector<std::string> msvcrt_argv(std::string_view line) {
    std::vector<std::string> argv;
    std::string cur;
    bool inQuotes = false, any = false;
    std::size_t i = 0;
    auto flush = [&] { if (any) { argv.push_back(cur); cur.clear(); any = false; } };
    while (i < line.size()) {
        char c = line[i];
        if (c == '\\') {
            std::size_t n = 0;
            while (i < line.size() && line[i] == '\\') { ++n; ++i; }
            if (i < line.size() && line[i] == '"') {
                cur.append(n / 2, '\\');
                if (n % 2 == 0) inQuotes = !inQuotes;
                else            cur.push_back('"');
                any = true;
                ++i;
            } else {
                cur.append(n, '\\');
                any = any || n > 0;
            }
            continue;
        }
        if (c == '"') { inQuotes = !inQuotes; any = true; ++i; continue; }
        if (!inQuotes && (c == ' ' || c == '\t')) { flush(); ++i; continue; }
        cur.push_back(c);
        any = true;
        ++i;
    }
    flush();
    return argv;
}

constexpr std::string_view kJsonWithAFloor =
    R"({"targets":["xim:shaderc@>=2026.3"],"yes":true})";

} // namespace

TEST(WindowsCommandLine, PlainQuotingLetsCmdSeeARedirection) {
    // The state before the fix, stated so the fix below is not asserting
    // against nothing. This is `shell::quote_windows`, which is correct for
    // the child and incomplete for cmd.
    auto line = "xlings.exe interface install_packages --args "
              + mcpp::platform::shell::quote_windows(kJsonWithAFloor);
    auto parsed = simulate_cmd_c(proc::windows_wrap_for_cmd_c(line));
    EXPECT_TRUE(parsed.sawActiveMetacharacter)
        << "if this ever becomes false the simulator stopped modelling cmd, "
           "and the test below proves nothing";
}

TEST(WindowsCommandLine, MetacharacterQuotingSurvivesBothParsers) {
    auto line = "xlings.exe interface install_packages --args "
              + mcpp::platform::shell::quote_windows_through_cmd(kJsonWithAFloor);
    auto parsed = simulate_cmd_c(proc::windows_wrap_for_cmd_c(line));

    EXPECT_FALSE(parsed.sawActiveMetacharacter)
        << "cmd would still read the `>` in the version floor as a redirection";

    // xlings.exe / interface / install_packages / --args / <json>
    auto argv = msvcrt_argv(parsed.passedOn);
    ASSERT_EQ(argv.size(), 5u) << parsed.passedOn;
    EXPECT_EQ(argv[4], kJsonWithAFloor)
        << "the child received something other than the JSON that was meant";
}

TEST(WindowsCommandLine, NothingToEscapeMeansByteIdenticalToPlainQuoting) {
    // The conservative half of the rule, and it is the half that was measured
    // the hard way: an earlier version escaped every metacharacter INCLUDING
    // the quotes, which is defensible on paper and broke every package fetch on
    // Windows -- including the ones whose JSON contains no metacharacter. A
    // payload with nothing to escape must come out exactly as before.
    for (std::string_view plain : {
             R"({"targets":["mcpplibs:tpl-demo@1.0.0"],"yes":true})",
             R"({"targets":["compat:widget@1.38.1"],"yes":true})",
             R"(a plain path C:\Program Files\x)" }) {
        EXPECT_EQ(mcpp::platform::shell::quote_windows_through_cmd(plain),
                  mcpp::platform::shell::quote_windows(plain))
            << "a payload with no metacharacter acquired an escape: " << plain;
    }
}

TEST(WindowsCommandLine, MetacharacterQuotingIsUnchangedForPlainText) {
    // A payload with nothing to escape must not acquire carets, so the common
    // case stays legible in a log.
    constexpr std::string_view plain = R"({"targets":["xim:shaderc@2026.3"]})";
    auto quoted = mcpp::platform::shell::quote_windows_through_cmd(plain);
    auto parsed = simulate_cmd_c(proc::windows_wrap_for_cmd_c("prog.exe --args " + quoted));
    auto argv = msvcrt_argv(parsed.passedOn);
    ASSERT_EQ(argv.size(), 3u) << parsed.passedOn;
    EXPECT_EQ(argv[2], plain);
}
