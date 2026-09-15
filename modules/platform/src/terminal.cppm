// mcpp.platform.terminal — terminal capability detection.
//
// Provides:
//   is_tty()         — whether stdout is a terminal
//   terminal_cols()  — terminal width in columns

module;
#include <cstdio>
#include <cstdlib>
#ifdef __unix__
#include <unistd.h>
#include <sys/ioctl.h>
#endif
#if defined(_WIN32)
#include <io.h>        // _dup, _dup2, _close, _get_osfhandle
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // SetHandleInformation, HANDLE_FLAG_INHERIT
#else
#include <unistd.h>    // dup2, close
#include <fcntl.h>     // fcntl, F_DUPFD_CLOEXEC
#endif

export module mcpp.platform.terminal;

import std;

export namespace mcpp::platform::terminal {

// Returns true if stdout is connected to a terminal (TTY).
bool is_tty();

// Returns the terminal width in columns. Tries TIOCGWINSZ on Unix,
// falls back to $COLUMNS, then defaults to 80.
std::size_t cols();

// EVERYTHING WRITTEN TO STANDARD OUTPUT GOES TO STANDARD ERROR UNTIL THIS IS
// DESTROYED.
//
// The redirection is made at the file descriptor, so a child process that
// inherits standard output follows it, and so does narration that prints to
// stdout without consulting any quiet flag. A command whose standard output is
// a document (`mcpp emit build-database`) plans under one of these and prints
// the document after it is gone: people still see the progress, on stderr, and
// the document arrives alone.
//
// THE SAVED DESCRIPTOR IS NOT INHERITED. It is the caller's pipe, and a child
// started during the redirection (a build program, an xlings refresh) that
// inherited it would keep the caller from reading end-of-file for as long as
// the child lives, however long after mcpp itself exited. Measured
// (mcpp-community/mcpp#648): a build program started by
// `emit build-database` held the caller's pipe as its descriptor 3. The copy
// is therefore close-on-exec on POSIX and not inheritable on Windows, so a
// child receives only descriptors 0 to 2, and 1 is standard error here.
class StdoutToStderr {
public:
    StdoutToStderr();
    ~StdoutToStderr();
    StdoutToStderr(const StdoutToStderr&) = delete;
    StdoutToStderr& operator=(const StdoutToStderr&) = delete;
private:
    int saved_ = -1;
};

} // namespace mcpp::platform::terminal

namespace mcpp::platform::terminal {

bool is_tty() {
#ifdef __unix__
    return ::isatty(::fileno(stdout)) != 0;
#else
    return false;
#endif
}

std::size_t cols() {
#ifdef __unix__
    struct winsize w{};
    if (::ioctl(::fileno(stdout), TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        return w.ws_col;
#endif
    if (auto* e = std::getenv("COLUMNS"); e && *e) {
        try { auto n = std::stoul(e); if (n > 0) return n; } catch (...) {}
    }
    return 80;
}

StdoutToStderr::StdoutToStderr() {
    std::fflush(stdout);
#if defined(_WIN32)
    saved_ = ::_dup(1);
    if (saved_ >= 0) {
        // `_dup` duplicates the handle as inheritable, and CreateProcess with
        // handle inheritance (every `_popen` and `system`) passes it on.
        const auto h = reinterpret_cast<HANDLE>(::_get_osfhandle(saved_));
        if (h != INVALID_HANDLE_VALUE)
            ::SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);
        ::_dup2(2, 1);
    }
#else
    // Not `dup`: its copy survives exec. Descriptor 3 or above, as `dup`
    // would have chosen, so nothing else about the redirection moves.
    saved_ = ::fcntl(1, F_DUPFD_CLOEXEC, 3);
    if (saved_ >= 0) ::dup2(2, 1);
#endif
}

StdoutToStderr::~StdoutToStderr() {
    std::fflush(stdout);
    if (saved_ < 0) return;
#if defined(_WIN32)
    ::_dup2(saved_, 1);
    ::_close(saved_);
#else
    ::dup2(saved_, 1);
    ::close(saved_);
#endif
}

} // namespace mcpp::platform::terminal
