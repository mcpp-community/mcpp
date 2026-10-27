// mcpp.ui.terminal over mcpplibs.os.
module;
#include <cerrno>
#include <cstdio>     // stdout, stderr: macros, which `import std` does not carry

module mcpp.ui.terminal;

import std;
import mcpplibs.os.platform;
import mcpplibs.os.terminal;
import mcpplibs.os.stdio;
import mcpplibs.os.windows;
import mcpp.invocation;
import mcpp.run;

namespace mcpp::ui::terminal {

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
    if (mcpp::invocation::get("TERM") == "dumb") return false;
    // A POSIX terminal is taken to obey them; a console is asked, and is
    // switched to virtual terminal processing by the asking.
    if constexpr (os::is_windows) return os::terminal::sequences(stream_of(s));
    else return true;
}

namespace {
std::size_t from_env(const char* name, std::size_t fallback) {
    if (auto e = mcpp::invocation::get(name)) {
        try { auto n = std::stoul(*e); if (n > 0) return n; } catch (...) {}
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
    if constexpr (os::is_windows) {
        if (is_terminal(s)) {
            std::fflush(file_of(s));
            if (write_through_os(s, text)) return;
        }
    }
    errno = 0;
    std::fwrite(text.data(), 1, text.size(), file_of(s));
    // A standard output that has lost its reader (`mcpp … | head`) ends mcpp
    // in order at the first line it refuses (design 2026-10-26 §25.5).
    if (s == Stream::Out && std::ferror(stdout) && errno == EPIPE) mcpp::run::end_for_closed_output();
}

void write_frame(Stream s, std::string_view bytes) {
    if (bytes.empty()) return;
    std::fflush(stdout);
    std::fflush(stderr);
    // One write of the whole frame (repeated by the library only for a
    // partial write or an interruption); on Windows only to a console, the
    // C runtime's text mode keeping the line ends of anything else.
    if (!os::is_windows || is_terminal(s))
        if (write_through_os(s, bytes)) return;
    std::fwrite(bytes.data(), 1, bytes.size(), file_of(s));
    std::fflush(file_of(s));
}

bool same_terminal() {
    return is_terminal(Stream::Out) && is_terminal(Stream::Err);
}

bool ambiguous_wide(Stream s) {
    if constexpr (os::is_windows) {
        const unsigned cp = os::windows::console_output_code_page(s == Stream::Err);
        if (cp == 932 || cp == 936 || cp == 949 || cp == 950) return true;
    }
    for (const char* name : {"LC_ALL", "LC_CTYPE", "LANG"}) {
        const auto v = mcpp::invocation::get(name);
        if (!v) continue;
        const std::string_view l(*v);
        return l.starts_with("zh") || l.starts_with("ja") || l.starts_with("ko");
    }
    return false;
}

bool unicode_capable() {
    if constexpr (os::is_windows) {
        for (const char* name : {"WT_SESSION", "TERM_PROGRAM"})
            if (mcpp::invocation::get(name)) return true;
        return false;
    } else {
        for (const char* name : {"LC_ALL", "LC_CTYPE", "LANG"}) {
            const auto v = mcpp::invocation::get(name);
            if (!v) continue;
            std::string l(*v);
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

// ─── Keys for a game, over mcpplibs.os ───────────────────────────────────
//
// The terminal in raw mode that keeps the interrupting keystroke, and reads that
// never wait. On Windows the arrows arrive as the same sequences, a console
// in raw mode reporting virtual-terminal input.

struct KeyInput::State {
    std::optional<os::terminal::raw_mode> mode;
    int cleanup = -1;
};

KeyInput::KeyInput(Stream display) : state_(std::make_unique<State>()) {
    if (!is_terminal(display) || !os::terminal::foreground()) return;
    auto mode = os::terminal::raw_mode::enter({ .keep_interrupt = true });
    if (!mode) return;
    state_->mode = std::move(*mode);
    // A request to end mcpp while the game plays gives the terminal back first.
    State* s = state_.get();
    state_->cleanup = mcpp::run::add_cleanup([s] { s->mode.reset(); });
    active_ = true;
}

KeyInput::~KeyInput() {
    if (!state_) return;
    if (state_->cleanup >= 0) mcpp::run::remove_cleanup(state_->cleanup);
    state_->mode.reset();
}

std::vector<Key> KeyInput::read() {
    std::vector<Key> keys;
    if (!active_) return keys;
    for (;;) {
        auto chunk = os::stdio::read_input_for(std::chrono::milliseconds(0), 64);
        if (!chunk || !chunk->has_value() || (*chunk)->empty()) break;
        pending_ += **chunk;
    }
    return decode_keys(pending_);
}

// ─── Standard output sent to standard error ──────────────────────────────

namespace {
std::mutex g_protocolLock;
os::stdio::output_to_error* g_protocol = nullptr;   // the active diversion, if any
}  // namespace

struct StdoutToStderr::State {
    std::optional<os::stdio::output_to_error> diversion;
};

void write_protocol_stdout(std::string_view text) {
    std::lock_guard g(g_protocolLock);
    if (g_protocol) {
        (void)g_protocol->write_original(text);
        return;
    }
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

StdoutToStderr::StdoutToStderr() : state_(std::make_unique<State>()) {
    std::fflush(stdout);
    auto d = os::stdio::output_to_error::begin();
    if (!d) return;
    state_->diversion = std::move(*d);
    std::lock_guard g(g_protocolLock);
    if (!g_protocol) g_protocol = &*state_->diversion;
}

StdoutToStderr::~StdoutToStderr() {
    std::fflush(stdout);
    if (!state_ || !state_->diversion) return;
    {
        std::lock_guard g(g_protocolLock);
        if (g_protocol == &*state_->diversion) g_protocol = nullptr;
    }
    state_->diversion.reset();
}

} // namespace mcpp::ui::terminal
