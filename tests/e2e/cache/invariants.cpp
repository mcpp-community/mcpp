// The invariants of a build that each pull request keeps (design 2026-11-20 §6): what a second
// build of the same thing does, measured from the build's own record (MCPP_BUILD_RECORD,
// SPEC-017 B4) and stated as metrics with bounds. Each was a defect when it was introduced, and no
// test saw it: #986 (a test compiled the package again), 5.2 (a second project compiled the
// platform), 5.3 (a fresh checkout compiled what the cache held), 5.4 (a dev-dependency missed the
// cache). The cases of #986, 5.2 and 5.3 are 986 (workspace/inheritance.cpp), 955 (deps/git.cpp)
// and 927 (cache/entries.cpp).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// The compile edges of the builds a record file holds, and the units the cache served each package.
struct Built {
    std::size_t compiled = 0;
    std::map<std::string, std::size_t> cached;
    std::set<std::string> compiledPackages;
    std::vector<std::string> compiledOutputs;  // `<package> <output>`, for the failure's message
};

bool compiles(std::string_view output) {
    for (auto ext : {".o", ".obj", ".gcm", ".pcm", ".ifc"})
        if (output.ends_with(ext)) return true;
    return false;
}

Built read_record(const fs::path& file) {
    Built b;
    for (const auto& line : s::json_lines(s::read_file(file))) {
        if (line.value("cached", false)) {
            b.cached[line.value("package", "")] += line.value("units", std::size_t{0});
            continue;
        }
        // A unit the unit cache placed (`served`) or copied for a package the cache served
        // (`staged`) ran no compiler.
        if (compiles(line.value("output", "")) && !line.value("served", false) && !line.value("staged", false)) {
            ++b.compiled;
            b.compiledPackages.insert(line.value("package", ""));
            b.compiledOutputs.push_back(line.value("package", "") + " " + line.value("output", ""));
        }
    }
    return b;
}

// mcpp in `p` with its build record written to a file of the case: what the build did.
std::pair<e2e::outcome, Built> recorded(const s::project& p, std::vector<std::string> args, std::string_view name) {
    const auto file = s::case_dir() / std::format("record-{}.ndjson", name);
    std::error_code ec;
    fs::remove(file, ec);
    auto r = p.mcpp(std::move(args), {.env = {{"MCPP_BUILD_RECORD", file.string()}}});
    return {r, read_record(file)};
}

E2E invariants{"invariants", s::home_fixture, [] {
    "1121 an unchanged project builds again with no compile"_test[s::gcc][at_most{"edges.compiled", 0}] = [] {
        s::own_home();
        auto p = s::project::scaffold("same");
        auto [first, built] = recorded(p, {"build"}, "first");
        expect(fatal(e2e::succeeded(first))) << s::text(first);
        expect(fatal(gt(built.compiled, std::size_t{0}))) << "the first build compiled nothing: its record is empty";
        auto [again, rebuilt] = recorded(p, {"build"}, "again");
        expect(fatal(e2e::succeeded(again))) << s::text(again);
        metric("edges.compiled", static_cast<double>(rebuilt.compiled), "edges");
    };

    "1176 a build with no edit after an edit of a module compiles nothing"_test[at_most{"edges.compiled", 0}] = [] {
        // mcpp#1015: `c` is one file, imported by `b`, imported by `main`. A build after an edit of
        // `c`'s body compiles what the edit reaches, and the next build, which has no edit,
        // compiles nothing. Under clang the importer of the importer was compiled one build late:
        // its depfile names `c`'s BMI by the absolute path clang finds it through, which ninja held
        // as another file than the graph's `pcm.cache/c.pcm`. The host's default toolchain: clang
        // on macOS and Windows, where the defect was.
        s::own_home();
        s::project p({{"mcpp.toml", "[package]\nname    = \"cascade\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.cppm\", \"src/*.cpp\"]\n\n"
                                    "[targets.cascade]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                      {"src/c.cppm", "export module c;\nexport int cv() { return 1; }\n"},
                      {"src/b.cppm", "export module b;\nimport c;\nexport int bv() { return cv() + 1; }\n"},
                      {"src/main.cpp", "import b;\nint main() { return bv() > 0 ? 0 : 1; }\n"}});
        auto [cold, built] = recorded(p, {"build"}, "cold");
        expect(fatal(e2e::succeeded(cold))) << s::text(cold);
        p.write("src/c.cppm", "export module c;\nexport int cv() { return 3; }\n");
        auto [edit, edited] = recorded(p, {"build"}, "edit");
        expect(fatal(e2e::succeeded(edit))) << s::text(edit);
        expect(fatal(gt(edited.compiled, std::size_t{0}))) << "the edit compiled nothing";
        auto [again, rebuilt] = recorded(p, {"build"}, "again");
        expect(fatal(e2e::succeeded(again))) << s::text(again);
        std::string compiled;
        for (auto const& o : rebuilt.compiledOutputs) compiled += "\n  " + o;
        expect(eq(rebuilt.compiled, std::size_t{0})) << "a build with no edit compiled:" << compiled;
        metric("edges.compiled", static_cast<double>(rebuilt.compiled), "edges");
    };

    "1122 a dev-dependency is served from the cache to a second project"_test[s::gcc][at_most{"edges.compiled.dependency", 1}] = [] {
        // 5.4: mcpplibs.mtest, a dev-dependency of every test, is compiled once per home, not once
        // per project. Its entry unit (`main`) is the one compile left: a package's entry unit is
        // compiled with the image that links it, never taken from the package's entry in the cache.
        s::own_home();
        auto one = s::project::scaffold("one");
        auto [first, built] = recorded(one, {"test"}, "one");
        expect(fatal(e2e::succeeded(first))) << s::text(first);
        auto two = s::project::scaffold("two");
        auto [second, rebuilt] = recorded(two, {"test"}, "two");
        expect(fatal(e2e::succeeded(second))) << s::text(second);
        std::size_t dependency = 0;
        std::string compiled;
        for (const auto& edge : rebuilt.compiledOutputs)
            if (edge.contains("mtest")) { ++dependency; compiled += "\n  " + edge; }
        expect(le(dependency, std::size_t{1})) << "the second project compiled the dev-dependency:" << compiled << "\n"
                                               << s::text(second);
        bool served = false;
        for (const auto& [pkg, units] : rebuilt.cached) served = served || (pkg.contains("mtest") && units > 0);
        expect(served) << "the record names no unit of mcpplibs.mtest served from the cache";
        metric("edges.compiled.dependency", static_cast<double>(dependency), "edges");
    };
}};

}  // namespace
