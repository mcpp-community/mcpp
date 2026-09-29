// mcpp.ui — verb-style colored status output.
//
// All user-visible status lines from CLI / fetcher / build go through
// here. TTY auto-detect; MCPP_NO_COLOR / --no-color disables colors.
//
// ONE RENDERER, TWO MEDIA (build progress design 2026-09-29, §5, and its
// revision 3, 2026-09-30, §9). Every line goes through `emit`. On a terminal
// that can move the cursor, the rows that are still changing -- a download
// bar and the status row -- form a region below the log, and a line written
// while the region is on screen is written above it. The lines and the
// region's new rows leave in ONE write that overwrites the old rows in place:
// no row is erased before it is written, so no screen between two writes
// shows the region half-drawn (measured with 2026.9.29.5: 184 of 202 frames of
// one build left in two writes, and every line above the region in three).
// Anywhere else only final lines are written, and the status line is repeated
// when the log has been silent for a minute.

module;
#include <cstdio>      // fileno, stdout

export module mcpp.ui;

import std;
import mcpp.platform;
import mcpp.log;

export namespace mcpp::ui {

// One-time initialization. Call once at program start.
void init();

// Force-disable color. Useful for --no-color flag handling.
void disable_color();

// Check if color is enabled.
bool is_color_enabled();

// Verb-style status ("Compiling foo v0.1.0" pattern).
//   verb        verb word, padded right-aligned in 12-char column
//   message     metadata after the verb
void status(std::string_view verb, std::string_view message);

// Cyan verb (Updating, Downloading, Cleaned).
void info(std::string_view verb, std::string_view message);

// Bold green Finished line, preceded by a blank line when the command wrote a
// line before it (design §4.5).
// `descriptor` annotates the profile's actual effect (e.g. "optimized",
// "unoptimized + debuginfo"). Empty = print the profile name alone; callers
// that never resolved the profile knobs must not invent one. `detail` follows
// the time: how the time was spent, when the caller knows it.
void finished(std::string_view profile, std::chrono::milliseconds elapsed,
              std::string_view descriptor = {}, std::string_view detail = {});

// "warning:" / "error:" prefix lines (yellow / red).
void warning(std::string_view message);
void error(std::string_view message);
// "note:" prefix line (cyan), on stderr like the two above: a statement that
// changes nothing the build does and that a reader may want to act on.
void note(std::string_view message);

// Text written to stderr as it is, through the region: a block of compiler
// diagnostics or advice that already carries its own line ends.
void block(std::string_view text);

// Closing notices: advisories that concern the run as a whole rather than the
// step that noticed them, such as a refreshed package index that requires a
// newer mcpp. They are printed once, after the command's own output, as `tip:`
// lines on stderr, so that they are the last thing a reader sees and never
// interleave with a build's progress. A command that writes a machine-readable
// envelope takes them first and reports them as `note` diagnostics instead.
// A notice never changes the exit status.
struct ClosingNotice {
    std::string code;      // a stable code for machine output
    std::string message;   // one line; no trailing newline
};
void add_closing_notice(std::string code, std::string message);
std::vector<ClosingNotice> take_closing_notices();
void print_closing_notices();

// Multi-line Rust-style diagnostic (M4 #8.1).
// Renders as:
//
//   error[E0001]: <title>
//     --> path:line
//      |
//   <line> | <source line>
//      |   ^^^^ <span message>
//      |
//      = note: <note>
//      = help: <help>
//      = help: see `mcpp --explain E0001` for more details
//
// Empty fields are omitted.
struct Diagnostic {
    std::string                      code;          // e.g. "E0001" (optional)
    std::string                      title;
    std::filesystem::path           path;
    std::size_t                      line   = 0;
    std::size_t                      column = 0;
    std::string                      sourceLine;   // optional snippet
    std::string                      spanMessage;  // points at column
    std::vector<std::string>         notes;
    std::vector<std::string>         helps;
};
void diagnostic(const Diagnostic& d);

// Plain output (no verb), respecting -q flag.
void plain(std::string_view message);

// Flush stdout. Every stdout-writing function above already calls this, so
// callers only need it when they wrote to stdout directly (std::println) and
// want that line visible now rather than whenever the libc buffer happens to
// fill. main() also sets stdout line-buffered, which covers the POSIX
// platforms; this is what makes the guarantee hold on Windows too, where
// MSVCRT treats _IOLBF as _IOFBF. Progress-driven output must be visible while
// the process is still running: a build that is killed mid-flight is exactly
// when its last lines matter most.
void flush();

// Make stdout line-buffered. Call once, before any output. See main() for why
// this is not left to the libc default: the default block size is a different
// number on every platform (musl 1024, Apple libc st_blksize = 65536 on a pipe,
// MSVCRT 4096), so identical output becomes visible at wildly different times —
// and not at all if the process is killed before its buffer fills.
void set_line_buffered();

// --- measures of text (design §4.2, §5.1) ---

// The columns `text` occupies on a terminal: colour sequences count zero, an
// East Asian wide or fullwidth character two, a combining mark zero, and every
// other character one. Where the terminal is likely to draw East Asian
// ambiguous characters wide (`terminal::ambiguous_wide`), those count two, so
// that a row fitted by this measure never overflows and wraps.
std::size_t display_width(std::string_view text);
// Overrides the ambiguous-width decision (tests).
void set_ambiguous_wide(bool wide);
// `text` cut to at most `width` columns, its last column `…` when it was cut.
// Colour sequences are kept, and a reset follows a cut inside colour.
std::string fit(std::string_view text, std::size_t width);
// One format for every duration: `0.84s` and `12.34s` below a minute, `3m12s`
// below an hour, `1h02m` above it.
std::string format_duration(std::chrono::milliseconds d);
// A clock for the status line: `4:05`, `1:02:10`.
std::string format_clock(std::chrono::milliseconds d);

// The tone of a step line's state.
enum class Tone { Plain, Good, Muted, Bad };
// A step line (design §4.2): the verb right-aligned in 12 columns, the
// subject, and the state starting at `column` (the column after the verb's
// space), or two spaces after a longer subject. `infoVerb` colours the verb
// as `info` does, otherwise as `status` does.
std::string step_line(std::string_view verb, std::string_view subject,
                      std::size_t column, std::string_view state,
                      Tone tone = Tone::Plain, bool infoVerb = false);
// The status line (design §4.4): its phase in the verb colour, right-aligned
// in the 12 columns of the verbs above it (revision 3, §7.2).
std::string status_line(std::string_view phase, std::string_view rest);

// A hue for part of a line (revision 3, §5.10): the name of a package from the
// official index is cyan, from another index magenta, from a git repository
// blue; a version and an origin are dim. Plain text when colour is off.
enum class Hue { Plain, Cyan, Magenta, Blue, Dim };
std::string hue(std::string_view text, Hue h);

// --- the command's clock ---

// The moment the command started; main() marks it before anything else. The
// status line's clock and `Finished` both count from it.
void mark_command_start();
std::chrono::steady_clock::time_point command_start();

// --- the live region (design §5) ---

// What the region shows: the lines still changing, and the status line.
struct Frame {
    std::vector<std::string> lines;
    std::string              status;
};
using FrameSource = std::function<Frame()>;

// Opens the region for the rest of the command. `source` is asked for the
// frame on every redraw and heartbeat; it may be called while mcpp.ui holds
// its own lock, so it must not call back into mcpp.ui, and nothing may call
// mcpp.ui while holding a lock that `source` takes. On a terminal that can
// move the cursor the frame is drawn below the log, at most ten times a
// second on events and once a second otherwise; anywhere else the status line
// is written when the log has been silent for `heartbeat`. Under --quiet the
// region shows nothing. `poll`, when given, is called by the same thread
// before each redraw and heartbeat, without mcpp.ui's lock: it is where the
// frame's owner reads the files that feed it, and it may write lines.
void open_region(FrameSource source, std::function<void()> poll = {});
// Erases the region and stops drawing it. Idempotent.
void close_region();
// Something the frame shows changed: redraw within a tenth of a second.
void touch_region();
// Whether the region is drawn on a terminal.
bool region_live();
// The heartbeat interval of the log medium (tests shorten it).
void set_heartbeat(std::chrono::milliseconds interval);

// Erases the region for the lifetime of the object, and draws it again after:
// for a child that writes to the terminal itself.
class SuspendRegion {
public:
    SuspendRegion();
    ~SuspendRegion();
    SuspendRegion(const SuspendRegion&) = delete;
    SuspendRegion& operator=(const SuspendRegion&) = delete;
};

// One final line to stdout, through the region; suppressed by --quiet.
void line(std::string_view text);

// The bytes of one frame: from the first row of a region of `previousRows`
// rows, `text` (whole lines) is written over it, then `rows` (each already
// fitted to the width), and what remains of the old region is cleared. Every
// row is written over the old one and its tail cleared (`ESC[K`) rather than
// erased first; each row of the region is drawn with autowrap off, so a row
// that a terminal draws wider than it was measured is cut at the margin
// instead of wrapping. The cursor is left at the end of the last row. Pure:
// the unit tests read it.
std::string frame_bytes(std::size_t previousRows, std::string_view text,
                        const std::vector<std::string>& rows);
// `frame_bytes` without lines above the region.
std::string redraw_bytes(std::size_t previousRows, const std::vector<std::string>& rows);
// The rows of a frame: the bars, then at most `maxLines` of the frame's lines
// (the rest summarised as `… N more`), then the status line when there is a
// status.
std::vector<std::string> region_rows(const std::vector<std::string>& bars,
                                     const Frame& frame, std::size_t maxLines);

// --- progress bar (single-line, \r-rewritten) ---
//
// ONE RENDERER, TWO OUTPUT MODES. On a terminal the bar is a line of the
// region, redrawn in place. When stdout is not a terminal (a CI log, a pipe, a
// file) it prints one line when the item finishes, with its duration: no `\r`,
// no erase sequence, no repaint per frame. The mode follows stdout;
// `set_live_progress` overrides it for tests.
void set_live_progress(bool live);
bool live_progress();

class ProgressBar {
public:
    ProgressBar(std::string_view verb, std::string_view label);
    ~ProgressBar();

    ProgressBar(const ProgressBar&) = delete;
    ProgressBar& operator=(const ProgressBar&) = delete;

    // Update progress; renders only once per ~50ms to avoid jitter.
    void update(std::size_t percent);
    // elapsed_sec, when > 0, drives a `~X.Y MB/s` average-rate suffix.
    void update_bytes(std::size_t current_bytes, std::size_t total_bytes,
                      double elapsed_sec = 0.0);

    // Connecting / pre-sizing phase: the total size isn't known yet (the
    // downloader reports totalBytes==0 during DNS/TLS/redirect before the
    // transfer's Content-Length is available, and some servers stream with no
    // length at all). Renders a swept (indeterminate) bar plus a ticking
    // `connecting… Ns` / `X.Y MB Ns` suffix so the line never freezes.
    void update_indeterminate(std::size_t current_bytes,
                              double elapsed_sec = 0.0);

    // Finish: replaces progress with final-state line.
    void finish();
    void finish_with(std::string_view final_message);
    // Finish an item that did not complete: the line says so rather than
    // reporting it done.
    void finish_failed(std::string_view final_message);

private:
    void render_line(std::size_t percent, const std::string& info_text);
    void render_line_swept(std::size_t frame, const std::string& info_text);

    // One line per item in both modes (#734): a terminal redraws it in place
    // and ends it with the completion line; elsewhere only the completion
    // line is printed. `announce` records the size without printing.
    void announce(std::size_t total_bytes);
    void finish_plain(std::string_view final_message);
    std::string completion(std::string_view final_message) const;

    std::string verb_;
    std::string label_;
    std::chrono::steady_clock::time_point lastDraw_;
    std::chrono::steady_clock::time_point start_;
    bool finished_  = false;
    bool announced_ = false;
    std::size_t lastBytes_ = 0;
};

// --- download progress (centralized) ---
//
// One file's download state, decoded from xlings' NDJSON `download_progress`
// `files[]` entries. A neutral struct so this UI module stays free of any
// fetcher / config dependency (those import each other and would cycle).
struct DownloadFile {
    std::string name;
    std::size_t downloaded = 0;
    std::size_t total      = 0;   // 0 = size not known yet (connecting)
    bool        started    = false;
    bool        finished   = false;
};

// Centralized renderer for a streaming multi-file download. Owns the
// ProgressBar plus the "which file is active / which are done" bookkeeping,
// and decides per frame whether to draw a percentage bar (size known) or a
// swept/indeterminate bar (still connecting). Absorbs two xlings quirks:
// each file's `finished=true` is reported twice, and `files[]` reshuffles
// between events. Feed it each event's `files[]` snapshot + cumulative
// `elapsedSec`; it is the single place mcpp turns download events into UI.
class DownloadProgress {
public:
    DownloadProgress() = default;
    ~DownloadProgress();
    DownloadProgress(const DownloadProgress&)            = delete;
    DownloadProgress& operator=(const DownloadProgress&) = delete;

    void update(std::span<const DownloadFile> files, double elapsed_sec);
    void finish();   // finish the active bar if any (idempotent)
    // Finish the active bar as not completed: the run that fed it failed.
    void finish_failed();

private:
    std::optional<ProgressBar>      bar_;
    std::string                     active_;
    std::unordered_set<std::string> finished_;
};

// --- quiet flag (suppresses status / info / finished) ---
void set_quiet(bool q);
bool is_quiet();

// --- path display ---
//
// Path shortening for status output. Long absolute paths under the project
// root, MCPP_HOME, or the user's home directory get rewritten to short
// relative forms so the user can see _what_ rather than _where_.
//
// Substitution rules (most specific wins):
//   <project_root>/x/y/z   →  x/y/z              (project-relative)
//   <mcpp_home>/x/y/z      →  @mcpp/x/y/z
//   <home>/x/y/z           →  ~/x/y/z
//   anything else          →  absolute path
//
// `project_root` is optional — leave empty when the caller doesn't have a
// project context (e.g. for `mcpp self env`).
struct PathContext {
    std::filesystem::path project_root;
    std::filesystem::path mcpp_home;
    std::filesystem::path home;
};
std::string shorten_path(const std::filesystem::path& p, const PathContext& ctx);

} // namespace mcpp::ui

namespace mcpp::ui {

namespace {

namespace term = mcpp::platform::terminal;

bool g_color  = false;
bool g_quiet  = false;
bool g_inited = false;
// -1: follow stdout; 0 / 1: set by set_live_progress.
int  g_liveOverride = -1;
// East Asian ambiguous characters count two columns (terminal::ambiguous_wide).
bool g_ambiguousWide = false;

constexpr std::string_view kReset      = "\033[0m";
constexpr std::string_view kBold       = "\033[1m";
constexpr std::string_view kDim        = "\033[2m";
constexpr std::string_view kGreen      = "\033[32m";
constexpr std::string_view kBrightGreen= "\033[92m";
constexpr std::string_view kCyan       = "\033[36m";
constexpr std::string_view kBrightCyan = "\033[96m";
constexpr std::string_view kYellow     = "\033[33m";
constexpr std::string_view kRed        = "\033[31m";
constexpr std::string_view kBrightRed  = "\033[91m";

bool detect_color() {
    if (auto* e = std::getenv("MCPP_NO_COLOR"); e && *e == '1') return false;
    if (auto* e = std::getenv("NO_COLOR");      e && *e)        return false;
    // On Windows this also turns the console's escape processing on, without
    // which the colour sequences would be printed as text.
    return term::can_move_cursor(term::Stream::Out);
}

std::string with_color(std::string_view code, std::string_view text) {
    if (!g_color) return std::string(text);
    std::string out;
    out.reserve(code.size() + text.size() + kReset.size());
    out.append(code).append(text).append(kReset);
    return out;
}

std::string verb_padded(std::string_view verb) {
    constexpr std::size_t W = 12;
    if (verb.size() >= W) return std::string(verb);
    std::string s(W - verb.size(), ' ');
    s.append(verb);
    return s;
}

std::string verb_line(std::string_view colour, std::string_view verb,
                      std::string_view message) {
    auto v = verb_padded(verb);
    if (g_color) return std::format("{}{}{}{} {}", kBold, colour, v, kReset, message);
    return std::format("{} {}", v, message);
}

// The configuration groups of one workspace command build on threads
// (workspace design 2026-09-29 §6), and each narrates its build: one line is
// written as a whole. The region below is guarded by the same lock.
std::mutex& line_mutex() {
    static std::mutex m;
    return m;
}

// ─── The region's state, guarded by line_mutex() ────────────────────────

struct Region {
    bool open      = false;   // open_region was called and close_region not yet
    bool live      = false;   // drawn on a terminal (decided at open)
    int  suspended = 0;       // SuspendRegion objects alive
    FrameSource source;
    std::function<void()> poll;
    // The bars of the ProgressBars alive, in creation order.
    std::vector<std::pair<const void*, std::string>> bars;
    std::size_t drawnRows = 0;  // rows of the region on the screen now
    std::vector<std::string> lastRows;   // the rows drawn last
    bool anythingAbove = false; // this command wrote a line to stdout
    std::chrono::steady_clock::time_point lastDraw{};
    std::chrono::steady_clock::time_point lastLine{};
    std::chrono::milliseconds heartbeat{60'000};
};

Region& region() {
    static Region r;
    return r;
}

// The ticker, and how events reach it without the line lock.
struct Ticker {
    std::jthread            thread;
    std::mutex              m;
    std::condition_variable_any cv;
    std::atomic<bool>       dirty{false};
};
Ticker& ticker() {
    static Ticker t;
    return t;
}

std::chrono::steady_clock::time_point& start_point() {
    static auto t = std::chrono::steady_clock::now();
    return t;
}

// Whether the bars draw on a terminal: the region's decision while it is
// open, stdout's otherwise.
bool bars_live() {
    auto& r = region();
    if (r.open) return r.live;
    if (g_liveOverride >= 0) return g_liveOverride == 1;
    return term::can_move_cursor(term::Stream::Out);
}

std::string erase_bytes(std::size_t rows) {
    if (rows == 0) return {};
    std::string s = "\r";
    if (rows > 1) s += std::format("\033[{}A", rows - 1);
    s += "\033[J";
    return s;
}

std::size_t max_live_lines() {
    const auto rows = term::rows();
    return std::min<std::size_t>(10, rows > 3 ? rows - 3 : 1);
}

// The region is first drawn half a second into the command (revision 3,
// §7.2): a command that ends sooner shows no status row, and the lines of its
// first half-second do not move one. Measured with 2026.9.29.5: the status
// line was on the screen 0.6 ms after the command started, and the first
// warning, 0.37 s later, pushed it down.
constexpr auto kFirstDraw = std::chrono::milliseconds(500);

// Whether the region has something to draw on a terminal now; line_mutex()
// held.
bool may_draw_locked() {
    auto& r = region();
    if (r.suspended > 0 || g_quiet || !bars_live()) return false;
    if (r.drawnRows > 0) return true;
    if (!(r.open && r.live) && r.bars.empty()) return false;
    return std::chrono::steady_clock::now() - start_point() >= kFirstDraw;
}

// The region's rows as they are now, fitted to the width; line_mutex() held.
std::vector<std::string> current_rows_locked() {
    auto& r = region();
    Frame frame;
    if (r.open && r.source) frame = r.source();
    std::vector<std::string> bars;
    for (auto const& [who, text] : r.bars) bars.push_back(text);
    auto rows = region_rows(bars, frame, max_live_lines());
    const auto width = term::cols() > 1 ? term::cols() - 1 : 1;
    for (auto& row : rows) row = fit(row, width);
    return rows;
}

void drawn_locked(std::vector<std::string> rows) {
    auto& r = region();
    r.drawnRows = rows.size();
    r.lastRows  = std::move(rows);
    r.lastDraw  = std::chrono::steady_clock::now();
}

// Draws the region as it is now, in one write; line_mutex() held. A region
// that has not changed is not written again.
void redraw_locked() {
    auto& r = region();
    if (!may_draw_locked()) return;
    auto rows = current_rows_locked();
    if (rows.size() == r.drawnRows && rows == r.lastRows) return;
    term::write_frame(term::Stream::Out, frame_bytes(r.drawnRows, {}, rows));
    drawn_locked(std::move(rows));
}

void erase_locked() {
    auto& r = region();
    if (r.drawnRows == 0) return;
    term::write_frame(term::Stream::Out, erase_bytes(r.drawnRows));
    r.drawnRows = 0;
    r.lastRows.clear();
}

// Writes `text` (whole lines) above the region; line_mutex() held. With the
// region on a terminal, the lines and the region's new rows leave in one
// write. A line for standard error travels in that write when standard error
// is the same terminal; otherwise it goes to standard error alone, which is
// not the screen the region is on.
void emit_locked(term::Stream s, std::string_view text) {
    auto& r = region();
    if (s == term::Stream::Out && !text.empty()) r.anythingAbove = true;
    r.lastLine = std::chrono::steady_clock::now();
    const bool framed = may_draw_locked()
        && (s == term::Stream::Out || term::same_terminal());
    if (!framed) {
        term::write(s, text);
        std::fflush(s == term::Stream::Out ? stdout : stderr);
        return;
    }
    auto rows = current_rows_locked();
    term::write_frame(term::Stream::Out, frame_bytes(r.drawnRows, text, rows));
    drawn_locked(std::move(rows));
}

void emit(term::Stream s, std::string_view text);

// The terminal side of mcpp.log's verbose records: through the one writer.
void verbose_record(const mcpp::log::Record& record) {
    emit(term::Stream::Err, mcpp::log::verbose_line(record, g_color));
}

void tick(std::stop_token stop) {
    auto& t = ticker();
    constexpr auto kMinInterval = std::chrono::milliseconds(100);
    auto lastTick = std::chrono::steady_clock::now() - kMinInterval;
    while (!stop.stop_requested()) {
        {
            std::unique_lock lk(t.m);
            // At most ten frames a second (design §5.1): an event within a
            // tenth of a second of the last tick waits for the rest of it,
            // and its `dirty` flag is honoured then.
            const auto next = lastTick + kMinInterval;
            if (std::chrono::steady_clock::now() < next)
                t.cv.wait_until(lk, stop, next, [] { return false; });
            else
                t.cv.wait_for(lk, stop, kMinInterval, [&] { return t.dirty.load(); });
        }
        if (stop.stop_requested()) break;
        lastTick = std::chrono::steady_clock::now();
        std::function<void()> poll;
        {
            std::lock_guard line(line_mutex());
            poll = region().poll;
        }
        if (poll) poll();
        const bool dirty = t.dirty.exchange(false);
        std::lock_guard line(line_mutex());
        auto& r = region();
        if (!r.open) continue;
        const auto now = std::chrono::steady_clock::now();
        if (r.live) {
            if (dirty || now - r.lastDraw >= std::chrono::seconds(1)) redraw_locked();
            continue;
        }
        if (g_quiet || !r.source || now - r.lastLine < r.heartbeat) continue;
        auto frame = r.source();
        if (frame.status.empty()) { r.lastLine = now; continue; }
        emit_locked(term::Stream::Out, frame.status + "\n");
    }
}

} // namespace

namespace {
void emit(term::Stream s, std::string_view text) {
    std::lock_guard line(line_mutex());
    emit_locked(s, text);
}
} // namespace

void init() {
    if (g_inited) return;
    g_color  = detect_color();
    g_ambiguousWide = term::ambiguous_wide();
    g_inited = true;
    mcpp::log::set_terminal_sink(&verbose_record);
}

void set_ambiguous_wide(bool wide) { g_ambiguousWide = wide; }

void disable_color() { g_color = false; }
bool is_color_enabled() { return g_color; }

void set_quiet(bool q) { g_quiet = q; }

void set_live_progress(bool live) { g_liveOverride = live ? 1 : 0; }

bool live_progress() {
    if (g_liveOverride >= 0) return g_liveOverride == 1;
    return term::can_move_cursor(term::Stream::Out);
}
bool is_quiet()        { return g_quiet; }

void flush() { std::fflush(stdout); }

void set_line_buffered() {
#if defined(_WIN32)
    // Not on Windows, and not as a preference. The UCRT documents setvbuf's
    // size as `2 <= size <= INT_MAX`, and a zero goes through the
    // invalid-parameter handler, whose default action terminates the process:
    // 0xC0000409, which git-bash reports as a bare exit 127. The freshly built
    // mcpp.exe died on `--version` before printing anything.
    //
    // Passing a real size instead would buy nothing: MSVCRT has no line
    // buffering at all — it accepts _IOLBF and treats it as _IOFBF. Windows
    // gets the same guarantee from ui::flush(), which every stdout-writing
    // function here calls, so the platform that cannot do this cheaply is also
    // the one that does not need it: its 4096-byte block is the smallest of the
    // three anyway, and `mcpp test` routes its result lines through ui::plain.
#else
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
#endif
}

void status(std::string_view verb, std::string_view message) {
    if (g_quiet) return;
    init();
    emit(term::Stream::Out, verb_line(kBrightGreen, verb, message) + "\n");
}

void info(std::string_view verb, std::string_view message) {
    if (g_quiet) return;
    init();
    emit(term::Stream::Out, verb_line(kBrightCyan, verb, message) + "\n");
}

void finished(std::string_view profile, std::chrono::milliseconds elapsed,
              std::string_view descriptor, std::string_view detail) {
    if (g_quiet) return;
    init();
    // `[optimized]` used to be hardcoded here alongside a hardcoded "release"
    // at the only call site, so every build — including `--dev` at -O0 -g —
    // announced "Finished release [optimized]". The descriptor is now supplied
    // by whoever actually resolved the profile knobs, and omitted by callers
    // (the fast path) that never resolved them: no caller has to guess.
    auto msg = descriptor.empty()
        ? std::format("{} in {}", profile, format_duration(elapsed))
        : std::format("{} [{}] in {}", profile, descriptor, format_duration(elapsed));
    if (!detail.empty()) msg += std::format(" · {}", detail);
    std::lock_guard line(line_mutex());
    // The summary is separated from the steps above it by one blank line
    // (design §4.5); a command that wrote nothing before it writes none.
    const std::string head = region().anythingAbove ? "\n" : "";
    emit_locked(term::Stream::Out, head + verb_line(kBrightGreen, "Finished", msg) + "\n");
}

void warning(std::string_view message) {
    init();
    emit(term::Stream::Err, g_color
        ? std::format("{}{}warning:{} {}\n", kBold, kYellow, kReset, message)
        : std::format("warning: {}\n", message));
}

void error(std::string_view message) {
    init();
    emit(term::Stream::Err, g_color
        ? std::format("{}{}error:{} {}\n", kBold, kBrightRed, kReset, message)
        : std::format("error: {}\n", message));
}

void note(std::string_view message) {
    init();
    emit(term::Stream::Err, g_color
        ? std::format("{}{}note:{} {}\n", kBold, kCyan, kReset, message)
        : std::format("note: {}\n", message));
}

void block(std::string_view text) {
    if (text.empty()) return;
    std::string s(text);
    if (s.back() != '\n') s += '\n';
    emit(term::Stream::Err, s);
}

namespace {
std::vector<ClosingNotice>& closing_notices() {
    static std::vector<ClosingNotice> notices;
    return notices;
}
} // namespace

void add_closing_notice(std::string code, std::string message) {
    auto& all = closing_notices();
    for (auto const& n : all)
        if (n.message == message) return;
    all.push_back({std::move(code), std::move(message)});
}

std::vector<ClosingNotice> take_closing_notices() {
    return std::exchange(closing_notices(), {});
}

void print_closing_notices() {
    auto notices = take_closing_notices();
    if (g_quiet) return;
    init();
    for (auto const& n : notices) {
        emit(term::Stream::Err, g_color
            ? std::format("{}{}tip:{} {}\n", kBold, kCyan, kReset, n.message)
            : std::format("tip: {}\n", n.message));
    }
}

void plain(std::string_view message) {
    if (g_quiet) return;
    emit(term::Stream::Out, std::string(message) + "\n");
}

void line(std::string_view text) {
    if (g_quiet) return;
    emit(term::Stream::Out, std::string(text) + "\n");
}

void diagnostic(const Diagnostic& d) {
    init();
    auto bold_red = [&](std::string_view s) {
        return g_color ? std::format("{}{}{}{}", kBold, kBrightRed, s, kReset)
                       : std::string(s);
    };
    auto blue = [&](std::string_view s) {
        return g_color ? std::format("{}{}{}{}", kBold, kBrightCyan, s, kReset)
                       : std::string(s);
    };
    std::string out;
    std::string head = "error";
    if (!d.code.empty()) head += "[" + d.code + "]";
    head += ":";
    out += std::format("{} {}\n", bold_red(head), d.title);

    if (!d.path.empty()) {
        if (d.line)
            out += std::format("  {} {}:{}{}\n",
                blue("-->"), d.path.string(), d.line,
                d.column ? std::format(":{}", d.column) : "");
        else
            out += std::format("  {} {}\n", blue("-->"), d.path.string());
    }

    if (!d.sourceLine.empty()) {
        out += std::format("   {}\n", blue("|"));
        out += std::format(" {} {} {}\n",
            d.line ? std::format("{:>2}", d.line) : "  ", blue("|"), d.sourceLine);
        if (!d.spanMessage.empty()) {
            std::string caret(d.column ? d.column - 1 : 0, ' ');
            caret += "^";
            out += std::format("   {} {} {}\n", blue("|"), caret, d.spanMessage);
        }
    }

    if (!d.notes.empty() || !d.helps.empty()) {
        out += std::format("   {}\n", blue("|"));
    }
    for (auto& n : d.notes) {
        out += std::format("   {} {}: {}\n", blue("="), blue("note"), n);
    }
    for (auto& h : d.helps) {
        out += std::format("   {} {}: {}\n", blue("="), blue("help"), h);
    }
    if (!d.code.empty()) {
        out += "\n";
        out += std::format("For more information on this error: `mcpp --explain {}`\n",
                           d.code);
    }
    emit(term::Stream::Err, out);
}

// ─── Measures of text ────────────────────────────────────────────────────

namespace {

// One UTF-8 character at `i`: its code point and its length in bytes. A
// malformed byte is one character of one byte.
std::pair<char32_t, std::size_t> decode(std::string_view s, std::size_t i) {
    const auto b = static_cast<unsigned char>(s[i]);
    auto cont = [&](std::size_t k) {
        return i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
    };
    auto bits = [&](std::size_t k) {
        return static_cast<char32_t>(static_cast<unsigned char>(s[i + k]) & 0x3F);
    };
    if (b < 0x80) return {b, 1};
    if ((b & 0xE0) == 0xC0 && cont(1))
        return {(static_cast<char32_t>(b & 0x1F) << 6) | bits(1), 2};
    if ((b & 0xF0) == 0xE0 && cont(1) && cont(2))
        return {(static_cast<char32_t>(b & 0x0F) << 12) | (bits(1) << 6) | bits(2), 3};
    if ((b & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3))
        return {(static_cast<char32_t>(b & 0x07) << 18) | (bits(1) << 12)
                    | (bits(2) << 6) | bits(3), 4};
    return {b, 1};
}

// East Asian ambiguous characters this program writes: the middle dot, the
// punctuation block (`…`, dashes), arrows, box drawing, blocks and geometric
// shapes. Braille, which the status row's display uses, is narrow everywhere.
bool ambiguous(char32_t c) {
    return c == 0x00B7 || (c >= 0x2010 && c <= 0x203E) || (c >= 0x2190 && c <= 0x21FF)
        || (c >= 0x2500 && c <= 0x25FF);
}

std::size_t char_width(char32_t c) {
    if (c < 0x20 || c == 0x7F) return 0;
    if (g_ambiguousWide && ambiguous(c)) return 2;
    // Combining marks.
    if ((c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF)
        || (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0x20D0 && c <= 0x20FF)
        || (c >= 0xFE20 && c <= 0xFE2F) || c == 0x200B || c == 0x200D)
        return 0;
    // East Asian wide and fullwidth.
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E)
        || (c >= 0x3041 && c <= 0x33FF) || (c >= 0x3400 && c <= 0x4DBF)
        || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF)
        || (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF)
        || (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFF60)
        || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x20000 && c <= 0x3FFFD))
        return 2;
    return 1;
}

// The length of the escape sequence starting at `i`, or 0.
std::size_t escape_length(std::string_view s, std::size_t i) {
    if (s[i] != '\033' || i + 1 >= s.size() || s[i + 1] != '[') return 0;
    std::size_t k = i + 2;
    while (k < s.size() && !(s[k] >= 0x40 && s[k] <= 0x7E)) ++k;
    return k < s.size() ? k - i + 1 : s.size() - i;
}

} // namespace

std::size_t display_width(std::string_view text) {
    std::size_t w = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (auto e = escape_length(text, i)) { i += e; continue; }
        auto [c, n] = decode(text, i);
        w += char_width(c);
        i += n;
    }
    return w;
}

std::string fit(std::string_view text, std::size_t width) {
    if (display_width(text) <= width) return std::string(text);
    std::string out;
    std::size_t w = 0;
    bool coloured = false;
    const std::size_t room = width > 0 ? width - 1 : 0;   // one column for `…`
    for (std::size_t i = 0; i < text.size();) {
        if (auto e = escape_length(text, i)) {
            out.append(text.substr(i, e));
            coloured = true;
            i += e;
            continue;
        }
        auto [c, n] = decode(text, i);
        const auto cw = char_width(c);
        if (w + cw > room) break;
        out.append(text.substr(i, n));
        w += cw;
        i += n;
    }
    if (width > 0) out += "…";
    if (coloured) out += kReset;
    return out;
}

std::string format_duration(std::chrono::milliseconds d) {
    const auto ms = d.count() < 0 ? 0 : d.count();
    if (ms < 60'000) return std::format("{:.2f}s", static_cast<double>(ms) / 1000.0);
    const auto s = ms / 1000;
    if (s < 3600) return std::format("{}m{:02}s", s / 60, s % 60);
    return std::format("{}h{:02}m", s / 3600, (s % 3600) / 60);
}

std::string format_clock(std::chrono::milliseconds d) {
    const auto s = (d.count() < 0 ? 0 : d.count()) / 1000;
    if (s < 3600) return std::format("{}:{:02}", s / 60, s % 60);
    return std::format("{}:{:02}:{:02}", s / 3600, (s % 3600) / 60, s % 60);
}

std::string step_line(std::string_view verb, std::string_view subject,
                      std::size_t column, std::string_view state,
                      Tone tone, bool infoVerb) {
    init();
    std::string s = verb_line(infoVerb ? kBrightCyan : kBrightGreen, verb, subject);
    if (state.empty()) return s;
    const auto used = display_width(subject);
    s.append(used + 2 <= column ? column - used : 2, ' ');
    std::string_view colour = tone == Tone::Good  ? kGreen
                            : tone == Tone::Muted ? kDim
                            : tone == Tone::Bad   ? kRed
                                                  : std::string_view{};
    if (g_color && !colour.empty()) s += std::format("{}{}{}", colour, state, kReset);
    else s += state;
    return s;
}

std::string status_line(std::string_view phase, std::string_view rest) {
    init();
    const auto verb = std::format("{:>12}", phase);
    std::string s = g_color ? std::format("{}{}{}{}", kBold, kBrightCyan, verb, kReset)
                            : verb;
    if (!rest.empty()) s += std::format(" {}", rest);
    return s;
}

std::string hue(std::string_view text, Hue h) {
    init();
    if (!g_color || h == Hue::Plain || text.empty()) return std::string(text);
    std::string_view code = h == Hue::Cyan    ? "\033[36m"
                          : h == Hue::Magenta ? "\033[95m"
                          : h == Hue::Blue    ? "\033[94m"
                                              : kDim;
    return std::format("{}{}{}", code, text, kReset);
}

// ─── The command's clock ─────────────────────────────────────────────────

void mark_command_start() { (void)start_point(); }
std::chrono::steady_clock::time_point command_start() { return start_point(); }

// ─── The live region ─────────────────────────────────────────────────────

std::string frame_bytes(std::size_t previousRows, std::string_view text,
                        const std::vector<std::string>& rows) {
    std::string s;
    if (previousRows > 0) {
        s += '\r';
        if (previousRows > 1) s += std::format("\033[{}A", previousRows - 1);
    }
    while (!text.empty()) {
        const auto nl = text.find('\n');
        s += text.substr(0, nl);
        s += "\033[K\n";
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i) s += '\n';
        s += "\033[?7l";
        s += rows[i];
        s += "\033[K\033[?7h";
    }
    if (previousRows > 0) s += "\033[J";
    return s;
}

std::string redraw_bytes(std::size_t previousRows, const std::vector<std::string>& rows) {
    return frame_bytes(previousRows, {}, rows);
}

std::vector<std::string> region_rows(const std::vector<std::string>& bars,
                                     const Frame& frame, std::size_t maxLines) {
    std::vector<std::string> rows = bars;
    const std::size_t room = maxLines > bars.size() ? maxLines - bars.size() : 0;
    const std::size_t shown = frame.lines.size() <= room
        ? frame.lines.size() : (room > 0 ? room - 1 : 0);
    for (std::size_t i = 0; i < shown; ++i) rows.push_back(frame.lines[i]);
    if (shown < frame.lines.size())
        rows.push_back(std::format("{}… {} more", std::string(12, ' '),
                                   frame.lines.size() - shown));
    if (!frame.status.empty()) rows.push_back(frame.status);
    return rows;
}

void open_region(FrameSource source, std::function<void()> poll) {
    {
        std::lock_guard line(line_mutex());
        auto& r = region();
        if (r.open) { r.source = std::move(source); r.poll = std::move(poll); return; }
        r.open   = true;
        r.live   = !g_quiet && live_progress();
        r.source = std::move(source);
        r.poll   = std::move(poll);
        r.lastLine = std::chrono::steady_clock::now();
    }
    ticker().thread = std::jthread(tick);
}

void close_region() {
    auto& t = ticker();
    if (t.thread.joinable()) {
        t.thread.request_stop();
        t.cv.notify_all();
        t.thread.join();
    }
    std::lock_guard line(line_mutex());
    auto& r = region();
    if (!r.open) return;
    // Bars that outlive the region are drawn by themselves from here on.
    const auto bars = std::move(r.bars);
    r.bars.clear();
    erase_locked();
    r.open   = false;
    r.source = nullptr;
    r.poll   = nullptr;
    r.bars   = bars;
}

void touch_region() {
    auto& t = ticker();
    t.dirty.store(true);
    t.cv.notify_all();
}

bool region_live() {
    std::lock_guard line(line_mutex());
    return region().open && region().live;
}

void set_heartbeat(std::chrono::milliseconds interval) {
    std::lock_guard line(line_mutex());
    region().heartbeat = interval;
}

SuspendRegion::SuspendRegion() {
    std::fflush(stdout);
    std::lock_guard line(line_mutex());
    erase_locked();
    ++region().suspended;
}

SuspendRegion::~SuspendRegion() {
    std::fflush(stdout);
    std::fflush(stderr);
    std::lock_guard line(line_mutex());
    auto& r = region();
    if (r.suspended > 0) --r.suspended;
    // The child's lines are above the region now, and it ended its last line.
    r.anythingAbove = true;
    redraw_locked();
}

// --- ProgressBar ---

namespace {

std::string render_bar(std::size_t percent, std::size_t width = 20) {
    auto filled = (percent * width) / 100;
    if (filled > width) filled = width;
    std::string bar = "[";
    for (std::size_t i = 0; i < filled; ++i)        bar += "=";
    if (filled < width)                              bar += ">";
    for (std::size_t i = filled + 1; i < width; ++i) bar += " ";
    bar += "]";
    return bar;
}

std::string fmt_bytes(std::size_t b) {
    if (b < 1024)              return std::format("{} B",   b);
    if (b < 1024 * 1024)       return std::format("{} KB",  b / 1024);
    if (b < 1024UL*1024*1024)  return std::format("{:.1f} MB", static_cast<double>(b) / (1024.0*1024.0));
    return std::format("{:.2f} GB", static_cast<double>(b) / (1024.0*1024.0*1024.0));
}

// Best-effort terminal width. Tries TIOCGWINSZ first; on failure (e.g.,
// stdout is a pipe) honours $COLUMNS so users can clamp the width
// manually for testing or when running under CI loggers that don't
// propagate winsize. Falls back to 80 cols.
//
// 80 is the right safe default for a "fixed-shape" status line — we'd
// rather collapse the bar than wrap into a second row that `\r\033[2K`
// can't clean up later.
std::size_t terminal_cols() {
    return term::cols();
}

// Truncate a "visible" string (no ANSI codes inside) to `max` chars, replacing
// the last char with `…` when we cut. Used to keep the progress line under
// terminal width without wrapping into a second row.
std::string trunc_visible(std::string s, std::size_t max) {
    if (s.size() <= max) return s;
    if (max == 0) return std::string{};
    if (max == 1) { s.resize(1); return s; }
    s.resize(max - 1);
    s += "…";  // 3-byte UTF-8 char in a single visible column — harmless
    return s;
}

// Indeterminate ("swept") bar: a fixed-width block bounces back and forth so
// the bar animates while the total size is still unknown. `frame` advances with
// elapsed time; the result includes the `[`/`]` brackets so it drops into the
// same layout budget as render_bar().
std::string render_bar_swept(std::size_t frame, std::size_t width = 20) {
    if (width == 0) return "[]";
    std::size_t block = std::min<std::size_t>(3, width);
    std::size_t span  = width - block;            // cells the block can travel
    std::size_t pos   = 0;
    if (span > 0) {
        std::size_t period = span * 2;
        std::size_t p      = frame % period;
        pos = (p <= span) ? p : (period - p);     // ping-pong
    }
    std::string inner(width, ' ');
    for (std::size_t i = 0; i < block && pos + i < width; ++i) inner[pos + i] = '=';
    return "[" + inner + "]";
}

// Shared terminal-width budgeting for a one-line status: composes
//   <verb-padded-12> <label> <bar> <info>
// shrinking the bar first, then truncating the label, so the result is always
// ≤ cols-1 visible chars. `makeBar(innerWidth)` produces the bar string
// (including its `[`/`]` brackets) for the negotiated inner width.
template <class MakeBar>
std::string status_bar_text(std::string_view verb, const std::string& label,
                            MakeBar&& makeBar, const std::string& info_text)
{
    constexpr std::size_t kVerbWidth = 12;
    constexpr std::size_t kBarMax    = 20;
    constexpr std::size_t kBarMin    = 6;

    auto cols = terminal_cols();
    if (cols < 30) cols = 30;             // pathological — give us a chance
    auto budget = cols - 1;               // leave one cell for cursor

    auto fixed = kVerbWidth + 3 + 2 + info_text.size();
    if (fixed >= budget) {
        // Truly tiny terminal — drop the bar entirely.
        auto labelBudget = budget > kVerbWidth + 1 + info_text.size() + 1
                         ? budget - kVerbWidth - 1 - info_text.size() - 1
                         : 0;
        auto lbl = trunc_visible(label, labelBudget);
        return verb_line(kBrightCyan, verb, std::format("{} {}", lbl, info_text));
    }
    auto contentBudget = budget - fixed;   // barInner + visible-label-cols

    std::size_t barW = std::min(kBarMax, contentBudget);
    std::size_t labelMax = contentBudget - barW;
    if (barW < kBarMin && labelMax > 0) {
        auto steal = std::min(kBarMin - barW, labelMax);
        barW += steal;
        labelMax -= steal;
    }
    auto bar = makeBar(barW);
    auto lbl = trunc_visible(label, labelMax);
    return verb_line(kBrightCyan, verb, std::format("{} {} {}", lbl, bar, info_text));
}

// Sets this bar's row of the region and draws it.
void show_bar(const void* who, std::string text) {
    std::lock_guard line(line_mutex());
    auto& bars = region().bars;
    auto it = std::ranges::find_if(bars, [&](auto const& b) { return b.first == who; });
    if (it == bars.end()) bars.emplace_back(who, std::move(text));
    else it->second = std::move(text);
    redraw_locked();
}

// Removes this bar's row; the region is drawn again by the next line.
void drop_bar(const void* who) {
    std::lock_guard line(line_mutex());
    auto& bars = region().bars;
    std::erase_if(bars, [&](auto const& b) { return b.first == who; });
}

} // namespace

ProgressBar::ProgressBar(std::string_view verb, std::string_view label)
    : verb_(verb), label_(label),
      lastDraw_(std::chrono::steady_clock::now() - std::chrono::seconds(1)),
      start_(std::chrono::steady_clock::now())
{}

void ProgressBar::announce(std::size_t total_bytes) {
    if (announced_) return;
    announced_ = true;
    if (total_bytes > lastBytes_) lastBytes_ = total_bytes;
}

std::string ProgressBar::completion(std::string_view final_message) const {
    const auto secs = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start_).count();
    if (lastBytes_ > 0)
        return std::format("{} done, {} in {:.1f}s", final_message, fmt_bytes(lastBytes_), secs);
    return std::format("{} done in {:.1f}s", final_message, secs);
}

void ProgressBar::finish_plain(std::string_view final_message) {
    info(verb_, completion(final_message));
}

ProgressBar::~ProgressBar() {
    if (!finished_) finish();
}

// Render a single progress-bar frame as this bar's row of the region. The
// verb is coloured separately so the ANSI escapes stay out of the truncation
// budget.
//
// Layout (visible chars only): <verb-padded-12> <label> <bar> <info>.
// The width budgeting lives in status_bar_text(); this just supplies a
// percentage-fill bar for the negotiated inner width.
void ProgressBar::render_line(std::size_t pct, const std::string& info_text)
{
    init();
    show_bar(this, status_bar_text(verb_, label_,
                                   [pct](std::size_t w) { return render_bar(pct, w); },
                                   info_text));
}

// Same layout, but an animated swept/indeterminate bar (used while the total
// download size is still unknown).
void ProgressBar::render_line_swept(std::size_t frame, const std::string& info_text)
{
    init();
    show_bar(this, status_bar_text(verb_, label_,
                                   [frame](std::size_t w) { return render_bar_swept(frame, w); },
                                   info_text));
}

void ProgressBar::update(std::size_t percent) {
    if (g_quiet || finished_) return;
    if (!bars_live()) { announce(0); return; }
    auto now = std::chrono::steady_clock::now();
    if (now - lastDraw_ < std::chrono::milliseconds(80) && percent < 100) return;
    lastDraw_ = now;
    render_line(percent, std::format("{}%", percent));
}

void ProgressBar::update_bytes(std::size_t current, std::size_t total,
                               double elapsed_sec) {
    if (g_quiet || finished_) return;
    if (!bars_live()) { announce(total); if (current > lastBytes_) lastBytes_ = current; return; }
    if (current > lastBytes_) lastBytes_ = current;
    auto now = std::chrono::steady_clock::now();
    auto pct = total ? (current * 100 / total) : 0;
    if (pct > 100) pct = 100;
    // Same throttle as update(): one render per ~80ms unless we hit 100%.
    if (now - lastDraw_ < std::chrono::milliseconds(80) && pct < 100) return;
    lastDraw_ = now;

    auto info = std::format("{} / {}", fmt_bytes(current), fmt_bytes(total));
    // Average rate since the download started. xlings only ships the
    // cumulative `elapsedSec`, so this is "since-start" rather than
    // a sliding-window instantaneous speed — accurate enough for UX.
    if (elapsed_sec > 0.5 && current > 0) {
        auto rate = static_cast<std::size_t>(
            static_cast<double>(current) / elapsed_sec);
        info += std::format("  {}/s", fmt_bytes(rate));
    }
    render_line(pct, info);
}

void ProgressBar::update_indeterminate(std::size_t current_bytes,
                                       double elapsed_sec) {
    if (g_quiet || finished_) return;
    if (!bars_live()) { announce(0); lastBytes_ = current_bytes; return; }
    lastBytes_ = current_bytes;
    auto now = std::chrono::steady_clock::now();
    // Same ~80ms throttle as update_bytes(); there is no "100%" early-out here
    // because there is no known total.
    if (now - lastDraw_ < std::chrono::milliseconds(80)) return;
    lastDraw_ = now;

    // Before any byte arrives we only have "connecting…"; once the body starts
    // streaming (no Content-Length) show the running byte count instead. The
    // ticking elapsed-seconds suffix proves the line is alive either way.
    std::string info = current_bytes > 0 ? fmt_bytes(current_bytes)
                                         : std::string{"connecting…"};
    if (elapsed_sec > 0)
        info += std::format("  {:.0f}s", elapsed_sec);

    auto frame = static_cast<std::size_t>(elapsed_sec * 6.0);
    render_line_swept(frame, info);
}

void ProgressBar::finish() {
    if (finished_) return;
    finished_ = true;
    drop_bar(this);
    if (g_quiet) return;
    if (!bars_live()) announce(0);
    // The bar's row is replaced by the completion line in both modes.
    finish_plain(label_);
}

void ProgressBar::finish_with(std::string_view final_message) {
    if (finished_) return;
    finished_ = true;
    drop_bar(this);
    if (g_quiet) return;
    if (!bars_live()) announce(0);
    finish_plain(final_message);
}

void ProgressBar::finish_failed(std::string_view final_message) {
    if (finished_) return;
    finished_ = true;
    drop_bar(this);
    if (g_quiet) return;
    if (!bars_live()) announce(0);
    const auto secs = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start_).count();
    info(verb_, std::format("{} did not complete ({:.1f}s)", final_message, secs));
}

// --- DownloadProgress ---

DownloadProgress::~DownloadProgress() { finish(); }

void DownloadProgress::finish() {
    if (bar_) bar_->finish();
    bar_.reset();
    active_.clear();
}

void DownloadProgress::finish_failed() {
    if (bar_) bar_->finish_failed(active_);
    bar_.reset();
    active_.clear();
}

void DownloadProgress::update(std::span<const DownloadFile> files,
                              double elapsed_sec) {
    if (files.empty()) return;

    // 1. Retire newly-finished entries. Each file's finished=true is reported
    //    twice (xlings quirk); the `finished_` set dedupes that and the case
    //    where the same file reappears at a different slot in a later event.
    for (auto& f : files) {
        if (finished_.contains(f.name)) continue;
        if (!f.finished) continue;
        if (active_ == f.name) {
            if (bar_) bar_->finish();
            bar_.reset();
            active_.clear();
        }
        finished_.insert(f.name);
    }

    // 2. Pick what to display: keep showing `active_` if it's still streaming,
    //    else the first started+unfinished file. This stops the bar from
    //    flickering between names when files[] reshuffles across events during
    //    a multi-package install.
    const DownloadFile* current = nullptr;
    for (auto& f : files) {
        if (f.name == active_ && !f.finished && !finished_.contains(f.name)) {
            current = &f;
            break;
        }
    }
    if (!current) {
        for (auto& f : files) {
            if (finished_.contains(f.name)) continue;
            if (f.started && !f.finished) { current = &f; break; }
        }
    }
    if (!current) return;

    if (current->name != active_) {
        if (bar_) bar_->finish();
        active_ = current->name;
        bar_.emplace("Downloading", current->name);
    }
    if (current->total > 0) {
        bar_->update_bytes(current->downloaded, current->total, elapsed_sec);
    } else {
        // Size not known yet (connecting / no Content-Length): keep the line
        // animated instead of frozen.
        bar_->update_indeterminate(current->downloaded, elapsed_sec);
    }
}

std::string shorten_path(const std::filesystem::path& p, const PathContext& ctx) {
    namespace fs = std::filesystem;
    // Use a pure string-prefix comparison rather than fs::relative —
    // fs::relative internally canonicalises both arguments, which would
    // resolve symlinks. We want to display the path the user thinks they
    // are working with (e.g. `<MCPP_HOME>/registry/data/xpkgs/<pkg>` even
    // when xpkgs/ is symlinked to a system xlings cache), so we keep
    // every comparison purely lexical.
    auto can = p.lexically_normal().generic_string();

    auto rel_to = [&](const fs::path& base) -> std::optional<std::string> {
        if (base.empty()) return std::nullopt;
        auto bs = base.lexically_normal().generic_string();
        // Strip trailing slashes on the base so "/x/y" and "/x/y/" match
        // the same set of candidate paths.
        while (!bs.empty() && bs.back() == '/') bs.pop_back();
        if (bs.empty()) return std::nullopt;
        if (can == bs) return std::string{};
        if (can.size() > bs.size()
            && can.compare(0, bs.size(), bs) == 0
            && can[bs.size()] == '/') {
            return can.substr(bs.size() + 1);
        }
        return std::nullopt;
    };

    if (auto r = rel_to(ctx.project_root); r) {
        // Project-relative — print bare ("target/release/foo"), no prefix.
        return r->empty() ? std::string{"."} : *r;
    }
    if (auto r = rel_to(ctx.mcpp_home); r) {
        return r->empty() ? std::string{"@mcpp"} : "@mcpp/" + *r;
    }
    if (auto r = rel_to(ctx.home); r) {
        return r->empty() ? std::string{"~"} : "~/" + *r;
    }
    return can;
}

} // namespace mcpp::ui
