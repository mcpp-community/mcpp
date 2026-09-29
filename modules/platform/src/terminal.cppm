// mcpp.platform.terminal — terminal capability detection and output.
//
// Provides:
//   is_tty()          — whether stdout is a terminal
//   is_terminal(s)    — whether a standard stream is a terminal
//   can_move_cursor(s)— whether a live display may be drawn on it
//   cols(), rows()    — the terminal's size
//   write(s, text)    — UTF-8 text to a standard stream
//   write_frame(s, b) — a frame of a live display, in one write
//   same_terminal()   — whether stdout and stderr reach one terminal

module;
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#endif
#if defined(_WIN32)
#include <io.h>        // _dup, _dup2, _close, _get_osfhandle, _fileno
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // SetHandleInformation, GetConsoleMode, WriteConsoleW
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING   // older SDK and MinGW headers
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#else
#include <unistd.h>    // dup2, close
#include <fcntl.h>     // fcntl, F_DUPFD_CLOEXEC
#endif

export module mcpp.platform.terminal;

import std;

export namespace mcpp::platform::terminal {

// The two standard streams a human reads.
enum class Stream { Out, Err };

// Returns true if stdout is connected to a terminal (TTY).
bool is_tty();

// Whether the stream is a terminal: `isatty` on POSIX, macOS included, and a
// console on Windows. Until 2026.9.29.5 this was compiled only under
// `__unix__`, which Apple's compilers do not define, so macOS and Windows were
// never terminals and drew neither colour nor a live line. A Windows program
// under mintty writes to a pipe, not a console, and is not a terminal here.
bool is_terminal(Stream s);

// Whether a display that moves the cursor may be drawn on the stream: a
// terminal, not `TERM=dumb`, and on Windows a console that accepted virtual
// terminal processing. The first call on Windows enables that processing, so
// that the escape sequences mcpp writes are interpreted rather than printed.
bool can_move_cursor(Stream s);

// Returns the terminal width in columns. Tries the terminal first (TIOCGWINSZ,
// or the console's window on Windows), falls back to $COLUMNS, then to 80.
std::size_t cols();

// The terminal's height in rows, by the same order, with $LINES and 24.
std::size_t rows();

// Writes UTF-8 text to the stream. A Windows console receives it as UTF-16
// through WriteConsoleW, so that text outside ASCII (`·`, `→`, a path in
// Chinese) appears as written whatever the console's code page is; the stdio
// buffer is flushed first, so the two paths keep their order. Everything else
// receives the bytes through stdio, unflushed.
void write(Stream s, std::string_view text);

// Writes a frame of a live display in one operation. The stdio buffers of
// both streams are flushed first, so that everything written before arrives
// before; the frame then leaves in one write(2) on POSIX (repeated only for a
// partial write or an interruption) and in one WriteConsoleW on a Windows
// console. Written through stdio, a frame whose last row has no line end was
// flushed in two parts on a line-buffered terminal, and a terminal that
// painted between them showed the display half-drawn (measured: 184 of 202
// frames of one build, .agents/docs/2026-09-30-build-output-refinement-design.md
// F1).
void write_frame(Stream s, std::string_view bytes);

// Whether standard error reaches the terminal standard output reaches: both
// are terminals and, on POSIX, the same device; on Windows both are console
// handles, and a process has at most one console. A line for standard error
// can then travel in a frame written to standard output.
bool same_terminal();

// Whether the terminal is likely to draw East Asian ambiguous-width
// characters (`·`, `…`, `→`, the box-drawing block) two columns wide: a
// Windows console whose output code page is 932, 936, 949 or 950, or, on
// POSIX, a locale (LC_ALL, LC_CTYPE, then LANG) for Chinese, Japanese or
// Korean. The answer is a likelihood; a live display that budgets its width
// by it is never wider than the terminal either way.
bool ambiguous_wide();

// Whether the terminal can be expected to draw characters beyond ASCII from
// its font or a fallback, braille included: on POSIX a UTF-8 locale (LC_ALL,
// LC_CTYPE, then LANG); on Windows a terminal that names itself (Windows
// Terminal sets WT_SESSION, VS Code and others TERM_PROGRAM). The console
// host alone has no glyph fallback, and its long-standing default font lacks
// the braille block.
bool unicode_capable();

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

namespace {

std::FILE* file_of(Stream s) { return s == Stream::Out ? stdout : stderr; }

#if defined(_WIN32)
HANDLE handle_of(Stream s) {
    const auto h = reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(file_of(s))));
    return h == nullptr ? INVALID_HANDLE_VALUE : h;
}

bool console_of(Stream s, HANDLE* out = nullptr) {
    const auto h = handle_of(s);
    DWORD mode = 0;
    if (h == INVALID_HANDLE_VALUE || !::GetConsoleMode(h, &mode)) return false;
    if (out) *out = h;
    return true;
}
#endif

} // namespace

bool is_terminal(Stream s) {
#if defined(_WIN32)
    return console_of(s);
#elif defined(__unix__) || defined(__APPLE__)
    return ::isatty(::fileno(file_of(s))) != 0;
#else
    (void)s;
    return false;
#endif
}

bool is_tty() { return is_terminal(Stream::Out); }

bool can_move_cursor(Stream s) {
    if (!is_terminal(s)) return false;
    if (const char* term = std::getenv("TERM"); term && std::string_view(term) == "dumb")
        return false;
#if defined(_WIN32)
    HANDLE h;
    if (!console_of(s, &h)) return false;
    DWORD mode = 0;
    if (!::GetConsoleMode(h, &mode)) return false;
    if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) return true;
    return ::SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
    return true;
#endif
}

namespace {
std::size_t from_env(const char* name, std::size_t fallback) {
    if (auto* e = std::getenv(name); e && *e) {
        try { auto n = std::stoul(e); if (n > 0) return n; } catch (...) {}
    }
    return fallback;
}
} // namespace

std::size_t cols() {
#if defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (HANDLE h; console_of(Stream::Out, &h) && ::GetConsoleScreenBufferInfo(h, &info))
        return static_cast<std::size_t>(info.srWindow.Right - info.srWindow.Left + 1);
#elif defined(__unix__) || defined(__APPLE__)
    struct winsize w{};
    if (::ioctl(::fileno(stdout), TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        return w.ws_col;
#endif
    return from_env("COLUMNS", 80);
}

std::size_t rows() {
#if defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (HANDLE h; console_of(Stream::Out, &h) && ::GetConsoleScreenBufferInfo(h, &info))
        return static_cast<std::size_t>(info.srWindow.Bottom - info.srWindow.Top + 1);
#elif defined(__unix__) || defined(__APPLE__)
    struct winsize w{};
    if (::ioctl(::fileno(stdout), TIOCGWINSZ, &w) == 0 && w.ws_row > 0)
        return w.ws_row;
#endif
    return from_env("LINES", 24);
}

void write(Stream s, std::string_view text) {
    if (text.empty()) return;
#if defined(_WIN32)
    if (HANDLE h; console_of(s, &h)) {
        std::fflush(file_of(s));
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                            static_cast<int>(text.size()), nullptr, 0);
        if (n > 0) {
            std::wstring wide(static_cast<std::size_t>(n), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                  wide.data(), n);
            DWORD written = 0;
            if (::WriteConsoleW(h, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr))
                return;
        }
    }
#endif
    std::fwrite(text.data(), 1, text.size(), file_of(s));
}

void write_frame(Stream s, std::string_view bytes) {
    if (bytes.empty()) return;
    std::fflush(stdout);
    std::fflush(stderr);
#if defined(_WIN32)
    if (HANDLE h; console_of(s, &h)) {
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
                                            static_cast<int>(bytes.size()), nullptr, 0);
        if (n > 0) {
            std::wstring wide(static_cast<std::size_t>(n), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()),
                                  wide.data(), n);
            DWORD written = 0;
            if (::WriteConsoleW(h, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr))
                return;
        }
    }
    std::fwrite(bytes.data(), 1, bytes.size(), file_of(s));
    std::fflush(file_of(s));
#elif defined(__unix__) || defined(__APPLE__)
    const int fd = ::fileno(file_of(s));
    const char* p = bytes.data();
    std::size_t left = bytes.size();
    while (left > 0) {
        const auto n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
#else
    std::fwrite(bytes.data(), 1, bytes.size(), file_of(s));
    std::fflush(file_of(s));
#endif
}

bool same_terminal() {
#if defined(_WIN32)
    return console_of(Stream::Out) && console_of(Stream::Err);
#elif defined(__unix__) || defined(__APPLE__)
    const int out = ::fileno(stdout), err = ::fileno(stderr);
    if (::isatty(out) == 0 || ::isatty(err) == 0) return false;
    struct stat a{}, b{};
    if (::fstat(out, &a) != 0 || ::fstat(err, &b) != 0) return false;
    return a.st_rdev == b.st_rdev;
#else
    return false;
#endif
}

bool ambiguous_wide() {
#if defined(_WIN32)
    if (console_of(Stream::Out)) {
        const UINT cp = ::GetConsoleOutputCP();
        if (cp == 932 || cp == 936 || cp == 949 || cp == 950) return true;
    }
#endif
    for (const char* name : {"LC_ALL", "LC_CTYPE", "LANG"}) {
        const char* v = std::getenv(name);
        if (!v || !*v) continue;
        const std::string_view l(v);
        return l.starts_with("zh") || l.starts_with("ja") || l.starts_with("ko");
    }
    return false;
}

bool unicode_capable() {
#if defined(_WIN32)
    for (const char* name : {"WT_SESSION", "TERM_PROGRAM"})
        if (const char* v = std::getenv(name); v && *v) return true;
    return false;
#else
    for (const char* name : {"LC_ALL", "LC_CTYPE", "LANG"}) {
        const char* v = std::getenv(name);
        if (!v || !*v) continue;
        std::string l(v);
        for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return l.find("utf-8") != std::string::npos || l.find("utf8") != std::string::npos;
    }
    return false;
#endif
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
