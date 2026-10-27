// Regression test for the Windows first-run hang where xlings / xim / curl /
// git child processes blocked on terminal stdin, forcing the user to press
// Enter repeatedly to advance bootstrap / toolchain install.
//
// A program mcpp starts reads an EMPTY standard input unless the caller asks
// for mcpp's own (`mcpp::run::Input::inherit`): a child that reads its input
// meets its end at once instead of waiting on mcpp's.
//
// Test strategy: this test program starts itself with a standard input that
// is a pipe never written and never closed, and that copy starts a program
// which reads its input to the end. Were the input inherited, the reader would
// wait for as long as the pipe is open, and the copy with it; the outer test
// bounds that wait.

#include <gtest/gtest.h>

import std;
import mcpp.run;
import mcpp.invocation;
import mcpplibs.os.platform;
import mcpplibs.os.process;

namespace {

namespace os = mcpplibs::os;

// A reading child completes in well under 100 ms on any machine; 5 s is a
// very generous bound.
constexpr auto kMaxSealed = std::chrono::seconds(5);

// A program that reads its standard input to the end.
std::vector<std::string> reader() {
    if constexpr (os::is_windows) return { "more" };
    else                          return { "cat" };
}

std::optional<std::string> child_mode() { return mcpp::invocation::get("MCPP_TEST_STDIN_CHILD"); }

}  // namespace

// The copy started by the test below: starts the reader through the launcher
// it is named, with this process's input the pipe nobody writes.
TEST(ProcessSealStdin, Child) {
    const auto mode = child_mode();
    if (!mode) GTEST_SKIP() << "run by AProgramReadsAnEmptyInput";
    if (*mode == "execute") {
        (void)mcpp::run::execute(reader(), { .streams = mcpp::run::Streams::discard });
    } else if (*mode == "stream") {
        bool timedOut = false;
        (void)mcpp::run::stream_exec(reader(), {}, {}, [](std::string_view) {}, &timedOut);
    } else if (*mode == "probe") {
        (void)mcpp::run::capture_stdout(reader());
    } else if (*mode == "shell") {
        (void)mcpp::run::capture_shell(os::is_windows ? "more >nul 2>&1" : "cat >/dev/null");
    }
}

TEST(ProcessSealStdin, AProgramReadsAnEmptyInput) {
    if (child_mode()) GTEST_SKIP();
    for (std::string mode : { "execute", "stream", "probe", "shell" }) {
        os::process::spawn_options o;
        o.program = mcpp::invocation::self_exe_path();
        o.args = { "--gtest_filter=ProcessSealStdin.Child" };
        o.env = { { "MCPP_TEST_STDIN_CHILD", mode } };
        o.in = os::process::stdio::pipe;   // held open, never written
        o.out = o.err = os::process::stdio::null;
        o.own_unit = true;
        auto p = os::process::process::spawn(o);
        ASSERT_TRUE(p) << mode;
        auto w = p->wait_for(kMaxSealed);
        if (!w || !w->has_value()) {
            p->end_unit();
            p->kill();
            FAIL() << mode << ": the started program waited on mcpp's input";
        }
        EXPECT_TRUE((*w)->success()) << mode;
    }
}
