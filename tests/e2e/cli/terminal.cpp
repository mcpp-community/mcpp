// A build on a terminal: the status row names the action that runs, and `--play-game` restores the
// terminal however the build ends (formerly tests/e2e/843, 845).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

std::u32string decode(std::string_view b) {
    std::u32string out;
    for (std::size_t i = 0; i < b.size();) {
        const auto c = static_cast<unsigned char>(b[i]);
        const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > b.size()) {
            out.push_back(0xfffd);
            ++i;
            continue;
        }
        char32_t cp = n == 1 ? c : c & (0x7f >> n);
        for (int k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(b[i + k]) & 0x3f);
        out.push_back(cp);
        i += n;
    }
    return out;
}

std::string encode(std::u32string_view s) {
    std::string out;
    for (char32_t c : s) {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
        }
    }
    return out;
}

// The screen the terminal shows after `bytes`: carriage return, line feed, cursor up (`CSI n A`),
// erase to the end of the screen (`CSI J`) and of the line (`CSI K`); colour is ignored. Rows
// without their trailing blanks.
std::vector<std::string> screen_of(std::string_view bytes) {
    const auto s = decode(bytes);
    std::vector<std::u32string> rows{U""};
    std::size_t r = 0, c = 0, i = 0;
    while (i < s.size()) {
        const char32_t ch = s[i];
        if (ch == 0x1b && i + 1 < s.size() && s[i + 1] == U'[') {
            std::size_t j = i + 2;
            while (j < s.size() && !(s[j] >= U'@' && s[j] <= U'~')) ++j;
            const std::u32string params = s.substr(i + 2, j - (i + 2));
            const char32_t final = j < s.size() ? s[j] : 0;
            const bool digits = !params.empty() && std::ranges::all_of(params, [](char32_t d) { return d >= U'0' && d <= U'9'; });
            std::size_t n = 1;
            if (digits) {
                n = 0;
                for (char32_t d : params) n = n * 10 + (d - U'0');
            }
            if (final == U'A') {
                r = r >= n ? r - n : 0;
            } else if (final == U'J') {
                rows[r] = rows[r].substr(0, c);
                rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(r) + 1, rows.end());
            } else if (final == U'K') {
                rows[r] = params == U"2" ? U"" : rows[r].substr(0, c);
            }
            i = j + 1;
            continue;
        }
        if (ch == U'\r') {
            c = 0;
        } else if (ch == U'\n') {
            ++r;
            while (rows.size() <= r) rows.emplace_back();
        } else {
            auto& line = rows[r];
            if (line.size() < c) line.resize(c, U' ');
            if (line.size() == c) line.push_back(ch);
            else line[c] = ch;
            ++c;
        }
        ++i;
    }
    std::vector<std::string> out;
    for (auto& row : rows) {
        while (!row.empty() && (row.back() == U' ' || row.back() == U'\t')) row.pop_back();
        out.push_back(encode(row));
    }
    return out;
}

bool matches(const std::string& text, std::string_view pattern) {
    return std::regex_search(text, std::regex{std::string(pattern)});
}

// A run of `n` braille cells (U+2800..U+28FF).
bool braille_run(std::string_view text, std::size_t n) {
    std::size_t run = 0;
    for (char32_t c : decode(text)) {
        run = (c >= 0x2800 && c <= 0x28ff) ? run + 1 : 0;
        if (run >= n) return true;
    }
    return false;
}

std::vector<std::string> split_rows(std::string_view text) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : text) {
        if (ch == '\r' || ch == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    out.push_back(cur);
    return out;
}

constexpr std::string_view slow_install = R"cpp(import mcpp;
#include <string>
int main() {
    const std::string prefix = std::string(mcpp::out_dir()) + "/tool";
    mcpp::action a;
    a.id   = "app:install";
    a.role = mcpp::roles::prepare;
    a.arg("python3").arg("-c")
     .arg("import os, sys, time; time.sleep(4); os.makedirs(sys.argv[1], exist_ok=True); open(os.path.join(sys.argv[1], \"ready\"), \"w\").close()")
     .arg(prefix.c_str())
     .output((prefix + ".stamp").c_str())
     .output_dir(prefix.c_str())
     .submit();
    return 0;
}
)cpp";

s::project app() {
    return s::project({{"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                       {"src/main.cpp", "int main() { return 0; }\n"},
                       {"build.mcpp", slow_install}});
}

// mcpp `args` in `p` on a terminal of 40 rows and 120 columns: its session's controlling
// terminal, all three standard streams (or standard input from /dev/null).
s::spawn on_terminal(const s::project& p, std::vector<std::string> args,
                     std::vector<std::pair<std::string, std::optional<std::string>>> env, bool stdin_null = false) {
    auto c = s::mcpp_command(std::move(args), p.dir());
    s::spawn sp{.argv = c.argv, .cwd = p.dir(), .env = c.env};
    sp.env.insert(sp.env.end(), env.begin(), env.end());
    sp.in = stdin_null ? s::stream::null : s::stream::terminal;
    sp.out = sp.err = s::stream::terminal;
    sp.session = true;
    return sp;
}

// The words of `stty -g` in `file`, the kernel's PENDIN (0x20000000 in a BSD `gfmt1` lflag) left
// out: macOS sets it whenever ICANON is turned back on and the next read clears it, so it is not
// a mode mcpp left behind. Empty when the file is absent.
std::string stty_mode(const fs::path& file) {
    if (!fs::is_regular_file(file)) return {};
    auto g = s::read_file(file);
    while (!g.empty() && (g.back() == '\n' || g.back() == '\r')) g.pop_back();
    if (!g.starts_with("gfmt1:")) return g;
    std::string out;
    for (auto part : std::views::split(std::string_view(g), ':')) {
        std::string f(part.begin(), part.end());
        if (f.starts_with("lflag="))
            f = std::format("lflag={:x}", std::stoul(f.substr(6), nullptr, 16) & ~0x20000000ul);
        out += (out.empty() ? "" : ":") + f;
    }
    return out;
}

// `--play-game=snake` in `p` through a shell in the terminal's session, which records the mode
// before and after mcpp (`stty -g`): on macOS the terminal is revoked for everyone else once the
// session leader exits, so the shell, still in the session, reads it. The shell traps SIGINT (a
// trap, not an ignore, so mcpp starts with the default action) and exits with mcpp's status.
struct played {
    int status = -1;
    std::string plain;
    std::optional<s::terminal_mode> during;
    std::string before, after;
};
played play(const s::project& p, bool interrupt) {
    fs::remove(p.at("before.txt"));
    fs::remove(p.at("after.txt"));
    auto sp = on_terminal(p, {"build", "--play-game=snake"}, {{"TERM", "xterm-256color"}, {"LANG", "C.UTF-8"}, {"NO_COLOR", std::nullopt}, {"LC_ALL", std::nullopt}});
    const auto mcpp = sp.argv;
    sp.argv = {"sh", "-c", "trap : INT; stty -g > before.txt; \"$0\" \"$@\"; code=$?; stty -g > after.txt; exit $code"};
    sp.argv.insert(sp.argv.end(), mcpp.begin(), mcpp.end());
    auto proc = s::process::start(sp);
    played out;
    // The keys go when the status row that carries the score first appears: the frame the
    // build drew, not a wall-clock delay, which sent them into the Programs phase whenever the
    // build program took that long. A build that never draws the row is a failure, not a hang.
    proc.wait_for(R"(Building .* · snake \d+)", std::chrono::seconds{60});
    out.during = proc.mode();
    for (std::string_view key : {"\x1b[B", "\x1b[C", "\x1b[A"}) {
        proc.send(key);
        std::this_thread::sleep_for(std::chrono::milliseconds{150});
    }
    if (interrupt) proc.send("\x03");
    out.status = proc.wait();
    out.plain = s::strip_escapes(proc.terminal());
    std::erase(out.plain, '\r');
    out.before = stty_mode(p.at("before.txt"));
    out.after = stty_mode(p.at("after.txt"));
    return out;
}

// The processes still running the prepare action of the project at `dir` (`ps`, on Linux and
// macOS): its command line names `time.sleep(4)` and a path under the project.
std::vector<std::string> prepare_actions_alive(const fs::path& dir) {
    auto r = e2e::run({.argv = {s::on_path("ps").value_or("/bin/ps").string(), "-axo", "pid=,command="}});
    std::vector<std::string> alive;
    for (auto& line : split_rows(r.out))
        if (line.contains("time.sleep(4)") && line.contains(dir.string())) alive.push_back(line);
    return alive;
}

E2E terminal{"terminal", [] {
    "843 a terminal names the action that runs"_test[s::python3][s::unix_shell] = [] {
        // T1 the status row names a running prepare action, its package and clock; T2 the
        // package once, a blank line before Finished; T3 no status row after Finished; T4 the
        // phase in 12 columns and a 24-cell screen; T5 the status row is the last row and no
        // blank row precedes it.
        auto p = app();
        auto proc = s::process::start(on_terminal(p, {"build"}, {{"TERM", "xterm-256color"}, {"LANG", "C.UTF-8"}, {"MCPP_PROGRESS", "snake"},
                                                                 {"NO_COLOR", std::nullopt}, {"LC_ALL", std::nullopt}}));
        const int status = proc.wait();
        const auto& text = proc.terminal();
        const auto plain = s::strip_escapes(text);
        const auto screen = screen_of(text);
        expect(fatal(status == 0)) << "the build failed\n" << plain;
        expect(fatal(matches(plain, R"(app: app:install \d+:\d\d)"))) << "T1: no status line named the running action\n" << plain;
        std::optional<std::size_t> at;
        for (std::size_t i = 0; i < screen.size(); ++i)
            if (screen[i].contains("Finished")) at = i;
        expect(fatal(at && *at > 0 && screen[*at - 1].empty())) << "T2: no blank line precedes Finished" << screen;
        expect(fatal(std::ranges::count(screen, std::string("   Compiling app v0.1.0 (.)")) == 1)) << "T2: the package is not named exactly once" << screen;
        for (std::size_t i = *at; i < screen.size(); ++i)
            expect(fatal(!screen[i].contains("Building") && !screen[i].contains("Checking"))) << "T3: the status line was drawn after Finished" << screen;
        std::vector<std::string> rows;
        for (auto& r : split_rows(plain))
            if (matches(r, "^ *(Planning|Running|Building|Checking) ")) rows.push_back(r);
        expect(fatal(!rows.empty())) << "T4: no status row was drawn";
        expect(fatal(std::ranges::all_of(rows, [](const std::string& r) { return matches(r, "^( {4}(Planning|Building|Checking) | {5}Running )"); })))
            << "T4: a status row is not aligned with the verbs" << rows;
        expect(fatal(std::ranges::any_of(rows, [](const std::string& r) { return braille_run(r, 24); }))) << "T4: no status row carries the 24-cell screen" << rows;
        int checked = 0;
        for (std::size_t pos = text.find("\x1b[?7h"); pos != std::string::npos; pos = text.find("\x1b[?7h", pos + 1)) {
            auto now = screen_of(std::string_view(text).substr(0, pos + 5));
            while (!now.empty() && now.back().empty()) now.pop_back();
            expect(fatal(!now.empty() && matches(now.back(), "^ *(Planning|Running|Building|Stopping|Checking) ")))
                << "T5: the last row after a frame is not the status row" << now;
            if (now.size() > 1)
                expect(fatal(!now[now.size() - 2].empty())) << "T5: a blank row precedes the status row" << now;
            ++checked;
        }
        expect(fatal(checked >= 3)) << "T5: only " << checked << " frames were drawn";
    };
    "845 play game restores the terminal"_test[s::python3][s::unix_shell] = [] {
        // Build output design 2026-09-30 (revision 3) §5.14, through a pseudo-terminal, with a
        // prepare action that takes four seconds. G1 while the game plays the terminal reads keys
        // without echo and without a line end, and the status row carries the score; G2 after
        // the build the terminal's mode is the mode it had, and a line after Finished states the
        // best round; G3 a Ctrl-C during the game ends mcpp by SIGINT, restores the mode, and
        // the prepare action does not outlive mcpp; G4 with standard input not a terminal the
        // game is off, a line says why, and the build succeeds.
        auto p = app();
        auto g = play(p, false);
        expect(fatal(g.during.has_value())) << "the build ended before the game could be observed\n" << g.plain;
        expect(fatal(!g.during->echo)) << "G1: echo stayed on during the game\n" << g.plain;
        expect(fatal(!g.during->canonical)) << "G1: the terminal still waited for a line end\n" << g.plain;
        expect(fatal(matches(g.plain, R"(Building .* · snake \d+)"))) << "G1: no status row carries the score\n" << g.plain;
        expect(fatal(!g.before.empty() && g.after == g.before)) << "G2: the terminal's mode was not restored: " << g.before << " -> " << g.after;
        expect(fatal(g.status == 0)) << "G2: the build failed (" << g.status << ")\n" << g.plain;
        const auto rows = split_rows(g.plain);
        std::optional<std::size_t> finished, best;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (!finished && rows[i].contains("Finished")) finished = i;
            if (!best && matches(rows[i], R"(^ +Played snake · best \d+$)")) best = i;
        }
        expect(fatal(best.has_value())) << "G2: no line states the best round\n" << g.plain;
        expect(fatal(finished && *best > *finished)) << "G2: the best round is not stated after Finished\n" << g.plain;

        fs::remove_all(p.at("target"));
        g = play(p, true);
        expect(fatal(!g.before.empty() && g.after == g.before)) << "G3: the terminal's mode was not restored: " << g.before << " -> " << g.after;
        expect(fatal(g.status == 130)) << "G3: mcpp did not end by SIGINT (the shell reports " << g.status << ")\n" << g.plain;
        std::this_thread::sleep_for(std::chrono::milliseconds{500});
        const auto alive = prepare_actions_alive(p.dir());
        expect(fatal(alive.empty())) << "G3: the prepare action outlived mcpp" << alive;

        fs::remove_all(p.at("target"));
        auto r = p.mcpp({"build", "--play-game"}, {.input = ""});
        expect(fatal(e2e::succeeded(r))) << "G4: the build failed: " << s::text(r);
        auto proc = s::process::start(on_terminal(p, {"build", "--play-game"}, {{"LANG", "C.UTF-8"}, {"LC_ALL", std::nullopt}}, true));
        const int status = proc.wait();
        const auto plain = s::strip_escapes(proc.terminal());
        expect(fatal(status == 0)) << "G4: the build failed\n" << plain;
        expect(fatal(plain.contains("standard input is not a terminal"))) << "G4: no line says why the game is off\n" << plain;
        expect(fatal(!plain.contains("Played"))) << "G4: a game was played without keys\n" << plain;
    };
}};

}  // namespace
