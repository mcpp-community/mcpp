// What `mcpp test` writes where and when: steps on standard error, results on standard output,
// streamed, timed and bounded (formerly tests/e2e/865 and 178).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;

namespace {

constexpr std::string_view hang_cpp =
    "#include <chrono>\n#include <thread>\nint main() { std::this_thread::sleep_for(std::chrono::seconds(30)); return 0; }\n";

E2E streams{"streams", [] {
    "865 mcpp test reports its steps on stderr and its results on stdout"_test[tag{"requires:unix-shell"}] = [] {
        s::needs({"unix-shell"});
        s::project p{{"mcpp.toml", R"([package]
name    = "p"
version = "0.1.0"

[targets.p]
kind = "bin"
main = "src/main.cpp"
)"},
                     {"src/main.cpp", "int main() { return 0; }\n"},
                     {"tests/t_ok.cpp", "#include <cstdio>\nint main() { std::puts(\"output of the passing test\"); return 0; }\n"}};
        // A: a run whose tests pass.
        auto a = p.mcpp({"test"});
        expect(fatal(e2e::succeeded(a)));
        expect(e2e::contains(a.out, "t_ok ... ok")) << "the verdict is not on standard output";
        expect(s::has_line(a.out, R"(test result ok\.)"));
        expect(s::no_line(a.out, "Compiling|Resolving|Resolved")) << "a step is on standard output";
        expect(e2e::contains(a.err, "Compiling")) << "the steps are not on standard error";
        expect(s::lacks(a.err, "test result")) << "the test result line is on standard error";
        expect(s::lacks(a.err, "... ok")) << "a verdict is on standard error";
        // B: a failing test exits 1.
        p.write("tests/t_bad.cpp", "#include <cstdio>\nint main() { std::puts(\"output of the failing test\"); return 1; }\n");
        auto b = p.mcpp({"test"});
        expect(e2e::exited(b, 1));
        expect(e2e::contains(b.out, "t_bad ... FAIL"));
        expect(e2e::contains(b.out, "output of the failing test"));
        expect(e2e::contains(b.out, "failures:"));
        expect(e2e::contains(b.err, "test result: FAILED")) << "the failed summary is not on standard error";
        expect(s::no_line(b.out, "Compiling|Resolving")) << "a step is on standard output";
        p.remove("tests/t_bad.cpp");
        // C: --list names on standard output alone.
        auto c = p.mcpp({"test", "--list"});
        expect(fatal(e2e::succeeded(c)));
        expect(s::has_line(c.out, "^t_ok$"));
        expect(s::lacks(c.err, "t_ok")) << "a name is on standard error";
        // D: a workspace.
        s::project ws(s::case_dir() / "ws", {{"mcpp.toml", "[workspace]\nmembers = [\"a\", \"b\"]\n"}});
        for (auto m : {"a", "b"}) {
            ws.write(std::format("{}/mcpp.toml", m),
                     std::format("[package]\nname    = \"{}\"\nversion = \"0.1.0\"\n\n[targets.{}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", m, m));
            ws.write(std::format("{}/src/main.cpp", m), "int main() { return 0; }\n");
            ws.write(std::format("{}/tests/t_{}.cpp", m, m), "int main() { return 0; }\n");
        }
        auto d = ws.mcpp({"test", "--workspace"});
        expect(fatal(e2e::succeeded(d)));
        expect(s::has_line(d.out, R"(workspace result ok\.)"));
        expect(s::has_line(d.out, R"(test result ok\.)"));
        expect(s::no_line(d.out, "Compiling|Resolving|Resolved|Workspace")) << "a step is on standard output";
        expect(e2e::contains(d.err, "Compiling"));
        expect(s::lacks(d.err, "workspace result")) << "the workspace result line is on standard error";
    };

    "178 test observability"_test[tag{"requires:unix-shell"}] = [] {
        // `mcpp test` is streamed, timed and bounded. POSIX only: the deadline runner has no
        // kill-by-handle path on Windows, so --timeout and --build-timeout are documented POSIX-only.
        s::needs({"unix-shell"});
        s::project ws{{"mcpp.toml", "[workspace]\nmembers = [\"fast\", \"slow\"]\n"},
                      {"fast/mcpp.toml", "[package]\nname = \"fast\"\nversion = \"0.1.0\"\n"},
                      {"slow/mcpp.toml", "[package]\nname = \"slow\"\nversion = \"0.1.0\"\n"},
                      {"fast/tests/quick.cpp", "int main() { return 0; }\n"},
                      {"slow/tests/hang.cpp", hang_cpp}};

        // 1. Streaming: the fast member's verdict reaches a pipe while the slow one still runs.
        {
            auto session = e2e::session::start(s::mcpp_command({"test", "--workspace", "--timeout", "25"}, ws.dir()));
            expect(fatal(session.started()));
            expect(session.saw(R"(quick \.\.\. ok)", std::chrono::seconds{25}))
                << "no output reached the pipe before the process ended (stdout not flushed)";
        }  // the session ends mcpp

        // 2. Timings: a test's duration and the build/run split.
        auto t2 = ws.mcpp({"test", "-p", "fast"});
        expect(fatal(e2e::succeeded(t2)));
        expect(s::has_line(s::text(t2), R"(quick \.\.\. ok \([0-9]+\.[0-9]+s\))")) << "the test's line carries no duration";
        expect(s::has_line(s::text(t2), R"(finished in [0-9]+\.[0-9]+s \(plan [0-9]+\.[0-9]+s( · programs [0-9]+\.[0-9]+s)? · build [0-9]+\.[0-9]+s · run [0-9]+\.[0-9]+s\))"))
            << "the summary does not divide its time into plan, build and run";

        // 3. --timeout bounds a hung run, and the fan-out continues.
        auto t3 = ws.mcpp({"test", "--workspace", "--timeout", "2"});
        expect(e2e::contains(s::text(t3), "hang ... FAIL (timeout after 2s)"));
        expect(s::has_line(s::text(t3), R"(member 'fast' \(1/2\) ok)")) << "no per-member progress line";
        expect(e2e::contains(s::text(t3), "failed members: slow"));
        expect(t3.started && t3.status != 0) << "the workspace test returned 0 despite a failed member";
        // `--timeout 0` means no limit.
        expect(e2e::succeeded(ws.mcpp({"test", "-p", "fast", "--timeout", "0"}))) << "--timeout 0 was refused";

        // 4. --build-timeout bounds a slow compile and names itself: 20k distinct class templates.
        std::string heavy;
        for (int i = 0; i < 20000; ++i) {
            heavy += std::format("template <int A> struct S{} {{ static constexpr long v = A*{}+1; }};\n", i, i);
            heavy += std::format("long f{}(long x) {{ return x + S{}<{}>::v; }}\n", i, i, i % 97);
        }
        heavy += "int main() { return 0; }\n";
        ws.write("heavy/mcpp.toml", "[package]\nname = \"heavy\"\nversion = \"0.1.0\"\n");
        ws.write("heavy/tests/slowbuild.cpp", heavy);
        expect(e2e::contains(s::text(ws.mcpp_in("heavy", {"test", "--build-timeout", "1"})), "build timeout after 1s"))
            << "--build-timeout did not stop the slow compile";
        // The deadlines share their argument's validation.
        expect(e2e::contains(s::text(ws.mcpp({"test", "-p", "fast", "--build-timeout", "abc"})), "invalid --build-timeout"));

        // 5. --workspace-timeout stops the fan-out and names what did not run (the slow member first).
        ws.write("wst/mcpp.toml", "[workspace]\nmembers = [\"one\", \"two\"]\n");
        ws.write("wst/one/mcpp.toml", "[package]\nname = \"wst_one\"\nversion = \"0.1.0\"\n");
        ws.write("wst/two/mcpp.toml", "[package]\nname = \"wst_two\"\nversion = \"0.1.0\"\n");
        ws.write("wst/one/tests/hang.cpp", hang_cpp);
        ws.write("wst/two/tests/quick.cpp", "int main() { return 0; }\n");
        auto t5 = ws.mcpp_in("wst", {"test", "--workspace", "--timeout", "3", "--workspace-timeout", "1"});
        expect(s::lacks(s::text(t5), "member 'two'")) << "--workspace-timeout did not stop the fan-out";
        expect(e2e::contains(s::text(t5), "not run (--workspace-timeout 1s reached)"));
        expect(t5.started && t5.status != 0) << "the workspace timeout returned 0";

        // 6. The JSON stream: no stray line, member-qualified, a workspace summary.
        auto t6 = ws.mcpp({"test", "--workspace", "--timeout", "2", "--message-format", "json"});
        expect(t6.out.starts_with("{")) << "the first line is not JSON (stdout polluted)";
        expect(e2e::contains(t6.out, R"("member":"fast")"));
        expect(e2e::contains(t6.out, R"("workspace_summary")"));
        expect(e2e::contains(t6.out, R"("failed_members":["slow"])"));
    };
}};

}  // namespace
