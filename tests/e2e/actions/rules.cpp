// Rule packages: a collection whose rules a consumer selects by feature, the tools a rule may
// declare and when, the environment a rule brings, the sources a rule claims (formerly
// tests/e2e/610, 619, 630, 798, 808).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

// examples/12-a-new-device-language copied into the case, without build trees or locks
// (`find -maxdepth 3`).
fs::path example_copy() {
    const auto src = s::repository() / "examples/12-a-new-device-language";
    expect(fatal(fs::is_directory(src))) << src.string() << " is missing";
    const auto ex = s::case_dir() / "ex";
    fs::copy(src, ex, fs::copy_options::recursive);
    std::vector<fs::path> doomed;
    for (auto it = fs::recursive_directory_iterator(ex); it != fs::recursive_directory_iterator(); ++it) {
        const auto depth = it.depth() + 1;
        if (depth > 3) { it.disable_recursion_pending(); continue; }
        const auto name = it->path().filename();
        if ((it->is_directory() && name == "target") || name == "mcpp.lock") {
            doomed.push_back(it->path());
            if (it->is_directory()) it.disable_recursion_pending();
        }
    }
    for (const auto& d : doomed) fs::remove_all(d);
    return ex;
}

E2E rules{"rules", [] {
    "610 feature controlled rule collection"_test[s::gcc] = [] {
        // One package, several rules, the set chosen by the consumer's feature request: an
        // activated feature's unit is importable under its declared name, an inactive one is
        // not compiled; the mcpp namespace draws no reserved-prefix warning, another draws one
        // per unit; a lib whose only unit implements an undeclared module keeps its diagnostic.
        const auto tmp = s::case_dir();
        auto collection = [&](std::string_view ns) {
            fs::remove_all(tmp / "plugins");
            s::project p{tmp / "plugins", {{"src/plugins.cppm", "export module mcpp.plugins;\nimport std;\nexport namespace mcpp::plugins {\ninline constexpr std::string_view version = \"0.1.0\";\n}\n"},
                                           {"rules/a.cppm", R"cpp(export module mcpp.rules.a;
import std;
import mcpp;
import mcpp.plugins;   // the lib root precedes every feature unit
export namespace mcpp::rules::a {
inline void apply() {
    mcpp::cxxflag("-DRULE_A=1");
    // Unquoted on purpose: the token is stringized by the consumer, and a
    // quoted value would have to survive two layers of shell quoting.
    mcpp::cxxflag(std::format("-DPLUGINS_VERSION={}", mcpp::plugins::version).c_str());
}
}
)cpp"},
                                           {"rules/b.cppm", "export module mcpp.rules.b;\nimport std;\nimport mcpp;\nexport namespace mcpp::rules::b {\ninline void apply() { mcpp::cxxflag(\"-DRULE_B=1\"); }\n}\n"}}};
            p.write("mcpp.toml", std::format("[package]\nname      = \"plugins\"\nnamespace = \"{}\"\nversion   = \"0.1.0\"\n[language]\nstandard = \"c++23\"\n[build]\nsources = [\"src/plugins.cppm\"]\n[features]\nrules-a = {{ sources = [\"rules/a.cppm\"] }}\nrules-b = {{ sources = [\"rules/b.cppm\"] }}\n[targets.plugins]\nkind = \"lib\"\n", ns));
        };
        auto app = [&](std::string_view ns, std::string_view features, std::vector<std::string> imports) {
            fs::remove_all(tmp / "app");
            s::project a{tmp / "app", {{"src/main.cpp", R"cpp(#include <cstdio>
#ifndef RULE_A
#define RULE_A 0
#endif
#ifndef RULE_B
#define RULE_B 0
#endif
#ifndef PLUGINS_VERSION
#define PLUGINS_VERSION none
#endif
#define STR_(x) #x
#define STR(x) STR_(x)
int main() { std::printf("A=%d B=%d V=%s\n", RULE_A, RULE_B, STR(PLUGINS_VERSION)); }
)cpp"}}};
            a.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[language]\nstandard = \"c++23\"\n[dependencies.{}]\nplugins = {{ path = \"../plugins\", features = [{}], host-module = true }}\n", ns, features));
            std::string program = "import std;\nimport mcpp;\n";
            for (const auto& m : imports) program += std::format("import {};\n", m);
            program += "int main() {\n";
            for (const auto& m : imports) {
                std::string qualified;  // `${m//./::}`
                for (char c : m) qualified += c == '.' ? std::string("::") : std::string(1, c);
                program += std::format("    {}::apply();\n", qualified);
            }
            program += "}\n";
            a.write("build.mcpp", program);
            return a;
        };
        const std::string reserved = "belongs to the modules maintained by the mcpp project";
        collection("mcpp");
        auto a1 = app("mcpp", "\"rules-a\"", {"mcpp.rules.a"});
        auto b1 = a1.mcpp({"build"});
        expect(fatal(e2e::succeeded(b1))) << "a feature-selected rule did not build";
        expect(eq(s::last_starting(a1.mcpp({"run"}).out, "A="), std::string{"A=1 B=0 V=0.1.0"})) << "expected A=1 B=0 V=0.1.0";
        expect(s::lacks(s::text(b1), reserved)) << "the mcpp namespace drew a reserved-prefix warning";
        auto a2 = app("mcpp", "\"rules-a\"", {"mcpp.rules.b"});
        auto b2 = a2.mcpp({"build"});
        expect(fatal(failed(b2))) << "a rule whose feature is not active was importable";
        expect(e2e::contains(s::text(b2), "mcpp.rules.b")) << "the refusal does not name the module";
        auto a3 = app("mcpp", "\"rules-a\", \"rules-b\"", {"mcpp.rules.a", "mcpp.rules.b"});
        expect(fatal(e2e::succeeded(a3.mcpp({"build"})))) << "two feature-selected rules did not build";
        expect(eq(s::last_starting(a3.mcpp({"run"}).out, "A="), std::string{"A=1 B=1 V=0.1.0"})) << "expected A=1 B=1 V=0.1.0";
        collection("acme");
        auto a4 = app("acme", "\"rules-a\", \"rules-b\"", {"mcpp.rules.a", "mcpp.rules.b"});
        auto b4 = a4.mcpp({"build"});
        expect(fatal(e2e::succeeded(b4))) << "the acme collection did not build";
        expect(eq(s::count_lines(s::text(b4), reserved), std::size_t{3})) << "expected 3 reserved-prefix warnings (one per unit)";

        fs::remove_all(tmp / "plugins");
        fs::remove_all(tmp / "app");
        s::project{tmp / "plugins", {{"mcpp.toml", "[package]\nname    = \"plugins\"\nversion = \"0.1.0\"\n[build]\nsources = [\"rules/*.cppm\"]\n[targets.plugins]\nkind = \"lib\"\n"},
                                     {"rules/impl.cppm", "module nothing.exported;   // an implementation unit is not an interface\n"}}};
        s::project a5{tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[dependencies]\nplugins = { path = \"../plugins\", host-module = true }\n"},
                                    {"build.mcpp", "import mcpp;\nint main() {}\n"},
                                    {"src/main.cpp", "int main() {}\n"}}};
        auto b5 = a5.mcpp({"build"});
        expect(fatal(failed(b5))) << "a library whose only unit implements a module no unit declares built";
        expect(e2e::contains(s::text(b5), "nothing.exported")) << "the diagnostic does not name the module";
        expect(e2e::contains(s::text(b5), "rules/impl.cppm")) << "the diagnostic does not name the unit";
        expect(e2e::contains(s::text(b5), "the build programs' workspace")) << "the diagnostic does not say where to reproduce it";
    };

    "619 a tool may not be conditioned on a layer"_test[s::gcc] = [] {
        // `[target.<selector>.xlings…]` refuses a predicate naming a target-side layer, naming
        // the tool, the predicate and the feature-xlings way out.
        s::project p{s::files{{"mcpp.toml", "[package]\nname    = \"layer-gated-tool\"\nversion = \"0.1.0\"\n[target.'cfg(c-abi = \"musl\")'.xlings.workspace]\n\"xim:mcpp-e2e-no-such-tool\" = \"1.0\"\n"},
                      {"src/main.cpp", "int main() { return 0; }\n"}}};
        auto r = p.mcpp({"build"});
        expect(fatal(failed(r))) << "the build succeeded; a tool conditioned on a layer was accepted and then never installed";
        expect(e2e::contains(s::text(r), "xim:mcpp-e2e-no-such-tool")) << "the refusal does not name the tool";
        expect(e2e::contains(s::text(r), "c-abi = \"musl\"")) << "the refusal does not name the predicate";
        expect(e2e::contains(s::text(r), "feature-xlings")) << "the refusal does not name the gate form as the way out";
    };

    "630 a rule brings its own environment"_test[s::gcc] = [] {
        // A rule package declares the payloads its rule needs, gated by the accelerator and the
        // feature: a build naming no accelerator installs nothing; a device build installs the
        // payload and the rule's build program finds it.
        const auto tmp = s::case_dir();
        const s::options fresh{.where = s::home::fresh};
        s::project dep{tmp / "dep", {{"src/lib.cpp", "int dep_touch() { return 1; }\n"},
                                     {"build.mcpp", R"cpp(#include <cstdio>
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
)cpp"},
                                     {"mcpp.toml", "[package]\nname    = \"ruleowner\"\nversion = \"0.1.0\"\n[modules]\nsources = [\"src/**/*.cpp\"]\n[features]\ndefault   = []\nrules-toy = []\n[target.'cfg(accelerator = \"cuda\")'.feature-xlings.rules-toy]\n\"xim:zoxide\" = \">=0.9.9\"\n[targets.ruleowner]\nkind = \"lib\"\n"}}};
        s::project app{tmp / "app", {{"src/main.cpp", "int main() { return 0; }\n"},
                                     {"mcpp.toml", "[package]\nname         = \"consumer\"\nversion      = \"0.1.0\"\naccelerators = [\"cuda\"]\n[dependencies]\nruleowner = { path = \"../dep\", features = [\"rules-toy\"] }\n[targets.consumer]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}}};
        const auto store = s::case_home(s::home::fresh) / "registry/data/xpkgs/xim-x-zoxide";
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, fresh)))) << "the CPU-only build did not build";
        expect(fatal(!fs::is_directory(store))) << "a build that named no accelerator downloaded the device payload";
        expect(fatal(e2e::succeeded(app.mcpp({"build", "--accel", "cuda12.9+{sm_89}"}, fresh)))) << "the device build did not build";
        expect(fatal(fs::is_directory(store))) << "the rule's payload was never installed";
        expect(fatal(dep.exists("answered.txt") && !dep.read("answered.txt").empty())) << "the rule's build program recorded nothing";
        expect(e2e::contains(dep.read("answered.txt"), "xim-x-zoxide")) << "the rule cannot find the payload it declared: " << dep.read("answered.txt");
    };

    "798 a rule applies through a source it claims"_test[s::gcc] = [] {
        // An active rule applies to a consumer through the sources it claims (mcpp#715): the
        // example's app gets a synthesised build program and its kernel; a package that only
        // imports the rule's module gets neither a report nor a build program.
        const auto ex = example_copy();
        s::project app{ex / "app", {}};
        app.remove("build.mcpp");
        auto b1 = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(b1))) << "leg 1 did not build";
        expect(fatal(s::has_line(s::text(b1), "Rules.*example.rules.toy"))) << "leg 1 did not report the rule it applied";
        expect(fatal(s::find_file(app.at("target"), "build.mcpp").has_value())) << "leg 1 synthesised no build program";
        expect(fatal(e2e::contains(s::last_line(app.mcpp({"run"}).out), "= 42"))) << "leg 1: the kernel did not reach the program";
        s::project only{ex / "importonly", {{"mcpp.toml", "[package]\nname    = \"importonly\"\nversion = \"0.1.0\"\n\n[language]\nstandard = \"c++23\"\n\n[dependencies]\nexample.rules-toy = { path = \"../rules-toy\", features = [\"rules-toy\"] }\n\n[build]\nsources = [\"src/*.cpp\"]\n"},
                                            {"src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"IMPORT_ONLY_OK\\n\"); }\n"}}};
        auto b2 = only.mcpp({"build"});
        expect(fatal(e2e::succeeded(b2))) << "leg 2 did not build";
        expect(s::lacks(s::text(b2), "Rules")) << "leg 2 reported a rule that claims none of its sources";
        expect(!s::find_file(only.at("target"), "build.mcpp").has_value()) << "leg 2 synthesised a build program with nothing to do";
        expect(eq(s::last_line(only.mcpp({"run"}).out), std::string{"IMPORT_ONLY_OK"})) << "leg 2 program";
    };

    "808 device source is not a compile unit"_test[s::gcc] = [] {
        // A rule-claimed device source is no C++ compile unit of the plan: no cxx_object edge,
        // no entry in compile_commands.json, in the build database or its compile-commands
        // spelling; the rule still compiles it and the program runs.
        const auto ex = example_copy();
        s::project app{ex / "app", {}};
        const s::options cc{.env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};
        expect(fatal(e2e::succeeded(app.mcpp({"build", "--configure-only"}, cc)))) << "configure-only did not succeed";
        std::optional<fs::path> nj;
        for (const auto& f : s::find_files(app.at("target"), R"(build\.ninja)"))
            if (!nj && f.generic_string().find("/.build-mcpp/") == std::string::npos) nj = f;
        expect(fatal(nj.has_value())) << "no build.ninja generated";
        expect(s::no_line(s::read_file(*nj), R"(^build [^:]*: cxx_object [^|]*answer\.toy)")) << "A: build.ninja carries a cxx_object edge for the device source";
        expect(fatal(app.exists("compile_commands.json"))) << "no compile_commands.json";
        expect(s::lacks(app.read("compile_commands.json"), "answer.toy")) << "B: compile_commands.json lists the device source";
        auto s1 = app.mcpp({"emit", "build-database", "--format", "json"}, cc);
        expect(fatal(e2e::succeeded(s1))) << "emit build-database failed";
        std::string listed;
        for (const auto& set : s::json(s1.out)["data"]["database"]["sets"])
            for (const auto& u : set["translation-units"])
                if (u["source"].get<std::string>().ends_with("answer.toy")) listed += set["name"].get<std::string>() + " ";
        expect(listed.empty()) << "C: device source listed in set " << listed;
        auto cdb = app.mcpp({"emit", "build-database", "--spec", "compile-commands"}, cc);
        expect(fatal(e2e::succeeded(cdb))) << "emit --spec compile-commands failed";
        expect(s::lacks(cdb.out, "answer.toy")) << "D: --spec compile-commands lists the device source";
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, cc)))) << "E: the build failed";
        expect(e2e::contains(s::last_line(app.mcpp({"run"}, cc).out), "= 42")) << "E: the kernel did not reach the program";
    };
}};

}  // namespace
