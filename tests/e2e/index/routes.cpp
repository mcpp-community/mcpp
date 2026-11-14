// The routes an index takes into a build: [indices] at a workspace root and in a member, the
// default namespace redirected to a local index, and a descriptor's per-OS features (formerly
// tests/e2e/120, 121, 147).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

const s::options inherited{.where = s::home::inherited};

std::string lib_package(std::string_view ns_line, std::string_view name, std::string_view description, std::string_view source, std::string_view target) {
    return std::format(R"(package = {{
    spec = "1",{0}
    name = "{1}",
    description = "{2}",
    licenses = {{"MIT"}},
    type = "package",
    xpm = {{
        linux = {{
            ["1.0.0"] = {{
                url = "https://example.invalid/{4}-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            }},
        }},
    }},
    mcpp = {{
        language = "c++23",
        import_std = false,
        sources = {{ "{3}" }},
        targets = {{ ["{4}"] = {{ kind = "lib" }} }},
        deps = {{}},
    }},
}}
)", ns_line, name, description, source, target);
}

E2E routes{"routes", [] {
    "120 ws root indices"_test[s::gcc][s::fresh_sandbox] = [] {
        // [indices] and [workspace.dependencies] at a workspace root reach a member by their
        // position, with a warning naming [workspace.indices]; a member's own relative path is
        // anchored at the member; members naming one index tree are one configuration.
        s::project ws(s::case_dir() / "ws", {
            {"local-index/pkgs/x/x.widget2.lua", lib_package("\n    namespace = \"x\",", "x.widget2", "Namespaced package reachable only via the workspace-root-anchored index", "src/widget2.cppm", "widget2")},
            {"ylib/mcpp.toml", "[package]\nname = \"ylib\"\nversion = \"0.1.0\"\n\n[targets.ylib]\nkind = \"lib\"\n"},
            {"ylib/src/ylib.cppm", "export module ylib;\nexport int ylib_value() {\n    return 7;\n}\n"},
            {"mcpp.toml", "[workspace]\nmembers = [\"member-a\", \"member-b\"]\n\n[indices]\nx = { path = \"local-index\" }\n\n[workspace.dependencies]\nylib = { path = \"ylib\" }\n"},
            {".mcpp/.xlings/data/xpkgs/x.widget2/1.0.0/src/widget2.cppm", "export module widget2;\nexport int widget2_value() {\n    return 35;\n}\n"},
            {"member-a/mcpp.toml", "[package]\nname = \"member-a\"\nversion = \"0.1.0\"\n\n[dependencies.x]\nwidget2 = \"1.0.0\"\n\n[dependencies]\nylib = { workspace = true }\n\n[targets.member-a]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
            {"member-a/src/main.cpp", "import widget2;\nimport ylib;\nint main() {\n    return (widget2_value() + ylib_value() == 42) ? 0 : 1;\n}\n"},
            {"member-b/mcpp.toml", "[indices]\nx = { path = \"../local-index\" }\n\n[package]\nname = \"member-b\"\nversion = \"0.1.0\"\n\n[dependencies.x]\nwidget2 = \"1.0.0\"\n"},
            {"member-b/src/main.cpp", "import widget2;\nint main() {\n    return widget2_value() == 35 ? 0 : 1;\n}\n"}});
        auto r = ws.mcpp({"build", "-p", "member-a"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "the member did not resolve the root-anchored [indices]/[workspace.dependencies] path: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "[indices] on the workspace root reaches every member by its position"))) << "the position reading of [indices] says nothing: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "[workspace.indices]"))) << "the warning does not name the canonical spelling: " << s::text(r);
        r = ws.mcpp({"run", "-p", "member-a"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "run failed: " << s::text(r);
        r = ws.mcpp({"build", "-p", "member-b"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "a member's own relative [indices].path is not anchored at the member: " << s::text(r);
        r = ws.mcpp({"run", "-p", "member-b"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "member-b's program failed: " << s::text(r);
        r = ws.mcpp({"build", "--workspace"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "--workspace: " << s::text(r);
        std::vector<std::string> ninjas;
        for (const auto& f : s::find_files(ws.at("target"), "build\\.ninja"))
            if (!f.generic_string().contains("/.build-mcpp/")) ninjas.push_back(f.string());
        expect(fatal(ninjas.size() == 1u)) << "members naming one index tree are " << ninjas.size() << " configurations, expected 1";
        expect(fatal(ws.exists("mcpp.lock") && e2e::contains(ws.read("mcpp.lock"), "[package.\"x.widget2\"]"))) << "expected an x.widget2 lock entry";
    };

    "121 default ns redirect"_test[s::gcc][s::fresh_sandbox] = [] {
        // `[indices] default = { path }` and `"" = { path }` both serve the default namespace.
        const auto tmp = s::case_dir();
        s::project index(tmp / "local-index", {{"pkgs/g/gizmo.lua", lib_package("", "gizmo", "Default-namespace package served only via a redirected index", "src/gizmo.cppm", "gizmo")}});
        const auto app = [&](std::string_view name, std::string_view key) {
            return s::project(tmp / "project" / name, {
                {".mcpp/.xlings/data/xpkgs/gizmo/1.0.0/src/gizmo.cppm", "export module gizmo;\nexport int gizmo_value() {\n    return 42;\n}\n"},
                {"src/main.cpp", "import gizmo;\nint main() {\n    return gizmo_value() == 42 ? 0 : 1;\n}\n"},
                {"mcpp.toml", std::format("[package]\nname = \"{0}\"\nversion = \"0.1.0\"\n\n[indices]\n{1} = {{ path = \"{2}\" }}\n\n[dependencies]\ngizmo = \"1.0.0\"\n\n[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", name, key, index.dir().generic_string())}});
        };
        auto a = app("app", "default");
        auto r = a.mcpp({"build"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "the build did not resolve a default-namespace dependency from the redirected local index: " << s::text(r);
        r = a.mcpp({"run"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "run failed: " << s::text(r);
        expect(fatal(e2e::contains(a.read("mcpp.lock"), "[package.\"gizmo\"]"))) << "expected a gizmo lock entry: " << a.read("mcpp.lock");
        auto b = app("app2", "\"\"");
        r = b.mcpp({"build"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "[indices] \"\" = {...} did not redirect the default namespace: " << s::text(r);
    };

    "147 per os features xpkg"_test[s::gcc][s::fresh_sandbox] = [] {
        // A feature's neutral sources and its host-OS overlay (with a flag on its own source)
        // build and sum to 8; another OS's overlay is not compiled; with the feature off no
        // accel source is compiled.
        const auto tmp = s::case_dir();
        const std::string os = s::on_macos() ? "macosx" : s::on_windows() ? "windows" : "linux";
        const std::string other = s::on_linux() ? "macosx" : "linux";
        s::project index(tmp / "local-index", {{"pkgs/o/osfeat.lua", std::format(R"(package = {{
    spec = "1",
    name = "osfeat",
    description = "per-OS features probe",
    licenses = {{"MIT"}},
    type = "package",
    xpm = {{
        {0} = {{
            ["1.0.0"] = {{
                url = "https://example.invalid/osfeat-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            }},
        }},
    }},
    mcpp = {{
        language = "c++23",
        import_std = false,
        sources = {{ "src/base.c" }},
        features = {{
            ["accel"] = {{
                defines = {{ "OSFEAT_ACCEL" }},
                sources = {{ "src/accel_common.c" }},
            }},
        }},
        {0} = {{
            features = {{
                ["accel"] = {{
                    sources = {{ "src/accel_os.c" }},
                    flags   = {{ {{ glob = "src/accel_os.c", defines = {{ "OSFEAT_OS_KERNEL=5" }} }} }},
                }},
            }},
        }},
        {1} = {{
            features = {{
                ["accel"] = {{ sources = {{ "src/other_os_only.c" }} }},
            }},
        }},
        targets = {{ ["osfeat"] = {{ kind = "lib" }} }},
        deps = {{}},
    }},
}}
)", os, other)}});
        const std::string src = ".mcpp/.xlings/data/xpkgs/local-dev.osfeat/1.0.0/src/";
        const auto manifest = [&](std::string_view dep) {
            return std::format("[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[indices]\nlocal-dev = {{ path = \"{}\" }}\n\n[dependencies]\n\"local-dev.osfeat\" = {}\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", index.dir().generic_string(), dep);
        };
        s::project app(tmp / "project/app", {
            {src + "base.c", "int osfeat_base(void) { return 1; }\n"},
            {src + "accel_common.c", "#ifndef OSFEAT_ACCEL\n#error \"neutral feature define missing\"\n#endif\nint osfeat_common(void) { return 2; }\n"},
            {src + "accel_os.c", "/* Compiled only via the host-OS features overlay; its per-OS feature flag\n * must land here (and only here). */\n#ifndef OSFEAT_OS_KERNEL\n#error \"per-OS feature flags did not reach the per-OS feature source\"\n#endif\nint osfeat_os_kernel(void) { return OSFEAT_OS_KERNEL; }\n"},
            // Poison: if the other OS's section leaks, this file compiles and breaks the build.
            {src + "other_os_only.c", "#error \"non-host OS features section must be invisible\"\n"},
            {"src/main.cpp", "extern \"C\" int osfeat_base(void);\nextern \"C\" int osfeat_common(void);\nextern \"C\" int osfeat_os_kernel(void);\nint main() {\n    // 1 + 2 + 5 == 8: base + neutral feature payload + per-OS feature delta.\n    return (osfeat_base() + osfeat_common() + osfeat_os_kernel()) == 8 ? 0 : 1;\n}\n"},
            {"mcpp.toml", manifest("{ version = \"1.0.0\", features = [\"accel\"] }")}});
        auto r = app.mcpp({"build"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "build failed: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "matched no source file"))) << "the per-OS feature's flags glob must match its per-OS source: " << s::text(r);
        r = app.mcpp({"run"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "run failed (payload sum wrong or link failed): " << s::text(r);

        app.write("src/main.cpp", "extern \"C\" int osfeat_base(void);\nint main() { return osfeat_base() == 1 ? 0 : 1; }\n");
        app.write("mcpp.toml", manifest("\"1.0.0\""));
        fs::remove_all(app.at("target"));
        r = app.mcpp({"build"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "build (off) failed: " << s::text(r);
        expect(fatal(s::find_files(app.dir(), "accel_.*\\.o").empty())) << "accel sources compiled with the feature off";
        r = app.mcpp({"run"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "run (off) failed: " << s::text(r);
    };

    "169 semver project index"_test[s::gcc][s::fresh_sandbox] = [] {
        // `^2.0` through a project [indices] entry resolves to 2.1.0 (no `index update` advice),
        // locked with its version and its host-independent FNV-1a hash; the exact 2.0.0 too.
        std::string xpm;
        for (std::string_view os : {"linux", "macosx", "windows"}) {
            const auto ext = os == "windows" ? "zip" : "tar.gz";
            xpm += std::format("        {} = {{\n", os);
            for (std::string_view v : {"2.0.0", "2.1.0"})
                xpm += std::format("            [\"{0}\"] = {{\n                url = \"https://example.invalid/gadget-{0}.{1}\",\n                sha256 = \"0000000000000000000000000000000000000000000000000000000000000000\",\n            }},\n", v, ext);
            xpm += "        },\n";
        }
        const std::string gadget = "export module gadget;\nexport int gadget_value() {\n    return 42;\n}\n";
        s::project p(s::case_dir() / "proj", {
            {"local-index/pkgs/a/acme.gadget.lua", std::format(R"(package = {{
    spec = "1",
    namespace = "acme",
    name = "acme.gadget",
    description = "Package reachable only through a project [indices] entry",
    licenses = {{"MIT"}},
    type = "package",
    xpm = {{
{}    }},
    mcpp = {{
        language = "c++23",
        import_std = false,
        sources = {{ "src/gadget.cppm" }},
        targets = {{ ["gadget"] = {{ kind = "lib" }} }},
        deps = {{}},
    }},
}}
)", xpm)},
            {".mcpp/.xlings/data/xpkgs/acme.gadget/2.1.0/src/gadget.cppm", gadget},
            {"src/main.cpp", "import gadget;\nint main() {\n    return gadget_value() == 42 ? 0 : 1;\n}\n"},
            {"mcpp.toml", "[package]\nname    = \"proj\"\nversion = \"0.1.0\"\n\n[indices]\nacme = { path = \"local-index\" }\n\n[dependencies.acme]\ngadget = \"^2.0\"\n\n[targets.proj]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}});
        auto r = p.mcpp({"build"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "the SemVer constraint did not resolve against the project [indices] entry: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "→ v2.1.0"))) << "expected '^2.0' to resolve to v2.1.0: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "index update"))) << "a local path index must never be answered with 'mcpp index update': " << s::text(r);
        auto lock = p.exists("mcpp.lock") ? p.read("mcpp.lock") : "";
        expect(fatal(e2e::contains(lock, "[package.\"acme.gadget\"]"))) << "expected an acme.gadget lock entry: " << lock;
        expect(fatal(e2e::contains(lock, "version = \"2.1.0\""))) << "the lock must record the resolved version, not the constraint: " << lock;
        expect(fatal(e2e::contains(lock, "hash    = \"fnv1a:2adea846f70078bc\""))) << "acme.gadget 2.1.0's lock hash is not the host-independent FNV-1a: " << lock;
        r = p.mcpp({"run"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "run failed: " << s::text(r);

        p.write(".mcpp/.xlings/data/xpkgs/acme.gadget/2.0.0/src/gadget.cppm", gadget);
        p.write("mcpp.toml", std::regex_replace(p.read("mcpp.toml"), std::regex{R"((^|\n)gadget = "\^2\.0"(\n|$))"}, "$1gadget = \"2.0.0\"$2"));
        expect(fatal(s::has_line(p.read("mcpp.toml"), R"(^gadget = "2\.0\.0"$)"))) << "the rewrite to an exact version did not apply: " << p.read("mcpp.toml");
        p.remove("mcpp.lock");
        r = p.mcpp({"build"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "an exact version through a project [indices] entry regressed: " << s::text(r);
        lock = p.read("mcpp.lock");
        expect(fatal(e2e::contains(lock, "2.0.0"))) << "expected a 2.0.0 pin: " << lock;
        expect(fatal(e2e::contains(lock, "hash    = \"fnv1a:34544b46fc9621c3\""))) << "acme.gadget 2.0.0's lock hash is not the host-independent FNV-1a: " << lock;
    };

    "192 install path namespace"_test[s::gcc][s::fresh_sandbox] = [] {
        // acme:widget is not satisfied from the only widget payload on disk, compat's; the
        // build never reaches that source, and no diagnostic prints the placeholder <verdir>.
        const auto home = s::case_home(s::home::inherited);
        s::project(home / "registry/data/xpkgs/compat-x-widget/1.5.0", {{"src/widget.cppm", "export module widget;\n// If this ever reaches a build that asked for acme:widget, the wrong package\n// was compiled. The value is the tell.\nexport int widget_value() { return 1; }\n"}});
        s::project p(s::case_dir() / "proj", {
            {"local-index/pkgs/a/acme.widget.lua", R"(package = {
    spec = "1",
    namespace = "acme",
    name = "widget",
    description = "acme's widget",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux   = { ["1.5.0"] = { url = "https://example.invalid/w.tar.gz", sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
        macosx  = { ["1.5.0"] = { url = "https://example.invalid/w.tar.gz", sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
        windows = { ["1.5.0"] = { url = "https://example.invalid/w.zip",    sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/widget.cppm" },
        targets = { ["widget"] = { kind = "lib" } },
        deps = {},
    },
}
)"},
            {"src/main.cpp", "import widget;\nint main() { return widget_value() == 2 ? 0 : 1; }\n"},
            {"mcpp.toml", "[package]\nname    = \"consumer\"\nversion = \"0.1.0\"\n\n[dependencies.acme]\nwidget = \"1.5.0\"\n\n[indices]\nacme = { path = \"local-index\" }\n"}});
        auto r = p.mcpp({"build"}, inherited);
        expect(fatal(r.started && r.status != 0)) << "the build succeeded, so acme:widget was satisfied from somewhere; the only widget payload on disk is compat's: " << s::text(r);
        expect(fatal(s::no_line(s::text(r), R"(compat-x-widget/1\.5\.0/src/widget\.cppm)"))) << "the build reached compat's source while resolving acme:widget: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "<verdir>"))) << "a diagnostic still prints the literal placeholder '<verdir>'";
    };

    "210 local index addressing on every host"_test = [] {
        // An absolute local index path, and a workspace's root-owned index read by a member, are
        // addressed as readable roots: `add` writes the dependency, a miss names the route, and
        // the route names no path of the filesystem.
        const auto tmp = s::case_dir();
        const auto make_index = [](const fs::path& dir) {
            s::project(dir, {{"pkgs/a/acme.util.lua", R"(package = {
    spec = "1",
    namespace = "acme",
    name = "util",
    description = "local index fixture",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux   = { ["2.0.0"] = { url = "https://example.invalid/u.tar.gz" } },
        macosx  = { ["2.0.0"] = { url = "https://example.invalid/u.tar.gz" } },
        windows = { ["2.0.0"] = { url = "https://example.invalid/u.zip" } },
    },
}
)"}});
        };
        constexpr std::string_view route = "route: local index 'acme': root present, pkgs present";
        make_index(tmp / "abs-index");
        s::project abs(tmp / "abs-app", {{"mcpp.toml", std::format("[package]\nname = \"absapp\"\nversion = \"0.1.0\"\n\n[indices]\nacme = {{ path = \"{}\" }}\n", (tmp / "abs-index").generic_string())}});
        auto r = abs.mcpp({"add", "acme.util@2.0.0"});
        expect(fatal(e2e::succeeded(r))) << "an absolute local index path did not resolve on this host: " << s::text(r);
        expect(fatal(s::has_line(abs.read("mcpp.toml"), R"(^util = "2\.0\.0"$)"))) << "the dependency was not written: " << abs.read("mcpp.toml");
        r = abs.mcpp({"add", "acme.nope@1.0.0"});
        expect(fatal(r.started && r.status != 0)) << "expected a miss for acme.nope";
        expect(fatal(e2e::contains(s::text(r), route))) << "the absolute index is not addressed as a readable root: " << s::text(r);

        make_index(tmp / "ws/index");
        s::project ws(tmp / "ws", {{"mcpp.toml", "[workspace]\nmembers = [\"m1\"]\n\n[workspace.indices]\nacme = { path = \"index\" }\n"},
                                   {"m1/mcpp.toml", "[package]\nname = \"m1\"\nversion = \"0.1.0\"\n"}});
        r = ws.mcpp_in("m1", {"add", "acme.util@2.0.0"});
        expect(fatal(e2e::succeeded(r))) << "a workspace member could not read its root-owned local index: " << s::text(r);
        expect(fatal(s::has_line(ws.read("m1/mcpp.toml"), R"(^util = "2\.0\.0"$)"))) << "the member did not inherit the workspace [indices]: " << ws.read("m1/mcpp.toml");
        r = ws.mcpp_in("m1", {"add", "acme.nope@1.0.0"});
        expect(fatal(r.started && r.status != 0)) << "expected a miss inside the workspace member";
        expect(fatal(e2e::contains(s::text(r), route))) << "the inherited index was anchored somewhere unreadable: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), tmp.string()) && s::lacks(s::text(r), tmp.generic_string()))) << "the route diagnostic leaked a filesystem path: " << s::text(r);
    };
}};

}  // namespace
