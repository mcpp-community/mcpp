// Defaults: default features (a consumer opting out, the opt-out with an explicit request,
// through a dependency; forwarding, a forward along every dependency table) and a required
// capability's default provider (formerly tests/e2e/126, 127, 128, 710, 1045).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

const s::options cdb{.env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};

E2E defaults{"defaults", [] {
    "126 default features opt out"_test = [] {
        // `default-features = false` keeps a dependency's default feature (and its define) off;
        // the default is on otherwise; an explicit request activates it beside the opt-out.
        const auto tmp = s::case_dir();
        s::project{tmp / "widget", {{"mcpp.toml", R"cpp([package]
name    = "widget"
version = "0.1.0"

[features]
default = ["turbo"]
turbo   = { defines = ["WIDGET_TURBO=1"] }

[build]
include_dirs = ["include"]

[targets.widget]
kind = "lib"
)cpp"}, {"include/widget/widget.hpp", R"cpp(#pragma once
inline int widget_mode() {
#ifdef WIDGET_TURBO
    return 1;
#else
    return 0;
#endif
}
)cpp"}, {"src/widget.cppm", R"cpp(export module widget;
export int widget_anchor() { return 0; }
)cpp"}}};
        s::project off{tmp / "appoff", {{"mcpp.toml", R"cpp([package]
name    = "appoff"
version = "0.1.0"

[dependencies]
widget = { path = "../widget", default-features = false }
)cpp"}, {"src/main.cpp", R"cpp(#include <widget/widget.hpp>
#ifdef WIDGET_TURBO
#error "WIDGET_TURBO leaked despite default-features = false"
#endif
int main() { return widget_mode() == 0 ? 0 : 2; }
)cpp"}}};
        expect(fatal(e2e::succeeded(off.mcpp({"build"}, cdb)))) << "(A) opt-out build failed";
        expect(fatal(s::lacks(off.read("compile_commands.json"), "WIDGET_TURBO"))) << "(A) WIDGET_TURBO present despite default-features = false";
        const auto bin = off.artifact(s::exe("appoff"));
        expect(fatal(bin.has_value())) << "(A) built binary not found";
        expect(fatal(e2e::succeeded(s::run(*bin)))) << "(A) binary observed turbo despite opt-out";
        s::project on{tmp / "appon", {{"mcpp.toml", R"cpp([package]
name    = "appon"
version = "0.1.0"

[dependencies]
widget = { path = "../widget" }
)cpp"}, {"src/main.cpp", R"cpp(#include <widget/widget.hpp>
#ifndef WIDGET_TURBO
#error "WIDGET_TURBO missing though default features are on"
#endif
int main() { return widget_mode() == 1 ? 0 : 2; }
)cpp"}}};
        expect(fatal(e2e::succeeded(on.mcpp({"build"}, cdb)))) << "(B) default-on build failed";
        expect(fatal(e2e::contains(on.read("compile_commands.json"), "WIDGET_TURBO"))) << "(B) WIDGET_TURBO missing with default features on";
        s::project re{tmp / "appre", {{"mcpp.toml", R"cpp([package]
name    = "appre"
version = "0.1.0"

[dependencies]
widget = { path = "../widget", default-features = false, features = ["turbo"] }
)cpp"}, {"src/main.cpp", R"cpp(#include <widget/widget.hpp>
#ifndef WIDGET_TURBO
#error "explicit features = [turbo] did not activate alongside default-features = false"
#endif
int main() { return widget_mode() == 1 ? 0 : 2; }
)cpp"}}};
        expect(fatal(e2e::succeeded(re.mcpp({"build"}, cdb)))) << "(C) opt-out + explicit build failed";
        expect(e2e::contains(re.read("compile_commands.json"), "WIDGET_TURBO")) << "(C) explicit feature did not activate alongside opt-out";
    };
    "127 transitive default features optout"_test[s::elf][s::gcc] = [] {
        // A consumer's `default-features = false` holds when the consumer is itself a dependency.
        const auto tmp = s::case_dir();
        s::project{tmp / "B", {{"mcpp.toml", R"cpp([package]
name    = "B"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[features]
default = ["heavy"]
heavy   = { defines = ["B_HEAVY=1"] }
[targets.B]
kind = "lib"
)cpp"}, {"src/b.cpp", R"cpp(int b_heavy() {
#ifdef B_HEAVY
    return 1;
#else
    return 0;
#endif
}
)cpp"}}};
        s::project a{tmp / "A", {{"mcpp.toml", R"cpp([package]
name    = "A"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[dependencies]
B = { path = "../B", default-features = false }
[targets.A]
kind = "lib"
)cpp"}, {"src/a.cpp", R"cpp(extern int b_heavy();
int a_val() { return b_heavy(); }
)cpp"}}};
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[dependencies]
A = { path = "../A" }
[targets.app]
kind = "bin"
main = "src/main.cpp"
)cpp"}, {"src/main.cpp", R"cpp(import std;
extern int a_val();
int main() { std::println("heavy={}", a_val()); return 0; }
)cpp"}}};
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "build failed";
        expect(fatal(eq(s::last_starting(app.mcpp({"run"}).out, "heavy="), std::string{"heavy=0"}))) << "transitive default-features opt-out not honored";
        a.write("mcpp.toml", R"cpp([package]
name    = "A"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[dependencies]
B = { path = "../B" }
[targets.A]
kind = "lib"
)cpp");
        app.mcpp({"clean"});
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "control build failed";
        expect(eq(s::last_starting(app.mcpp({"run"}).out, "heavy="), std::string{"heavy=1"})) << "control (defaults kept) expected heavy=1";
    };

    "128 feature forwarding"_test = [] {
        // A feature may open a feature of a dependency (`withextra = ["leaf/extra"]`), under
        // build and test, and only when it is requested.
        const auto tmp = s::case_dir();
        s::project{tmp / "leaf", {{"mcpp.toml", R"cpp([package]
name    = "leaf"
version = "0.1.0"

[features]
default = []
extra   = { defines = ["LEAF_EXTRA=1"] }

[build]
include_dirs = ["include"]

[targets.leaf]
kind = "lib"
)cpp"}, {"include/leaf/leaf.hpp", R"cpp(#pragma once
inline int leaf_mode() {
#ifdef LEAF_EXTRA
    return 1;
#else
    return 0;
#endif
}
)cpp"}, {"src/leaf.cppm", R"cpp(export module leaf;
export int leaf_anchor() { return 0; }
)cpp"}}};
        s::project{tmp / "mid", {{"mcpp.toml", R"cpp([package]
name    = "mid"
version = "0.1.0"

[features]
default   = []
withextra = ["leaf/extra"]

[dependencies]
leaf = { path = "../leaf", default-features = false }

[targets.mid]
kind = "lib"
)cpp"}, {"src/mid.cppm", R"cpp(export module mid;
export int mid_anchor() { return 0; }
)cpp"}}};
        s::project on{tmp / "appon", {{"mcpp.toml", R"cpp([package]
name    = "appon"
version = "0.1.0"

[dependencies]
mid = { path = "../mid", features = ["withextra"] }
)cpp"}, {"src/main.cpp", R"cpp(int main() { return 0; }
)cpp"}}};
        expect(fatal(e2e::succeeded(on.mcpp({"build"}, cdb)))) << "(A) build failed";
        expect(fatal(e2e::contains(on.read("compile_commands.json"), "LEAF_EXTRA"))) << "(A) leaf/extra was NOT activated by the forward";
        expect(fatal(e2e::succeeded(on.mcpp({"test"}, cdb)))) << "(A) test build failed";
        expect(fatal(e2e::contains(on.read("compile_commands.json"), "LEAF_EXTRA"))) << "(A/test) forward did not hold on the test path";
        s::project off{tmp / "appoff", {{"mcpp.toml", R"cpp([package]
name    = "appoff"
version = "0.1.0"

[dependencies]
mid = { path = "../mid" }
)cpp"}, {"src/main.cpp", R"cpp(int main() { return 0; }
)cpp"}}};
        expect(fatal(e2e::succeeded(off.mcpp({"build"}, cdb)))) << "(B) build failed";
        expect(fatal(s::lacks(off.read("compile_commands.json"), "LEAF_EXTRA"))) << "(B) leaf/extra leaked though no consumer requested mid/withextra";
        expect(fatal(e2e::succeeded(off.mcpp({"test"}, cdb)))) << "(B) test build failed";
        expect(s::lacks(off.read("compile_commands.json"), "LEAF_EXTRA")) << "(B/test) leaf/extra leaked on the test path";
    };
    "710 a forward reaches every dependency table"_test[s::gcc] = [] {
        // A feature forward is validated against every dependency table of its manifest: a
        // two-level forward along build-dependency edges builds under --strict and reaches the
        // leaf's tool; a row-declared dependency's forward is not reported; a forward to a key
        // no table declares is named, and refused under --strict.
        const auto tmp = s::case_dir();
        const s::options linked{.where = s::home::linked};
        s::project{tmp / "kt", {{"mcpp.toml", R"cpp([package]
name      = "kt"
namespace = "spike"
version   = "0.1.0"

[targets.kt]
kind = "bin"
main = "src/main.cpp"
)cpp"}, {"src/main.cpp", "int main() { return 0; }\n"}}};
        s::project{tmp / "leaf", {{"mcpp.toml", R"cpp([package]
name      = "leaf"
namespace = "spike"
version   = "0.1.0"

[targets.leaf]
kind = "lib"

[build]
sources = ["src/*.cpp"]

[features]
kt = []

[feature-deps.kt]
spike.kt = { path = "../kt", tools = ["kt"] }
)cpp"}, {"src/leaf.cpp", "int leaf_answer() { return 1; }\n"}}};
        s::project{tmp / "rules", {{"mcpp.toml", R"cpp([package]
name      = "rules"
namespace = "spike"
version   = "0.1.0"

[targets.rules]
kind = "lib"

[build]
sources = ["src/*.cpp"]

[build-dependencies]
spike.leaf = { path = "../leaf" }

[features]
kotlin = ["spike.leaf/kt"]
)cpp"}, {"src/rules.cpp", "int rules_answer() { return 1; }\n"}}};
        s::project{tmp / "win", {{"mcpp.toml", R"cpp([package]
name      = "win"
namespace = "spike"
version   = "0.1.0"

[targets.win]
kind = "lib"

[build]
sources = ["src/*.cpp"]

[features]
x = []
)cpp"}, {"src/win.cpp", "int win_answer() { return 1; }\n"}}};
        s::project fw{tmp / "fw", {{"mcpp.toml", R"cpp([package]
name      = "fw"
namespace = "spike"
version   = "0.1.0"

[targets.fw]
kind = "lib"

[build]
sources = ["src/*.cpp"]

[build-dependencies]
spike.rules = { path = "../rules" }

[target.'cfg(os = "windows")'.dependencies]
spike.win = { path = "../win" }

[features]
kotlin = ["spike.rules/kotlin", "spike.win/x"]
)cpp"}, {"src/fw.cpp", "int fw_answer() { return 42; }\n"}}};
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"

[dependencies]
spike.fw = { path = "../fw", features = ["kotlin"] }
)cpp"}, {"src/main.cpp", R"cpp(int fw_answer();
int main() { return fw_answer() == 42 ? 0 : 1; }
)cpp"}}};
        auto b1 = app.mcpp({"build", "--strict"}, linked);
        expect(fatal(e2e::succeeded(b1))) << "the two-level build-dependency forward was refused under --strict";
        expect(fatal(s::lacks(s::text(b1), "forwards to dependency"))) << "a forward along a build-dependency edge was reported as undeclared";
        expect(fatal(e2e::contains(s::text(b1), "Building host tool kt"))) << "the forwarded feature did not reach the leaf (its tool was not built)";
        expect(fatal(s::lacks(s::text(b1), "spike.win"))) << "a row-declared dependency's forward was reported";
        auto manifest = fw.read("mcpp.toml");
        const std::string_view from = "kotlin = [\"spike.rules/kotlin\", \"spike.win/x\"]";
        expect(fatal(manifest.find(from) != std::string::npos)) << "the fixture's forward line is missing";
        manifest.replace(manifest.find(from), from.size(), "kotlin = [\"spike.rules/kotlin\", \"spike.nowhere/x\"]");
        fw.write("mcpp.toml", manifest);
        app.remove("target");
        auto b3 = app.mcpp({"build"}, linked);
        expect(fatal(e2e::succeeded(b3))) << "an undeclared forward failed a non-strict build";
        expect(fatal(e2e::contains(s::text(b3), "forwards to dependency 'spike.nowhere'"))) << "a forward to an undeclared key was not named";
        auto b4 = app.mcpp({"build", "--strict"}, linked);
        expect(fatal(failed(b4))) << "--strict accepted a forward to a key no table declares";
        expect(e2e::contains(s::text(b4), "spike.nowhere")) << "the --strict refusal does not name the key";
    };
    "1045 a required capability takes its default provider"_test[s::elf] = [] {
        // A library's required capability with no provider in the graph takes the provider its
        // [defaults] table names for the target (over `*`), reported; a provider the consumer
        // supplies is used instead, and no default is added.
        const auto tmp = s::case_dir();
        for (std::string_view p : {"a", "b"})
            s::project{tmp / std::format("greet-{}", p), {{"mcpp.toml", std::format(R"cpp([package]
name     = "greet-{0}"
version  = "0.1.0"
provides = ["acme:greeter"]

[targets.greet-{0}]
kind = "lib"
)cpp", p)}}};
        s::project{tmp / "greet-a", {{"src/a.cpp", "extern \"C\" int greet() { return 1; }\n"}}};
        s::project{tmp / "greet-b", {{"src/b.cpp", "extern \"C\" int greet() { return 2; }\n"}}};
        s::project{tmp / "lib", {{"mcpp.toml", R"cpp([package]
name     = "lib"
version  = "0.1.0"
requires = ["acme:greeter"]

[targets.lib]
kind = "lib"

[defaults."acme:greeter"]
linux = { path = "../greet-a" }
"*"   = { path = "../greet-b" }
)cpp"}, {"src/lib.cpp", "extern \"C\" int greet(); int lib_greet() { return greet(); }\n"}}};
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"

[dependencies]
lib = { path = "../lib" }
)cpp"}, {"src/main.cpp", "int lib_greet(); int main() { return lib_greet(); }\n"}}};
        auto b1 = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(b1))) << "the build failed\n" << s::text(b1);
        expect(fatal(s::has_line(s::text(b1), "Default .*greet-a for acme:greeter of lib \\(linux\\)"))) << "the default is not reported\n" << s::text(b1);
        std::optional<fs::path> exe;
        for (const auto& f : s::find_files(app.at("target"), "app"))
            if (!exe && f.generic_string().ends_with("/bin/app")) exe = f;
        expect(fatal(exe.has_value())) << "no target/*/bin/app";
        const auto r1 = s::run(*exe);
        expect(fatal(r1.status == 1)) << "the program returned " << r1.status << ", not the Linux default's 1";
        app.write("mcpp.toml", app.read("mcpp.toml") + "greet-b = { path = \"../greet-b\" }\n");
        auto b2 = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(b2))) << "the build with the consumer's provider failed\n" << s::text(b2);
        expect(fatal(s::lacks(s::text(b2), "Default "))) << "a default was added though the graph provides the capability";
        const auto r2 = s::run(*exe);
        expect(r2.status == 2) << "the program returned " << r2.status << ", not the consumer's provider's 2";
    };
}};

}  // namespace
