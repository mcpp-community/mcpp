// A dependency's host tools: its `bin` targets built for the host by a nested sub-build, kept in
// the tool store and handed to the consumer's build program (formerly tests/e2e/187).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// A tool that writes a source whose function `fn` returns `value`, to its first argument.
std::string generator(std::string_view fn, int value) {
    return std::format(R"cpp(#include <cstdio>
int main(int c, char** v) {{
    if (c < 2) return 2;
    FILE* f = std::fopen(v[1], "w");
    if (!f) return 3;
    std::fprintf(f, "int {}() {{ return {}; }}\n");
    std::fclose(f);
    return 0;
}}
)cpp", fn, value);
}

// A consumer's build.mcpp that runs dep_bin(`spelling`, `tool`) into a generated source; every
// spelling of `check` must resolve.
std::string runner(std::initializer_list<std::string_view> check, std::string_view use, std::string_view tool) {
    std::string out = "#include <cstdio>\n#include <cstdlib>\n#include <string>\nimport mcpp;\nint main() {\n";
    for (auto c : check)
        out += std::format("    if (!*mcpp::dep_bin(\"{0}\", \"{1}\")) {{ std::fprintf(stderr, \"{0} did not resolve\\n\"); return 1; }}\n", c, tool);
    out += std::format(R"cpp(    const char* t = mcpp::dep_bin("{}", "{}");
    if (!t || !*t) {{ std::fprintf(stderr, "no tool path\n"); return 1; }}
    std::string out = std::string(mcpp::out_dir()) + "/gen.cpp";
    std::string cmd = std::string("\"") + t + "\" \"" + out + "\"";
    if (std::system(cmd.c_str()) != 0) {{ std::fprintf(stderr, "tool failed\n"); return 1; }}
    mcpp::generated(out.c_str());
}}
)cpp", use, tool);
    return out;
}

E2E hosttools{"hosttools", [] {
    "187 dep host tool"_test[s::gcc] = [] {
        // #355: the tool is built for the host into the store, found by mcpp::dep_bin(), and what
        // it generates is linked; a tool behind required_features is built with them active; a
        // consumer that does not ask gets nothing; a bad name lists the bin targets; an override
        // skips the build; a namespaced package answers both of its spellings. The tool store is
        // the case's own (a linked home), so a stale entry cannot make a broken build look green.
        const s::options home{.where = s::home::linked};
        const auto root = s::case_dir();
        s::project toolpkg{root / "toolpkg", {{"mcpp.toml", R"cpp([package]
name    = "toolpkg"
version = "0.1.0"

[build]
sources = ["src/lib.cpp"]

[features.codegen]
sources = ["src/codegen.cpp"]

[targets.codegen]
kind              = "bin"
main              = "src/codegen.cpp"
required_features = ["codegen"]
)cpp"},
                                              {"src/lib.cpp", "int toolpkg_lib() { return 1; }\n"},
                                              {"src/codegen.cpp", generator("generated_answer", 42)}}};
        const std::string app_manifest = "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\ntoolpkg = { path = \"../toolpkg\", tools = [\"codegen\"] }\n";
        s::project app{root / "app", {{"mcpp.toml", app_manifest},
                                      {"src/main.cpp", "#include <cstdio>\nint generated_answer();\nint main() { std::printf(\"ANSWER=%d\\n\", generated_answer()); }\n"},
                                      {"build.mcpp", runner({}, "toolpkg", "codegen")}}};
        auto b1 = app.mcpp({"build"}, home);
        expect(fatal(e2e::succeeded(b1))) << "build with a dep host tool failed";
        expect(e2e::contains(s::text(b1), "host tool")) << "no tool-build announcement";
        expect(eq(s::last_starting(s::text(app.mcpp({"run"}, home)), "ANSWER="), std::string("ANSWER=42")));
        // The store keeps the tool: a second build does not build it again.
        app.remove("target");
        auto b2 = app.mcpp({"build"}, home);
        expect(fatal(e2e::succeeded(b2))) << "second build failed";
        expect(s::no_line(s::text(b2), "Building.*host tool")) << "the tool was rebuilt despite a valid store entry";

        // Without `tools = [...]` nothing is built.
        s::project plain{root / "plain", {{"mcpp.toml", "[package]\nname    = \"plain\"\nversion = \"0.1.0\"\n\n[dependencies]\ntoolpkg = { path = \"../toolpkg\" }\n"},
                                          {"src/main.cpp", "int main() {}\n"}}};
        auto b3 = plain.mcpp({"build"}, home);
        expect(fatal(e2e::succeeded(b3))) << "plain consumer build failed";
        expect(s::lacks(s::text(b3), "host tool")) << "a tool was provisioned for a consumer that never asked";

        // A bad tool name names the request and the bin targets there are.
        app.write("mcpp.toml", std::regex_replace(app_manifest, std::regex(R"(tools = \["codegen"\])"), R"(tools = ["nosuchtool"])"));
        app.remove("target");
        auto b4 = app.mcpp({"build"}, home);
        expect(b4.started && b4.status != 0) << "an unknown tool name was accepted";
        expect(e2e::contains(s::text(b4), "nosuchtool")) << "error does not name the request";
        expect(e2e::contains(s::text(b4), "codegen")) << "error does not list the available bin targets";

        // MCPP_TOOL_<PKG>_<TOOL> overrides the tool and skips its build.
        app.write("mcpp.toml", app_manifest);
        s::write_executable(root / "fake_codegen.sh", "#!/usr/bin/env bash\nprintf 'int generated_answer() { return 7; }\\n' > \"$1\"\n");
        app.remove("target");
        auto b5 = app.mcpp({"build"}, {.where = s::home::linked, .env = {{"MCPP_TOOL_TOOLPKG_CODEGEN", (root / "fake_codegen.sh").string()}}});
        expect(fatal(e2e::succeeded(b5))) << "build with a tool override failed";
        expect(e2e::contains(s::text(b5), "override")) << "the override was not reported";
        expect(eq(s::last_starting(s::text(app.mcpp({"run"}, home)), "ANSWER="), std::string("ANSWER=7")))
            << "the override was not actually used";

        // A legacy dotted name answers its canonical and short spellings.
        s::project ns{root / "ns", {{"mcpp.toml", "[package]\nname    = \"myns.tp\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/lib.cpp\"]\n\n[targets.gen]\nkind = \"bin\"\nmain = \"src/gen.cpp\"\n"},
                                    {"src/lib.cpp", "int tp_lib(){return 1;}\n"},
                                    {"src/gen.cpp", generator("gv", 5)}}};
        s::project nsapp{root / "nsapp", {{"mcpp.toml", "[package]\nname    = \"nsapp\"\nversion = \"0.1.0\"\n\n[dependencies]\n\"myns.tp\" = { path = \"../ns\", tools = [\"gen\"] }\n"},
                                          {"src/main.cpp", "#include <cstdio>\nint gv();\nint main(){std::printf(\"G=%d\\n\",gv());}\n"},
                                          {"build.mcpp", runner({"myns.tp", "tp"}, "tp", "gen")}}};
        expect(fatal(e2e::succeeded(nsapp.mcpp({"build"}, home)))) << "namespaced tool lookup failed";
        expect(eq(s::last_starting(s::text(nsapp.mcpp({"run"}, home)), "G="), std::string("G=5")));

        // `namespace` and `name` written apart (#647 E4.3): dep_bin answers the qualified name as
        // dep_dir does.
        s::project ns2{root / "ns2", {{"mcpp.toml", "[package]\nname      = \"tp2\"\nnamespace = \"myns2\"\nversion   = \"0.1.0\"\n\n[targets.gen]\nkind = \"bin\"\nmain = \"src/gen.cpp\"\n"},
                                      {"src/gen.cpp", generator("gv", 5)}}};
        s::project ns2app{root / "ns2app", {{"mcpp.toml", "[package]\nname    = \"ns2app\"\nversion = \"0.1.0\"\n\n[dependencies]\nmyns2.tp2 = { path = \"../ns2\", tools = [\"gen\"] }\n"},
                                            {"src/main.cpp", "#include <cstdio>\nint gv();\nint main(){std::printf(\"G2=%d\\n\",gv());}\n"},
                                            {"build.mcpp", runner({"myns2.tp2", "tp2"}, "myns2.tp2", "gen")}}};
        expect(fatal(e2e::succeeded(ns2app.mcpp({"build"}, home)))) << "a namespace + name package's tool was not addressable by its qualified name";
        expect(eq(s::last_starting(s::text(ns2app.mcpp({"run"}, home)), "G2="), std::string("G2=5")));
    };

    "189 host module rules"_test[s::gcc] = [] {
        // `host-module = true` makes a dependency's module importable from the consumer's
        // build.mcpp: a rule written once, as a C++ module in an ordinary package, compiled in the
        // same command as build.mcpp.
        const auto root = s::case_dir();
        auto rule = [](std::string_view param, std::string_view value) {
            return std::format(R"cpp(module;
#include <cstdio>
export module rules;
export namespace rules {{
inline void banner(const char* macro) {{
    std::printf("mcpp:cxxflag=-D%s=1\n", macro);
}}
inline void define_answer(int {}) {{
    std::printf("mcpp:cxxflag=-DRULE_ANSWER=%d\n", {});
}}
}}
)cpp", param, value);
        };
        s::project rules{root / "rules", {{"mcpp.toml", "[package]\nname    = \"rules\"\nversion = \"0.1.0\"\n\n[targets.rules]\nkind = \"lib\"\n"},
                                          {"src/rules.cppm", rule("n", "n")}}};
        s::project app{root / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nrules = { path = \"../rules\", host-module = true }\n"},
                                      {"src/main.cpp", R"cpp(#include <cstdio>
#ifndef RULE_ANSWER
#error "the rule package's directive never reached the compile"
#endif
#ifndef RULE_BANNER_OK
#error "the second rule directive never reached the compile"
#endif
int main() { std::printf("ANSWER=%d\n", RULE_ANSWER); }
)cpp"},
                                      {"build.mcpp", "import mcpp;\nimport rules;\nint main() {\n    rules::banner(\"RULE_BANNER_OK\");\n    rules::define_answer(42);\n}\n"}}};
        auto answer = [&] { return s::last_starting(s::text(app.mcpp({"run"})), "ANSWER="); };
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "build with a host rule module failed";
        expect(eq(answer(), std::string("ANSWER=42"))) << "the rule package's directives did not reach the build";
        // An edited rule runs build.mcpp again: the rule is part of what the helper was built from.
        rules.write("src/rules.cppm", rule("n_", "n_ + 1"));
        fs::last_write_time(app.at("src/main.cpp"), fs::file_time_type::clock::now());
        auto b2 = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(b2))) << "rebuild after editing the rule failed";
        expect(s::has_line(s::text(b2), R"(^ *build\.mcpp .* ran [0-9])")) << "editing the rule module did not re-run build.mcpp";
        expect(eq(answer(), std::string("ANSWER=43"))) << "the edited rule did not take effect";

        // A rule that uses `import std;` and the `import mcpp;` it wraps; the package is not also
        // compiled into the consumer's target.
        const auto edited = rules.read("src/rules.cppm");
        rules.write("src/rules.cppm", R"cpp(export module rules;
import std;
import mcpp;
export namespace rules {
inline void banner(const char* macro) { mcpp::define(macro); }
inline void define_answer(int n) {
    mcpp::cxxflag(std::format("-DRULE_ANSWER={}", n).c_str());
}
}
)cpp");
        app.remove("target");
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "a rule using import std + import mcpp did not build";
        expect(eq(answer(), std::string("ANSWER=42"))) << "the std/mcpp-based rule's directives did not land";
        bool compiled_as_library = false;
        for (const auto& o : s::find_files(app.at("target"), R"(.*\.o)"))
            compiled_as_library = compiled_as_library || o.generic_string().contains("obj/rules/");
        expect(!compiled_as_library) << "the host-module package was also built as a normal library";

        // mcpp#797: the module is found by the name its unit declares, wherever the file is.
        rules.remove("src/rules.cppm");
        rules.write("src/elsewhere.cppm", edited);
        app.remove("target");
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "a library whose interface is not at the conventional lib root was refused";
        expect(eq(answer(), std::string("ANSWER=43"))) << "the renamed interface's directive did not land";
    };

    "193 provision reexport"_test[s::gcc] = [] {
        // #359: a library stands up a toolchain on its user's behalf. Without `reexport` its tool
        // stays with it (an arbitrary transitive dependency cannot put entries in your build
        // program's tool namespace); with it, the consumer sees the tool, the rule module and the
        // package's directory through one dependency; a feature can add the request to an edge.
        const s::options home{.where = s::home::linked};
        const auto root = s::case_dir();
        s::project toolpkg{root / "toolpkg", {{"mcpp.toml", "[package]\nname    = \"toolpkg\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/lib.cpp\"]\n\n[targets.codegen]\nkind = \"bin\"\nmain = \"src/codegen.cpp\"\n"},
                                              {"src/lib.cpp", "int toolpkg_lib(){return 1;}\n"},
                                              {"src/codegen.cpp", generator("generated_answer", 42)}}};
        s::project rulepkg{root / "rulepkg", {{"mcpp.toml", "[package]\nname    = \"rulepkg\"\nversion = \"0.1.0\"\n\n[lib]\npath = \"src/rulepkg.cppm\"\n"},
                                              {"src/rulepkg.cppm", R"cpp(export module rulepkg;
import std;
import mcpp;
export namespace rulepkg {
bool generate() {
    const char* tool = mcpp::dep_bin("toolpkg", "codegen");
    if (!tool || !*tool) {
        std::println(std::cerr, "rulepkg: no codegen tool");
        return false;
    }
    if (std::string(mcpp::dep_dir("toolpkg")).empty()) {
        std::println(std::cerr, "rulepkg: no dep_dir for toolpkg");
        return false;
    }
    std::string out = std::string(mcpp::out_dir()) + "/gen.cpp";
    std::string cmd = std::string("\"") + tool + "\" \"" + out + "\"";
    if (std::system(cmd.c_str()) != 0) return false;
    mcpp::generated(out.c_str());
    return true;
}
}
)cpp"}}};
        const std::string lib_head = "[package]\nname    = \"mylib\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/mylib.cpp\"]\n\n";
        s::project lib{root / "lib", {{"mcpp.toml", lib_head + "[dependencies]\ntoolpkg = { path = \"../toolpkg\", tools = [\"codegen\"] }\nrulepkg = { path = \"../rulepkg\", host-module = true }\n"},
                                      {"src/mylib.cpp", "int mylib_fn(){return 1;}\n"}}};
        const std::string app_manifest = "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nmylib = { path = \"../lib\" }\n";
        s::project app{root / "app", {{"mcpp.toml", app_manifest},
                                      {"src/main.cpp", "#include <cstdio>\nint generated_answer();\nint main() { std::printf(\"ANSWER=%d\\n\", generated_answer()); }\n"},
                                      {"build.mcpp", R"cpp(#include <cstdio>
import mcpp;
int main() {
    const char* tool = mcpp::dep_bin("toolpkg", "codegen");
    std::printf("SEES_TOOL=%d\n", (tool && *tool) ? 1 : 0);
    return 1;
}
)cpp"}}};
        // 1. No reexport: the program (which fails on purpose) does not see the tool.
        auto b1 = app.mcpp({"build"}, home);
        expect(b1.started && b1.status != 0) << "build.mcpp returned 1 but the build succeeded";
        expect(s::lacks(s::text(b1), "SEES_TOOL=1")) << "a library's tool leaked to its consumer without reexport";
        expect(e2e::contains(s::text(b1), "SEES_TOOL=0")) << "build.mcpp did not run";

        // 2. reexport: one dependency line is enough; the rule is build-time only.
        lib.write("mcpp.toml", lib_head + "[dependencies]\ntoolpkg = { path = \"../toolpkg\", tools = [\"codegen\"], reexport = true }\nrulepkg = { path = \"../rulepkg\", host-module = true, reexport = true }\n");
        app.write("build.mcpp", "import mcpp;\nimport rulepkg;\nint main() { return rulepkg::generate() ? 0 : 1; }\n");
        app.remove("target");
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, home)))) << "build with re-exported provisions failed";
        expect(eq(s::last_starting(s::text(app.mcpp({"run"}, home)), "ANSWER="), std::string("ANSWER=42")))
            << "the re-exported tool's output was not linked";
        if (auto nm = s::on_path("nm"))
            for (const auto& bin : s::glob(app.at("target"), "*/*/bin/app"))
                expect(s::lacks(s::run(*nm, {"-C", bin.string()}).out, "rulepkg")) << "the rule package was linked into the consumer's binary";

        // 3. A feature adds the request (and reexport) to an edge declared unconditionally.
        lib.write("mcpp.toml", lib_head + "[dependencies]\ntoolpkg = { path = \"../toolpkg\" }\nrulepkg = { path = \"../rulepkg\", host-module = true, reexport = true }\n\n"
                               "[feature-deps.codegen]\ntoolpkg = { path = \"../toolpkg\", tools = [\"codegen\"], reexport = true }\n");
        app.write("mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nmylib = { path = \"../lib\", features = [\"codegen\"] }\n");
        app.remove("target");
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, home)))) << "a feature could not add a tool request to an existing dependency";
        expect(eq(s::last_starting(s::text(app.mcpp({"run"}, home)), "ANSWER="), std::string("ANSWER=42")));
        // ... and without the feature, nothing is built.
        app.write("mcpp.toml", app_manifest);
        app.remove("target");
        auto b4 = app.mcpp({"build"}, home);
        expect(b4.started && b4.status != 0) << "the build succeeded without the codegen feature — the tool ran anyway";
        expect(s::lacks(s::text(b4), "host tool")) << "a tool was built for a consumer that did not enable the feature";
    };

    "309 host module identity"_test[s::gcc] = [] {
        // A rule's module name is what its source declares, not its package's name (Clang and MSVC
        // are handed an explicit name=BMI mapping, so a package-derived name built only under
        // GCC); a dotted name survives whole; two rules declaring one name are refused naming both
        // packages; an `mcpp.` name from another namespace warns and still builds.
        const auto root = s::case_dir();
        // 1. Package `protobufgen`, module `acme.rules.protobuf`.
        s::project rules{root / "rules", {{"mcpp.toml", "[package]\nname    = \"protobufgen\"\nversion = \"0.1.0\"\n\n[targets.protobufgen]\nkind = \"lib\"\n"},
                                          {"src/protobufgen.cppm", "export module acme.rules.protobuf;\nimport std;\nimport mcpp;\nexport namespace acme::rules::protobuf {\ninline void apply() { mcpp::cxxflag(\"-DDIVERGENT_NAME_REACHED=1\"); }\n}\n"}}};
        s::project app{root / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nprotobufgen = { path = \"../rules\", host-module = true }\n"},
                                      {"src/main.cpp", "#include <cstdio>\n#ifndef DIVERGENT_NAME_REACHED\n#error \"the rule's directive never reached the compile\"\n#endif\nint main() { std::printf(\"DIVERGENT=%d\\n\", DIVERGENT_NAME_REACHED); }\n"},
                                      {"build.mcpp", "import mcpp;\nimport acme.rules.protobuf;\nint main() { acme::rules::protobuf::apply(); }\n"}}};
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "a rule whose declared module name differs from its package name did not build";
        expect(eq(s::last_starting(s::text(app.mcpp({"run"})), "DIVERGENT="), std::string("DIVERGENT=1")));
        // The import does not discriminate on GCC (its BMIs are keyed by the declared name); the
        // BMI the build programs' graph names (mcpp#797) is the evidence on every platform.
        const auto bm = app.at("target/.build-mcpp/host");
        expect(!s::find_files(bm, R"(acme\.rules\.protobuf\..*)").empty()) << "the host module was not named for its DECLARED module name";
        expect(s::find_files(bm, R"(protobufgen\.(gcm|pcm|ifc))").empty()) << "a BMI is named for the PACKAGE name";

        // 2. Two packages, one module name: refused, naming the name, both packages and both units.
        for (std::string n : {"alpha", "beta"}) {
            s::project dup{root / ("dup-" + n), {}};
            dup.write("mcpp.toml", std::format("[package]\nname      = \"{0}\"\nnamespace = \"vendor{0}\"\nversion   = \"0.1.0\"\n\n[targets.{0}]\nkind = \"lib\"\n", n));
            dup.write("src/" + n + ".cppm", std::format("export module sharedname;\nimport std;\nimport mcpp;\nexport namespace shared {{ inline void go() {{ mcpp::cxxflag(\"-DFROM_{}=1\"); }} }}\n", n));
        }
        s::project dupapp{root / "dupapp", {{"mcpp.toml", "[package]\nname    = \"dupapp\"\nversion = \"0.1.0\"\n\n[dependencies]\nalpha = { path = \"../dup-alpha\", host-module = true }\nbeta  = { path = \"../dup-beta\",  host-module = true }\n"},
                                            {"src/main.cpp", "int main() { return 0; }\n"},
                                            {"build.mcpp", "import mcpp;\nimport sharedname;\nint main() { shared::go(); }\n"}}};
        auto b2 = dupapp.mcpp({"build"});
        expect(b2.started && b2.status != 0) << "two rules declaring the same module name were accepted";
        for (auto want : {"sharedname", "vendoralpha.alpha", "vendorbeta.beta", "dup-alpha/src/alpha.cppm", "dup-beta/src/beta.cppm"})
            expect(e2e::contains(s::text(b2), want)) << "the collision diagnostic does not name '" << want << "'";

        // 3. The reserved `mcpp.` prefix from another namespace warns, and the build succeeds.
        const std::string claimer_manifest = "[package]\nname      = \"claimer\"\nnamespace = \"acme\"\nversion   = \"0.1.0\"\n\n[targets.claimer]\nkind = \"lib\"\n";
        s::project claimer{root / "claimer", {{"mcpp.toml", claimer_manifest},
                                              {"src/claimer.cppm", "export module mcpp.build.claimed;\nimport std;\nimport mcpp;\nexport namespace claimed { inline void go() { mcpp::cxxflag(\"-DCLAIMED=1\"); } }\n"}}};
        s::project claimapp{root / "claimapp", {{"mcpp.toml", "[package]\nname    = \"claimapp\"\nversion = \"0.1.0\"\n\n[dependencies]\nclaimer = { path = \"../claimer\", host-module = true }\n"},
                                                {"src/main.cpp", "#include <cstdio>\n#ifndef CLAIMED\n#error \"the rule never ran\"\n#endif\nint main() { std::printf(\"CLAIMED=%d\\n\", CLAIMED); }\n"},
                                                {"build.mcpp", "import mcpp;\nimport mcpp.build.claimed;\nint main() { claimed::go(); }\n"}}};
        auto b3 = claimapp.mcpp({"build"});
        expect(fatal(e2e::succeeded(b3))) << "the reserved prefix must WARN, not fail the build";
        expect(e2e::contains(s::text(b3), "mcpp.build.claimed")) << "no warning naming the claimed module";
        expect(e2e::contains(s::text(b3), "acme.claimer")) << "the warning does not name the package making the claim";
        expect(eq(s::last_starting(s::text(claimapp.mcpp({"run"})), "CLAIMED="), std::string("CLAIMED=1")));
        // A package in the mcpp namespace says the same without a warning.
        claimer.write("mcpp.toml", std::regex_replace(claimer_manifest, std::regex("namespace = \"acme\""), "namespace = \"mcpp\""));
        claimapp.remove("target");
        auto b4 = claimapp.mcpp({"build"});
        expect(fatal(e2e::succeeded(b4))) << "the official namespace build failed";
        expect(s::lacks(s::text(b4), "reserved for rules maintained by the mcpp project")) << "a package in the mcpp namespace was warned about its own prefix";
    };

    "312 build rules example"_test[s::gcc] = [] {
        // examples/08-build-rules builds and runs: two rule packages, one depending on the other
        // through its [build-dependencies], a `role = "check"` action and a generated source. It
        // is copied, without what an earlier run left in it, so that a second run does not
        // measure the first one's cache.
        const auto src = s::repository() / "examples/08-build-rules";
        expect(fatal(fs::is_directory(src))) << src.string() << " is missing";
        const auto ex = s::case_dir() / "ex";
        fs::copy(src, ex, fs::copy_options::recursive);
        for (const auto& t : s::glob(ex, "target")) fs::remove_all(t);
        for (const auto& t : s::glob(ex, "*/target")) fs::remove_all(t);
        for (const auto& t : s::glob(ex, "*/*/target")) fs::remove_all(t);
        for (const auto& l : s::find_files(ex, R"(mcpp\.lock)")) fs::remove(l);
        s::project app{ex / "app", {}};
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "the example did not build";
        expect(eq(s::last_line(app.mcpp({"run"}).out), std::string("hello from an embedded asset")));
        // The rules are build-time only, read on the artifact; the host modules' store under
        // .build-mcpp has an obj/ of its own and is not the consumer's.
        std::vector<std::string> objs;
        for (const auto& o : s::find_files(app.at("target"), R"(.*\.o)")) {
            const auto g = o.generic_string();
            if (g.contains("obj") && !g.contains("/.build-mcpp/")) objs.push_back(o.filename().string());
        }
        std::ranges::sort(objs);
        expect(eq(objs, std::vector<std::string>{"embed_greeting.o", "main.o"})) << "a rule package reached the link, or the denominator is wrong";
        // The generated source is a graph node: an edited input runs its edge again.
        std::this_thread::sleep_for(std::chrono::seconds(1));
        app.write("assets/greeting.txt", "a different embedded asset\n");
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "rebuild failed";
        expect(eq(s::last_line(app.mcpp({"run"}).out), std::string("a different embedded asset")))
            << "editing the asset did not re-run the embed action";
        // mcpp writes the check's stamp when its command succeeds (ninja would otherwise run the
        // edge again on every build, and stay green); a failing check fails the build.
        expect(eq(s::find_files(app.at("target"), R"(.*\.stamp)").size(), std::size_t{1})) << "expected exactly one check stamp";
        auto b3 = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(b3))) << "no-op rebuild failed";
        expect(s::lacks(s::text(b3), "CHECK")) << "the check re-ran with nothing changed — its stamp is not satisfying the edge";
        app.write("src/main.cpp", "int main() { goto done; done: return 0; }\n");
        auto b4 = app.mcpp({"build"});
        expect(b4.started && b4.status != 0) << "a failing check did not fail the build";
        expect(e2e::contains(s::text(b4), "goto")) << "the build failed without reporting what the check found";
    };

    "624 a missing host module names the key"_test[s::elf][s::gcc] = [] {
        // An import no dependency provides is refused by name before the compiler is reached
        // (whose message names neither the package nor the key); the names that can compile are
        // a closed set. The fix the message names is the fix that works.
        const auto root = s::case_dir();
        s::project rule{root / "rule", {{"src/rule.cppm", "export module rule;\nexport namespace testrule { inline int answer() { return 42; } }\n"},
                                        {"mcpp.toml", "[package]\nname    = \"rule\"\nversion = \"0.1.0\"\n\n[language]\nstandard   = \"c++23\"\nmodules    = true\nimport_std = true\n\n[build]\nsources = [\"src/rule.cppm\"]\n\n[targets.rule]\nkind = \"lib\"\n"}}};
        auto manifest = [](std::string_view host) {
            return std::format("[package]\nname    = \"consumer\"\nversion = \"0.1.0\"\n\n[language]\nstandard   = \"c++23\"\nmodules    = true\nimport_std = true\n\n"
                               "[build-dependencies]\nrule = {{ path = \"../rule\", host-module = {} }}\n\n[targets.consumer]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", host);
        };
        s::project app{root / "app", {{"src/main.cpp", "int main() { return 0; }\n"},
                                      {"build.mcpp", "import std;\nimport mcpp;\nimport rule;\nint main() { return testrule::answer() == 42 ? 0 : 1; }\n"},
                                      {"mcpp.toml", manifest("false")}}};
        auto r = app.mcpp({"build"});
        const auto out = s::text(r);
        expect(r.started && r.status != 0) << "the build succeeded with an unprovided import";
        expect(e2e::contains(out, "imports 'rule'")) << "the refusal does not name the module";
        expect(e2e::contains(out, "host-module = true")) << "the refusal does not name the key";
        expect(e2e::contains(out, "rule (in [build-dependencies])")) << "the refusal does not name the candidate dependency";
        expect(!s::contains_nocase(out, "failed to read compiled module")) << "the compiler was reached; the check must run first";
        app.write("mcpp.toml", manifest("true"));
        expect(e2e::succeeded(app.mcpp({"build"}))) << "adding host-module = true did not make it build";
    };

    "633 host modules are ordered by what they import"_test[s::gcc] = [] {
        // A host-module package's units are compiled in the order their imports require, not in
        // the order their paths sort; with no internal imports, path order survives. The reading
        // is a value the modules compute together, printed through mcpp::warning.
        const auto root = s::case_dir();
        const std::string head = "[package]\nname      = \"probe\"\nnamespace = \"ex\"\nversion   = \"0.1.0\"\n\n[language]\nstandard = \"c++23\"\nmodules  = true\n\n[build]\n";
        const std::string tail = "\n[features]\ndefault = []\nthing = { sources = [\"rules/thing.cppm\"] }\n";
        // Leg 1: import order is the reverse of path order.
        s::project probe{root / "probe", {{"mcpp.toml", head + "sources = [\"src/probe.cppm\", \"src/mm_mid.cppm\", \"src/zz_base.cppm\"]\n" + tail},
                                          {"src/probe.cppm", "export module probe;\nexport namespace probe { inline int root() { return 1000; } }\n"},
                                          {"src/zz_base.cppm", "export module probe.base;\nexport namespace probe::base { inline int value() { return 40; } }\n"},
                                          {"src/mm_mid.cppm", "export module probe.mid;\nimport probe.base;\nexport namespace probe::mid { inline int value() { return probe::base::value() + 1; } }\n"},
                                          {"rules/thing.cppm", "export module probe.rules.thing;\nimport probe;\nimport probe.mid;\nexport namespace probe::rules::thing {\ninline int value() { return probe::root() + probe::mid::value() + 1; }\n}\n"}}};
        s::project app{root / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[language]\nstandard = \"c++23\"\nmodules  = true\n\n[build-dependencies.ex]\nprobe = { path = \"../probe\", features = [\"thing\"], host-module = true }\n"},
                                      {"src/main.cpp", "int main() { return 0; }\n"},
                                      {"build.mcpp", "#include <string>\nimport mcpp;\nimport probe.rules.thing;\nint main() {\n    const std::string m = \"CHAIN=\" + std::to_string(probe::rules::thing::value());\n    mcpp::warning(m.c_str());\n}\n"}}};
        auto b1 = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(b1))) << "a package whose units import each other was compiled in path order";
        expect(e2e::contains(s::text(b1), "CHAIN=1042")) << "the chain did not compute 1000 + 41 + 1";
        // Leg 2: nothing constrains the order.
        probe.remove("src/mm_mid.cppm");
        probe.remove("src/zz_base.cppm");
        probe.write("mcpp.toml", head + "sources = [\"src/probe.cppm\", \"src/aa_one.cppm\", \"src/bb_two.cppm\"]\n" + tail);
        probe.write("src/aa_one.cppm", "export module probe.one;\nexport namespace probe::one { inline int value() { return 7; } }\n");
        probe.write("src/bb_two.cppm", "export module probe.two;\nexport namespace probe::two { inline int value() { return 35; } }\n");
        probe.write("rules/thing.cppm", "export module probe.rules.thing;\nimport probe;\nimport probe.one;\nimport probe.two;\nexport namespace probe::rules::thing {\ninline int value() { return probe::root() + probe::one::value() + probe::two::value(); }\n}\n");
        app.remove("target");
        auto b2 = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(b2))) << "a package with no internal imports stopped building";
        expect(e2e::contains(s::text(b2), "CHAIN=1042")) << "the unconstrained case did not compute 1000 + 7 + 35";
    };

    "665 a host tool follows its source"_test[s::gcc] = [] {
        // #630 item 6: the tool store's key carries a stamp of a `path` package's tree and the
        // resolved commit of a `git` one, so an edit under an unchanged version reaches the
        // consumer, so does its reversal, and an unchanged tree (or a commit seen before) is not
        // rebuilt. The store is the case's own.
        const s::options home{.where = s::home::linked};
        const auto root = s::case_dir();
        auto write_tool = [&](const fs::path& dir, int value) {
            s::project t{dir, {{"mcpp.toml", "[package]\nname    = \"toolpkg\"\nversion = \"0.1.0\"\n\n[targets.gen]\nkind = \"bin\"\nmain = \"src/gen.cpp\"\n"},
                               {"src/gen.cpp", generator("generated_answer", value)}}};
        };
        auto write_app = [&](const fs::path& dir, const std::string& dep) {
            return s::project{dir, {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\n" + dep + "\n"},
                                    {"src/main.cpp", "#include <cstdio>\nint generated_answer();\nint main() { std::printf(\"ANSWER=%d\\n\", generated_answer()); }\n"},
                                    {"build.mcpp", runner({}, "toolpkg", "gen")}}};
        };
        // Builds from nothing; the program's answer and whether the tool was built.
        auto answer = [&](const s::project& p) {
            p.remove("target");
            auto b = p.mcpp({"build"}, home);
            expect(fatal(e2e::succeeded(b))) << "build failed";
            return std::pair{s::last_starting(s::text(p.mcpp({"run"}, home)), "ANSWER="),
                             bool(s::has_line(s::text(b), "Building.*host tool"))};
        };
        auto second = [] { std::this_thread::sleep_for(std::chrono::seconds(1)); };
        write_tool(root / "toolpkg", 41);
        auto app = write_app(root / "app", "toolpkg = { path = \"../toolpkg\", tools = [\"gen\"] }");
        auto [a1, built1] = answer(app);
        expect(eq(a1, std::string("ANSWER=41")));
        expect(built1) << "the first build did not build the tool";
        second();
        auto [a2, built2] = answer(app);
        expect(eq(a2, std::string("ANSWER=41")));
        expect(!built2) << "an unchanged path tool was rebuilt";
        second();
        write_tool(root / "toolpkg", 42);
        auto [a3, built3] = answer(app);
        expect(eq(a3, std::string("ANSWER=42"))) << "an edit to a path tool did not reach the consumer";
        expect(built3) << "the edit did not rebuild the tool";
        second();
        write_tool(root / "toolpkg", 41);
        auto [a4, built4] = answer(app);
        expect(eq(a4, std::string("ANSWER=41"))) << "the reversal did not reach the consumer";
        expect(built4) << "the reversal did not rebuild the tool";

        // A `git` tool: keyed by the resolved commit, each commit's entry reused.
        const auto git_path = s::on_path("git");
        expect(fatal(git_path.has_value())) << "git is not on PATH";
        const auto git = [&](std::vector<std::string> args) {
            args.insert(args.begin(), {"-C", (root / "gittool").string(), "-c", "user.name=t", "-c", "user.email=t@t"});
            auto r = s::run(*git_path, args);
            expect(fatal(e2e::succeeded(r))) << "git failed";
            return s::last_line(r.out);
        };
        write_tool(root / "gittool", 7);
        git({"init", "-q", "-b", "main"});
        git({"add", "-A"});
        git({"commit", "-qm", "seven"});
        const auto a = git({"rev-parse", "HEAD"});
        write_tool(root / "gittool", 8);
        git({"commit", "-qam", "eight"});
        const auto b = git({"rev-parse", "HEAD"});
        auto pin = [&](const std::string& rev) {
            return std::format("toolpkg = {{ git = \"file://{}\", rev = \"{}\", tools = [\"gen\"] }}", (root / "gittool").generic_string(), rev);
        };
        auto gapp = write_app(root / "gapp", pin(a));
        expect(eq(answer(gapp).first, std::string("ANSWER=7"))) << "git rev A";
        gapp = write_app(root / "gapp", pin(b));
        auto [g2, gbuilt2] = answer(gapp);
        expect(eq(g2, std::string("ANSWER=8"))) << "git rev B";
        expect(gbuilt2) << "moving the pin to B did not rebuild the tool";
        gapp = write_app(root / "gapp", pin(a));
        auto [g3, gbuilt3] = answer(gapp);
        expect(eq(g3, std::string("ANSWER=7"))) << "git rev A again";
        expect(!gbuilt3) << "the entry for commit A was not reused";
    };

    "698 a tool source under its dependency has a bounded address"_test = [] {
        // #641 item 3: a source a build program takes from under its dependency's root has the
        // object address obj/<declaring>/__pkg/<owner>/<path inside the owner>, which does not
        // grow with the distance between the packages (A); such a tool builds and runs through
        // `tools = [...]` (B), also with the file nested deep, which on Windows passes 260
        // characters in the tool store's scratch (C: asserted wherever the runner enables long
        // paths; the readings say which case this runner is).
        const auto root = s::case_dir();
        auto write_dep = [&](const fs::path& dir, std::string_view inner) {
            s::project d{dir, {{"mcpp.toml", "[package]\nname    = \"dep\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.cpp\"]\n\n[targets.dep]\nkind = \"lib\"\n"},
                               {"src/dep.cpp", "int dep_base() { return 40; }\n"}}};
            d.write(std::string(inner) + "/helper.cpp", "int dep_base(); int helper_answer() { return dep_base() + 2; }\n");
        };
        auto write_installer = [&](const fs::path& dir, std::string_view dep, std::string_view inner) {
            return s::project{dir, {{"mcpp.toml", std::format("[package]\nname    = \"installer\"\nversion = \"0.1.0\"\n\n[dependencies]\ndep = {{ path = \"{}\" }}\n\n[targets.installer-app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", dep)},
                                    {"src/main.cpp", R"cpp(#include <cstdio>
int helper_answer();
int main(int argc, char** argv) {
    if (argc < 2) { std::printf("HELPER=%d\n", helper_answer()); return 0; }
    FILE* f = std::fopen(argv[1], "w");
    if (!f) return 3;
    std::fprintf(f, "int generated_answer() { return %d; }\n", helper_answer());
    std::fclose(f);
    return 0;
}
)cpp"},
                                    {"build.mcpp", std::format("#include <string>\nimport mcpp;\nint main() {{\n    const char* dep = mcpp::dep_dir(\"dep\");\n    if (!dep || !*dep) return 1;\n    mcpp::source((std::string(dep) + \"/{}/helper.cpp\").c_str());\n}}\n", inner)}}};
        };
        auto write_app = [&](const fs::path& dir, std::string_view installer) {
            return s::project{dir, {{"mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\ninstaller = {{ path = \"{}\", tools = [\"installer-app\"] }}\n", installer)},
                                    {"src/main.cpp", "#include <cstdio>\nint generated_answer();\nint main() { std::printf(\"ANSWER=%d\\n\", generated_answer()); }\n"},
                                    {"build.mcpp", R"cpp(#include <cstdio>
#include <cstdlib>
#include <string>
import mcpp;
int main() {
    const char* tool = mcpp::dep_bin("installer", "installer-app");
    if (!tool || !*tool) { std::fprintf(stderr, "no tool path\n"); return 1; }
    std::string out = std::string(mcpp::out_dir()) + "/gen.cpp";
    std::string cmd = std::string("\"") + tool + "\" \"" + out + "\"";
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";
#endif
    if (std::system(cmd.c_str()) != 0) { std::fprintf(stderr, "tool failed\n"); return 1; }
    mcpp::generated(out.c_str());
}
)cpp"}}};
        };
        // The helper's object address, read from the plan ninja was given.
        auto helper_address = [](const s::project& p) {
            std::smatch m;
            for (const auto& n : s::glob(p.at("target"), "*/*/build.ninja")) {
                const auto g = s::read_file(n);
                if (std::regex_search(g, m, std::regex(R"(obj/[^ :|\n]*helper\.o(bj)?)")))
                    return m.str();
            }
            return std::string();
        };
        // A.
        write_dep(root / "fw/dep", "platform/impl");
        auto near_inst = write_installer(root / "fw/tools/installer", "../../dep", "platform/impl");
        expect(fatal(e2e::succeeded(near_inst.mcpp({"build"})))) << "the installer did not build at its own root";
        const auto near = helper_address(near_inst);
        expect(fatal(!near.empty())) << "no object address for helper.cpp in the installer's plan";
        expect(s::lacks(near, "__up")) << "the address climbs with __up";
        // The owner's slug is its qualified name (`mcpplibs.dep` for a path package with no
        // namespace), spelled with `_`.
        expect(e2e::matches(near, R"(^obj/installer/__pkg/.*dep/platform/impl/helper\.o(bj)?$)"))
            << "the address does not name the owning package and the path inside it";
        auto deep_inst = write_installer(root / "fw/tools/windows/nested/installer", "../../../../dep", "platform/impl");
        expect(fatal(e2e::succeeded(deep_inst.mcpp({"build"})))) << "the deeper installer did not build";
        expect(eq(helper_address(deep_inst), near)) << "moving the installer two levels deeper changed the address";
        expect(eq(s::last_starting(s::text(deep_inst.mcpp({"run"})), "HELPER="), std::string("HELPER=42")))
            << "the installer does not run its dependency's helper";
        // B.
        auto app = write_app(root / "app", "../fw/tools/windows/nested/installer");
        auto b = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(b))) << "the consumer did not build the tool";
        expect(e2e::contains(s::text(b), "host tool")) << "no host tool was built";
        expect(eq(s::last_starting(s::text(app.mcpp({"run"})), "ANSWER="), std::string("ANSWER=42")));
        // C.
        const std::string deep = "platform/windows/installer_interface_components/generated_resources_and_views/bootstrapper_application_integration";
        write_dep(root / "deep/dep", deep);
        auto c_inst = write_installer(root / "deep/tools/installer", "../../dep", deep);
        auto c_app = write_app(root / "deep/app", "../tools/installer");
        std::smatch m;
        const auto env = c_inst.mcpp({"self", "env"}).out;
        const std::string cache_dir = std::regex_search(env, m, std::regex("(^|\n)build cache *= *([^\r\n]*)")) ? m[2].str() : "";
        expect(fatal(e2e::succeeded(c_inst.mcpp({"build"})))) << "the deep installer did not build at its own root";
        const auto deep_addr = helper_address(c_inst);
        const auto builds = s::glob(c_inst.at("target"), "*/*");
        const auto triple_fp = builds.empty() ? std::string() : builds.front().lexically_relative(c_inst.dir()).generic_string() + "/";
        const auto longest = cache_dir.size() + std::string_view("/tool/.build/0123456789abcdef/").size() + triple_fp.size() + deep_addr.size() + 8;
        std::println("READING C cache directory: {}", cache_dir);
        std::println("READING C longest sub-build path (estimated, characters): {}", longest);
        std::string long_paths = "unknown";
        if (s::on_windows()) {
            auto r = e2e::run({.argv = {"powershell", "-NoProfile", "-Command",
                                        R"((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem').LongPathsEnabled)"}});
            long_paths = s::last_line(r.out);
            if (long_paths.ends_with('\r')) long_paths.pop_back();
            if (long_paths.empty()) long_paths = "unreadable";
        }
        std::println("READING C LongPathsEnabled: {}", long_paths);
        auto c = c_app.mcpp({"build"});
        std::println("READING C deep tool build exit: {}", c.status);
        if (c) {
            expect(eq(s::last_starting(s::text(c_app.mcpp({"run"})), "ANSWER="), std::string("ANSWER=42")))
                << "the deep tool's output did not reach the consumer";
        } else if (s::on_windows() && long_paths != "1") {
            std::println("READING C the runner does not enable long paths; the failure is recorded, not asserted");
        } else {
            expect(e2e::succeeded(c)) << "the deep tool did not build";
        }
    };

    "712 a tool package contributes nothing to its consumer"_test[s::gcc] = [] {
        // #649 E6: a package whose targets are all programs contributes nothing to a consumer's
        // graph; its tools come from the tool sub-build. An SDK's program behind a feature that
        // depends on the SDK builds (it was a cycle of the consumer's graph) and runs, read under
        // both spellings; a tool's own library is not linked into the application; a cycle among
        // libraries is refused alike under both cache modes; a tool whose own build requests it
        // again is refused once, naming the repetition. The tool store is the case's own.
        const s::options home{.where = s::home::linked};
        auto fixture = [](const fs::path& w, std::string_view tool_dep) {
            s::project{w / "fw", {{"mcpp.toml", R"cpp([package]
name      = "fw"
namespace = "spike"
version   = "0.1.0"

[targets.fw]
kind = "lib"

[build]
sources = ["src/*.cpp"]

[features]
installer = []

[feature-deps.installer]
spike.fw-installer = { path = "tool", tools = ["fw-installer"], reexport = true }
)cpp"},
                                  {"src/fw.cpp", "int fw_answer() { return 42; }\n"},
                                  {"tool/mcpp.toml", std::format("[package]\nname      = \"fw-installer\"\nnamespace = \"spike\"\nversion   = \"0.1.0\"\n\n[targets.fw-installer]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n[dependencies]\n{}\n", tool_dep)}}};
            s::project{w / "z", {{"mcpp.toml", "[package]\nname      = \"z\"\nnamespace = \"spike\"\nversion   = \"0.1.0\"\n\n[targets.z]\nkind = \"lib\"\n\n[build]\nsources = [\"src/*.cpp\"]\n"},
                                 {"src/z.cpp", "int z_only_in_the_tool() { return 7; }\n"}}};
            return s::project{w / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n[dependencies]\nspike.fw = { path = \"../fw\" }\n\n[features]\nwindows-installer = [\"spike.fw/installer\"]\n"},
                                          {"src/main.cpp", "int fw_answer();\nint main() { return fw_answer() == 42 ? 0 : 1; }\n"},
                                          {"build.mcpp", R"cpp(import std;
import mcpp;
int main() {
    const char* a = mcpp::dep_bin("fw-installer", "fw-installer");
    const char* b = mcpp::dep_bin("spike.fw-installer", "fw-installer");
    mcpp::warning((std::string("DEPBIN short=[") + (a ? a : "") + "] qualified=["
                   + (b ? b : "") + "]").c_str());
    return 0;
}
)cpp"}}};
        };
        const auto root = s::case_dir();
        // 1. The SDK shape: the tool depends on the package declaring it.
        auto app1 = fixture(root / "s1", "spike.fw = { path = \"..\" }");
        app1.write("../fw/tool/src/main.cpp", "#include <cstdio>\nint fw_answer();\nint main() { std::printf(\"fw-installer ran %d\\n\", fw_answer()); return 0; }\n");
        auto b1 = app1.mcpp({"build", "--features", "windows-installer"}, home);
        expect(fatal(e2e::succeeded(b1))) << "a feature tool depending on its declaring package did not build";
        std::smatch m;
        const auto t1 = s::text(b1);
        expect(fatal(std::regex_search(t1, m, std::regex(R"(DEPBIN short=\[([^\]]*))")) && m[1].length() > 0))
            << "dep_bin(\"fw-installer\") read nothing";
        const fs::path tool = m[1].str();
        expect(s::has_line(t1, R"(qualified=\[[^\]])")) << "dep_bin(\"spike.fw-installer\") read nothing";
        auto ran = s::run(tool);
        expect(e2e::succeeded(ran)) << "the tool did not run";
        expect(eq(s::last_line(ran.out), std::string("fw-installer ran 42")));
        expect(e2e::succeeded(app1.mcpp({"run"}, home))) << "the application does not run";

        // 2. A tool's own library does not reach the application.
        auto app2 = fixture(root / "s2", "spike.z = { path = \"../../z\" }");
        app2.write("../fw/tool/src/main.cpp", "#include <cstdio>\nint z_only_in_the_tool();\nint main() { std::printf(\"tool %d\\n\", z_only_in_the_tool()); return 0; }\n");
        expect(fatal(e2e::succeeded(app2.mcpp({"build", "--features", "windows-installer"}, home)))) << "the tool-with-a-library build failed";
        std::optional<fs::path> graph;
        for (const auto& f : s::find_files(app2.at("target"), R"(build\.ninja)"))
            if (!graph && !f.generic_string().contains("/.build-mcpp/")) graph = f;
        expect(fatal(graph.has_value()));
        const auto g = s::read_file(*graph);
        expect(s::no_line(g, "^build bin/app.*spike_z")) << "the tool's own library was linked into the application";
        expect(s::lacks(g, "spike_fw-installer")) << "the tool package's sources were compiled in the consumer's build";

        // 3. A cycle among libraries, under both cache modes.
        for (std::string p : {"a", "b"}) {
            const std::string q = p == "a" ? "b" : "a";
            s::project{root / "s3" / p, {}}.write("mcpp.toml", std::format("[package]\nname      = \"{0}\"\nnamespace = \"spike\"\nversion   = \"0.1.0\"\n\n[targets.{0}]\nkind = \"lib\"\n\n[build]\nsources = [\"src/*.cpp\"]\n\n[dependencies]\nspike.{1} = {{ path = \"../{1}\" }}\n", p, q));
            s::project{root / "s3" / p, {}}.write("src/" + p + ".cpp", std::format("int {}_answer() {{ return 1; }}\n", p));
        }
        s::project app3{root / "s3/app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n[dependencies]\nspike.a = { path = \"../a\" }\n"},
                                          {"src/main.cpp", "int main() { return 0; }\n"}}};
        for (std::vector<std::string> args : {std::vector<std::string>{"build"}, std::vector<std::string>{"build", "--cache=local"}}) {
            auto b3 = app3.mcpp(args, home);
            expect(b3.started && b3.status != 0) << "a cycle of packages was accepted (" << args.size() << " args)";
            expect(e2e::contains(s::text(b3), "dependency cycle: 'spike.a' -> 'spike.b' -> 'spike.a'")) << "the cycle refusal does not name its edges";
        }

        // 4. A tool whose own build requests it again: refused once.
        auto app4 = fixture(root / "s4", "spike.fw = { path = \"..\", features = [\"installer\"] }");
        app4.write("../fw/tool/src/main.cpp", "int main() { return 0; }\n");
        auto b4 = app4.mcpp({"build", "--features", "windows-installer"}, home);
        expect(b4.started && b4.status != 0) << "a tool requesting itself through its own build was accepted";
        expect(e2e::contains(s::text(b4), "is requested by its own build")) << "the repetition is not named";
        std::size_t nested = 0;
        for (auto at = s::text(b4).find("building host tool"); at != std::string::npos; at = s::text(b4).find("building host tool", at + 1)) ++nested;
        expect(le(nested, std::size_t{1})) << "the repetition was refused only after nested sub-builds";
    };

    "775 a host tool merges its conditional sections once"_test = [] {
        // #690 F12: a dependency compiled as a host tool receives each entry of its matching
        // [target.<selector>.build] sections once (the sub-build merged the merged manifest again).
        // The entry is `-include once.h`, whose variable has no include guard; the package builds
        // on its own (the control).
        const s::options home{.where = s::home::inherited};
        const auto root = s::case_dir();
        s::project toolpkg{root / "toolpkg", {{"inc/once.h", "int once_counter = 1;\n"},
                                              {"mcpp.toml", "[package]\nname    = \"toolpkg\"\nversion = \"0.1.0\"\n\n[targets.gen]\nkind = \"bin\"\nmain = \"src/gen.cpp\"\n\n[build]\ninclude_dirs = [\"inc\"]\n\n[target.'cfg(not(os = \"none\"))'.build]\ncxxflags = [\"-include once.h\"]\n"},
                                              {"src/gen.cpp", R"cpp(#include <cstdio>
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::FILE* f = std::fopen(argv[1], "w");
    if (!f) return 3;
    std::fprintf(f, "int generated_answer() { return %d; }\n", 40 + once_counter);
    std::fclose(f);
    return 0;
}
)cpp"}}};
        s::project app{root / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\ntoolpkg = { path = \"../toolpkg\", tools = [\"gen\"] }\n"},
                                      {"src/main.cpp", "#include <cstdio>\nint generated_answer();\nint main() { std::printf(\"ANSWER=%d\\n\", generated_answer()); }\n"},
                                      {"build.mcpp", R"cpp(import std;
import mcpp;
int main() {
    const char* tool = mcpp::dep_bin("toolpkg", "gen");
    if (!tool || !*tool) return 1;
    std::string out = std::string(mcpp::out_dir()) + "/gen.cpp";
    std::string cmd = std::string("\"") + tool + "\" \"" + out + "\"";
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";
#endif
    if (std::system(cmd.c_str()) != 0) return 1;
    mcpp::generated(out.c_str());
}
)cpp"}}};
        expect(fatal(e2e::succeeded(toolpkg.mcpp({"build"}, home)))) << "control: the tool package does not build on its own";
        auto b = app.mcpp({"build"}, home);
        expect(s::lacks(s::text(b), "redefinition")) << "the host tool received a conditional entry twice";
        expect(fatal(e2e::succeeded(b))) << "the consumer did not build";
        auto r = app.mcpp({"run"}, home);
        expect(e2e::succeeded(r)) << "the program did not run";
        expect(s::has_line(s::text(r), "^ANSWER=41")) << "wrong answer";
    };

    "800 a feature provides its host tools"_test[s::gcc] = [] {
        // mcpp#709: `[features.<f>] tools = ["<bin>"]`: a consumer that enables the feature
        // receives the tool as if its edge had written `tools = [...]`; one that does not gets
        // nothing built; an entry naming no bin target is refused, naming the bin targets.
        const s::options home{.where = s::home::linked};
        const auto root = s::case_dir();
        const std::string tool_manifest = "[package]\nname    = \"toolpkg\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/lib.cpp\"]\n\n[features.codegen]\ntools = [\"codegen\"]\n\n[targets.codegen]\nkind = \"bin\"\nmain = \"src/codegen.cpp\"\n\n[targets.toolpkg]\nkind = \"lib\"\n";
        s::project toolpkg{root / "toolpkg", {{"mcpp.toml", tool_manifest},
                                              {"src/lib.cpp", "int toolpkg_lib() { return 1; }\n"},
                                              {"src/codegen.cpp", generator("generated_answer", 42)}}};
        s::project app{root / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\ntoolpkg = { path = \"../toolpkg\", features = [\"codegen\"] }\n"},
                                      {"src/main.cpp", "#include <cstdio>\nint generated_answer();\nint main() { std::printf(\"ANSWER=%d\\n\", generated_answer()); }\n"},
                                      {"build.mcpp", runner({}, "toolpkg", "codegen")}}};
        auto b1 = app.mcpp({"build"}, home);
        expect(fatal(e2e::succeeded(b1))) << "1: build failed";
        expect(e2e::contains(s::text(b1), "host tool toolpkg:codegen")) << "1: the feature's tool was not built";
        expect(eq(s::last_starting(s::text(app.mcpp({"run"}, home)), "ANSWER="), std::string("ANSWER=42"))) << "1: generated source not linked";
        s::project plain{root / "plain", {{"mcpp.toml", "[package]\nname    = \"plain\"\nversion = \"0.1.0\"\n\n[dependencies]\ntoolpkg = { path = \"../toolpkg\" }\n"},
                                          {"src/main.cpp", "int main() {}\n"}}};
        auto b2 = plain.mcpp({"build"}, home);
        expect(fatal(e2e::succeeded(b2))) << "2: build failed";
        expect(s::lacks(s::text(b2), "host tool")) << "2: a tool was built for a consumer that did not enable the feature";
        toolpkg.write("mcpp.toml", std::regex_replace(tool_manifest, std::regex(R"(tools = \["codegen"\])"), R"(tools = ["nosuch"])"));
        app.remove("target");
        auto b3 = app.mcpp({"build"}, home);
        expect(b3.started && b3.status != 0) << "3: an unknown tool name was accepted";
        expect(e2e::contains(s::text(b3), "nosuch") && e2e::contains(s::text(b3), "codegen")) << "3: the refusal does not name the entry and the bin targets";
    };

    "802 a host build applies its host row"_test[s::gcc][s::elf] = [] {
        // mcpp#704: a build without --target targets the host, and [target.<host-triple>]
        // describes it: without the row a plain build is self-contained (no NEEDED
        // libstdc++.so.6); with `cxx_runtime = "toolchain-coupled"` it needs it, as
        // `--target <host>` does; the row is found under another spelling of the triple.
        const auto uname = s::on_path("uname");
        expect(fatal(uname.has_value()));
        const auto arch = s::last_line(s::run(*uname, {"-m"}).out);
        s::project p{{"src/main.cpp", "#include <cstdio>\n#include <string>\nint main() { std::string s = \"RT_OK\"; std::printf(\"%s\\n\", s.c_str()); }\n"}};
        auto write_manifest = [&](const std::string& selector) {
            std::string m = "[package]\nname    = \"cxxrt\"\nversion = \"0.1.0\"\n\n[language]\nstandard   = \"c++23\"\nimport_std = false\n\n[toolchain]\ndefault = \"gcc@16.1.0\"\n\n[targets.cxxrt]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n";
            if (!selector.empty())
                m += std::format("\n[target.{}]\ncxx_runtime = \"toolchain-coupled\"\n", selector);
            p.write("mcpp.toml", m);
        };
        auto needs_libstdcxx = [&] {
            std::optional<fs::path> bin;
            for (const auto& f : s::find_files(p.at("target"), "cxxrt"))
                if (!bin && f.parent_path().filename() == "bin") bin = f;
            expect(fatal(bin.has_value())) << "no program built";
            return std::ranges::any_of(s::read_header(*bin).needed, [](const std::string& n) { return n.starts_with("libstdc++.so"); });
        };
        write_manifest("");
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "1: build failed";
        expect(!needs_libstdcxx()) << "1: the default build is not self-contained";
        p.remove("target");
        write_manifest(arch + "-linux-gnu");
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "2: build failed";
        expect(needs_libstdcxx()) << "2: [target.<host>] cxx_runtime was not applied to a host build";
        expect(eq(s::last_line(p.mcpp({"run"}).out), std::string("RT_OK"))) << "2: the program does not run";
        p.remove("target");
        write_manifest(arch + "-unknown-linux-gnu");
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "3: build failed";
        expect(needs_libstdcxx()) << "3: the row was not matched under another spelling";
    };

    "804 a path host tool builds with its chosen toolchain"_test[s::gcc][s::elf] = [] {
        // mcpp#710: a host tool is built by one compiler recorded in the store's key:
        // MCPP_TOOLCHAIN, else the tool package's own [toolchain], else the consumer's
        // build-program compiler. Through a plain path dependency: 1. no toolchain of its own,
        // the consumer's llvm (not the global default gcc); 2. its own gcc 16.1.0 while the
        // consumer keeps llvm. The compiler is read from the tool's .comment strings (the script
        // used `readelf -p .comment`; the case reads the bytes).
        const bool llvm_only = s::env("MCPP_E2E_804_LLVM_HOST_ONLY").value_or("0") == "1";
        if (llvm_only)
            expect(fatal(s::on_linux() && !s::on_x86_64())) << "LLVM-only host-helper admission requires native Linux ARM64";
        const auto llvm = s::llvm_version();
        const s::options home{.where = s::home::linked, .env = {{"MCPP_TOOLCHAIN", std::nullopt}}};
        if (auto index = s::env("MCPP_NATIVE_XIM_INDEX"); index && !index->empty()) {
            const auto python = s::on_path("python3");
            expect(fatal(python.has_value()));
            expect(fatal(e2e::succeeded(s::run(*python, {(s::repository() / ".github/tools/seed_native_xim_index.py").string(),
                                                          s::case_home(s::home::linked).string(), *index}))));
        }
        const auto root = s::case_dir();
        s::project toolpkg{root / "toolpkg", {{"src/stamp.cpp", "int main() { return 0; }\n"}}};
        auto write_tool = [&](std::string_view body) {
            std::string m = "[package]\nname    = \"toolpkg\"\nversion = \"0.1.0\"\n\n[targets.stamp]\nkind = \"bin\"\nmain = \"src/stamp.cpp\"\n";
            if (!body.empty()) m += std::format("\n[toolchain]\n{}\n", body);
            toolpkg.write("mcpp.toml", m);
        };
        s::project app{root / "app", {{"mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"llvm@{}\"\n\n[dependencies]\ntoolpkg = {{ path = \"../toolpkg\", tools = [\"stamp\"] }}\n", llvm)},
                                      {"src/main.cpp", "int main() {}\n"},
                                      {"build.mcpp", "#include <string>\nimport mcpp;\nint main() {\n    const char* tool = mcpp::dep_bin(\"toolpkg\", \"stamp\");\n    if (!tool || !*tool) return 1;\n    mcpp::warning((std::string(\"TOOL=\") + tool).c_str());\n}\n"}}};
        auto tool_path = [](const e2e::outcome& r) {
            std::string t;
            for (const auto& l : s::lines_with(s::text(r), "TOOL=")) t = l.substr(l.find("TOOL=") + 5);
            return fs::path(t);
        };
        // 1.
        write_tool("");
        auto b1 = app.mcpp({"build"}, home);
        expect(fatal(e2e::succeeded(b1))) << "1: build failed";
        const auto t1 = tool_path(b1);
        expect(fatal(!t1.empty() && fs::is_regular_file(t1))) << "1: no tool binary at '" << t1.string() << "'";
        expect(e2e::contains(s::read_file(t1), "clang version " + llvm)) << "1: the tool was not built by the consumer's llvm " << llvm;
        if (llvm_only)
            return;  // ARM64 publishes LLVM GNU only: the LLVM case is the one this host proves
        // 2.
        write_tool("default = \"gcc@16.1.0\"");
        app.remove("target");
        auto b2 = app.mcpp({"build"}, home);
        expect(fatal(e2e::succeeded(b2))) << "2: build failed";
        const auto t2 = tool_path(b2);
        expect(fatal(!t2.empty() && fs::is_regular_file(t2))) << "2: no tool binary at '" << t2.string() << "'";
        const auto bytes = s::read_file(t2);
        expect(std::regex_search(bytes, std::regex(R"(GCC: \([^\n]*\) 16\.1\.0)"))) << "2: the tool's own [toolchain] was not used";
        expect(s::lacks(bytes, "clang version")) << "2: the tool was built by the consumer's clang";
    };

    "826 plugin diagnostics features and names"_test[s::unix_shell] = [] {
        // #734 E11, E7, E10: a plugin `acme.gen` offers `mcpp.acme.gen` and two modules behind
        // features. D1 a structured diagnostic in the engine's form (impact, help); D2 a cache hit
        // reports it again; N1 a module behind a disabled feature fails naming the package and the
        // feature; R1 `mcpp.acme.*` from `acme` draws no warning; R2 `mcpp.rules.*` from `acme`
        // does, naming `mcpp.acme.*`.
        const auto root = s::case_dir();
        s::project gen{root / "gen", {{"mcpp.toml", "[package]\nnamespace = \"acme\"\nname      = \"gen\"\nversion   = \"0.1.0\"\n\n[lib]\npath = \"src/gen.cppm\"\n\n[features.extra]\nsources = [\"src/extra.cppm\"]\n\n[features.bad]\nsources = [\"src/bad.cppm\"]\n"},
                                      {"src/gen.cppm", R"cpp(export module mcpp.acme.gen;
import mcpp.core;
export namespace acme {
inline void tell() {
    mcpp::report({.severity = "warning",
                  .message  = "the generator found no schema",
                  .impact   = "no bindings are generated",
                  .hint     = "add schema/*.proto"});
}
}
)cpp"},
                                      {"src/extra.cppm", "export module mcpp.acme.gen.extra;\nexport namespace acme { inline int extra() { return 1; } }\n"},
                                      {"src/bad.cppm", "export module mcpp.rules.bad;\nexport namespace acme { inline int bad() { return 2; } }\n"}}};
        s::project app{root / "app", {{"src/main.cpp", "int main() { return 0; }\n"}}};
        auto edge = [&](std::string_view extra) {
            app.write("mcpp.toml", std::format("[package]\nname = \"app826\"\nversion = \"0.1.0\"\n\n[build-dependencies]\n\"acme.gen\" = {{ path = \"../gen\", host-module = true{} }}\n", extra));
        };
        // D1, R1.
        edge("");
        app.write("build.mcpp", "import mcpp.core;\nimport mcpp.acme.gen;\nint main() { acme::tell(); return 0; }\n");
        auto d1 = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(d1))) << "D1: the build failed";
        expect(e2e::contains(s::text(d1), "the generator found no schema")) << "D1: the message is missing";
        expect(e2e::contains(s::text(d1), "impact: no bindings are generated")) << "D1: the impact line is missing";
        expect(e2e::contains(s::text(d1), "help: add schema/*.proto")) << "D1: the help line is missing";
        expect(s::no_line(s::text(d1), R"(mcpp\.acme\.gen'.*mcpp\.|claims an origin)")) << "R1: an own-namespace module was warned about";
        // D2: past the fast path, a cache hit.
        fs::last_write_time(app.at("src/main.cpp"), fs::file_time_type::clock::now());
        auto d2 = app.mcpp({"build", "-v"});
        expect(fatal(e2e::succeeded(d2))) << "D2: the second build failed";
        expect(s::has_line(s::text(d2), R"(^ *build\.mcpp .* cached)")) << "D2: the second build ran the program; the replay is not measured";
        expect(e2e::contains(s::text(d2), "impact: no bindings are generated")) << "D2: the cached run did not report the diagnostic";
        // N1.
        app.write("build.mcpp", "import mcpp.core;\nimport mcpp.acme.gen.extra;\nint main() { return acme::extra() == 1 ? 0 : 1; }\n");
        auto n1 = app.mcpp({"build"});
        expect(n1.started && n1.status != 0) << "N1: a module behind a disabled feature was importable";
        expect(e2e::contains(s::text(n1), "provided by: acme.gen, feature \"extra\" (not enabled)")) << "N1: the error does not name the feature";
        edge(", features = [\"extra\"]");
        expect(e2e::succeeded(app.mcpp({"build"}))) << "N1: enabling the named feature did not fix the build";
        // R2.
        edge(", features = [\"bad\"]");
        app.write("build.mcpp", "import mcpp.core;\nimport mcpp.rules.bad;\nint main() { return acme::bad() == 2 ? 0 : 1; }\n");
        auto r2 = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r2))) << "R2: the build failed";
        expect(e2e::contains(s::text(r2), "uses 'mcpp.rules'")) << "R2: a reserved segment was not warned about";
        expect(e2e::contains(s::text(r2), "'mcpp.acme.*'")) << "R2: the warning does not name the accepted form";
    };

    "859 a host module entry is reused while its inputs agree"_test = [] {
        // #748 (B1), mcpp#797 (R6): a build program's library is compiled again exactly when what
        // it is compiled from changed (the identity holds the digest of the package's tree, so an
        // included header counts), and the program never runs on a stale compile. A: a stale
        // program compiles nothing; B: an interface edit compiles both; C: a header edit does,
        // and the program reads the new value; D: a record that disagrees is a miss, replaced.
        const s::options home{.where = s::home::inherited, .env = {{"MCPP_VERBOSE", "1"}}};
        const auto root = s::case_dir();
        s::project rules{root / "rules", {{"mcpp.toml", "[package]\nname    = \"rule859\"\nversion = \"0.1.0\"\n\n[targets.rule859]\nkind = \"lib\"\n"},
                                          {"src/value.h", "#define T859_VALUE 1\n"},
                                          {"src/rule859.cppm", "module;\n#include \"value.h\"\nexport module t859.rules;\nimport mcpp;\nexport int rule_value() { return T859_VALUE; }\n"}}};
        s::project app{root / "app", {{"mcpp.toml", "[package]\nname    = \"app859\"\nversion = \"0.1.0\"\n\n[build-dependencies]\nrule859 = { path = \"../rules\", host-module = true }\n\n[targets.app859]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                      {"src/main.cpp", "int main() { return 0; }\n"},
                                      {"build.mcpp", R"cpp(import mcpp;
import t859.rules;
int main() {
    mcpp::rerun_if_changed("marker.txt");
    mcpp::warning(rule_value() == 1 ? "T859 value one"
                : rule_value() == 2 ? "T859 value two"
                :                     "T859 value other");
    return 0;
}
)cpp"},
                                      {"marker.txt", "0\n"}}};
        const auto host = app.at("target/.build-mcpp/host");
        auto stamp = [&] {
            auto o = s::find_file(host, "rule859.m.o");
            expect(fatal(o.has_value())) << "no library object";
            return std::pair{fs::last_write_time(*o), fs::file_size(*o)};
        };
        auto compiled = [](const e2e::outcome& r) {
            std::size_t n = 0;
            const std::regex re(R"(build-program: app859: identity [0-9a-f]+ -> [0-9a-f]+, compiled)");
            for (const auto& l : s::lines_with(s::text(r), "build-program: app859"))
                n += std::regex_search(l, re) ? 1 : 0;
            return n;
        };
        // A stale program: its declared input changed, the sources of its imports did not.
        auto rerun = [&](std::string_view marker) {
            app.write("marker.txt", std::string(marker) + "\n");
            auto r = app.mcpp({"build"}, home);
            expect(fatal(e2e::succeeded(r))) << "the build failed";
            return r;
        };
        auto first = app.mcpp({"build"}, home);
        expect(fatal(e2e::succeeded(first))) << "the first build failed";
        expect(fatal(s::find_file(host, "rule859.m.o").has_value())) << "the first build compiled no library object";
        expect(e2e::contains(s::text(first), "T859 value one")) << "the program did not read the library's value";
        const auto s0 = stamp();
        auto second = [] { std::this_thread::sleep_for(std::chrono::seconds(1)); };
        // A.
        second();
        auto a = rerun("1");
        expect(eq(compiled(a), std::size_t{0})) << "A: a stale program was compiled again";
        expect(stamp() == s0) << "A: the library was compiled again";
        expect(e2e::contains(s::text(a), "T859 value one")) << "A: the program gave another value";
        // B.
        second();
        rules.write("src/rule859.cppm", "module;\n#include \"value.h\"\nexport module t859.rules;\nimport mcpp;\nexport int rule_value() { return T859_VALUE + 0; }\n");
        auto b = rerun("2");
        expect(eq(compiled(b), std::size_t{1})) << "B: an edit to the interface did not compile the program again";
        expect(stamp() != s0) << "B: an edit to the interface did not compile the library";
        const auto s1 = stamp();
        // C.
        second();
        rules.write("src/value.h", "#define T859_VALUE 2\n");
        auto c = rerun("3");
        expect(eq(compiled(c), std::size_t{1})) << "C: an edit to an included header did not compile the program again";
        expect(stamp() != s1) << "C: an edit to an included header did not compile the library";
        expect(e2e::contains(s::text(c), "T859 value two")) << "C: the program took the compile made before the header changed";
        // D.
        auto recs = s::glob(host, "*/programs.json");
        expect(fatal(!recs.empty())) << "D: no build programs' record";
        const auto rec = recs.front();
        s::project{rec.parent_path(), {}}.write("programs.json", std::regex_replace(s::read_file(rec), std::regex(R"("identity": "[0-9a-f]*")"), R"("identity": "0000000000000000")"));
        auto d = rerun("4");
        expect(eq(compiled(d), std::size_t{1})) << "D: a record that disagrees was used without compiling";
        expect(s::lacks(s::read_file(rec), R"("identity": "0000000000000000")")) << "D: the record that disagreed was not replaced";
        expect(e2e::contains(s::text(d), "T859 value two")) << "D: the replaced compile gave another value";
    };

    "997 a members dev dependencies stay out of a host tool"_test[s::gcc] = [] {
        // mcpp#874: a host tool has one store key whatever the command and the member; its
        // closure is the tool package's own manifests (it was read from the graph the build had
        // walked, holding a member's dev-dependencies). A: test --workspace then build: one entry,
        // naming `lib` and not `testing`; B: a second test --workspace builds no host tool.
        const s::options home{.where = s::home::linked};
        s::project ws{s::case_dir() / "ws", {{"mcpp.toml", "[workspace]\nmembers = [\"testing\", \"lib\", \"toolpkg\", \"app\"]\n"},
                                          {"testing/mcpp.toml", "[package]\nname    = \"testing\"\nversion = \"0.1.0\"\n\n[targets.testing]\nkind = \"lib\"\n"},
                                          {"testing/src/testing.cpp", "int testing_ok() { return 0; }\n"},
                                          {"lib/mcpp.toml", "[package]\nname    = \"lib\"\nversion = \"0.1.0\"\n\n[targets.lib]\nkind = \"lib\"\n\n[dev-dependencies]\ntesting = { path = \"../testing\" }\n"},
                                          {"lib/src/lib.cpp", "int lib_value() { return 41; }\n"},
                                          {"lib/tests/smoke.cpp", "int testing_ok();\nint lib_value();\nint main() { return testing_ok() + (lib_value() == 41 ? 0 : 1); }\n"},
                                          {"toolpkg/mcpp.toml", "[package]\nname    = \"toolpkg\"\nversion = \"0.1.0\"\n\n[dependencies]\nlib = { path = \"../lib\" }\n\n[targets.gen]\nkind = \"bin\"\nmain = \"src/gen.cpp\"\n"},
                                          {"toolpkg/src/gen.cpp", R"cpp(#include <cstdio>
int lib_value();
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    FILE* f = std::fopen(argv[1], "w");
    if (!f) return 3;
    std::fprintf(f, "int generated_answer() { return %d; }\n", lib_value() + 1);
    return std::fclose(f);
}
)cpp"},
                                          {"app/mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[build-dependencies]\ntoolpkg = { path = \"../toolpkg\", tools = [\"gen\"] }\n"},
                                          {"app/src/main.cpp", "#include <cstdio>\nint generated_answer();\nint main() { std::printf(\"ANSWER=%d\\n\", generated_answer()); }\n"},
                                          {"app/tests/answer.cpp", "int generated_answer();\nint main() { return generated_answer() == 42 ? 0 : 1; }\n"},
                                          {"app/build.mcpp", R"cpp(#include <cstdlib>
#include <string>
import mcpp;
int main() {
    const std::string out = std::string(mcpp::out_dir()) + "/gen.cpp";
    const std::string cmd = std::string("\"") + mcpp::dep_bin("toolpkg", "gen") + "\" \"" + out + "\"";
    if (std::system(cmd.c_str()) != 0) return 1;
    mcpp::generated(out.c_str());
}
)cpp"}}};
        // A.
        expect(fatal(e2e::succeeded(ws.mcpp({"test", "--workspace"}, home)))) << "the first test run failed";
        expect(fatal(e2e::succeeded(ws.mcpp({"build"}, home)))) << "the build failed";
        std::vector<fs::path> entries;
        for (const auto& e : s::find_files(s::case_home(s::home::linked) / "build-cache/v1/tool", R"(entry\.json)"))
            if (e.generic_string().contains("toolpkg@")) entries.push_back(e);
        expect(fatal(eq(entries.size(), std::size_t{1}))) << "the tool's store entries";
        const auto entry = s::read_file(entries.front());
        expect(e2e::contains(entry, "\"lib@")) << "the key does not name the tool's dependency lib";
        expect(s::lacks(entry, "\"testing@")) << "the key names lib's dev-dependency testing";
        // B.
        auto t2 = ws.mcpp({"test", "--workspace"}, home);
        expect(fatal(e2e::succeeded(t2))) << "the second test run failed";
        expect(s::no_line(s::text(t2), "Building.*host tool")) << "the second test run built a host tool again";
    };
}};

// The cases that hold the machine (P7.1): a suite of their own, which the runner runs with no
// other test beside it. 831 states that the fast path replays the build, and the fast path declines
// when the runtime environment's description changed: another program installing into the shared
// home beside it rewrote that description (measured on Linux CI, 2026-11-14).
E2E hosttools_machine{"hosttools-machine", resource{"machine"}, [] {
    "831 a path dependency host module outside src"_test[s::unix_shell] = [] {
        // #734: a path dependency's host module is compiled into the consumer's build program,
        // which no edge of build.ninja names, so the fast path sees an edit to it itself (it swept
        // only src/). S1 an edit to rules/hm.cppm re-runs the program; S2 a newer file of a
        // package nested in the dependency does not decline the fast path.
        const auto root = s::case_dir();
        s::project plug{root / "plug", {{"mcpp.toml", "[package]\nnamespace = \"probe\"\nname      = \"plug831\"\nversion   = \"0.1.0\"\n\n[build]\nsources = [\"src/plug831.cppm\"]\n\n[features.hm]\nsources = [\"rules/hm.cppm\"]\n"},
                                        {"src/plug831.cppm", "export module probe.plug831;\nexport int plug_root() { return 0; }\n"},
                                        {"rules/hm.cppm", "export module probe.hm831;\nexport inline int value() { return 1; }\n"},
                                        {"tests/fixture/mcpp.toml", "[package]\nname = \"fixture831\"\nversion = \"0.1.0\"\n"},
                                        {"tests/fixture/src/main.cpp", "int main() { return 0; }\n"}}};
        s::project app{root / "app", {{"mcpp.toml", "[package]\nname    = \"app831\"\nversion = \"0.1.0\"\n\n[build-dependencies.probe]\nplug831 = { path = \"../plug\", features = [\"hm\"], host-module = true }\n"},
                                      {"build.mcpp", "import std;\nimport mcpp;\nimport probe.hm831;\nint main() {\n    if (value() != 1) { std::println(\"host module value {}\", value()); return 1; }\n    return 0;\n}\n"},
                                      {"src/main.cpp", "int main() { return 0; }\n"}}};
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "the first build failed";
        expect(fatal(e2e::succeeded(app.mcpp({"build", "-v"})))) << "the second build failed";
        // S1.
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        plug.write("rules/hm.cppm", "export module probe.hm831;\nexport inline int value() { return 2; }\n");
        auto s1 = app.mcpp({"build"});
        expect(s1.started && s1.status != 0) << "S1: the build after an edit to rules/hm.cppm succeeded without re-running the build program";
        expect(e2e::contains(s::text(s1), "host module value 2")) << "S1: the build program did not read the edited host module";
        // S2.
        plug.write("rules/hm.cppm", "export module probe.hm831;\nexport inline int value() { return 1; }\n");
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "S2: the build after restoring the host module failed";
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        fs::last_write_time(plug.at("tests/fixture/src/main.cpp"), fs::file_time_type::clock::now());
        auto s2 = app.mcpp({"build", "-v"});
        expect(fatal(e2e::succeeded(s2))) << "S2: the build failed";
        expect(s::lacks(s::text(s2), "declined")) << "S2: the fast path declined after a file of a package nested in the dependency changed";
        expect(s::lacks(s::text(s2), "Resolving toolchain")) << "S2: the build was planned instead of replayed";
    };
}};

}  // namespace
