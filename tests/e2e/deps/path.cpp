// Path dependencies and the manifest's dependency tables as `mcpp add` and `mcpp remove` write
// them (formerly tests/e2e/09, 12).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// A package of a local index: `namespace`.`name` at `version` for the three systems.
std::string index_package(std::string_view ns, std::string_view name, std::string_view description, std::string_view version) {
    return std::format(R"lua(package = {{
    spec = "1",
    namespace = "{0}",
    name = "{1}",
    description = "{2}",
    licenses = {{"MIT"}},
    type = "package",
    xpm = {{
        linux   = {{ ["{3}"] = {{ url = "https://example.invalid/x.tar.gz" }} }},
        macosx  = {{ ["{3}"] = {{ url = "https://example.invalid/x.tar.gz" }} }},
        windows = {{ ["{3}"] = {{ url = "https://example.invalid/x.zip" }} }},
    }},
}}
)lua", ns, name, description, version);
}

E2E path{"path", [] {
    "09 path dependency"_test[tag{"requires:elf"}][tag{"requires:gcc"}] = [] {
        // Package B imports modules of package A through `[dependencies.A] path`: the
        // multi-package scanner and link; a path whose manifest names another package is refused.
        s::needs({"elf", "gcc"});
        auto a = s::project::scaffold("mylibA");
        a.write("src/greet.cppm", "export module mylibA.greet;\nimport std;\nexport auto greet(std::string_view who) -> void {\n    std::println(\"Hello {} from mylibA!\", who);\n}\n");
        a.remove("src/main.cpp");
        const std::string lib_manifest = "[package]\nname        = \"mylibA\"\nversion     = \"0.1.0\"\n\n[language]\nstandard   = \"c++23\"\nmodules    = true\nimport_std = true\n\n[modules]\nsources = [\"src/**/*.cppm\"]\n\n[targets.mylibA]\nkind = \"lib\"\n";
        a.write("mcpp.toml", lib_manifest);
        auto b = s::project::scaffold("myappB");
        b.write("src/main.cpp", "import std;\nimport mylibA.greet;\nint main() {\n    greet(\"modular C++23\");\n    std::println(\"path dep build OK\");\n    return 0;\n}\n");
        auto app_manifest = [](std::string_view path) {
            return std::format("[package]\nname        = \"myappB\"\nversion     = \"0.1.0\"\n\n[language]\nstandard   = \"c++23\"\nmodules    = true\nimport_std = true\n\n[modules]\nsources = [\"src/**/*.cppm\", \"src/**/*.cpp\"]\n\n[targets.myappB]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n[dependencies.mylibA]\npath = \"{}\"\n", path);
        };
        b.write("mcpp.toml", app_manifest("../mylibA"));
        b.mcpp({"build"});
        const auto out = s::text(b.mcpp({"run"}));
        expect(e2e::contains(out, "Hello modular C++23 from mylibA")) << "dep module not invoked";
        expect(e2e::contains(out, "path dep build OK")) << "main println missing";
        std::string ninja;
        for (const auto& f : s::find_files(b.at("target"), R"(build\.ninja)"))
            if (!f.generic_string().contains("/.build-mcpp/")) ninja += s::read_file(f);
        expect(s::has_line(ninja, "mylibA.*greet.cppm")) << "ninja missing dep package source";
        expect(s::has_line(ninja, "gcm.cache/[^ /]*mylibA/mylibA.greet.gcm")) << "ninja missing dep BMI";
        // A path whose manifest names another package.
        const auto wrong = s::case_dir() / "elsewhere/wrongname";
        fs::create_directories(wrong.parent_path());
        fs::copy(a.dir(), wrong, fs::copy_options::recursive);
        s::project{wrong, {}}.write("mcpp.toml", std::regex_replace(lib_manifest, std::regex("name        = \"mylibA\""), "name        = \"differentname\""));
        b.write("mcpp.toml", app_manifest(wrong.generic_string()));
        auto err = b.mcpp({"build"});
        expect(err.started && err.status != 0) << "expected name-mismatch error";
        expect(e2e::contains(s::text(err), "mismatch with declared name")) << "wrong error";
    };

    "12 add command"_test = [] {
        // `mcpp add` writes [dependencies]: default-namespace packages as bare keys, `ns.name` as a
        // subtable, `ns:name` only as a deprecated alias; a package no readable index serves is
        // refused before mcpp.toml is touched (#305), where absence is provable; `mcpp remove`
        // stays within [dependencies].
        auto app = s::project::scaffold("myapp");
        app.write("index/pkgs/w/widget.lua", index_package("mcpplibs", "widget", "default-namespace package served by a local index", "0.1.0"));
        app.write("index/pkgs/g/gadget.lua", index_package("mcpplibs", "gadget", "second default-namespace package", "0.2.0"));
        app.write("index/pkgs/a/acme.util.lua", index_package("acme", "util", "nested-namespace package served by a local index", "2.0.0"));
        app.write("index/pkgs/c/capi.lua.lua", index_package("capi", "lua", "new exact dotted identity", "5.4.7"));
        app.write("index/pkgs/m/mcpplibs.capi.lua.lua", index_package("mcpplibs.capi", "lua", "old prefixed dotted candidate", "9.9.9"));
        app.write("mcpp.toml", app.read("mcpp.toml") + "\n[indices]\ndefault = { path = \"index\" }\nacme = { path = \"index\" }\ncapi = { path = \"index\" }\n");
        auto manifest = [&] { return app.read("mcpp.toml"); };
        auto failed = [](const e2e::outcome& r) { return r.started && r.status != 0; };

        app.mcpp({"add", "widget@0.1.0"});
        expect(s::has_line(manifest(), R"(^\[dependencies\])")) << "no [dependencies] section";
        expect(s::has_line(manifest(), R"(^widget = "0\.1\.0"$)")) << "widget entry missing or quoted";
        expect(s::no_line(manifest(), R"(^"widget")")) << "default-ns key should not be quoted";
        app.mcpp({"add", "mcpplibs.gadget@0.2.0"});
        expect(eq(s::count_matching(manifest(), R"(^\[dependencies\]$)"), std::size_t{1})) << "[dependencies] header duplicated";
        expect(s::has_line(manifest(), R"(^gadget = "0\.2\.0"$)")) << "gadget not set";
        app.mcpp({"add", "acme.util@2.0.0"});
        expect(s::has_line(manifest(), R"(^\[dependencies\.acme\]$)")) << "missing [dependencies.acme] section";
        expect(s::has_line(manifest(), R"(^util = "2\.0\.0"$)")) << "util entry missing";
        expect(s::no_line(manifest(), R"(^acme\.util = )")) << "dotted selector must not be stored as an ambiguous flat key";
        const auto alias = s::text(app.mcpp({"add", "acme:util@2.0.0"}));
        expect(e2e::contains(alias, "deprecated")) << "missing migration warning";
        expect(e2e::contains(alias, "acme.util@2.0.0")) << "warning lacks canonical replacement";
        const auto exact = s::text(app.mcpp({"add", "capi.lua@9.9.9"}));
        expect(e2e::contains(exact, "available: 5.4.7")) << "sibling stole exact selector";
        expect(e2e::contains(exact, "mcpplibs.capi.lua")) << "migration warning lacks old selector";
        expect(e2e::contains(exact, "capi.lua")) << "migration warning lacks new selector";
        app.mcpp({"add", "capi.lua@5.4.7"});
        expect(s::has_line(manifest(), R"(^\[dependencies\.capi\]$)")) << "missing [dependencies.capi] section";
        expect(s::has_line(manifest(), R"(^lua = "5\.4\.7"$)")) << "exact capi.lua entry missing";
        app.mcpp({"add", "mcpplibs.capi.lua@9.9.9"});
        expect(s::has_line(manifest(), R"(^\[dependencies\.mcpplibs\.capi\]$)")) << "nested namespace section missing";
        app.mcpp({"remove", "mcpplibs.capi.lua"});
        expect(s::no_line(manifest(), R"(^lua = "9\.9\.9"$)")) << "nested exact remove failed";
        app.write("mcpp.toml", manifest() + "\n[dependencies.legacy]\nold = \"0.1.0\"\n");
        app.mcpp({"remove", "legacy.old"});
        expect(s::no_line(manifest(), R"(^old = "0\.1\.0"$)")) << "legacy.old was not removed";
        auto bare = app.mcpp({"add", "bareword"});
        expect(failed(bare)) << "expected error for missing version";
        expect(e2e::contains(s::text(bare), "version required")) << "wrong error";
        expect(failed(app.mcpp({"add", ":@1.0"}))) << "expected error for empty package name";
        const auto before_invalid = manifest();
        for (auto invalid : {"ocornut..imgui@1.0.0", "acme/util@1.0.0", "acme\\util@1.0.0", "acme.util@", "acme.util@1.0@2.0", "widget@1.0\"bad"}) {
            auto r = app.mcpp({"add", invalid});
            expect(failed(r)) << "expected error for invalid selector: " << invalid;
            expect(s::text(r).contains("invalid") || s::text(r).contains("version required")) << "wrong error for " << invalid << ": " << s::text(r);
            expect(eq(manifest(), before_invalid)) << "mcpp.toml mutated for invalid selector: " << invalid;
        }
        const auto before = manifest();
        auto missing = app.mcpp({"add", "definitely-not-a-real-package@9.9.9"});
        expect(failed(missing)) << "expected error for missing package";
        expect(e2e::contains(s::text(missing), "not found")) << "wrong error";
        expect(e2e::contains(s::text(missing), "tried:")) << "error should list the identities tried";
        expect(eq(manifest(), before)) << "mcpp.toml mutated for missing package";
        auto nope = app.mcpp({"add", "acme.nope@1.0.0"});
        expect(failed(nope)) << "expected error for missing acme package";
        expect(e2e::contains(s::text(nope), "(acme, nope)")) << "wrong error";
        expect(e2e::contains(s::text(nope), "route: local index 'acme': root present, pkgs present")) << "missing privacy-safe local-index route state";
        expect(eq(manifest(), before)) << "mcpp.toml mutated for missing package";
        app.mcpp({"add", "unknownidx.thing@1.0.0"});
        expect(s::has_line(manifest(), R"(^\[dependencies\.unknownidx\]$)")) << "unverifiable namespace should still be added";
        auto avail = app.mcpp({"add", "widget@9.9.9"});
        expect(e2e::contains(s::text(avail), "available: 0.1.0")) << "expected available-versions warning";
        expect(s::has_line(manifest(), R"(^widget = "9\.9\.9"$)")) << "widget@9.9.9 should still be written";

        // A workspace member reads its root's [workspace.indices].
        const auto ws_dir = s::case_dir() / "ws";
        fs::create_directories(ws_dir);
        fs::copy(app.at("index"), ws_dir / "index", fs::copy_options::recursive);
        expect(fatal(fs::is_regular_file(ws_dir / "index/pkgs/a/acme.util.lua"))) << "workspace index fixture copy is incomplete";
        s::project ws{ws_dir, {{"mcpp.toml", "[workspace]\nmembers = [\"m1\"]\n\n[workspace.indices]\nacme = { path = \"index\" }\n"}}};
        expect(fatal(e2e::succeeded(ws.mcpp({"new", "m1"}))));
        auto added = ws.mcpp_in("m1", {"add", "acme.util@2.0.0"});
        expect(e2e::succeeded(added)) << "workspace member could not read its root-owned local index";
        expect(s::has_line(ws.read("m1/mcpp.toml"), R"(^util = "2\.0\.0"$)")) << "member should inherit workspace [indices]";
        auto mnope = ws.mcpp_in("m1", {"add", "acme.nope@1.0.0"});
        expect(failed(mnope)) << "expected error inside workspace member";
        expect(e2e::contains(s::text(mnope), "(acme, nope)")) << "wrong error";

        // A legacy flat selector is migrated, not duplicated.
        s::project upsert{s::case_dir() / "upsert", {}};
        upsert.write("mcpp.toml", std::format("[package]\nname = \"upsert\"\nversion = \"0.1.0\"\n\n[indices]\nacme = {{ path = \"{}\" }}\n\n[dependencies]\n\"acme.util\" = \"1.0.0\"\n", app.at("index").generic_string()));
        upsert.mcpp({"add", "acme.util@2.0.0"});
        expect(s::no_line(upsert.read("mcpp.toml"), R"(^"acme\.util" = )")) << "legacy flat selector was duplicated instead of migrated";
        expect(eq(s::count_matching(upsert.read("mcpp.toml"), R"(^util = "2\.0\.0"$)"), std::size_t{1})) << "canonical exact selector was not upserted once";
        // remove stays within [dependencies].
        upsert.write("mcpp.toml", "[package]\nname = \"remove-scope\"\nversion = \"0.1.0\"\n\n[dev-dependencies]\nwidget = \"0.2.0\"\n\n[dependencies]\nwidget = \"0.1.0\"\n");
        upsert.mcpp({"remove", "widget"});
        expect(s::has_line(upsert.read("mcpp.toml"), R"(^widget = "0\.2\.0"$)")) << "remove escaped [dependencies] and deleted the dev dependency";
        expect(s::no_line(upsert.read("mcpp.toml"), R"(^widget = "0\.1\.0"$)")) << "regular dependency was not removed";
    };

    "27 namespace dependencies"_test[tag{"requires:fresh-sandbox"}] = [] {
        // `[dependencies.<ns>] name = { path = ... }` is parsed and built; the legacy quoted
        // `"<ns>.<name>"` key still round-trips; a flat key of the default namespace too.
        s::needs({"fresh-sandbox"});
        const s::options home{.where = s::home::inherited};
        const auto tmp = s::case_dir();
        auto lib = [&](std::string_view name, std::string_view module, std::string_view body) {
            s::project p{tmp / std::format("{}-pkg/{}", name, name), {}};
            p.write("src/" + std::string(name) + ".cppm", std::format("export module {};\nimport std;\n{}\n", module, body));
            p.write("mcpp.toml", std::format("[package]\nname        = \"{0}\"\nversion     = \"0.1.0\"\n\n[modules]\nsources = [\"src/**/*.cppm\"]\n\n[targets.{0}]\nkind = \"lib\"\n", name));
            return p.dir().generic_string();
        };
        const auto util = lib("util", "acme.util", "export int answer() { return 42; }");
        auto app = s::project::scaffold("app");
        app.write("src/main.cpp", "import std;\nimport acme.util;\nint main() { std::println(\"answer = {}\", answer()); return answer() == 42 ? 0 : 1; }\n");
        app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies.acme]\nutil = {{ path = \"{}\" }}\n", util));
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, home)))) << "build failed";
        expect(eq(s::last_line(app.mcpp({"run"}, home).out), std::string("answer = 42")));
        app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\n\"acme.util\" = {{ path = \"{}\" }}\n", util));
        app.remove("target");
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, home)))) << "legacy form build failed";
        expect(eq(s::last_line(app.mcpp({"run"}, home).out), std::string("answer = 42"))) << "legacy form";
        const auto util2 = lib("util2", "util2", "export int two() { return 2; }");
        app.write("src/main.cpp", "import std;\nimport util2;\nint main() { std::println(\"two = {}\", two()); return two() == 2 ? 0 : 1; }\n");
        app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nutil2 = {{ path = \"{}\" }}\n", util2));
        app.remove("target");
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, home)))) << "flat form build failed";
        expect(eq(s::last_line(app.mcpp({"run"}, home).out), std::string("two = 2"))) << "flat form";
    };

    "31 transitive deps"_test[tag{"requires:musl"}] = [] {
        // A path dependency that declares a path dependency is resolved whole (the consumer need
        // not list the grandchild), and the grandchild's include_dirs reach its parent's compile;
        // naming both is consistent. A home with the payloads only; a skip without a musl payload.
        s::needs({"musl"});
        const s::options home{.where = s::home::payloads};
        const auto musl = s::payload_root("xim-x-musl-gcc", s::musl_gcc_version()) / "bin/x86_64-linux-musl-g++";
        if (!fs::is_regular_file(musl))
            skip("no reusable musl-gcc xpkg payload");
        const auto tmp = s::case_dir();
        s::project gc{tmp / "grandchild/gc", {{"include/gc/gc.h", "#pragma once\ninline int gc_answer(void) { return 42; }\n"},
                                               {"src/gc.cppm", "export module gc;\n"},
                                               {"mcpp.toml", "[package]\nname    = \"gc\"\nversion = \"0.1.0\"\n\n[build]\ninclude_dirs = [\"include\"]\n\n[targets.gc]\nkind = \"lib\"\n"}}};
        s::project ch{tmp / "child/ch", {{"src/ch.cppm", "module;\n#include <gc/gc.h>\nexport module ch;\nexport int ch_answer() { return gc_answer(); }\n"},
                                         {"mcpp.toml", std::format("[package]\nname    = \"ch\"\nversion = \"0.1.0\"\n\n[targets.ch]\nkind = \"lib\"\n\n[dependencies]\ngc = {{ path = \"{}\" }}\n", gc.dir().generic_string())}}};
        auto top = [&](std::string_view name, std::string_view extra) {
            return s::project{tmp / name / name, {{"src/main.cpp", "import std;\nimport ch;\nint main() {\n    std::println(\"answer={}\", ch_answer());\n    return ch_answer() == 42 ? 0 : 1;\n}\n"},
                                                  {"mcpp.toml", std::format("[package]\nname    = \"{}\"\nversion = \"0.1.0\"\n\n[dependencies]\nch = {{ path = \"{}\" }}\n{}", name, ch.dir().generic_string(), extra)}}};
        };
        auto t = top("top", "");
        expect(fatal(e2e::succeeded(t.mcpp({"build"}, home)))) << "transitive build failed";
        expect(eq(s::last_line(t.mcpp({"run"}, home).out), std::string("answer=42")));
        auto t2 = top("top2", std::format("gc = {{ path = \"{}\" }}\n", gc.dir().generic_string()));
        expect(e2e::succeeded(t2.mcpp({"build"}, home))) << "duplicate-but-consistent dep failed";
    };
}};

}  // namespace
