// Modules across programs: a lib root that imports its own package's unit, a module name unique
// within a program (not across programs), a host module several members import compiled once
// per configuration, the engine's module compiled once per home (formerly tests/e2e/807, 847,
// 857, 858).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

std::string output_of(const fs::path& program) {
    auto out = s::run(program).out;
    std::erase(out, '\r');
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

// The program `name` below `root` (`find <root> -path "*/bin/*" -name <name> -type f | head -1`).
std::optional<fs::path> bin_of(const fs::path& root, std::string_view name) {
    auto all = s::find_files(root, s::exe(name));
    std::ranges::sort(all);
    for (const auto& f : all)
        if (f.generic_string().contains("/bin/")) return f;
    return std::nullopt;
}

std::string module_file(int value) { return std::format("export module boost;\nexport int value() {{ return {}; }}\n", value); }
constexpr std::string_view main_file = "#include <cstdio>\nimport boost;\nint main() { std::printf(\"%d\\n\", value()); }\n";

// The build programs' graph of a project (`find target/.build-mcpp/host -name build.ninja | head -1`).
std::string host_graph(const fs::path& dir) {
    auto all = s::find_files(dir / "target/.build-mcpp/host", R"(build\.ninja)");
    std::ranges::sort(all);
    expect(fatal(!all.empty())) << "no build programs' graph under " << dir.string();
    return s::read_file(all.front());
}
// The graph compiles `obj` (a pattern) with the module rule.
bool compiles(const fs::path& dir, std::string_view obj) {
    return static_cast<bool>(s::has_line(host_graph(dir), std::format(R"(^build obj/[^ ]*/{}( [|] [^:]*)? : cxx_module )", obj)));
}
// The graph stages `obj` from the build cache's `pkg/<entry>`.
bool places(const fs::path& dir, std::string_view obj, std::string_view entry) {
    return static_cast<bool>(s::has_line(host_graph(dir), std::format(R"(^build obj/[^ ]*/{} : stage_file [^ ]*/build-cache/v1/pkg/{})", obj, entry)));
}

E2E programs{"programs", [] {
    "807 a lib root imports its own package"_test[tag{"requires:gcc"}] = [] {
        // A host-module dependency whose lib root, explicit or by convention, imports a unit of
        // its own package builds, and the build program runs.
        s::needs({"gcc"});
        for (std::string_view form : {"explicit", "convention"}) {
            s::project root(mcpplibs::mtest::temp_dir() / form, {});
            std::string rules = "[package]\nnamespace = \"repro\"\nname = \"rules\"\nversion = \"0.1.0\"\n\n";
            if (form == "explicit") rules += "[lib]\npath = \"src/rules.cppm\"\n\n";
            rules += "[build]\nsources = [\"src/*.cppm\"]\n\n[targets.rules]\nkind = \"lib\"\n";
            root.write("rules/mcpp.toml", rules);
            root.write("rules/src/rules.cppm", "export module repro.rules;\nimport repro.helper;\nexport int answer() { return helper_answer(); }\n");
            root.write("rules/src/helper.cppm", "export module repro.helper;\nexport int helper_answer() { return 42; }\n");
            root.write("app/mcpp.toml", R"([package]
namespace = "repro"
name = "app"
version = "0.1.0"
[build-dependencies]
"repro.rules" = { path = "../rules", host-module = true }
[build]
sources = ["main.cpp"]
[targets.app]
kind = "bin"
main = "main.cpp"
)");
            root.write("app/build.mcpp", "import repro.rules;\nint main() { return answer() == 42 ? 0 : 1; }\n");
            root.write("app/main.cpp", "int main() { return 0; }\n");
            auto r = root.mcpp_in("app", {"build"});
            expect(fatal(e2e::succeeded(r))) << form << " lib root: a lib root that imports its own package's unit did not build\n" << s::text(r);
            expect(fatal(s::has_line(s::text(r), R"(^ *build\.mcpp .* ran [0-9])"))) << form << " lib root: the build program did not run\n" << s::text(r);
        }
    };

    "847 a module name is unique within a program"_test = [] {
        // A: an app and its artifact program each have their own `boost`; B: an edit to one
        // rebuilds its own program only; C: two workspace members each have their own; D: one
        // file in two programs builds; E: two providers in one program are refused naming it;
        // F: one file reached as two packages is refused as such; G: a plan of the root's own
        // modules writes no module map and binds no name.
        const auto tmp = mcpplibs::mtest::temp_dir();
        s::project a(tmp / "a", {{"app/src/main.cpp", main_file}, {"updater/src/main.cpp", main_file},
                                 {"updater/mcpp.toml", "[package]\nname    = \"updater\"\nversion = \"0.1.0\"\n[targets.updater]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                 {"app/mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[dependencies]\nupdater = { path = \"../updater\", artifacts = [\"updater\"] }\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}});
        a.write("app/src/boost.cppm", module_file(1));
        a.write("updater/src/boost.cppm", module_file(2));
        auto r = a.mcpp_in("app", {"build"});
        expect(fatal(e2e::succeeded(r))) << "A: the build was refused\n" << s::text(r);
        const auto app = bin_of(a.at("app/target"), "app");
        const auto upd = bin_of(a.at("app/target"), "updater");
        expect(fatal(app && upd)) << "A: a program is missing\n" << s::text(r);
        expect(fatal(output_of(*app) == "1")) << "A: the app does not print its own module's value";
        expect(fatal(output_of(*upd) == "2")) << "A: the updater does not print its own module's value";
        std::size_t nq = 0, nf = 0;
        for (const auto& f : s::find_files(a.at("app/target"), R"(boost\..*)")) {
            const auto g = f.generic_string();
            if (std::regex_search(g, std::regex{R"(\.cache/.*updater/boost\.)"})) ++nq;
            if (std::regex_search(g, std::regex{R"(\.cache/boost\.)"})) ++nf;
        }
        expect(fatal(nq == 1u && nf == 1u)) << "A: expected the updater's BMI below its directory and the app's at its name, found " << nq << " and " << nf;

        const auto marker = fs::file_time_type::clock::now();
        std::this_thread::sleep_for(std::chrono::seconds{1});
        a.write("updater/src/boost.cppm", module_file(3));
        r = a.mcpp_in("app", {"build"});
        expect(fatal(e2e::succeeded(r))) << "B: the rebuild failed\n" << s::text(r);
        expect(fatal(output_of(*upd) == "3")) << "B: the updater did not take its edited module";
        expect(fatal(output_of(*app) == "1")) << "B: the app changed its value";
        expect(fatal(fs::last_write_time(*app) <= marker)) << "B: editing the updater's module relinked the app\n" << s::text(r);

        s::project c(tmp / "c", {{"mcpp.toml", "[workspace]\nmembers = [\"one\", \"two\"]\n"}, {"one/src/main.cpp", main_file}, {"two/src/main.cpp", main_file}});
        c.write("one/src/boost.cppm", module_file(5));
        c.write("two/src/boost.cppm", module_file(6));
        for (std::string_view m : {"one", "two"})
            c.write(std::format("{}/mcpp.toml", m), std::format("[package]\nname    = \"{0}\"\nversion = \"0.1.0\"\n[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", m));
        r = c.mcpp({"build", "--workspace"});
        expect(fatal(e2e::succeeded(r))) << "C: the workspace build was refused\n" << s::text(r);
        const auto one = bin_of(c.at("target"), "one");
        const auto two = bin_of(c.at("target"), "two");
        expect(fatal(one && two)) << "C: a member's program is missing\n" << s::text(r);
        expect(fatal(output_of(*one) == "5" && output_of(*two) == "6")) << "C: a member does not print its own module's value";

        s::project d(tmp / "d", {{"gui/src/main.cpp", main_file}, {"upd/src/main.cpp", main_file},
                                 {"upd/mcpp.toml", "[package]\nname    = \"upd\"\nversion = \"0.1.0\"\n[build]\nsources = [\"../shared/boost.cppm\"]\n[targets.upd]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                 {"gui/mcpp.toml", "[package]\nname    = \"gui\"\nversion = \"0.1.0\"\n[dependencies]\nupd = { path = \"../upd\", artifacts = [\"upd\"] }\n[build]\nsources = [\"../shared/boost.cppm\"]\n[targets.gui]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}});
        d.write("shared/boost.cppm", module_file(9));
        r = d.mcpp_in("gui", {"build"});
        expect(fatal(e2e::succeeded(r))) << "D: one file in two programs was refused\n" << s::text(r);
        const auto gui = bin_of(d.at("gui/target"), "gui");
        const auto upd2 = bin_of(d.at("gui/target"), "upd");
        expect(fatal(gui && upd2 && output_of(*gui) == "9" && output_of(*upd2) == "9")) << "D: a program does not run\n" << s::text(r);

        s::project e(tmp / "e", {{"prog/src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"x\\n\"); }\n"},
                                 {"prog/mcpp.toml", "[package]\nname    = \"prog\"\nversion = \"0.1.0\"\n[dependencies]\nlib1 = { path = \"../lib1\" }\nlib2 = { path = \"../lib2\" }\n[targets.prog]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}});
        e.write("lib1/src/boost.cppm", module_file(1));
        e.write("lib2/src/boost.cppm", module_file(2));
        for (std::string_view l : {"lib1", "lib2"}) e.write(std::format("{}/mcpp.toml", l), std::format("[package]\nname    = \"{}\"\nversion = \"0.1.0\"\n", l));
        r = e.mcpp_in("prog", {"build"});
        expect(fatal(failed(r))) << "E: two providers in one program were accepted\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "module 'boost' is provided by package") && e2e::contains(s::text(r), "closure of package 'prog'")))
            << "E: the refusal does not name the program's package\n" << s::text(r);

        s::project f(tmp / "f", {{"prog/src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"x\\n\"); }\n"},
                                 {"prog/mcpp.toml", "[package]\nname    = \"prog\"\nversion = \"0.1.0\"\n[dependencies]\nlib3 = { path = \"../lib3\" }\nlib4 = { path = \"../lib4\" }\n[targets.prog]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}});
        f.write("shared/boost.cppm", module_file(1));
        for (std::string_view l : {"lib3", "lib4"})
            f.write(std::format("{}/mcpp.toml", l), std::format("[package]\nname    = \"{}\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"../shared/boost.cppm\"]\n", l));
        r = f.mcpp_in("prog", {"build"});
        expect(fatal(failed(r))) << "F: one file twice in one program was accepted\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "one file is reached as two packages"))) << "F: the refusal does not say that one file is reached as two packages\n" << s::text(r);

        r = a.mcpp_in("updater", {"build"});
        expect(fatal(e2e::succeeded(r))) << "G: a plain build failed\n" << s::text(r);
        bool modmap = false;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(a.at("updater/target"), ec); it != fs::recursive_directory_iterator(); it.increment(ec))
            modmap = modmap || (it->is_directory() && it->path().filename() == "modmap");
        expect(fatal(!modmap)) << "G: a plan of the root's own modules wrote a module map";
        auto graphs = s::find_files(a.at("updater/target"), R"(build\.ninja)");
        std::ranges::sort(graphs);
        expect(fatal(!graphs.empty())) << "G: no build.ninja";
        expect(fatal(s::no_line(s::read_file(graphs.front()), R"(module-map|fmodule-mapper|cache/[^ ]*/boost\.)"))) << "G: a plan of the root's own modules binds a module name";
    };

    "857 a host module several members import is compiled once"_test = [] {
        // A path host-module library three members' build programs import is compiled once per
        // standard: two graphs (m1 and m2 share one, m3 has its own), none per member, and none of
        // its modules enters the global cache, which holds the engine's interface.
        const s::options home{.where = s::home::inherited, .env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};
        s::project ws({{"mcpp.toml", "[workspace]\nmembers = [\"m1\", \"m2\", \"m3\"]\n"},
                       {"rules/mcpp.toml", "[package]\nname    = \"rule857\"\nversion = \"0.1.0\"\n[targets.rule857]\nkind = \"lib\"\n"},
                       {"rules/src/rule857.cppm", "export module t857.rules;\nimport mcpp;\nexport int rule_value() { return 7; }\nexport void apply(const char* define) { mcpp::define(define); }\n"}});
        for (std::string_view m : {"m1", "m2", "m3"}) {
            ws.write(std::format("{}/mcpp.toml", m), std::format("[package]\nname    = \"{0}\"\nversion = \"0.1.0\"\n{1}\n[build-dependencies]\nrule857 = {{ path = \"../rules\", host-module = true }}\n\n[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n",
                                                                 m, m == "m3" ? "standard = \"c++20\"\n" : ""));
            ws.write(std::format("{}/src/main.cpp", m), "int main() { return 0; }\n");
            ws.write(std::format("{}/build.mcpp", m), std::format("import mcpp;\nimport t857.rules;\nint main() {{\n    if (rule_value() != 7) return 1;\n    apply(\"T857_{}=1\");\n    return 0;\n}}\n", m));
        }
        auto verbose = home;
        verbose.env.emplace_back("MCPP_VERBOSE", "1");
        auto r = ws.mcpp({"build", "--workspace", "--cache", "global", "-j", "4"}, verbose);
        expect(fatal(e2e::succeeded(r))) << "the workspace did not build\n" << s::text(r);
        for (std::string_view m : {"m1", "m2", "m3"})
            expect(fatal(s::has_line(s::text(r), std::format(R"(^ *build\.mcpp {} .* ran )", m)))) << "the program of " << m << " did not run\n" << s::text(r);
        const auto host = ws.at("target/.build-mcpp/host");
        std::string databases;
        for (const auto& f : s::glob(host, "*/compile_commands.json")) databases += s::read_file(f);
        const auto entries = s::count_matching(databases, R"re("file": ".*rules[/\\]+src[/\\]+rule857\.cppm")re");
        const auto programs = s::glob(host, "*/programs.json");
        expect(fatal(programs.size() == 2u)) << "A/B: " << programs.size() << " build programs' graphs for three programs of two standards (want 2)\n" << s::text(r);
        expect(fatal(entries == 2u)) << "A/B: the library was compiled " << entries << " times for three programs of two standards (want 2)\n" << s::text(r);
        for (const auto& g : programs) {
            const auto text = s::read_file(g);
            if (text.contains("\"m3-"))
                expect(fatal(!text.contains("\"m1-"))) << "B: m3's program shares a graph with m1's\n" << text;
            else
                expect(fatal(text.contains("\"m1-") && text.contains("\"m2-"))) << "A: m1 and m2 are not in one graph\n" << text;
        }
        std::set<std::string> stds;
        static const std::regex std_flag{"-std=c\\+\\+2[03]"};
        for (std::sregex_iterator it(databases.begin(), databases.end(), std_flag), end; it != end; ++it) stds.insert(it->str());
        expect(fatal(stds.size() == 2u)) << "B: the two graphs compile at " << stds.size() << " distinct standards (want 2)";
        for (std::string_view m : {"m1", "m2", "m3"})
            expect(fatal(!fs::is_directory(ws.at(std::format("{}/target/.build-mcpp/host", m))))) << "C: " << m << " keeps its own build programs' graph beside the workspace's";
        const auto cache_dir = ws.mcpp({"cache", "dir"}, home).out;
        const fs::path cache_root = cache_dir.substr(0, cache_dir.find('\n'));
        bool leaked = false;
        for (const auto& e : s::find_files(cache_root / "pkg", R"(entry\.json)")) leaked = leaked || s::read_file(e).contains("t857.rules");
        expect(fatal(!leaked)) << "D: a module of a path dependency was written below the global cache root";
        expect(fatal(!s::find_files(cache_root / "pkg/_engine", R"(entry\.json)").empty())) << "D: the engine interface is not in the global cache\n" << s::text(r);
    };

    "858 the engines module is compiled once per home"_test = [] {
        // A: the engine's interface is compiled by the first project of a home and placed from the
        // cache by the second; B: one entry, complete, recording its sources and no project; C: gc
        // collects it and the next build compiles it again; D: --cache local writes no entry; E: an
        // index package's library a build program imports is cached at its package address.
        const auto tmp = mcpplibs::mtest::temp_dir();
        const auto project = [&](const fs::path& dir, std::string_view name) {
            return s::project(dir, {{"mcpp.toml", std::format("[package]\nname    = \"{0}\"\nversion = \"0.1.0\"\n[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", name)},
                                    {"src/main.cpp", "int main() { return 0; }\n"},
                                    {"build.mcpp", std::format("import mcpp;\nint main() {{ mcpp::define(\"T858_{}=1\"); return 0; }}\n", name)}});
        };
        const auto alpha = project(tmp / "alpha", "alpha858");
        const auto beta = project(tmp / "beta", "beta858");
        const s::options home{.where = s::home::inherited, .env = {{"MCPP_VERBOSE", "1"}}};
        const s::options quiet_home{.where = s::home::inherited};
        // The commands that name no project run outside the repository, as the script's did.
        const auto nowhere = s::case_home(s::home::inherited);
        auto r = alpha.mcpp({"build"}, home);
        expect(fatal(e2e::succeeded(r))) << "the first project did not build\n" << s::text(r);
        expect(fatal(compiles(alpha.dir(), R"(mcpp\.m\.o)"))) << "A: the first project did not compile the engine interface\n" << s::text(r);
        r = beta.mcpp({"build"}, home);
        expect(fatal(e2e::succeeded(r))) << "the second project did not build\n" << s::text(r);
        expect(fatal(places(beta.dir(), R"(mcpp\.m\.o)", "_engine/mcpp-build-module@"))) << "A: the second project did not place the cached entry\n" << s::text(r) << host_graph(beta.dir());
        expect(fatal(!compiles(beta.dir(), R"(mcpp\.m\.o)"))) << "A: the second project compiled the engine interface again";
        const auto list = s::text(s::mcpp({"cache", "list"}, nowhere, quiet_home));
        expect(fatal(s::count_lines(list, "_engine/mcpp-build-module@") == 1u)) << "B: expected one entry of the bundled module in the cache\n" << list;
        r = s::mcpp({"cache", "verify"}, nowhere, quiet_home);
        expect(fatal(e2e::succeeded(r))) << "B: mcpp cache verify failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "all complete"))) << "B: the entry is not complete\n" << s::text(r);
        const auto info = s::text(s::mcpp({"cache", "info", "_engine/mcpp-build-module"}, nowhere, quiet_home));
        expect(fatal(e2e::contains(info, "mcpp.cppm"))) << "B: the entry does not record its sources\n" << info;
        expect(fatal(s::lacks(info, alpha.dir().generic_string()) && s::lacks(info, beta.dir().generic_string())))
            << "B: the recorded inputs name a project, so the entry could not serve another\n" << info;
        std::this_thread::sleep_for(std::chrono::seconds{2});
        r = s::mcpp({"cache", "gc", "--older-than", "1s"}, nowhere, quiet_home);
        expect(fatal(e2e::succeeded(r))) << "C: mcpp cache gc failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Collected 1 entries"))) << "C: gc did not collect the entry of the bundled module\n" << s::text(r);
        fs::last_write_time(alpha.at("build.mcpp"), fs::file_time_type::clock::now());
        alpha.write("build.mcpp", alpha.read("build.mcpp") + "// edited\n");
        r = alpha.mcpp({"build"}, home);
        expect(fatal(e2e::succeeded(r))) << "C: the build after gc failed\n" << s::text(r);
        expect(fatal(compiles(alpha.dir(), R"(mcpp\.m\.o)"))) << "C: the interface was not compiled again after gc collected it\n" << s::text(r);

        const auto local_home = s::another_home(s::home::inherited);
        const s::options local{.env = {{"MCPP_HOME", local_home.string()}, {"MCPP_VERBOSE", "1"}}};
        const auto gamma = project(tmp / "gamma", "gamma858");
        r = gamma.mcpp({"build", "--cache", "local"}, local);
        expect(fatal(e2e::succeeded(r))) << "D: the --cache local build failed\n" << s::text(r);
        expect(fatal(s::find_files(local_home / "build-cache/v1/pkg", R"(entry\.json)").empty())) << "D: --cache local wrote an entry below the global cache's package tree\n" << s::text(r);
        expect(fatal(compiles(gamma.dir(), R"(mcpp\.m\.o)"))) << "D: the interface was not compiled in the project's build programs' graph\n" << s::text(r);

        const auto index_home = s::another_home(s::home::inherited);
        const s::options indexed_home{.env = {{"MCPP_HOME", index_home.string()}, {"MCPP_VERBOSE", "1"}}};
        s::project idx(tmp / "local-index", {{"pkgs/r/rule-dep.lua", R"(package = {
    spec = "1",
    name = "rule-dep",
    description = "A library a build program imports",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = { ["1.0.0"] = {
            url = "https://example.invalid/rule-dep-1.0.0.tar.gz",
            sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
        macosx = { ["1.0.0"] = {
            url = "https://example.invalid/rule-dep-1.0.0.tar.gz",
            sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
        windows = { ["1.0.0"] = {
            url = "https://example.invalid/rule-dep-1.0.0.tar.gz",
            sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
    },
}
)"}});
        const auto indexed = [&](const fs::path& dir, std::string_view name) {
            const std::string payload = ".mcpp/.xlings/data/xpkgs/local-dev.rule-dep/1.0.0";
            s::project p(dir, {{"src/main.cpp", "int main() { return 0; }\n"},
                               {"build.mcpp", "import mcpp;\nimport t858.rule;\nint main() { return rule_v() == 5 ? 0 : 1; }\n"}});
            p.write(payload + "/mcpp.toml", "[package]\nname    = \"rule-dep\"\nversion = \"1.0.0\"\n[targets.rule-dep]\nkind = \"lib\"\n");
            p.write(payload + "/src/rule-dep.cppm", "export module t858.rule;\nimport mcpp;\nexport int rule_v() { return 5; }\n");
            p.write("mcpp.toml", std::format("[package]\nname    = \"{0}\"\nversion = \"0.1.0\"\n[indices]\nlocal-dev = {{ path = \"{1}\" }}\n[build-dependencies]\n\"local-dev.rule-dep\" = {{ version = \"1.0.0\", host-module = true }}\n[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n",
                                             name, idx.dir().generic_string()));
            return p;
        };
        const auto delta = indexed(tmp / "delta", "delta858");
        const auto epsilon = indexed(tmp / "epsilon", "epsilon858");
        r = delta.mcpp({"build"}, indexed_home);
        expect(fatal(e2e::succeeded(r))) << "E: the first project that imports the index package's library did not build\n" << s::text(r);
        expect(fatal(compiles(delta.dir(), R"(rule-dep\.m\.o)"))) << "E: the first project did not compile the library\n" << s::text(r);
        std::optional<fs::path> entry;
        auto entries = s::find_files(index_home / "build-cache/v1/pkg", R"(entry\.json)");
        std::ranges::sort(entries);
        for (const auto& e : entries)
            if (!entry && e.generic_string().contains("local-dev.rule-dep@1.0.0")) entry = e;
        expect(fatal(entry.has_value())) << "E: the library of an index package is not in the global cache\n" << s::text(r);
        expect(fatal(std::regex_search(entry->generic_string(), std::regex{R"(/pkg/local-dev/local-dev\.rule-dep@1\.0\.0/[^/]*/entry\.json$)"})))
            << "E: the entry is not at its package address: " << entry->string();
        r = epsilon.mcpp({"build"}, indexed_home);
        expect(fatal(e2e::succeeded(r))) << "E: the second project did not build\n" << s::text(r);
        expect(fatal(places(epsilon.dir(), R"(rule-dep\.m\.o)", "local-dev/local-dev.rule-dep@1.0.0"))) << "E: the second project did not place the cached library\n" << s::text(r);
    };
}};

}  // namespace
