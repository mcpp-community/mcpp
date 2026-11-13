// The build program of a dependency: its scope, where its outputs live, what it is told of its own
// dependencies (formerly tests/e2e/111, 125).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// The regular files under `root` named `name` that their owner may execute.
std::vector<fs::path> executables(const fs::path& root, std::string_view name) {
    std::vector<fs::path> out;
    for (const auto& f : s::find_files(root, ".*"))
        if (f.filename() == name && (fs::status(f).permissions() & fs::perms::owner_exec) != fs::perms::none)
            out.push_back(f);
    return out;
}

// `strings -a <bin>; nm -C <bin>` holds `symbol`: the bytes hold it, or the demangled table does.
bool has_symbol(const fs::path& bin, std::string_view symbol) {
    if (s::read_file(bin).contains(symbol))
        return true;
    if (auto nm = s::on_path("nm"))
        return s::run(*nm, {"-C", bin.string()}).out.contains(symbol);
    return false;
}

std::string library(std::string_view name) {
    return std::format("[package]\nname    = \"{0}\"\nversion = \"0.1.0\"\n\n[targets.{0}]\nkind = \"lib\"\n", name);
}

E2E deps{"deps", [] {
    "111 dep build mcpp"_test[s::gcc] = [] {
        // A dependency's build.mcpp runs with Cargo's scope: its define reaches only the
        // dependency's own units, its generated source (in MCPP_OUT_DIR) compiles into it, and
        // what it writes lives in the consuming project; the dependency's root is never written.
        const auto root = s::case_dir();
        s::project dep{root / "deplib", {{"mcpp.toml", "[package]\nname    = \"deplib\"\nversion = \"0.1.0\"\n\n[targets.deplib]\nkind = \"lib\"\n"},
                                         {"build.mcpp", R"cpp(#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
int main() {
    const char* out = std::getenv("MCPP_OUT_DIR");
    if (!out) { std::fprintf(stderr, "MCPP_OUT_DIR unset\n"); return 1; }
    std::ofstream f(std::string(out) + "/gen_dep.cpp");
    f << "extern \"C\" int dep_gen_answer() { return 77; }\n";
    if (!f) return 1;
    std::printf("mcpp:generated=gen_dep.cpp\n");
    std::printf("mcpp:cfg=DEP_HAS_BP\n");
    return 0;
}
)cpp"},
                                         {"src/dep.cpp", R"cpp(#ifndef DEP_HAS_BP
#error "dep build.mcpp define did not reach the dep's own TU"
#endif
extern "C" int dep_gen_answer();
extern "C" int dep_answer() { return dep_gen_answer(); }
)cpp"}}};
        s::project app{root / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies.deplib]\npath = \"../deplib\"\n"},
                                      {"src/main.cpp", R"cpp(#ifdef DEP_HAS_BP
#error "dep build.mcpp define leaked into the consumer TU"
#endif
import std;
extern "C" int dep_answer();
int main() {
    std::println("dep = {}", dep_answer());
    return 0;
}
)cpp"}}};
        const auto before = s::listing(dep.dir());
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "build failed";
        expect(eq(s::last_line(app.mcpp({"run"}).out), std::string("dep = 77")));
        expect(eq(s::listing(dep.dir()), before)) << "dependency root was written to";
        // The program lives in .build-mcpp/deps, its output in this configuration's directory.
        bool found = false;
        for (const auto& f : s::find_files(app.at("target/.build-mcpp/out"), R"(gen_dep\.cpp)"))
            found = found || f.generic_string().ends_with("deplib/gen_dep.cpp");
        expect(found) << "dep OUT_DIR generated file not in project tree";
    };

    "125 build mcpp dep dir"_test[s::elf][s::gcc] = [] {
        // mcpp#241: each resolved dependency's directory is MCPP_DEP_<NAME>_DIR
        // (mcpp::dep_dir("<name>")): consumer -> pkgp (a build.mcpp) -> datad.
        const auto root = s::case_dir();
        s::project datad{root / "datad", {{"src/d.cpp", "int datad_touch() { return 1; }\n"},
                                          {"mcpp.toml", "[package]\nname    = \"datad\"\nversion = \"0.1.0\"\n\n[modules]\nsources = [\"src/**/*.cpp\"]\n\n[targets.datad]\nkind = \"lib\"\n"}}};
        s::project pkgp{root / "pkgp", {{"src/lib.cpp", "extern const char* dep_datad_dir();\nconst char* pkgp_datad_dir() { return dep_datad_dir(); }\n"},
                                        {"build.mcpp", R"cpp(#include <cstdio>
#include <string>
import mcpp;
int main() {
    const char* d = mcpp::dep_dir("datad");
    if (d == nullptr || d[0] == '\0') {
        std::fprintf(stderr, "build.mcpp: MCPP_DEP_DATAD_DIR not set\n");
        return 1;
    }
    std::string out = std::string(mcpp::out_dir()) + "/depdir.cpp";
    std::FILE* f = std::fopen(out.c_str(), "w");
    std::fprintf(f, "extern const char* dep_datad_dir();\n");
    std::fprintf(f, "const char* dep_datad_dir() { return \"%s\"; }\n", d);
    std::fclose(f);
    mcpp::generated("depdir.cpp");
    return 0;
}
)cpp"},
                                        {"mcpp.toml", "[package]\nname    = \"pkgp\"\nversion = \"0.1.0\"\n\n[modules]\nsources = [\"src/**/*.cpp\"]\n\n[dependencies]\ndatad = { path = \"../datad\" }\n\n[targets.pkgp]\nkind = \"lib\"\n"}}};
        s::project consumer{root / "consumer", {{"src/main.cpp", "import std;\nextern const char* pkgp_datad_dir();\nint main() {\n    std::println(\"DEPDIR={}\", pkgp_datad_dir());\n    return 0;\n}\n"},
                                                {"mcpp.toml", "[package]\nname    = \"consumer\"\nversion = \"0.1.0\"\n\n[modules]\nsources = [\"src/**/*.cpp\"]\n\n[dependencies]\npkgp = { path = \"../pkgp\" }\n\n[targets.consumer]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}}};
        expect(fatal(e2e::succeeded(consumer.mcpp({"build"})))) << "build failed";
        std::string dir;
        for (const auto& l : s::lines_with(s::text(consumer.mcpp({"run"})), "DEPDIR="))
            if (l.starts_with("DEPDIR=")) dir = l.substr(7);
        expect(fatal(!dir.empty())) << "empty dep dir (contract var missing)";
        expect(fs::is_directory(dir)) << "dep dir does not exist: " << dir;
        auto d = fs::path(dir);
        if (d.filename().empty()) d = d.parent_path();  // basename ignores a trailing slash
        expect(eq(d.filename().string(), std::string("datad"))) << "dep dir is not datad's root: " << dir;
    };

    "145 root build mcpp dep dirs"_test[s::gcc] = [] {
        // The root's build.mcpp runs after resolution and is given MCPP_DEP_<NAME>_DIR as a
        // dependency's is: root -> datad.
        const auto root = s::case_dir();
        s::project datad{root / "datad", {{"src/d.cpp", "int datad_touch() { return 1; }\n"},
                                          {"mcpp.toml", "[package]\nname    = \"datad\"\nversion = \"0.1.0\"\n\n[modules]\nsources = [\"src/**/*.cpp\"]\n\n[targets.datad]\nkind = \"lib\"\n"}}};
        s::project rootp{root / "rootp", {{"build.mcpp", R"cpp(#include <cstdio>
#include <string>
import mcpp;
int main() {
    const char* d = mcpp::dep_dir("datad");
    if (d == nullptr || d[0] == '\0') {
        std::fprintf(stderr, "build.mcpp: MCPP_DEP_DATAD_DIR not set for the ROOT\n");
        return 1;
    }
    std::string out = "src/depdir.cpp";
    std::FILE* f = std::fopen(out.c_str(), "w");
    std::fprintf(f, "const char* root_datad_dir() { return \"%s\"; }\n", d);
    std::fclose(f);
    mcpp::generated("src/depdir.cpp");
    return 0;
}
)cpp"},
                                          {"src/main.cpp", "import std;\nextern const char* root_datad_dir();\nint main() {\n    std::println(\"DEPDIR={}\", root_datad_dir());\n    return 0;\n}\n"},
                                          {"mcpp.toml", "[package]\nname    = \"rootp\"\nversion = \"0.1.0\"\n\n[modules]\nsources = [\"src/**/*.cpp\"]\n\n[dependencies]\ndatad = { path = \"../datad\" }\n\n[targets.rootp]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}}};
        expect(fatal(e2e::succeeded(rootp.mcpp({"build"})))) << "build failed";
        std::string dir;
        for (const auto& l : s::lines_with(s::text(rootp.mcpp({"run"})), "DEPDIR="))
            if (l.starts_with("DEPDIR=")) dir = l.substr(7);
        expect(fatal(!dir.empty())) << "empty dep dir (root contract var missing)";
        expect(fs::is_directory(dir)) << "dep dir does not exist: " << dir;
        auto d = fs::path(dir);
        if (d.filename().empty()) d = d.parent_path();
        expect(eq(d.filename().string(), std::string("datad"))) << "dep dir is not datad's root: " << dir;
    };

    "310 build dependencies"_test[s::gcc] = [] {
        // `[build-dependencies]`: what serves the build and what reaches the target are two
        // questions. A rule's own [dependencies] and a build dependency (with what lies behind
        // them) stay out of the consumer's binary; a package the project depends on directly
        // still reaches it, also when named in both tables. The reading is the artifact.
        const auto root = s::case_dir();
        for (std::string n : {"behindrule", "buildside", "bothsides"}) {
            s::project lib{root / n, {}};
            lib.write("mcpp.toml", library(n));
            lib.write("src/" + n + ".cppm", std::format("export module {0};\nexport int {0}_symbol() {{ return 1; }}\n", n));
        }
        s::project rules{root / "rules", {{"mcpp.toml", library("ruleswithdeps") + "\n[dependencies]\nbehindrule = { path = \"../behindrule\" }\n"},
                                          {"src/ruleswithdeps.cppm", "export module ruleswithdeps;\nimport std;\nimport mcpp;\nexport namespace rules { inline void go() { mcpp::cxxflag(\"-DRULE_RAN=1\"); } }\n"}}};
        const std::string head = "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nruleswithdeps = { path = \"../rules\", host-module = true }\nbothsides     = { path = \"../bothsides\" }\n\n[build-dependencies]\n";
        s::project app{root / "app", {{"mcpp.toml", head + "buildside = { path = \"../buildside\" }\n"},
                                      {"src/main.cpp", "#include <cstdio>\n#ifndef RULE_RAN\n#error \"the rule never ran\"\n#endif\nimport bothsides;\nint main() { std::printf(\"APP=%d\\n\", bothsides_symbol()); }\n"},
                                      {"build.mcpp", "import mcpp;\nimport ruleswithdeps;\nint main() { rules::go(); }\n"}}};
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "build failed";
        expect(eq(s::last_starting(s::text(app.mcpp({"run"})), "APP="), std::string("APP=1"))) << "the app did not run";
        fs::path bin = s::last_line(app.mcpp({"run", "--print-artifact"}).out);
        if (bin.empty() || !fs::is_regular_file(bin)) {
            auto bins = executables(app.at("target"), "app");
            expect(fatal(eq(bins.size(), std::size_t{1}))) << "expected exactly one app binary";
            bin = bins.front();
        }
        // The direct dependency first, so that a failure to link anything cannot pass the rest.
        expect(fatal(has_symbol(bin, "bothsides_symbol"))) << "a direct [dependencies] library did not reach the target";
        expect(!has_symbol(bin, "behindrule_symbol")) << "a build rule's own [dependencies] leaked into the consumer's binary";
        expect(!has_symbol(bin, "buildside_symbol")) << "a [build-dependencies] entry reached the target";

        // One package in both tables: the ordinary declaration wins.
        app.write("mcpp.toml", head + "bothsides = { path = \"../bothsides\" }\n");
        app.remove("target");
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "dual-table build failed";
        auto bins = executables(app.at("target"), "app");
        expect(fatal(eq(bins.size(), std::size_t{1}))) << "expected exactly one app binary";
        expect(has_symbol(bins.front(), "bothsides_symbol")) << "naming a library in [build-dependencies] too removed it from the target";
    };

    "311 rule build dependencies"_test[s::gcc] = [] {
        // A rule may depend on another rule in its own [build-dependencies]; the consumer may not
        // import that one unless the edge says `reexport = true` (enforced on every platform, as
        // GCC's BMIs are reachable by name whatever the flags), also on a warm cache; a cycle is
        // reported as one, naming its packages.
        const auto root = s::case_dir();
        s::project globbing{root / "globbing", {{"mcpp.toml", library("globbing")},
                                                {"src/globbing.cppm", "export module globbing;\nimport std;\nexport namespace globbing {\ninline int answer() { return 7; }\n}\n"}}};
        s::project tidy{root / "tidyrule", {{"src/tidyrule.cppm", R"cpp(export module tidyrule;
import std;
import mcpp;
import globbing;
export namespace tidyrule {
inline void go() {
    mcpp::cxxflag(std::format("-DTIDY_ANSWER={}", globbing::answer()).c_str());
}
}
)cpp"}}};
        auto outer = [&](std::string_view extra) {
            tidy.write("mcpp.toml", library("tidyrule") + std::format("\n[build-dependencies]\nglobbing = {{ path = \"../globbing\", host-module = true{} }}\n", extra));
        };
        outer("");
        s::project app{root / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\ntidyrule = { path = \"../tidyrule\", host-module = true }\n"},
                                      {"src/main.cpp", "#include <cstdio>\n#ifndef TIDY_ANSWER\n#error \"the outer rule never ran\"\n#endif\nint main() { std::printf(\"ANSWER=%d\\n\", TIDY_ANSWER); }\n"},
                                      {"build.mcpp", "import mcpp;\nimport tidyrule;\nint main() { tidyrule::go(); }\n"}}};
        // 1. The value travels through the inner rule: it was compiled and linked, not only named.
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "a rule with its own [build-dependencies] did not build";
        expect(eq(s::last_starting(s::text(app.mcpp({"run"})), "ANSWER="), std::string("ANSWER=7")));
        expect(eq(executables(app.at("target"), "app").size(), std::size_t{1})) << "expected one app binary";

        // 2. The consumer may not import what it never declared.
        app.write("build.mcpp", "import mcpp;\nimport tidyrule;\nimport globbing;\nint main() { tidyrule::go(); }\n");
        app.remove("target");
        auto b2 = app.mcpp({"build"});
        expect(b2.started && b2.status != 0) << "build.mcpp imported a module reachable only as another rule's prerequisite";
        expect(e2e::contains(s::text(b2), "globbing")) << "the diagnostic does not name the module";
        expect(e2e::contains(s::text(b2), "reexport")) << "the diagnostic does not say how to fix it";

        // 3. reexport = true makes it the consumer's to import.
        outer(", reexport = true");
        app.remove("target");
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "a re-exported host module was still refused";
        expect(eq(s::last_starting(s::text(app.mcpp({"run"})), "ANSWER="), std::string("ANSWER=7")));

        // 3b. Withdrawing it is caught with the build.mcpp cache and the fast path warm.
        outer("");
        fs::last_write_time(app.at("src/main.cpp"), fs::file_time_type::clock::now());
        auto b3b = app.mcpp({"build"});
        expect(b3b.started && b3b.status != 0) << "withdrawing reexport was replayed from the build.mcpp cache";
        expect(e2e::contains(s::text(b3b), "globbing")) << "the refusal after cache invalidation does not name the module";

        // 4. A cycle.
        globbing.write("mcpp.toml", globbing.read("mcpp.toml") + "\n[build-dependencies]\ntidyrule = { path = \"../tidyrule\", host-module = true }\n");
        app.remove("target");
        auto b4 = app.mcpp({"build"});
        expect(b4.started && b4.status != 0) << "a rule import cycle was accepted";
        expect(s::contains_nocase(s::text(b4), "cycle")) << "the cycle was not reported as a cycle";
        expect(e2e::contains(s::text(b4), "globbing") && e2e::contains(s::text(b4), "tidyrule"))
            << "the cycle diagnostic does not name the packages on it";
    };

    "334 dependency declared tool is found"_test[s::gcc][s::unix_shell] = [] {
        // A runner named by bare name (mcpp::runner) is looked up through every package of the
        // graph, so a dependency (a board package) may declare it. With the program nowhere, the
        // run is an error naming it — not a fallback to executing the artifact. The stand-in
        // payload is never on PATH: a lookup through PATH would pass for the wrong reason.
        const auto root = s::case_dir();
        s::write_executable(root / "fakepkg/bin/demo-tool", "#!/bin/sh\necho \"DEMO-TOOL ran with $*\"\n");
        if (s::on_path("demo-tool"))
            skip("a demo-tool already on PATH");
        s::project dep{root / "dep", {{"mcpp.toml", "[package]\nname    = \"toolbox\"\nversion = \"0.1.0\"\n"},
                                      {"src/t.cppm", "export module toolbox;\n"},
                                      {"build.mcpp", "import mcpp;\nimport std;\nint main() {\n    mcpp::runner(\"demo-tool\");\n    return 0;\n}\n"}}};
        s::project app{root / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\ntoolbox = { path = \"../dep\" }\n"},
                                      {"src/main.cpp", "int main() { return 0; }\n"}}};
        auto r = app.mcpp({"run"});
        const auto out = s::text(r);
        expect(r.started && r.status != 0) << "run succeeded with an unresolvable runner — it fell back to executing the artifact";
        expect(e2e::contains(out, "demo-tool")) << "the diagnostic does not name the program that was not found";
        expect(e2e::contains(out, "not found")) << "the diagnostic does not say the program was not found";
        expect(s::lacks(out, "DEMO-TOOL ran")) << "the artifact was executed anyway";
    };

    "618 dependency feature tools reach its build program"_test[s::elf][s::gcc] = [] {
        // A dependency's [feature-xlings.<f>] is installed before that dependency's build.mcpp
        // runs (it was provisioned after every build program had run). Only a registry that has
        // never seen the tool can tell, so the home is fresh, and the tool (shaderc) is one mcpp
        // does not install for its own reasons. The program writes what it found to a file.
        const auto root = s::case_dir();
        s::project dep{root / "dep", {{"src/lib.cpp", "int dep_touch() { return 1; }\n"},
                                      {"build.mcpp", R"cpp(#include <cstdio>
#include <string>
import mcpp;
int main() {
    const char* d = mcpp::xpkg_dir("xim", "shaderc");
    if (d == nullptr || d[0] == '\0') {
        std::fprintf(stderr,
            "a dependency's [feature-xlings] tool was not provisioned before "
            "its build.mcpp ran\n");
        return 2;
    }
    std::string out = std::string(mcpp::manifest_dir()) + "/tool-seen.txt";
    std::FILE* f = std::fopen(out.c_str(), "w");
    if (f == nullptr) return 3;
    std::fprintf(f, "%s\n", d);
    std::fclose(f);
    return 0;
}
)cpp"},
                                      {"mcpp.toml", "[package]\nname    = \"featuretool\"\nversion = \"0.1.0\"\n\n[modules]\nsources = [\"src/**/*.cpp\"]\n\n[features]\ndefault   = []\nusestool  = []\n\n[feature-xlings.usestool]\n\"xim:shaderc\" = \"2026.3\"\n\n[targets.featuretool]\nkind = \"lib\"\n"}}};
        s::project app{root / "app", {{"src/main.cpp", "int main() { return 0; }\n"},
                                      {"mcpp.toml", "[package]\nname    = \"consumer\"\nversion = \"0.1.0\"\n\n[dependencies]\nfeaturetool = { path = \"../dep\", features = [\"usestool\"] }\n\n[targets.consumer]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}}};
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, {.where = s::home::fresh})))) << "the consumer did not build";
        expect(fatal(dep.exists("tool-seen.txt") && !dep.read("tool-seen.txt").empty()))
            << "the dependency's build program left no record of finding its tool";
        expect(e2e::contains(dep.read("tool-seen.txt"), "xim-x-shaderc")) << "the recorded path does not name shaderc";
    };

    "622 rule declared payload reaches the consumers build program"_test[s::elf][s::gcc] = [] {
        // A rule package's [feature-xlings.<f>] is reachable from the consumer's build program,
        // where the rule's code runs (xpkg_dir read one manifest: "" while the payload sat on
        // disk). The reading is the path the rule answers with; a fresh home, for 618's reason.
        const auto root = s::case_dir();
        s::project rule{root / "rule", {{"src/rule.cppm", R"cpp(export module rule;
import std;
import mcpp;
export namespace testrule {
inline std::string tool_dir() {
    const char* d = mcpp::xpkg_dir("xim", "shaderc");
    return d == nullptr ? std::string() : std::string(d);
}
}
)cpp"},
                                        {"mcpp.toml", "[package]\nname    = \"rule\"\nversion = \"0.1.0\"\n\n[language]\nstandard   = \"c++23\"\nmodules    = true\nimport_std = true\n\n[build]\nsources = [\"src/rule.cppm\"]\n\n[features]\ndefault  = []\nusestool = []\n\n[feature-xlings.usestool]\n\"xim:shaderc\" = \"2026.3\"\n\n[targets.rule]\nkind = \"lib\"\n"}}};
        s::project app{root / "app", {{"src/main.cpp", "int main() { return 0; }\n"},
                                      {"build.mcpp", R"cpp(#include <cstdio>
#include <string>
import mcpp;
import rule;
int main() {
    std::string out = std::string(mcpp::manifest_dir()) + "/rule-saw.txt";
    std::FILE* f = std::fopen(out.c_str(), "w");
    if (f == nullptr) return 3;
    std::fprintf(f, "%s\n", testrule::tool_dir().c_str());
    std::fclose(f);
    return 0;
}
)cpp"},
                                      {"mcpp.toml", "[package]\nname    = \"consumer\"\nversion = \"0.1.0\"\n\n[language]\nstandard   = \"c++23\"\nmodules    = true\nimport_std = true\n\n[build-dependencies]\nrule = { path = \"../rule\", features = [\"usestool\"], host-module = true }\n\n[targets.consumer]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}}};
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, {.where = s::home::fresh})))) << "the consumer did not build";
        expect(fatal(app.exists("rule-saw.txt") && !app.read("rule-saw.txt").empty())) << "the rule left no record";
        const auto answer = s::last_line(app.read("rule-saw.txt"));
        expect(fatal(!answer.empty())) << "the rule got an empty answer -- its payload was installed and unreachable";
        expect(e2e::contains(answer, "xim-x-shaderc")) << "the answer does not name shaderc";
    };

    "693 the root build program reads dependency link forms"_test[s::elf] = [] {
        // #642 E2: mcpp::dep_linkage(name) answers "static" or "shared" from the resolution that
        // decides what is linked, to the root's program only. A: a shared-by-default package and a
        // generated-only one asked shared; B: nothing changed, no re-run; C: the dependency's
        // default turns static; D: the edge asks shared; E: a dependency's program reads "" (gen
        // fails the build otherwise); F: `namespace` + `name` answer both spellings.
        const auto root = s::case_dir();
        s::project leaf{root / "leaf", {{"mcpp.toml", "[package]\nname = \"leaf\"\nversion = \"0.1.0\"\n[targets.leaf]\nkind = \"lib\"\n"},
                                        {"src/leaf.cpp", "int leaf_value() { return 1; }\n"}}};
        s::project fw{root / "fw", {{"src/fw.cpp", "int fw_value() { return 40; }\n"}}};
        auto write_fw = [&](std::string_view form) {
            fw.write("mcpp.toml", std::format("[package]\nname = \"fw\"\nversion = \"0.1.0\"\n[targets.fw]\nkind = \"lib\"\nlinkage = \"{}\"\n", form));
        };
        write_fw("shared");
        s::project gen{root / "gen", {{"mcpp.toml", "[package]\nname    = \"gen\"\nversion = \"0.1.0\"\n\n[modules]\nsources = []\n\n[dependencies]\nleaf = { path = \"../leaf\" }\n\n[targets.gen]\nkind = \"lib\"\n"},
                                      {"build.mcpp", R"cpp(#include <cstdio>
#include <string>
import mcpp;
int main() {
    const char* dir  = mcpp::dep_dir("leaf");
    const char* form = mcpp::dep_linkage("leaf");
    if (dir == nullptr || dir[0] == '\0') {
        std::fprintf(stderr, "gen: dep_dir(leaf) is empty\n");
        return 1;
    }
    if (form != nullptr && form[0] != '\0') {
        std::fprintf(stderr, "gen: a dependency's program was offered dep_linkage(leaf)=%s\n", form);
        return 1;
    }
    std::string out = std::string(mcpp::out_dir()) + "/gen.cpp";
    std::FILE* f = std::fopen(out.c_str(), "w");
    std::fprintf(f, "int leaf_value();\nint gen_value() { return leaf_value() + 1; }\n");
    std::fclose(f);
    mcpp::generated("gen.cpp");
    return 0;
}
)cpp"}}};
        s::project app{root / "app", {{"build.mcpp", R"cpp(#include <cstdio>
#include <string>
import mcpp;
int main() {
    std::string line = std::string("fw=") + mcpp::dep_linkage("fw")
                     + " gen=" + mcpp::dep_linkage("gen") + "\n";
    std::string log = std::string(mcpp::manifest_dir()) + "/runs.log";
    std::FILE* f = std::fopen(log.c_str(), "a");
    std::fputs(line.c_str(), f);
    std::fclose(f);
    return 0;
}
)cpp"},
                                      {"src/main.cpp", "int fw_value();\nint gen_value();\nint main() { return fw_value() + gen_value() == 42 ? 0 : 1; }\n"}}};
        auto write_app = [&](std::string_view edge) {
            app.write("mcpp.toml", std::format("[package]\nname = \"app\"\nversion = \"0.1.0\"\n[dependencies]\nfw = {{ path = \"../fw\"{} }}\ngen = {{ path = \"../gen\", linkage = \"shared\" }}\n", edge));
        };
        write_app("");
        auto runs = [&] { return s::last_line(app.read("runs.log")); };
        auto run_count = [&] { return s::count_lines(app.read("runs.log"), "="); };
        auto lib = [&](std::string_view name) { return s::find_file(app.at("target"), name).has_value(); };
        // A.
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "A: build failed";
        expect(eq(runs(), std::string("fw=shared gen=shared"))) << "A";
        expect(lib("libfw.so")) << "A: no libfw.so";
        expect(lib("libgen.so")) << "A: no libgen.so for a package whose sources are all generated";
        std::optional<fs::path> bin;
        for (const auto& f : executables(app.at("target"), "app"))
            if (!bin && f.generic_string().contains("/bin/")) bin = f;
        expect(fatal(bin.has_value()));
        expect(e2e::succeeded(s::run(*bin))) << "A: the program did not run";
        // B.
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "B: build failed";
        expect(eq(run_count(), std::size_t{1})) << "B: an unchanged build re-ran the root program";
        // C.
        write_fw("static");
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "C: build failed";
        expect(eq(run_count(), std::size_t{2})) << "C: a changed dependency form did not re-run the root program";
        expect(eq(runs(), std::string("fw=static gen=shared"))) << "C";
        app.remove("target");
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "C: clean build failed";
        expect(!lib("libfw.so")) << "C: libfw.so was built while the program read static";
        // D.
        write_app(", linkage = \"shared\"");
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "D: build failed";
        expect(eq(runs(), std::string("fw=shared gen=shared"))) << "D";
        expect(lib("libfw.so")) << "D: no libfw.so while the program read shared";
        // F.
        s::project nsfw{root / "nsfw", {{"mcpp.toml", "[package]\nnamespace = \"ns\"\nname = \"nsfw\"\nversion = \"0.1.0\"\n[targets.nsfw]\nkind = \"lib\"\nlinkage = \"shared\"\n"},
                                        {"src/nsfw.cpp", "int nsfw_value() { return 42; }\n"}}};
        s::project nsapp{root / "nsapp", {{"mcpp.toml", "[package]\nname = \"nsapp\"\nversion = \"0.1.0\"\n[dependencies]\nns.nsfw = { path = \"../nsfw\" }\n"},
                                          {"src/main.cpp", "int nsfw_value();\nint main() { return nsfw_value() == 42 ? 0 : 1; }\n"},
                                          {"build.mcpp", R"cpp(#include <cstdio>
#include <string>
import mcpp;
int main() {
    std::string line = std::string("qualified=") + mcpp::dep_linkage("ns.nsfw")
                     + " bare=" + mcpp::dep_linkage("nsfw")
                     + " dir=" + (mcpp::dep_dir("ns.nsfw")[0] != '\0' ? "yes" : "no") + "\n";
    std::string log = std::string(mcpp::manifest_dir()) + "/runs.log";
    std::FILE* f = std::fopen(log.c_str(), "a");
    std::fputs(line.c_str(), f);
    std::fclose(f);
    return 0;
}
)cpp"}}};
        expect(fatal(e2e::succeeded(nsapp.mcpp({"build"})))) << "F: build failed";
        expect(eq(s::last_line(nsapp.read("runs.log")), std::string("qualified=shared bare=shared dir=yes"))) << "F";
    };

    "720 the root build program reads the resolved graph"_test = [] {
        // #647 E1: mcpp::graph_file() gives the root's program the resolved graph and each
        // package's [package.metadata] (a transitive library has no name to ask dep_dir for).
        // A: b before a before the root, b's directory, kind and metadata verbatim; B: nothing
        // changed, no re-run; C: a metadata edit re-runs with the new value; D: a source edit does
        // not; E: a dependency's program reads ""; F: an unknown [package] key warns, fails under
        // --strict, and `metadata` is known. (The script read the document with python3.)
        const auto root = s::case_dir();
        s::project b{root / "b", {{"src/b.cpp", "int b_answer() { return 40; }\n"}}};
        auto write_b = [&](std::string_view resources) {
            b.write("mcpp.toml", std::format("[package]\nname      = \"b\"\nnamespace = \"spike\"\nversion   = \"0.2.0\"\n\n[package.metadata.demo]\nresources = \"{}\"\nlanguages = [\"en\", \"zh\"]\n\n[targets.b]\nkind = \"lib\"\n", resources));
        };
        write_b("res");
        s::project a{root / "a", {{"mcpp.toml", "[package]\nname      = \"a\"\nnamespace = \"spike\"\nversion   = \"0.1.0\"\n\n[dependencies]\nspike.b = { path = \"../b\" }\n\n[targets.a]\nkind = \"lib\"\n"},
                                  {"src/a.cpp", "int b_answer();\nint a_answer() { return b_answer() + 1; }\n"},
                                  {"build.mcpp", R"cpp(#include <cstdio>
import mcpp;
int main() {
    const char* g = mcpp::graph_file();
    if (g != nullptr && g[0] != '\0') {
        std::fprintf(stderr, "a: a dependency's program was offered graph_file()=%s\n", g);
        return 1;
    }
    return 0;
}
)cpp"}}};
        const std::string app_manifest = "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[dependencies]\nspike.a = { path = \"../a\" }\n";
        s::project app{root / "app", {{"mcpp.toml", app_manifest},
                                      {"src/main.cpp", "int a_answer();\nint main() { return a_answer() == 41 ? 0 : 1; }\n"},
                                      {"build.mcpp", R"cpp(#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
import mcpp;
int main() {
    const char* g = mcpp::graph_file();
    if (g == nullptr || g[0] == '\0') {
        std::fprintf(stderr, "app: graph_file() is empty\n");
        return 1;
    }
    std::ifstream in(g, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string dir = mcpp::manifest_dir();
    int n = 0;
    for (;; ++n) {
        std::ifstream probe(dir + "/graph-" + std::to_string(n) + ".json");
        if (!probe) break;
    }
    std::ofstream(dir + "/graph-" + std::to_string(n) + ".json", std::ios::binary) << ss.str();
    return 0;
}
)cpp"}}};
        auto runs = [&] { return s::glob(app.dir(), "graph-*.json").size(); };
        // A, E.
        auto ra = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(ra))) << "A: build failed";
        expect(fatal(eq(runs(), std::size_t{1}))) << "A: the root program's runs";
        const auto d = s::json_file(app.at("graph-0.json"));
        std::vector<std::string> names;
        for (const auto& p : d["packages"]) names.push_back(p["package"]["canonical"].get<std::string>());
        expect(fatal(eq(names, std::vector<std::string>{"spike.b@0.2.0", "spike.a@0.1.0", "mcpplibs.app@0.1.0"})));
        const auto& gb = d["packages"][0];
        const auto& ga = d["packages"][1];
        const auto& gapp = d["packages"][2];
        expect(gapp["root"] == true && gb["root"] == false);
        const fs::path bdir = gb["manifest_dir"].get<std::string>();
        expect(bdir.is_absolute()) << bdir.string();
        expect(fs::equivalent(bdir, b.dir())) << bdir.string();
        expect(gb["targets"] == nlohmann::json::parse(R"([{"name": "b", "kind": "lib"}])")) << gb["targets"].dump();
        expect(gb["metadata"] == nlohmann::json::parse(R"({"demo": {"resources": "res", "languages": ["en", "zh"]}})")) << gb["metadata"].dump();
        expect(ga["metadata"] == nlohmann::json::object()) << ga["metadata"].dump();
        std::vector<std::string> requesters;
        for (const auto& r : gb["requested_by"]) requesters.push_back(r["requester"].get<std::string>());
        expect(eq(requesters, std::vector<std::string>{"spike.a@0.1.0"}));
        expect(gb["features"].is_array());
        // B.
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "B: build failed";
        expect(eq(runs(), std::size_t{1})) << "B: an unchanged build re-ran the root program";
        // C.
        write_b("assets");
        auto rc = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(rc))) << "C: build failed";
        expect(fatal(eq(runs(), std::size_t{2}))) << "C: editing a dependency's metadata did not re-run the root program";
        expect(eq(s::json_file(app.at("graph-1.json"))["packages"][0]["metadata"]["demo"]["resources"].get<std::string>(), std::string("assets")));
        // D.
        b.write("src/b.cpp", "int b_answer() { return 39 + 1; }\n");
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "D: build failed";
        expect(eq(runs(), std::size_t{2})) << "D: editing a dependency's source re-ran the root program";
        // F.
        app.write("mcpp.toml", std::regex_replace(app_manifest, std::regex("version = \"0.1.0\"\n"), "version = \"0.1.0\"\nlicence = \"MIT\"\n", std::regex_constants::format_first_only));
        auto rf = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(rf))) << "F: build failed on a warning";
        expect(e2e::contains(s::text(rf), "[package] has unsupported key 'licence'")) << "F: no warning for an unknown [package] key";
        auto rf2 = app.mcpp({"build", "--strict"});
        expect(rf2.started && rf2.status != 0) << "F: --strict accepted an unknown [package] key";
        expect(e2e::contains(s::text(rf2), "[package] has unsupported key 'licence'")) << "F: --strict failed for another reason";
        for (const auto* r : {&ra, &rc, &rf})
            expect(s::lacks(s::text(*r), "unsupported key 'metadata'")) << "F: metadata reported as unknown";
    };

    "779 a build program declares a runtime search dir"_test[s::pack][s::gcc] = [] {
        // mcpp::runtime_search_dir(dir), the build-program form of runtime_search_dirs (docs/04
        // §2.11, mcpp#701/#702): -Wl,-rpath in build.ninja, never -L; `mcpp run` finds the library
        // through that RUNPATH alone; a build.mcpp cache hit keeps it; pack stages it into lib/;
        // a dependency's declaration reaches its consumer's executable.
        const auto gcc = s::on_path("gcc");
        expect(fatal(gcc.has_value())) << "no gcc on PATH";
        const auto root = s::case_dir();
        auto graph = [](const s::project& p) {
            for (const auto& f : s::find_files(p.at("target"), R"(build\.ninja)"))
                if (!f.generic_string().contains("/.build-mcpp/")) return s::read_file(f);
            return std::string();
        };
        auto shared = [&](const fs::path& c, const fs::path& so) {
            expect(fatal(e2e::succeeded(s::run(*gcc, {"-shared", "-fPIC", c.string(), "-o", so.string()}))));
        };
        auto program = [](std::string_view lib, std::string_view dir) {
            return std::format("import mcpp;\n#include <string>\nint main() {{\n    const std::string root = mcpp::manifest_dir();\n    mcpp::link_lib(\"{0}\");\n    mcpp::link_search((root + \"/{1}\").c_str());\n    mcpp::runtime_search_dir((root + \"/{1}\").c_str());\n    return 0;\n}}\n", lib, dir);
        };
        s::project app{root / "app", {{"rtlib/plugin.c", "int runtime_plugin_answer(void) { return 42; }\n"},
                                      {"src/main.cpp", "extern \"C\" int runtime_plugin_answer();\nint main() { return runtime_plugin_answer() == 42 ? 0 : 1; }\n"},
                                      {"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[toolchain]\nlinux = \"gcc@16.1.0\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                      {"build.mcpp", program("runtime_plugin", "rtlib")}}};
        shared(app.at("rtlib/plugin.c"), app.at("rtlib/libruntime_plugin.so"));
        const auto rtdir = fs::canonical(app.at("rtlib")).string();
        // 1. RUNPATH, never -L.
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "build failed";
        expect(e2e::contains(graph(app), "-Wl,-rpath," + rtdir)) << "runtime-search-dir directive missing from RUNPATH intent";
        // 2. `mcpp run` finds the library through it.
        expect(e2e::succeeded(app.mcpp({"run"}))) << "run failed to find the plugin through the directive's RUNPATH";
        // 3. A cache hit keeps it (before pack, which builds under another profile).
        fs::last_write_time(app.at("src/main.cpp"), fs::file_time_type::clock::now());
        auto b2 = app.mcpp({"build", "-v"});
        expect(fatal(e2e::succeeded(b2))) << "second build failed";
        expect(s::has_line(s::text(b2), R"(^ *build\.mcpp .* cached)")) << "the second build re-ran build.mcpp; the replay path was not exercised";
        expect(e2e::contains(graph(app), "-Wl,-rpath," + rtdir)) << "the RUNPATH directive did not survive a build.mcpp cache hit";
        expect(e2e::succeeded(app.mcpp({"run"}))) << "run failed after the directive was replayed from a build.mcpp cache hit";
        // 4. pack stages it into lib/.
        expect(fatal(e2e::succeeded(app.mcpp({"pack", "--pkg-format", "dir"})))) << "pack --pkg-format dir failed";
        auto staged = s::find_file(app.at("target/dist"), "libruntime_plugin.so");
        expect(fatal(staged.has_value())) << "the runtime-search-dir closure was not staged by mcpp pack";
        expect(staged->generic_string().ends_with("/lib/libruntime_plugin.so")) << "the staged library is not under the packed bundle's lib/";
        // 5. A dependency's declaration reaches the consumer.
        s::project libdep{root / "libdep", {{"plugin/dep_plugin.c", "int dep_plugin_answer(void) { return 7; }\n"},
                                            {"src/lib.cpp", "extern \"C\" int dep_plugin_answer();\nint libdep_touch() { return dep_plugin_answer(); }\n"},
                                            {"mcpp.toml", "[package]\nname    = \"libdep\"\nversion = \"0.1.0\"\n\n[modules]\nsources = [\"src/**/*.cpp\"]\n\n[targets.libdep]\nkind = \"lib\"\n"},
                                            {"build.mcpp", program("dep_plugin", "plugin")}}};
        shared(libdep.at("plugin/dep_plugin.c"), libdep.at("plugin/libdep_plugin.so"));
        s::project consumer{root / "consumer", {{"src/main.cpp", "extern \"C\" int dep_plugin_answer();\nint main() { return dep_plugin_answer() == 7 ? 0 : 1; }\n"},
                                                {"mcpp.toml", "[package]\nname    = \"consumer\"\nversion = \"0.1.0\"\n\n[toolchain]\nlinux = \"gcc@16.1.0\"\n\n[dependencies]\nlibdep = { path = \"../libdep\" }\n\n[targets.consumer]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}}};
        expect(fatal(e2e::succeeded(consumer.mcpp({"build"})))) << "consumer build failed";
        expect(e2e::contains(graph(consumer), "-Wl,-rpath," + fs::canonical(libdep.at("plugin")).string())) << "the dependency's runtime-search-dir did not reach the consumer's link";
        expect(e2e::succeeded(consumer.mcpp({"run"}))) << "consumer run failed to find the dependency's plugin";
    };

    "873 payload overrides"_test[s::gcc][s::unix_shell] = [] {
        // mcpp#755: [xlings.overrides], MCPP_XLINGS_OVERRIDE_<NS>_<NAME> and config.toml state
        // where a declared payload comes from: it is not provisioned, the build program receives
        // the override, and the build says so on a `Using` line and in `Finished`. Every build
        // runs with MCPP_NO_AUTO_INSTALL=1 and the payload is in no index, so "built" means "not
        // asked for"; the baseline (no override, refused) is the control.
        const auto work = s::case_dir();
        const auto w = work.generic_string();
        s::project plug{work / "plug", {{"mcpp.toml", "[package]\nname      = \"plug\"\nnamespace = \"e2e\"\nversion   = \"0.1.0\"\n\n[build]\nsources = [\"src/plug.cppm\"]\n\n[features.tools-x]\nsources = [\"members/x.cppm\"]\n\n[feature-xlings.tools-x]\n\"xim:mcpp-e2e-absent-tool\" = \">=2.0\"\n\n[targets.plug]\nkind = \"lib\"\n"},
                                        {"src/plug.cppm", "export module e2e.plug;\n"},
                                        {"members/x.cppm", "export module e2e.tools.x;\nexport namespace e2e::x { inline int v = 1; }\n"}}};
        const std::string base = std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[build-dependencies.e2e]\nplug = {{ path = \"{}/plug\", features = [\"tools-x\"], host-module = true }}\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", w);
        s::project app{work / "app", {{"mcpp.toml", base},
                                      {"build.mcpp", R"cpp(import std;
import mcpp;
import e2e.tools.x;
int main() {
    std::string s = std::string("dir=") + mcpp::xpkg_dir("xim", "mcpp-e2e-absent-tool")
        + " source=" + mcpp::xpkg_source("xim", "mcpp-e2e-absent-tool")
        + " program=" + mcpp::xpkg_program("xim", "mcpp-e2e-absent-tool");
    mcpp::warning(s.c_str());
    return 0;
}
)cpp"},
                                      {"src/main.cpp", "int main() { return 0; }\n"}}};
        s::write_executable(work / "opt/bin/absent-tool", "#!/bin/sh\necho tool\n");
        auto probe = [&](std::vector<std::string> args, std::vector<std::pair<std::string, std::optional<std::string>>> extra = {}) {
            app.remove("target");
            s::options o{.env = {{"NO_COLOR", "1"}, {"MCPP_NO_AUTO_INSTALL", "1"}}};
            for (auto& e : extra) o.env.push_back(std::move(e));
            return s::text(app.mcpp(std::move(args), o));
        };
        // Control.
        auto out = probe({"build"});
        expect(e2e::contains(out, "not provisioned")) << "the baseline did not ask for the payload";
        expect(e2e::contains(out, "mcpp-e2e-absent-tool")) << "the refusal does not name the payload";
        // The environment variable.
        out = probe({"build"}, {{"MCPP_XLINGS_OVERRIDE_XIM_MCPP_E2E_ABSENT_TOOL", w + "/opt/bin/absent-tool"}});
        expect(e2e::contains(out, "Finished")) << "an env override still needed the payload";
        expect(e2e::contains(out, "Using xim:mcpp-e2e-absent-tool ← " + w + "/opt/bin/absent-tool  [custom · env MCPP_XLINGS_OVERRIDE_XIM_MCPP_E2E_ABSENT_TOOL]"))
            << "no Using line naming the env override";
        expect(e2e::contains(out, "dir=" + w + "/opt source=override program=" + w + "/opt/bin/absent-tool"))
            << "the build program did not receive the override (root, source, program)";
        expect(s::has_line(out, "Finished .* · custom: xim:mcpp-e2e-absent-tool")) << "Finished does not summarise the source";
        // The manifest, with a version that satisfies the requirement.
        auto override_with = [&](const std::string& value) {
            app.write("mcpp.toml", base + "\n[xlings.overrides]\n\"xim:mcpp-e2e-absent-tool\" = " + value + "\n");
        };
        override_with(std::format("{{ program = \"{}/opt/bin/absent-tool\", version = \"2.1\" }}", w));
        out = probe({"build"});
        expect(s::has_line(out, R"(\[custom · mcpp\.toml:[0-9]*\])")) << "no Using line naming mcpp.toml";
        // --managed-only refuses it, naming it.
        out = probe({"--managed-only", "build"});
        expect(e2e::contains(out, "managed-only") && e2e::contains(out, "mcpp-e2e-absent-tool")) << "--managed-only did not refuse the override";
        // A version below the requirement, naming both sides.
        override_with(std::format("{{ program = \"{}/opt/bin/absent-tool\", version = \"1.0\" }}", w));
        out = probe({"build"});
        expect(e2e::contains(out, ">=2.0") && e2e::contains(out, "e2e:plug")) << "a version below the requirement was accepted";
        // A path that does not exist.
        override_with(std::format("\"{}/nope\"", w));
        out = probe({"build"});
        expect(e2e::contains(out, "does not exist")) << "a missing override path was accepted";
        // A dependency may not state where a payload comes from.
        app.write("mcpp.toml", base);
        plug.write("mcpp.toml", plug.read("mcpp.toml") + "\n[xlings.overrides]\n\"xim:mcpp-e2e-absent-tool\" = \"/bin/sh\"\n");
        out = probe({"build"});
        expect(e2e::contains(out, "is a dependency of this build")) << "a dependency's override was accepted";
    };

    "874 payload on request"_test[s::gcc][s::unix_shell] = [] {
        // mcpp#755 `provision = "on-request"`: a payload installed when a build program asks with
        // xpkg_request, not before it runs. Under MCPP_NO_AUTO_INSTALL=1 the project builds while
        // the program does not ask, and is refused naming the program when it does.
        const auto work = s::case_dir();
        const auto w = work.generic_string();
        s::project plug{work / "plug", {{"mcpp.toml", "[package]\nname      = \"plug\"\nnamespace = \"e2e\"\nversion   = \"0.1.0\"\n\n[build]\nsources = [\"src/plug.cppm\"]\n\n[features.tools-x]\nsources = [\"members/x.cppm\"]\n\n[feature-xlings.tools-x]\n\"xim:mcpp-e2e-absent-tool\" = { version = \"1.0.0\", provision = \"on-request\" }\n\n[targets.plug]\nkind = \"lib\"\n"},
                                        {"src/plug.cppm", "export module e2e.plug;\n"},
                                        {"members/x.cppm", "export module e2e.tools.x;\nexport namespace e2e::x { inline int v = 1; }\n"}}};
        s::project app{work / "app", {{"mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[build-dependencies.e2e]\nplug = {{ path = \"{}/plug\", features = [\"tools-x\"], host-module = true }}\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", w)},
                                      {"build.mcpp", R"cpp(import std;
import mcpp;
import e2e.tools.x;
int main() {
    std::string s = std::string("source=") + mcpp::xpkg_source("xim", "mcpp-e2e-absent-tool");
    if (const char* ask = std::getenv("E2E_ASK"); ask && *ask) {
        if (std::string_view(ask) == "undeclared") {
            std::printf("mcpp:xpkg-request=xim:mcpp-e2e-undeclared\n");
            return 0;
        }
        mcpp::xpkg_request("xim", "mcpp-e2e-absent-tool");
        if (mcpp::xpkg_pending()) return 0;
    }
    mcpp::warning(s.c_str());
    return 0;
}
)cpp"},
                                      {"src/main.cpp", "int main() { return 0; }\n"}}};
        auto probe = [&](std::optional<std::string> ask) {
            app.remove("target");
            s::options o{.env = {{"NO_COLOR", "1"}, {"MCPP_NO_AUTO_INSTALL", "1"}}};
            if (ask) o.env.emplace_back("E2E_ASK", *ask);
            return s::text(app.mcpp({"build"}, o));
        };
        auto out = probe(std::nullopt);
        expect(e2e::contains(out, "Finished")) << "a payload nobody asked for was required";
        expect(e2e::contains(out, "source=pending")) << "the build program was not told the payload is pending";
        out = probe("1");
        expect(e2e::contains(out, "payloads requested by the build program of `app`")) << "a request did not reach the installer naming the requester";
        expect(e2e::contains(out, "mcpp-e2e-absent-tool@1.0.0")) << "the refusal does not name the payload";
        out = probe("undeclared");
        expect(e2e::contains(out, "no manifest of this build declares")) << "a request for an undeclared payload was accepted";
        auto db = app.mcpp({"emit", "build-database", "--format", "json"},
                           {.env = {{"NO_COLOR", "1"}, {"MCPP_NO_AUTO_INSTALL", "1"}, {"E2E_ASK", "1"}}});
        expect(e2e::contains(db.out, "MCPP_BUILD_DATABASE_PAYLOAD_DEFERRED")) << "planning did not defer the request";
    };

    "877 why sources"_test[s::gcc][s::unix_shell] = [] {
        // mcpp#755: `mcpp why sources|tool|payload` and `--format json`, the decision record of a
        // prepare as the build reported it. (The script read the JSON with python3.)
        const auto work = s::case_dir();
        const auto w = work.generic_string();
        s::write_executable(work / "opt/bin/tool", "#!/bin/sh\n");
        s::project app{work / "app", {{"mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[xlings.workspace]\n\"xim:mcpp-e2e-absent-tool\" = \"1.0.0\"\n\n[xlings.overrides]\n\"xim:mcpp-e2e-absent-tool\" = \"{}/opt/bin/tool\"\n", w)},
                                      {"src/main.cpp", "int main() { return 0; }\n"}}};
        const s::options o{.env = {{"NO_COLOR", "1"}, {"MCPP_NO_AUTO_INSTALL", "1"}}};
        auto payload = app.mcpp({"why", "payload", "mcpp-e2e"}, o);
        expect(fatal(e2e::succeeded(payload))) << "why payload failed";
        expect(e2e::contains(s::text(payload), "payload:xim:mcpp-e2e-absent-tool  " + w + "/opt/bin/tool")) << "the payload is not listed";
        expect(s::has_line(s::text(payload), "custom · mcpp\\.toml:[0-9]*")) << "the origin is not listed";
        auto sources = app.mcpp({"why", "sources", "--format", "json"}, o);
        expect(fatal(e2e::succeeded(sources))) << "why sources --format json failed";
        const auto env = s::json(sources.out);
        expect(eq(env.value("kind", std::string()), std::string("mcpp.why.sources")));
        std::map<std::string, nlohmann::json> subjects;
        for (const auto& x : env["data"]["sources"]) subjects[x["subject"].get<std::string>()] = x;
        expect(fatal(subjects.contains("payload:xim:mcpp-e2e-absent-tool")));
        const auto& p = subjects["payload:xim:mcpp-e2e-absent-tool"];
        expect(p["class"] == "custom" && p["origin"]["kind"] == "manifest") << p.dump();
        expect(subjects.contains("toolchain.build")) << "no toolchain.build subject";
        auto nonsense = app.mcpp({"why", "nonsense"}, {.env = {{"NO_COLOR", "1"}}});
        expect(e2e::contains(s::text(nonsense), "is not a topic of `mcpp why`")) << "an unknown topic was accepted";
    };

    "902 a build dependency library is compiled as its package"_test = [] {
        // mcpp#797 R1: a library in [build-dependencies] is usable from build.mcpp as the package
        // it is — a module wrapper over a header library, whose global module fragment includes
        // its header through the package's include_dirs. A: no `host-module`, the header resolves,
        // its defines apply; B: [lib] path outside src/, imported by its declared name; C: no
        // object of it in the target's plan.
        const s::options home{.where = s::home::inherited};
        const auto root = s::case_dir();
        s::project hl{root / "hl", {{"include/hl/hl.hpp", "#pragma once\nnamespace hl { inline int answer() { return HL_BASE + 2; } }\n"},
                                    {"modules/hl.cppm", "module;\n#include <hl/hl.hpp>\nexport module acme.hl;\nexport namespace hl { using hl::answer; }\n"},
                                    {"mcpp.toml", "[package]\nnamespace = \"acme\"\nname      = \"hl\"\nversion   = \"0.1.0\"\n\n[lib]\npath = \"modules/hl.cppm\"\n\n[build]\nsources      = [\"modules/hl.cppm\"]\ninclude_dirs = [\"include\"]\ndefines      = [\"HL_BASE=40\"]\n\n[targets.hl]\nkind = \"lib\"\n"}}};
        s::project app{root / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[build-dependencies.acme]\nhl = { path = \"../hl\" }\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                      {"src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"V=%d\\n\", V); }\n"},
                                      {"build.mcpp", "import std;\nimport mcpp;\nimport acme.hl;\nint main() { mcpp::cxxflag(std::format(\"-DV={}\", hl::answer()).c_str()); }\n"}}};
        auto b = app.mcpp({"build"}, home);
        expect(fatal(e2e::succeeded(b))) << "A: a header-backed module library in [build-dependencies] did not build";
        auto v = s::last_starting(app.mcpp({"run"}, home).out, "V=");
        if (v.ends_with('\r')) v.pop_back();
        expect(eq(v, std::string("V=42"))) << "A/B: the library's value did not reach the target";
        expect(s::lacks(s::text(b), "host module")) << "A: the old host-module path was reported";
        bool in_plan = false;
        for (const auto& o : s::find_files(app.at("target"), R"(hl\.m\.o.*)")) {
            const auto g = o.generic_string();
            in_plan = in_plan || (g.contains("/obj/") && !g.contains("/.build-mcpp/"));
        }
        expect(!in_plan) << "C: the build dependency was compiled into the target's plan";
    };

    "58 preinstall mcpp deps for hooks"_test[s::gcc][s::fresh_sandbox] = [] {
        // A package installed from an mcpp index may need its mcpp.deps before its install hook
        // runs: they are installed first (library deps belong in mcpp.deps; only hook-time tools
        // are xpm deps). A fake xlings records the order of the installs.
        const auto home = s::case_home(s::home::inherited);
        const auto seed = home / "registry/subos/default/.xlings.json";
        expect(fatal(fs::is_regular_file(seed))) << "inherited toolchain has no default SubOS runtime contract";
        const auto tmp = s::case_dir();
        auto package = [](std::string_view name, std::string_view description, std::string_view source,
                          std::string_view target, std::string_view deps) {
            return std::format(R"lua(package = {{
    spec = "1",
    namespace = "compat",
    name = "{0}",
    description = "{1}",
    licenses = {{"MIT"}},
    type = "package",
    xpm = {{
        linux = {{
            ["1.0.0"] = {{
                url = "https://example.invalid/{3}-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            }},
        }},
    }},
    mcpp = {{
        language = "c++23",
        import_std = false,
        sources = {{ "{2}" }},
        targets = {{ ["{3}"] = {{ kind = "lib" }} }},
        deps = {{{4}}},
    }},
}}
)lua", name, description, source, target, deps);
        };
        s::project index{tmp / "local-index", {}};
        index.write("pkgs/c/compat.proto.lua", package("compat.proto", "Protocol helper dependency", "src/proto.c", "proto", ""));
        index.write("pkgs/c/compat.appdep.lua", package("compat.appdep", "Package whose install hook needs mcpp deps", "src/appdep.c", "appdep",
                                                        "\n            [\"compat.proto\"] = \"1.0.0\",\n        "));
        const auto registry = tmp / "fake-registry";
        fs::create_directories(registry / "data");
        if (const auto xpkgs = fs::path(s::env("HOME").value_or("")) / ".mcpp/registry/data/xpkgs"; fs::is_directory(xpkgs))
            fs::create_directory_symlink(xpkgs, registry / "data/xpkgs");
        s::write_executable(tmp / "fake-bin/xlings", R"sh(#!/usr/bin/env bash
# The handshake mcpp asks of every xlings (docs/25 §5).
[[ "${1:-}" == "interface" && "${2:-}" == "--version" ]] && { echo '{"protocol_version":"1.13","version":"9999.1.1.1","capabilities":["install_packages"],"home_layouts":[2],"features":["index_dir","index_store","upstreams","payload_status","sync_index","payload_currency","binding_state","network_diagnostics","download_socket_budgets","read_only_queries"]}'; exit 0; }
set -e
if [[ "${1:-}" == "self" && "${2:-}" == "init" ]]; then
    mkdir -p "${XLINGS_HOME:?}/subos/default"
    cp "${FAKE_XLINGS_SUBOS_INFO:?}" \
       "$XLINGS_HOME/subos/default/.xlings.json"
    exit 0
fi
if [[ "${1:-}" == "update" || ( "${1:-}" == "interface" && "${2:-}" == "update_packages" ) ]]; then
    exit 0
fi
if [[ "${1:-}" == "interface" && "${2:-}" == "install_packages" ]]; then
    args=""
    while [[ $# -gt 0 ]]; do
        if [[ "$1" == "--args" ]]; then args="$2"; break; fi
        shift
    done
    printf '%s\n' "$args" >> "${FAKE_XLINGS_LOG:?}"
    if [[ ! -d "${XLINGS_PROJECT_DIR:?}/.xlings/data/compat/pkgs" \
       && ! -d "${XLINGS_PROJECT_DIR:?}/data/compat/pkgs" ]]; then
        echo "missing project local path index link" >&2
        find "${XLINGS_PROJECT_DIR:?}" -maxdepth 4 -type d -print >&2 2>/dev/null || true
        exit 23
    fi
    case "$args" in
        *compat:compat.proto@1.0.0*)
            install_root="${XLINGS_PROJECT_DIR:?}/.xlings/data/xpkgs/compat-x-compat.proto/1.0.0"
            mkdir -p "$install_root/src"
            cat > "$install_root/src/proto.c" <<'SRC'
int proto_value(void) {
    return 42;
}
SRC
            ;;
        *compat:compat.appdep@1.0.0*)
            proto_root="${XLINGS_PROJECT_DIR:?}/.xlings/data/xpkgs/compat-x-compat.proto/1.0.0"
            if [[ ! -d "$proto_root" ]]; then
                echo "mcpp.deps were not installed before appdep install hook" >&2
                exit 24
            fi
            install_root="${XLINGS_PROJECT_DIR:?}/.xlings/data/xpkgs/compat-x-compat.appdep/1.0.0"
            mkdir -p "$install_root/src"
            cat > "$install_root/src/appdep.c" <<'SRC'
extern int proto_value(void);
int app_value(void) {
    return proto_value();
}
SRC
            ;;
    esac
    printf '{"kind":"result","exitCode":0}\n'
    exit 0
fi
if [[ "${1:-}" == "install" ]]; then
    echo "direct install should not be used for project path indices" >&2
    exit 31
fi
exit 0
)sh");
        s::project{home, {}}.write("config.toml", std::format(R"toml([xlings]
binary = "{}"
home = "{}"

[index]
default = "mcpplibs"

[index.repos."mcpplibs"]
url = "https://github.com/mcpp-community/mcpp-index.git"

[cache]
search_ttl_seconds = 3600

[build]
default_jobs = 0

[toolchain]
default = "gcc@16.1.0"
)toml", (tmp / "fake-bin/xlings").generic_string(), registry.generic_string()));
        s::project app{tmp / "project/app", {{"src/app.cpp", "extern \"C\" int app_value(void);\nint app_smoke(void) {\n    return app_value();\n}\n"}}};
        app.write("mcpp.toml", std::format("[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[indices]\ncompat = {{ path = \"{}\" }}\n\n[dependencies.compat]\nappdep = \"1.0.0\"\n\n[targets.app]\nkind = \"lib\"\n", index.dir().generic_string()));
        const auto log = tmp / "fake-xlings.log";
        auto b = app.mcpp({"build"}, {.where = s::home::inherited,
                                      .env = {{"FAKE_XLINGS_LOG", log.string()},
                                              {"FAKE_XLINGS_DIRECT_LOG", (tmp / "fake-xlings-direct.log").string()},
                                              {"FAKE_XLINGS_SUBOS_INFO", seed.string()}}});
        expect(fatal(e2e::succeeded(b))) << "the build failed";
        const auto installs = s::read_file(log);
        expect(fatal(e2e::contains(installs, "compat:compat.proto@1.0.0"))) << "compat.proto was not installed from mcpp.deps";
        const auto proto = installs.find("compat:compat.proto@1.0.0");
        const auto appdep = installs.find("compat:compat.appdep@1.0.0");
        expect(appdep != std::string::npos && proto < appdep) << "mcpp.deps should be installed before dependent package hook";
    };
}};

}  // namespace
