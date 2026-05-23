// Regression test for the Windows first-run hang where xlings / xim / curl /
// git child processes blocked on terminal stdin, forcing the user to press
// Enter repeatedly to advance bootstrap / toolchain install.
//
// `mcpp::platform::process::{capture, run_silent, run_streaming}` MUST seal
// stdin so any child reading stdin gets immediate EOF, not a blocking read.
//   POSIX:   appends "</dev/null"
//   Windows: appends "<NUL"
//
// Test strategy: rebind this test process's own stdin to an open, empty,
// never-closing pipe. Then invoke run_silent / capture with a child that
// reads stdin. Without seal_stdin, the child would inherit our pipe stdin
// and block forever; the gtest runner would then hang until CI timeout.
// With the fix, the child reads from NUL / /dev/null and exits immediately.

#include <gtest/gtest.h>
#include <chrono>

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

import std;
import mcpp.platform.process;

namespace {

// Maximum seconds a sealed-stdin command may take before we declare it
// "hung". Real child runs (cat / more reading from EOF stdin) complete in
// well under 100ms on any modern machine, so 5s is a very generous bound.
constexpr int kMaxSealedSeconds = 5;

// RAII: rebind STDIN to an open, empty, never-closing pipe for the duration
// of one test. Restores the original stdin on destruction.
class OpenEmptyStdinScope {
public:
    OpenEmptyStdinScope() {
#if defined(_WIN32)
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        if (!CreatePipe(&hRead_, &hWrite_, &sa, 0)) {
            std::abort();
        }
        // Make the read end inheritable (already is via sa, but be explicit).
        SetHandleInformation(hRead_, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

        // Save the original stdin (both Win32 handle + CRT fd) so we can
        // restore in the destructor.
        origStdinHandle_ = GetStdHandle(STD_INPUT_HANDLE);
        origStdinFd_     = _dup(0);

        // Bind the pipe-read-end as our process's stdin at both layers.
        SetStdHandle(STD_INPUT_HANDLE, hRead_);
        int newFd = _open_osfhandle(reinterpret_cast<intptr_t>(hRead_),
                                    _O_RDONLY | _O_BINARY);
        if (newFd >= 0) {
            _dup2(newFd, 0);
            _close(newFd);  // _dup2 keeps a reference; we're done with newFd
        }
#else
        if (::pipe(pipeFds_) != 0) std::abort();
        origStdinFd_ = ::dup(0);
        ::dup2(pipeFds_[0], 0);
#endif
    }

    ~OpenEmptyStdinScope() {
#if defined(_WIN32)
        // Restore original stdin.
        if (origStdinFd_ >= 0) {
            _dup2(origStdinFd_, 0);
            _close(origStdinFd_);
        }
        SetStdHandle(STD_INPUT_HANDLE, origStdinHandle_);
        if (hWrite_) CloseHandle(hWrite_);
        if (hRead_)  CloseHandle(hRead_);
#else
        if (origStdinFd_ >= 0) {
            ::dup2(origStdinFd_, 0);
            ::close(origStdinFd_);
        }
        ::close(pipeFds_[0]);
        ::close(pipeFds_[1]);
#endif
    }

    OpenEmptyStdinScope(const OpenEmptyStdinScope&) = delete;
    OpenEmptyStdinScope& operator=(const OpenEmptyStdinScope&) = delete;

private:
#if defined(_WIN32)
    HANDLE hRead_  = nullptr;
    HANDLE hWrite_ = nullptr;          // intentionally never written → reader blocks
    HANDLE origStdinHandle_ = nullptr;
    int    origStdinFd_     = -1;
#else
    int pipeFds_[2]    = {-1, -1};      // intentionally never written → reader blocks
    int origStdinFd_   = -1;
#endif
};

// A child command that reads stdin to EOF and exits.
// With seal_stdin in effect → stdin is NUL / /dev/null → child exits immediately.
// Without seal_stdin AND with an open-empty parent stdin → child blocks forever.
constexpr std::string_view kStdinReaderCmd =
#if defined(_WIN32)
    "more >nul 2>&1"
#else
    "cat >/dev/null"
#endif
    ;

template <class F>
double time_seconds(F&& fn) {
    auto t0 = std::chrono::steady_clock::now();
    fn();
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t1 - t0).count();
}

} // namespace

// run_silent: must seal stdin so the child does not inherit our pipe stdin
// and block forever.
TEST(ProcessSealStdin, RunSilentDoesNotHangWhenParentStdinIsOpenPipe) {
    OpenEmptyStdinScope scope;
    double elapsed = time_seconds([] {
        (void)mcpp::platform::process::run_silent(kStdinReaderCmd);
    });
    EXPECT_LT(elapsed, static_cast<double>(kMaxSealedSeconds))
        << "run_silent took " << elapsed
        << "s — child blocked on stdin → seal_stdin is broken or not applied";
}

// capture: must also seal stdin (it shares seal_stdin with run_silent).
TEST(ProcessSealStdin, CaptureDoesNotHangWhenParentStdinIsOpenPipe) {
    OpenEmptyStdinScope scope;
    double elapsed = time_seconds([] {
        (void)mcpp::platform::process::capture(kStdinReaderCmd);
    });
    EXPECT_LT(elapsed, static_cast<double>(kMaxSealedSeconds))
        << "capture took " << elapsed
        << "s — child blocked on stdin → seal_stdin is broken or not applied";
}

// run_streaming: same property — children spawned via popen-streaming must
// not inherit a live stdin.
TEST(ProcessSealStdin, RunStreamingDoesNotHangWhenParentStdinIsOpenPipe) {
    OpenEmptyStdinScope scope;
    double elapsed = time_seconds([] {
        (void)mcpp::platform::process::run_streaming(
            kStdinReaderCmd,
            [](std::string_view) {});
    });
    EXPECT_LT(elapsed, static_cast<double>(kMaxSealedSeconds))
        << "run_streaming took " << elapsed
        << "s — child blocked on stdin → seal_stdin is broken or not applied";
}
