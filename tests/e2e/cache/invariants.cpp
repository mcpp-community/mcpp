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
        if (compiles(line.value("output", "")) && !line.value("served", false)) {
            ++b.compiled;
            b.compiledPackages.insert(line.value("package", ""));
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

    "1122 a dev-dependency is served from the cache to a second project"_test[s::gcc][at_most{"edges.compiled.dependency", 0}] = [] {
        // 5.4: mcpplibs.mtest, a dev-dependency of every test, is compiled once per home, not once
        // per project.
        s::own_home();
        auto one = s::project::scaffold("one");
        auto [first, built] = recorded(one, {"test"}, "one");
        expect(fatal(e2e::succeeded(first))) << s::text(first);
        auto two = s::project::scaffold("two");
        auto [second, rebuilt] = recorded(two, {"test"}, "two");
        expect(fatal(e2e::succeeded(second))) << s::text(second);
        std::size_t dependency = 0;
        for (const auto& pkg : rebuilt.compiledPackages)
            if (pkg.contains("mtest")) ++dependency;
        expect(eq(dependency, std::size_t{0})) << "the second project compiled the dev-dependency";
        bool served = false;
        for (const auto& [pkg, units] : rebuilt.cached) served = served || (pkg.contains("mtest") && units > 0);
        expect(served) << "the record names no unit of mcpplibs.mtest served from the cache";
        metric("edges.compiled.dependency", static_cast<double>(dependency), "edges");
    };
}};

}  // namespace
