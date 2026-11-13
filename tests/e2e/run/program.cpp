// What `run` hands back of the program: its exit status, 126 and 2 for a runner that cannot
// start or is missing, 101 for a build that failed, exactly its standard output under -q, and
// the terminal (formerly tests/e2e/339, 862, 863, 883).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

std::optional<fs::path> bin_of(const fs::path& root, std::string_view name) {
    auto all = s::find_files(root, std::regex_replace(s::exe(name), std::regex{R"([.])"}, R"(\.)"));
    std::ranges::sort(all);
    for (const auto& f : all)
        if (f.generic_string().contains("/bin/")) return f;
    return std::nullopt;
}

std::string host_triple(const s::project& p) {
    std::vector<std::string> triples;
    for (const auto& e : fs::directory_iterator(p.at("target")))
        if (!e.path().filename().string().starts_with(".")) triples.push_back(e.path().filename().string());
    std::ranges::sort(triples);
    expect(fatal(!triples.empty())) << "no target directory to read the host triple from";
    return triples.front();
}

E2E program{"program", [] {
    "339 run reports the program status"_test[tag{"requires:gcc"}][tag{"requires:unix-shell"}] = [] {
        // `run` exits with the program's status (127 too, without an error line of its own); a
        // runner that cannot be executed exits 126 saying why; a missing runner exits 2 or 127.
        s::needs({"gcc", "unix-shell"});
        s::project p({{"mcpp.toml", "[package]\nname    = \"status\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/main.cpp\"]\n"}});
        auto status_of = [&](int v) {
            p.write("src/main.cpp", std::format("int main() {{ return {}; }}\n", v));
            return p.mcpp({"run"});
        };
        for (int want : {0, 1, 3, 42, 124}) {
            const auto r = status_of(want);
            expect(fatal(e2e::exited(r, want))) << "main returned " << want << ", mcpp run exited " << r.status;
        }
        auto r = status_of(127);
        expect(fatal(e2e::exited(r, 127))) << "main returned 127, mcpp run exited " << r.status;
        expect(fatal(!s::has_line_nocase(s::text(r), "^error:"))) << "a program exiting 127 must not produce an mcpp error line: " << s::text(r);
        p.write("src/main.cpp", "int main() { return 0; }\n");
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build for the runner cases: " << s::text(r);
        const auto host = host_triple(p);
        s::write_executable(p.at("not-a-program"), "this is not an executable format\n");
        const auto base = p.read("mcpp.toml");
        p.write("mcpp.toml", base + std::format("\n[target.{}]\nrunner = [\"{}\"]\n", host, p.at("not-a-program").string()));
        r = p.mcpp({"run"});
        expect(fatal(e2e::exited(r, 126))) << "unexecutable runner: want 126: " << s::text(r);
        expect(fatal(s::has_line_nocase(s::text(r), "^error:"))) << "a refused spawn must say why: " << s::text(r);
        p.write("mcpp.toml", base + std::format("\n[target.{}]\nrunner = [\"{}\"]\n", host, p.at("no-such-runner-at-all").string()));
        r = p.mcpp({"run"});
        expect(fatal(r.status == 2 || r.status == 127)) << "missing runner: exit " << r.status << ", want 2 (pre-flight) or 127 (spawn): " << s::text(r);
    };

    "862 run q writes exactly the programs stdout"_test[tag{"requires:unix-shell"}] = [] {
        // `run -q` writes exactly the program's standard output and standard error, on the fast
        // path too; without -q, standard output is still the program's alone, and the status
        // lines on standard error end with `Running`, a blank line, and the program's own.
        s::project p({{"mcpp.toml", "[package]\nname    = \"p\"\nversion = \"0.1.0\"\n\n[targets.p]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                      {"src/main.cpp", R"(#include <cstdio>
int main() {
    std::fputs("OUT one\n", stdout);
    std::fputs("ERR one\n", stderr);
    std::fputs("OUT two\n", stdout);
    std::fputs("ERR two\n", stderr);
    std::fputs("OUT no-line-end", stdout);
    return 0;
}
)"}});
        const auto a = p.mcpp({"run", "-q"});
        expect(fatal(e2e::succeeded(a))) << "A: mcpp run -q failed: " << a.err;
        const auto bin = bin_of(p.at("target"), "p");
        expect(fatal(bin.has_value())) << "A: the program was not built";
        const auto direct = s::run(*bin);
        expect(fatal(a.out == direct.out)) << "A: standard output is not exactly the program's: " << a.out;
        expect(fatal(a.err == direct.err)) << "A: standard error is not exactly the program's: " << a.err;
        const auto b = p.mcpp({"run", "-q"});
        expect(fatal(e2e::succeeded(b))) << "B: the second mcpp run -q failed: " << b.err;
        expect(fatal(b.out == direct.out)) << "B: standard output is not exactly the program's: " << b.out;
        expect(fatal(b.err == direct.err)) << "B: standard error is not exactly the program's: " << b.err;
        const auto c = p.mcpp({"run"});
        expect(fatal(e2e::succeeded(c))) << "C: mcpp run failed: " << c.err;
        expect(fatal(c.out == direct.out)) << "C: standard output carries more than the program's: " << c.out;
        const auto at = c.err.find("Running `");
        expect(fatal(at != std::string::npos)) << "C: no Running line on standard error: " << c.err;
        const auto eol = c.err.find('\n', at);
        expect(fatal(eol != std::string::npos && c.err.substr(eol + 1).starts_with("\n"))) << "C: the line after Running is not blank: " << c.err;
        expect(fatal(c.err.ends_with(direct.err))) << "C: the program's own standard error does not end the stream: " << c.err;
        expect(fatal(a.out.starts_with("OUT one"))) << "D: standard output does not begin with the program's first byte";
        expect(fatal(a.out.ends_with("OUT no-line-end"))) << "D: standard output does not end with the program's last byte";
    };

    "863 a run whose build failed exits 101"_test[tag{"requires:unix-shell"}] = [] {
        // `run` exits with the program's status, 126 for a program that cannot be executed, and
        // 101 when its build fails (on the fast path, in the main source, in a first run, for an
        // unreadable manifest), which `build` (1) and `test` (1) do not adopt.
        s::project p({{"mcpp.toml", "[package]\nname    = \"p\"\nversion = \"0.1.0\"\n\n[targets.p]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                      {"src/main.cpp", "#include <cstdlib>\n#include \"impl.h\"\nint main(int argc, char** argv) { return status_of(argc, argv); }\n"},
                      {"tests/failing.cpp", "int main() { return 1; }\n"}});
        const auto good = "inline int status_of(int argc, char** argv) { return argc > 1 ? std::atoi(argv[1]) : 0; }\n";
        const auto bad = "inline int status_of(int, char**) { return this_name_is_not_declared; }\n";
        const auto main_good = p.read("src/main.cpp");
        p.write("src/impl.h", good);
        auto run_q = [&](std::vector<std::string> extra = {}) {
            std::vector<std::string> args{"run", "-q"};
            args.insert(args.end(), extra.begin(), extra.end());
            return p.mcpp(args);
        };
        auto r = run_q();
        expect(fatal(e2e::exited(r, 0))) << "C: a program that returns 0 exited " << r.status << ": " << r.err;
        r = run_q({"--", "1"});
        expect(fatal(e2e::exited(r, 1))) << "C: a program that returns 1 exited " << r.status << ": " << r.err;
        r = run_q({"--", "3"});
        expect(fatal(e2e::exited(r, 3))) << "C: a program that returns 3 exited " << r.status << ": " << r.err;
        const auto bin = bin_of(p.at("target"), "p");
        expect(fatal(bin.has_value())) << "C: the program was not built";
        const auto perms = fs::status(*bin).permissions();
        fs::permissions(*bin, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::remove);
        r = run_q();
        fs::permissions(*bin, perms);
        expect(fatal(e2e::exited(r, 126))) << "D: a program that cannot be executed exited " << r.status << ", not 126: " << r.err;
        expect(fatal(!r.err.empty())) << "D: a refused start wrote no reason to standard error";
        p.write("src/impl.h", bad);
        r = run_q();
        expect(fatal(e2e::exited(r, 101))) << "A: a compile error after a build (the fast path) exited " << r.status << ", not 101: " << r.err;
        expect(fatal(e2e::contains(r.err, "this_name_is_not_declared"))) << "A: the diagnostic is not shown: " << r.err;
        p.write("src/impl.h", good);
        r = run_q({"--", "1"});
        expect(fatal(e2e::exited(r, 1))) << "A: after the fix, a program that returns 1 exited " << r.status << ", not 1: " << r.err;
        p.write("src/main.cpp", "int main() { return this_name_is_not_declared; }\n");
        r = run_q();
        expect(fatal(e2e::exited(r, 101))) << "A: a compile error in the main source exited " << r.status << ", not 101: " << r.err;
        fs::remove_all(p.at("target"));
        r = run_q();
        expect(fatal(e2e::exited(r, 101))) << "A: a compile error in a first run exited " << r.status << ", not 101: " << r.err;
        r = p.mcpp({"build", "-q"});
        expect(fatal(e2e::exited(r, 1))) << "E: mcpp build with a compile error exited " << r.status << ", not 1: " << r.err;
        p.write("src/main.cpp", main_good);
        r = p.mcpp({"test", "-q"});
        expect(fatal(e2e::exited(r, 1))) << "E: mcpp test with a failing test exited " << r.status << ", not 1: " << r.err;
        const auto manifest = p.read("mcpp.toml");
        p.write("mcpp.toml", "[package\nname = \"p\"\n");
        r = run_q();
        expect(fatal(e2e::exited(r, 101))) << "B: an unreadable manifest exited " << r.status << ", not 101: " << r.err;
        expect(fatal(!r.err.empty())) << "B: an unreadable manifest wrote no reason to standard error";
        r = p.mcpp({"build", "-q"});
        expect(fatal(r.status != 101)) << "B: mcpp build adopted the status of mcpp run: " << r.err;
        expect(fatal(failed(r))) << "B: mcpp build accepted an unreadable manifest";
        p.write("mcpp.toml", manifest);
    };
    "883 run hands the terminal to the program"_test[tag{"requires:python3"}][tag{"requires:unix-shell"}] = [] {
        // The program reads the terminal through the fast path and the prepared path; ^C
        // reaches its handler and its status comes back; piped input reaches it; a death by a
        // signal comes back as the same status; the program runs as the run's own process, so a
        // terminated run leaves nothing behind.
        const auto home = s::case_home(s::home::fresh);
        s::project(home, {{"config.toml", std::format("[xlings]\nhome = \"{}\"\n", (s::shared_home() / "registry").generic_string())}});
        const s::options o{.where = s::home::fresh};
        s::project app(s::case_dir() / "app", {{"mcpp.toml", "[package]\nname = \"t883\"\nversion = \"0.1.0\"\n"},
                                               {"src/main.cpp", R"(#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

volatile std::sig_atomic_t interrupted = 0;

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "echo";
    if (mode == "echo") {
        std::string line;
        while (std::cout << "> " << std::flush, std::getline(std::cin, line)) {
            if (line == "quit") break;
            std::cout << "read: " << line << std::endl;
        }
        return 0;
    }
    if (mode == "interrupt") {
        std::signal(SIGINT, [](int) { interrupted = 1; });
        std::puts("ready");
        std::fflush(stdout);
        while (!interrupted) ::usleep(20000);
        std::puts("handled");
        std::fflush(stdout);
        return 3;
    }
    if (mode == "abort") std::abort();
    if (mode == "forever") {
        std::printf("pid %d\n", static_cast<int>(::getpid()));
        std::fflush(stdout);
        for (;;) ::pause();
    }
    return 2;
}
)"}});
        auto r = app.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "the fixture did not build: " << s::text(r);
        // The script's drive.py (pty.fork) is a process on a pseudo-terminal of its own session.
        auto on_terminal = [&](std::vector<std::string> args) {
            args.insert(args.begin(), s::mcpp_path());
            return s::process::start({.argv = args, .cwd = app.dir(), .env = {{"MCPP_HOME", home.string()}},
                                      .in = s::stream::terminal, .out = s::stream::terminal, .err = s::stream::terminal, .session = true});
        };
        constexpr auto deadline = std::chrono::seconds{120};
        for (const auto& [label, args] : std::vector<std::pair<std::string, std::vector<std::string>>>{
                 {"the fast path", {"run"}}, {"the prepared path", {"run", "-q", "--release"}}}) {
            auto p = on_terminal(args);
            expect(fatal(p.wait_for("> ", deadline))) << label << ": no prompt: " << s::strip_escapes(p.terminal());
            p.send("hello\r");
            expect(fatal(p.wait_for("read: hello", deadline))) << "the program did not read the terminal through " << label << ": " << s::strip_escapes(p.terminal());
            p.send("quit\r");
            const auto status = p.wait(deadline);
            expect(fatal(status == 0)) << "the program did not read the terminal through " << label << " (status " << status << "): " << s::strip_escapes(p.terminal());
        }
        {
            auto p = on_terminal({"run", "-q", "--", "interrupt"});
            expect(fatal(p.wait_for("ready", deadline))) << "the program did not start: " << s::strip_escapes(p.terminal());
            p.send("\x03");
            const auto status = p.wait(deadline);
            expect(fatal(e2e::contains(p.terminal(), "handled") && status == 3)) << "Ctrl-C did not reach the program's handler (status " << status << "): " << s::strip_escapes(p.terminal());
        }
        r = app.mcpp({"run", "-q"}, {.where = s::home::fresh, .input = "piped\nquit\n"});
        expect(fatal(e2e::succeeded(r))) << "the piped run failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "read: piped"))) << "the program did not read piped input: " << s::text(r);

        // Python's returncode of a signal death is the negative signal: the status and the signal.
        const auto bin = bin_of(app.at("target"), "t883");
        expect(fatal(bin.has_value())) << "no t883 program";
        auto direct = s::process::start({.argv = {bin->string(), "abort"}, .cwd = app.dir(), .out = s::stream::null, .err = s::stream::null});
        const auto direct_status = direct.wait();
        auto via = s::process::start({.argv = {s::mcpp_path(), "run", "-q", "--", "abort"}, .cwd = app.dir(), .env = {{"MCPP_HOME", home.string()}},
                                      .out = s::stream::null, .err = s::stream::null});
        const auto via_status = via.wait();
        expect(fatal(direct_status == via_status && direct.ended_by() == via.ended_by()))
            << "status of a signal death: direct " << direct_status << " (signal " << direct.ended_by() << "), through mcpp run " << via_status << " (signal " << via.ended_by() << ")";

        auto forever = s::process::start({.argv = {s::mcpp_path(), "run", "-q", "--", "forever"}, .cwd = app.dir(), .env = {{"MCPP_HOME", home.string()}},
                                          .out = s::stream::pipe, .err = s::stream::null});
        expect(fatal(forever.wait_for("pid [0-9]+\n", deadline))) << "the program did not print its pid: " << forever.piped();
        std::smatch m;
        const auto piped = forever.piped();
        std::regex_search(piped, m, std::regex{"pid ([0-9]+)"});
        const int child = std::stoi(m[1]);
        expect(fatal(child == forever.pid())) << "the program runs as pid " << child << ", not as the run's own pid " << forever.pid();
        forever.signal(s::sig::terminate);
        forever.wait(std::chrono::seconds{30});
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        expect(fatal(!fs::exists(fs::path("/proc") / std::to_string(child)))) << "a terminated run left a process behind: " << child;
    };
}};


}  // namespace
