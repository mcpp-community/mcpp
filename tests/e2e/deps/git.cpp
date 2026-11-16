// Git dependencies: the clone cache, the lock, branches and their refresh; one identity declared
// twice; a repository's members; the fetch report; transitive branches; checkouts answering a
// version; the default branch (formerly tests/e2e/24, 661, 713, 816, 943, 944, 1048).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// git in `dir`, as the scripts' `git -c user.name=... -c user.email=...`; its last line.
std::string git(const fs::path& dir, std::vector<std::string> args) {
    const auto program = s::on_path("git");
    if (!program) skip("git is not installed");
    args.insert(args.begin(), {"-C", dir.string(), "-c", "user.name=test", "-c", "user.email=test@local"});
    auto r = s::run(*program, args);
    expect(fatal(e2e::succeeded(r))) << "git failed";
    return s::last_line(r.out);
}

// `mcpp new <name>` whose files become those of `dir` (a repository's work tree), a library
// exporting `<name>.greet` that prints `said`.
void library_repository(const fs::path& dir, std::string_view name, std::string_view said, bool branch) {
    fs::create_directories(dir);
    git(dir, {"init", "--quiet"});
    if (branch) git(dir, {"checkout", "-B", "main", "--quiet"});
    auto made = s::project::scaffold(name);
    for (const auto& e : fs::directory_iterator(made.dir()))
        fs::rename(e.path(), dir / e.path().filename());
    s::project lib{dir, {}};
    lib.write("src/greet.cppm", std::format("export module {}.greet;\nimport std;\nexport auto greet() -> void {{ std::println(\"{}\"); }}\n", name, said));
    lib.remove("src/main.cpp");
    lib.write("mcpp.toml", std::format("[package]\nname        = \"{0}\"\nversion     = \"0.1.0\"\n\n[language]\nstandard   = \"c++23\"\nmodules    = true\nimport_std = true\n\n[modules]\nsources = [\"src/**/*.cppm\"]\n\n[targets.{0}]\nkind = \"lib\"\n", name));
    git(dir, {"add", "-A"});
    git(dir, {"commit", "--quiet", "-m", "init"});
}

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

E2E git_deps{"git", [] {
    "24 git dependency"_test[s::fresh_sandbox] = [] {
        // A git dependency is cloned to $MCPP_HOME/git/<hash>/ and treated as a path dependency;
        // a rebuild does not clone again; a branch is locked (`git+...branch=main`), replayed
        // offline from the lock, kept at the locked commit when the clone cache is evicted, and
        // advanced by `mcpp update`. HOME is the case's own, so the caches resolve there.
        const auto tmp = s::case_dir();
        const auto mhome = s::case_home(s::home::inherited);
        fs::create_directories(tmp / "home");
        auto opts = [&](std::vector<std::pair<std::string, std::optional<std::string>>> extra = {}) {
            s::options o{.where = s::home::inherited, .env = {{"HOME", (tmp / "home").string()}}};
            for (auto& e : extra) o.env.push_back(std::move(e));
            return o;
        };
        auto program = [](const s::project& p, std::string_view name) {
            auto bins = s::glob(p.at("target"), std::format("*/*/bin/{}", name));
            expect(fatal(!bins.empty())) << "no program built";
            return s::run(bins.front()).out;
        };
        auto app_manifest = [](std::string_view name, std::string_view dep, std::string_view source) {
            return std::format("[package]\nname        = \"{0}\"\nversion     = \"0.1.0\"\n\n[language]\nstandard   = \"c++23\"\nmodules    = true\nimport_std = true\n\n[modules]\nsources = [\"src/**/*.cppm\", \"src/**/*.cpp\"]\n\n[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n[dependencies.{1}]\n{2}\n", name, dep, source);
        };
        library_repository(tmp / "origin", "mylibA", "hello from git dep", false);
        const auto head = git(tmp / "origin", {"rev-parse", "HEAD"});
        s::project app{tmp / "myapp", {{"src/main.cpp", "import std;\nimport mylibA.greet;\nint main() { greet(); return 0; }\n"}}};
        app.write("mcpp.toml", app_manifest("myapp", "mylibA", std::format("git = \"{}\"\nrev = \"{}\"", (tmp / "origin").generic_string(), head)));
        app.mcpp({"build"}, opts());
        expect(e2e::contains(program(app, "myapp"), "hello from git dep")) << "git dep module not invoked";
        expect(fatal(fs::is_directory(mhome / "git"))) << "$MCPP_HOME/git not created";
        expect(!fs::is_empty(mhome / "git")) << "git cache empty";
        expect(s::lacks(s::text(app.mcpp({"build"}, opts())), "Cloning")) << "re-cloned on rebuild";

        // A branch.
        library_repository(tmp / "branch-origin", "branchlib", "branch dep v1", true);
        s::project bapp{tmp / "branchapp", {{"src/main.cpp", "import std;\nimport branchlib.greet;\nint main() { greet(); return 0; }\n"}}};
        bapp.write("mcpp.toml", app_manifest("branchapp", "branchlib", std::format("git = \"{}\"\nbranch = \"main\"", (tmp / "branch-origin").generic_string())));
        bapp.mcpp({"build"}, opts());
        expect(e2e::contains(program(bapp, "branchapp"), "branch dep v1")) << "branch dep v1 not invoked";
        bapp.mcpp({"clean"}, opts());
        auto b2 = bapp.mcpp({"build"}, opts({{"MCPP_OFFLINE", "1"}}));
        expect(fatal(e2e::succeeded(b2))) << "offline rebuild with lock + cache present failed";
        expect(e2e::contains(s::text(b2), "from mcpp.lock")) << "branch dep not resolved from mcpp.lock on rebuild";
        expect(s::lacks(s::text(b2), "Cloning")) << "branch dep re-cloned on rebuild";
        const auto lock = bapp.read("mcpp.lock");
        expect(e2e::contains(lock, "source  = \"git+")) << "git dep lock source is not marked as git";
        expect(e2e::contains(lock, "branch=main")) << "git dep lock source does not record branch ref";
        expect(s::lacks(lock, "source  = \"index+")) << "git dep lock source incorrectly uses index source";
        // v2 upstream; the clone cache evicted: the lock holds v1.
        s::project{tmp / "branch-origin", {}}.write("src/greet.cppm", "export module branchlib.greet;\nimport std;\nexport auto greet() -> void { std::println(\"branch dep v2\"); }\n");
        git(tmp / "branch-origin", {"add", "-A"});
        git(tmp / "branch-origin", {"commit", "--quiet", "-m", "v2"});
        fs::remove_all(mhome / "git");
        bapp.mcpp({"clean"}, opts());
        expect(fatal(e2e::succeeded(bapp.mcpp({"build"}, opts())))) << "rebuild after git-cache eviction";
        expect(e2e::contains(program(bapp, "branchapp"), "branch dep v1")) << "git-cache eviction silently advanced the locked branch";
        // `mcpp update` advances it.
        bapp.mcpp({"update", "branchlib"}, opts());
        bapp.mcpp({"clean"}, opts());
        bapp.mcpp({"build"}, opts());
        expect(e2e::contains(program(bapp, "branchapp"), "branch dep v2")) << "branch dep did not refresh after mcpp update";
    };

    "661 one identity two declarations"_test[s::gcc] = [] {
        // Two declarations of one dependency identity: the root's reference wins over a
        // library's, warned and locked; with no root opinion the first-resolved declaration wins
        // for every edge; a kind clash between two non-root requesters is refused with the root
        // hint; a root pin below a library's SemVer floor is refused naming both; the same
        // reference declared twice is silent, under --strict too.
        const auto tmp = s::case_dir();
        const auto fw_git = tmp / "framework-git";
        const auto fw_host = fw_git.generic_string();
        s::project fw{fw_git, {{"mcpp.toml", "[package]\nname    = \"framework\"\nversion = \"0.1.0\"\n[build]\nsources = [\"src/*.c\"]\n[targets.framework]\nkind = \"lib\"\n"}}};
        git(fw_git, {"init", "--quiet"});
        auto commit_marker = [&](int marker, std::string_view message) {
            fw.write("src/framework.c", std::format("int framework_marker(void) {{ return {}; }}\n", marker));
            if (message.empty()) return std::string{};
            git(fw_git, {"add", "-A"});
            git(fw_git, {"commit", "--quiet", "-m", std::string(message)});
            return git(fw_git, {"rev-parse", "HEAD"});
        };
        const auto rev_a = commit_marker(101, "A");
        const auto rev_b = commit_marker(102, "B");
        const auto rev_c = commit_marker(103, "C");
        commit_marker(199, "");
        const auto fw_v020 = tmp / "framework-v020";
        s::project{fw_v020, {{"mcpp.toml", "[package]\nname    = \"framework\"\nversion = \"0.2.0\"\n[build]\nsources = [\"src/*.c\"]\n[targets.framework]\nkind = \"lib\"\n"},
                             {"src/framework.c", "int framework_marker(void) { return 20; }\n"}}};
        auto make_lib = [](const fs::path& dir, std::string_view name, std::string_view extra) {
            s::project lib{dir, {}};
            lib.write("mcpp.toml", std::format("[package]\nname    = \"{0}\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.c\"]\n\n[targets.{0}]\nkind = \"lib\"\n\n{1}\n", name, extra));
            lib.write(std::format("src/{}.c", name), std::format("extern int framework_marker(void);\nint {}_marker(void) {{ return framework_marker(); }}\n", name));
        };
        auto git_at = [&](std::string_view rev) { return std::format("[dependencies.framework]\ngit = \"{}\"\nrev = \"{}\"", fw_host, rev); };
        auto app = [&](const fs::path& dir, std::string_view deps, std::string_view main) {
            return s::project{dir, {{"mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[build]\nsources = [\"src/*.cpp\"]\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n{}", deps)},
                                    {"src/main.cpp", main}}};
        };
        auto printing = [](std::vector<std::string_view> libs) {
            std::string decls, format, args;
            for (auto l : libs) {
                decls += std::format("extern \"C\" int {}_marker(void);\n", l);
                format += std::format("{}{}=%d", format.empty() ? "" : " ", l);
                args += std::format(", {}_marker()", l);
            }
            return std::format("#include <cstdio>\n{}int main() {{\n    std::printf(\"{}\\n\"{});\n    return 0;\n}}\n", decls, format, args);
        };

        // 1. The root's git rev wins over a library's, warned, locked, built against A.
        make_lib(tmp / "case1/libb", "libb", git_at(rev_b));
        auto a1 = app(tmp / "case1/app", std::format("{}\n[dependencies.libb]\npath = \"../libb\"\n", git_at(rev_a)), printing({"libb"}));
        auto r1 = a1.mcpp({"run"});
        expect(fatal(e2e::succeeded(r1))) << "case 1: exit " << r1.status;
        expect(e2e::contains(s::text(r1), "app")) << "case 1: warning does not name the root";
        expect(e2e::contains(s::text(r1), "libb")) << "case 1: warning does not name the library";
        expect(e2e::contains(s::text(r1), rev_a)) << "case 1: warning does not name rev A";
        expect(e2e::contains(s::text(r1), rev_b)) << "case 1: warning does not name rev B";
        expect(e2e::contains(s::text(r1), "libb=101")) << "case 1: program did not print A's marker";
        expect(e2e::contains(a1.read("mcpp.lock"), rev_a)) << "case 1: mcpp.lock does not record A";

        // 2. The root's path wins over a library's git rev; the program sees the working tree.
        make_lib(tmp / "case2/libg", "libg", git_at(rev_b));
        auto a2 = app(tmp / "case2/app", std::format("[dependencies.framework]\npath = \"{}\"\n[dependencies.libg]\npath = \"../libg\"\n", fw_host), printing({"libg"}));
        auto r2 = a2.mcpp({"run"});
        expect(fatal(e2e::succeeded(r2))) << "case 2: exit " << r2.status;
        expect(e2e::contains(s::text(r2), "libg")) << "case 2: warning does not name the library";
        expect(e2e::contains(s::text(r2), "libg=199")) << "case 2: program did not print the working tree's marker";

        // 3. No root opinion: the first-dequeued declaration wins for both edges.
        make_lib(tmp / "case3/libb", "libb", git_at(rev_b));
        make_lib(tmp / "case3/libc", "libc", git_at(rev_c));
        auto a3 = app(tmp / "case3/app", "[dependencies.libb]\npath = \"../libb\"\n[dependencies.libc]\npath = \"../libc\"\n", printing({"libb", "libc"}));
        auto r3 = a3.mcpp({"run"});
        expect(fatal(e2e::succeeded(r3))) << "case 3: exit " << r3.status;
        expect(e2e::contains(s::text(r3), "libb")) << "case 3: warning does not name libb";
        expect(e2e::contains(s::text(r3), "libc")) << "case 3: warning does not name libc";
        expect(e2e::contains(s::text(r3), "libb=102 libc=102")) << "case 3: both libraries did not converge on the first-resolved framework (B)";

        // 4. A kind clash between two non-root requesters is refused, with the hint.
        make_lib(tmp / "case4/libd", "libd", std::format("[dependencies.framework]\npath = \"{}\"", fw_v020.generic_string()));
        make_lib(tmp / "case4/libe", "libe", git_at(rev_b));
        auto a4 = app(tmp / "case4/app", "[dependencies.libd]\npath = \"../libd\"\n[dependencies.libe]\npath = \"../libe\"\n", "int main() { return 0; }\n");
        auto r4 = a4.mcpp({"build"});
        expect(fatal(failed(r4))) << "case 4: expected a refusal but the build succeeded";
        expect(e2e::contains(s::text(r4), "Pick one")) << "case 4: refusal message missing 'Pick one'";
        expect(e2e::contains(s::text(r4), "in the root to settle it")) << "case 4: refusal is missing the new root hint";
        expect(e2e::contains(s::text(r4), "libd")) << "case 4: refusal does not name libd";
        expect(e2e::contains(s::text(r4), "libe")) << "case 4: refusal does not name libe";

        // 5. A root pin below a library's SemVer floor is refused, naming both.
        s::project{tmp / "case5/local-index", {{"pkgs/f/framework.lua", R"cpp(package = {
    spec = "1",
    name = "framework",
    description = "SemVer side of the 661 fixture -- never actually fetched",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["0.5.0"] = {
                url = "https://example.invalid/framework-0.5.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/*.c" },
        targets = { ["framework"] = { kind = "lib" } },
        deps = {},
    },
}
)cpp"}}};
        make_lib(tmp / "case5/libv", "libv", "[dependencies]\nframework = \">=0.3\"");
        auto a5 = app(tmp / "case5/app", std::format("[indices]\ndefault = {{ path = \"{}\" }}\n[dependencies.framework]\npath = \"{}\"\n[dependencies.libv]\npath = \"../libv\"\n", (tmp / "case5/local-index").generic_string(), fw_v020.generic_string()), "int main() { return 0; }\n");
        auto r5 = a5.mcpp({"build"});
        expect(fatal(failed(r5))) << "case 5: expected a refusal but the build succeeded";
        expect(e2e::contains(s::text(r5), "0.2.0")) << "case 5: refusal does not name the checkout's version";
        expect(e2e::contains(s::text(r5), ">=0.3")) << "case 5: refusal does not name the requirement";
        expect(e2e::contains(s::text(r5), "app")) << "case 5: refusal does not name the root";
        expect(e2e::contains(s::text(r5), "libv")) << "case 5: refusal does not name the library";

        // 6. The same reference declared twice is silent, and --strict stays green.
        make_lib(tmp / "case6/libf", "libf", git_at(rev_a));
        auto a6 = app(tmp / "case6/app", std::format("{}\n[dependencies.libf]\npath = \"../libf\"\n", git_at(rev_a)), printing({"libf"}));
        auto r6 = a6.mcpp({"build", "--strict"});
        expect(fatal(e2e::succeeded(r6))) << "case 6: --strict refused an identical, repeated declaration";
        expect(s::lacks(s::text(r6), "wins")) << "case 6: an identical reference declared twice still produced an override warning";
    };

    "713 a git dependency selects a repository member"_test[s::gcc] = [] {
        // A git dependency whose key names a package other than the repository's root package
        // selects the `[workspace] members` entry declaring it; a second declaration of one
        // dependency by one consumer keeps its requests; a git dependency's compile banner names
        // its reference.
        const auto tmp = s::case_dir();
        const s::options linked{.where = s::home::linked};
        s::project repo{tmp / "repo", {{"mcpp.toml", "[package]\nname      = \"fw\"\nnamespace = \"spike\"\nversion   = \"0.1.0\"\n[targets.fw]\nkind = \"lib\"\n[build]\nsources = [\"src/*.cpp\"]\n[workspace]\nmembers = [\"tool\"]\n"},
                                       {"src/fw.cpp", "int fw_answer() { return 42; }\n"},
                                       {"tool/mcpp.toml", "[package]\nname      = \"fw-installer\"\nnamespace = \"spike\"\nversion   = \"0.1.0\"\n[targets.fw-installer]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n[dependencies]\nspike.fw = { path = \"..\" }\n"},
                                       {"tool/src/main.cpp", "#include <cstdio>\nint fw_answer();\nint main() { std::printf(\"fw-installer ran %d\\n\", fw_answer()); return 0; }\n"}}};
        git(repo.dir(), {"init", "-q"});
        git(repo.dir(), {"add", "-A"});
        git(repo.dir(), {"commit", "-qm", "fixture"});
        const auto rev = git(repo.dir(), {"rev-parse", "HEAD"});
        const auto url = "file://" + repo.dir().generic_string();
        auto write_app = [&](std::string_view name, std::string_view manifest) {
            return s::project{tmp / name, {{"mcpp.toml", manifest}, {"src/main.cpp", "int main() { return 0; }\n"},
                                           {"build.mcpp", R"cpp(import std;
import mcpp;
int main() {
    const char* a = mcpp::dep_bin("spike.fw-installer", "fw-installer");
    mcpp::warning((std::string("DEPBIN tool=[") + (a ? a : "") + "]").c_str());
    return 0;
}
)cpp"}}};
        };
        auto member = write_app("member", std::format("[package]\nname    = \"member\"\nversion = \"0.1.0\"\n[targets.member]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n[dependencies]\nspike.fw-installer = {{ git = \"{}\", rev = \"{}\", tools = [\"fw-installer\"] }}\n", url, rev));
        auto b1 = member.mcpp({"build"}, linked);
        expect(fatal(e2e::succeeded(b1))) << "a git dependency naming a repository member was refused";
        std::smatch m;
        const auto t1 = s::text(b1);
        const bool named = std::regex_search(t1, m, std::regex{R"(DEPBIN tool=\[([^\]]*))"});
        const std::string tool = named ? m[1].str() : "";
        expect(fatal(!tool.empty())) << "the member's tool was not provided";
        expect(eq(s::last_line(s::run(tool).out), std::string{"fw-installer ran 42"})) << "the member's tool did not run";

        auto both = write_app("both", std::format("[package]\nname    = \"both\"\nversion = \"0.1.0\"\n[targets.both]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n[dependencies]\nspike.fw = {{ git = \"{0}\", rev = \"{1}\" }}\nspike.fw-installer = {{ git = \"{0}\", rev = \"{1}\", tools = [\"fw-installer\"] }}\n", url, rev));
        both.write("src/main.cpp", "int fw_answer();\nint main() { return fw_answer() == 42 ? 0 : 1; }\n");
        auto b2 = both.mcpp({"build", "-v"}, linked);
        expect(fatal(e2e::succeeded(b2))) << "the root and a member by one revision did not build";
        expect(s::lacks(s::text(b2), "declared identity is used")) << "the member's key adopted the root's identity";
        expect(s::has_line(s::text(b2), R"(DEPBIN tool=\[[^\]])")) << "the member's tools request was lost";
        expect(s::no_line(s::text(b2), R"(Compiling spike\.fw v( |$))")) << "the git banner printed an empty version";
        expect(s::has_line(s::text(b2), std::format(R"(spike\.fw v[^ ]+ \(git rev {}\))", rev.substr(0, 12)))) << "the git banner does not name the reference";
        expect(e2e::succeeded(both.mcpp({"run"}, linked))) << "the application over the git root does not run";

        // A path dependency declared in two tables, the second asking for its tools.
        s::project{tmp / "local", {{"mcpp.toml", "[package]\nname      = \"fw-installer\"\nnamespace = \"spike\"\nversion   = \"0.1.0\"\n[targets.fw-installer]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n[dependencies]\n\n"},
                                   {"src/main.cpp", "#include <cstdio>\nint main() { std::puts(\"local tool\"); return 0; }\n"}}};
        auto twice = write_app("twice", "[package]\nname    = \"twice\"\nversion = \"0.1.0\"\n[targets.twice]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n[dependencies]\nspike.fw-installer = { path = \"../local\" }\n[build-dependencies]\nspike.fw-installer = { path = \"../local\", tools = [\"fw-installer\"] }\n");
        auto b3 = twice.mcpp({"build"}, linked);
        expect(fatal(e2e::succeeded(b3))) << "a dependency declared in two tables did not build";
        expect(s::has_line(s::text(b3), R"(DEPBIN tool=\[[^\]])")) << "the second declaration's tools request was dropped";
    };

    "816 a git dependency reports its fetch"_test[s::unix_shell] = [] {
        // Fetching a git dependency is reported with the renderer every other acquisition uses:
        // a start and a finish line when stdout is not a terminal, without CR or ESC bytes.
        const auto tmp = s::case_dir();
        s::project repo{tmp / "repo", {{"mcpp.toml", "[package]\nnamespace = \"probe816\"\nname = \"gdep\"\nversion = \"0.1.0\"\n[targets.gdep]\nkind = \"lib\"\n"},
                                       {"src/gdep.cppm", "export module probe816.gdep;\nexport int gdep_value() { return 7; }\n"}}};
        git(repo.dir(), {"init", "-q", "-b", "main", "."});
        git(repo.dir(), {"add", "-A"});
        git(repo.dir(), {"commit", "-qm", "init"});
        const auto url = "file://" + repo.dir().generic_string();
        s::project consumer{tmp / "consumer", {{"src/main.cpp", "import probe816.gdep;\nint main() { return gdep_value() == 7 ? 0 : 1; }\n"}}};
        consumer.write("mcpp.toml", std::format("[package]\nname = \"consumer\"\nversion = \"0.1.0\"\n[dependencies]\n\"probe816.gdep\" = {{ git = \"{}\", branch = \"main\" }}\n", url));
        auto b = consumer.mcpp({"build"});
        expect(fatal(e2e::succeeded(b))) << "the build failed";
        expect(e2e::contains(s::text(b), "Fetching " + url)) << "A: no start line for the fetch";
        auto escaped = std::regex_replace(url, std::regex{R"([.^$|()\[\]{}*+?\\])"}, R"(\$&)");
        expect(s::has_line(s::text(b), "Fetching " + escaped + ".* done")) << "A: no finish line for the fetch";
        expect(s::lacks(s::text(b), "\r")) << "B: the log carries a carriage return";
        expect(s::lacks(s::text(b), "\x1b")) << "B: the log carries an ESC byte";
    };

    "943 a transitive git branch is locked"_test[s::gcc] = [] {
        // A git branch reached through another package is locked as the root's own are: a later
        // commit on it is not taken until `mcpp update`.
        const auto tmp = s::case_dir();
        auto repo = [](const fs::path& dir) {
            git(dir, {"init", "--quiet", "-b", "main"});
            git(dir, {"add", "-A"});
            git(dir, {"commit", "--quiet", "-m", "init"});
        };
        s::project leaf{tmp / "leaf", {{"mcpp.toml", "[package]\nname    = \"leaf\"\nversion = \"0.1.0\"\n[targets.leaf]\nkind = \"lib\"\n"},
                                       {"src/leaf.cppm", "export module leaf;\nexport int leaf_value() { return 1; }\n"}}};
        repo(leaf.dir());
        s::project mid{tmp / "mid", {{"src/mid.cppm", "export module mid;\nimport leaf;\nexport int mid_value() { return leaf_value(); }\n"}}};
        mid.write("mcpp.toml", std::format("[package]\nname    = \"mid\"\nversion = \"0.1.0\"\n[targets.mid]\nkind = \"lib\"\n[dependencies]\nleaf = {{ git = \"file://{}\", branch = \"main\" }}\n", leaf.dir().generic_string()));
        repo(mid.dir());
        s::project app{tmp / "app", {{"src/main.cpp", "import mid;\n#include <cstdio>\nint main() { std::printf(\"value %d\\n\", mid_value()); }\n"}}};
        app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n[dependencies]\nmid = {{ git = \"file://{}\", branch = \"main\" }}\n", mid.dir().generic_string()));
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "the first build";
        const auto leaf_commit = git(leaf.dir(), {"rev-parse", "HEAD"});
        expect(fatal(e2e::contains(app.read("mcpp.lock"), "branch=main@" + leaf_commit))) << "A: mcpp.lock does not hold the transitive branch's commit";
        leaf.write("src/leaf.cppm", "export module leaf;\nexport int leaf_value() { return 2; }\n");
        git(leaf.dir(), {"commit", "--quiet", "-am", "two"});
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "B: the second build";
        auto run = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(run))) << "B: the program did not run";
        expect(e2e::contains(s::text(run), "value 1")) << "B: the build moved to the branch's new tip";
        expect(fatal(e2e::succeeded(app.mcpp({"update"})))) << "C: mcpp update";
        auto run2 = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(run2))) << "C: the program did not run after the update";
        expect(e2e::contains(s::text(run2), "value 2")) << "C: the build after mcpp update did not take the branch's tip";
    };

    "944 a root checkout answers a version the index lacks"_test[s::gcc] = [] {
        // A dependency's request by version is answered by the root's path or git checkout of
        // the package, whether or not the index carries it; an unsatisfied requirement is still
        // refused.
        const auto tmp = s::case_dir();
        const auto host = tmp.generic_string();
        const std::string os = s::on_linux() ? "linux" : s::on_macos() ? "macos" : "windows";
        s::project ws{tmp / "ws", {{"mcpp.toml", "[workspace]\nmembers = [\"m944leaf\"]\n"},
                                   {"m944leaf/mcpp.toml", "[package]\nname = \"m944leaf\"\nversion = \"0.1.0\"\n\n[targets.m944leaf]\nkind = \"lib\"\n"},
                                   {"m944leaf/src/leaf.cppm", "export module m944leaf;\nexport int leaf() { return 1; }\n"}}};
        git(ws.dir(), {"init", "--quiet", "-b", "main"});
        git(ws.dir(), {"add", "-A"});
        git(ws.dir(), {"commit", "--quiet", "-m", "init"});
        s::project mid{tmp / "mid", {{"src/mid.cppm", "export module m944mid;\nimport m944leaf;\nexport int mid() { return leaf(); }\n"}}};
        auto mid_requires = [&](std::string_view version) {
            mid.write("mcpp.toml", std::format("[package]\nname = \"m944mid\"\nversion = \"0.1.0\"\n[targets.m944mid]\nkind = \"lib\"\n[target.'cfg(os = \"{}\")'.dependencies]\nm944leaf = \"{}\"\n", os, version));
        };
        auto app = [&](std::string_view dir, std::string_view leaf) {
            s::project a{tmp / dir, {{"src/main.cpp", "import m944mid;\nint main() { return mid() - 1; }\n"}}};
            a.write("mcpp.toml", std::format("[package]\nname = \"app\"\nversion = \"0.1.0\"\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n[dependencies]\nm944mid = {{ path = \"{0}/mid\" }}\n[target.'cfg(os = \"{1}\")'.dependencies]\nm944leaf = {2}\n", host, os, leaf));
            return a;
        };
        mid_requires("0.1.0");
        auto a = app("a", std::format("{{ path = \"{}/ws/m944leaf\" }}", host)).mcpp({"build"});
        expect(fatal(e2e::succeeded(a))) << "A: the root's path checkout did not answer";
        expect(s::lacks(s::text(a), "no package found")) << "A: the index was asked";
        expect(e2e::succeeded(app("b", std::format("{{ git = \"file://{}/ws\", branch = \"main\" }}", host)).mcpp({"build"}))) << "B: the root's git checkout did not answer";
        mid_requires("0.2.0");
        auto c = app("c", std::format("{{ path = \"{}/ws/m944leaf\" }}", host)).mcpp({"build"});
        expect(fatal(failed(c))) << "C: a requirement the checkout does not satisfy was accepted";
        expect(e2e::contains(s::text(c), "requires 0.2.0")) << "C: the refusal does not name the requirement";
    };

    "1048 a git reference follows the default branch"_test[s::elf][s::llvm] = [] {
        // A git dependency naming no reference follows the repository's default branch (here
        // `trunk`), reported, locked with its commit and read offline; two references are
        // refused; `--platform git+<url>#tag=<t>` takes a platform from its repository.
        const auto tmp = s::case_dir();
        auto repo = [](const fs::path& dir, std::string_view message) {
            git(dir, {"-c", "init.defaultBranch=trunk", "init", "-q", "."});
            git(dir, {"add", "-A"});
            git(dir, {"commit", "-qm", std::string(message)});
        };
        s::project lib{tmp / "lib", {{"mcpp.toml", "[package]\nname    = \"gitlib\"\nversion = \"0.1.0\"\n[targets.gitlib]\nkind = \"lib\"\n"},
                                     {"src/lib.cpp", "int gitlib_value() { return 9; }\n"}}};
        repo(lib.dir(), "init");
        const auto lib_url = lib.dir().generic_string();
        s::project app{tmp / "app", {{"src/main.cpp", "int gitlib_value(); int main() { return gitlib_value(); }\n"}}};
        app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[dependencies]\ngitlib = {{ git = \"{}\" }}\n", lib_url));
        auto b = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(b))) << "the build failed";
        expect(s::has_line(s::text(b), "the default branch of .* is trunk")) << "the default branch is not reported";
        auto bins = s::glob(app.at("target"), "*/*/bin/app");
        expect(fatal(!bins.empty())) << "no program built";
        auto ran = s::run(bins.front());
        expect(eq(ran.status, 9)) << "the program returned " << ran.status << ", not 9";
        expect(s::has_line(app.read("mcpp.lock"), R"(source  = "git\+.*#branch=trunk@[0-9a-f]{40}")")) << "the lock does not record the branch and its commit";
        app.remove("target");
        expect(e2e::succeeded(app.mcpp({"build", "--offline"}))) << "the offline build did not read the lock";
        app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[dependencies]\ngitlib = {{ git = \"{}\", tag = \"v1\", branch = \"trunk\" }}\n", lib_url));
        expect(e2e::contains(s::text(app.mcpp({"build"})), "names 2 of rev / tag / branch; a git dependency names at most one")) << "two refs were not refused";

        s::project platform{tmp / "toy-platform", {}};
        platform.write("mcpp.toml", std::format(R"cpp([package]
name    = "toy-platform"
version = "0.1.0"
[platform]
toolchain = "llvm@{}"
[[platform.define]]
triple        = "x86_64-toyos"
clang-triple  = "x86_64-unknown-linux-gnu"
object-format = "elf"
execution     = "linux"
[[platform.provider]]
serves = ["x86_64-toyos"]
[platform.build]
cflags = ["-ffreestanding", "-fno-stack-protector", "-fno-pic"]
[platform.link]
driver = "compiler"
[platform.layers]
"c-abi" = false
"c++-abi" = false
"compiler-runtime" = false
"kernel-abi" = false
)cpp", s::llvm_version()));
        repo(platform.dir(), "toy");
        git(platform.dir(), {"tag", "v1"});
        s::project bare{tmp / "bare", {{"mcpp.toml", "[package]\nname    = \"bare\"\nversion = \"0.1.0\"\n[build]\nsources = [\"src/*.c\"]\nldflags = [\"-nostdlib\", \"-static\", \"-no-pie\"]\n[targets.bare]\nkind = \"bin\"\nmain = \"src/main.c\"\n"},
                                       {"src/main.c", "void _start(void) { __asm__ volatile(\"mov $60, %eax\\n mov $7, %edi\\n syscall\"); }\n"}}};
        auto p = bare.mcpp({"build", "--target", "x86_64-toyos", "--platform", std::format("git+{}#tag=v1", platform.dir().generic_string())});
        expect(fatal(e2e::succeeded(p))) << "the platform from git did not build its row";
        expect(e2e::contains(s::text(p), "Platform toy-platform 0.1.0")) << "the platform is not reported";
    };
}};

}  // namespace
