// A tool package declared by more than one package of a graph: one version, floors, ranges
// (formerly tests/e2e/627, 628, 629).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

const s::options fresh{.where = s::home::fresh};

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

// The build program of 627 and 629: records what `xpkg_dir("xim", "zoxide")` answered in the
// package's answered.txt (629's refuses an empty answer).
constexpr std::string_view recorder = R"cpp(#include <cstdio>
#include <string>
import mcpp;
int main() {
    const char* d = mcpp::xpkg_dir("xim", "zoxide");
    std::string out = std::string(mcpp::manifest_dir()) + "/answered.txt";
    std::FILE* f = std::fopen(out.c_str(), "w");
    if (f == nullptr) return 3;
    std::fprintf(f, "%s\n", d == nullptr ? "" : d);
    std::fclose(f);
    return 0;
}
)cpp";

constexpr std::string_view refusing_recorder = R"cpp(#include <cstdio>
#include <string>
import mcpp;
int main() {
    const char* d = mcpp::xpkg_dir("xim", "zoxide");
    if (d == nullptr || d[0] == '\0') {
        std::fprintf(stderr,
            "a [feature-xlings] entry stated as a range was installed and then "
            "answered as absent\n");
        return 2;
    }
    std::string out = std::string(mcpp::manifest_dir()) + "/answered.txt";
    std::FILE* f = std::fopen(out.c_str(), "w");
    if (f == nullptr) return 3;
    std::fprintf(f, "%s\n", d);
    std::fclose(f);
    return 0;
}
)cpp";

// dep/ (the library `toolowner`, `extra` appended to its manifest) and app/ (the binary
// `consumer` depending on it by path, `app_extra` in its manifest).
struct graph {
    s::project dep;
    s::project app;
};

graph make_graph(std::string_view dep_extra, std::string_view dep_dependency, std::string_view app_extra) {
    const auto tmp = temp_dir();
    graph g{s::project{tmp / "dep", {{"src/lib.cpp", "int dep_touch() { return 1; }\n"}}},
            s::project{tmp / "app", {{"src/main.cpp", "int main() { return 0; }\n"}}}};
    g.dep.write("mcpp.toml", std::format("[package]\nname    = \"toolowner\"\nversion = \"0.1.0\"\n[modules]\nsources = [\"src/**/*.cpp\"]\n{}[targets.toolowner]\nkind = \"lib\"\n", dep_extra));
    g.app.write("mcpp.toml", std::format("[package]\nname    = \"consumer\"\nversion = \"0.1.0\"\n[dependencies]\ntoolowner = {}\n{}[targets.consumer]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", dep_dependency, app_extra));
    return g;
}

E2E tools{"tools", [] {
    "627 one package one version"_test[tag{"requires:gcc"}] = [] {
        // A project and one of its dependencies both name `xim:zoxide`, at different versions:
        // one package, so one version is installed (the declaration nearer the artifact), the
        // dependency's build program is answered that version, and the override is reported.
        s::needs({"gcc"});
        auto g = make_graph("[xlings.workspace]\n\"xim:zoxide\" = \"0.9.7\"\n", "{ path = \"../dep\" }", "[xlings.workspace]\n\"xim:zoxide\" = \"0.9.9\"\n");
        g.dep.write("build.mcpp", recorder);
        auto b = g.app.mcpp({"build"}, fresh);
        expect(fatal(e2e::succeeded(b))) << "the consumer did not build";
        const auto store = s::case_home(s::home::fresh) / "registry/data/xpkgs/xim-x-zoxide";
        expect(fatal(fs::is_directory(store))) << "zoxide was never installed (" << store.string() << ")";
        std::vector<fs::path> installed;  // `ls -1`: the store's own entries
        for (const auto& e : fs::directory_iterator(store)) installed.push_back(e.path());
        expect(eq(installed.size(), std::size_t{1})) << "two declarations of one package installed " << installed.size() << " versions";
        expect(fs::is_directory(store / "0.9.9")) << "the declaration nearer the artifact did not win";
        expect(fatal(g.dep.exists("answered.txt") && !g.dep.read("answered.txt").empty())) << "the dependency's build program recorded nothing";
        expect(e2e::contains(g.dep.read("answered.txt"), "0.9.9")) << "installed 0.9.9 and answered " << g.dep.read("answered.txt");
        expect(e2e::contains(s::text(b), "declared at two versions")) << "the override was not reported";
        expect(e2e::contains(s::text(b), "toolowner")) << "the report does not name the dependency that lost";
    };

    "628 a pin below a stated floor is refused"_test = [] {
        // No capability gate: the first leg refuses before anything is compiled and the second
        // builds an ordinary library.
        auto g = make_graph("[xlings.workspace]\n\"xim:zoxide\" = \">=0.9.9\"\n", "{ path = \"../dep\" }", "[xlings.workspace]\n\"xim:zoxide\" = \"0.9.7\"\n");
        auto b = g.app.mcpp({"build"}, fresh);
        expect(fatal(failed(b))) << "a pin below the dependency's floor was accepted";
        for (std::string_view needle : {"xim:zoxide", "0.9.7", ">=0.9.9", "toolowner", "this project"})
            expect(e2e::contains(s::text(b), needle)) << "the refusal does not mention '" << needle << "'";
        expect(e2e::contains(s::text(b), "drop the pin")) << "the refusal does not state the way out";
        auto why = g.app.mcpp({"why", "toolchain", "--format", "json"}, fresh);
        const auto data = s::json(why.out);
        const auto reason = data.contains("data") && data["data"].contains("reason") ? data["data"]["reason"].get<std::string>() : std::string{};
        expect(eq(reason, std::string{"tool-version-conflict"})) << "reason was '" << reason << "', expected tool-version-conflict";
        g.app.write("mcpp.toml", "[package]\nname    = \"consumer\"\nversion = \"0.1.0\"\n[dependencies]\ntoolowner = { path = \"../dep\" }\n[xlings.workspace]\n\"xim:zoxide\" = \"0.9.9\"\n[targets.consumer]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n");
        expect(e2e::succeeded(g.app.mcpp({"build"}, fresh))) << "a pin that DOES satisfy the floor was still refused";
    };

    "629 a version range is answered by xpkg dir"_test[tag{"requires:gcc"}] = [] {
        // A range in an xlings address's version position is a constraint: installed, and
        // answered by `xpkg_dir`; an unsatisfiable one is refused, naming it.
        s::needs({"gcc"});
        auto g = make_graph("[features]\ndefault  = []\nusestool = []\n[feature-xlings.usestool]\n\"xim:zoxide\" = \">=0.9.9\"\n", "{ path = \"../dep\", features = [\"usestool\"] }", "");
        g.dep.write("build.mcpp", refusing_recorder);
        expect(fatal(e2e::succeeded(g.app.mcpp({"build"}, fresh)))) << "the consumer did not build";
        expect(fatal(g.dep.exists("answered.txt") && !g.dep.read("answered.txt").empty())) << "the build program recorded nothing";
        const auto answered = g.dep.read("answered.txt");
        expect(e2e::contains(answered, "xim-x-zoxide")) << "the recorded path does not name zoxide: " << answered;
        expect(s::has_line(answered, R"(/(0\.9\.9|0\.10\.[0-9]+)$)")) << "the answer does not satisfy >=0.9.9: " << answered;
        g.dep.write("mcpp.toml", "[package]\nname    = \"toolowner\"\nversion = \"0.1.0\"\n[modules]\nsources = [\"src/**/*.cpp\"]\n[features]\ndefault  = []\nusestool = []\n[feature-xlings.usestool]\n\"xim:zoxide\" = \">=99.0.0\"\n[targets.toolowner]\nkind = \"lib\"\n");
        fs::remove_all(g.app.at("target"));
        g.dep.remove("answered.txt");
        auto u = g.app.mcpp({"build"}, fresh);
        expect(fatal(failed(u))) << "an unsatisfiable range was accepted";
        expect(e2e::contains(s::text(u), "not found in the synced index")) << "refused, but not because the range is unsatisfiable";
        expect(e2e::contains(s::text(u), ">=99.0.0")) << "the refusal does not quote the range that could not be satisfied";
    };
}};

}  // namespace
