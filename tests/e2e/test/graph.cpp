// What `mcpp test` builds: dev-dependencies, the profile, link order against a framework's main,
// per-glob flags, the goal set and the test graph beside the build's (formerly tests/e2e/18, 72,
// 78, 157, 164 and 213).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// The graph `mcpp test` writes (test.ninja, beside build.ninja since 2026.11.10.1), outside the
// build programs' directories.
std::optional<fs::path> test_graph(const s::project& p) {
    for (const auto& f : s::find_files(p.at("target"), "test\\.ninja"))
        if (!f.generic_string().contains("/.build-mcpp/"))
            return f;
    return std::nullopt;
}

E2E graph{"graph", [] {
    "18 devdeps isolation"_test = [] {
        // dev-dependencies are not resolved by `mcpp build` and are by `mcpp test`.
        auto p = s::project::scaffold("myapp");
        p.write("mcpp.toml", R"([package]
name        = "myapp"
version     = "0.1.0"

[language]
standard   = "c++23"
modules    = true
import_std = true

[modules]
sources = ["src/**/*.cppm", "src/**/*.cpp"]

[targets.myapp]
kind = "bin"
main = "src/main.cpp"

[dev-dependencies."ghost-test-framework"]
path = "/nonexistent/ghost-package"
)");
        auto b = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(b)));
        expect(e2e::contains(s::text(b), "Finished"));
        expect(s::lacks(s::text(b), "ghost-test-framework")) << "the build path resolved the dev-dependency";
        auto t = p.mcpp({"test"});
        expect(t.started && t.status != 0) << "the test should fail on the bogus dev-dependency";
        expect(e2e::contains(s::text(t), "ghost-test-framework")) << "the test path did not reach the dev-dependency";
    };

    "72 test profile features"_test[s::gcc] = [] {
        // `mcpp test --profile` selects the mode of the whole test build: it reaches the code
        // under test (a shared src/ unit), not only the test's own file (#131 scenario B).
        s::project p{{"mcpp.toml", R"([package]
name    = "proj"
version = "0.1.0"

[build]
sources = ["src/**/*.cpp"]

[profile.observe]
cxxflags = ["-DOBSERVE_MODE=1"]
)"},
                     {"src/probe.cpp", R"(extern "C" int probe_mode(void) {
#ifdef OBSERVE_MODE
    return 1;
#else
    return 0;
#endif
}
)"},
                     {"tests/test_mode.cpp",
                      "extern \"C\" int probe_mode(void);\nint main() { return probe_mode() == 1 ? 0 : 1; }\n"}};
        auto plain = p.mcpp({"test"});
        expect(plain.started && plain.status != 0) << "the default `mcpp test` passed: the macro reached the code";
        expect(e2e::succeeded(p.mcpp({"test", "--profile", "observe"})));
    };

    "78 test main combinations"_test = [] {
        // Every combination of {own main, the framework's main} x {uses gtest, does not} with gtest
        // a dev-dependency: a library dependency is linked as an archive after the test's objects,
        // so gtest_main.o is pulled only while main is undefined.
        auto p = s::project::scaffold("combo");
        // The scaffold's tests use mtest; these do not.
        auto manifest = p.read("mcpp.toml");
        manifest = std::regex_replace(manifest, std::regex("\n\\[dev-dependencies\\]\nmcpplibs\\.mtest = \"[^\"]*\"\n"), "");
        p.write("mcpp.toml", manifest + "\n[dev-dependencies.compat]\ngtest = \"1.15.2\"\n");
        p.remove("tests/test_smoke.cpp");
        p.write("tests/t_own_main_gtest.cpp", R"(#include <gtest/gtest.h>
import std;
TEST(A, ok) { EXPECT_EQ(1 + 1, 2); }
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
)");
        p.write("tests/t_framework_main.cpp", "#include <gtest/gtest.h>\nTEST(B, ok) { EXPECT_TRUE(true); }\n");
        p.write("tests/t_own_main_no_gtest.cpp", "import std;\nint main() { std::println(\"ok\"); return 0; }\n");
        auto r = p.mcpp({"test"});
        expect(fatal(e2e::succeeded(r)));
        const auto out = s::text(r);
        for (auto t : {"t_own_main_gtest", "t_framework_main", "t_own_main_no_gtest"})
            expect(e2e::contains(out, std::format("{} ... ok", t)));
        expect(e2e::contains(out, "3 passed; 0 failed"));
        // The link line `build bin<sep><name>[.exe] : cxx_link ...` of each.
        auto nj = test_graph(p);
        expect(fatal(nj.has_value())) << "no test.ninja";
        const auto graph = s::read_file(*nj);
        std::string own, fw;
        for (const auto& l : s::lines_with(graph, " : cxx_link")) {
            if (std::regex_search(l, std::regex(R"(t_own_main_gtest(\.exe)? : cxx_link)"))) own += l;
            if (std::regex_search(l, std::regex(R"(t_framework_main(\.exe)? : cxx_link)"))) fw += l;
        }
        expect(fatal(!own.empty())) << "no link line for t_own_main_gtest";
        expect(fatal(!fw.empty())) << "no link line for t_framework_main";
        expect(s::lacks(own, "gtest_main")) << "the own-main test links gtest_main (a duplicate main)";
        expect(e2e::contains(fw, "gtest_main")) << "the framework-main test does not link gtest_main";
    };

    "157 test glob flags"_test = [] {
        // [build].flags globs cover tests/ units too, and a glob that matches real files (not
        // scanned sources) does not warn that it matched nothing; a dead glob still does.
        s::project p{{"mcpp.toml", R"([package]
name = "globtest"
version = "0.1.0"
standard = "c++23"

[build]
flags = [
  { glob = "tests/tagged*.cpp", cxxflags = ["-DTAG=7"] },
  { glob = "tests/never/**",    cxxflags = ["-DNOPE"] },
]
)"},
                     {"tests/tagged.cpp", R"(#ifndef TAG
#error "per-glob cxxflags did not reach the test TU"
#endif
int main() { return TAG == 7 ? 0 : 1; }
)"},
                     {"tests/plain.cpp", R"(#ifdef TAG
#error "per-glob cxxflags leaked into an unmatched test TU"
#endif
int main() { return 0; }
)"}};
        auto r = p.mcpp({"test"});
        expect(fatal(e2e::succeeded(r)));
        const auto out = s::text(r);
        expect(e2e::contains(out, "tagged ... ok"));
        expect(e2e::contains(out, "plain ... ok"));
        expect(s::lacks(out, "'tests/tagged*.cpp' matched no source file")) << "a spurious dead-glob warning";
        expect(e2e::contains(out, "'tests/never/**' matched no source file")) << "a dead glob must still warn";
    };

    "164 ninja goal command length"_test = [] {
        // The explicit goal set of `mcpp test` reaches ninja through the manifest (a phony edge),
        // never on the command line (FFmpeg's 50,781-character argv against cmd.exe's 8191).
        s::project p{{"mcpp.toml", "[package]\nname    = \"goalset\"\nversion = \"0.1.0\"\n"},
                     {"src/main.cpp", "int main() { return 0; }\n"},
                     {"tests/sanity.cpp", "extern int unit_1();\nint main() { return unit_1() == 1 ? 0 : 1; }\n"}};
        for (int i = 1; i <= 30; ++i)
            p.write(std::format("src/unit_{}.cpp", i), std::format("int unit_{}() {{ return {}; }}\n", i, i));
        auto r = p.mcpp({"test"});
        expect(fatal(e2e::succeeded(r)));
        expect(e2e::contains(s::text(r), "sanity ... ok"));
        auto nj = test_graph(p);
        expect(fatal(nj.has_value())) << "no test.ninja";
        expect(s::has_line(s::read_file(*nj), "^build mcpp-requested-goals : phony "))
            << "the explicit goals are not aggregated into a phony edge";
        // mcpp records the ninja program it ran in target/.build_cache; the goals never belong there.
        if (p.exists("target/.build_cache"))
            expect(s::no_line(p.read("target/.build_cache"), "obj/.*obj/.*obj/"))
                << "goal paths leaked into the recorded ninja invocation";
    };

    "213 build after test is not the test graph"_test[s::gcc] = [] {
        // mcpp#407: a plain build after `mcpp test` must not replay the test's graph, and a broken
        // test file must not fail a plain build. Nothing is touched or deleted between the
        // commands: the defect exists only while the fast path is eligible.
        s::project p{{"mcpp.toml", "[package]\nname = \"tst\"\nversion = \"0.1.0\"\n"},
                     {"src/main.cpp", "int main() { return 0; }\n"},
                     {"tests/smoke.cpp", "int main() { return 0; }\n"}};
        auto target_bin = [&] { return s::glob(p.at("target"), "*/*/bin/tst"); };
        auto build_graph = [&] { return s::find_file(p.at("target"), "build.ninja"); };

        // 1. A plain build declares its graph.
        expect(fatal(e2e::succeeded(p.mcpp({"build"}))));
        auto n = build_graph();
        expect(fatal(n.has_value())) << "no build.ninja after the first build";
        expect(!target_bin().empty()) << "the plain build produced no bin/tst";
        expect(fatal(s::has_line(s::read_file(*n), "^# mcpp:graph=normal")));

        // 2. `mcpp test` writes test.ninja beside it and leaves build.ninja the build's.
        expect(fatal(e2e::succeeded(p.mcpp({"test"}))));
        const auto t = n->parent_path() / "test.ninja";
        expect(fatal(fs::is_regular_file(t))) << "mcpp test wrote no test.ninja beside build.ninja";
        expect(s::has_line(s::read_file(t), "^# mcpp:graph=test"));
        expect(s::has_line(s::read_file(*n), "^# mcpp:graph=normal")) << "mcpp test rewrote build.ninja";

        // 3. A plain build after the test does not replay that graph.
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "the plain build after mcpp test failed";
        auto n3 = build_graph();
        expect(fatal(n3.has_value()));
        const auto g3 = s::read_file(*n3);
        expect(s::has_line(g3, "^# mcpp:graph=normal")) << "the plain build replayed the test graph (mcpp#407)";
        const auto defaults = s::lines_with(g3, "default ");
        std::string default_line;
        for (const auto& l : defaults)
            if (l.starts_with("default ")) { default_line = l; break; }
        expect(e2e::contains(default_line, "bin/tst")) << "the default line names the test graph";
        expect(!target_bin().empty()) << "bin/tst is gone after a plain build";

        // 4. A broken test file does not fail a plain build.
        p.write("tests/smoke.cpp", "this is not valid C++ at all\n");
        expect(e2e::succeeded(p.mcpp({"build"}))) << "a broken tests/*.cpp failed a plain mcpp build";
    };
}};

}  // namespace
