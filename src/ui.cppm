// mcpp.ui — verb-style colored status output.
//
// All user-visible status lines from CLI / fetcher / build go through
// here. TTY auto-detect; MCPP_NO_COLOR / --no-color disables colors.

module;
#include <cstdio>      // isatty, fileno, stdout
#ifdef __unix__
#include <unistd.h>
#include <sys/ioctl.h>
#endif

export module mcpp.ui;

import std;

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

// Bold green Finished line.
void finished(std::string_view profile, std::chrono::milliseconds elapsed);

// "warning:" / "error:" prefix lines (yellow / red).
void warning(std::string_view message);
void error(std::string_view message);

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

// --- progress bar (single-line, \r-rewritten) ---
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

    // Finish: replaces progress with final-state line.
    void finish();
    void finish_with(std::string_view final_message);

private:
    void render_line(std::size_t percent, const std::string& info_text);

    std::string verb_;
    std::string label_;
    std::chrono::steady_clock::time_point lastDraw_;
    bool finished_ = false;
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

bool g_color  = false;
bool g_quiet  = false;
bool g_inited = false;

constexpr std::string_view kReset      = "\033[0m";
constexpr std::string_view kBold       = "\033[1m";
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
#ifdef __unix__
    return ::isatty(::fileno(stdout)) != 0;
#else
    return false;
#endif
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

} // namespace

void init() {
    if (g_inited) return;
    g_color  = detect_color();
    g_inited = true;
}

void disable_color() { g_color = false; }
bool is_color_enabled() { return g_color; }

void set_quiet(bool q) { g_quiet = q; }
bool is_quiet()        { return g_quiet; }

void status(std::string_view verb, std::string_view message) {
    if (g_quiet) return;
    init();
    auto v = verb_padded(verb);
    if (g_color) {
        std::println("{}{}{}{} {}",
                     kBold, kBrightGreen, v, kReset, message);
    } else {
        std::println("{} {}", v, message);
    }
}

void info(std::string_view verb, std::string_view message) {
    if (g_quiet) return;
    init();
    auto v = verb_padded(verb);
    if (g_color) {
        std::println("{}{}{}{} {}",
                     kBold, kBrightCyan, v, kReset, message);
    } else {
        std::println("{} {}", v, message);
    }
}

void finished(std::string_view profile, std::chrono::milliseconds elapsed) {
    if (g_quiet) return;
    init();
    auto v = verb_padded("Finished");
    auto secs = static_cast<double>(elapsed.count()) / 1000.0;
    auto msg  = std::format("{} [optimized] in {:.2f}s", profile, secs);
    if (g_color) {
        std::println("{}{}{}{} {}",
                     kBold, kBrightGreen, v, kReset, msg);
    } else {
        std::println("{} {}", v, msg);
    }
}

void warning(std::string_view message) {
    init();
    if (g_color) {
        std::println(stderr, "{}{}warning:{} {}", kBold, kYellow, kReset, message);
    } else {
        std::println(stderr, "warning: {}", message);
    }
}

void error(std::string_view message) {
    init();
    if (g_color) {
        std::println(stderr, "{}{}error:{} {}", kBold, kBrightRed, kReset, message);
    } else {
        std::println(stderr, "error: {}", message);
    }
}

void plain(std::string_view message) {
    if (g_quiet) return;
    std::println("{}", message);
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
    std::string head = "error";
    if (!d.code.empty()) head += "[" + d.code + "]";
    head += ":";
    std::println(stderr, "{} {}", bold_red(head), d.title);

    if (!d.path.empty()) {
        if (d.line)
            std::println(stderr, "  {} {}:{}{}",
                blue("-->"), d.path.string(), d.line,
                d.column ? std::format(":{}", d.column) : "");
        else
            std::println(stderr, "  {} {}", blue("-->"), d.path.string());
    }

    if (!d.sourceLine.empty()) {
        std::println(stderr, "   {}", blue("|"));
        std::println(stderr, " {} {} {}",
            d.line ? std::format("{:>2}", d.line) : "  ", blue("|"), d.sourceLine);
        if (!d.spanMessage.empty()) {
            std::string caret(d.column ? d.column - 1 : 0, ' ');
            caret += "^";
            std::println(stderr, "   {} {} {}", blue("|"), caret, d.spanMessage);
        }
    }

    if (!d.notes.empty() || !d.helps.empty()) {
        std::println(stderr, "   {}", blue("|"));
    }
    for (auto& n : d.notes) {
        std::println(stderr, "   {} {}: {}", blue("="), blue("note"), n);
    }
    for (auto& h : d.helps) {
        std::println(stderr, "   {} {}: {}", blue("="), blue("help"), h);
    }
    if (!d.code.empty()) {
        std::println(stderr, "");
        std::println(stderr, "For more information on this error: `mcpp --explain {}`",
                     d.code);
    }
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

} // namespace

ProgressBar::ProgressBar(std::string_view verb, std::string_view label)
    : verb_(verb), label_(label),
      lastDraw_(std::chrono::steady_clock::now() - std::chrono::seconds(1))
{}

ProgressBar::~ProgressBar() {
    if (!finished_) finish();
}

// Render a single progress-bar frame. The verb is drawn separately (with
// optional color) so we can keep ANSI escapes out of the truncation budget.
// `cols` is the available terminal width; `info_text` is the trailing
// "%" / "X MB / Y MB / Z MB/s" suffix; `pct` drives the bar fill.
//
// Layout (visible chars only):
//   <verb-padded-12> <label> <bar> <info>
//
// The bar shrinks first when we run out of room, then `label` is truncated
// with an ellipsis. Result is always ≤ cols-1 chars so a `\r\033[2K{...}`
// write never wraps into a second row.
void ProgressBar::render_line(std::size_t pct, const std::string& info_text)
{
    init();
    constexpr std::size_t kVerbWidth = 12;
    constexpr std::size_t kBarMax    = 20;
    constexpr std::size_t kBarMin    = 6;

    auto cols = terminal_cols();
    if (cols < 30) cols = 30;             // pathological — give us a chance
    auto budget = cols - 1;               // leave one cell for cursor

    // Visible layout, accounting for `[…]` bracket chars on the bar:
    //   <verb-padded-12><space><label><space>[<bar-inner>]<space><info>
    //
    // Fixed cost = verbWidth + 3 spaces + 2 brackets + info.
    // Whatever's left in `contentBudget` is split between bar-inner and label.
    auto fixed = kVerbWidth + 3 + 2 + info_text.size();
    if (fixed >= budget) {
        // Truly tiny terminal — drop the bar entirely.
        auto labelBudget = budget > kVerbWidth + 1 + info_text.size() + 1
                         ? budget - kVerbWidth - 1 - info_text.size() - 1
                         : 0;
        auto lbl = trunc_visible(label_, labelBudget);
        if (g_color) {
            std::print("\r\033[2K{}{}{}{} {} {}",
                       kBold, kBrightCyan, verb_padded(verb_), kReset,
                       lbl, info_text);
        } else {
            std::print("\r\033[2K{} {} {}", verb_padded(verb_), lbl, info_text);
        }
        std::fflush(stdout);
        return;
    }
    auto contentBudget = budget - fixed;   // barInner + visible-label-cols

    std::size_t barW = std::min(kBarMax, contentBudget);
    std::size_t labelMax = contentBudget - barW;
    if (barW < kBarMin && labelMax > 0) {
        // Steal from label to keep at least a tiny bar.
        auto steal = std::min(kBarMin - barW, labelMax);
        barW += steal;
        labelMax -= steal;
    }
    auto bar = render_bar(pct, barW);
    auto lbl = trunc_visible(label_, labelMax);

    if (g_color) {
        std::print("\r\033[2K{}{}{}{} {} {} {}",
                   kBold, kBrightCyan, verb_padded(verb_), kReset,
                   lbl, bar, info_text);
    } else {
        std::print("\r\033[2K{} {} {} {}",
                   verb_padded(verb_), lbl, bar, info_text);
    }
    std::fflush(stdout);
}

void ProgressBar::update(std::size_t percent) {
    if (g_quiet || finished_) return;
    auto now = std::chrono::steady_clock::now();
    if (now - lastDraw_ < std::chrono::milliseconds(80) && percent < 100) return;
    lastDraw_ = now;
    render_line(percent, std::format("{}%", percent));
}

void ProgressBar::update_bytes(std::size_t current, std::size_t total,
                               double elapsed_sec) {
    if (g_quiet || finished_) return;
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

void ProgressBar::finish() {
    if (finished_) return;
    finished_ = true;
    if (g_quiet) return;
    // Clear the line and re-emit as a static info line.
    std::print("\r\033[2K");
    info(verb_, label_);
}

void ProgressBar::finish_with(std::string_view final_message) {
    if (finished_) return;
    finished_ = true;
    if (g_quiet) return;
    std::print("\r\033[2K");
    info(verb_, final_message);
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
