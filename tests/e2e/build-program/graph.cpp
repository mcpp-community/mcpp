// The build programs' graph (mcpp#797): a library a build program uses is a package of that graph,
// with instances, features and links of its own (formerly tests/e2e/903, 904, 905).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

const s::options inherited{.where = s::home::inherited};

// The last line of `mcpp run` (stdout) in `p` that starts with `prefix`.
std::string run_line(const s::project& p, std::string_view prefix, std::vector<std::string> args = {"run"}) {
    auto line = s::last_starting(p.mcpp(std::move(args), inherited).out, prefix);
    if (line.ends_with('\r')) line.pop_back();
    return line;
}

E2E graph{"graph", [] {
    "903 one package in both tables has two instances"_test[tag{"requires:elf"}] = [] {
        // mcpp#797 R2, R3: one package in [dependencies] and [build-dependencies] has two
        // instances, each correct (they merged into one edge read as "build time only"). M1 both
        // import it; M2 `host-module = true` beside it changes nothing; M3 a feature of the build
        // dependency reaches only the program's instance; M5 a workspace; M6 the target reaches it
        // through another library; M7 a dependency's own program uses it; M9 the build database
        // has a target set and a `:host` set, compile_commands the target units only; M10 a header
        // edit recompiles both and reruns the program. (The script read the database with python3.)
        s::needs({"elf"});
        const auto root = s::case_dir();
        const std::string header = "#pragma once\nnamespace hl {\ninline int answer() { return 42; }\ninline int x_on() {\n#ifdef HL_X\n    return 1;\n#else\n    return 0;\n#endif\n}\n}\n";
        s::project hl{root / "hl", {{"include/hl/hl.hpp", header},
                                    {"src/hl.cppm", "module;\n#include <hl/hl.hpp>\nexport module hl;\nexport namespace hl { using hl::answer; using hl::x_on; }\n"},
                                    {"mcpp.toml", "[package]\nname    = \"hl\"\nversion = \"0.1.0\"\n\n[build]\nsources      = [\"src/hl.cppm\"]\ninclude_dirs = [\"include\"]\n\n[features]\nx = { defines = [\"HL_X\"] }\n\n[targets.hl]\nkind = \"lib\"\n"}}};
        const std::string program = "import std;\nimport mcpp;\nimport hl;\nint main() {\n    mcpp::cxxflag(std::format(\"-DV={}\", hl::answer()).c_str());\n    mcpp::cxxflag(std::format(\"-DX_IN_PROGRAM={}\", hl::x_on()).c_str());\n}\n";
        const std::string target_main = "#include <cstdio>\nimport hl;\nint main() { std::printf(\"V=%d target=%d xprog=%d xtarget=%d\\n\", V, hl::answer(), X_IN_PROGRAM, hl::x_on()); }\n";
        auto app = [&](const fs::path& dir, std::string_view extra) {
            const auto name = dir.filename().string();
            return s::project{dir, {{"mcpp.toml", std::format("[package]\nname = \"{0}\"\nversion = \"0.1.0\"\n{1}\n[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", name, extra)},
                                    {"src/main.cpp", target_main},
                                    {"build.mcpp", program}}};
        };
        // M1, M3.
        const std::string m1_tables = "[dependencies]\nhl = { path = \"../hl\" }\n[build-dependencies]\nhl = { path = \"../hl\", features = [\"x\"] }";
        auto m1 = app(root / "m1", m1_tables);
        expect(fatal(e2e::succeeded(m1.mcpp({"build"}, inherited)))) << "M1: one package in both tables did not build";
        expect(eq(run_line(m1, "V="), std::string("V=42 target=42 xprog=1 xtarget=0"))) << "M1/M3";
        // M2.
        m1.write("mcpp.toml", std::regex_replace(m1.read("mcpp.toml"), std::regex(R"(features = \["x"\] \})"), R"(features = ["x"], host-module = true })"));
        auto b2 = m1.mcpp({"build"}, inherited);
        expect(fatal(e2e::succeeded(b2))) << "M2: host-module = true beside the entry broke the build";
        expect(s::lacks(s::text(b2), "deprecated")) << "M2: stating the default was reported";
        expect(eq(run_line(m1, "V="), std::string("V=42 target=42 xprog=1 xtarget=0"))) << "M2";
        // M9.
        auto db = m1.mcpp({"emit", "build-database", "--format", "json"}, inherited);
        expect(fatal(e2e::succeeded(db))) << "M9: emit build-database failed";
        const auto doc = s::json(db.out)["data"]["database"];
        std::map<std::string, nlohmann::json> sets;
        for (const auto& x : doc["sets"]) sets[x["name"].get<std::string>()] = x;
        expect(sets.contains("hl") || std::ranges::any_of(sets, [](const auto& kv) { return kv.first.ends_with(".hl"); })) << "M9: no target set for hl";
        auto host = std::ranges::find_if(sets, [](const auto& kv) { return kv.first.ends_with("hl:host"); });
        expect(fatal(host != sets.end())) << "M9: no :host set for hl";
        expect(host->second["ide"]["configuration"] == "build-program") << host->second.dump();
        expect(std::ranges::any_of(sets, [](const auto& kv) { return kv.first.ends_with(":build"); })) << "M9: no :build set";
        auto cc = m1.mcpp({"emit", "build-database", "--spec", "compile-commands"}, inherited);
        expect(s::lacks(cc.out, "build.mcpp")) << "M9: compile_commands describes build.mcpp";
        // M10.
        std::this_thread::sleep_for(std::chrono::seconds(1));
        hl.write("include/hl/hl.hpp", std::regex_replace(header, std::regex("return 42;"), "return 43;"));
        expect(fatal(e2e::succeeded(m1.mcpp({"build"}, inherited)))) << "M10: the build after a header edit failed";
        expect(eq(run_line(m1, "V="), std::string("V=43 target=43 xprog=1 xtarget=0"))) << "M10: an instance kept the old header";
        hl.write("include/hl/hl.hpp", header);
        // M5.
        s::project ws{root / "ws", {{"mcpp.toml", "[workspace]\nmembers = [\"a\", \"b\"]\n"}}};
        auto a = app(root / "ws/a", "[dependencies]\nhl = { path = \"../../hl\" }");
        a.write("src/main.cpp", "#include <cstdio>\nimport hl;\nint main() { std::printf(\"V=%d\\n\", hl::answer()); }\n");
        a.remove("build.mcpp");
        auto b = app(root / "ws/b", "[build-dependencies]\nhl = { path = \"../../hl\" }");
        b.write("src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"V=%d\\n\", V); }\n");
        expect(fatal(e2e::succeeded(ws.mcpp({"build", "--workspace"}, inherited)))) << "M5: the workspace did not build";
        expect(eq(run_line(ws, "V=", {"run", "-p", "a"}), std::string("V=42"))) << "M5: member a";
        expect(eq(run_line(ws, "V=", {"run", "-p", "b"}), std::string("V=42"))) << "M5: member b";
        // M6, M7.
        s::project mid{root / "mid", {{"mcpp.toml", "[package]\nname = \"mid\"\nversion = \"0.1.0\"\n[dependencies]\nhl = { path = \"../hl\" }\n[build-dependencies]\nhl = { path = \"../hl\" }\n[targets.mid]\nkind = \"lib\"\n"},
                                      {"src/mid.cppm", "export module mid;\nimport hl;\nexport int via_mid() { return hl::answer() + MID; }\n"},
                                      {"build.mcpp", "import std;\nimport mcpp;\nimport hl;\nint main() { mcpp::cxxflag(std::format(\"-DMID={}\", hl::answer() - 42).c_str()); }\n"}}};
        auto m6 = app(root / "m6", "[dependencies]\nmid = { path = \"../mid\" }\n[build-dependencies]\nhl = { path = \"../hl\" }");
        m6.write("src/main.cpp", "#include <cstdio>\nimport mid;\nint main() { std::printf(\"V=%d mid=%d\\n\", V, via_mid()); }\n");
        expect(fatal(e2e::succeeded(m6.mcpp({"build"}, inherited)))) << "M6/M7: a library reached through another one, used by two programs, did not build";
        expect(eq(run_line(m6, "V="), std::string("V=42 mid=42"))) << "M6/M7";
    };

    "904 a build dependency feature stays out of the target"_test[tag{"requires:elf"}] = [] {
        // mcpp#797 R3: a feature one package requests of a library in its [build-dependencies] is
        // a request of the program's instance: the target instance does not have it (A), the
        // program's does (B).
        s::needs({"elf"});
        const auto root = s::case_dir();
        s::project hl{root / "hl", {{"src/hl.cppm", "export module hl;\nexport namespace hl {\ninline int x_on() {\n#ifdef HL_X\n    return 1;\n#else\n    return 0;\n#endif\n}\n}\n"},
                                    {"mcpp.toml", "[package]\nname = \"hl\"\nversion = \"0.1.0\"\n[features]\nx = { defines = [\"HL_X\"] }\n[targets.hl]\nkind = \"lib\"\n"}}};
        s::project mid{root / "mid", {{"mcpp.toml", "[package]\nname = \"mid\"\nversion = \"0.1.0\"\n[build-dependencies]\nhl = { path = \"../hl\", features = [\"x\"] }\n[targets.mid]\nkind = \"lib\"\n"},
                                      {"src/mid.cppm", "export module mid;\nexport int mid_x() { return MID_X; }\n"},
                                      {"build.mcpp", "import std;\nimport mcpp;\nimport hl;\nint main() { mcpp::cxxflag(std::format(\"-DMID_X={}\", hl::x_on()).c_str()); }\n"}}};
        s::project app{root / "app", {{"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n[dependencies]\nhl = { path = \"../hl\" }\nmid = { path = \"../mid\" }\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                      {"src/main.cpp", "#include <cstdio>\nimport hl;\nimport mid;\nint main() { std::printf(\"target=%d program=%d\\n\", hl::x_on(), mid_x()); }\n"}}};
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, inherited)))) << "the build failed";
        expect(eq(run_line(app, "target="), std::string("target=0 program=1"))) << "A/B";
    };

    "905 a build program links its libraries whole"_test[tag{"requires:elf"}] = [] {
        // mcpp#797 Q4/Q5: a library a build program uses is compiled and linked whole: its
        // implementation units, its C sources and its own dependencies.
        s::needs({"elf"});
        const auto root = s::case_dir();
        s::project base{root / "base", {{"src/base.cppm", "export module acme.base;\nexport int base_two();\n"},
                                        {"src/base_impl.cpp", "module acme.base;\nint base_two() { return 2; }\n"},
                                        {"mcpp.toml", "[package]\nname = \"base\"\nversion = \"0.1.0\"\n[targets.base]\nkind = \"lib\"\n"}}};
        s::project lib{root / "lib", {{"src/lib.cppm", "export module lib;\nexport int lib_value();\n"},
                                      {"src/lib_impl.cpp", "module lib;\nimport acme.base;\nextern \"C\" int c_forty();\nint lib_value() { return c_forty() + base_two(); }\n"},
                                      {"src/forty.c", "int c_forty(void) { return 40; }\n"},
                                      {"mcpp.toml", "[package]\nname = \"lib\"\nversion = \"0.1.0\"\n[dependencies]\nbase = { path = \"../base\" }\n[targets.lib]\nkind = \"lib\"\n"}}};
        s::project app{root / "app", {{"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n[build-dependencies]\nlib = { path = \"../lib\" }\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                      {"src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"V=%d\\n\", V); }\n"},
                                      {"build.mcpp", "import std;\nimport mcpp;\nimport lib;\nint main() { mcpp::cxxflag(std::format(\"-DV={}\", lib_value()).c_str()); }\n"}}};
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, inherited)))) << "a library with implementation units, C sources and a dependency did not link into the build program";
        expect(eq(run_line(app, "V="), std::string("V=42"))) << "the program computed another value";
    };

    "906 a build program is compiled when its graph changes"_test[tag{"requires:elf"}] = [] {
        // mcpp#797 R6: a build program is compiled again exactly when its identity moves, and a
        // build whose programs stand does not plan their graph. A a header edit reruns it with the
        // new value; B nothing changed, the identity current, no graph planned; C both stated in
        // the verbose log.
        s::needs({"elf"});
        const auto root = s::case_dir();
        s::project hl{root / "hl", {{"src/value.h", "#define HL_VALUE 42\n"},
                                    {"src/hl.cppm", "module;\n#include \"value.h\"\nexport module hl;\nexport int hl_value() { return HL_VALUE; }\n"},
                                    {"mcpp.toml", "[package]\nname = \"hl\"\nversion = \"0.1.0\"\n[targets.hl]\nkind = \"lib\"\n"}}};
        s::project app{root / "app", {{"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n[build-dependencies]\nhl = { path = \"../hl\" }\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                      {"src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"V=%d\\n\", V); }\n"},
                                      {"build.mcpp", "import std;\nimport mcpp;\nimport hl;\nint main() { mcpp::cxxflag(std::format(\"-DV={}\", hl_value()).c_str()); }\n"}}};
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, inherited)))) << "the first build failed";
        expect(eq(run_line(app, "V="), std::string("V=42"))) << "first value";
        // B, C: a swept source touched so that planning runs.
        std::this_thread::sleep_for(std::chrono::seconds(1));
        fs::last_write_time(app.at("src/main.cpp"), fs::file_time_type::clock::now());
        auto b2 = app.mcpp({"build", "--verbose"}, inherited);
        expect(fatal(e2e::succeeded(b2))) << "B: the second build failed";
        expect(s::has_line(s::text(b2), "build-program: app: identity [0-9a-f]+ current")) << "C: the log does not state the identity stood";
        expect(s::lacks(s::text(b2), "host-graph: build programs in")) << "B: the build programs' graph was planned although nothing changed";
        // A.
        std::this_thread::sleep_for(std::chrono::seconds(1));
        hl.write("src/value.h", "#define HL_VALUE 43\n");
        auto b3 = app.mcpp({"build", "--verbose"}, inherited);
        expect(fatal(e2e::succeeded(b3))) << "A: the build after the header edit failed";
        expect(s::has_line(s::text(b3), "build-program: app: identity [0-9a-f]+ -> [0-9a-f]+, compiled")) << "C: the log does not state why the program was compiled";
        expect(eq(run_line(app, "V="), std::string("V=43"))) << "A: the program ran on the old header";
    };

    "908 what a build dependency asks for"_test[tag{"requires:elf"}] = [] {
        // SPEC-004 §12 item 5: a [build-dependencies] entry with neither `tools` nor `host-module`
        // gives the library (A); `tools` alone gives the program and not the library, refused by
        // name before the compiler (B); both keys give both (C); `host-module = false` declines
        // the library (D).
        s::needs({"elf"});
        const auto root = s::case_dir();
        s::project gen{root / "gen", {{"src/gen.cppm", "export module acme.gen;\nexport int gen_value() { return 42; }\n"},
                                      {"tools/gen_main.cpp", "#include <cstdio>\nint main() { std::puts(\"generated\"); }\n"},
                                      {"mcpp.toml", "[package]\nnamespace = \"acme\"\nname      = \"gen\"\nversion   = \"0.1.0\"\n\n[build]\nsources = [\"src/gen.cppm\"]\n\n[targets.gen]\nkind = \"lib\"\n\n[targets.gentool]\nkind = \"bin\"\nmain = \"tools/gen_main.cpp\"\n"}}};
        s::project app{root / "app", {{"src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"V=%d\\n\", V); }\n"},
                                      {"build.mcpp", "import std;\nimport mcpp;\nimport acme.gen;\nint main() { mcpp::cxxflag(std::format(\"-DV={}\", gen_value()).c_str()); }\n"}}};
        auto manifest = [&](std::string_view extra) {
            app.write("mcpp.toml", std::format("[package]\nname = \"app\"\nversion = \"0.1.0\"\n[build-dependencies.acme]\ngen = {{ path = \"../gen\"{} }}\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", extra));
        };
        manifest("");
        expect(e2e::succeeded(app.mcpp({"build"}, inherited))) << "A: the default entry did not give the library";
        manifest(", tools = [\"gentool\"]");
        auto b = app.mcpp({"build"}, inherited);
        expect(b.started && b.status != 0) << "B: a tools-only entry gave the library";
        expect(e2e::contains(s::text(b), "imports 'acme.gen'")) << "B: the refusal does not name the module";
        expect(e2e::contains(s::text(b), "host-module = true")) << "B: the refusal does not name the key";
        expect(!s::contains_nocase(s::text(b), "failed to read compiled module")) << "B: the compiler was reached";
        manifest(", tools = [\"gentool\"], host-module = true");
        expect(e2e::succeeded(app.mcpp({"build"}, inherited))) << "C: tools and host-module = true did not give both";
        manifest(", host-module = false");
        auto d = app.mcpp({"build"}, inherited);
        expect(d.started && d.status != 0) << "D: host-module = false still gave the library";
        expect(e2e::contains(s::text(d), "imports 'acme.gen'")) << "D: the refusal does not name the module";
    };

    "910 a build plugin is a package of the build program"_test[tag{"requires:elf"}] = [] {
        // SPEC-007 §10: a unit importing `mcpp.core` (or `mcpp`) is a build-plugin unit. A a plugin
        // on a plugin and a library serves a consumer's program; B it builds as the root of a
        // build in the build-program configuration, its tests run, the engine package's entry
        // holds `mcpp.core`; C --target for another machine is refused; D a plugin reached through
        // [dependencies] of a target is refused naming the chain; E a program importing mcpp is
        // refused; F pack refuses a plugin; G a library the plugin depends on is importable by its
        // consumers' programs only when handed on with `reexport = true`.
        s::needs({"elf"});
        const auto root = s::case_dir();
        auto lib = [&](std::string_view name, std::string_view deps) {
            return s::project{root / name, {{"mcpp.toml", std::format("[package]\nnamespace = \"acme\"\nname = \"{0}\"\nversion = \"0.1.0\"\n[dependencies.acme]\n{1}\n[targets.{0}]\nkind = \"lib\"\n", name, deps)}}};
        };
        lib("util", "").write("src/util.cppm", "export module acme.util;\nexport int util_base() { return 40; }\n");
        lib("core-rule", "").write("src/core-rule.cppm", "export module acme.core_rule;\nimport std;\nimport mcpp;\nexport namespace acme::core_rule {\ninline void define(std::string_view key, int value) { mcpp::define(std::format(\"{}={}\", key, value).c_str()); }\n}\n");
        auto rule = lib("rule", "core-rule = { path = \"../core-rule\" }\nutil = { path = \"../util\" }");
        rule.write("src/rule.cppm", "export module acme.rule;\nimport mcpp;\nimport acme.core_rule;\nimport acme.util;\nexport namespace acme::rule {\ninline int value() { return util_base() + 2; }\ninline void go() { acme::core_rule::define(\"V\", value()); }\n}\n");
        // A.
        s::project app{root / "app", {{"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n[build-dependencies.acme]\nrule = { path = \"../rule\" }\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                      {"src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"V=%d\\n\", V); }\n"},
                                      {"build.mcpp", "import acme.rule;\nint main() { acme::rule::go(); }\n"}}};
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, inherited)))) << "A: a plugin built on a plugin and a library did not serve the build program";
        expect(eq(run_line(app, "V="), std::string("V=42"))) << "A: value";
        bool reached = false;
        for (const auto& o : s::find_files(app.at("target"), R"(.*\.m\.o)")) {
            const auto g = o.generic_string();
            reached = reached || (g.contains("/obj/") && !g.contains("/.build-mcpp/") && std::regex_search(g, std::regex("rule|util")));
        }
        expect(!reached) << "A: a package of the build program reached the target";
        // B.
        rule.write("tests/value.cpp", "import mcpp;\nimport acme.rule;\nint main() { return acme::rule::value() == 42 ? 0 : 1; }\n");
        auto b1 = rule.mcpp({"build"}, inherited);
        expect(fatal(e2e::succeeded(b1))) << "B: the plugin did not build as the root of a build";
        expect(e2e::contains(s::text(b1), "build-program")) << "B: the build does not state its configuration";
        expect(s::lacks(s::text(b1), "populate failed")) << "B: the engine package's cache entry was not filled";
        expect(!s::glob(s::case_home(s::home::inherited) / "build-cache/v1/pkg/_engine", "*/*/bmi/mcpp.core.*").empty())
            << "B: the engine package's entry lacks mcpp.core";
        auto b2 = rule.mcpp({"test"}, inherited);
        expect(e2e::succeeded(b2)) << "B: the plugin's tests did not run";
        expect(e2e::contains(s::text(b2), "1 passed")) << "B: the test did not pass";
        // C.
        auto c = rule.mcpp({"build", "--target", "aarch64-linux-gnu"}, inherited);
        expect(c.started && c.status != 0) << "C: --target for another machine was accepted";
        expect(e2e::contains(s::text(c), "acme.rule' is a build plugin") && e2e::contains(s::text(c), "cannot apply")) << "C: the refusal does not name the plugin";
        // D.
        s::project app2{root / "app2", {{"mcpp.toml", "[package]\nname = \"app2\"\nversion = \"0.1.0\"\n[dependencies.acme]\nrule = { path = \"../rule\" }\n[targets.app2]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                        {"src/main.cpp", "int main() { return 0; }\n"}}};
        auto d = app2.mcpp({"build"}, inherited);
        expect(d.started && d.status != 0) << "D: a plugin in [dependencies] of a target was accepted";
        expect(e2e::contains(s::text(d), "is a build plugin") && e2e::contains(s::text(d), "app2 → acme.rule") && e2e::contains(s::text(d), "[build-dependencies]"))
            << "D: the refusal does not name the chain and the table";
        // E.
        s::project app3{root / "app3", {{"mcpp.toml", "[package]\nname = \"app3\"\nversion = \"0.1.0\"\n[targets.app3]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                        {"src/main.cpp", "import mcpp;\nint main() { return 0; }\n"}}};
        auto e = app3.mcpp({"build"}, inherited);
        expect(e.started && e.status != 0) << "E: a program importing mcpp was accepted";
        expect(e2e::contains(s::text(e), "exists only in a build")) << "E: the refusal does not say why";
        // F.
        auto f = rule.mcpp({"pack"}, inherited);
        expect(f.started && f.status != 0) << "F: mcpp pack accepted a plugin";
        expect(e2e::contains(s::text(f), "is a build plugin")) << "F: the refusal does not say why";
        // G.
        app.write("build.mcpp", "import acme.rule;\nimport acme.util;\nint main() { if (util_base() == 40) acme::rule::go(); }\n");
        auto g1 = app.mcpp({"build"}, inherited);
        expect(g1.started && g1.status != 0) << "G: a library the plugin does not hand on was importable";
        expect(e2e::contains(s::text(g1), "reexport = true")) << "G: the refusal does not name reexport";
        rule.write("mcpp.toml", std::regex_replace(rule.read("mcpp.toml"), std::regex(R"(util = \{ path = "\.\./util" \})"), R"(util = { path = "../util", reexport = true })"));
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, inherited)))) << "G: a library the plugin hands on was refused";
        expect(eq(run_line(app, "V="), std::string("V=42"))) << "G: value";
    };
}};

}  // namespace
