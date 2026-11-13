// Feature activation: `backend=` sugar and the --strict schema gate, required_features,
// feature defines and their propagation, capabilities, feature-deps, a dependency's
// required_features, exclusive capabilities, a feature-deps shared
// library, restated feature-deps, a dependency's feature on the command line, a private
// feature-dep, capabilities bound in each consumer context, a later edge walking the
// feature-deps it activates (formerly tests/e2e/67, 71, 80, 81, 82, 83, 308, 601, 615, 711, 714,
// 739, 1012, 1019).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

const s::options cdb{.env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};

// `find target \( -name app -o -name app.exe \) -type f | head -1`, run.
e2e::outcome run_app(const s::project& p) {
    const auto bin = p.artifact(s::exe("app"));
    expect(fatal(bin.has_value())) << "no app binary";
    return s::run(*bin);
}

E2E activation{"activation", [] {
    "67 features strict"_test = [] {
        // Feature activation, `backend=` sugar, and the --strict schema gate: an unknown feature
        // or platform warns, and --strict refuses; the six platform names are known.
        const auto tmp = temp_dir();
        s::project{tmp / "widget", {{"mcpp.toml", "[package]\nname    = \"widget\"\nversion = \"0.1.0\"\n\n[targets.widget]\nkind = \"lib\"\n\n[features]\ndefault   = []\nbackend-a = []\nbackend-b = []\n"},
                                    {"src/widget.cppm", "export module widget;\nexport int widget_backend() {\n#if defined(MCPP_FEATURE_BACKEND_A)\n    return 1;\n#elif defined(MCPP_FEATURE_BACKEND_B)\n    return 2;\n#else\n    return 0;\n#endif\n}\n"}}};
        s::project app{tmp / "app", {{"src/main.cpp", "import std;\nimport widget;\nint main() {\n#ifdef MCPP_FEATURE_BASE\n    std::println(\"base\");\n#endif\n#ifdef MCPP_FEATURE_EXTRA\n    std::println(\"extra\");\n#endif\n    std::println(\"backend={}\", widget_backend());\n    return 0;\n}\n"}}};
        auto manifest = [&](std::string_view platforms, std::string_view backend) {
            app.write("mcpp.toml", std::format("[package]\nname      = \"app\"\nversion   = \"0.1.0\"\nplatforms = [{}]\n\n[features]\ndefault = [\"base\"]\nbase    = []\nextra   = []\n\n[dependencies]\nwidget = {{ path = \"../widget\", backend = \"{}\" }}\n", platforms, backend));
        };
        const std::string three = "\"linux\", \"macos\", \"windows\"";
        manifest(three, "a");
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "build failed";
        expect(e2e::contains(run_app(app).out, "backend=1")) << "backend=a not activated";
        app.remove("target");
        expect(fatal(e2e::succeeded(app.mcpp({"build", "--features", "extra"})))) << "features build failed";
        expect(e2e::contains(run_app(app).out, "extra")) << "--features extra not active";
        app.remove("target");
        auto b3 = app.mcpp({"build", "--features", "nosuch"});
        expect(fatal(e2e::succeeded(b3))) << "warn path must not fail";
        expect(e2e::contains(s::text(b3), "does not declare")) << "missing unknown-feature warning";
        auto b4 = app.mcpp({"build", "--features", "nosuch", "--strict"});
        expect(fatal(failed(b4))) << "--strict must fail on unknown feature";
        expect(e2e::contains(s::text(b4), "does not declare")) << "missing strict error text";
        manifest(three, "zzz");
        app.remove("target");
        auto b5 = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(b5))) << "unknown-backend warn path must not fail";
        expect(e2e::contains(s::text(b5), "does not declare requested feature 'backend-zzz'")) << "missing unknown-backend warning";
        manifest("\"linux\", \"amiga\"", "a");
        app.remove("target");
        auto b6 = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(b6))) << "platform warn path must not fail";
        expect(e2e::contains(s::text(b6), "unknown platform 'amiga'")) << "missing platform warning";
        auto b7 = app.mcpp({"build", "--strict"});
        expect(fatal(failed(b7))) << "--strict must fail on unknown platform";
        expect(e2e::contains(s::text(b7), "expected: linux | macos | windows | ios | android | emscripten")) << "the refusal must list the six platform names";
        manifest("\"linux\", \"ios\", \"android\", \"emscripten\"", "a");
        app.remove("target");
        auto b8 = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(b8))) << "a manifest naming the six platforms must build";
        expect(s::lacks(s::text(b8), "unknown platform")) << "a known platform name was reported as unknown";
        manifest("\"web\"", "a");
        app.remove("target");
        auto b9 = app.mcpp({"build", "--strict"});
        expect(fatal(failed(b9))) << "--strict must fail on 'web': the word is emscripten";
        expect(e2e::contains(s::text(b9), "unknown platform 'web'")) << "missing platform refusal for 'web'";
    };

    "71 required features gate"_test[tag{"requires:gcc"}] = [] {
        // A target is emitted only when all its required features are active.
        s::needs({"gcc"});
        s::project p{s::files{{"src/common.cpp", "extern \"C\" int shared_answer(void) { return 0; }\n"},
                      {"bin/server.cpp", "extern \"C\" int shared_answer(void);\nint main() { return shared_answer(); }\n"},
                      {"bin/client.cpp", "extern \"C\" int shared_answer(void);\nint main() { return shared_answer(); }\n"},
                      {"mcpp.toml", "[package]\nname    = \"proj\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/**/*.cpp\"]\n\n[features]\nserver = []\nclient = []\n\n[targets.server]\nkind = \"bin\"\nmain = \"bin/server.cpp\"\nrequired_features = [\"server\"]\n\n[targets.client]\nkind = \"bin\"\nmain = \"bin/client.cpp\"\nrequired_features = [\"client\"]\n"}}};
        auto built = [&](std::string_view name) { return !s::glob(p.at("target"), std::format("*/*/bin/{}", name)).empty(); };
        expect(fatal(e2e::succeeded(p.mcpp({"build", "--features", "server"})))) << "server build failed";
        expect(built("server")) << "server binary not built under --features server";
        expect(!built("client")) << "client binary should be gated out under --features server";
        expect(fatal(e2e::succeeded(p.mcpp({"build", "--features", "client"})))) << "client build failed";
        expect(built("client")) << "client binary not built under --features client";
    };

    "80 feature defines"_test = [] {
        // A feature's `defines` land on the package's compile flags when it is active only.
        s::project app{s::files{{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[features]\ndefault = []\naccel   = { defines = [\"APP_ACCEL=1\"] }\n"},
                        {"src/main.cpp", "int main() {\n#ifdef APP_ACCEL\n    return 0;\n#else\n    return 1;\n#endif\n}\n"}}};
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, cdb)))) << "baseline build failed";
        expect(fatal(s::lacks(app.read("compile_commands.json"), "APP_ACCEL"))) << "APP_ACCEL present without the feature active";
        app.remove("target");
        app.remove("compile_commands.json");
        expect(fatal(e2e::succeeded(app.mcpp({"build", "--features", "accel"}, cdb)))) << "feature build failed";
        expect(e2e::contains(app.read("compile_commands.json"), "APP_ACCEL")) << "APP_ACCEL missing with --features accel";
        expect(e2e::contains(app.read("compile_commands.json"), "MCPP_FEATURE_ACCEL")) << "MCPP_FEATURE_ACCEL missing";
    };

    "81 capability binding"_test = [] {
        // A feature's `requires` binds exactly one provider of the graph: none is an error, one
        // binds, two unpinned are an error listing both; [capabilities] and --cap pin one; an
        // inactive feature requires nothing.
        const auto tmp = temp_dir();
        for (std::string_view impl : {"alpha", "beta"}) {
            s::project blas{tmp / std::format("blas_{}", impl), {}};
            blas.write("mcpp.toml", std::format("[package]\nname    = \"blas_{0}\"\nversion = \"0.1.0\"\nprovides = [\"blas\"]\n\n[targets.blas_{0}]\nkind = \"lib\"\n", impl));
            blas.write(std::format("src/blas_{}.cppm", impl), std::format("export module blas_{0};\nexport int blas_{0}() {{ return 0; }}\n", impl));
        }
        s::project app{tmp / "app", {{"src/main.cpp", "int main() { return 0; }\n"}}};
        auto manifest = [&](std::string_view deps) {
            app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[features]\ndefault  = []\nuse_blas = {{ requires = [\"blas\"] }}\n\n[dependencies]\n{}\n", deps));
            app.remove("target");
        };
        const std::string both = "blas_alpha = { path = \"../blas_alpha\" }\nblas_beta  = { path = \"../blas_beta\" }";
        manifest("blas_alpha = { path = \"../blas_alpha\" }");
        expect(e2e::succeeded(app.mcpp({"build", "--features", "use_blas"}))) << "case1: single provider should bind";
        manifest("# no provider");
        auto c2 = app.mcpp({"build", "--features", "use_blas"});
        expect(fatal(failed(c2))) << "case2: missing provider must error";
        expect(s::has_line_nocase(s::text(c2), "no package provides .*blas|capability 'blas'")) << "case2: wrong/no error text";
        manifest(both);
        auto c3 = app.mcpp({"build", "--features", "use_blas"});
        expect(fatal(failed(c3))) << "case3: ambiguous providers must error";
        expect(s::contains_nocase(s::text(c3), "blas_alpha") && s::contains_nocase(s::text(c3), "blas_beta")) << "case3: error must list candidates";
        app.write("mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[features]\ndefault  = []\nuse_blas = { requires = [\"blas\"] }\n\n[capabilities]\nblas = \"blas_beta\"\n\n[dependencies]\nblas_alpha = { path = \"../blas_alpha\" }\nblas_beta  = { path = \"../blas_beta\" }\n");
        app.remove("target");
        expect(e2e::succeeded(app.mcpp({"build", "--features", "use_blas"}))) << "case4: pin should resolve ambiguity";
        manifest("# no provider");
        expect(e2e::succeeded(app.mcpp({"build"}))) << "case5: inactive feature must not require capability";
        manifest(both);
        expect(e2e::succeeded(app.mcpp({"build", "--features", "use_blas", "--cap", "blas=blas_alpha"}))) << "case6: --cap pin should resolve ambiguity";
    };

    "82 feature optional deps"_test = [] {
        // A dependency declared under a feature is pulled only when the feature is active.
        const auto tmp = temp_dir();
        s::project{tmp / "widget", {{"mcpp.toml", "[package]\nname    = \"widget\"\nversion = \"0.1.0\"\n\n[targets.widget]\nkind = \"lib\"\n"},
                                    {"src/widget.cppm", "export module widget;\nexport int widget_answer() { return 42; }\n"}}};
        s::project app{tmp / "app", {{"src/main.cpp", "int main() { return 0; }\n"},
                                     {"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[features]\ndefault = []\nextra   = []\n\n# `widget` is declared ONLY under the feature, so it is optional: resolved only\n# when --features extra is active.\n[feature-deps.extra]\nwidget = { path = \"../widget\" }\n"}}};
        auto b1 = app.mcpp({"build", "-v"});
        expect(fatal(e2e::succeeded(b1))) << "baseline build failed";
        expect(s::no_line(s::text(b1), "Compiling widget ")) << "widget must NOT be pulled when feature inactive";
        app.remove("target");
        auto b2 = app.mcpp({"build", "-v", "--features", "extra"});
        expect(fatal(e2e::succeeded(b2))) << "feature build failed (widget not pulled?)";
        expect(s::has_line(s::text(b2), "Compiling widget ")) << "widget was not pulled when feature active";
    };

    "83 feature defines propagate"_test = [] {
        // A dependency's active-feature `defines` reach the consumer's own translation units.
        const auto tmp = temp_dir();
        s::project{tmp / "widget", {{"mcpp.toml", "[package]\nname    = \"widget\"\nversion = \"0.1.0\"\n\n[features]\ndefault = []\nturbo   = { defines = [\"WIDGET_TURBO=1\"] }\n\n[build]\ninclude_dirs = [\"include\"]\n\n[targets.widget]\nkind = \"lib\"\n"},
                                    {"include/widget/widget.hpp", "#pragma once\n// The macro's value is only meaningful in the TU that includes this header —\n// exactly the header-only library situation.\ninline int widget_mode() {\n#ifdef WIDGET_TURBO\n    return 1;\n#else\n    return 0;\n#endif\n}\n"},
                                    {"src/widget.cppm", "export module widget;\nexport int widget_anchor() { return 0; }\n"}}};
        s::project app{tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nwidget = { path = \"../widget\", features = [\"turbo\"] }\n"},
                                     {"src/main.cpp", "#include <widget/widget.hpp>\n#ifndef WIDGET_TURBO\n#error \"WIDGET_TURBO did not propagate from widget[turbo] to the consumer\"\n#endif\nint main() { return widget_mode() == 1 ? 0 : 2; }\n"}}};
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, cdb)))) << "feature define did not propagate to consumer";
        expect(e2e::contains(app.read("compile_commands.json"), "WIDGET_TURBO")) << "WIDGET_TURBO missing from consumer compile_commands.json";
        expect(e2e::succeeded(run_app(app))) << "binary did not observe WIDGET_TURBO";
    };
    "308 dependency required features"_test[tag{"requires:elf"}] = [] {
        // A dependency's `[targets.<n>] required_features` gates its target for every consumer:
        // closed without the feature (the program still runs), open with it.
        s::needs({"elf"});
        const auto tmp = temp_dir();
        s::project{tmp / "dep", {{"mcpp.toml", R"cpp([package]
name    = "dep"
version = "0.1.0"
[build]
c_standard = "c11"
sources    = ["src/*.c"]
[features]
plugin = []
[targets.dep]
kind = "lib"
[targets.depshared]
kind = "shared"
required_features = ["plugin"]
)cpp"}, {"src/dep.c", R"cpp(int dep_value(void) { return 5; }
)cpp"}}};
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"
[build]
c_standard = "c11"
[targets.app]
kind = "bin"
main = "src/main.c"
[dependencies]
dep = { path = "../dep" }
)cpp"}, {"src/main.c", R"cpp(#include <stdio.h>
int dep_value(void);
int main(void) { printf("%d\n", dep_value()); return 0; }
)cpp"}}};
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "the feature-off build failed";
        expect(fatal(s::find_files(app.at("target"), R"(libdepshared\.so)").empty())) << "a feature-gated dependency target was built with the feature off";
        const auto bin = s::find_file(app.at("target"), "app");
        expect(fatal(bin.has_value())) << "no executable";
        expect(fatal(s::has_line(s::run(*bin).out, "^5$"))) << "the gated-off build does not run";
        app.write("mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"
[build]
c_standard = "c11"
[targets.app]
kind = "bin"
main = "src/main.c"
[dependencies]
dep = { path = "../dep", features = ["plugin"] }
)cpp");
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "the feature-on build failed";
        expect(s::find_file(app.at("target"), "libdepshared.so").has_value()) << "requesting the feature did not build the gated target";
    };

    "601 exclusive capability"_test = [] {
        // Two exclusive providers of one capability are refused at binding, naming it, both and
        // why; two ordinary providers coexist.
        const auto parent = temp_dir() / "new";
        fs::create_directories(parent);
        auto mkpkg = [&](std::string_view name, std::string_view cap, bool exclusive) {
            s::project p{parent / name, {}};
            p.write("mcpp.toml", std::format("[package]\nname    = \"{0}\"\nversion = \"0.1.0\"\nprovides = [\"{1}\"]\n{2}[language]\nstandard = \"c++23\"\n[targets.{0}]\nkind = \"lib\"\n", name, cap, exclusive ? std::format("exclusive = [\"{}\"]\n", cap) : std::string{}));
            std::string ident{name};
            std::ranges::replace(ident, '-', '_');
            p.write(std::format("src/{}.c", name), std::format("int {}_anchor(void) {{ return 0; }}\n", ident));
        };
        auto consumer = [&](std::vector<std::string_view> deps) {
            fs::remove_all(parent / "app");
            auto app = s::project::scaffold("app");
            std::string m = "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[language]\nstandard = \"c++23\"\n[dependencies]\n";
            for (auto d : deps) m += std::format("{0} = {{ path = \"../{0}\" }}\n", d);
            app.write("mcpp.toml", m);
            for (const auto& f : s::glob(app.at("src"), "*.cppm")) fs::remove(f);
            app.write("src/main.cpp", "int main(){return 0;}\n");
            return app;
        };
        mkpkg("gpublas-a", "gpu-blas", true);
        mkpkg("gpublas-b", "gpu-blas", true);
        auto out = consumer({"gpublas-a", "gpublas-b"}).mcpp({"build"});
        expect(fatal(failed(out))) << "two exclusive providers of 'gpu-blas' were accepted";
        expect(e2e::contains(s::text(out), "gpu-blas")) << "refusal does not name the capability";
        expect(e2e::contains(s::text(out), "gpublas-a")) << "refusal does not name the first provider";
        expect(e2e::contains(s::text(out), "gpublas-b")) << "refusal does not name the second provider";
        expect(s::contains_nocase(s::text(out), "exclusive")) << "refusal does not say why";
        mkpkg("blas-a", "blas", false);
        mkpkg("blas-b", "blas", false);
        expect(e2e::succeeded(consumer({"blas-a", "blas-b"}).mcpp({"build"}))) << "two ordinary providers were refused";
    };
    "615 feature deps shared library is linked"_test[tag{"requires:gcc"}][tag{"requires:unix-shell"}] = [] {
        // A shared library a library package acquires through [feature-deps] reaches the link
        // line of its test binaries; the project builds without the feature too.
        s::needs({"gcc", "unix-shell"});
        const auto tmp = temp_dir();
        s::project{tmp / "greeter", {{"mcpp.toml", R"cpp([package]
name    = "greeter"
version = "0.1.0"

[build]
sources = ["src/greeter.cpp"]

[targets.greeter]
kind = "shared"
)cpp"}, {"src/greeter.cpp", R"cpp(extern "C" const char* greeter_line() { return "greeter-from-the-shared-library"; }
)cpp"}}};
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"

[build]
sources = ["src/lib.cpp"]

[targets.app]
kind = "lib"

[features]
default = {}
loud    = { defines = ["USE_GREETER"] }

[feature-deps.loud]
greeter = { path = "../greeter" }
)cpp"}, {"src/lib.cpp", R"cpp(int app_anchor() { return 0; }
)cpp"}, {"tests/use.cpp", R"cpp(#include <cstdio>
#ifdef USE_GREETER
extern "C" const char* greeter_line();
#endif
int main() {
#ifdef USE_GREETER
    std::printf("%s\n", greeter_line());
#else
    std::printf("quiet\n");
#endif
    return 0;
}
)cpp"}}};
        const auto loud = s::text(app.mcpp({"test", "use", "--features", "loud"}));
        app.remove("target");
        const auto quiet = s::text(app.mcpp({"test", "use"}));
        expect(e2e::contains(loud, "greeter-from-the-shared-library")) << "the feature's shared library links and runs";
        expect(e2e::contains(quiet, "quiet")) << "and the same project builds without the feature";
    };
    "711 a feature deps restatement names one source"_test[tag{"requires:gcc"}] = [] {
        // A [feature-deps.<f>] entry adding `tools` to a dependency declared unconditionally
        // restates its source; another source (or constraint) is refused naming both; a build
        // program addresses a namespaced package's tool by both spellings, only with the feature.
        s::needs({"gcc"});
        const auto tmp = temp_dir();
        const s::options linked{.where = s::home::linked};
        for (std::string_view d : {"installer", "other"})
            s::project{tmp / d, {{"mcpp.toml", R"cpp([package]
name      = "installer"
namespace = "spike"
version   = "0.1.0"

[targets.installer]
kind = "bin"
main = "src/main.cpp"
)cpp"}, {"src/main.cpp", R"cpp(#include <cstdio>
int main() { std::puts("installer ran"); return 0; }
)cpp"}}};
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"

[dependencies]
spike.installer = { path = "../installer" }

[features]
installer = []

[feature-deps.installer]
spike.installer = { tools = ["installer"] }
)cpp"}, {"src/main.cpp", "int main() { return 0; }\n"}, {"build.mcpp", R"cpp(import std;
import mcpp;
int main() {
    const char* a = mcpp::dep_bin("installer", "installer");
    const char* b = mcpp::dep_bin("spike.installer", "installer");
    mcpp::warning((std::string("DEPBIN short=[") + (a ? a : "") + "] qualified=["
                   + (b ? b : "") + "]").c_str());
    return 0;
}
)cpp"}}};
        auto replace = [&](std::string_view from, std::string_view to) {
            auto m = app.read("mcpp.toml");
            expect(fatal(m.find(from) != std::string::npos)) << "the fixture line is missing: " << from;
            m.replace(m.find(from), from.size(), to);
            app.write("mcpp.toml", m);
        };
        auto b1 = app.mcpp({"build", "--features", "installer"}, linked);
        expect(fatal(failed(b1))) << "a source-less [feature-deps] entry was accepted";
        expect(fatal(e2e::contains(s::text(b1), "source"))) << "the refusal does not say to restate the source";
        replace("spike.installer = { tools = [\"installer\"] }", "spike.installer = { path = \"../installer\", tools = [\"installer\"] }");
        auto b2 = app.mcpp({"build", "--strict", "--features", "installer"}, linked);
        expect(fatal(e2e::succeeded(b2))) << "the restated feature-deps entry was refused under --strict";
        expect(fatal(s::has_line(s::text(b2), R"(DEPBIN short=\[[^\]])"))) << "dep_bin(\"installer\") read nothing";
        expect(fatal(s::has_line(s::text(b2), R"(qualified=\[[^\]])"))) << "dep_bin(\"spike.installer\") read nothing for a namespace + name package";
        app.remove("target");
        auto b3 = app.mcpp({"build", "--strict"}, linked);
        expect(fatal(e2e::succeeded(b3))) << "the build without the feature failed";
        expect(fatal(e2e::contains(s::text(b3), "DEPBIN short=[] qualified=[]"))) << "the tool was provided without its feature";
        replace("spike.installer = { path = \"../installer\", tools", "spike.installer = { path = \"../other\", tools");
        app.remove("target");
        auto b4 = app.mcpp({"build", "--strict", "--features", "installer"}, linked);
        expect(fatal(failed(b4))) << "a restatement naming another path was accepted";
        expect(fatal(e2e::contains(s::text(b4), "../other") && e2e::contains(s::text(b4), "../installer"))) << "the refusal does not name both sources";
        s::project spell{tmp / "spell", {{"src/main.cpp", "int main() { return 0; }\n"}}};
        const s::options offline{.where = s::home::linked, .env = {{"MCPP_OFFLINE", "1"}}};
        auto write_spell = [&](std::string_view restated) { spell.write("mcpp.toml", std::format(R"cpp([package]
name        = "spellprobe"
version     = "0.1.0"
description = "two spellings of one constraint"
license     = "Apache-2.0"
authors     = ["mcpp"]

[language]
standard = "c++23"

[features]
extra = []

[dependencies]
mcpplibs.nonexistent-spell-probe = ">=9.9.9"

[feature-deps.extra]
mcpplibs.nonexistent-spell-probe = {{ version = "{0}", reexport = true }}

[targets.spellprobe]
kind = "bin"
main = "src/main.cpp"
)cpp", restated)); };
        write_spell(">= 9.9.9");
        auto s1 = spell.mcpp({"build", "--features", "extra"}, offline);
        expect(fatal(s::lacks(s::text(s1), "restates the dependency"))) << "a restatement differing from the declaration only in whitespace was refused";
        write_spell(">=9.9.8");
        spell.remove("target");
        auto s2 = spell.mcpp({"build", "--features", "extra"}, offline);
        expect(fatal(failed(s2))) << "a restatement naming another constraint was accepted";
        expect(e2e::contains(s::text(s2), "restates the dependency")) << "a restatement naming another constraint was not refused";
    };

    "714 features names a dependency feature"_test[tag{"requires:gcc"}] = [] {
        // `--features <dependency>/<feature>` opens a feature of a dependency (no root macro), a
        // plain name without a [features] table is accepted, an unknown dependency is refused
        // under --strict; beside a [features] table too, and in `mcpp why deps`.
        s::needs({"gcc"});
        const auto tmp = temp_dir();
        const s::options linked{.where = s::home::linked};
        s::project{tmp / "fw", {{"mcpp.toml", R"cpp([package]
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
)cpp"}, {"src/fw.cpp", "int fw_answer() { return 42; }\n"}, {"tool/mcpp.toml", R"cpp([package]
name      = "fw-installer"
namespace = "spike"
version   = "0.1.0"

[targets.fw-installer]
kind = "bin"
main = "src/main.cpp"
)cpp"}, {"tool/src/main.cpp", "int main() { return 0; }\n"}}};
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"

[dependencies]
spike.fw = { path = "../fw" }
)cpp"}, {"src/main.cpp", R"cpp(int fw_answer();
int main() { return fw_answer() == 42 ? 0 : 1; }
)cpp"}, {"build.mcpp", R"cpp(import std;
import mcpp;
int main() {
    const char* a = mcpp::dep_bin("spike.fw-installer", "fw-installer");
    mcpp::warning((std::string("DEPBIN tool=[") + (a ? a : "") + "]").c_str());
    return 0;
}
)cpp"}}};
        auto b1 = app.mcpp({"build", "--features", "spike.fw/installer"}, linked);
        expect(fatal(e2e::succeeded(b1))) << "the dependency feature build failed";
        expect(fatal(s::has_line(s::text(b1), R"(DEPBIN tool=\[[^\]])"))) << "--features spike.fw/installer did not open the feature";
        std::optional<fs::path> nj;
        for (const auto& f : s::find_files(app.at("target"), R"(build\.ninja)"))
            if (!nj && f.generic_string().find("/.build-mcpp/") == std::string::npos) nj = f;
        expect(fatal(nj.has_value())) << "no build.ninja";
        expect(fatal(s::lacks(s::read_file(*nj), "MCPP_FEATURE_SPIKE_FW_INSTALLER"))) << "the dependency token became a macro of the root";
        app.remove("target");
        expect(fatal(e2e::succeeded(app.mcpp({"build", "--strict", "--features", "nothing-here"}, linked)))) << "a plain name without a [features] table was refused";
        auto b3 = app.mcpp({"build", "--strict", "--features", "nope/x"}, linked);
        expect(fatal(failed(b3))) << "--features nope/x was accepted under --strict";
        expect(fatal(e2e::contains(s::text(b3), "nope"))) << "the refusal does not name the token";
        app.write("mcpp.toml", app.read("mcpp.toml") + R"cpp(
[features]
windows-installer = []
)cpp");
        app.remove("target");
        auto b4 = app.mcpp({"build", "--strict", "--features", "spike.fw/installer"}, linked);
        expect(fatal(e2e::succeeded(b4))) << "the dependency token was refused beside a [features] table";
        expect(fatal(s::has_line(s::text(b4), R"(DEPBIN tool=\[[^\]])"))) << "the feature did not open beside a [features] table";
        auto w1 = app.mcpp({"why", "deps", "--features", "spike.fw/installer"}, linked);
        expect(fatal(e2e::succeeded(w1))) << "why deps --features failed";
        expect(e2e::contains(s::text(w1), "spike.fw-installer")) << "why deps does not show the feature's dependency";
    };

    "739 a private feature dep does not reach the consumer"_test = [] {
        // A platform SDK under [feature-deps.<f>] with visibility = "private" reaches only the
        // declaring package's own units: the consumer fails on the missing header, the declaring
        // package builds, and with "public" the consumer builds (the harness is not vacuous).
        const auto tmp = temp_dir();
        s::project{tmp / "sdk", {{"mcpp.toml", R"cpp([package]
name    = "sdk"
version = "0.1.0"

[targets.sdk]
kind = "lib"
)cpp"}, {"include/sdk.h", R"cpp(#pragma once
#define SDK_PRESENT 1
inline int sdk_value(void) { return 42; }
)cpp"}, {"src/empty.cpp", R"cpp(// no compiled sources of its own beyond the header
)cpp"}}};
        s::project lib{tmp / "lib_platform", {{"src/lib_platform.cpp", R"cpp(#include "sdk.h"
#ifndef SDK_PRESENT
#error "lib_platform's own translation unit must see sdk's headers"
#endif
int lib_platform_uses_sdk(void) { return sdk_value(); }
)cpp"}}};
        auto manifest = [&](std::string_view visibility) { lib.write("mcpp.toml", std::format(R"cpp([package]
name    = "lib_platform"
version = "0.1.0"

[targets.lib_platform]
kind = "lib"

[features]
plat = {{}}

[feature-deps.plat]
sdk = {{ path = "../sdk", visibility = "{0}" }}
)cpp", visibility)); };
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"

[dependencies]
lib_platform = { path = "../lib_platform", features = ["plat"] }
)cpp"}, {"src/main.cpp", R"cpp(#include "sdk.h"
int main() { return 0; }
)cpp"}}};
        manifest("private");
        auto a1 = app.mcpp({"build", "--cache", "off"});
        expect(fatal(failed(a1))) << "A: app built despite depending (via a private feature-dep) on a package whose headers it must not see";
        const auto a1_text = s::text(a1);
        const auto at_sdk = a1_text.find("sdk.h");
        const bool missing = at_sdk != std::string::npos
            && std::ranges::any_of(std::array{"No such file", "not found", "cannot open"},
                                   [&](const char* m) { return a1_text.find(m, at_sdk) != std::string::npos; });
        expect(fatal(missing)) << "A: app's build failed, but not on a missing sdk.h — cannot tell privacy from an unrelated break";
        expect(fatal(e2e::succeeded(lib.mcpp({"build", "--features", "plat", "--cache", "off"})))) << "A: lib_platform (the declaring package) could not build against its own private feature-dep";
        manifest("public");
        expect(e2e::succeeded(app.mcpp({"build", "--cache", "off"}))) << "B: app failed to build once sdk is a PUBLIC feature-dep of lib_platform";
    };
    "1012 capabilities bind in each consumer context"_test[tag{"requires:gcc"}][tag{"requires:unix-shell"}][tag{"requires:python3"}] = [] {
        // Independent products may select different providers; a library requirement still
        // accepts a provider supplied by its final application's sibling edge, and each refusal
        // names the consumer whose closure it is about.
        s::needs({"gcc", "unix-shell", "python3"});
        const s::options o{.where = s::home::inherited};
        s::project ws{s::files{{"mcpp.toml", R"cpp([workspace]
members = ["app-a", "app-b", "helper", "provider-a", "provider-b"]
)cpp"}}};
        for (std::string_view suffix : {"a", "b"}) {
            ws.write(std::format("provider-{}/mcpp.toml", suffix), std::format(R"cpp([package]
name = "provider-{0}"
version = "0.1.0"
provides = ["probe-interface"]
exclusive = ["probe-interface"]
[targets.provider-{0}]
kind = "lib"
)cpp", suffix));
            ws.write(std::format("provider-{}/src/anchor.c", suffix), std::format("int provider_{}(void){{return 0;}}\n", suffix));
            ws.write(std::format("app-{}/mcpp.toml", suffix), std::format(R"cpp([package]
name = "app-{0}"
version = "0.1.0"
[dependencies]
provider-{0} = {{ path = "../provider-{0}" }}
helper = {{ path = "../helper" }}
[targets.app-{0}]
kind = "bin"
main = "src/main.cpp"
)cpp", suffix));
            ws.write(std::format("app-{}/src/main.cpp", suffix), "int main(){return 0;}\n");
        }
        ws.write("helper/mcpp.toml", R"cpp([package]
name = "helper"
version = "0.1.0"
[features]
default = ["need-provider"]
need-provider = { requires = ["probe-interface"] }
[targets.helper]
kind = "lib"
)cpp");
        ws.write("helper/src/anchor.c", "int helper_anchor(void){return 0;}\n");
        // sed's line edits: the lines starting with `prefix` dropped, or `line` after the first.
        auto drop = [&](std::string_view file, std::string_view prefix) {
            std::istringstream in{ws.read(file)};
            std::string out;
            for (std::string l; std::getline(in, l);)
                if (!l.starts_with(prefix)) out += l + "\n";
            ws.write(file, out);
        };
        auto replace = [&](std::string_view file, std::string_view from, std::string_view to) {
            auto m = ws.read(file);
            const auto at = m.find(from);
            expect(fatal(at != std::string::npos)) << file << " lacks " << from;
            m.replace(at, from.size(), to);
            ws.write(file, m);
        };
        auto build = [&](std::vector<std::string> args, std::string_view in = {}) {
            return in.empty() ? ws.mcpp(std::move(args), o) : ws.mcpp_in(in, std::move(args), o);
        };

        auto r = build({"build", "--workspace"});
        expect(fatal(e2e::succeeded(r))) << "independent exclusive providers collided\n" << s::text(r);
        expect(fatal(e2e::succeeded(build({"run", "-p", "app-a"})))) << "app-a did not run";
        expect(fatal(e2e::succeeded(build({"run", "-p", "app-b"})))) << "app-b did not run";
        r = build({"build", "-p", "helper"});
        expect(fatal(failed(r))) << "standalone library borrowed an unselected consumer";
        expect(fatal(e2e::contains(s::text(r), "consumer 'helper'"))) << "standalone library lost its context\n" << s::text(r);
        ws.write("helper/tests/context.cpp", "int main(){return 0;}\n");
        r = build({"test", "--workspace"});
        expect(fatal(failed(r))) << "library test borrowed an application provider";
        expect(fatal(e2e::contains(s::text(r), "consumer 'helper'"))) << "library test lost its own context\n" << s::text(r);
        ws.remove("helper/tests");
        for (std::string_view suffix : {"a", "b"})
            ws.write(std::format("app-{}/tests/context.cpp", suffix), "int main(){return 0;}\n");
        replace("provider-a/mcpp.toml", "kind = \"lib\"", "kind = \"shared\"");
        r = build({"test", "--workspace"});
        expect(fatal(e2e::succeeded(r))) << "test/shared contexts crossed independent products\n" << s::text(r);
        expect(fatal(e2e::succeeded(build({"run", "-p", "app-a"})))) << "shared provider consumer did not run";

        const auto saved = ws.read("app-a/mcpp.toml");
        drop("app-a/mcpp.toml", "provider-a =");
        r = build({"build", "--workspace"});
        expect(fatal(failed(r))) << "another product supplied a missing provider";
        expect(fatal(e2e::contains(s::text(r), "no package provides capability 'probe-interface'"))) << "wrong missing-provider diagnosis\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "consumer 'app-a'"))) << "missing consumer context\n" << s::text(r);
        ws.write("app-a/mcpp.toml", ws.read("app-a/mcpp.toml") + R"cpp([build-dependencies]
provider-a = { path = "../provider-a" }
)cpp");
        ws.write("app-a/build.mcpp", "int main(){return 0;}\n");
        r = build({"build", "--workspace"});
        expect(fatal(failed(r))) << "a host-only provider satisfied a target requirement";
        expect(fatal(e2e::contains(s::text(r), "no package provides capability 'probe-interface'"))) << "wrong host/target scope diagnosis\n" << s::text(r);
        ws.remove("app-a/build.mcpp");

        ws.write("app-a/mcpp.toml", saved + R"cpp([capabilities]
probe-interface = "provider-a"
)cpp");
        replace("app-a/mcpp.toml", "\nhelper = { path = \"../helper\" }\n", "\nhelper = { path = \"../helper\" }\nprovider-b = { path = \"../provider-b\" }\n");
        r = build({"build", "--workspace"});
        expect(fatal(failed(r))) << "pin hid an exclusive collision";
        expect(fatal(s::contains_nocase(s::text(r), "exclusive"))) << "wrong exclusive diagnosis\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "consumer 'app-a'"))) << "exclusive diagnosis omitted consumer\n" << s::text(r);
        for (std::string_view suffix : {"a", "b"}) drop(std::format("provider-{}/mcpp.toml", suffix), "exclusive =");
        r = build({"build", "--workspace"});
        expect(fatal(e2e::succeeded(r))) << "member pin did not resolve local ambiguity\n" << s::text(r);
        r = build({"build", "--workspace", "--cap", "probe-interface=provider-b"});
        expect(fatal(e2e::succeeded(r))) << "CLI pin did not override the member pin\n" << s::text(r);
        r = build({"build", "--workspace", "--cap", "probe-interface=provider-a"});
        expect(fatal(failed(r))) << "CLI pin escaped the second consumer closure";
        expect(fatal(e2e::contains(s::text(r), "consumer 'app-b'"))) << "invalid pin omitted its consumer\n" << s::text(r);

        for (std::string_view ns : {"first", "second"}) {
            ws.write(std::format("{}/mcpp.toml", ns), std::format(R"cpp([package]
name = "implementation"
namespace = "{0}"
version = "0.1.0"
provides = ["namespaced-interface"]
[targets.implementation]
kind = "lib"
)cpp", ns));
            ws.write(std::format("{}/src/anchor.c", ns), std::format("int {}_anchor(void){{return 0;}}\n", ns));
        }
        ws.write("namespaced/mcpp.toml", R"cpp([package]
name = "namespaced-consumer"
version = "0.1.0"
[features]
default = ["need"]
need = { requires = ["namespaced-interface"] }
[dependencies.first]
implementation = { path = "../first" }
[dependencies.second]
implementation = { path = "../second" }
[capabilities]
namespaced-interface = "implementation"
)cpp");
        ws.write("namespaced/src/main.cpp", "int main(){return 0;}\n");
        r = build({"build"}, "namespaced");
        expect(fatal(failed(r))) << "ambiguous short provider identity was accepted";
        expect(fatal(e2e::contains(s::text(r), "first:implementation"))) << "missing first qualified candidate\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "second:implementation"))) << "missing second qualified candidate\n" << s::text(r);
        replace("namespaced/mcpp.toml", "= \"implementation\"\n", "= \"first:implementation\"\n");
        r = build({"build"}, "namespaced");
        expect(e2e::succeeded(r)) << "qualified provider pin did not bind\n" << s::text(r);
    };

    "1019 a later edge walks the feature deps it activates"_test = [] {
        // `s` asks `v` for `x`, whose [feature-deps.x] asks `a` for `f2` (#897): with `v`
        // selected beside `s`, in either order, `x`'s feature-deps reach the graph and `a` is
        // built with `f2`.
        s::project ws{s::files{{"mcpp.toml", "[workspace]\nmembers = [\"a\", \"v\", \"s\"]\n"},
            {"a/mcpp.toml", R"cpp([package]
name = "a"
version = "0.1.0"
[targets.a]
kind = "lib"
[build]
sources = []
[features]
f1 = { sources = ["src/f1/*.cppm"] }
f2 = { sources = ["src/f2/*.cppm"] }
)cpp"},
            {"a/src/f1/f1.cppm", "export module a.f1;\nexport int f1() { return 1; }\n"},
            {"a/src/f2/f2.cppm", "export module a.f2;\nexport int f2() { return 2; }\n"},
            {"v/mcpp.toml", R"cpp([package]
name = "v"
version = "0.1.0"
[targets.v]
kind = "lib"
[build]
sources = ["src/base/*.cppm"]
[dependencies]
a = { path = "../a", features = ["f1"] }
[features]
x = { sources = ["src/x/*.cppm"] }
[feature-deps.x]
a = { path = "../a", features = ["f2"] }
)cpp"},
            {"v/src/base/v.cppm", "export module v;\nimport a.f1;\nexport int vv() { return f1(); }\n"},
            {"v/src/x/x.cppm", "export module v.x;\nimport a.f2;\nexport int vx() { return f2(); }\n"},
            {"s/mcpp.toml", R"cpp([package]
name = "s"
version = "0.1.0"
[targets.s]
kind = "bin"
main = "src/main.cpp"
[dependencies]
v = { path = "../v", features = ["x"] }
)cpp"},
            {"s/src/main.cpp", "import std;\nimport v;\nimport v.x;\nint main() { std::println(\"sum {}\", vv() + vx()); }\n"}}};
        for (const std::vector<std::string>& order : {std::vector<std::string>{"-p", "s", "-p", "v"}, std::vector<std::string>{"-p", "v", "-p", "s"}}) {
            ws.remove("target");
            std::vector<std::string> args{"build"};
            args.insert(args.end(), order.begin(), order.end());
            auto r = ws.mcpp(args);
            expect(fatal(e2e::succeeded(r))) << "mcpp build " << order[1] << " then " << order[3] << "\n" << s::text(r);
        }
        auto r = ws.mcpp({"run", "-p", "s"});
        expect(fatal(e2e::succeeded(r))) << "the program\n" << s::text(r);
        expect(e2e::contains(s::text(r), "sum 3")) << "the program's output\n" << s::text(r);
    };
}};

}  // namespace
