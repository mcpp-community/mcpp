// mcpp.platform.terminal — terminal capability detection and output.
//
// Provides:
//   is_tty()          — whether stdout is a terminal
//   is_terminal(s)    — whether a standard stream is a terminal
//   can_move_cursor(s)— whether a live display may be drawn on it
//   cols(s), rows(s)  — the size of the terminal a stream reaches
//   write(s, text)    — UTF-8 text to a standard stream
//   write_frame(s, b) — a frame of a live display, in one write
//   same_terminal()   — whether stdout and stderr reach one terminal
//   KeyInput          — keys read from the terminal while a game plays

module;
#include <cstdio>     // stdout, stderr: macros, which `import std` does not carry

export module mcpp.platform.terminal;

import std;
import mcpp.platform.common;
import mcpplibs.os.terminal;
import mcpplibs.os.stdio;

export namespace mcpp::platform::terminal {

// The two standard streams a human reads.
enum class Stream { Out, Err };

// Returns true if stdout is connected to a terminal (TTY).
bool is_tty();
bool input_is_terminal();

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

// The width in columns of the terminal the stream reaches. Tries that terminal
// first (TIOCGWINSZ on the stream's descriptor, or the console's window on
// Windows), falls back to $COLUMNS, then to 80. The stream is a parameter
// because the display is drawn on one stream while the other may be a pipe: a
// width asked of a descriptor that is not a terminal is always the fallback.
std::size_t cols(Stream s);

// The terminal's height in rows, by the same order, with $LINES and 24.
std::size_t rows(Stream s);

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
// handles, and a process has at most one console. A line for the stream a live
// display is not drawn on can then travel in the frame written to the stream it
// is drawn on.
bool same_terminal();

// Whether the terminal the stream reaches is likely to draw East Asian
// ambiguous-width characters (`·`, `…`, `→`, the box-drawing block) two columns
// wide: a Windows console whose output code page is 932, 936, 949 or 950, or,
// on POSIX, a locale (LC_ALL, LC_CTYPE, then LANG) for Chinese, Japanese or
// Korean. The answer is a likelihood; a live display that budgets its width
// by it is never wider than the terminal either way.
bool ambiguous_wide(Stream s);

// Whether the terminal can be expected to draw characters beyond ASCII from
// its font or a fallback, braille included: on POSIX a UTF-8 locale (LC_ALL,
// LC_CTYPE, then LANG); on Windows a terminal that names itself (Windows
// Terminal sets WT_SESSION, VS Code and others TERM_PROGRAM). The console
// host alone has no glyph fallback, and its long-standing default font lacks
// the braille block.
bool unicode_capable();

// The keys a game reads (build output design revision 3, §5.14): the arrows,
// the space bar, and `wasd` as a second set of arrows.
enum class Key { Up, Down, Left, Right, Space };

// The keys in `pending`, the bytes a POSIX terminal sent: an arrow is
// `ESC [ A` to `ESC [ D`, `ESC O A` to `ESC O D` in application mode, or
// `ESC [ 1 ; 5 A` and the like with a modifier; a sequence for any other key
// is skipped whole. An incomplete sequence at the end is left in `pending`.
std::vector<Key> decode_keys(std::string& pending);

// KEYS READ FROM THE TERMINAL FOR THE LIFETIME OF THE OBJECT, without echo and
// without waiting for a line end; Ctrl-C still interrupts. Active only when
// standard input and the stream the display is drawn on (`display`) are both
// terminals and, on POSIX, mcpp is in the terminal's foreground process group:
// a background job that changed the terminal's mode would be stopped by
// SIGTTOU. The mode the terminal had is restored when the object is destroyed,
// and by the signal handler if a signal ends mcpp first (POSIX:
// `unixproc::guard_terminal_mode`; Windows: a console control handler).
class KeyInput {
public:
    explicit KeyInput(Stream display);
    ~KeyInput();
    KeyInput(const KeyInput&) = delete;
    KeyInput& operator=(const KeyInput&) = delete;
    bool active() const { return active_; }
    // The keys pressed since the last call; never blocks.
    std::vector<Key> read();
private:
    bool active_ = false;
    std::string pending_;          // an escape sequence not yet complete
    unsigned long savedMode_ = 0;  // Windows: the console input mode
};

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
// Writes `text` to the process's standard output as it was before any
// StdoutToStderr took it: a machine-readable stream (the build's NDJSON
// events) keeps the channel the protocol owns while the narration around it is
// redirected.
void write_protocol_stdout(std::string_view text);

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

// ─── Each system's own (posix/terminal.cpp, windows/terminal.cpp) ─────────
//
// Not exported. Whether a stream is a terminal, its size, its control
// sequences and UTF-8 text on a console are mcpplibs.os's; what remains is
// what that library does not carry.
namespace mcpp::platform::terminal::native {
// Whether standard output and standard error reach one terminal, both being
// terminals: the same device on POSIX, consoles on Windows.
bool same_terminal();
// The console's output code page; 0 where a stream is not a console, and on
// every system without one.
unsigned console_output_code_page(Stream s);
// Writes all of `text` to the C runtime's descriptor `fd`; false when a write
// fails.
bool write_descriptor(int fd, std::string_view text);
// A descriptor for standard output that a started program does not inherit,
// standard output then made a copy of standard error; -1 when it cannot be.
int divert_stdout();
// Standard output put back from `saved`, which is closed.
void restore_stdout(int saved);
}  // namespace mcpp::platform::terminal::native

namespace mcpp::platform::terminal {

namespace os = mcpplibs::os;

namespace {

std::FILE* file_of(Stream s) { return s == Stream::Out ? stdout : stderr; }
os::terminal::stream stream_of(Stream s) {
    return s == Stream::Out ? os::terminal::stream::out : os::terminal::stream::err;
}

} // namespace

bool is_terminal(Stream s) {
    return os::terminal::interactive(stream_of(s));
}

bool input_is_terminal() {
    return os::terminal::interactive(os::terminal::stream::in);
}

bool is_tty() { return is_terminal(Stream::Out); }

bool can_move_cursor(Stream s) {
    if (!is_terminal(s)) return false;
    if (const char* term = std::getenv("TERM"); term && std::string_view(term) == "dumb")
        return false;
    // A POSIX terminal is taken to obey them; a console is asked, and is
    // switched to virtual terminal processing by the asking.
    if constexpr (mcpp::platform::is_windows) return os::terminal::sequences(stream_of(s));
    else return true;
}

namespace {
std::size_t from_env(const char* name, std::size_t fallback) {
    if (auto* e = std::getenv(name); e && *e) {
        try { auto n = std::stoul(e); if (n > 0) return n; } catch (...) {}
    }
    return fallback;
}
} // namespace

std::size_t cols(Stream s) {
    if (const auto size = os::terminal::size(stream_of(s)); size && size->columns > 0)
        return size->columns;
    return from_env("COLUMNS", 80);
}

std::size_t rows(Stream s) {
    if (const auto size = os::terminal::size(stream_of(s)); size && size->rows > 0)
        return size->rows;
    return from_env("LINES", 24);
}

namespace {
// The whole of `bytes` to the stream through mcpplibs.os, which gives a
// console UTF-16 (openkal K10): text outside ASCII appears as written
// whatever the console's code page is.
bool write_through_os(Stream s, std::string_view bytes) {
    return (s == Stream::Out ? os::stdio::write_output(bytes) : os::stdio::write_error(bytes)).has_value();
}
} // namespace

void write(Stream s, std::string_view text) {
    if (text.empty()) return;
    if constexpr (mcpp::platform::is_windows) {
        if (is_terminal(s)) {
            std::fflush(file_of(s));
            if (write_through_os(s, text)) return;
        }
    }
    std::fwrite(text.data(), 1, text.size(), file_of(s));
}

void write_frame(Stream s, std::string_view bytes) {
    if (bytes.empty()) return;
    std::fflush(stdout);
    std::fflush(stderr);
    // One write of the whole frame (repeated by the library only for a
    // partial write or an interruption); on Windows only to a console, the
    // C runtime's text mode keeping the line ends of anything else.
    if (!mcpp::platform::is_windows || is_terminal(s))
        if (write_through_os(s, bytes)) return;
    std::fwrite(bytes.data(), 1, bytes.size(), file_of(s));
    std::fflush(file_of(s));
}

bool same_terminal() {
    return native::same_terminal();
}

bool ambiguous_wide(Stream s) {
    const unsigned cp = native::console_output_code_page(s);
    if (cp == 932 || cp == 936 || cp == 949 || cp == 950) return true;
    for (const char* name : {"LC_ALL", "LC_CTYPE", "LANG"}) {
        const char* v = std::getenv(name);
        if (!v || !*v) continue;
        const std::string_view l(v);
        return l.starts_with("zh") || l.starts_with("ja") || l.starts_with("ko");
    }
    return false;
}

bool unicode_capable() {
    if constexpr (mcpp::platform::is_windows) {
        for (const char* name : {"WT_SESSION", "TERM_PROGRAM"})
            if (const char* v = std::getenv(name); v && *v) return true;
        return false;
    } else {
        for (const char* name : {"LC_ALL", "LC_CTYPE", "LANG"}) {
            const char* v = std::getenv(name);
            if (!v || !*v) continue;
            std::string l(v);
            for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return l.find("utf-8") != std::string::npos || l.find("utf8") != std::string::npos;
        }
        return false;
    }
}

std::vector<Key> decode_keys(std::string& pending) {
    std::vector<Key> keys;
    std::size_t i = 0;
    while (i < pending.size()) {
        const char c = pending[i];
        if (c == '\x1b') {
            if (i + 1 >= pending.size()) break;   // wait for the rest
            if (pending[i + 1] != '[' && pending[i + 1] != 'O') { ++i; continue; }
            // Parameter and intermediate bytes (0x20 to 0x3F), then one final
            // byte (0x40 to 0x7E) that names the key.
            std::size_t end = i + 2;
            while (end < pending.size()
                   && static_cast<unsigned char>(pending[end]) >= 0x20
                   && static_cast<unsigned char>(pending[end]) <= 0x3F)
                ++end;
            if (end >= pending.size()) break;     // wait for the final byte
            switch (pending[end]) {
            case 'A': keys.push_back(Key::Up);    break;
            case 'B': keys.push_back(Key::Down);  break;
            case 'C': keys.push_back(Key::Right); break;
            case 'D': keys.push_back(Key::Left);  break;
            default: break;
            }
            i = end + 1;
            continue;
        }
        switch (c) {
        case ' ': keys.push_back(Key::Space); break;
        case 'w': case 'W': keys.push_back(Key::Up);    break;
        case 's': case 'S': keys.push_back(Key::Down);  break;
        case 'a': case 'A': keys.push_back(Key::Left);  break;
        case 'd': case 'D': keys.push_back(Key::Right); break;
        default: break;
        }
        ++i;
    }
    pending.erase(0, i);
    return keys;
}

// KeyInput's constructor, destructor and read: each system's own.

namespace {
std::atomic<int> g_protocolStdout{-1};
}

void write_protocol_stdout(std::string_view text) {
    const int fd = g_protocolStdout.load();
    if (fd < 0) {
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fflush(stdout);
        return;
    }
    native::write_descriptor(fd, text);
}

StdoutToStderr::StdoutToStderr() {
    std::fflush(stdout);
    saved_ = native::divert_stdout();
    if (saved_ >= 0) {
        int expected = -1;
        g_protocolStdout.compare_exchange_strong(expected, saved_);
    }
}

StdoutToStderr::~StdoutToStderr() {
    std::fflush(stdout);
    if (saved_ < 0) return;
    int expected = saved_;
    g_protocolStdout.compare_exchange_strong(expected, -1);
    native::restore_stdout(saved_);
}

} // namespace mcpp::platform::terminal
