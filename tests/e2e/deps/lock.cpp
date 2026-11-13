// Versions as the index's keys, and what mcpp.lock records of them, one entry per identity, written
// optimistically, keyed independently of the members planned, kept across configuration groups
// (formerly tests/e2e/196, 203, 844, 939, 990, 1027).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

const s::options inherited{.where = s::home::inherited};

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

// The index descriptor of mcpplibs.cmdline 1.0.0 (844, 939): never fetched, its payload seeded.
std::string cmdline_descriptor(std::string_view description) {
    return std::format(R"cpp(package = {{
    spec = "1",
    namespace = "mcpplibs",
    name = "cmdline",
    description = "{}",
    licenses = {{"MIT"}},
    type = "package",
    xpm = {{ linux = {{ ["1.0.0"] = {{
        url = "https://example.invalid/cmdline-1.0.0.tar.gz",
        sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
    }} }} }},
    mcpp = {{
        language = "c++23",
        import_std = false,
        sources = {{ "src/cmdline.cppm" }},
        targets = {{ ["cmdline"] = {{ kind = "lib" }} }},
        deps = {{}},
    }},
}}
)cpp", description);
}

// The index descriptor of mcpplibs.<name> 1.0.0 for every OS (990, 1027): its payload seeded.
std::string widget_descriptor(std::string_view name) {
    return std::format(R"cpp(package = {{
    spec = "1",
    namespace = "mcpplibs",
    name = "{0}",
    description = "a package of the default namespace from a local index",
    licenses = {{"MIT"}},
    type = "package",
    xpm = {{
        linux   = {{ ["1.0.0"] = {{ url = "https://example.invalid/w.tar.gz" }} }},
        macosx  = {{ ["1.0.0"] = {{ url = "https://example.invalid/w.tar.gz" }} }},
        windows = {{ ["1.0.0"] = {{ url = "https://example.invalid/w.zip" }} }},
    }},
    mcpp = {{
        language = "c++23",
        import_std = false,
        sources = {{ "src/{0}.cppm" }},
        targets = {{ ["{0}"] = {{ kind = "lib" }} }},
        deps = {{}},
    }},
}}
)cpp", name);
}

E2E lock{"lock", [] {
    "196 version identity and lock"_test[tag{"requires:gcc"}][tag{"requires:fresh-sandbox"}] = [] {
        // mcpp#363: a resolved version is an index key (an address re-rendered from parsed
        // numbers did not exist), and mcpp.lock records what was resolved, stably, whatever the
        // command, without dev-dependencies.
        s::needs({"gcc", "fresh-sandbox"});
        s::project p{{"src/main.cpp", "import gadget;\nint main(){ return gadget_value() == 42 ? 0 : 1; }\n"}};
        auto mk_pkg = [&](std::string_view name, std::string_view table, std::string_view mod = "gadget") {
            p.write(std::format("local-index/pkgs/a/acme.{}.lua", name),
                    std::format("package = {{\n    spec = \"1\", namespace = \"acme\", name = \"acme.{0}\",\n    description = \"version-model fixture\", licenses = {{\"MIT\"}}, type = \"package\",\n    xpm = {{ linux = {1}, macosx = {1}, windows = {1} }},\n    mcpp = {{ language = \"c++23\", import_std = false,\n             sources = {{ \"src/{2}.cppm\" }},\n             targets = {{ [\"{2}\"] = {{ kind = \"lib\" }} }}, deps = {{}} }},\n}}\n", name, table, mod));
        };
        mk_pkg("im", "{ [\"1.92.8\"] = { url = \"u\", sha256 = \"0\" },\n              [\"1.92.8-docking\"] = { url = \"u\", sha256 = \"0\" } }");
        mk_pkg("jdk", "{ [\"latest\"] = { ref = \"25.0.4.7.1\" },\n              [\"25.0.4\"] = { ref = \"25.0.4.7.1\" },\n              [\"25.0.4.7.1\"] = { url = \"u\", sha256 = \"0\" } }");
        mk_pkg("kh", "{ [\"latest\"] = { ref = \"pre-v0.0.5\" },\n              [\"pre-v0.0.5\"] = { url = \"u\", sha256 = \"0\" } }");
        mk_pkg("tie", "{ [\"1.0.0+a\"] = { url = \"u\", sha256 = \"0\" },\n              [\"1.0.0+b\"] = { url = \"u\", sha256 = \"0\" } }");
        mk_pkg("dv", "{ [\"1.0.0\"] = { url = \"u\", sha256 = \"0\" } }", "devkit");
        // Payloads seeded so that only the right key can install.
        auto seed = [&](std::string_view name, std::string_view version, std::string_view mod = "gadget") {
            p.write(std::format(".mcpp/.xlings/data/xpkgs/acme.{}/{}/src/{}.cppm", name, version, mod),
                    std::format("export module {0};\nexport int {0}_value() {{ return 42; }}\n", mod));
        };
        seed("im", "1.92.8");
        seed("jdk", "25.0.4.7.1");
        seed("dv", "1.0.0", "devkit");
        auto use_dep = [&](std::string_view name, std::string_view constraint) {
            p.write("mcpp.toml", std::format("[package]\nname    = \"proj\"\nversion = \"0.1.0\"\n\n[indices]\nacme = {{ path = \"local-index\" }}\n\n[dependencies.acme]\n{} = \"{}\"\n\n[targets.proj]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", name, constraint));
            p.remove("mcpp.lock");
        };
        auto build = [&](std::vector<std::string> args = {"build"}) { return p.mcpp(std::move(args), inherited); };

        use_dep("im", "^1.92.8");
        auto b1 = build();
        expect(fatal(e2e::succeeded(b1))) << "^1.92.8 did not build";
        expect(s::has_line(s::text(b1), R"(^\s*Resolved acme.im .* → v1\.92\.8$)")) << "^1.92.8 must resolve to 1.92.8, never the -docking branch";
        use_dep("im", "1.92.8-docking");
        auto b2 = build();
        expect(failed(b2)) << "expected the unseeded -docking payload to fail";
        expect(e2e::contains(s::text(b2), "1.92.8-docking")) << "exact pre-release must be addressed literally, not truncated";
        use_dep("jdk", "^25.0");
        auto b3 = build();
        expect(fatal(e2e::succeeded(b3))) << "^25.0 did not build";
        expect(s::has_line(s::text(b3), R"(^\s*Resolved acme.jdk .* → v25\.0\.4\.7\.1$)")) << "^25.0 must resolve to the real five-segment key";
        use_dep("kh", "*");
        auto b4 = build();
        expect(failed(b4)) << "a range over unorderable keys must not succeed";
        expect(e2e::contains(s::text(b4), "not ordered versions")) << "expected an error naming the unorderable keys";
        expect(e2e::contains(s::text(b4), "acme.kh = \"pre-v0.0.5\"")) << "the error must show the exact pin that works";
        expect(s::lacks(s::text(b4), "no valid versions in index")) << "the old message blames the index for a package it publishes fine";
        use_dep("kh", "pre-v0.0.5");
        auto b5 = build();
        expect(failed(b5)) << "expected the unseeded payload to fail";
        expect(s::lacks(s::text(b5), "not ordered versions")) << "an exact unorderable key must resolve, not be rejected";
        use_dep("kh", "=pre-v0.0.5");
        auto b5b = build();
        expect(failed(b5b)) << "expected the unseeded payload to fail";
        expect(s::no_line(s::text(b5b), "invalid version constraint|not ordered versions")) << "'=<unorderable key>' must resolve to that key";
        use_dep("tie", "^1.0");
        auto b6 = build();
        expect(failed(b6)) << "a tie must not silently pick one";
        expect(e2e::contains(s::text(b6), "compare EQUAL")) << "expected the tie to be named";
        expect(e2e::contains(s::text(b6), "1.0.0+a") && e2e::contains(s::text(b6), "1.0.0+b")) << "both tied keys must be listed";
        use_dep("tie", "=1.0.0+b");
        auto b6b = build();
        expect(failed(b6b)) << "expected the unseeded payload to fail";
        expect(s::lacks(s::text(b6b), "compare EQUAL")) << "an exact build-metadata pin must not be reported as a tie";
        expect(e2e::contains(s::text(b6b), "1.0.0+b")) << "the exact key must be addressed literally";

        // The lock records the resolved version, and is stable.
        use_dep("im", "^1.92.8");
        auto b7 = build({"build", "-v"});
        expect(fatal(e2e::succeeded(b7))) << "rebuild failed";
        const auto lock1 = p.read("mcpp.lock");
        expect(e2e::contains(lock1, "version = \"1.92.8\"")) << "the lock must record the resolved version";
        expect(s::lacks(lock1, "^")) << "a lock that records a range locks nothing";
        expect(s::has_line(s::text(b7), R"((Compiling|Cached) +acme\.im v1\.92\.8)")) << "the dependency banner must announce the resolved version";
        expect(s::lacks(s::text(b7), "acme.im v^")) << "the banner must never print a constraint as a version";
        expect(e2e::contains(lock1, "does not yet pin future builds")) << "the lock must state that it does not pin yet";
        expect(fatal(e2e::succeeded(build()))) << "second build failed";
        expect(eq(p.read("mcpp.lock"), lock1)) << "mcpp.lock is not stable across builds";

        // The lock does not depend on the command, and holds no dev-dependency.
        p.write("tests/t_dev.cpp", "import devkit;\nint main(){ return devkit_value()==42?0:1; }\n");
        p.write("mcpp.toml", "[package]\nname    = \"proj\"\nversion = \"0.1.0\"\n\n[indices]\nacme = { path = \"local-index\" }\n\n[dependencies.acme]\nim = \"^1.92.8\"\n\n[dev-dependencies.acme]\ndv = \"^1.0\"\n\n[targets.proj]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n");
        p.remove("mcpp.lock");
        expect(fatal(e2e::succeeded(build()))) << "build with a dev-dep failed";
        const auto lock_build = p.read("mcpp.lock");
        auto c2 = build({"test"});
        expect(fatal(e2e::succeeded(c2))) << "test with a dev-dep failed";
        expect(s::lacks(s::text(c2), "acme.im v^")) << "mcpp test announced a constraint as a version";
        expect(eq(p.read("mcpp.lock"), lock_build)) << "mcpp test rewrote mcpp.lock — the lock must not depend on the command";
        expect(fatal(e2e::succeeded(build()))) << "build after test failed";
        expect(eq(p.read("mcpp.lock"), lock_build)) << "mcpp build rewrote mcpp.lock after mcpp test";
        expect(e2e::contains(s::text(c2), "Resolved acme.dv")) << "the dev-dependency was never resolved — the check above is vacuous";
        expect(s::lacks(p.read("mcpp.lock"), "acme.dv")) << "dev-dependencies must not be recorded in mcpp.lock";
        expect(e2e::contains(p.read("mcpp.lock"), "dev-dependencies are excluded")) << "the lock must state that dev-dependencies are excluded";
    };

    "203 exact selector lock migration"_test[tag{"requires:gcc"}][tag{"requires:fresh-sandbox"}] = [] {
        // A compact dotted selector used to try mcpplibs.<ns> first; during the migration release
        // an existing lock keeps the identity it selected. Both identities exist, so index order
        // cannot make the assertion pass.
        s::needs({"gcc", "fresh-sandbox"});
        const auto tmp = s::case_dir();
        auto descriptor = [](std::string_view ns, std::string_view desc) {
            return std::format("package = {{\n    spec = \"1\",\n    namespace = \"{0}\",\n    name = \"lua\",\n    description = \"{1}\",\n    licenses = {{\"MIT\"}},\n    type = \"package\",\n    xpm = {{ linux = {{ [\"1.0.0\"] = {{\n        url = \"https://example.invalid/{0}-lua.tar.gz\",\n        sha256 = \"0000000000000000000000000000000000000000000000000000000000000000\",\n    }} }} }},\n    mcpp = {{\n        language = \"c++23\",\n        import_std = false,\n        sources = {{ \"src/lua.cppm\" }},\n        targets = {{ [\"lua\"] = {{ kind = \"lib\" }} }},\n        deps = {{}},\n    }},\n}}\n", ns, desc);
        };
        s::project index{tmp / "index", {{"pkgs/c/capi.lua.lua", descriptor("capi", "new exact identity")},
                                         {"pkgs/m/mcpplibs.capi.lua.lua", descriptor("mcpplibs.capi", "old prefixed identity")}}};
        s::project app{tmp / "app", {{".mcpp/.xlings/data/xpkgs/mcpplibs.capi-x-lua/1.0.0/src/lua.cppm", "export module selected.lua;\nexport int selected_value() { return 41; }\n"},
                                     {".mcpp/.xlings/data/xpkgs/capi-x-lua/1.0.0/src/lua.cppm", "export module selected.lua;\nexport int selected_value() { return 42; }\n"},
                                     {".mcpp/.xlings/data/xpkgs/mcpplibs.capi-x-lua/1.0.0/.mcpp_ok", "ok\n"},
                                     {".mcpp/.xlings/data/xpkgs/capi-x-lua/1.0.0/.mcpp_ok", "ok\n"},
                                     {"src/main.cpp", "import selected.lua;\nint main() { return selected_value() == 41 ? 0 : 1; }\n"},
                                     {"mcpp.lock", "# Existing pre-exact-selector resolution anchor.\nversion = 2\n\n[package.\"capi.lua\"]\nnamespace = \"mcpplibs.capi\"\nversion = \"1.0.0\"\nsource = \"index+mcpplibs.capi@1.0.0\"\nhash = \"fnv1a:migration-fixture\"\n"}}};
        app.write("mcpp.toml", std::format("[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[indices]\ndefault = {{ path = \"{0}\" }}\ncapi = {{ path = \"{0}\" }}\n\n[dependencies]\ncapi.lua = \"1.0.0\"\n", index.dir().generic_string()));
        auto b = app.mcpp({"build"}, inherited);
        expect(fatal(e2e::succeeded(b))) << "build failed";
        expect(e2e::contains(s::text(b), "keeping the locked identity")) << "expected selector migration warning";
        expect(e2e::contains(s::text(b), "mcpplibs.capi.lua")) << "warning must include the locked selector";
        expect(fatal(e2e::succeeded(app.mcpp({"run"}, inherited)))) << "run failed";
        expect(e2e::contains(app.read("mcpp.lock"), "namespace = \"mcpplibs.capi\"")) << "lock identity changed during migration";
    };

    "844 a planned build keeps one lock entry per identity"_test[tag{"requires:gcc"}][tag{"requires:fresh-sandbox"}] = [] {
        // A planned build of a workspace root keeps one mcpp.lock entry per identity, spelled as
        // the root's manifest spells it (F11): it keeps a lock in the 2026.9.28.3 spelling,
        // drops a second entry for the same identity, and does not rewrite an unchanged lock.
        s::needs({"gcc", "fresh-sandbox"});
        s::project ws{{{"local-index/pkgs/c/cmdline.lua", cmdline_descriptor("fixture for the one-entry-per-identity lock (F11)")},
                       {".mcpp/.xlings/data/xpkgs/mcpplibs-x-cmdline/1.0.0/src/cmdline.cppm", "export module fixture.cmdline;\nexport int cmdline_value() { return 7; }\n"},
                       {".mcpp/.xlings/data/xpkgs/mcpplibs-x-cmdline/1.0.0/.mcpp_ok", "ok\n"},
                       {"mcpp.toml", "[package]\nname    = \"ws\"\nversion = \"0.1.0\"\n[workspace]\nmembers = [\"m\"]\n[indices]\ndefault = { path = \"local-index\" }\n[dependencies.mcpplibs]\ncmdline = \"1.0.0\"\n[targets.ws]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                       {"src/main.cpp", "import fixture.cmdline;\nint main() { return cmdline_value() == 7 ? 0 : 1; }\n"},
                       {"m/mcpp.toml", "[package]\nname    = \"m\"\nversion = \"0.1.0\"\n[targets.m]\nkind = \"lib\"\n[build]\nsources = [\"src/m.cppm\"]\n"},
                       {"m/src/m.cppm", "export module m;\nexport int m_value() { return 1; }\n"}}};
        // The lock's hash: FNV-1a 64 of the identity (python3 computed it for the script).
        auto digest = [](std::string_view s) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (unsigned char b : s) { h ^= b; h *= 0x100000001b3ull; }
            return std::format("fnv1a:{:016x}", h);
        };
        auto entries = [&] { return s::count_matching(ws.read("mcpp.lock"), R"(^\[package\.)"); };
        const auto before = std::format(R"cpp(# Auto-generated by mcpp. Do not edit by hand.
# Records what this build resolved. It does not yet pin future builds:
# index dependencies are re-resolved from their constraints each time.
# dev-dependencies are excluded: only `mcpp test` resolves them, and this
# file must not change depending on which command ran last.
version = 2

[package."mcpplibs.cmdline"]
namespace = "mcpplibs"
version = "1.0.0"
source  = "index+mcpplibs@1.0.0"
hash    = "{}"

)cpp", digest("mcpplibs:mcpplibs.cmdline@1.0.0"));
        ws.write("mcpp.lock", before);
        expect(fatal(e2e::succeeded(ws.mcpp({"build"}, inherited)))) << "the planned build did not succeed";
        expect(fatal(eq(ws.read("mcpp.lock"), before))) << "(a) a planned build rewrote a lock in the 2026.9.28.3 spelling";
        ws.write("mcpp.lock", before + std::format("[package.\"cmdline\"]\nnamespace = \"mcpplibs\"\nversion = \"1.0.0\"\nsource  = \"index+mcpplibs@1.0.0\"\nhash    = \"{}\"\n\n", digest("mcpplibs:cmdline@1.0.0")));
        expect(fatal(eq(entries(), std::size_t{2}))) << "the fixture lock does not hold two entries";
        auto touch_main = [&](std::string_view line) { ws.write("src/main.cpp", ws.read("src/main.cpp") + std::string(line) + "\n"); };
        touch_main("// touched");
        expect(fatal(e2e::succeeded(ws.mcpp({"build"}, inherited)))) << "the second planned build did not succeed";
        expect(eq(entries(), std::size_t{1})) << "(b) the lock still holds " << entries() << " entries for one identity";
        expect(eq(ws.read("mcpp.lock"), before)) << "(b) the surviving entry is not the one 2026.9.28.3 wrote";
        const auto stamp = s::file_time(std::chrono::sys_seconds{std::chrono::seconds{1000000000}});
        fs::last_write_time(ws.at("mcpp.lock"), stamp);
        touch_main("// touched again");
        expect(fatal(e2e::succeeded(ws.mcpp({"build"}, inherited)))) << "the third planned build did not succeed";
        expect(eq(ws.read("mcpp.lock"), before)) << "(c) a repeated planned build changed the lock";
        expect(fs::last_write_time(ws.at("mcpp.lock")) == stamp) << "(c) a repeated planned build rewrote an unchanged lock";
        touch_main("// and again");
        s::options locked = inherited;
        locked.env.push_back({"MCPP_LOCKED", "1"});
        expect(e2e::succeeded(ws.mcpp({"build"}, locked))) << "--locked reported drift on an unchanged lock";
    };

    "939 a lock written meanwhile is not overwritten"_test[tag{"requires:gcc"}][tag{"requires:fresh-sandbox"}] = [] {
        // mcpp.lock is written optimistically: a lock that changed while a build resolved (here
        // the project's own build program appends to it) is not overwritten, and the build is
        // refused naming lockfile-concurrent and the remedy; the next build succeeds.
        s::needs({"gcc", "fresh-sandbox"});
        s::project p{{{"local-index/pkgs/c/cmdline.lua", cmdline_descriptor("fixture for the optimistic lock write")},
                      {".mcpp/.xlings/data/xpkgs/mcpplibs-x-cmdline/1.0.0/src/cmdline.cppm", "export module fixture.cmdline;\nexport int cmdline_value() { return 7; }\n"},
                      {".mcpp/.xlings/data/xpkgs/mcpplibs-x-cmdline/1.0.0/.mcpp_ok", "ok\n"},
                      {"mcpp.toml", "[package]\nname    = \"p\"\nversion = \"0.1.0\"\n[indices]\ndefault = { path = \"local-index\" }\n[dependencies.mcpplibs]\ncmdline = \"1.0.0\"\n"},
                      {"src/main.cpp", "import fixture.cmdline;\nint main() { return cmdline_value() == 7 ? 0 : 1; }\n"}}};
        expect(fatal(e2e::succeeded(p.mcpp({"build"}, inherited)))) << "A: the first build failed";
        expect(e2e::contains(p.read("mcpp.lock"), "cmdline")) << "A: mcpp.lock does not record the dependency";
        p.write("build.mcpp", R"cpp(#include <fstream>
int main() {
    std::ofstream f("mcpp.lock", std::ios::app);
    f << "# written by another run\n";
    return f ? 0 : 1;
}
)cpp");
        auto b = p.mcpp({"build"}, inherited);
        expect(fatal(failed(b))) << "B: a lock changed meanwhile was overwritten (the build succeeded)";
        expect(e2e::contains(s::text(b), "lockfile-concurrent")) << "B: the refusal does not name lockfile-concurrent";
        expect(e2e::contains(s::text(b), "run the command again")) << "B: the refusal names no remedy";
        expect(e2e::contains(p.read("mcpp.lock"), "# written by another run")) << "B: the other run's lock was overwritten";
        p.remove("build.mcpp");
        expect(e2e::succeeded(p.mcpp({"build"}, inherited))) << "C: the build after the concurrent write failed";
    };

    "990 a lock key does not depend on the selected members"_test[tag{"requires:gcc"}][tag{"requires:fresh-sandbox"}] = [] {
        // Two members take one workspace dependency under two spellings; the lock entry's key and
        // hash do not depend on which members a command plans, and the key is the workspace's
        // own spelling.
        s::needs({"gcc", "fresh-sandbox"});
        s::project ws{{{"local-index/pkgs/w/widget2.lua", widget_descriptor("widget2")},
                       {".mcpp/.xlings/data/xpkgs/mcpplibs.widget2/1.0.0/src/widget2.cppm", "export module widget2;\nexport int widget2_value() { return 35; }\n"},
                       {"mcpp.toml", "[workspace]\nmembers = [\"a\", \"c\"]\n[workspace.indices]\nmcpplibs = { path = \"local-index\" }\n[workspace.dependencies]\nwidget2 = \"1.0.0\"\n"},
                       {"a/mcpp.toml", "[package]\nname = \"a\"\nversion = \"0.1.0\"\n[dependencies]\nwidget2.workspace = true\n[targets.a]\nkind = \"lib\"\n"},
                       {"a/src/a.cppm", "export module a;\nimport widget2;\nexport int a_value() { return widget2_value() + 7; }\n"},
                       {"c/mcpp.toml", "[package]\nname = \"c\"\nversion = \"0.1.0\"\n[dependencies.mcpplibs]\nwidget2.workspace = true\n[targets.c]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                       {"c/src/main.cpp", "import widget2;\nint main() { return widget2_value() == 35 ? 0 : 1; }\n"}}};
        // The lock's widget2 entry: its header and its hash.
        auto entry = [&] {
            const auto lock = ws.read("mcpp.lock");
            std::string header, hash;
            bool in = false;
            std::istringstream lines{lock};
            for (std::string line; std::getline(lines, line);) {
                if (std::regex_search(line, std::regex{R"(^\[package\..*widget2"\])"})) header += line + "\n";
                if (line.find("widget2\"]") != std::string::npos) in = true;
                if (in && hash.empty() && line.starts_with("hash")) hash = line;
            }
            return header + hash;
        };
        std::map<std::string, std::string> seen;
        const std::vector<std::vector<std::string>> selections{{"--workspace"}, {"-p", "a"}, {"-p", "c"}};
        for (const auto& sel : selections) {
            ws.remove("mcpp.lock");
            std::vector<std::string> args{"build"};
            args.insert(args.end(), sel.begin(), sel.end());
            args.push_back("--configure-only");
            expect(fatal(e2e::succeeded(ws.mcpp(args, inherited)))) << "mcpp build " << sel.back() << " --configure-only";
            seen[sel.back()] = entry();
        }
        for (const auto& [sel, e] : seen)
            expect(eq(e, seen["--workspace"])) << "the lock entry of widget2 depends on the members a command plans (" << sel << ")";
        expect(s::has_line(seen["--workspace"], R"(^\[package\."widget2"\])")) << "the key is not the workspace's own spelling (widget2)";
    };

    "1027 a configuration group keeps the other groups lock entries"_test[tag{"requires:gcc"}][tag{"requires:fresh-sandbox"}] = [] {
        // A workspace whose members take two configurations is planned one group at a time,
        // and each plan writes the lock: a plan keeps the other groups' targets of an entry.
        s::needs({"gcc", "fresh-sandbox"});
        s::project ws{{{"local-index/pkgs/w/widget3.lua", widget_descriptor("widget3")},
                       {".mcpp/.xlings/data/xpkgs/mcpplibs.widget3/1.0.0/src/widget3.cppm", "export module widget3;\nexport int widget3_value() { return 36; }\n"},
                       {"mcpp.toml", "[workspace]\nmembers = [\"a\", \"b\"]\n[workspace.indices]\nmcpplibs = { path = \"local-index\" }\n"},
                       {"a/mcpp.toml", "[package]\nname = \"a\"\nversion = \"0.1.0\"\n[dependencies]\nmcpplibs.widget3 = \"1.0.0\"\n[targets.a]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                       {"a/src/main.cpp", "import widget3;\nint main() { return widget3_value() == 36 ? 0 : 1; }\n"},
                       {"b/mcpp.toml", "[package]\nname = \"b\"\nversion = \"0.1.0\"\nstandard = \"c++26\"\n[targets.b]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                       {"b/src/main.cpp", "int main() { return 0; }\n"}}};
        expect(fatal(e2e::succeeded(ws.mcpp({"build", "--workspace", "--configure-only"}, inherited)))) << "the first configure failed";
        expect(fatal(e2e::contains(ws.read("mcpp.lock"), "widget3"))) << "the lock records no widget3 entry";
        // The widget3 entry's `targets` replaced by another configuration's (python3 did it for
        // the script): the entry's lines are those up to the next line opening with `[`.
        {
            std::istringstream in{ws.read("mcpp.lock")};
            std::string out, line;
            bool block = false, found = false;
            auto close = [&] { if (block) out += "targets = [\"x86_64-openkal\"]\n"; block = false; };
            while (std::getline(in, line)) {
                if (line.starts_with("[")) {
                    close();
                    if (!found && std::regex_match(line, std::regex{R"(\[package\."[^"]*widget3"\])"})) block = found = true;
                } else if (block && line.starts_with("targets = ")) {
                    continue;
                }
                out += line + "\n";
            }
            close();
            expect(fatal(found)) << "no widget3 entry to rewrite";
            ws.write("mcpp.lock", out);
        }
        auto second = ws.mcpp({"build", "--workspace", "--configure-only"}, inherited);
        expect(fatal(e2e::succeeded(second))) << "the second configure failed";
        std::string targets;
        {
            std::istringstream in{ws.read("mcpp.lock")};
            bool after = false;
            for (std::string line; std::getline(in, line);) {
                if (line.find("widget3\"]") != std::string::npos) after = true;
                if (after && line.starts_with("targets")) { targets = line; break; }
            }
        }
        expect(e2e::contains(targets, "x86_64-openkal")) << "the other configuration's target was dropped: " << targets;
        expect(e2e::contains(targets, ",")) << "this configuration's target is missing: " << targets;
    };
}};

}  // namespace
