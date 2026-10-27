#include <gtest/gtest.h>

import std;
import mcpplibs.os;
import mcpp.run;

// SUBSYSTEM-LEVEL, and the contract is B2-0 of the member-selection and
// build-program-cost plan (#748): the launcher is safe for concurrent callers.
//
// A workspace's build programs are compiled together, so several threads start
// children at the same moment. A child inherits every descriptor (every
// inheritable handle on Windows) its parent holds, so a child started by one
// thread while another thread's capture pipe is open keeps that pipe's write end
// open for as long as it lives. The other thread's reader then waits for end of
// file until an unrelated child exits.

namespace proc = mcpp::run;

namespace {

using Clock = std::chrono::steady_clock;

// `cmd /c echo` exits at once on every Windows; `ping -n 2` answers the second
// time one second after the first, which is the slowest child this needs.
constexpr bool kWindows = mcpplibs::os::is_windows;
const std::vector<std::string> kQuick = kWindows
    ? std::vector<std::string>{"cmd.exe", "/d", "/c", "echo quick"}
    : std::vector<std::string>{"/bin/sh", "-c", "echo quick"};
const std::vector<std::string> kSlow = kWindows
    ? std::vector<std::string>{"ping", "-n", "2", "127.0.0.1"}
    : std::vector<std::string>{"/bin/sh", "-c", "sleep 0.5"};
constexpr auto kSlowFor = std::chrono::milliseconds(kWindows ? 1000 : 500);
constexpr int  kRounds  = kWindows ? 6 : 12;

struct Outcome {
    Clock::time_point end{};
    int               exit_code = -1;
    std::string       output;
};

// Runs `argv` once the gate opens and records when its reader returned.
Outcome run_when_open(const std::atomic<bool>& gate, const std::vector<std::string>& argv) {
    while (!gate.load(std::memory_order_acquire)) std::this_thread::yield();
    auto r = proc::capture_exec(argv);
    return {Clock::now(), r.exit_code, std::move(r.output)};
}

} // namespace

// Two children started at once from two threads, one of which exits at once and
// one of which sleeps: the reader of the first returns when the first child
// exits, not when the second does.
//
// The window a leaked descriptor needs is the few hundred microseconds between
// the creation of a pipe and the parent's close of its write end, so one round
// cannot be expected to fall inside it. Each round releases both threads at one
// instant, which makes an overlap likely, and the rounds are repeated. With the
// launcher as it was, an overlap turns the quick reader's return into the slow
// child's exit; with it fixed, no round can.
TEST(ConcurrentChildren, AReaderReturnsWhenItsOwnChildExits) {
    for (int round = 0; round < kRounds; ++round) {
        std::atomic<bool> gate{false};
        Outcome quick, slow;
        std::thread tSlow([&] { slow = run_when_open(gate, kSlow); });
        std::thread tQuick([&] { quick = run_when_open(gate, kQuick); });
        gate.store(true, std::memory_order_release);
        tQuick.join();
        tSlow.join();

        ASSERT_EQ(quick.exit_code, 0) << "round " << round << ": " << quick.output;
        ASSERT_EQ(slow.exit_code, 0) << "round " << round << ": " << slow.output;
        // Each reader is handed its own child's output and nothing else.
        EXPECT_NE(quick.output.find("quick"), std::string::npos) << quick.output;
        // The quick child was gone long before the slow one. Half the slow
        // child's life is the margin: it does not depend on how fast the
        // machine starts a process, only on the two readers not sharing a pipe.
        const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(
            slow.end - quick.end);
        EXPECT_GT(gap, kSlowFor / 2)
            << "round " << round << ": the reader of the child that exited at once "
            << "returned only " << gap.count() << " ms before the slow child did; "
            << "it waited for a child another thread started";
    }
}
