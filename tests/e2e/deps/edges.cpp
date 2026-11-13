// What a dependency edge carries: patches, and `embed` (formerly tests/e2e/1001, 1002, 1046, 1060).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

const s::options inherited{.where = s::home::inherited};

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

// git in `dir` with a committer: its standard output.
std::string git(const fs::path& dir, std::vector<std::string> args) {
    const auto program = s::on_path("git");
    if (!program) skip("git is not installed");
    args.insert(args.begin(), {"-C", dir.string(), "-c", "user.name=t", "-c", "user.email=t@t"});
    auto r = s::run(*program, args);
    expect(fatal(e2e::succeeded(r))) << "git failed: " << s::text(r);
    return r.out;
}

// The directories under `dir` whose name starts with `prefix`.
std::vector<fs::path> entries(const fs::path& dir, std::string_view prefix = "") {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().filename().string().starts_with(prefix)) out.push_back(e.path());
    return out;
}

// The `root:` `mcpp why deps` reports for mcpplibs.cmdline (`sed -n '/^  mcpplibs.cmdline[@+]/,
// /^  [^ ]/s/^      root: //p' | head -1`).
std::string reported_root(std::string_view why) {
    bool in = false;
    std::istringstream lines{std::string(why)};
    for (std::string line; std::getline(lines, line);) {
        if (!in) {
            in = line.starts_with("  mcpplibs.cmdline@") || line.starts_with("  mcpplibs.cmdline+");
            if (!in) continue;
        } else if (line.size() > 2 && line.starts_with("  ") && line[2] != ' ') {
            in = false;
        }
        if (line.starts_with("      root: ")) return line.substr(12);
    }
    return {};
}

E2E edges{"edges", [] {
    "1001 a dependency carries its patches"_test[tag{"requires:gcc"}] = [] {
        // A patched library is another library: the verified source is copied to the variant's
        // own tree under the mcpp home and patched there; the lock names the variant and each
        // patch with its SHA-256; a changed patch is another variant; a patch that does not
        // apply is refused naming the patch and its hunk and leaves no tree; a path dependency
        // carries no patches.
        s::needs({"gcc"});
        const auto home = s::case_home(s::home::inherited);
        const auto variants = home / "cache/src/compat.zlib";
        s::project app{{{"patches/0001-mark.patch", "--- a/zlib-1.3.2/zlib.h\n+++ b/zlib-1.3.2/zlib.h\n@@ -31,4 +31,6 @@\n #ifndef ZLIB_H\n #define ZLIB_H\n \n+#define MCPP_PATCHED_ZLIB 1\n+\n #ifdef ZLIB_BUILD\n"},
                        {"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[dependencies]\n\"compat.zlib\" = { version = \"1.3.2\", patches = [\"patches/0001-mark.patch\"] }\n"},
                        {"src/main.cpp", R"cpp(#include <zlib.h>
#include <cstdio>
int main() {
#ifdef MCPP_PATCHED_ZLIB
    std::printf("patched %d %s\n", MCPP_PATCHED_ZLIB, zlibVersion());
    return 0;
#else
    std::printf("unpatched\n");
    return 1;
#endif
}
)cpp"}}};
        auto a = app.mcpp({"run"}, inherited);
        expect(fatal(e2e::succeeded(a))) << "the patched build did not run";
        expect(s::has_line(s::text(a), "^patched 1 1.3.2")) << "the program did not see the patched header";
        const auto made = entries(variants, "1.3.2+p");
        expect(fatal(!made.empty() && fs::exists(made.front() / ".mcpp-variant"))) << "no variant tree under the mcpp home";
        expect(e2e::contains(s::read_file(made.front() / "zlib-1.3.2/zlib.h"), "MCPP_PATCHED_ZLIB")) << "the variant's header is not patched";
        const auto installed = home / "registry/data/xpkgs/compat-x-zlib/1.3.2";
        if (fs::is_directory(installed))
            expect(s::lacks(s::read_file(installed / "zlib-1.3.2/zlib.h"), "MCPP_PATCHED_ZLIB")) << "the installed source was patched in place";
        const auto lock = app.read("mcpp.lock");
        expect(s::has_line(lock, "^variant = ")) << "the lock names no variant";
        expect(s::has_line(lock, "patches/0001-mark.patch#sha256:[0-9a-f]{64}")) << "the lock does not name the patch and its digest";
        auto sbom = app.mcpp({"emit", "sbom"}, inherited);
        expect(fatal(e2e::succeeded(sbom))) << "sbom failed";
        expect(e2e::contains(sbom.out, "\"pedigree\"") && e2e::contains(sbom.out, "0001-mark.patch")) << "the bill of materials states no pedigree";

        auto patch = app.read("patches/0001-mark.patch");
        patch.replace(patch.find("MCPP_PATCHED_ZLIB 1"), 19, "MCPP_PATCHED_ZLIB 2");
        app.write("patches/0001-mark.patch", patch);
        auto c = app.mcpp({"run"}, inherited);
        expect(fatal(e2e::succeeded(c))) << "the build with a changed patch failed";
        expect(s::has_line(s::text(c), "^patched 2 ")) << "a changed patch did not make another variant";
        expect(entries(variants, "1.3.2+p").size() >= 2) << "the changed patch did not get its own tree";

        app.write("patches/0001-mark.patch", "--- a/zlib-1.3.2/zlib.h\n+++ b/zlib-1.3.2/zlib.h\n@@ -31,1 +31,1 @@\n-#ifndef NOT_THE_LINE\n+#ifndef ZLIB_H\n");
        const auto before = entries(variants).size();
        auto d = app.mcpp({"build"}, inherited);
        expect(fatal(failed(d))) << "a patch that does not apply was accepted";
        expect(e2e::contains(s::text(d), "0001-mark.patch") && e2e::contains(s::text(d), "hunk 1")) << "the refusal does not name the patch and the hunk";
        expect(eq(entries(variants).size(), before)) << "a failed patching left a tree";

        s::project{app.dir().parent_path() / "lib", {{"mcpp.toml", "[package]\nname = \"lib\"\nversion = \"0.1.0\"\n"},
                                                     {"src/lib.cpp", "int lib_one() { return 1; }\n"}}};
        app.write("mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[dependencies]\nlib = { path = \"../lib\", patches = [\"patches/0001-mark.patch\"] }\n");
        auto e = app.mcpp({"build"}, inherited);
        expect(fatal(failed(e))) << "patches on a path dependency were accepted";
        expect(e2e::contains(s::text(e), "is a path and carries patches")) << "the refusal does not say why";
    };

    "1002 a dependency is embedded"_test[tag{"requires:gcc"}] = [] {
        // `embed = true` compiles a dependency into its consumer: its sources, less `exclude`,
        // with its own include directories and macros; it builds no library; `exclude` needs
        // `embed`; a program reaching it holds its embedder or is refused; its link flags and
        // runtime libraries do not reach the program's link.
        s::needs({"gcc"});
        const auto tmp = temp_dir();
        s::project zip{tmp / "zip", {{"mcpp.toml", "[package]\nname    = \"zip\"\nversion = \"0.1.0\"\n[dependencies]\n\"compat.zlib\" = { version = \"1.3.2\", embed = true, exclude = [\"*/uncompr.c\"] }\n"},
                                     {"src/zip.cppm", "module;\n#include <zlib.h>\nexport module zip;\nexport int zip_bound(int n) { return static_cast<int>(compressBound(static_cast<uLong>(n))); }\nexport const char* zip_version() { return zlibVersion(); }\n"},
                                     {"build.mcpp", "import std;\nint main() { return 0; }\n"}}};
        s::project app{tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[dependencies]\nzip = { path = \"../zip\" }\n"},
                                     {"src/main.cpp", "import std;\nimport zip;\nint main() { std::println(\"{} {}\", zip_version(), zip_bound(100) > 100); }\n"}}};
        auto a = app.mcpp({"run"}, inherited);
        expect(fatal(e2e::succeeded(a))) << "the program with an embedded zlib did not run";
        expect(s::lacks(s::text(a), "matched no source file")) << "the flags embed gives were reported as a glob that matched nothing";
        expect(s::has_line(s::text(a), "^1.3.2 true")) << "the program did not call the embedded zlib";
        fs::path dir;
        for (const auto& d : s::glob(app.at("target"), "*/*"))
            if (dir.empty() && fs::is_directory(d) && std::regex_match(d.filename().string(), std::regex{"[0-9a-f]{16}"})) dir = d;
        expect(fatal(!dir.empty())) << "no build directory";
        const auto db = s::find_file(dir, "compile_commands.json").value_or(app.at("compile_commands.json"));
        const auto commands = s::read_file(db);
        expect(e2e::contains(commands, "deflate.c")) << "zlib's sources were not compiled";
        expect(s::lacks(commands, "uncompr.c")) << "an excluded file was compiled";
        expect(s::find_files(dir, R"(.*zlib.*\.(a|lib))").empty()) << "a library of the embedded package was built";
        expect(e2e::contains(commands, "mcpp_zlib_config.h")) << "the dependency's own flags did not reach its files";
        bool leaked = false;
        for (const auto& e : s::json(commands)) {
            if (!e["file"].get<std::string>().ends_with("zip.cppm")) continue;
            std::string all;
            if (e.contains("arguments"))
                for (const auto& arg : e["arguments"]) all += arg.get<std::string>() + " ";
            if (e.contains("command")) all += e["command"].get<std::string>();
            leaked = leaked || all.find("mcpp_zlib_config.h") != std::string::npos;
        }
        expect(!leaked) << "the dependency's flags reached the consumer's own unit";

        s::project bad{tmp / "bad", {{"mcpp.toml", "[package]\nname = \"bad\"\nversion = \"0.1.0\"\n\n[dependencies]\n\"compat.zlib\" = { version = \"1.3.2\", exclude = [\"*/gzwrite.c\"] }\n"},
                                     {"src/main.cpp", "int main() {}\n"}}};
        auto c = bad.mcpp({"build"}, inherited);
        expect(fatal(failed(c))) << "exclude without embed was accepted";
        expect(e2e::contains(s::text(c), "exclude is allowed only with embed")) << "the refusal does not say why";

        s::project ws{tmp / "ws", {{"mcpp.toml", "[workspace]\nmembers = [\"app\", \"plain\", \"zip\"]\n"},
                                   {"plain/mcpp.toml", "[package]\nname = \"plain\"\nversion = \"0.1.0\"\n\n[dependencies]\n\"compat.zlib\" = \"1.3.2\"\n"},
                                   {"plain/src/main.cpp", "#include <zlib.h>\nint main() { return zlibVersion()[0] == 0; }\n"}}};
        fs::copy(app.dir(), ws.at("app"), fs::copy_options::recursive);
        fs::copy(zip.dir(), ws.at("zip"), fs::copy_options::recursive);
        ws.remove("app/target");
        auto d = ws.mcpp({"build", "--workspace"}, inherited);
        expect(fatal(failed(d))) << "a program without the embedder was accepted";
        expect(e2e::contains(s::text(d), "which 'zip' embeds")) << "the refusal does not name the embedder";

        s::project{tmp / "lk", {{"mcpp.toml", "[package]\nname = \"lk\"\nversion = \"0.1.0\"\n\n[build]\nldflags = [\"-lmcpp-no-such-library\"]\n\n[runtime]\nlibraries = [\"mcpp-no-such-runtime-library\"]\n"},
                                {"src/lk.c", "int lk_answer(void) { return 42; }\n"}}};
        s::project lkapp{tmp / "lkapp", {{"mcpp.toml", "[package]\nname = \"lkapp\"\nversion = \"0.1.0\"\n\n[dependencies]\nlk = { path = \"../lk\", embed = true }\n"},
                                         {"src/main.cpp", "extern \"C\" int lk_answer();\nint main() { return lk_answer() == 42 ? 0 : 1; }\n"}}};
        expect(e2e::succeeded(lkapp.mcpp({"run"}, inherited))) << "the embedded package's link flags or libraries reached the program's link";
    };

    "1046 a patch made with git diff from the package root is applied"_test[tag{"requires:elf"}][tag{"requires:gcc"}] = [] {
        // `mcpp why deps` reports an index dependency's root; a patch made there with `git diff`
        // applies, and the variant is then the root the build reads.
        s::needs({"elf", "gcc"});
        const auto tmp = temp_dir();
        s::project app{tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[dependencies]\nmcpplibs.cmdline = \"0.0.1\"\n"},
                                     {"src/main.cpp", "int main() { return 0; }\n"}}};
        auto why = app.mcpp({"why", "deps"});
        expect(fatal(e2e::succeeded(why))) << "why deps failed";
        const auto root = reported_root(s::text(why));
        expect(fatal(!root.empty() && fs::is_directory(root))) << "the dependency's root is not reported";
        const auto copy = tmp / "cmdline";
        fs::copy(root, copy, fs::copy_options::recursive);
        const auto found = s::find_file(copy, "parse.cppm");
        expect(fatal(found.has_value())) << "the root holds no parse.cppm";
        const auto file = fs::relative(*found, copy).generic_string();
        git(copy, {"init", "-q"});
        git(copy, {"add", "-A"});
        git(copy, {"commit", "-qm", "base"});
        s::project{copy, {}}.write(file, s::read_file(*found) + "// changed for the patch\n");
        app.write("patches/0001-change.patch", git(copy, {"diff"}));
        expect(fatal(e2e::contains(app.read("patches/0001-change.patch"), "--- a/" + file))) << "git did not name the file from the root";
        app.write("mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[dependencies]\nmcpplibs.cmdline = { version = \"0.0.1\", patches = [\"patches/0001-change.patch\"] }\n");
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "the patched build failed";
        auto why2 = app.mcpp({"why", "deps"});
        expect(fatal(e2e::succeeded(why2))) << "why deps failed after the patch";
        const auto variant = reported_root(s::text(why2));
        expect(fatal(!variant.empty() && variant != root)) << "the variant is not the reported root";
        expect(e2e::contains(s::read_file(fs::path(variant) / file), "changed for the patch")) << "the variant does not hold the change";
    };

    "1060 a patched member of a repository keeps its siblings"_test[tag{"requires:gcc"}][tag{"requires:unix-shell"}] = [] {
        // A patch on a member of a git repository applies to that member, and its path
        // dependency on a sibling still reaches the sibling.
        s::needs({"gcc", "unix-shell"});
        const auto tmp = temp_dir();
        s::project repo{tmp / "repo", {{"mcpp.toml", "[workspace]\nmembers = [\"core\", \"front\"]\n"},
                                       {"core/mcpp.toml", "[package]\nname    = \"toy-core\"\nversion = \"0.1.0\"\n[targets.toy-core]\nkind = \"lib\"\n"},
                                       {"core/src/core.c", "int core_value(void) { return 40; }\n"},
                                       {"front/mcpp.toml", "[package]\nname    = \"toy-front\"\nversion = \"0.1.0\"\n[dependencies]\ntoy-core = { path = \"../core\" }\n[targets.toy-front]\nkind = \"lib\"\n"},
                                       {"front/src/front.c", "int front_value(void) { return 1; }\n"}}};
        git(repo.dir(), {"init", "-q"});
        git(repo.dir(), {"add", "-A"});
        git(repo.dir(), {"commit", "-qm", "base"});
        git(repo.dir(), {"tag", "v1"});
        const auto edit = tmp / "front-edit";
        fs::copy(repo.at("front"), edit, fs::copy_options::recursive);
        git(edit, {"init", "-q"});
        git(edit, {"add", "-A"});
        git(edit, {"commit", "-qm", "b"});
        s::project{edit, {}}.write("src/front.c", "int front_value(void) { return 2; }\n");
        s::project app{tmp / "app", {{"src/main.c", "#include <stdio.h>\nint front_value(void); int core_value(void);\nint main(void) { printf(\"%d %d\\n\", front_value(), core_value()); return 0; }\n"}}};
        app.write("patches/0001-front.patch", git(edit, {"diff"}));
        expect(fatal(e2e::contains(app.read("patches/0001-front.patch"), "--- a/src/front.c"))) << "git wrote no member-rooted patch";
        app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[dependencies]\ntoy-front = {{ git = \"file://{}\", tag = \"v1\", patches = [\"patches/0001-front.patch\"] }}\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.c\"\n", repo.dir().generic_string()));
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "the patched member did not build";
        auto bins = s::glob(app.at("target"), "*/*/bin/app");
        expect(fatal(!bins.empty())) << "no program built";
        expect(eq(s::last_line(s::run(bins.front()).out), std::string{"2 40"})) << "the program's output";
    };
}};

}  // namespace
