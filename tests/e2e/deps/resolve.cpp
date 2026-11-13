// Resolution of the dependency graph: versions merged or mangled, visibility, transitive edges
// (formerly tests/e2e/32, 33, 61).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

const s::options inherited{.where = s::home::inherited};

// An inherited home that also reads the user's mcpplibs index (indices/mcpplibs), as 32 and 33
// set theirs up.
void with_user_index() {
    const auto h = s::case_home(s::home::inherited);
    fs::create_directories(h / "indices");
    const fs::path user = fs::path(s::env("HOME").value_or("")) / ".mcpp";
    for (const auto& src : {user / "indices/mcpplibs", user / "registry/data/mcpplibs", user / "registry/data/mcpp-index"}) {
        std::error_code ec;
        if (!fs::is_directory(src, ec))
            continue;
        fs::create_directory_symlink(src, h / "indices/mcpplibs", ec);
        if (ec) fs::copy(src, h / "indices/mcpplibs", fs::copy_options::recursive, ec);
        break;
    }
}

// A package of a local index with an mcpp section: one library `target` of `source`, version 1.0.0.
std::string local_package(std::string_view ns, std::string_view name, std::string_view description,
                          std::string_view archive, std::string_view source, std::string_view target) {
    return std::format(R"lua(package = {{
    spec = "1",
    namespace = "{0}",
    name = "{1}",
    description = "{2}",
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
        sources = {{ "{4}" }},
        targets = {{ ["{5}"] = {{ kind = "lib" }} }},
        deps = {{}},
    }},
}}
)lua", ns, name, description, archive, source, target);
}

E2E resolve{"resolve", [] {
    "32 semver merge"_test[tag{"requires:fresh-sandbox"}] = [] {
        // Two consumers of one package with overlapping constraints (exact and range) merge to
        // one satisfying version instead of erroring out.
        s::needs({"fresh-sandbox"});
        with_user_index();
        const auto tmp = s::case_dir();
        s::project mylib{tmp / "mylib/mylib", {{"src/mylib.cppm", "export module mylib;\nexport int mylib_answer() { return 1; }\n"},
                                               {"mcpp.toml", "[package]\nname    = \"mylib\"\nversion = \"0.1.0\"\n\n[targets.mylib]\nkind = \"lib\"\n\n[dependencies.mcpplibs]\ncmdline = \"=0.0.1\"\n"}}};
        s::project app{tmp / "app/app", {{"src/main.cpp", "import std;\nimport mylib;\nint main() {\n    std::println(\"ok={}\", mylib_answer());\n    return mylib_answer() == 1 ? 0 : 1;\n}\n"},
                                         {"mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nmylib = {{ path = \"{}\" }}\n\n[dependencies.mcpplibs]\ncmdline = \">=0.0.1, <1\"\n", mylib.dir().generic_string())}}};
        auto b = app.mcpp({"build"}, inherited);
        expect(fatal(e2e::succeeded(b))) << "compatible merge failed";
        expect(s::has_line(s::text(b), R"(Merged.*cmdline.*0\.0\.1|→ v0\.0\.1)")) << "no merge trace in build log";
        expect(eq(s::last_line(app.mcpp({"run"}, inherited).out), std::string("ok=1")));
    };

    "33 multi version mangling"_test[tag{"requires:elf"}][tag{"requires:gcc"}] = [] {
        // Two transitive consumers wanting non-overlapping versions of one package: the second
        // copy's module name is mangled so both BMIs coexist; the header beside a staged source is
        // staged with it.
        s::needs({"elf", "gcc"});
        with_user_index();
        const auto tmp = s::case_dir();
        s::project liba{tmp / "libA/libA", {{"src/libA.cppm", "export module libA;\nimport mcpplibs.cmdline;\nimport std;\nexport int libA_v() { return 1; }\n"},
                                            {"mcpp.toml", "[package]\nname    = \"libA\"\nversion = \"0.1.0\"\n\n[targets.libA]\nkind = \"lib\"\n\n[dependencies.mcpplibs]\ncmdline = \"=0.0.1\"\n"}}};
        s::project libb{tmp / "libB/libB", {{"src/libB.cppm", "module;\nint libB_impl_v();\nexport module libB;\nimport mcpplibs.cmdline;\nimport std;\nexport int libB_v() { return libB_impl_v(); }\n"},
                                            {"src/libB_detail.h", "#pragma once\ninline int libB_detail_v() { return 2; }\n"},
                                            {"src/libB_impl.cpp", "#include \"libB_detail.h\"\nint libB_impl_v() { return libB_detail_v(); }\n"},
                                            {"mcpp.toml", "[package]\nname    = \"libB\"\nversion = \"0.1.0\"\n\n[targets.libB]\nkind = \"lib\"\n\n[dependencies.mcpplibs]\ncmdline = \"=0.0.2\"\n"}}};
        s::project app{tmp / "app/app", {{"src/main.cpp", "import std;\nimport libA;\nimport libB;\nint main() {\n    std::println(\"a={} b={}\", libA_v(), libB_v());\n    return libA_v() + libB_v() == 3 ? 0 : 1;\n}\n"},
                                         {"mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nlibA = {{ path = \"{}\" }}\nlibB = {{ path = \"{}\" }}\n", liba.dir().generic_string(), libb.dir().generic_string())}}};
        auto b = app.mcpp({"build"}, inherited);
        expect(fatal(e2e::succeeded(b))) << "mangling build failed";
        expect(s::has_line(s::text(b), "Mangled.*cmdline")) << "expected 'Mangled' trace in build log";
        expect(s::find_file(app.at("target"), "mcpplibs.cmdline__v0_0_2__mcpp.gcm").has_value()) << "mangled BMI not found";
        expect(s::find_file(app.at("target"), "mcpplibs.cmdline.gcm").has_value()) << "primary BMI not found";
        expect(s::find_file(app.at("target/.mangled"), "libB_detail.h").has_value()) << "the header beside the staged source was not staged with it";
        expect(eq(s::last_line(app.mcpp({"run"}, inherited).out), std::string("a=1 b=2")));
    };

    "61 dependency visibility"_test[tag{"requires:gcc"}] = [] {
        // A private dependency is available while compiling its dependent, and its include
        // directories do not leak to that package's consumers.
        s::needs({"gcc"});
        const auto tmp = s::case_dir();
        s::project hidden{tmp / "hidden", {{"include/hidden/value.hpp", "#pragma once\ninline int hidden_value() { return 7; }\n"},
                                           {"src/hidden.cppm", "module;\n#include <hidden/value.hpp>\nexport module hidden;\nexport int hidden_answer() { return hidden_value(); }\n"},
                                           {"mcpp.toml", "[package]\nname    = \"hidden\"\nversion = \"0.1.0\"\n\n[build]\ninclude_dirs = [\"include\"]\n\n[modules]\nsources = [\"src/*.cppm\"]\n\n[targets.hidden]\nkind = \"lib\"\n"}}};
        s::project facade{tmp / "facade", {{"src/facade.cppm", "export module facade;\nimport hidden;\nexport int facade_answer() { return hidden_answer(); }\n"},
                                           {"mcpp.toml", "[package]\nname    = \"facade\"\nversion = \"0.1.0\"\n\n[modules]\nsources = [\"src/*.cppm\"]\n\n[targets.facade]\nkind = \"lib\"\n\n[dependencies]\nhidden = { path = \"../hidden\", visibility = \"private\" }\n"}}};
        s::project app{tmp / "app", {{"src/main.cpp", "import facade;\nint main() {\n    return facade_answer() == 7 ? 0 : 1;\n}\n"},
                                     {"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n[dependencies]\nfacade = { path = \"../facade\" }\n"}}};
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, inherited)))) << "private dep should compile its direct consumer";
        expect(e2e::succeeded(app.mcpp({"run"}, inherited))) << "app run failed";
        app.write("src/main.cpp", "#include <hidden/value.hpp>\nimport facade;\nint main() {\n    return hidden_value() == facade_answer() ? 0 : 1;\n}\n");
        auto leak = app.mcpp({"build"}, inherited);
        expect(leak.started && leak.status != 0) << "private dependency include dir leaked to consumer";
        expect(e2e::contains(s::text(leak), "hidden/value.hpp")) << "consumer failed for an unexpected reason";
    };

    "62 dotted dependency selector priority"_test[tag{"requires:gcc"}][tag{"requires:fresh-sandbox"}] = [] {
        // A dotted selector in [dependencies] is exact: imgui.core -> (imgui, core), never
        // mcpplibs.imgui/core. The package is provided, so the build stays offline.
        s::needs({"gcc", "fresh-sandbox"});
        const auto tmp = s::case_dir();
        s::project index{tmp / "imgui-index", {{"pkgs/i/imgui.core.lua", local_package("imgui", "imgui.core", "Exact dotted selector test package", "imgui-core", "src/core.cppm", "core")}}};
        s::project app{tmp / "project/app", {{".mcpp/.xlings/data/xpkgs/imgui-x-imgui.core/1.0.0/src/core.cppm", "export module imgui.core;\nexport int imgui_core_value() {\n    return 42;\n}\n"},
                                             {"src/main.cpp", "import imgui.core;\nint main() {\n    return imgui_core_value() == 42 ? 0 : 1;\n}\n"}}};
        app.write("mcpp.toml", std::format("[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[indices]\nimgui = {{ path = \"{}\" }}\n\n[dependencies]\nimgui.core = \"1.0.0\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", index.dir().generic_string()));
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, inherited)))) << "build failed";
        expect(fatal(e2e::succeeded(app.mcpp({"run"}, inherited)))) << "run failed";
        expect(e2e::contains(app.read("mcpp.lock"), "namespace = \"imgui\"")) << "expected lockfile to record resolved peer namespace imgui";
    };

    "63 bare dependency peer root priority"_test[tag{"requires:gcc"}][tag{"requires:fresh-sandbox"}] = [] {
        // A bare selector means the default mcpplibs namespace: `imgui = "1.0.0"` locks
        // mcpplibs:imgui.
        s::needs({"gcc", "fresh-sandbox"});
        const auto h = s::case_home(s::home::inherited);
        s::project index{h / "indices/mcpplibs", {{".mcpp-index-updated", "ok\n"},
                                                  {"pkgs/i/imgui.lua", local_package("mcpplibs", "imgui", "Default-namespace exact selector test package", "imgui", "src/imgui.cppm", "imgui")}}};
        s::project payload{h / "registry/data/xpkgs/mcpplibs-x-imgui/1.0.0", {{"src/imgui.cppm", "export module imgui;\nexport int imgui_value() {\n    return 42;\n}\n"},
                                                                              {".mcpp_ok", "ok\n"}}};
        s::project app{s::case_dir() / "project/app", {{"src/main.cpp", "import imgui;\nint main() {\n    return imgui_value() == 42 ? 0 : 1;\n}\n"},
                                                    {"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nimgui = \"1.0.0\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}}};
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, inherited)))) << "build failed";
        expect(fatal(e2e::succeeded(app.mcpp({"run"}, inherited)))) << "run failed";
        expect(e2e::contains(app.read("mcpp.lock"), "[package.\"imgui\"]")) << "expected imgui package lock entry";
        expect(e2e::contains(app.read("mcpp.lock"), "namespace = \"mcpplibs\"")) << "bare package must lock as the default mcpplibs namespace";
    };

    "79 gtest regular dep feature main"_test = [] {
        // #168: compat.gtest as a regular dependency (`mcpp add compat.gtest`) does not inject
        // gtest_main into an app with its own main; `features = ["main"]` does; `add --dev`
        // writes [dev-dependencies.compat].
        auto app = s::project::scaffold("app");
        app.mcpp({"add", "compat.gtest@1.15.2"});
        expect(fatal(s::has_line(app.read("mcpp.toml"), R"(^\[dependencies\.compat\])"))) << "add did not write [dependencies.compat]";
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "#168 — build with regular-dep gtest failed";
        auto newest_graph = [&] {
            std::optional<fs::path> best;
            for (const auto& f : s::find_files(app.at("target"), R"(build\.ninja)"))
                if (!f.generic_string().contains("/.build-mcpp/") && (!best || fs::last_write_time(f) > fs::last_write_time(*best))) best = f;
            expect(fatal(best.has_value())) << "no build.ninja";
            return s::read_file(*best);
        };
        expect(s::lacks(newest_graph(), "gtest_main")) << "gtest_main linked into app by default (would collide with main)";
        expect(e2e::contains(newest_graph(), "gtest-all")) << "gtest framework (gtest-all) not linked";
        app.write("mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[dependencies.compat]\ngtest = { version = \"1.15.2\", features = [\"main\"] }\n");
        app.write("src/main.cpp", "#include <gtest/gtest.h>\nTEST(App, ok) { EXPECT_EQ(1 + 1, 2); }\n");
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "features=[main] build failed";
        expect(e2e::contains(newest_graph(), "gtest_main")) << "features=[main] did not link gtest_main";
        auto lib = s::project::scaffold("libapp");
        lib.mcpp({"add", "--dev", "compat.gtest@1.15.2"});
        expect(s::has_line(lib.read("mcpp.toml"), R"(^\[dev-dependencies\.compat\])")) << "add --dev did not write [dev-dependencies.compat]";
    };
}};

}  // namespace
