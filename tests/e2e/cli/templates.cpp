// `mcpp new`: package templates selected by exact identity and rendered, portable project names,
// and a scaffold that is published whole or not at all (formerly tests/e2e/69 and 204).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

void write(const fs::path& file, std::string_view content) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << content;
}

void append(const fs::path& file, std::string_view content) { std::ofstream(file, std::ios::binary | std::ios::app) << content; }

// The `mcpp self config --mirror` every fresh home of the scripts began with (its failure ignored).
void configure_mirror(const fs::path& dir, const s::options& o) {
    (void)s::mcpp({"self", "config", "--mirror", s::env("MCPP_E2E_TOOLCHAIN_MIRROR").value_or("GLOBAL")}, dir, o);
}

std::string descriptor(std::string_view ns, std::string_view shortname, std::string_view ver, std::string_view mcpp_value = "\"mcpp.toml\"") {
    return std::format(R"(package = {{
    spec = "1", namespace = "{0}", name = "{1}",
    description = "exact template fixture {0}.{1}",
    type = "package",
    xpm = {{
        linux   = {{ ["{2}"] = {{ url = "https://example.invalid/{1}.tar.gz", sha256 = "digest-{1}" }} }},
        macosx  = {{ ["{2}"] = {{ url = "https://example.invalid/{1}.tar.gz", sha256 = "digest-{1}" }} }},
        windows = {{ ["{2}"] = {{ url = "https://example.invalid/{1}.zip", sha256 = "digest-{1}" }} }},
    }},
    mcpp = {3},
}}
)", ns, shortname, ver, mcpp_value);
}

// No `.mcpp-new-*` staging directory is left in `parent`.
void no_stage(const fs::path& parent) {
    std::vector<std::string> left;
    for (const auto& e : fs::directory_iterator(parent))
        if (e.path().filename().string().starts_with(".mcpp-new-")) left.push_back(e.path().string());
    expect(fatal(left.empty())) << "transaction staging directory survived" << left;
}

E2E templates{"templates", [] {
    "69 package templates"_test = [] {
        // A package's templates by exact identity ([ns.]pkg, pkg:tmpl, pkg@ver:tmpl), {{var}}
        // rendering, injected features, --list-templates, a sole template as the default, the
        // provider errors, and the wire address of a package not installed.
        const s::options o{.where = s::home::fresh};
        const auto home = s::case_home();
        const auto work = mcpplibs::mtest::temp_dir() / "work";
        fs::create_directories(work);
        configure_mirror(work, o);
        const auto index = home / "indices/mcpplibs/pkgs";
        write(index / "t/tpl-demo.lua", R"(package = {
    spec      = "1",
    namespace = "mcpplibs",
    name      = "tpl-demo",
    description = "template e2e fixture",
    type      = "package",
    xpm = {
        linux = {
            ["latest"] = { ref = "1.2.0" },
            ["2.0.0-rc.1"] = { url = "https://example.invalid/x.tar.gz", sha256 = "rc" },
            ["1.2.0"] = { url = "https://example.invalid/x.tar.gz", sha256 = "0" },
            ["1.0.0"] = { url = "https://example.invalid/x.tar.gz", sha256 = "0" },
        },
        macosx = {
            ["latest"] = { ref = "1.2.0" },
            ["2.0.0-rc.1"] = { url = "https://example.invalid/x.tar.gz", sha256 = "rc" },
            ["1.2.0"] = { url = "https://example.invalid/x.tar.gz", sha256 = "0" },
            ["1.0.0"] = { url = "https://example.invalid/x.tar.gz", sha256 = "0" },
        },
        windows = {
            ["latest"] = { ref = "1.2.0" },
            ["2.0.0-rc.1"] = { url = "https://example.invalid/x.tar.gz", sha256 = "rc" },
            ["1.2.0"] = { url = "https://example.invalid/x.tar.gz", sha256 = "0" },
            ["1.0.0"] = { url = "https://example.invalid/x.tar.gz", sha256 = "0" },
        },
    },
    mcpp = "src/*.cppm",
}
)");
        const auto xpkgs = home / "registry/data/xpkgs";
        for (auto [ver, marker] : {std::pair{"1.0.0", "old"}, {"1.2.0", "new"}, {"2.0.0-rc.1", "rc"}}) {
            const auto root = xpkgs / "mcpplibs-x-tpl-demo" / ver / std::format("tpl-demo-{}", ver);
            write(root / "mcpp.toml", std::format("[package]\nname    = \"tpl-demo\"\nversion = \"{}\"\n", ver));
            write(root / "src/lib.cppm", "export module tpldemo;\n");
            write(root / "templates/starter/template.toml", std::format(R"x([template]
description  = "starter template ({0})"
default      = true
post_message = "post-message-marker-{0}"

[template.inject]
self = {{ features = ["alpha", "beta"] }}
)x", marker));
            write(root / "templates/starter/mcpp.toml.in", "[package]\nname    = \"{{project.name}}\"\nversion = \"0.1.0\"\n# from {{self.name}} {{self.version}}\n");
            write(root / "templates/starter/src/main.cpp.in", "// {{project.name}} via {{self.name}}@{{self.version}}\nimport std;\nint main() { std::println(\"{{project.name}}\"); return 0; }\n");
            write(root / "templates/starter/NOTES.md", "static-data\n");
            write(root / "templates/extra/template.toml", "[template]\ndescription = \"secondary template\"\n");
            write(root / "templates/extra/mcpp.toml.in", "[package]\nname    = \"{{project.name}}\"\nversion = \"0.1.0\"\n\n[dependencies]\ntpl-demo = \"{{self.version}}\"\n");
        }
        const auto seed_template = [&](std::string_view store, std::string_view shortname, std::string_view ver, std::string_view tmpl, bool is_default, std::string_view marker) {
            const auto root = xpkgs / store / ver / std::format("{}-{}", shortname, ver);
            write(root / "mcpp.toml", std::format("[package]\nname = \"{}\"\nversion = \"{}\"\n", shortname, ver));
            write(root / "templates" / tmpl / "template.toml", std::format("[template]\ndescription = \"{}\"\n", marker));
            if (is_default) append(root / "templates" / tmpl / "template.toml", "default = true\n");
            write(root / "templates" / tmpl / "mcpp.toml.in", std::format("[package]\nname = \"{{{{project.name}}}}\"\nversion = \"0.1.0\"\n# fixture-marker={}\n", marker));
        };
        write(index / "a/acme.tpl-demo.lua", descriptor("acme", "tpl-demo", "3.0.0"));
        seed_template("acme-x-tpl-demo", "tpl-demo", "3.0.0", "solo", false, "acme-solo");
        write(index / "m/mcpplibs.capi.lua.lua", descriptor("mcpplibs.capi", "lua", "5.4.7"));
        seed_template("mcpplibs.capi-x-lua", "lua", "5.4.7", "module", false, "nested-lua");
        write(index / "a/ambiguous.lua", descriptor("mcpplibs", "ambiguous", "1.0.0"));
        seed_template("mcpplibs-x-ambiguous", "ambiguous", "1.0.0", "one", false, "ambiguous-one");
        seed_template("mcpplibs-x-ambiguous", "ambiguous", "1.0.0", "two", false, "ambiguous-two");
        write(index / "d/dupe-default.lua", descriptor("mcpplibs", "dupe-default", "1.0.0"));
        seed_template("mcpplibs-x-dupe-default", "dupe-default", "1.0.0", "one", true, "dupe-one");
        seed_template("mcpplibs-x-dupe-default", "dupe-default", "1.0.0", "two", true, "dupe-two");
        write(index / "n/no-templates.lua", descriptor("mcpplibs", "no-templates", "1.0.0"));
        write(xpkgs / "mcpplibs-x-no-templates/1.0.0/no-templates-1.0.0/mcpp.toml", "[package]\nname = \"no-templates\"\nversion = \"1.0.0\"\n");

        const auto run = [&](std::vector<std::string> args, s::options opts) { return s::mcpp(std::move(args), work, opts); };
        const auto in = [&](std::string_view rel) { return s::read_file(work / rel); };

        // 1. A bare package name: the default template of the latest version.
        auto r = run({"new", "app1", "--template", "tpl-demo"}, o);
        auto out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "L0 failed\n" << out;
        expect(fatal(e2e::contains(out, "tpl-demo@1.2.0:starter"))) << "missing resolved spec\n" << out;
        expect(fatal(e2e::contains(out, "post-message-marker-new"))) << "missing post_message\n" << out;
        expect(fatal(e2e::contains(in("app1/mcpp.toml"), "name    = \"app1\""))) << "project.name not rendered";
        expect(fatal(e2e::contains(in("app1/mcpp.toml"), "from tpl-demo 1.2.0"))) << "self.* not rendered";
        expect(fatal(e2e::contains(in("app1/mcpp.toml"), "tpl-demo = { version = \"1.2.0\", features = [\"alpha\", \"beta\"] }"))) << "inject(features) missing\n" << in("app1/mcpp.toml");
        expect(fatal(fs::is_regular_file(work / "app1/NOTES.md"))) << "verbatim copy missing";
        expect(fatal(!fs::is_regular_file(work / "app1/template.toml"))) << "template.toml must not be copied";
        expect(fatal(e2e::contains(in("app1/src/main.cpp"), "app1 via tpl-demo@1.2.0"))) << "main.cpp not rendered";

        // 2. Fully explicit; the template declares the dependency itself.
        r = run({"new", "app2", "--template", "tpl-demo@1.0.0:extra"}, o);
        expect(fatal(e2e::succeeded(r))) << "L3 failed\n" << s::text(r);
        expect(fatal(e2e::contains(in("app2/mcpp.toml"), "tpl-demo = \"1.0.0\""))) << "self.version pin missing\n" << in("app2/mcpp.toml");
        expect(fatal(s::count_lines(in("app2/mcpp.toml"), "tpl-demo") == 1u)) << "duplicate injection\n" << in("app2/mcpp.toml");
        r = run({"new", "app2rc", "--template", "tpl-demo@2.0.0-rc.1:extra"}, o);
        expect(fatal(e2e::succeeded(r))) << "explicit prerelease failed\n" << s::text(r);
        expect(fatal(e2e::contains(in("app2rc/mcpp.toml"), "tpl-demo = \"2.0.0-rc.1\""))) << "exact prerelease pin missing\n" << in("app2rc/mcpp.toml");

        // 3. --list-templates.
        r = run({"new", "--list-templates", "tpl-demo"}, o);
        out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "list failed\n" << out;
        expect(fatal(e2e::contains(out, "starter") && e2e::contains(out, "extra"))) << "missing entries\n" << out;
        expect(fatal(e2e::contains(out, "(default)"))) << "default not marked\n" << out;

        // 4, 5. An unknown template; an unknown package.
        r = run({"new", "app3", "--template", "tpl-demo:nosuch"}, o);
        expect(fatal(failed(r))) << "unknown template must fail";
        expect(fatal(e2e::contains(s::text(r), "starter"))) << "error must list alternatives\n" << s::text(r);
        r = run({"new", "app4", "--template", "no-such-pkg-zz"}, o);
        out = s::text(r);
        expect(fatal(failed(r))) << "unknown package must fail";
        expect(fatal(s::contains_nocase(out, "not found for exact identity"))) << "missing exact index error\n" << out;
        expect(fatal(e2e::contains(out, "namespace omitted means 'mcpplibs'"))) << "missing default namespace diagnostic\n" << out;

        // 6. The builtin gui template, deprecated.
        r = run({"new", "app6", "--template", "gui"}, o);
        expect(fatal(e2e::succeeded(r))) << "gui builtin broke\n" << s::text(r);
        expect(fatal(s::contains_nocase(s::text(r), "deprecated"))) << "missing gui deprecation\n" << s::text(r);

        // 7. A Form B compat package: selected only when named, then refused.
        write(index / "c/compat.tpl-repro.lua", R"(package = {
    spec      = "1",
    namespace = "compat",
    name      = "compat.tpl-repro",
    type      = "package",
    xpm = {
        linux   = { ["0.1.0"] = { url = "https://example.invalid/x.tar.gz", sha256 = "0" } },
        macosx  = { ["0.1.0"] = { url = "https://example.invalid/x.tar.gz", sha256 = "0" } },
        windows = { ["0.1.0"] = { url = "https://example.invalid/x.tar.gz", sha256 = "0" } },
    },
    mcpp = {
        language     = "c++23",
        include_dirs = {"*"},
        sources      = {"*.cpp"},
        targets      = { tpl_repro = { kind = "lib" } },
    },
}
)");
        fs::create_directories(xpkgs / "compat-x-compat.tpl-repro/0.1.0");
        r = run({"new", "app7-bare", "--template", "tpl-repro"}, o);
        expect(fatal(failed(r))) << "L7: bare selector crossed into compat\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "(mcpplibs, tpl-repro)"))) << "L7: bare miss did not name exact identity\n" << s::text(r);
        r = run({"new", "app7", "--template", "compat.tpl-repro"}, o);
        expect(fatal(failed(r))) << "L7: Form B template must fail\n" << s::text(r);
        expect(fatal(s::contains_nocase(s::text(r), "inline build recipe"))) << "L7: wrong provider error for Form B\n" << s::text(r);

        // 8. A foreign namespace with the same short name.
        r = run({"new", "app-acme", "--template", "acme.tpl-demo"}, o);
        out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "exact acme template failed\n" << out;
        const auto acme = in("app-acme/mcpp.toml");
        expect(fatal(e2e::contains(acme, "fixture-marker=acme-solo"))) << "default sibling stole acme selector\n" << acme;
        expect(fatal(s::has_line(acme, R"(^\[dependencies\.acme\]$)") && s::has_line(acme, R"(^tpl-demo = "3\.0\.0"$)"))) << "canonical self dependency lost namespace\n" << acme;
        expect(fatal(e2e::contains(out, "namespace=acme name=tpl-demo"))) << "resolved output lost PackageId\n" << out;
        expect(fatal(e2e::contains(out, "payload=digest-tpl-demo"))) << "resolved output lost payload provenance\n" << out;
        expect(fatal(e2e::contains(out, "acme.tpl-demo@3.0.0:solo"))) << "sole template did not become default\n" << out;

        // 9. A nested namespace.
        r = run({"new", "app-lua", "--template", "mcpplibs.capi.lua@5.4.7"}, o);
        out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "nested namespace template failed\n" << out;
        const auto lua = in("app-lua/mcpp.toml");
        expect(fatal(e2e::contains(lua, "fixture-marker=nested-lua"))) << "nested template payload missing\n" << lua;
        expect(fatal(s::has_line(lua, R"(^\[dependencies\.mcpplibs\.capi\]$)") && s::has_line(lua, R"(^lua = "5\.4\.7"$)"))) << "nested PackageId lost during injection\n" << lua;
        expect(fatal(e2e::contains(out, "namespace=mcpplibs.capi name=lua"))) << "nested resolved identity missing\n" << out;

        // 10. Ambiguous, explicit, several defaults, no templates.
        r = run({"new", "app-amb", "--template", "ambiguous"}, o);
        expect(fatal(failed(r))) << "multiple templates without default must fail\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "one") && e2e::contains(s::text(r), "two"))) << "ambiguous error did not list choices\n" << s::text(r);
        expect(fatal(!fs::exists(work / "app-amb"))) << "ambiguous selection created target";
        r = run({"new", "app-explicit", "--template", "ambiguous:two"}, o);
        expect(fatal(e2e::succeeded(r))) << "explicit ambiguous choice failed\n" << s::text(r);
        expect(fatal(e2e::contains(in("app-explicit/mcpp.toml"), "fixture-marker=ambiguous-two"))) << "wrong explicit template";
        r = run({"new", "app-dupe", "--template", "dupe-default"}, o);
        expect(fatal(failed(r))) << "multiple defaults must fail\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "more than one default"))) << "wrong multiple-default diagnostic\n" << s::text(r);
        r = run({"new", "app-none", "--template", "no-templates"}, o);
        expect(fatal(failed(r))) << "package without templates must fail\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "ships no templates"))) << "wrong non-provider diagnostic\n" << s::text(r);

        // 11. An unpublished version, a moving alias, the legacy list alias.
        r = run({"new", "app-missing-ver", "--template", "tpl-demo@9.9.9"}, o);
        expect(fatal(failed(r))) << "unpublished exact template version must fail\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "matches none of"))) << "missing version inventory diagnostic\n" << s::text(r);
        expect(fatal(!fs::exists(work / "app-missing-ver"))) << "missing version created target";
        r = run({"new", "app-alias", "--template", "tpl-demo@latest"}, o);
        expect(fatal(failed(r))) << "moving version alias must not become a resolved template pin\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "version alias"))) << "wrong alias diagnostic\n" << s::text(r);
        expect(fatal(!fs::exists(work / "app-alias"))) << "version alias created target";
        r = run({"new", "ignored-name", "--template", "tpl-demo:"}, o);
        out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "legacy list alias failed\n" << out;
        expect(fatal(s::contains_nocase(out, "deprecated"))) << "legacy list alias did not warn\n" << out;
        expect(fatal(e2e::contains(out, "--list-templates tpl-demo"))) << "legacy list warning is not copyable\n" << out;
        expect(fatal(!fs::exists(work / "ignored-name"))) << "legacy list alias created a project";

        // 12. The wire address of a package that is not installed.
        fs::remove_all(xpkgs / "mcpplibs-x-tpl-demo/1.0.0");
        auto verbose = o;
        verbose.env.emplace_back("MCPP_VERBOSE", "1");
        r = run({"new", "app8", "--template", "tpl-demo@1.0.0:starter"}, verbose);
        auto log = s::text(r);
        for (auto at = log.find("\\\""); at != std::string::npos; at = log.find("\\\"", at)) log.erase(at, 1);
        std::vector<std::string> targets;
        static const std::regex t{R"re("targets":\["[^"]*"\])re"};
        for (std::sregex_iterator it(log.begin(), log.end(), t), end; it != end; ++it) targets.push_back(it->str());
        expect(fatal(!targets.empty())) << "L12: no install target was emitted at all\n" << log;
        const auto joined = std::accumulate(targets.begin(), targets.end(), std::string(), [](std::string a, const std::string& b) { return a + b + "\n"; });
        expect(fatal(s::lacks(joined, "\"mcpplibs.tpl-demo@1.0.0\""))) << "L12: install target re-derived the FQN (mcpplibs.tpl-demo) instead of addressing the descriptor's literal name as mcpplibs:tpl-demo\n" << joined;
        expect(fatal(e2e::contains(joined, "\"mcpplibs:tpl-demo@1.0.0\""))) << "L12: expected the wire address mcpplibs:tpl-demo@1.0.0\n" << joined;
    };

    "204 new transactional scaffold"_test = [] {
        // A project name is validated before anything else; a builtin and a package scaffold
        // publish every file and identity; an unknown token, a destination collision and a
        // symbolic link roll the whole staging tree back.
        const auto tmp = mcpplibs::mtest::temp_dir();
        const auto pre_home = tmp / "prevalidation-home";
        const auto pre_work = tmp / "prevalidation-work";
        fs::create_directories(pre_work);
        const s::options pre{.env = {{"MCPP_HOME", pre_home.string()}}};
        auto r = s::mcpp({"new", "../escape", "--template", "no-such-package"}, pre_work, pre);
        expect(fatal(failed(r))) << "path escape project name was accepted";
        expect(fatal(s::contains_nocase(s::text(r), "project name") || s::contains_nocase(s::text(r), "package selector"))) << "invalid-name diagnostic missing\n" << s::text(r);
        expect(fatal(!fs::exists(tmp / "escape"))) << "path escape created a sibling";
        expect(fatal(!fs::exists(pre_home))) << "invalid name initialized MCPP_HOME";
        no_stage(pre_work);
        for (std::string_view name : {"/absolute", ".", "..", "a/b", "a\\b", "CON", "con.txt", "trail.", "trail ", "myPROJECTname", "bad\tname"}) {
            r = s::mcpp({"new", std::string(name)}, pre_work, pre);
            expect(fatal(failed(r))) << "invalid project name was accepted: [" << name << "]";
            no_stage(pre_work);
        }

        const s::options o{.where = s::home::fresh};
        const auto home = s::case_home();
        const auto work = tmp / "work";
        fs::create_directories(work);
        configure_mirror(work, o);
        for (auto [ns, shortname, store, version] : {std::tuple{"acme", "tx-template", "acme-x-tx-template", "2.0.0"},
                                                     std::tuple{"mcpplibs", "tx-template", "mcpplibs-x-tx-template", "9.0.0"}}) {
            write(home / "indices/mcpplibs/pkgs/t" / std::format("{}.{}.lua", ns, shortname), std::format(R"(package = {{
    spec = "1", namespace = "{0}", name = "{1}",
    description = "transaction scaffold fixture",
    type = "package",
    xpm = {{
        linux   = {{ ["{2}"] = {{ url = "https://example.invalid/{1}.tar.gz", sha256 = "{0}-digest" }} }},
        macosx  = {{ ["{2}"] = {{ url = "https://example.invalid/{1}.tar.gz", sha256 = "{0}-digest" }} }},
        windows = {{ ["{2}"] = {{ url = "https://example.invalid/{1}.zip", sha256 = "{0}-digest" }} }},
    }},
    mcpp = "mcpp.toml",
}}
)", ns, shortname, version));
            const auto root = home / "registry/data/xpkgs" / store / version / std::format("{}-{}", shortname, version);
            write(root / "mcpp.toml", std::format("[package]\nname = \"{}\"\nversion = \"{}\"\n", shortname, version));
            write(root / "templates/starter/template.toml", "[template]\ndescription = \"single template is implicitly default\"\n\n[template.inject]\nself = { features = [\"gui\"] }\n");
            write(root / "templates/starter/mcpp.toml.in", R"([package]
name = "{{project.qualifiedName}}"
version = "0.1.0"

[dependencies.compat]
tx-template = "1.0.0"

# project={{project.name}} ns={{project.namespace}} qualified={{project.qualifiedName}}
# provider={{template.package.namespace}}/{{template.package.name}}
# selector={{template.package.selector}} version={{template.package.version}}
# template={{template.name}}
)");
            write(root / "templates/starter/src/main.cpp.in", "// {{project.qualifiedName}} from {{template.package.selector}}@{{template.package.version}}:{{template.name}}\nint main() { return 0; }\n");
        }
        const auto in = [&](std::string_view rel) { return s::read_file(work / rel); };

        r = s::mcpp({"new", "acme.tools.app"}, work, o);
        expect(fatal(e2e::succeeded(r))) << "builtin scaffold failed\n" << s::text(r);
        for (std::string_view f : {"acme.tools.app/mcpp.toml", "acme.tools.app/src/main.cpp", "acme.tools.app/tests/test_smoke.cpp"})
            expect(fatal(fs::is_regular_file(work / f))) << "builtin scaffold is incomplete: " << f;
        expect(fatal(e2e::contains(in("acme.tools.app/mcpp.toml"), "name        = \"acme.tools.app\""))) << "qualified project identity missing from builtin manifest";
        expect(fatal(e2e::contains(in("acme.tools.app/src/main.cpp"), "Hello from app!"))) << "builtin project.name token did not use the short name";
        no_stage(work);

        r = s::mcpp({"new", "org.demo.app", "--template", "acme.tx-template@2.0.0"}, work, o);
        expect(fatal(e2e::succeeded(r))) << "package scaffold failed\n" << s::text(r);
        const auto m = in("org.demo.app/mcpp.toml");
        expect(fatal(e2e::contains(m, "project=app ns=org.demo qualified=org.demo.app"))) << "project RenderVars are incomplete";
        expect(fatal(e2e::contains(m, "provider=acme/tx-template"))) << "provider PackageId was lost";
        expect(fatal(e2e::contains(m, "selector=acme.tx-template version=2.0.0"))) << "resolved selector/version were lost";
        expect(fatal(e2e::contains(m, "template=starter"))) << "implicit single-template selection was not rendered";
        expect(fatal(s::has_line(m, R"(^\[dependencies\.acme\]$)") && s::has_line(m, R"(^tx-template = \{ version = "2\.0\.0", features = \["gui"\] \}$)"))) << "exact self dependency missing\n" << m;
        expect(fatal(s::has_line(m, R"(^tx-template = "1\.0\.0"$)"))) << "same-short compat dependency was overwritten";
        no_stage(work);

        const auto pkg = home / "registry/data/xpkgs/acme-x-tx-template/2.0.0/tx-template-2.0.0";
        write(pkg / "templates/broken/template.toml", "[template]\ndescription = \"unknown token rollback\"\n");
        write(pkg / "templates/broken/mcpp.toml.in", "[package]\nname = \"{{project.typo}}\"\nversion = \"0.1.0\"\n");
        write(pkg / "templates/collision/template.toml", "[template]\ndescription = \"destination type collision rollback\"\n");
        write(pkg / "templates/collision/mcpp.toml.in", "[package]\nname = \"{{project.name}}\"\nversion = \"0.1.0\"\n");
        write(pkg / "templates/collision/a.in", "data\n");
        write(pkg / "templates/collision/a/member.txt", "nested\n");
        std::map<std::string, std::string> logs;
        for (std::string_view case_name : {"broken", "collision"}) {
            const auto target = std::format("fail-{}", case_name);
            r = s::mcpp({"new", target, "--template", std::format("acme.tx-template@2.0.0:{}", case_name)}, work, o);
            expect(fatal(failed(r))) << case_name << " failure template unexpectedly succeeded\n" << s::text(r);
            expect(fatal(!fs::exists(work / target))) << case_name << " left a partial final target";
            no_stage(work);
            logs[std::string(case_name)] = s::text(r);
        }
        expect(fatal(e2e::contains(logs["broken"], "unknown template token"))) << "unknown-token diagnostic missing\n" << logs["broken"];
        expect(fatal(s::contains_nocase(logs["collision"], "failed") || s::contains_nocase(logs["collision"], "cannot") ||
                     s::contains_nocase(logs["collision"], "directory")))
            << "destination-collision diagnostic missing\n" << logs["collision"];

        write(pkg / "templates/symlinked/template.toml", "[template]\ndescription = \"symlink rollback\"\n");
        write(pkg / "templates/symlinked/mcpp.toml.in", "[package]\nname = \"{{project.name}}\"\nversion = \"0.1.0\"\n");
        const auto link = pkg / "templates/symlinked/escape-link";
        std::error_code ec;
        fs::create_symlink("mcpp.toml.in", link, ec);
        if (!ec && fs::is_symlink(fs::symlink_status(link))) {
            r = s::mcpp({"new", "fail-symlink", "--template", "acme.tx-template@2.0.0:symlinked"}, work, o);
            expect(fatal(failed(r))) << "symlink template unexpectedly succeeded\n" << s::text(r);
            expect(fatal(s::contains_nocase(s::text(r), "symlink"))) << "symlink rejection diagnostic missing\n" << s::text(r);
            expect(fatal(!fs::exists(work / "fail-symlink"))) << "symlink failure left final target";
            no_stage(work);
        } else {
            fs::remove(link, ec);
        }
    };
}};

}  // namespace
