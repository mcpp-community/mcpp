// The Web row (wasm32-emscripten): the executable named `<name>.js` beside its `.wasm`, staged and
// run, and `kind = "shared"` refused; a build program compiled by the host's compiler, also from a
// fresh home; the profile's level at the link and the wasm exception model; the longjmp model
// (formerly tests/e2e/650, 657, 981, 1079). Each case needs an emsdk payload and skips without one.
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// An emsdk payload's `em++` in the shared home's registry or the user's xlings.
bool have_emsdk() {
    std::vector<fs::path> roots{s::shared_home() / "registry/data/xpkgs/xim-x-emsdk"};
    if (const auto home = s::env("HOME")) roots.push_back(fs::path(*home) / ".xlings/data/xpkgs/xim-x-emsdk");
    for (const auto& root : roots)
        for (const auto& d : s::glob(root, "*/emscripten")) {
            const auto em = d / "em++";
            std::error_code ec;
            if (fs::is_regular_file(em, ec) && (fs::status(em, ec).permissions() & fs::perms::owner_exec) != fs::perms::none) return true;
        }
    return false;
}

std::vector<std::string> split(std::string_view text, char sep) {
    std::vector<std::string> out;
    for (auto part : std::views::split(text, sep)) out.emplace_back(part.begin(), part.end());
    return out;
}

// The lines of a graph that start with `key` (`grep -E '^<key>'`).
std::string lines_starting(std::string_view text, std::string_view key) {
    std::string out;
    for (const auto& l : split(text, '\n'))
        if (l.starts_with(key)) out += l + "\n";
    return out;
}

const std::string build_program = R"(import mcpp;
#include <cstdio>
int main() {
    std::FILE* f = std::fopen("bp-env.txt", "w");
    if (!f) return 1;
    std::fprintf(f, "host=%s\ntarget=%s\n", mcpp::host(), mcpp::target());
    std::fclose(f);
    return 0;
}
)";

// `grep '^<key>=' bp-env.txt | cut -d= -f2`
std::string recorded(const s::project& p, std::string_view key) {
    const auto line = s::last_starting(p.read("bp-env.txt"), std::string(key) + "=");
    return line.empty() ? std::string() : line.substr(key.size() + 1);
}

E2E emscripten{"emscripten", [] {
    "650 wasm row names its launcher"_test[s::elf] = [] {
        // `kind = "shared"` is refused on the row before a toolchain is resolved, and an ordinary
        // bin is not; a program is bin/<name>.js with <name>.wasm, runs, is staged with both,
        // and `--no-entry` reaches its link as an ordinary ldflag.
        const auto tmp = s::case_dir();
        s::project shared(tmp / "shared", {{"mcpp.toml", "[package]\nname    = \"sh\"\nversion = \"0.1.0\"\n\n[targets.sh]\nkind = \"shared\"\nmain = \"src/lib.cpp\"\n"},
                                           {"src/lib.cpp", "extern \"C\" int foo() { return 1; }\n"}});
        auto r = shared.mcpp({"build", "--target", "wasm32-emscripten"}, {.env = {{"MCPP_NO_AUTO_INSTALL", "1"}}});
        expect(fatal(!r)) << "kind = shared built instead of being refused: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "[targets.sh] kind = \"shared\" is not supported on wasm32-emscripten: a side module needs -sSIDE_MODULE, which mcpp does not render")))
            << "the refusal does not carry the exact sentence: " << s::text(r);
        s::project notshared(tmp / "notshared", {{"mcpp.toml", "[package]\nname = \"ns\"\nversion = \"0.1.0\"\n\n[targets.ns]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                                 {"src/main.cpp", "import std;\nint main() { std::println(\"ok\"); return 0; }\n"}});
        if (!have_emsdk()) skip("650/2 -- no emsdk payload installed (looked under xim-x-emsdk/*/emscripten); part 1 passed");
        r = notshared.mcpp({"build", "--target", "wasm32-emscripten"});
        expect(fatal(e2e::succeeded(r))) << "an ordinary bin on wasm32-emscripten was refused: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "kind = \"shared\" is not supported"))) << "the shared refusal fired on a bin target: " << s::text(r);
        // 650/2
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n[target.wasm32-emscripten]\nrunner = [\"node\"]\n"},
                                     {"src/main.cpp", "import std;\nint main() { std::println(\"1-2-3\"); return 0; }\n"}});
        r = app.mcpp({"build", "--target", "wasm32-emscripten"});
        expect(fatal(e2e::succeeded(r))) << "mcpp build --target wasm32-emscripten failed: " << s::text(r);
        const auto js = s::find_file(app.at("target/wasm32-emscripten"), "app.js");
        expect(fatal(js.has_value())) << "no bin/app.js under target/wasm32-emscripten: " << s::text(r);
        const auto bindir = js->parent_path();
        expect(fatal(fs::is_regular_file(bindir / "app.wasm"))) << "bin/app.wasm is missing: " << s::text(r);
        expect(fatal(!fs::is_regular_file(bindir / "app"))) << "a bare bin/app (no extension) exists alongside app.js -- the fallback naming leaked";
        r = app.mcpp({"run", "--target", "wasm32-emscripten"});
        expect(fatal(e2e::succeeded(r))) << "mcpp run --target wasm32-emscripten failed: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), "^1-2-3$"))) << "mcpp run did not print 1-2-3: " << s::text(r);
        r = app.mcpp({"pack", "--target", "wasm32-emscripten", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "mcpp pack --pkg-format dir failed: " << s::text(r);
        const auto staged = app.at("target/dist/app-0.1.0-wasm32-emscripten");
        expect(fatal(fs::is_directory(staged))) << "no staged directory under target/dist: " << s::text(r);
        expect(fatal(fs::is_regular_file(staged / "bin/app.js"))) << "the staged tree has no bin/app.js";
        expect(fatal(fs::is_regular_file(staged / "bin/app.wasm"))) << "the staged tree has no bin/app.wasm";
        const auto manifest = app.at("target/dist/app-0.1.0-wasm32-emscripten.stage-manifest");
        expect(fatal(fs::is_regular_file(manifest))) << "no .stage-manifest sibling of the staged tree";
        expect(fatal(e2e::contains(s::read_file(manifest), "bin/app.js"))) << "the stage manifest does not list bin/app.js";
        expect(fatal(e2e::contains(s::read_file(manifest), "bin/app.wasm"))) << "the stage manifest does not list bin/app.wasm";
        s::project noent(tmp / "noent", {{"mcpp.toml", "[package]\nname = \"noent\"\nversion = \"0.1.0\"\n\n[targets.noent]\nkind = \"bin\"\nmain = \"src/lib.cpp\"\n\n[target.'cfg(os = \"emscripten\")'.build]\nldflags = [\"--no-entry\"]\n"},
                                         {"src/lib.cpp", "extern \"C\" int mcpp_factory() { return 42; }\n"}});
        r = noent.mcpp({"build", "--target", "wasm32-emscripten"});
        expect(fatal(e2e::succeeded(r))) << "a main()-less bin with --no-entry did not link: " << s::text(r);
        std::optional<fs::path> ninja;
        for (const auto& f : s::find_files(noent.at("target/wasm32-emscripten"), R"(build\.ninja)"))
            if (!f.generic_string().contains("/.build-mcpp/")) { ninja = f; break; }
        expect(fatal(ninja && e2e::contains(s::read_file(*ninja), "--no-entry"))) << "--no-entry did not reach build.ninja";
    };

    "657 a build program under the web row uses the host compiler"_test[s::elf] = [] {
        // A project with a build.mcpp builds for the Web row: its build program is compiled for
        // this machine (MCPP_HOST a native triple, MCPP_TARGET the row), also in a fresh home
        // whose first build names the row, and the same fresh home still builds natively.
        if (!have_emsdk()) skip("657 -- no emsdk payload installed (looked under xim-x-emsdk/*/emscripten)");
        const auto tmp = s::case_dir();
        const auto main_cpp = "#include <cstdio>\nint main() { std::puts(\"1-2-3\"); return 0; }\n";
        auto manifest = [](std::string_view name) {
            return std::format("[package]\nname = \"{0}\"\nversion = \"0.1.0\"\n\n[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n[target.wasm32-emscripten]\nrunner = [\"node\"]\n", name);
        };
        // PHASE 1: the shared home
        s::project p(tmp / "p", {{"mcpp.toml", manifest("bp")}, {"src/main.cpp", main_cpp}, {"build.mcpp", build_program}});
        auto r = p.mcpp({"build", "--target", "wasm32-emscripten"});
        expect(fatal(e2e::succeeded(r))) << "a project with a build.mcpp does not build for wasm32-emscripten (the host compiler for build.mcpp was the row's em++): " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "AssertionError"))) << "emcc.py's assertion is in the log: " << s::text(r);
        expect(fatal(p.exists("bp-env.txt"))) << "the build program did not run: " << s::text(r);
        auto host = recorded(p, "host"), target = recorded(p, "target");
        expect(fatal(target == "wasm32-emscripten")) << "MCPP_TARGET was '" << target << "'";
        expect(fatal(!host.empty() && !host.starts_with("wasm32"))) << "MCPP_HOST was '" << host << "'";
        std::optional<fs::path> js;
        for (const auto& f : s::find_files(p.at("target/wasm32-emscripten"), R"(bp\.js)"))
            if (f.generic_string().ends_with("/bin/bp.js")) js = f;
        expect(fatal(js.has_value())) << "no bin/bp.js: " << s::text(r);
        r = p.mcpp({"run", "--target", "wasm32-emscripten"});
        expect(fatal(e2e::succeeded(r))) << "mcpp run failed: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), "^1-2-3$"))) << "the program did not print 1-2-3: " << s::text(r);
        // PHASE 2: a fresh home whose first invocation is the cross build
        const s::options fresh{.where = s::home::fresh, .env = {{"MCPP_VERBOSE", "1"}}};
        const s::options fresh_quiet{.where = s::home::fresh};
        s::project q(tmp / "q", {{"mcpp.toml", manifest("bq")}, {"src/main.cpp", main_cpp}, {"build.mcpp", build_program}});
        r = q.mcpp({"build", "--target", "wasm32-emscripten"}, fresh);
        const auto log = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "a fresh $MCPP_HOME's first build does not build for wasm32-emscripten (row-pin fallback picked the row's own compiler as the host toolchain again): " << log;
        expect(fatal(s::lacks(log, "AssertionError"))) << "emcc.py's assertion is in the fresh-home log: " << log;
        expect(fatal(s::lacks(log, "host toolchain for build.mcpp: clang"))) << "the fresh-home host toolchain for build.mcpp was the Web row's own clang: " << log;
        expect(fatal(!s::contains_nocase(log, "features.h"))) << "features.h was reported missing while compiling the mcpp module (host toolchain has no C library attached): " << log;
        expect(fatal(q.exists("bp-env.txt"))) << "the build program did not run in the fresh home: " << log;
        host = recorded(q, "host");
        target = recorded(q, "target");
        expect(fatal(target == "wasm32-emscripten")) << "fresh-home MCPP_TARGET was '" << target << "'";
        expect(fatal(!host.empty() && !host.starts_with("wasm32"))) << "fresh-home MCPP_HOST was '" << host << "'";
        js.reset();
        for (const auto& f : s::find_files(q.at("target/wasm32-emscripten"), R"(bq\.js)"))
            if (f.generic_string().ends_with("/bin/bq.js")) js = f;
        expect(fatal(js.has_value())) << "no bin/bq.js in the fresh home: " << log;
        r = q.mcpp({"run", "--target", "wasm32-emscripten"}, fresh_quiet);
        expect(fatal(e2e::succeeded(r))) << "mcpp run failed in the fresh home: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), "^1-2-3$"))) << "the program did not print 1-2-3 in the fresh home: " << s::text(r);
        // PHASE 3: the same fresh home's native path
        q.remove("target");
        r = q.mcpp({"build"}, fresh_quiet);
        expect(fatal(e2e::succeeded(r))) << "the native build in the same fresh home regressed: " << s::text(r);
        std::optional<fs::path> native;
        for (const auto& f : s::find_files(q.at("target"), "bq"))
            if (f.generic_string().ends_with("/bin/bq")) { native = f; break; }
        expect(fatal(native.has_value())) << "no native bin/bq in the same fresh home: " << s::text(r);
        const auto out = s::run(*native);
        auto printed = out.out;
        while (printed.ends_with('\n')) printed.pop_back();
        expect(fatal(printed == "1-2-3")) << "the native bin/bq in the same fresh home did not print 1-2-3";
    };

    "981 a web link has the profiles level and the wasm exception model"_test[s::elf] = [] {
        // The profile's level reaches the link (-O2 release, -O0 dev); `exceptions = "wasm"`
        // compiles and links -fwasm-exceptions and catches; `true` keeps the JavaScript model;
        // another value is refused naming the values.
        if (!have_emsdk()) skip("981 -- no emsdk payload installed");
        s::project app(s::case_dir() / "app", {{"src/main.cpp", "import std;\nint main() {\n    try { throw std::runtime_error(\"boom\"); }\n    catch (std::exception const& e) { std::println(\"caught: {}\", e.what()); }\n}\n"}});
        auto write = [&](std::string_view value) {
            std::string m = "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n[target.wasm32-emscripten]\nrunner = [\"node\"]\n";
            if (!value.empty()) m += std::format("\n[target.'cfg(os = \"emscripten\")'.abi]\nexceptions = {}\n", value);
            app.write("mcpp.toml", m);
        };
        // The newest build.ninja of the row (`ls -t | head -1`).
        auto ninja = [&] {
            std::optional<fs::path> newest;
            for (const auto& f : s::find_files(app.at("target/wasm32-emscripten"), R"(build\.ninja)"))
                if (!f.generic_string().contains("/.build-mcpp/") && (!newest || fs::last_write_time(f) > fs::last_write_time(*newest))) newest = f;
            expect(fatal(newest.has_value())) << "no build.ninja";
            return s::read_file(*newest);
        };
        // 1
        write("");
        auto r = app.mcpp({"build", "--release", "--target", "wasm32-emscripten"});
        expect(fatal(e2e::succeeded(r))) << "release build failed: " << s::text(r);
        expect(fatal(s::has_line(lines_starting(ninja(), "ldflags"), "(^| )-O2( |$)"))) << "the release link carries no -O2";
        r = app.mcpp({"build", "--target", "wasm32-emscripten"});
        expect(fatal(e2e::succeeded(r))) << "dev build failed: " << s::text(r);
        expect(fatal(s::has_line(lines_starting(ninja(), "ldflags"), "(^| )-O0( |$)"))) << "the dev link carries no -O0";
        // 2
        write("\"wasm\"");
        r = app.mcpp({"build", "--target", "wasm32-emscripten"});
        expect(fatal(e2e::succeeded(r))) << "the wasm-model build failed: " << s::text(r);
        auto n = ninja();
        expect(fatal(e2e::contains(lines_starting(n, "cxxflags"), "-fwasm-exceptions"))) << "-fwasm-exceptions is not compiled with";
        expect(fatal(e2e::contains(lines_starting(n, "ldflags"), "-fwasm-exceptions"))) << "-fwasm-exceptions is not linked with";
        expect(fatal(s::no_line(lines_starting(n, "cxxflags"), "(^| )-fexceptions( |$)"))) << "the JS model is also selected";
        r = app.mcpp({"run", "--target", "wasm32-emscripten"});
        expect(fatal(e2e::succeeded(r))) << "the wasm-model program did not run: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), "^caught: boom$"))) << "the wasm-model program did not catch: " << s::text(r);
        // 3
        write("true");
        r = app.mcpp({"build", "--target", "wasm32-emscripten"});
        expect(fatal(e2e::succeeded(r))) << "the js-model build failed: " << s::text(r);
        n = ninja();
        expect(fatal(s::has_line(lines_starting(n, "cxxflags"), "(^| )-fexceptions( |$)"))) << "true no longer selects -fexceptions";
        expect(fatal(s::lacks(lines_starting(n, "cxxflags"), "-fwasm-exceptions"))) << "true selects the wasm model";
        // 4
        write("\"seh\"");
        r = app.mcpp({"build", "--target", "wasm32-emscripten"});
        expect(fatal(!r)) << "exceptions = \"seh\" was accepted: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "\"wasm\" or \"js\""))) << "the refusal does not name the values: " << s::text(r);
    };

    "1079 an emscripten rows longjmp model round trips"_test[s::elf] = [] {
        // `longjmp = "wasm"` makes a non-local exit over two call levels round-trip; on a Linux
        // row it is refused naming that row's os.
        if (!have_emsdk() || !s::on_path("node")) skip("1079 -- no emsdk payload or node on this machine");
        s::project app(s::case_dir() / "app", {{"mcpp.toml", "[package]\nname    = \"lj\"\nversion = \"0.1.0\"\n\n[targets.lj]\nkind = \"bin\"\nmain = \"src/main.c\"\n\n[target.wasm32-emscripten]\nrunner = [\"node\"]\n\n"
                                                             "[target.'cfg(os = \"emscripten\")'.abi]\nlongjmp = \"wasm\"\n\n[target.'cfg(os = \"emscripten\")'.build]\nldflags = [\"-sSTACK_SIZE=1048576\"]\n"},
                                               {"src/main.c", R"(/* Emscripten's libc declares the sigsetjmp/siglongjmp aliases behind the
   feature macros, like every libc; the fixture states the one it needs. */
#define _GNU_SOURCE 1
#include <setjmp.h>
#include <stdio.h>

static jmp_buf home;

static void level_two(void) {
    volatile int kept = 7;
    siglongjmp(home, 1);
    (void)kept;
}

static void level_one(void) { level_two(); }

int main(void) {
    volatile int marker = 0;
    if (sigsetjmp(home, 1) == 0) {
        marker = 1;
        level_one();
        printf("unreachable\n");
        return 1;
    }
    /* The volatile locals of the frames unwound by the non-local exit. */
    if (marker != 1) { printf("marker lost\n"); return 2; }
    printf("round-trip\n");
    return 0;
}
)"}});
        auto r = app.mcpp({"build", "--target", "wasm32-emscripten"});
        expect(fatal(e2e::succeeded(r))) << "the wasm build with longjmp = \"wasm\" failed: " << s::text(r);
        r = app.mcpp({"run", "--target", "wasm32-emscripten"});
        expect(fatal(e2e::succeeded(r))) << "the run failed -- the non-local exit did not round-trip: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), "^round-trip$"))) << "the program did not reach its end: " << s::text(r);
        app.write("mcpp.toml", "[package]\nname    = \"lj\"\nversion = \"0.1.0\"\n\n[targets.lj]\nkind = \"bin\"\nmain = \"src/main.c\"\n\n[target.'cfg(os = \"linux\")'.abi]\nlongjmp = \"wasm\"\n");
        r = app.mcpp({"build", "--target", "x86_64-linux-musl"});
        expect(fatal(!r)) << "longjmp = \"wasm\" on a Linux row was accepted: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "not an Emscripten row"))) << "the refusal does not name the row's own os: " << s::text(r);
    };
}};

}  // namespace
