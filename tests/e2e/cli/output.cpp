// What mcpp writes: a failed build's diagnostic once and without noise, each step of a build
// reported once, status on standard error and results on standard output, both streams on one
// pipe in order, and the control contract a program reads (formerly tests/e2e/48, 842, 864, 866,
// 936).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

// The lines of `text` (`while read line`), without a carriage return.
std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> out;
    std::istringstream in{std::string(text)};
    for (std::string l; std::getline(in, l);) {
        if (l.ends_with('\r')) l.pop_back();
        out.push_back(l);
    }
    return out;
}

// The index of the first line matching `pattern` (`grep -n ... | head -1`), or nothing.
std::optional<std::size_t> first_line(const std::vector<std::string>& lines, std::string_view pattern) {
    const std::regex re{std::string(pattern)};
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (std::regex_search(lines[i], re)) return i;
    return std::nullopt;
}

// The number of lines equal to `line` (`grep -cxF`).
std::size_t count_exact(std::string_view text, std::string_view line) {
    return static_cast<std::size_t>(std::ranges::count(lines_of(text), std::string(line)));
}

// Both standard streams of mcpp on one pipe, in the order written (`2>&1 | cat`).
std::string one_pipe(std::vector<std::string> args, const fs::path& cwd, s::spawn extra = {}) {
    auto c = s::mcpp_command(std::move(args), cwd);
    extra.argv = c.argv;
    extra.cwd = cwd;
    extra.env.insert(extra.env.begin(), c.env.begin(), c.env.end());
    auto p = s::process::start(std::move(extra));
    (void)p.wait();
    return p.piped();
}

// The lines of `text` that hold more than blanks.
std::size_t nonempty_lines(std::string_view text) {
    return static_cast<std::size_t>(std::ranges::count_if(lines_of(text), [](const std::string& l) {
        return l.find_first_not_of(" \t") != std::string::npos;
    }));
}

// Two paths name the same file (`os.path.normcase(os.path.normpath(...))`).
bool same(const std::string& a, const std::string& b) {
    return fs::path(a).lexically_normal() == fs::path(b).lexically_normal();
}

constexpr std::string_view slow_prepare = R"cpp(import mcpp;
#include <string>
int main() {
    const std::string prefix = std::string(mcpp::out_dir()) + "/{prefix}";
    mcpp::action a;
    a.id   = "{id}";
    a.role = mcpp::roles::prepare;
    a.arg("python3").arg("-c")
     .arg("import os, sys, time; time.sleep({seconds}); os.makedirs(sys.argv[1], exist_ok=True); open(os.path.join(sys.argv[1], \"ready\"), \"w\").close()")
     .arg(prefix.c_str())
     .output((prefix + ".stamp").c_str())
     .output_dir(prefix.c_str())
     .submit();
    return 0;
}
)cpp";

// A build program whose prepare action `id` sleeps `seconds`.
std::string slow_program(std::string_view prefix, std::string_view id, int seconds) {
    std::string out(slow_prepare);
    const auto put = [&](std::string_view key, std::string_view value) {
        out.replace(out.find(key), key.size(), value);
    };
    put("{prefix}", prefix);
    put("{id}", id);
    put("{seconds}", std::to_string(seconds));
    return out;
}

E2E output{"output", [] {
    "48 build error output"_test = [] {
        // A failed build shows the compiler's diagnostic once, after the package's status line,
        // without ninja's progress or a private environment prefix; the graph holds no such
        // prefix and no link-only flag in its compile flags; --verbose keeps ninja's details.
        s::project p({{"mcpp.toml", "[package]\nname    = \"bad_error_output\"\nversion = \"0.1.0\"\n\n[language]\nimport_std = false\n"},
                      {"src/main.cpp", "#error MCPPE2E_BUILD_ERROR_MARKER\nint main() {\n    return 0;\n}\n"}});
        const s::options home{.where = s::home::inherited};
        auto r = p.mcpp({"build"}, home);
        const auto out = s::text(r);
        expect(fatal(failed(r))) << "expected build to fail\n" << out;
        expect(fatal(e2e::contains(out, "MCPPE2E_BUILD_ERROR_MARKER"))) << "missing compiler diagnostic marker\n" << out;
        const auto lines = lines_of(out);
        const auto compiling = first_line(lines, "Compiling bad_error_output");
        const auto error = first_line(lines, "^error: build failed");
        expect(fatal(compiling && error && *compiling < *error)) << "expected build status before diagnostics\n" << out;
        expect(fatal(s::count_matching(out, "error:.*MCPPE2E_BUILD_ERROR_MARKER") == 1u)) << "expected compiler diagnostic marker once\n" << out;
        std::map<std::string, int> seen;
        std::vector<std::string> duplicates;
        for (const auto& l : lines) {
            if (l.find_first_not_of(" \t") == std::string::npos) continue;
            if (++seen[l] == 2) duplicates.push_back(l);
        }
        expect(fatal(duplicates.empty())) << "unexpected duplicate diagnostic lines" << duplicates << "\n" << out;
        for (std::string_view noise : {"LD_LIBRARY_PATH=", "FAILED:", "ninja: Entering directory", "ninja: build stopped",
                                       "argument unused during compilation", R"(^\[[0-9][0-9]*/[0-9][0-9]*\] )"})
            expect(fatal(s::no_line(out, noise))) << "unexpected build-output noise matched: " << noise;
        std::optional<fs::path> graph;
        auto all = s::find_files(p.at("target"), R"(build\.ninja)");
        std::ranges::sort(all);
        for (const auto& f : all)
            if (!graph && !f.generic_string().contains("/.build-mcpp/")) graph = f;
        expect(fatal(graph.has_value())) << "missing generated build.ninja\n" << out;
        const auto g = s::read_file(*graph);
        expect(fatal(s::lacks(g, "LD_LIBRARY_PATH") && s::lacks(g, "toolenv"))) << "build.ninja should not contain private runtime env prefixes";
        for (const auto& l : lines_of(g))
            if (l.starts_with("cxxflags") || l.starts_with("cflags"))
                expect(fatal(!e2e::matches(l, R"(-stdlib=libc\+\+|-fuse-ld=lld|--unwindlib=libunwind)"))) << "compile flags should not contain clang link-only flags: " << l;
        r = p.mcpp({"build", "--verbose"}, home);
        expect(fatal(failed(r))) << "expected verbose build to fail\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "FAILED:"))) << "verbose output should retain ninja failure details";
        expect(fatal(e2e::contains(s::text(r), "MCPPE2E_BUILD_ERROR_MARKER"))) << "verbose output missing compiler diagnostic marker";
    };

    "842 the build reports each step once"_test[tag{"requires:python3"}] = [] {
        // R1 each package once, in the order its work finished, a blank line before Finished,
        // nothing redrawn; R2 a build program ran, then cached (--verbose); R3 an edit names its
        // package alone; R4 an edited dependency, then its dependant, --verbose's steps; R5 a
        // failed step is reported while another package's prepare action still runs.
        s::needs({"python3"});
        s::project root({{"ws/mcpp.toml", "[workspace]\nmembers = [\"core\", \"cli\"]\n"},
                         {"ws/core/mcpp.toml", "[package]\nname = \"core\"\nversion = \"0.1.0\"\n\n[targets.core]\nkind = \"lib\"\n"},
                         {"ws/core/src/core.cppm", "export module rp_core;\nexport int core_v() { return 1; }\n"},
                         {"util/mcpp.toml", "[package]\nname = \"util\"\nversion = \"0.2.0\"\n\n[targets.util]\nkind = \"lib\"\n"},
                         {"util/src/util.cppm", "export module rp_util;\nexport int util_v() { return 2; }\n"},
                         {"ws/cli/mcpp.toml", R"([package]
name = "cli"
version = "0.1.0"

[dependencies]
core = { path = "../core" }
util = { path = "../../util" }

[targets.cli]
kind = "bin"
main = "src/main.cpp"
)"},
                         {"ws/cli/src/main.cpp", "import rp_core;\nimport rp_util;\nint main() { return core_v() + util_v() == 3 ? 0 : 1; }\n"},
                         {"ws/cli/build.mcpp", "import mcpp;\nint main() { mcpp::define(\"RP_CLI=1\"); return 0; }\n"}});
        auto r = root.mcpp_in("ws", {"build", "--workspace"});
        auto b1 = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "the first build failed\n" << b1;
        for (std::string_view line : {"core v0.1.0 (core)", "cli v0.1.0 (cli)", "util v0.2.0 (../util)"})
            expect(fatal(count_exact(b1, std::format("   Compiling {}", line)) == 1u)) << "R1: '" << line << "' is not named exactly once\n" << b1;
        const auto l1 = lines_of(b1);
        const auto core_at = first_line(l1, "Compiling core v0\\.1\\.0");
        const auto cli_at = first_line(l1, "Compiling cli v0\\.1\\.0");
        expect(fatal(core_at && cli_at && *core_at < *cli_at)) << "R1: cli is named before core, which it imports\n" << b1;
        expect(fatal(s::no_line(b1, R"(Compiling [0-9]+ dependenc|done [0-9]|mcpplibs\.util)"))) << "R1: a fold, an outcome or a namespace nobody wrote";
        const auto fin = first_line(l1, "^ +Finished ");
        expect(fatal(fin && *fin > 0 && l1[*fin - 1].empty())) << "R1: no blank line precedes Finished\n" << b1;
        expect(fatal(s::lacks(b1, "\r"))) << "R1: the log holds a carriage return";
        expect(fatal(s::lacks(b1, "\x1b"))) << "R1: the log holds an escape sequence";

        expect(fatal(s::has_line(b1, R"(^ *build\.mcpp cli +ran [0-9])"))) << "R2: the program's line does not state that it ran\n" << b1;
        r = root.mcpp_in("ws", {"build", "--workspace", "--profile", "dev"});
        expect(fatal(e2e::succeeded(r))) << "the planned rebuild failed\n" << s::text(r);
        expect(fatal(s::lacks(s::text(r), "build.mcpp"))) << "R2: a reused program has a line without --verbose\n" << s::text(r);
        r = root.mcpp_in("ws", {"build", "--workspace", "--profile", "dev", "-v"});
        expect(fatal(e2e::succeeded(r))) << "the verbose rebuild failed\n" << s::text(r);
        expect(fatal(s::has_line(s::text(r), R"(^ *build\.mcpp cli +cached)"))) << "R2: --verbose does not state the reused program\n" << s::text(r);

        root.write("ws/cli/src/main.cpp", "import rp_core;\nimport rp_util;\nint main() { return core_v() + util_v() == 3 ? 0 : 2; }\n");
        r = root.mcpp_in("ws", {"build", "--workspace"});
        expect(fatal(e2e::succeeded(r))) << "the incremental build failed\n" << s::text(r);
        expect(fatal(count_exact(s::text(r), "   Compiling cli v0.1.0 (cli)") >= 1u)) << "R3: the edited member has no line\n" << s::text(r);
        expect(fatal(s::no_line(s::text(r), "Compiling (core|util)"))) << "R3: a package with nothing to do has a line";

        root.write("util/src/util.cppm", "export module rp_util;\nexport int util_v() { return 2; }\nint unused = 0;\n");
        r = root.mcpp_in("ws", {"build", "--workspace", "-v"});
        const auto b4 = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "the verbose build failed\n" << b4;
        expect(fatal(count_exact(b4, "   Compiling util v0.2.0 (../util)") >= 1u)) << "R4: the edited dependency is not named\n" << b4;
        expect(fatal(s::has_line(b4, R"(^\[[0-9]+/[0-9]+\] )"))) << "R4: --verbose prints no step";
        expect(fatal(count_exact(b4, "       Fresh core v0.1.0 (core)") >= 1u)) << "R4: --verbose does not name core as fresh\n" << b4;
        expect(fatal(s::has_line(b4, R"(^ +Compiled util v0\.2\.0 \(\.\./util\) · [0-9]+ steps? · [0-9])"))) << "R4: --verbose does not state util's steps and span";

        s::project r5(mcpplibs::mtest::temp_dir() / "r5",
                      {{"slow/mcpp.toml", "[package]\nname = \"slow\"\nversion = \"0.1.0\"\n\n[targets.slow]\nkind = \"lib\"\n"},
                       {"slow/src/slow.cppm", "export module rp_slow;\nexport int slow_v() { return 3; }\n"},
                       {"app/mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nslow = { path = \"../slow\" }\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                       {"app/src/main.cpp", "int main() { return R5_BROKEN; }\n"}});
        r5.write("slow/build.mcpp", slow_program("slow-install", "slow:install", 6));
        // The script's Python reader: the time of the first `error: build failed` line and of the end.
        auto c = s::mcpp_command({"build"}, r5.at("app"));
        auto p = s::process::start({.argv = c.argv, .cwd = r5.at("app"), .env = c.env});
        const auto start = std::chrono::steady_clock::now();
        std::optional<double> first_error;
        while (!p.poll()) {
            p.pump(std::chrono::milliseconds{50});
            if (!first_error && s::has_line(p.piped(), R"(^error: build failed in app v0\.1\.0 \(\.\))"))
                first_error = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        }
        const double end = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const int code = p.wait();
        expect(fatal(code != 0)) << "R5: the broken build succeeded\n" << p.piped();
        expect(fatal(first_error.has_value())) << "R5: no `error: build failed` line\n" << p.piped();
        expect(end - *first_error >= 3.0) << std::format("R5: the failure was reported {:.1f}s before mcpp exited; it waited for ninja\n", end - *first_error) << p.piped();
    };

    "864 a builds status is on standard error"_test = [] {
        // A: the status of a build is on standard error; B: standard output stays empty for a
        // build that compiles, does nothing or is verbose; C: -q writes nothing; D: 2>&1 records
        // the steps; E: new and clean say what they did on standard error.
        s::project p({{"mcpp.toml", "[package]\nname    = \"p\"\nversion = \"0.1.0\"\n\n[targets.p]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                      {"src/main.cpp", "int main() { return 0; }\n"}});
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "A: the build failed\n" << r.err;
        expect(fatal(e2e::contains(r.err, "Compiling p v0.1.0"))) << "A: no Compiling line on standard error\n" << r.err;
        expect(fatal(e2e::contains(r.err, "Finished "))) << "A: no Finished line on standard error\n" << r.err;
        expect(fatal(s::no_line(r.err, "^warning"))) << "A: the project is not warning-free\n" << r.err;

        p.write("src/main.cpp", "int main() { return 1 - 1; }\n");
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "B: the build that compiles failed\n" << r.err;
        expect(fatal(r.out.empty())) << "B: the build that compiles wrote to standard output\n" << r.out;
        expect(fatal(e2e::contains(r.err, "Compiling p v0.1.0"))) << "B: the build that compiles names nothing on standard error";
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "B: the build with nothing to do failed\n" << r.err;
        expect(fatal(r.out.empty())) << "B: the build with nothing to do wrote to standard output\n" << r.out;
        expect(fatal(e2e::contains(r.err, "Finished "))) << "B: the build with nothing to do has no Finished line on standard error";
        p.write("src/main.cpp", "int main() { return 2 - 2; }\n");
        r = p.mcpp({"build", "--verbose"});
        expect(fatal(e2e::succeeded(r))) << "B: the verbose build failed\n" << r.err;
        expect(fatal(r.out.empty())) << "B: --verbose wrote to standard output\n" << r.out;
        expect(fatal(!r.err.empty())) << "B: --verbose wrote nothing to standard error";
        p.write("src/main.cpp", "int main() { return 3 - 3; }\n");
        r = p.mcpp({"build"});
        expect(fatal(r.out.empty())) << "B: a pipe received: " << r.out;

        p.write("src/main.cpp", "int main() { return 4 - 4; }\n");
        r = p.mcpp({"build", "-q"});
        expect(fatal(e2e::succeeded(r))) << "C: the quiet build failed\n" << r.err;
        expect(fatal(r.out.empty())) << "C: -q wrote to standard output\n" << r.out;
        expect(fatal(r.err.empty())) << "C: -q wrote to standard error\n" << r.err;

        p.write("src/main.cpp", "int main() { return 5 - 5; }\n");
        const auto d = one_pipe({"build"}, p.dir());
        expect(fatal(e2e::contains(d, "Compiling p v0.1.0"))) << "D: 2>&1 | tee did not record the steps\n" << d;
        expect(fatal(e2e::contains(d, "Finished "))) << "D: 2>&1 | tee did not record Finished\n" << d;

        r = s::mcpp({"new", "q"}, mcpplibs::mtest::temp_dir());
        expect(fatal(e2e::succeeded(r))) << "E: mcpp new failed\n" << r.err;
        expect(fatal(r.out.empty())) << "E: mcpp new wrote to standard output\n" << r.out;
        expect(fatal(e2e::contains(r.err, "Created"))) << "E: mcpp new does not say on standard error what it created";
        r = p.mcpp({"clean"});
        expect(fatal(e2e::succeeded(r))) << "E: mcpp clean failed\n" << r.err;
        expect(fatal(r.out.empty())) << "E: mcpp clean wrote to standard output\n" << r.out;
        expect(fatal(e2e::contains(r.err, "Cleaned"))) << "E: mcpp clean does not say on standard error what it cleaned";
    };

    "866 both streams on one pipe keep their order and json stays clean"_test[tag{"requires:unix-shell"}][tag{"requires:python3"}] = [] {
        // C: a build's steps on one pipe, a blank line before Finished; A: run's line, a blank
        // line, then the program's lines in the order written; B: --message-format json keeps
        // standard output JSON and standard error free of status; D: the status row follows
        // standard error onto a terminal and stays off a terminal on standard output.
        s::needs({"unix-shell", "python3"});
        s::project root({{"p/mcpp.toml", "[package]\nname    = \"p\"\nversion = \"0.1.0\"\n\n[targets.p]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                         {"p/src/main.cpp", R"(#include <cstdio>
int main() {
    std::fputs("first, to standard output\n", stdout);
    std::fflush(stdout);
    std::fputs("second, to standard error\n", stderr);
    std::fputs("third, to standard output\n", stdout);
    std::fflush(stdout);
    return 0;
}
)"},
                         {"p/tests/t_ok.cpp", "int main() { return 0; }\n"},
                         {"q/mcpp.toml", "[package]\nname    = \"q\"\nversion = \"0.1.0\"\n\n[targets.q]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                         {"q/src/main.cpp", "int main() { return 0; }\n"}});
        root.write("q/build.mcpp", slow_program("tool", "q:wait", 3));
        const auto p = root.at("p");

        const auto c = lines_of(one_pipe({"build"}, p));
        const auto compiling = first_line(c, "Compiling p v0\\.1\\.0");
        const auto finished = first_line(c, "Finished ");
        expect(fatal(compiling && finished)) << "C: the steps are not on the pipe" << c;
        expect(fatal(*compiling < *finished)) << "C: Finished precedes Compiling" << c;
        expect(fatal(*finished > 0 && c[*finished - 1].empty())) << "C: no blank line precedes Finished" << c;

        const auto a = lines_of(one_pipe({"run"}, p));
        const auto running = first_line(a, "Running `");
        const auto first = first_line(a, "^first, to standard output");
        const auto second = first_line(a, "^second, to standard error");
        const auto third = first_line(a, "^third, to standard output");
        expect(fatal(running && first && second && third)) << "A: a line is missing from the pipe" << a;
        expect(fatal(*running < *first)) << "A: the Running line does not precede the program's output" << a;
        expect(fatal(*running + 1 < a.size() && a[*running + 1].empty())) << "A: no blank line follows Running" << a;
        expect(fatal(*running + 2 == *first)) << "A: something lies between the blank line and the program's output" << a;
        expect(fatal(*first < *second && *second < *third)) << "A: the program's lines are out of order" << a;

        auto r = s::mcpp({"test", "--message-format", "json"}, p);
        expect(fatal(e2e::succeeded(r))) << "B: mcpp test --message-format json failed\n" << s::text(r);
        expect(fatal(!r.out.empty())) << "B: no record on standard output";
        for (const auto& l : lines_of(r.out)) {
            if (l.find_first_not_of(" \t") == std::string::npos) continue;
            const auto j = nlohmann::json::parse(l, nullptr, false);
            expect(fatal(!j.is_discarded() && j.is_object())) << "B: a line of standard output is not a JSON object: " << l;
        }
        expect(fatal(s::no_line(r.err, "Compiling|Resolving|Resolved|Finished"))) << "B: status on standard error under --message-format json\n" << r.err;

        const auto q = root.at("q");
        const auto terminal_build = [&](bool stdout_is_terminal) {
            auto cmd = s::mcpp_command({"build"}, q);
            s::spawn sp{.argv = cmd.argv, .cwd = q, .env = cmd.env};
            sp.env.insert(sp.env.end(), {{"TERM", "xterm-256color"}, {"LANG", "C.UTF-8"}, {"MCPP_PROGRESS", "plain"}, {"NO_COLOR", std::nullopt}});
            if (stdout_is_terminal) {
                sp.out = s::stream::terminal;
                sp.err = s::stream::file;
                sp.err_file = q / "err.txt";
            } else {
                sp.out = s::stream::pipe;
                sp.err = s::stream::terminal;
            }
            auto proc = s::process::start(std::move(sp));
            const int status = proc.wait();
            expect(fatal(status == 0)) << "the build failed\n" << proc.terminal() << proc.piped();
            return std::pair{proc.terminal(), proc.piped()};
        };
        auto [term1, piped1] = terminal_build(false);
        expect(fatal(e2e::contains(term1, "\x1b[?7l"))) << "D1: the status row is not drawn on standard error";
        expect(fatal(e2e::contains(term1, "Finished"))) << "D1: Finished is not on standard error";
        expect(fatal(piped1.empty())) << "D1: the pipe received " << piped1;
        auto [term2, piped2] = terminal_build(true);
        const auto err = s::read_file(q / "err.txt");
        expect(fatal(term2.empty())) << "D2: the terminal on standard output received " << term2;
        expect(fatal(s::lacks(err, "\x1b"))) << "D2: the file on standard error holds an escape sequence";
        expect(fatal(e2e::contains(err, "Finished"))) << "D2: Finished is not in the file on standard error";
    };

    "936 the control contract"_test[tag{"requires:python3"}][resource{"machine"}] = [] {
        // A the JSON event stream and the build envelope, a failure's located diagnostic; B
        // --print-launch; C --deny; D CLICOLOR_FORCE and NO_COLOR; E --home; F the envelopes of
        // search, index list and pack --list-formats; G --protocol-version; H a build ended by
        // SIGTERM is recorded cancelled and exits 143.
        s::needs({"python3"});
        auto p = s::project::scaffold("app");
        auto r = p.mcpp({"build", "--message-format", "json"});
        expect(fatal(e2e::succeeded(r))) << "A: the build failed\n" << r.err;
        {
            const auto lines = s::json_lines(r.out);
            expect(fatal(!lines.empty() && lines.size() == nonempty_lines(r.out))) << "A: the stream is not as specified\n" << r.out;
            bool plan = false, artifact = false;
            for (const auto& e : lines) {
                if (e.value("kind", "") != "mcpp.event") continue;
                plan = plan || e.value("event", "") == "plan";
                artifact = artifact || (e.value("event", "") == "artifact" && e["artifact"]["kind"] == "bin");
            }
            expect(fatal(plan)) << "A: no plan event";
            expect(fatal(artifact)) << "A: no artifact event";
            const auto& env = lines.back();
            expect(fatal(env["kind"] == "mcpp.build" && env["schemaVersion"] == 1)) << "A: " << env.dump();
            const auto& data = env["data"];
            expect(fatal(data["status"] == "ok")) << "A: " << data["status"].dump();
            nlohmann::json art;
            for (const auto& a : data["artifacts"])
                if (a["kind"] == "bin") { art = a; break; }
            expect(fatal(!art.is_null() && same(art["launch"]["argv"][0].get<std::string>(), art["path"].get<std::string>()))) << "A: " << art.dump();
        }
        p.write("src/main.cpp", "int main() { return undeclared_name; }\n");
        r = p.mcpp({"build", "--message-format", "json"});
        expect(fatal(failed(r))) << "A: a failing build exited 0";
        {
            const auto lines = s::json_lines(r.out);
            expect(fatal(!lines.empty() && lines.size() == nonempty_lines(r.out))) << "A: a line of the stream is not JSON\n" << r.out;
            std::vector<nlohmann::json> diags;
            for (const auto& l : lines)
                if (l.value("event", "") == "diagnostic") diags.push_back(l["diagnostic"]);
            expect(fatal(!diags.empty())) << "A: no diagnostic event\n" << r.out;
            expect(fatal(std::ranges::any_of(diags, [](const nlohmann::json& d) {
                return d["path"].get<std::string>().ends_with("main.cpp") && d["range"]["start"]["line"] == 1;
            }))) << "A: the failure carries no located diagnostic";
            expect(fatal(lines.back()["data"]["status"] == "failed")) << "A: " << lines.back()["data"]["status"].dump();
            expect(fatal(!lines.back()["data"]["diagnostics"].empty())) << "A: the record has no diagnostics";
        }
        p.write("src/main.cpp", "import std;\nint main() { std::println(\"started\"); }\n");

        r = p.mcpp({"run", "--print-launch"});
        expect(fatal(e2e::succeeded(r))) << "B: --print-launch failed\n" << r.err;
        expect(fatal(s::lacks(r.out, "started"))) << "B: the program was started";
        {
            const auto d = nlohmann::json::parse(r.out);
            expect(fatal(d["kind"] == "mcpp.launch")) << "B: " << d["kind"].dump();
            const auto& data = d["data"];
            expect(fatal(!data["argv"].empty() && same(data["argv"][0].get<std::string>(), data["program"].get<std::string>()))) << "B: " << data.dump();
            expect(fatal(data.contains("env") && data.contains("cwd"))) << "B: " << data.dump();
            const auto last = s::json_file(p.at("target/.last-result.json"))["data"];
            const auto record = s::json_file(last["result"].get<std::string>())["data"]["configuration"];
            const auto& configuration = data["configuration"];
            for (auto key : {"triple", "profile", "platform"})
                expect(fatal(configuration[key] == record[key])) << "B: " << configuration.dump();
            expect(fatal(configuration["runners"].is_array() &&
                         std::ranges::all_of(configuration["runners"], [](const nlohmann::json& n) { return n.is_string(); })))
                << "B: " << configuration.dump();
        }

        p.remove("target");
        r = p.mcpp({"--deny", "write-project", "build"});
        expect(fatal(e2e::exited(r, 5))) << "C: --deny write-project";
        expect(fatal(e2e::contains(r.err, "effect denied: write-project"))) << "C: the effect is not named\n" << r.err;
        expect(fatal(!p.exists("target") || s::find_files(p.at("target"), R"(build\.ninja)").empty())) << "C: the project was written";
        r = p.mcpp({"--deny", "no-such-effect", "build"});
        expect(fatal(e2e::exited(r, 2))) << "C: an unknown effect";

        r = p.mcpp({"build"}, {.env = {{"CLICOLOR_FORCE", "1"}}});
        expect(fatal(e2e::succeeded(r))) << "D: the build failed\n" << r.err;
        expect(fatal(e2e::contains(r.err, "\x1b["))) << "D: CLICOLOR_FORCE did not colour a pipe";
        r = p.mcpp({"build"}, {.env = {{"CLICOLOR_FORCE", "1"}, {"NO_COLOR", "1"}}});
        expect(fatal(e2e::succeeded(r))) << "D: the build failed\n" << r.err;
        expect(fatal(s::lacks(r.err, "\x1b["))) << "D: NO_COLOR did not win";

        const auto other = mcpplibs::mtest::temp_dir() / "other-home";
        fs::create_directories(other);
        r = p.mcpp({"--home", other.string(), "self", "env", "--format", "json"});
        {
            const auto d = s::json(r.out);
            auto home = d["data"]["mcppHome"].get<std::string>();
            std::ranges::replace(home, '\\', '/');
            expect(fatal(fs::path(home).lexically_normal().filename() == "other-home")) << "E: --home did not select the home for the run: " << home;
        }

        r = p.mcpp({"search", "--format", "json", "fmt"});
        {
            const auto d = s::json(r.out);
            expect(fatal(d["kind"] == "mcpp.search")) << "F: search --format json\n" << s::text(r);
            expect(fatal((d.contains("data") && d["data"].contains("packages")) || (d.contains("diagnostics") && !d["diagnostics"].empty())))
                << "F: search --format json\n" << s::text(r);
        }
        r = p.mcpp({"index", "list", "--format", "json"});
        expect(fatal(e2e::succeeded(r))) << "F: index list --format json failed\n" << r.err;
        {
            const auto d = s::json(r.out);
            expect(fatal(d["kind"] == "mcpp.index" && d["data"]["registries"].is_array())) << "F: index list --format json\n" << r.out;
        }
        r = p.mcpp({"pack", "--list-formats"});
        expect(fatal(e2e::succeeded(r))) << "F: pack --list-formats failed\n" << r.err;
        {
            const auto d = s::json(r.out);
            std::vector<std::string> names;
            for (const auto& f : d["data"]["formats"]) names.push_back(f["name"]);
            expect(fatal(d["kind"] == "mcpp.pack.formats" && std::ranges::contains(names, "tar") && std::ranges::contains(names, "dir")))
                << "F: pack --list-formats" << names;
        }
        r = p.mcpp({"--protocol-version"});
        for (std::string_view k : {"mcpp.build", "mcpp.launch", "mcpp.search", "mcpp.index", "mcpp.pack.formats"})
            expect(fatal(e2e::contains(r.out, std::format("\"{}\"", k)))) << "G: --protocol-version does not list " << k;

        if (s::on_windows()) return;  // H is POSIX
        s::project h(mcpplibs::mtest::temp_dir() / "h", {{"mcpp.toml", "[package]\nname = \"h\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n"}});
        std::string main_cpp = "import std;\n";
        for (int i = 1; i <= 40; ++i) {
            h.write(std::format("src/m{}.cppm", i), std::format("export module m{0};\nimport std;\nexport int f{0}(){{ std::vector<int> v({0}); return (int)v.size(); }}\n", i));
            main_cpp += std::format("import m{};\n", i);
        }
        h.write("src/main.cpp", main_cpp + "int main(){ std::println(\"{}\", f1()+f40()); }\n");
        r = h.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "H: the first build failed\n" << s::text(r);
        h.remove("target");
        auto c = s::mcpp_command({"build"}, h.dir());
        auto build = s::process::start({.argv = c.argv, .cwd = h.dir(), .env = c.env});
        const auto last_result = h.at("target/.last-result.json");
        for (int i = 0; i < 500; ++i) {
            std::error_code ec;
            if (fs::is_regular_file(last_result, ec) && s::read_file(last_result).contains("\"status\": \"running\"")) break;
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        build.signal(s::sig::terminate);
        const int rc = build.wait();
        {
            const auto last = s::json_file(last_result)["data"];
            expect(fatal(last["status"] == "cancelled")) << "H: the interrupted build left no cancelled record (rc=" << rc << ")\n" << last.dump() << "\n" << build.piped();
            const auto rec = s::json_file(last["result"].get<std::string>())["data"];
            expect(fatal(rec["status"] == "cancelled")) << "H: " << rec["status"].dump();
        }
        expect(fatal(rc == 143)) << "H: SIGTERM exited " << rc << ", expected 143\n" << build.piped();
        r = h.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "H: the build after the interrupted one failed\n" << s::text(r);
    };
}};

}  // namespace
