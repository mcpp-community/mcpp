// Where a workspace's statements are written and what they reach: a member's own statements, the
// profiles of a workspace and of its root package, the `workspace.` prefix that carries the
// meaning, the workspace layer a published member carries, and each table stated once (formerly
// tests/e2e/836, 885, 893, 900, 911).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

const s::options inherited{.where = s::home::inherited};

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

std::string escaped(std::string_view name) { return std::regex_replace(std::string(name), std::regex{R"([.])"}, R"(\.)"); }

std::optional<fs::path> bin_of(const fs::path& root, std::string_view name) {
    auto all = s::find_files(root, escaped(s::exe(name)));
    std::ranges::sort(all);
    for (const auto& f : all)
        if (f.generic_string().contains("/bin/")) return f;
    return std::nullopt;
}

std::vector<fs::path> graphs(const fs::path& root) {
    std::vector<fs::path> out;
    for (const auto& f : s::find_files(root / "target", R"(build\.ninja)"))
        if (!f.generic_string().contains("/.build-mcpp/")) out.push_back(f);
    std::ranges::sort(out);
    return out;
}

// A fresh home of the case whose registry is the shared home's ([xlings] home in config.toml).
s::options registry_home() {
    const auto home = s::case_home(s::home::fresh);
    s::project(home, {{"config.toml", std::format("[xlings]\nhome = \"{}\"\n", (s::shared_home() / "registry").generic_string())}});
    return {.where = s::home::fresh};
}

E2E statements{"statements", [] {
    "836 a members own statements reach its build"_test = [] {
        // M1 a member whose tests import std is tested, from the root and from its directory;
        // M2 a member's unknown `mcpp:` capability is refused naming its manifest; M3 a member's
        // unknown cfg() key is reported.
        const auto tmp = s::case_dir();
        s::project ws1(tmp / "ws1", {{"mcpp.toml", "[workspace]\nmembers = [\"probe\"]\n"},
                                     {"probe/mcpp.toml", "[package]\nname = \"probe-tests\"\nversion = \"0.1.0\"\n"},
                                     {"probe/tests/hello.cpp", "import std;\nint main() {\n    std::println(\"probe\");\n    return 0;\n}\n"}});
        auto r = ws1.mcpp({"test", "-p", "probe"});
        expect(fatal(e2e::succeeded(r))) << "M1 a member whose tests import std is not tested: " << s::text(r);
        r = ws1.mcpp_in("probe", {"test"});
        expect(fatal(e2e::succeeded(r))) << "M1 the same member, tested from its directory: " << s::text(r);
        s::project ws2(tmp / "ws2", {{"mcpp.toml", "[workspace]\nmembers = [\"a\"]\n"},
                                     {"a/mcpp.toml", "[package]\nname = \"a\"\nversion = \"0.1.0\"\nprovides = [\"mcpp:no-such-layer\"]\n"},
                                     {"a/src/main.cpp", "int main() { return 0; }\n"}});
        r = ws2.mcpp({"build", "--workspace"});
        expect(fatal(failed(r))) << "M2 a member's unknown mcpp: capability was not refused";
        expect(fatal(s::has_line(s::text(r), R"(ws2[/\\]a[/\\]mcpp\.toml)") && e2e::contains(s::text(r), "mcpp:no-such-layer")))
            << "M2 the refusal names the member's manifest and the capability: " << s::text(r);
        s::project ws3(tmp / "ws3", {{"mcpp.toml", "[workspace]\nmembers = [\"b\"]\n"},
                                     {"b/mcpp.toml", "[package]\nname = \"b\"\nversion = \"0.1.0\"\n\n[target.'cfg(no_such_key = \"x\")'.build]\ncxxflags = [\"-DNEVER=1\"]\n"},
                                     {"b/src/main.cpp", "int main() { return 0; }\n"}});
        r = ws3.mcpp({"build", "--workspace"});
        expect(fatal(e2e::succeeded(r))) << "M3 build: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "no_such_key"))) << "M3 a member's unknown cfg() key is not reported: " << s::text(r);
    };

    "885 workspace profiles and the root package"_test[s::unix_shell] = [] {
        // W1 the workspace's [profile.release] reaches a member; W2 a member's own profile
        // replaces it, unmerged; W3 a rooted workspace's root package receives [workspace.build]
        // once beside its own [build], in one graph, built as the root or with --workspace, and
        // the root file's profile reaches a member built on its own.
        const auto o = registry_home();
        const auto tmp = s::case_dir();
        auto profile = [](std::string_view mark) { return std::format("[profile.release]\ncxxflags = [\"-DMARK_PROFILE_{}\"]\n", mark); };
        // The variables of the graph and the build statement of `object` with its indented lines.
        auto edge = [](const fs::path& root, std::string_view object) {
            const auto g = graphs(root);
            expect(fatal(!g.empty())) << "no build.ninja";
            std::string vars, stmt;
            bool in = false;
            std::istringstream text(s::read_file(g.front()));
            for (std::string l; std::getline(text, l);) {
                if (std::regex_search(l, std::regex{"^(cxxflags|cflags) *="})) vars += l + "\n";
                if (l.starts_with("build " + std::string(object))) { in = true; stmt += l + "\n"; continue; }
                if (in && l.starts_with("  ")) { stmt += l + "\n"; continue; }
                in = false;
            }
            return vars + stmt;
        };
        const auto lib_toml = "[package]\nname = \"lib\"\nversion = \"0.1.0\"\n\n[targets.lib]\nkind = \"lib\"\n";
        const auto lib_cppm = "export module t885_lib;\nexport int lv() { return 1; }\n";
        const auto main_cpp = "import t885_lib;\nint main() { return lv() == 1 ? 0 : 1; }\n";
        s::project v(tmp / "v", {{"mcpp.toml", "[workspace]\nmembers = [\"app\", \"lib\"]\n\n" + profile("WSROOT")},
                                 {"lib/mcpp.toml", lib_toml}, {"lib/src/lib.cppm", lib_cppm}, {"app/src/main.cpp", main_cpp},
                                 {"app/mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nlib = { path = \"../lib\" }\n\n"
                                                   "[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}});
        auto r = v.mcpp({"build", "--release", "-p", "app"}, o);
        expect(fatal(e2e::succeeded(r))) << "W1: build failed: " << s::text(r);
        auto e = edge(v.dir(), "obj/app/src/main");
        expect(fatal(e2e::contains(e, "MARK_PROFILE_WSROOT"))) << "W1: the workspace's profile did not reach a member:\n" << e;
        v.write("app/mcpp.toml", v.read("app/mcpp.toml") + "\n" + profile("APP"));
        fs::remove_all(v.at("target"));
        r = v.mcpp({"build", "--release", "-p", "app"}, o);
        expect(fatal(e2e::succeeded(r))) << "W2: build failed: " << s::text(r);
        e = edge(v.dir(), "obj/app/src/main");
        expect(fatal(e2e::contains(e, "MARK_PROFILE_APP"))) << "W2: the member's own profile was not used:\n" << e;
        expect(fatal(s::lacks(e, "MARK_PROFILE_WSROOT"))) << "W2: the workspace's profile was merged into the member's own table";

        s::project root(tmp / "r", {{"lib/mcpp.toml", lib_toml}, {"lib/src/lib.cppm", lib_cppm}, {"src/main.cpp", main_cpp},
                                    {"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[build]\ncxxflags = [\"-DMARK_OWN\"]\n\n"
                                                  "[dependencies]\nlib = { path = \"lib\" }\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n"
                                                  "[workspace]\nmembers = [\"lib\"]\n\n[workspace.build]\ncxxflags = [\"-DMARK_WS_BUILD\"]\n\n" + profile("ROOTFILE")}});
        for (const auto& selection : std::vector<std::vector<std::string>>{{}, {"--workspace"}}) {
            const auto label = selection.empty() ? std::string("root") : selection[0];
            fs::remove_all(root.at("target"));
            std::vector<std::string> args{"build", "--release"};
            args.insert(args.end(), selection.begin(), selection.end());
            r = root.mcpp(args, o);
            expect(fatal(e2e::succeeded(r))) << "W3: build (" << label << ") failed: " << s::text(r);
            expect(fatal(graphs(root.dir()).size() == 1u)) << "W3 (" << label << "): " << graphs(root.dir()).size() << " graphs, expected one";
            e = edge(root.dir(), "obj/app/src/main");
            expect(fatal(e2e::contains(e, "MARK_WS_BUILD"))) << "W3 (" << label << "): the root package did not receive [workspace.build]:\n" << e;
            expect(fatal(e2e::contains(e, "MARK_OWN"))) << "W3 (" << label << "): the root package lost its own [build]:\n" << e;
            std::istringstream lines(e);
            for (std::string l; std::getline(lines, l);) {
                std::size_t n = 0;
                for (auto at = l.find("MARK_WS_BUILD"); at != std::string::npos; at = l.find("MARK_WS_BUILD", at + 1)) ++n;
                expect(fatal(n <= 1u)) << "W3 (" << label << "): [workspace.build] was applied more than once:\n" << e;
            }
        }
        fs::remove_all(root.at("target"));
        r = root.mcpp({"build", "--release", "-p", "lib"}, o);
        expect(fatal(e2e::succeeded(r))) << "W3: build -p lib failed: " << s::text(r);
        bool reached = false;
        for (const auto& g : s::glob(root.at("target"), "*/*/build.ninja")) reached = reached || e2e::contains(s::read_file(g), "MARK_PROFILE_ROOTFILE");
        expect(fatal(reached)) << "W3: the root file's profile did not reach a member built on its own";
    };

    "893 the workspace prefix carries the meaning"_test[s::gcc] = [] {
        // [workspace.package], [workspace.target.<cfg>.build] and [workspace.profile.release]
        // reach the members, a member's own conditional define and profile coming after; an
        // unknown [workspace] table is refused by name; one table written twice is refused
        // naming the spelling; [build] on a root without [package] warns (W7), --strict refuses.
        const auto tmp = s::case_dir();
        const auto app_toml = R"([package]
name = "app"
version = "0.1.0"

[dependencies]
lib = { path = "../lib" }

[target.'cfg(any(os = "linux", os = "macos", os = "windows"))'.build]
defines = ["WS_MACRO=2"]

[profile.release]
cxxflags = ["-DAPP_RELEASE=1"]

[targets.app]
kind = "bin"
main = "src/main.cpp"
)";
        s::project ws(tmp / "ws", {
            {"mcpp.toml", R"([workspace]
members = ["lib", "app"]

[workspace.package]
mcpp = ">=2026.10.10.1"

[workspace.target.'cfg(any(os = "linux", os = "macos", os = "windows"))'.build]
dialect_cxxflags = ["-DWS_DIALECT=1"]
cxxflags = ["-DWS_PACKAGE=1"]
defines = ["WS_MACRO=1"]

[workspace.profile.release]
cxxflags = ["-DWS_RELEASE=1"]
)"},
            {"lib/mcpp.toml", "[package]\nname = \"lib\"\nversion = \"0.1.0\"\n\n[targets.lib]\nkind = \"lib\"\n"},
            {"lib/src/lib.cppm", "module;\n#if !defined(WS_DIALECT) || !defined(WS_PACKAGE)\n#error \"the workspace layer did not reach lib\"\n#endif\n"
                                 "export module lib;\nexport int lib_value() { return WS_MACRO; }\n"},
            {"app/mcpp.toml", app_toml},
            {"app/src/main.cpp", "#if !defined(WS_DIALECT) || !defined(WS_PACKAGE)\n#error \"the workspace layer did not reach app\"\n#endif\n"
                                 "#include <cstdio>\nimport lib;\nint main() { std::printf(\"macro=%d lib=%d\\n\", WS_MACRO, lib_value()); }\n"},
        });
        auto r = ws.mcpp({"build", "-p", "app"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "the workspace layer build failed: " << s::text(r);
        const auto app = bin_of(ws.at("target"), "app");
        expect(fatal(app.has_value())) << "no app binary";
        auto got = s::run(*app).out;
        std::erase(got, '\r');
        expect(fatal(got == "macro=2 lib=1\n")) << "expected app's own WS_MACRO=2 and lib's inherited WS_MACRO=1, got '" << got << "'";
        expect(fatal(s::no_line(s::text(r), "workspace-position|acts on no package"))) << "the canonical spelling was reported as a deprecated or inert one: " << s::text(r);
        r = ws.mcpp({"build", "-p", "app", "--release"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "release build failed: " << s::text(r);
        std::string entry;
        for (const auto& db : s::find_files(ws.at("target"), R"(compile_commands\.json)")) {
            const auto doc = s::json_file(db);
            for (const auto& en : doc) {
                auto file = en.value("file", std::string{});
                std::ranges::replace(file, '\\', '/');
                if (!file.ends_with("app/src/main.cpp")) continue;
                const auto words = s::cdb_arguments(db, "app/src/main.cpp");
                if (words && std::ranges::any_of(*words, [](const std::string& w) { return w.contains("APP_RELEASE"); })) {
                    for (const auto& w : *words) entry += w + " ";
                }
            }
            if (!entry.empty()) break;
        }
        expect(fatal(std::regex_search(entry, std::regex{"-DWS_RELEASE=1.*-DAPP_RELEASE=1"})))
            << "[workspace.profile.release] and the member's [profile.release] did not merge, workspace first: " << entry;

        s::project unknown(tmp / "unknown", {{"app/mcpp.toml", app_toml}, {"mcpp.toml", "[workspace]\nmembers = [\"app\"]\n[workspace.tolchain]\ndefault = \"gcc@16.1.0\"\n"}});
        r = unknown.mcpp({"build", "-p", "app"}, inherited);
        expect(fatal(failed(r))) << "an unknown [workspace] table was accepted";
        expect(fatal(e2e::contains(s::text(r), "[workspace] has no key or table 'tolchain'"))) << "the refusal does not name the table: " << s::text(r);
        s::project twice(tmp / "twice", {{"app/mcpp.toml", app_toml},
                                         {"mcpp.toml", "[workspace]\nmembers = [\"app\"]\n[toolchain]\ndefault = \"gcc@16.1.0\"\n[workspace.toolchain]\ndefault = \"gcc@16.1.0\"\n"}});
        r = twice.mcpp({"build", "-p", "app"}, inherited);
        expect(fatal(failed(r))) << "one table written twice was accepted";
        expect(fatal(e2e::contains(s::text(r), "write it once, as [workspace.toolchain]"))) << "the refusal does not say which spelling: " << s::text(r);
        s::project inert(tmp / "inert", {{"app/mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                         {"app/src/main.cpp", "int main() { return 0; }\n"},
                                         {"mcpp.toml", "[workspace]\nmembers = [\"app\"]\n[build]\ncxxflags = [\"-DNOWHERE=1\"]\n"}});
        r = inert.mcpp({"build", "-p", "app"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "W7 is a warning, not a refusal: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "[build] is written on a workspace root without [package]"))) << "W7 said nothing: " << s::text(r);
        r = inert.mcpp({"build", "-p", "app", "--strict"}, inherited);
        expect(fatal(failed(r))) << "--strict accepted W7: " << s::text(r);
    };

    "900 publish writes the workspace layer into the member"_test[s::gcc][s::unix_shell] = [] {
        // The archived manifest of a published member is what it was built with: the
        // workspace's [toolchain], its conditional [build] merged before the member's, its
        // include_dirs rebased to the member; a workspace path outside the package is refused.
        const auto tmp = s::case_dir();
        s::project repo(tmp / "repo", {
            {"mcpp.toml", R"([workspace]
members = ["util"]

[workspace.package]
version = "0.3.0"
license = "MIT"
repo    = "https://github.com/example/probe"

[workspace.toolchain]
default = "gcc@16.1.0"

[workspace.target.'cfg(os = "linux")'.build]
cxxflags = ["-DWS_COND=1"]
include_dirs = ["util/inc"]
)"},
            {"util/mcpp.toml", "[package]\nnamespace = \"probe\"\nname = \"util\"\n\n[target.'cfg(os = \"linux\")'.build]\ncxxflags = [\"-DOWN_COND=1\"]\n\n[targets.probe_util]\nkind = \"lib\"\n"},
            {"util/src/util.cppm", "export module probe.util;\nexport int util_value() { return 42; }\n"},
        });
        fs::create_directories(repo.at("util/inc"));
        const auto git = s::on_path("git");
        if (!git) skip("git is not installed");
        for (std::vector<std::string> args : {std::vector<std::string>{"init", "-q", "-b", "main", "."}, {"add", "-A"},
                                              {"-c", "user.email=e2e@example.invalid", "-c", "user.name=e2e", "commit", "-qm", "init"}}) {
            auto g = s::run(*git, args, repo.dir());
            expect(fatal(e2e::succeeded(g))) << "git failed: " << s::text(g);
        }
        auto r = repo.mcpp_in("util", {"publish", "--dry-run", "--allow-dirty"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "publish of a member with a workspace layer failed: " << s::text(r);
        const auto archive = repo.at("util/target/dist/util-0.3.0.tar.gz");
        expect(fatal(fs::is_regular_file(archive))) << "no archive at " << archive;
        s::tar_extract(archive, tmp / "out", {"util-0.3.0/mcpp.toml"});
        // The reading is Python's tomllib, as the script's was: the support has no TOML reader.
        const auto python = s::on_path("python3");
        expect(fatal(python.has_value())) << "python3 is needed to read the archived manifest";
        s::project(tmp / "check", {{"check.py", R"(import sys, tomllib
m = tomllib.load(open(sys.argv[1], "rb"))
assert m["toolchain"]["default"] == "gcc@16.1.0", m.get("toolchain")
row = m["target"]['cfg(os = "linux")']["build"]
assert row["cxxflags"] == ["-DWS_COND=1", "-DOWN_COND=1"], row
assert row["include_dirs"] == ["inc"], row
)"}});
        auto c = s::run(*python, {"-I", (tmp / "check/check.py").string(), (tmp / "out/util-0.3.0/mcpp.toml").string()});
        expect(fatal(e2e::succeeded(c))) << "the archived manifest is not what the member was built with: " << s::text(c) << "\n" << s::read_file(tmp / "out/util-0.3.0/mcpp.toml");
        auto text = repo.read("mcpp.toml");
        text.replace(text.find(R"(include_dirs = ["util/inc"])"), std::string_view(R"(include_dirs = ["util/inc"])").size(), R"(include_dirs = ["shared/inc"])");
        repo.write("mcpp.toml", text);
        fs::create_directories(repo.at("shared/inc"));
        r = repo.mcpp_in("util", {"publish", "--dry-run", "--allow-dirty"}, inherited);
        expect(fatal(failed(r))) << "a workspace path outside the package was published: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "outside this package's directory"))) << "the refusal does not say why: " << s::text(r);
    };

    "911 a workspace states each table once"_test[s::unix_shell] = [] {
        // R4 a rooted workspace's [workspace.xlings.overrides] serves the root and not the
        // member; R5 the root's own [xlings.overrides] is its own, and a member built alone is
        // told the position reading is deprecated; V [workspace.xlings.overrides] serves a member
        // of a root without [package], from either directory; W1 a build.mcpp beside such a root
        // is reported; W2 [workspace.dependencies] carries features and reports a request key.
        const auto tmp = s::case_dir();
        const s::options o{.env = {{"NO_COLOR", "1"}}};
        const s::options no_install{.env = {{"NO_COLOR", "1"}, {"MCPP_NO_AUTO_INSTALL", "1"}}};
        const auto tool = (tmp / "opt/bin/tool").generic_string();
        s::write_executable(tmp / "opt/bin/tool", "#!/bin/sh\necho tool\n");
        auto rooted = [&](const fs::path& dir, const std::string& tables) {
            return s::project(dir, {
                {"mcpp.toml", "[package]\nname = \"root\"\nversion = \"0.1.0\"\n[dependencies]\nsub = { path = \"sub\" }\n[targets.root]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"
                              "[workspace]\nmembers = [\"sub\"]\n" + tables + "\n"},
                {"src/main.cpp", "import sub;\nint main() { return f() - 1; }\n"},
                {"sub/mcpp.toml", "[package]\nname = \"sub\"\nversion = \"0.1.0\"\n[targets.sub]\nkind = \"lib\"\n"},
                {"sub/src/sub.cppm", "export module sub;\nexport int f() { return 1; }\n"},
            });
        };
        auto r4 = rooted(tmp / "r4", "[workspace.xlings.overrides]\n\"xim:mcpp-e2e-absent-tool\" = \"" + tool + "\"");
        auto r = r4.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "R4: the workspace's overrides failed its own build: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "is a dependency of this build"))) << "R4: the member received the overrides: " << s::text(r);
        auto r5 = rooted(tmp / "r5", "[xlings.overrides]\n\"xim:mcpp-e2e-absent-tool\" = \"" + tool + "\"");
        r = r5.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "R5: the root's own overrides failed its own build: " << s::text(r);
        expect(fatal(s::no_line(s::text(r), "workspace-position|deprecated since"))) << "R5: the root's own build was told about a position reading: " << s::text(r);
        r = r5.mcpp_in("sub", {"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "R5: the member did not build alone: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "deprecated since mcpp 2026.10.10.1 and removed from mcpp 2027.1")))
            << "R5: the member built alone was not told the position reading is deprecated: " << s::text(r);

        s::project v(tmp / "v", {
            {"mcpp.toml", "[workspace]\nmembers = [\"m\"]\n[workspace.xlings.overrides]\n\"xim:mcpp-e2e-absent-tool\" = { program = \"" + tool + "\", version = \"2.1\" }\n"},
            {"m/mcpp.toml", "[package]\nname = \"m\"\nversion = \"0.1.0\"\n[xlings.workspace]\n\"xim:mcpp-e2e-absent-tool\" = \">=2.0\"\n[targets.m]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
            {"m/src/main.cpp", "int main() { return 0; }\n"},
        });
        r = v.mcpp({"build"}, no_install);
        expect(fatal(e2e::succeeded(r))) << "V: [workspace.xlings.overrides] did not serve a member of a root without [package]: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "custom: xim:mcpp-e2e-absent-tool"))) << "V: the build does not state the source: " << s::text(r);
        r = v.mcpp_in("m", {"build"}, no_install);
        expect(fatal(e2e::succeeded(r))) << "V: the member built from its own directory was not served: " << s::text(r);

        s::project w1(tmp / "w1", {
            {"mcpp.toml", "[workspace]\nmembers = [\"m\"]\n"},
            {"build.mcpp", "int main() { return 1; }\n"},
            {"m/mcpp.toml", "[package]\nname = \"m\"\nversion = \"0.1.0\"\n[targets.m]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
            {"m/src/main.cpp", "int main() { return 0; }\n"},
        });
        r = w1.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "W1: the workspace did not build: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "build.mcpp is written beside a workspace root without [package]"))) << "W1: the build.mcpp beside the root was not reported: " << s::text(r);

        s::project w2(tmp / "w2", {
            {"hl/mcpp.toml", "[package]\nname = \"hl\"\nversion = \"0.1.0\"\n[features]\nx = []\n[targets.hl]\nkind = \"lib\"\n"},
            {"hl/src/hl.cppm", "export module hl;\nexport int feature_x() {\n#ifdef MCPP_FEATURE_X\n    return 1;\n#else\n    return 0;\n#endif\n}\n"},
            {"mcpp.toml", "[workspace]\nmembers = [\"m\"]\n[workspace.dependencies]\nhl = { path = \"hl\", features = [\"x\"], linkage = \"static\" }\n"},
            {"m/mcpp.toml", "[package]\nname = \"m\"\nversion = \"0.1.0\"\n[dependencies]\nhl.workspace = true\n[targets.m]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
            {"m/src/main.cpp", "#include <cstdio>\nimport hl;\nint main() { std::printf(\"X=%d\\n\", feature_x()); }\n"},
        });
        r = w2.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "W2: the workspace did not build: " << s::text(r);
        expect(fatal(s::last_starting(w2.mcpp({"run", "-p", "m"}, o).out, "X=") == "X=1")) << "W2: the feature stated in [workspace.dependencies] did not reach the member";
        expect(fatal(e2e::contains(s::text(r), "'linkage' is a request of a member's own edge"))) << "W2: the request key in [workspace.dependencies] was not reported: " << s::text(r);
    };
}};

}  // namespace
