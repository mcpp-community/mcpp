// A test run beside other commands, and the tests of an unchanged project (formerly tests/e2e/1068
// and 1069).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

void touch(const fs::path& f) { std::ofstream(f) << ""; }

E2E fast{"fast", [] {
    "1068 a running test holds no build directory"_test = [] {
        // `mcpp test` releases the build directory's lock once its tests are built (mcpp#915): a
        // build and a second test of the same configuration proceed while a test runs. The first
        // test waits for a file only the commands started beside it create.
        auto p = s::project::scaffold("app");
        for (const auto& f : s::glob(p.at("tests"), "*.cpp")) fs::remove(f);
        const auto gate = temp_dir() / "gate";
        p.write("tests/waits.cpp", std::format(R"(import std;
int main() {{
    for (int i = 0; i < 600; ++i) {{
        if (std::filesystem::exists("{}")) return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }}
    std::println("the gate never opened");
    return 1;
}}
)",
                                               gate.generic_string()));
        p.write("tests/quick.cpp", "int main() { return 0; }\n");

        // Warm: the tests are built.
        {
            auto warm = e2e::session::start(s::mcpp_command({"test"}, p.dir()));
            expect(fatal(warm.started()));
            std::this_thread::sleep_for(1s);
            touch(gate);
            auto r = warm.finish(10min);
            expect(fatal(e2e::succeeded(r))) << "the tests did not build\n" << warm.transcript();
        }
        fs::remove(gate);

        auto first = e2e::session::start(s::mcpp_command({"test", "waits"}, p.dir()));
        expect(fatal(first.started()));
        // The first test is running once its program has started.
        first.expect("Running|waits", 10s);
        std::this_thread::sleep_for(1s);
        auto open_and_end = [&] { touch(gate); first.finish(2min); };

        // Beside it: a build of the same configuration, and a second test.
        auto build = p.mcpp({"build"}, {.timeout = 60s});
        if (!build) {
            open_and_end();
            expect(fatal(e2e::succeeded(build))) << "mcpp build waited for a running test";
        }
        auto second = p.mcpp({"test", "quick"}, {.timeout = 60s});
        if (!second) {
            open_and_end();
            expect(fatal(e2e::succeeded(second))) << "a second mcpp test waited for a running one";
        }
        expect(s::lacks(s::text(build), "Waiting for the build directory"));
        expect(s::lacks(s::text(second), "Waiting for the build directory"));

        touch(gate);
        auto r = first.finish(2min);
        expect(e2e::succeeded(r)) << "the first test failed\n" << first.transcript();
        expect(e2e::contains(first.transcript(), "1 passed; 0 failed")) << "the first test's summary";
        expect(e2e::contains(s::text(second), "1 passed; 0 failed")) << "the second test's summary";
        // The protocol files were the invocations', and went with them.
        expect(s::glob(p.at("target"), "*/*/test-protocol/*").empty()) << "an ended invocation left its protocol directory";

        // The issue's own shape: a test that builds its own project with mcpp. The inner build has
        // the test's environment; it finds the directory free, the configuration built and its
        // graph current, so it neither waits nor plans.
        const auto inner_log = temp_dir() / "inner.log";
        p.write("tests/inner.cpp", std::format(R"(#include <cstdlib>
#ifdef _WIN32
#define WHOLE(s) "\"" s "\""   // cmd /c drops the outer pair of quotes
#else
#define WHOLE(s) s
#endif
int main() {{ return std::system(WHOLE("\"{}\" build > \"{}\" 2>&1")) == 0 ? 0 : 1; }}
)",
                                               fs::path(s::mcpp_path()).generic_string(), inner_log.generic_string()));
        auto prime = p.mcpp({"test", "inner"});
        expect(fatal(e2e::succeeded(prime))) << "a test that builds its project failed\n" << s::read_file(inner_log);
        auto again = p.mcpp({"test", "inner"}, {.timeout = 120s});
        expect(fatal(e2e::succeeded(again))) << "a test that builds its project failed\n" << s::read_file(inner_log);
        expect(s::no_line(s::read_file(inner_log), "Waiting for the build directory|Resolving")) << "the inner build waited or planned";
    };

    "1069 an unchanged project tests without planning"_test[resource{"machine"}] = [] {
        // A full `mcpp test` records what it planned beside test.ninja; the next one runs the
        // selected tests from it, resolving no dev-dependency. An edited test, a test added or
        // removed and an edited source each take the full path again. Alone on the machine: the
        // fast path declines when the home's environment description is newer than its graph.
        auto p = s::project::scaffold("app");
        for (const auto& f : s::glob(p.at("tests"), "*.cpp")) fs::remove(f);
        p.write("mcpp.toml", p.read("mcpp.toml") + "[dev-dependencies]\nmcpplibs.mtest = \"0.5.2\"\n");
        auto write_cases = [&](int two) {
            p.write("tests/cases.cpp", std::format(R"(import std;
import mcpplibs.mtest;
using namespace mcpplibs::mtest;
UTest one{{"one", [] {{ expect(eq(1, 1)); }}}};
UTest two{{"two", [] {{ expect(eq(2, {})); }}}};
)",
                                                   two));
        };
        write_cases(2);
        auto full = p.mcpp({"test"});
        expect(fatal(e2e::succeeded(full)));
        expect(e2e::contains(s::text(full), "Resolved mcpplibs.")) << "the first run did not plan";
        auto fast = p.mcpp({"test"});
        expect(fatal(e2e::succeeded(fast)));
        expect(s::lacks(s::text(fast), "Resolved mcpplibs.")) << "an unchanged project was planned again";
        expect(e2e::contains(s::text(fast), "cases: 2 passed, 0 failed"));
        // A selection runs from the record.
        auto one = p.mcpp({"test", "cases", "--case", "two"});
        expect(fatal(e2e::succeeded(one)));
        expect(s::lacks(s::text(one), "Resolved mcpplibs.")) << "a selection was planned again";
        expect(e2e::contains(s::text(one), "cases: 1 passed, 0 failed"));
        // An edited test is built, and its result is the edit's.
        std::this_thread::sleep_for(1s);
        write_cases(3);
        auto edited = p.mcpp({"test"});
        expect(edited.started && edited.status != 0) << "the edited test passed";
        expect(e2e::contains(s::text(edited), "cases: 1 passed, 1 failed"));
        auto again = p.mcpp({"test"});
        expect(again.started && again.status != 0) << "the failing test passed from the record";
        expect(s::lacks(s::text(again), "Resolved mcpplibs.")) << "an unchanged failing project was planned again";
        expect(e2e::contains(s::text(again), "cases: 1 passed, 1 failed")) << "the record's run lost the failure";
        // A test added is planned and run; one removed is no longer run.
        std::this_thread::sleep_for(1s);
        write_cases(2);
        p.write("tests/extra.cpp", "int main() { return 0; }\n");
        auto added = p.mcpp({"test"});
        expect(fatal(e2e::succeeded(added)));
        expect(e2e::contains(s::text(added), "extra ... ok"));
        p.remove("tests/extra.cpp");
        auto removed = p.mcpp({"test"});
        expect(fatal(e2e::succeeded(removed)));
        expect(s::lacks(s::text(removed), "extra")) << "a removed test ran";
        // A build after the tests keeps its own graph.
        auto build = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(build)));
        expect(s::lacks(s::text(build), "Compiling")) << "mcpp build after mcpp test compiled again";
    };
}};

}  // namespace
