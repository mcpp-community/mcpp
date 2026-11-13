// Which members a command takes: a workspace built from its root, `run -p` and `build -p` by
// package and by directory, a selected member that declares as the root, a repeated `-p`, and
// `run` with one member and `--exclude` (formerly tests/e2e/35, 122, 806, 850, 852, 853).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

const s::options inherited_cdb{.where = s::home::inherited, .env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

// git in `dir` with a committer: its standard output.
std::string git(const fs::path& dir, std::vector<std::string> args) {
    const auto program = s::on_path("git");
    if (!program) skip("git is not installed");
    args.insert(args.begin(), {"-C", dir.string(), "-c", "user.email=test@local", "-c", "user.name=test"});
    auto r = s::run(*program, args);
    expect(fatal(e2e::succeeded(r))) << "git failed: " << s::text(r);
    return r.out;
}

// The first executable `name` under `root` in a `bin` directory (`find -path '*/bin/*' -name`).
std::optional<fs::path> bin_of(const fs::path& root, std::string_view name) {
    auto all = s::find_files(root, std::regex_replace(s::exe(name), std::regex{R"([.])"}, R"(\.)"));
    std::ranges::sort(all);
    for (const auto& f : all)
        if (f.generic_string().contains("/bin/")) return f;
    return std::nullopt;
}

// The program's standard output without carriage returns.
std::string output_of(const std::optional<fs::path>& program) {
    expect(fatal(program.has_value())) << "no such program was built";
    auto out = s::run(*program).out;
    std::erase(out, '\r');
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

// The directory of the first build.ninja under target, outside the build programs' tree.
std::optional<fs::path> build_dir(const fs::path& root) {
    auto all = s::find_files(root / "target", R"(build\.ninja)");
    std::ranges::sort(all);
    for (const auto& f : all)
        if (!f.generic_string().contains("/.build-mcpp/")) return f.parent_path();
    return std::nullopt;
}

std::size_t graphs(const fs::path& root) {
    std::size_t n = 0;
    for (const auto& f : s::find_files(root / "target", R"(build\.ninja)"))
        if (!f.generic_string().contains("/.build-mcpp/")) ++n;
    return n;
}

E2E members{"members", [] {
    "35 workspace"_test = [] {
        // A virtual workspace of two libraries and a program: the root's build builds every
        // member, path edges between members hold, and the program is bin/hello/hello in the
        // root's build directory; no member receives a build directory.
        s::project ws({
            {"mcpp.toml", "[workspace]\nmembers = [\"libs/core\", \"libs/greeter\", \"apps/hello\"]\n"},
            {"libs/core/mcpp.toml", "[package]\nnamespace = \"demo\"\nname      = \"core\"\nversion   = \"0.1.0\"\n\n[targets.core]\nkind = \"lib\"\n"},
            {"libs/core/src/core.cppm", "export module demo.core;\nimport std;\nexport namespace demo::core {\n    inline std::string greet_target() { return \"World\"; }\n}\n"},
            {"libs/greeter/mcpp.toml", "[package]\nnamespace = \"demo\"\nname      = \"greeter\"\nversion   = \"0.1.0\"\n\n[targets.greeter]\nkind = \"lib\"\n\n[dependencies]\ncore = { path = \"../core\" }\n"},
            {"libs/greeter/src/greeter.cppm", "export module demo.greeter;\nimport std;\nimport demo.core;\nexport namespace demo::greeter {\n"
                                              "    inline std::string greet() {\n        return \"Hello, \" + demo::core::greet_target() + \"!\";\n    }\n}\n"},
            {"apps/hello/mcpp.toml", "[package]\nnamespace = \"demo\"\nname      = \"hello\"\nversion   = \"0.1.0\"\n\n[dependencies]\ngreeter = { path = \"../../libs/greeter\" }\n"},
            {"apps/hello/src/main.cpp", "import std;\nimport demo.greeter;\nint main() {\n    std::println(\"{}\", demo::greeter::greet());\n    return 0;\n}\n"},
        });
        auto r = ws.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        std::optional<fs::path> bin;
        for (const auto& f : s::find_files(ws.at("target"), std::regex_replace(s::exe("hello"), std::regex{R"([.])"}, R"(\.)")))
            if (f.generic_string().ends_with("/bin/hello/" + s::exe("hello"))) bin = f;
        expect(fatal(bin.has_value())) << "hello binary not found in target/<triple>/<key>/bin/hello/";
        expect(fatal(!fs::exists(ws.at("apps/hello/target")))) << "a member directory received a build directory";
        auto out = s::text(s::run(*bin));
        std::erase(out, '\r');
        expect(fatal(out == "Hello, World!\n")) << "unexpected output '" << out << "'";
    };

    "122 run member"_test = [] {
        // `run -p <member>` builds and runs that member's program, standard output the
        // program's alone; an unknown member is refused by name.
        s::project ws({
            {"mcpp.toml", "[workspace]\nmembers = [\"memberA\", \"memberB\"]\n"},
            {"memberA/mcpp.toml", "[package]\nname = \"memberA\"\nversion = \"0.1.0\"\n\n[targets.memberA]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
            {"memberA/src/main.cpp", "import std;\nint main() { std::println(\"hello from memberA\"); return 0; }\n"},
            {"memberB/mcpp.toml", "[package]\nname = \"memberB\"\nversion = \"0.1.0\"\n\n[targets.memberB]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
            {"memberB/src/main.cpp", "import std;\nint main() { std::println(\"hello from memberB\"); return 0; }\n"},
        });
        auto r = ws.mcpp({"run", "-p", "memberB"});
        expect(fatal(e2e::succeeded(r))) << "run -p memberB failed: " << r.err;
        expect(fatal(r.out == "hello from memberB\n")) << "expected memberB's output, got: " << r.out;
        r = ws.mcpp({"run", "-p", "memberA"});
        expect(fatal(e2e::succeeded(r))) << "run -p memberA failed: " << r.err;
        expect(fatal(r.out == "hello from memberA\n")) << "expected memberA's output, got: " << r.out;
        r = ws.mcpp({"run", "-p", "nope"});
        expect(fatal(failed(r))) << "run -p nope should have failed";
        expect(fatal(s::contains_nocase(s::text(r), "nope"))) << "expected an error mentioning the unknown member 'nope': " << s::text(r);
    };

    "806 dash p resolves the package first"_test[tag{"requires:gcc"}] = [] {
        // `-p` names a package first, then a directory: the package name, the last directory
        // component and the path each select a member, without a warning when nothing else could
        // match; a name two namespaces share is refused naming both qualified names; a directory
        // spelled as another member's package name warns and still selects the package.
        s::needs({"gcc"});
        s::project ws(s::case_dir() / "ws", {{"mcpp.toml", "[workspace]\nmembers = [\"modules/base\", \"ns1/common\", \"ns2/common\"]\n"}});
        auto member = [&](std::string dir, std::string name, std::string ns) {
            ws.write(dir + "/mcpp.toml", std::format("[package]\n{}name = \"{}\"\nversion = \"0.1.0\"\n\n[targets.{}]\nkind = \"lib\"\n\n[build]\nsources = [\"x.cpp\"]\n",
                                                     ns.empty() ? "" : "namespace = \"" + ns + "\"\n", name, name));
            ws.write(dir + "/x.cpp", std::format("int {}_x() {{ return 0; }}\n", std::regex_replace(name, std::regex{"[^a-zA-Z0-9_]"}, "_")));
        };
        member("modules/base", "ws-base", "");
        member("ns1/common", "ws-common", "ns1");
        member("ns2/common", "ws-common", "ns2");
        auto fresh = [&] {
            fs::remove_all(ws.at("target"));
            fs::remove(ws.at("compile_commands.json"));
        };
        auto built = [&](std::string_view dir) {
            return ws.exists("compile_commands.json") && e2e::contains(ws.read("compile_commands.json"), std::string(dir) + "/x.cpp");
        };
        for (std::string filter : {"ws-base", "base", "modules/base"}) {
            fresh();
            auto r = ws.mcpp({"build", "-p", filter}, inherited_cdb);
            expect(fatal(e2e::succeeded(r))) << "-p " << filter << " did not build: " << s::text(r);
            expect(fatal(built("modules/base"))) << "-p " << filter << " did not select modules/base";
            expect(fatal(!s::contains_nocase(s::text(r), "warning"))) << "-p " << filter << " warned when no other member could conflict: " << s::text(r);
        }
        auto r = ws.mcpp({"build", "-p", "ws-common"}, inherited_cdb);
        expect(fatal(failed(r))) << "-p ws-common should have been refused as ambiguous";
        expect(fatal(e2e::contains(s::text(r), "ns1.ws-common"))) << "the ambiguity refusal must name 'ns1.ws-common': " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "ns2.ws-common"))) << "the ambiguity refusal must name 'ns2.ws-common': " << s::text(r);
        for (std::string ns : {"ns1", "ns2"}) {
            fresh();
            r = ws.mcpp({"build", "-p", ns + ".ws-common"}, inherited_cdb);
            expect(fatal(e2e::succeeded(r))) << "-p " << ns << ".ws-common did not build: " << s::text(r);
            expect(fatal(built(ns + "/common"))) << "-p " << ns << ".ws-common did not select " << ns << "/common";
        }
        member("dup/ws-base", "other", "");
        ws.write("mcpp.toml", "[workspace]\nmembers = [\"modules/base\", \"ns1/common\", \"ns2/common\", \"dup/ws-base\"]\n");
        fresh();
        r = ws.mcpp({"build", "-p", "ws-base"}, inherited_cdb);
        expect(fatal(e2e::succeeded(r))) << "-p ws-base did not build once a same-named directory existed: " << s::text(r);
        expect(fatal(built("modules/base"))) << "-p ws-base must still select the package 'ws-base' (modules/base)";
        expect(fatal(!built("dup/ws-base"))) << "-p ws-base must not have built dup/ws-base";
        expect(fatal(s::contains_nocase(s::text(r), "warning"))) << "-p ws-base must warn once a directory shares its spelling: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "dup/ws-base"))) << "the warning must name the other member's path ('dup/ws-base'): " << s::text(r);
    };

    "850 a selected member declares as the root"_test[tag{"requires:gcc"}] = [] {
        // A: a rooted workspace whose root and member declare one dependency by path while a
        // shared dependency reaches it by git: the root's declaration wins, said so. B: `-p tool`
        // gives the selected member the same. C: two selected members that declare one dependency
        // by two kinds, or by two directories, are refused naming both. D, F: a member's linkage
        // request is honoured and its own dependencies reach the index refresh. E: two members
        // asking two forms of one dependency are refused naming both; one alone builds.
        s::needs({"gcc"});
        const auto tmp = s::case_dir();
        const auto fw = tmp / "framework";
        fs::create_directories(fw / "src");
        git(fw, {"init", "--quiet"});
        s::project framework(fw, {
            {"mcpp.toml", "[package]\nname    = \"framework\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.c\"]\n\n[targets.framework]\nkind = \"lib\"\n"},
            {"src/framework.c", "int framework_marker(void) { return 101; }\n"},
        });
        git(fw, {"add", "-A"});
        git(fw, {"commit", "--quiet", "-m", "A"});
        auto rev_a = git(fw, {"rev-parse", "HEAD"});
        rev_a.erase(rev_a.find_last_not_of("\r\n") + 1);
        framework.write("src/framework.c", "int framework_marker(void) { return 199; }\n");
        const auto fw_host = fw.generic_string();
        s::project libg(tmp / "libg", {
            {"mcpp.toml", std::format("[package]\nname    = \"libg\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.c\"]\n\n"
                                      "[targets.libg]\nkind = \"lib\"\n\n[dependencies.framework]\ngit = \"{}\"\nrev = \"{}\"\n",
                                      fw_host, rev_a)},
            {"src/libg.c", "extern int framework_marker(void);\nint libg_marker(void) { return framework_marker(); }\n"},
        });
        auto package = [&](const fs::path& dir, std::string_view name, std::string_view extra) {
            s::project(dir, {
                {"src/main.cpp", "#include <cstdio>\nextern \"C\" int libg_marker(void);\nint main() { std::printf(\"libg=%d\\n\", libg_marker()); return 0; }\n"},
                {"mcpp.toml", std::format("[package]\nname    = \"{0}\"\nversion = \"0.1.0\"\n\n[dependencies]\nlibg = {{ path = \"{1}\" }}\n{2}\n\n"
                                          "[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n",
                                          name, libg.dir().generic_string(), extra)},
            });
        };
        const auto by_path = std::format("framework = {{ path = \"{}\" }}", fw_host);
        const auto w1 = tmp / "w1";
        package(w1, "app", by_path);
        package(w1 / "tool", "tool", by_path);
        s::project ws1(w1, {});
        ws1.write("mcpp.toml", ws1.read("mcpp.toml") + "\n[workspace]\nmembers = [\".\", \"tool\"]\n");
        auto r = ws1.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "A: the rooted workspace was refused: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "the root's declaration wins"))) << "A: no statement that the root's declaration wins: " << s::text(r);
        expect(fatal(output_of(bin_of(w1 / "target", "app")) == "libg=199")) << "A: the program does not read the path checkout";
        r = ws1.mcpp({"build", "-p", "tool"});
        expect(fatal(e2e::succeeded(r))) << "B: -p tool was refused: " << s::text(r);
        expect(fatal(output_of(bin_of(w1 / "target", "tool")) == "libg=199")) << "B: the selected member does not read the path checkout";

        const auto w2 = tmp / "w2";
        s::project ws2(w2, {{"mcpp.toml", "[workspace]\nmembers = [\"one\", \"two\"]\n"}});
        package(w2 / "one", "one", by_path);
        package(w2 / "two", "two", std::format("framework = {{ git = \"{}\", rev = \"{}\" }}", fw_host, rev_a));
        auto names_both = [](std::string_view text) {
            return e2e::contains(text, "two members this build selects") && std::regex_search(std::string(text), std::regex{"'[a-z.]*one@path'"}) &&
                   std::regex_search(std::string(text), std::regex{"'[a-z.]*two@path'"});
        };
        r = ws2.mcpp({"build", "--workspace"});
        expect(fatal(failed(r))) << "C: two members that declare one dependency by two kinds were accepted: " << s::text(r);
        expect(fatal(names_both(s::text(r)))) << "C: the refusal does not name both members: " << s::text(r);
        git(tmp, {"clone", "--quiet", fw.string(), (tmp / "framework-copy").string()});
        package(w2 / "two", "two", std::format("framework = {{ path = \"{}\" }}", (tmp / "framework-copy").generic_string()));
        r = ws2.mcpp({"build", "--workspace"});
        expect(fatal(failed(r))) << "C: two members that point a dependency at two directories were accepted: " << s::text(r);
        expect(fatal(names_both(s::text(r)))) << "C: the refusal of two references does not name both members: " << s::text(r);

        s::project ws3(tmp / "w3", {
            {"mcpp.toml", "[workspace]\nmembers = [\"tool\"]\n"},
            {"libs/mcpp.toml", "[package]\nname    = \"libs\"\nversion = \"0.1.0\"\n\n[targets.libs]\nkind = \"lib\"\n"},
            {"libs/src/libs.cpp", "int libs_value() { return 7; }\n"},
            {"tool/mcpp.toml", "[package]\nname    = \"tool\"\nversion = \"0.1.0\"\n\n[dependencies]\nlibs = { path = \"../libs\", linkage = \"shared\" }\n\n"
                               "[targets.tool]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
            {"tool/src/main.cpp", "#include <cstdio>\nint libs_value();\nint main() { std::printf(\"%d\\n\", libs_value()); }\n"},
        });
        r = ws3.mcpp({"build", "-v"});
        expect(fatal(e2e::succeeded(r))) << "D: the member with a linkage request did not build: " << s::text(r);
        const auto tool = bin_of(ws3.at("target"), "tool");
        expect(fatal(tool.has_value())) << "D: no tool program";
        bool shared = false;
        for (const auto& e : fs::directory_iterator(tool->parent_path())) {
            const auto n = e.path().filename().string();
            shared = shared || n.starts_with("liblibs.so") || (n.starts_with("liblibs") && n.ends_with(".dylib")) || n == "libs.dll" || n == "liblibs.dll";
        }
        expect(fatal(shared)) << "D: the dependency is not a shared library beside the member's program: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "only the root project decides link forms"))) << "D: the member's request was reported as ignored";
        expect(fatal(output_of(tool) == "7")) << "D: the program does not run";
        expect(fatal(e2e::contains(s::text(r), "index: libs:"))) << "F: the index refresh did not consider the member's own dependency: " << s::text(r);

        auto tool2 = ws3.read("tool/mcpp.toml");
        tool2 = std::regex_replace(tool2, std::regex{"\"tool\""}, "\"tool2\"", std::regex_constants::format_first_only);
        tool2 = std::regex_replace(tool2, std::regex{"targets\\.tool"}, "targets.tool2", std::regex_constants::format_first_only);
        tool2 = std::regex_replace(tool2, std::regex{"linkage = \"shared\""}, "linkage = \"static\"", std::regex_constants::format_first_only);
        ws3.write("tool2/mcpp.toml", tool2);
        ws3.write("tool2/src/main.cpp", ws3.read("tool/src/main.cpp"));
        ws3.write("mcpp.toml", "[workspace]\nmembers = [\"tool\", \"tool2\"]\n");
        r = ws3.mcpp({"build", "--workspace"});
        expect(fatal(failed(r))) << "E: two members asking for two forms of one dependency were accepted: " << s::text(r);
        const auto e = s::text(r);
        expect(fatal(s::has_line(e, "linked as '(shared|static)' by 'tool2?' and as '(shared|static)' by 'tool2?'") && e2e::contains(e, "'tool'") &&
                     e2e::contains(e, "'tool2'") && e2e::contains(e, "'shared'") && e2e::contains(e, "'static'")))
            << "E: the refusal does not name both members and both forms: " << e;
        r = ws3.mcpp({"build", "-p", "tool2"});
        expect(fatal(e2e::succeeded(r))) << "E: one member alone did not build: " << s::text(r);
    };

    "852 a repeated dash p selects every member it names"_test[tag{"requires:unix-shell"}] = [] {
        // A repeated -p selects every member it names, in any order and named twice, in one
        // build directory; a -p that names no member is refused before planning, listing the
        // members, and leaves no build directory.
        s::project ws({{"mcpp.toml", "[workspace]\nmembers = [\"a\", \"b\", \"c\"]\n"}});
        for (std::string m : {"a", "b", "c"}) {
            ws.write(m + "/mcpp.toml", std::format("[package]\nname    = \"{0}\"\nversion = \"0.1.0\"\n\n[targets.{0}]\nkind = \"lib\"\n", m));
            ws.write(m + "/src/" + m + ".cppm", std::format("export module sel852_{0};\nexport int {0}_value() {{ return 1; }}\n", m));
        }
        auto object_edges = [](const fs::path& dir, const std::string& m) {
            const std::regex re{"(^|/)obj/" + m + "/src/" + m + "\\.m\\.o$"};
            return std::ranges::count_if(s::ninja_log_outputs(dir), [&](const std::string& o) { return std::regex_search(o, re); });
        };
        auto r = ws.mcpp({"build", "-p", "a", "-p", "nosuch"});
        expect(fatal(failed(r))) << "C: a -p that names no member was accepted";
        const auto c = s::text(r);
        expect(fatal(e2e::contains(c, "nosuch"))) << "C: the refusal does not name the member it could not find: " << c;
        for (std::string_view m : {"'a'", "'b'", "'c'"}) expect(fatal(e2e::contains(c, m))) << "C: the refusal does not list member " << m << ": " << c;
        expect(fatal(s::lacks(c, "Resolving toolchain"))) << "C: the command planned before it refused";
        expect(fatal(!fs::is_directory(ws.at("target")))) << "C: the refused command left a build directory";

        r = ws.mcpp({"build", "-p", "a", "-p", "b"});
        expect(fatal(e2e::succeeded(r))) << "A: -p a -p b did not build: " << s::text(r);
        const auto dir = build_dir(ws.dir());
        expect(fatal(dir.has_value())) << "A: no build directory";
        expect(fatal(object_edges(*dir, "a") == 1)) << "A: a was not built";
        expect(fatal(object_edges(*dir, "b") == 1)) << "A: b was not built (the repeated -p kept one value)";
        expect(fatal(!fs::exists(*dir / "obj/c"))) << "A: c was built although no -p named it";
        expect(fatal(e2e::contains(s::text(r), "Workspace building 2 members: a, b"))) << "A: the plan does not state the two members: " << s::text(r);
        r = ws.mcpp({"build", "-p", "b", "-p", "a"});
        expect(fatal(e2e::succeeded(r))) << "B: -p b -p a did not build: " << s::text(r);
        expect(fatal(object_edges(*dir, "a") == 1 && object_edges(*dir, "b") == 1)) << "B: the reverse order compiled an edge again";
        r = ws.mcpp({"build", "-p", "a", "-p", "./b", "-p", "b", "-p", "a"});
        expect(fatal(e2e::succeeded(r))) << "B: a member named twice did not build: " << s::text(r);
        expect(fatal(object_edges(*dir, "a") == 1 && object_edges(*dir, "b") == 1)) << "B: naming a member twice compiled an edge again";
        expect(fatal(graphs(ws.dir()) == 1u)) << "B: the order made a second build directory";
    };

    "853 run keeps one member and exclude removes members"_test[tag{"requires:unix-shell"}] = [] {
        // D: `run` takes one member (two -p are refused naming both). E: --exclude removes
        // members from --workspace and at a virtual root, and is refused when it names no member,
        // beside -p, with nothing left, outside a workspace selection; --workspace with -p is
        // refused naming both. F: `test` and `emit build-database` take the same selection.
        // Every refusal comes before planning and leaves no build directory.
        s::project ws({{"mcpp.toml", "[workspace]\nmembers = [\"a\", \"b\", \"c\"]\n"}});
        for (std::string m : {"a", "b", "c"}) {
            ws.write(m + "/mcpp.toml", std::format("[package]\nname    = \"{0}\"\nversion = \"0.1.0\"\n\n[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", m));
            ws.write(m + "/src/main.cpp", std::format("#include <cstdio>\nint main() {{ std::puts(\"hello from {}\"); return 0; }}\n", m));
            ws.write(m + "/tests/smoke.cpp", "int main() { return 0; }\n");
        }
        auto refused = [&](std::string_view label, std::vector<std::string> args, std::string_view sub = "") {
            auto r = sub.empty() ? ws.mcpp(std::move(args)) : ws.mcpp_in(sub, std::move(args));
            expect(fatal(failed(r))) << label << ": the command was accepted: " << s::text(r);
            expect(fatal(s::lacks(s::text(r), "Resolving toolchain"))) << label << ": the command planned before it refused";
            expect(fatal(!fs::is_directory(ws.at("target")))) << label << ": the refused command left a build directory";
            return s::text(r);
        };
        auto t = refused("D", {"run", "-p", "a", "-p", "b"});
        expect(fatal(e2e::contains(t, "'a'") && e2e::contains(t, "'b'"))) << "D: the refusal does not name both members: " << t;
        auto r = ws.mcpp({"run", "-p", "a"});
        expect(fatal(e2e::succeeded(r))) << "D: -p a did not run: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "hello from a"))) << "D: -p a did not run member a";
        expect(fatal(s::lacks(s::text(r), "hello from b"))) << "D: -p a ran member b";
        fs::remove_all(ws.at("target"));

        t = refused("E1", {"build", "--workspace", "--exclude", "nosuch"});
        expect(fatal(e2e::contains(t, "nosuch"))) << "E: the refusal does not name the member it could not find: " << t;
        t = refused("E2", {"build", "-p", "a", "--exclude", "b"});
        expect(fatal(e2e::contains(t, "--exclude"))) << "E: the refusal does not name --exclude: " << t;
        t = refused("E3", {"build", "--workspace", "--exclude", "a", "--exclude", "b", "--exclude", "c"});
        expect(fatal(e2e::contains(t, "every member"))) << "E: the refusal does not say that nothing is left: " << t;
        t = refused("E4", {"build", "--exclude", "b"}, "a");
        expect(fatal(e2e::contains(t, "--workspace"))) << "E: the refusal does not name --workspace: " << t;
        t = refused("E6", {"build", "--workspace", "-p", "a"});
        expect(fatal(e2e::contains(t, "--workspace") && e2e::contains(t, "-p"))) << "E: the refusal of --workspace with -p does not name both: " << t;

        r = ws.mcpp({"build", "--workspace", "--exclude", "c"});
        expect(fatal(e2e::succeeded(r))) << "E: --workspace --exclude c did not build: " << s::text(r);
        auto dir = build_dir(ws.dir());
        expect(fatal(dir && fs::exists(*dir / "obj/a") && fs::exists(*dir / "obj/b"))) << "E: a and b were not built";
        expect(fatal(!fs::exists(*dir / "obj/c"))) << "E: c was built although it was excluded";
        fs::remove_all(ws.at("target"));
        r = ws.mcpp({"build", "--exclude", "c", "--exclude", "./b"});
        expect(fatal(e2e::succeeded(r))) << "E: --exclude at a virtual root did not build: " << s::text(r);
        dir = build_dir(ws.dir());
        expect(fatal(dir && fs::exists(*dir / "obj/a"))) << "E: a was not built";
        expect(fatal(!fs::exists(*dir / "obj/b") && !fs::exists(*dir / "obj/c"))) << "E: an excluded member was built";
        fs::remove_all(ws.at("target"));

        r = ws.mcpp({"test", "--workspace", "--exclude", "c"});
        expect(fatal(e2e::succeeded(r))) << "F: test --workspace --exclude c failed: " << s::text(r);
        t = s::text(r);
        expect(fatal(e2e::contains(t, "member 'a' (1/2) ok") && e2e::contains(t, "member 'b' (2/2) ok"))) << "F: test did not report exactly a and b: " << t;
        expect(fatal(s::lacks(t, "member 'c'"))) << "F: test touched the excluded member";
        expect(fatal(e2e::contains(t, "workspace result ok. 2 member(s)"))) << "F: the workspace result does not count two members: " << t;
        fs::remove_all(ws.at("target"));
        r = ws.mcpp({"emit", "build-database", "--format", "json", "-p", "a", "-p", "b"});
        expect(fatal(e2e::succeeded(r))) << "F: emit -p a -p b failed: " << r.err;
        expect(fatal(e2e::contains(r.out, R"("family-name": "a")") && e2e::contains(r.out, R"("family-name": "b")"))) << "F: the database does not describe a and b";
        expect(fatal(s::lacks(r.out, R"("family-name": "c")"))) << "F: the database describes c";
        r = ws.mcpp({"emit", "build-database", "--format", "json", "--workspace", "--exclude", "a"});
        expect(fatal(e2e::succeeded(r))) << "F: emit --exclude failed: " << r.err;
        expect(fatal(e2e::contains(r.out, R"("family-name": "b")") && e2e::contains(r.out, R"("family-name": "c")"))) << "F: the database does not describe b and c";
        expect(fatal(s::lacks(r.out, R"("family-name": "a")"))) << "F: the database describes the excluded member";
        r = ws.mcpp({"emit", "build-database", "--format", "json", "-p", "a", "--exclude", "b"});
        expect(fatal(failed(r))) << "F: emit accepted -p with --exclude";
        // The script read the refusal from standard output (`> f4.json`).
        expect(fatal(e2e::contains(r.out, "--exclude"))) << "F: emit's refusal does not name --exclude: " << r.out << r.err;
    };
}};

}  // namespace
