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
#include <io.h>        // _dup, _dup2, _close
#else
#include <unistd.h>    // dup, dup2, close
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
    if (saved_ >= 0) ::_dup2(2, 1);
#else
    saved_ = ::dup(1);
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
