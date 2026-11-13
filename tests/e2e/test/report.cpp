// `mcpp test` runs each test and reports it: verdicts, failures, the JSON stream, signals and
// timeouts (formerly tests/e2e/15, 16, 17, 153, 155, 158 and 160).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;

namespace {

std::string manifest(std::string_view name) {
    return std::format("[package]\nname = \"{}\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n", name);
}

E2E report{"report", [] {
    "15 test passing"_test = [] {
        // `mcpp test` discovers tests/**/*.cpp and runs each as a separate binary; all passing:
        // exit 0 and the summary.
        auto p = s::project::scaffold("myapp");
        p.write("tests/test_other.cpp", "import std;\nint main() { std::println(\"test_other: ok\"); return 0; }\n");
        auto r = p.mcpp({"test"});
        expect(fatal(e2e::succeeded(r)));
        const auto out = s::text(r);
        expect(e2e::contains(out, "test_smoke ... ok"));
        expect(e2e::contains(out, "test_other ... ok"));
        expect(e2e::contains(out, "2 passed; 0 failed"));
    };

    "16 test failing"_test[s::scan_deps] = [] {
        // One ok and one failing test: exit non-zero, and the summary lists the failures.
        auto p = s::project::scaffold("myapp");
        p.write("tests/test_fail.cpp", "int main() { return 1; }\n");
        auto r = p.mcpp({"test"});
        expect(r.started && r.status != 0) << "expected a non-zero exit\n" << r.line;
        const auto out = s::text(r);
        expect(e2e::contains(out, "test_fail ... FAIL"));
        expect(e2e::contains(out, "test_smoke ... ok"));
        expect(e2e::contains(out, "failures:"));
        expect(e2e::contains(out, "1 passed; 1 failed"));
    };

    "17 test no tests"_test = [] {
        // A project with no tests/: `mcpp test` says "no tests found" (and exits 0).
        auto p = s::project::scaffold("myapp");
        p.remove("tests");
        auto r = p.mcpp({"test"});
        expect(e2e::succeeded(r));
        expect(e2e::contains(s::text(r), "no tests found"));
    };

    "153 test isolation"_test = [] {
        // A test that does not compile fails alone; the others build and run.
        s::project p{{"mcpp.toml", manifest("isolation")},
                     {"tests/ok.cpp", "int main() { return 0; }\n"},
                     {"tests/runfail.cpp", "int main() { return 1; }\n"},
                     {"tests/nocompile.cpp", "int main() { D2X_YOUR_ANSWER x = 1; return x; }\n"}};
        auto r = p.mcpp({"test"});
        expect(e2e::exited(r, 1));
        const auto out = s::text(r);
        expect(e2e::contains(out, "ok ... ok"));
        // A test's line carries its own wall time: the prefix is matched.
        expect(e2e::contains(out, "runfail ... FAIL (exit 1,"));
        expect(e2e::contains(out, "nocompile ... FAIL (compile,"));
        expect(e2e::contains(out, "1 passed"));
        expect(e2e::contains(out, "2 failed"));

        // A package that does not compile stays a hard error: 'build failed', not all red.
        p.write("src/isolation.cppm", "export module isolation;\nthis is not C++;\n");
        auto r2 = p.mcpp({"test"});
        expect(r2.started && r2.status != 0) << "a package error must fail\n" << r2.line;
        expect(s::lacks(s::text(r2), "nocompile ... FAIL (compile,")) << "the package error was given to the tests";
    };

    "155 test json"_test = [] {
        // --message-format json: a record per test, the package error's record, stdout pure.
        s::project p{{"mcpp.toml", manifest("jsonout")},
                     {"tests/ok.cpp", "#include <cstdio>\nint main() { std::puts(\"hello from ok\"); return 0; }\n"},
                     {"tests/runfail.cpp", "int main() { return 1; }\n"},
                     {"tests/nocompile.cpp", "int main() { D2X_YOUR_ANSWER x = 1; return x; }\n"}};
        auto r = p.mcpp({"test", "--message-format", "json"});
        expect(e2e::exited(r, 1));
        for (const auto& line : s::lines_with(r.out, ""))
            if (!line.empty())
                expect(line.starts_with("{")) << "a line of stdout is not JSON: " << line;
        expect(e2e::contains(r.out, R"("test":"ok","status":"pass")"));
        expect(e2e::contains(r.out, R"("test":"runfail","status":"run_fail")"));
        expect(e2e::contains(r.out, R"("exit_code":1)"));
        expect(e2e::contains(r.out, R"("test":"nocompile","status":"compile_fail")"));
        expect(e2e::contains(r.out, "D2X_YOUR_ANSWER")) << "the compile output carries no diagnostic";
        expect(e2e::contains(r.out, "hello from ok")) << "the run output is not captured";
        // The summary leads with its member and carries the build/run split: the counts are matched.
        expect(e2e::contains(r.out, R"("passed":1,"failed":2)"));
        expect(e2e::matches(r.out, R"("build_ms":[0-9]+,"run_ms":[0-9]+)"));
        // An invalid format is a usage error.
        expect(e2e::exited(p.mcpp({"test", "--message-format", "yaml"}), 2));
    };

    "158 test signal and duration"_test[s::unix_shell] = [] {
        // A signaled test reports the shell's 128+signal with the signal in JSON, and every
        // test record carries duration_ms.
        s::project p{{"mcpp.toml", manifest("sigdur")},
                     {"tests/crash.cpp", "int main() { int* p = nullptr; return *p; }\n"},
                     {"tests/ok.cpp", "int main() { return 0; }\n"}};
        auto r = p.mcpp({"test", "--message-format", "json"});
        expect(e2e::exited(r, 1));
        expect(e2e::contains(r.out, R"("test":"crash","status":"run_fail","exit_code":139,"signal":11)"));
        expect(e2e::contains(r.out, R"("test":"ok","status":"pass")"));
        expect(eq(s::count_lines(r.out, R"("duration_ms":)"), 2u));
        // The human report uses the shell's code too; its line carries the wall time.
        expect(e2e::contains(s::text(p.mcpp({"test", "crash"})), "crash ... FAIL (exit 139,"));
    };

    "160 test timeout"_test[s::unix_shell] = [] {
        // `mcpp test --timeout <secs>`: a hung test is killed and reported; the rest still runs.
        s::project p{{"mcpp.toml", manifest("hang")},
                     {"tests/sleepy.cpp",
                      "import std;\nint main() { std::this_thread::sleep_for(std::chrono::seconds(30)); return 0; }\n"},
                     {"tests/quick.cpp", "int main() { return 0; }\n"}};
        const auto start = std::chrono::steady_clock::now();
        auto r = p.mcpp({"test", "--timeout", "2"});
        const auto elapsed = std::chrono::steady_clock::now() - start;
        expect(e2e::exited(r, 1));
        expect(elapsed < std::chrono::seconds{25}) << "the timeout did not take effect";
        const auto out = s::text(r);
        expect(e2e::contains(out, "sleepy ... FAIL (timeout"));
        expect(e2e::contains(out, "quick ... ok"));
        auto j = p.mcpp({"test", "--timeout", "2", "--message-format", "json"});
        expect(e2e::contains(j.out, R"("test":"sleepy","status":"run_fail")"));
        expect(e2e::contains(j.out, R"("timed_out":true)"));
    };
}};

}  // namespace
