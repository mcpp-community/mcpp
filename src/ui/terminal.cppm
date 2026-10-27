// mcpp.ui.terminal — the terminal mcpp draws on, over mcpplibs.os: detection,
// size, UTF-8 output, a live display's frames, keys for a game, and standard
// output sent to standard error while a document is prepared.
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

export module mcpp.ui.terminal;

import std;

export namespace mcpp::ui::terminal {

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
// are terminals (a process has at most one console on Windows, and on POSIX
// two terminals at once is a redirection to another one, which is left out). A line for the stream a live
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
// and before mcpp ends if a request to end it arrives first (mcpp.run's
// watcher runs the restoration among its cleanups).
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
    struct State;
    std::unique_ptr<State> state_;
    bool active_ = false;
    std::string pending_;          // an escape sequence not yet complete
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
// THE SAVED OUTPUT IS NOT INHERITED (mcpplibs.os `output_to_error`). It is the
// caller's pipe, and a child started during the redirection (a build program,
// an xlings refresh) that inherited it would keep the caller from reading
// end-of-file for as long as the child lives, however long after mcpp itself
// exited. Measured (mcpp-community/mcpp#648): a build program started by
// `emit build-database` held the caller's pipe as its descriptor 3.
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
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace mcpp::ui::terminal

