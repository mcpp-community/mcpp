// Test programs of a directory, and the tests a change affects (formerly tests/e2e/1038).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

E2E affected{"affected", [] {

    "1038 test programs of a directory and the tests a change affects"_test[tag{"requires:gcc"}] = [] {
        s::needs({"gcc"});
        auto p = s::project::scaffold("app");
        p.write("src/calc.cppm", "export module app.calc;\nexport int twice(int x) { return 2 * x; }\n");
        p.write("tests/unit/a.cpp", R"(import mcpplibs.mtest;
import app.calc;
using namespace mcpplibs::mtest;
UTest a{"doubles", [] { expect(eq(twice(2), 4)); }};
)");
        p.write("tests/unit/b.cpp", R"(import std;
import mcpplibs.mtest;
using namespace mcpplibs::mtest;
UTest b{"reads its data", [] { expect(eq(std::string_view("tests/data/f.txt").size(), 16u)); }};
)");
        p.write("tests/e2e/plain.cpp", "import std;\nint main() { return 0; }\n");
        p.write("tests/data/f.txt", "data\n");
        // A
        p.write("mcpp.toml", p.read("mcpp.toml") + "\n[test]\nprograms = \"directory\"\n");
        auto a = p.mcpp({"test"});
        expect(fatal(e2e::succeeded(a)));
        // A program that speaks the protocol runs as a process per file of its cases.
        expect(s::has_line(s::text(a), R"(unit/a \.\.\. ok \(.*1 case\))"));
        expect(s::has_line(s::text(a), R"(unit/b \.\.\. ok \(.*1 case\))"));
        auto ap = p.mcpp({"test", "unit/b"});
        expect(e2e::succeeded(ap));
        expect(e2e::contains(s::text(ap), "unit/b ... ok"));
        expect(s::lacks(s::text(ap), "unit/a ...")) << "the pattern of a part did not run that part alone";
        p.write("mcpp.toml", p.read("mcpp.toml") + "split = \"none\"\n");
        auto a1 = p.mcpp({"test"});
        expect(fatal(e2e::succeeded(a1)));
        expect(s::has_line(s::text(a1), R"(unit \.\.\. ok \(.*2 cases\))")) << "unit is not one program of two cases";
        expect(s::has_line(s::text(a1), R"(^e2e \.\.\. ok)")) << "the directory of a file with main";
        auto ac = p.mcpp({"test", "--cases"});
        expect(fatal(e2e::succeeded(ac)));
        expect(e2e::contains(s::text(ac), "doubles"));
        expect(e2e::contains(s::text(ac), "reads its data"));
        // B
        expect(e2e::contains(s::text(ac), "e2e: one case, its exit status")) << "a program of plain main is listed as one case";
        p.write("tests/e2e/second.cpp", "int main() { return 0; }\n");
        auto a2 = p.mcpp({"test"});
        expect(a2.started && a2.status != 0) << "two files of one directory that define main were taken";
        expect(e2e::contains(s::text(a2), "plain.cpp"));
        expect(e2e::contains(s::text(a2), "second.cpp"));
        p.remove("tests/e2e/second.cpp");
        // C
        auto changed = [&](std::initializer_list<std::string_view> paths) {
            std::vector<std::string> args{"test"};
            for (auto x : paths) { args.emplace_back("--changed"); args.emplace_back(x); }
            auto r = p.mcpp(args);
            expect(e2e::succeeded(r));
            return s::text(r);
        };
        auto c1 = changed({"src/calc.cppm"});
        expect(e2e::contains(c1, "Affected 1 of 3 tests"));
        expect(e2e::contains(c1, "unit ... ok"));
        expect(s::lacks(c1, "test_smoke ...")) << "a test not built from the change ran";
        expect(e2e::contains(changed({"README.md"}), "Affected 0 of 3 tests"));
        auto c3 = changed({"tests/data/f.txt"});
        expect(e2e::contains(c3, "Affected 1 of 3 tests"));
        expect(e2e::contains(c3, "unit ... ok"));
        expect(e2e::contains(changed({"mcpp.toml"}), "Affected 3 of 3 tests"));
        auto git = [&](std::vector<std::string> args) {
            args.insert(args.begin(), "git");
            return e2e::run({.argv = std::move(args), .cwd = p.dir()});
        };
        expect(fatal(e2e::succeeded(git({"init", "-q", "."}))));
        expect(fatal(e2e::succeeded(git({"add", "-A"}))));
        expect(fatal(e2e::succeeded(git({"-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "base"}))));
        p.write("src/calc.cppm", p.read("src/calc.cppm") + "\nexport int thrice(int x) { return 3 * x; }\n");
        auto c5 = p.mcpp({"test", "--since", "HEAD"});
        expect(e2e::succeeded(c5));
        expect(e2e::contains(s::text(c5), "Affected 1 of 3 tests")) << "--since reads the working tree's change";
        // D: back to a program per file, and to the directories.
        auto manifest = p.read("mcpp.toml");
        p.write("mcpp.toml", manifest.substr(0, manifest.find("[test]")));
        auto d = p.mcpp({"test"});
        expect(e2e::succeeded(d));
        expect(e2e::contains(s::text(d), "unit/a ... ok"));
        expect(e2e::contains(s::text(d), "e2e/plain ... ok"));
        p.write("mcpp.toml", p.read("mcpp.toml") + "\n[test]\nprograms = \"directory\"\n");
        expect(e2e::succeeded(p.mcpp({"test"})));
        // E: a shipping profile.
        auto rel = s::project::scaffold("rel");
        rel.write("src/where.cppm", R"(export module rel.where;
import std;
export std::string library_file() { return std::source_location::current().file_name(); }
)");
        rel.write("tests/where.cpp", R"(import std;
import rel.where;
int main() {
    const std::string here = std::source_location::current().file_name();
    std::println("test {}", here);
    std::println("library {}", library_file());
    return std::filesystem::exists(std::filesystem::path(here).parent_path() / "test_smoke.cpp") ? 0 : 1;
}
)");
        expect(fatal(e2e::succeeded(rel.mcpp({"build", "--release"}))));
        auto et = rel.mcpp({"test", "--profile", "release", "where"});
        expect(e2e::succeeded(et)) << "the release test (its file did not find its directory)";
        const auto rg = s::glob(rel.at("target"), "*/*/build.ninja");
        expect(fatal(eq(rg.size(), 1u))) << "build and test are several configurations";
        expect(e2e::contains(s::text(et), "library rel@0.1.0/src/where.cppm")) << "the code under test is not trimmed as built";
        const auto where_edges = std::ranges::count_if(s::ninja_log_outputs(rg.front().parent_path()), [](const std::string& o) {
            return std::regex_search(o, std::regex(R"(obj/where\.m\.(o|obj)$)"));
        });
        expect(eq(where_edges, 1)) << "the test compiled the library again";
        // F
        auto grp = s::project::scaffold("grp");
        grp.write("tests/test_smoke.cpp", R"(import mcpplibs.mtest;
using namespace mcpplibs::mtest;
UTest g{"g", [] {
    "one"_test = [] { expect(true); };
    "two"_test = [] { expect(true); };
}};
)");
        auto fc = grp.mcpp({"test", "--cases"});
        expect(fatal(e2e::succeeded(fc)));
        expect(e2e::contains(s::text(fc), "test_smoke: 1 case")) << "the object is the one listed case";
        auto f1 = grp.mcpp({"test", "--case", "g/one"});
        expect(e2e::succeeded(f1));
        expect(s::has_line(s::text(f1), R"(test_smoke \.\.\. ok \(.*1 case\))")) << "a declared case is not selected alone";
        auto f2 = grp.mcpp({"test", "--case", "g/t*"});
        expect(e2e::succeeded(f2));
        expect(s::has_line(s::text(f2), R"(test_smoke \.\.\. ok \(.*3 cases\))")) << "a pattern below the listed case does not run it whole";
    };
}};

}  // namespace
