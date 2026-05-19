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

export module mcpp.platform.terminal;

import std;

export namespace mcpp::platform::terminal {

// Returns true if stdout is connected to a terminal (TTY).
bool is_tty();

// Returns the terminal width in columns. Tries TIOCGWINSZ on Unix,
// falls back to $COLUMNS, then defaults to 80.
std::size_t cols();

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

} // namespace mcpp::platform::terminal
