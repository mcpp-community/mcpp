// MinGW-w64 GCC on a Windows host: the xlings payload installs, becomes the default and builds
// modules and `import std`, and the program and the build program run without the toolchain on
// PATH; a machine without Visual Studio builds on first use with the GNU-ABI row, says so,
// persists it, and does not overrule a project that wrote the MSVC ABI (formerly tests/e2e/97,
// 182).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

// The program copied into a directory of its own and run there with only the system's directory on
// PATH: what a user who received the file alone would see.
e2e::outcome run_alone(const fs::path& program, std::string_view directory) {
    const auto dir = s::case_dir() / std::string(directory);
    fs::create_directories(dir);
    fs::copy_file(program, dir / program.filename(), fs::copy_options::overwrite_existing);
    const auto root = s::env("SystemRoot").value_or("C:\\Windows");
    return s::run(dir / program.filename(), {}, dir, {{"PATH", root + "\\System32"}});
}

// The DLL names of an image's import table, by the MinGW payload's objdump (empty without one).
std::string imports_of(const fs::path& image) {
    for (const auto& objdump : s::glob(s::shared_home() / "registry/data/xpkgs", "xim-x-mingw-gcc/*/bin/objdump.exe")) {
        std::string names;
        for (const auto& line : s::lines_with(s::run(objdump, {"-p", image.string()}).out, "DLL Name")) names += line + "\n";
        return names;
    }
    return {};
}

void self_contained(const std::string& imports, std::string_view what) {
    for (std::string_view dll : {"libstdc", "libgcc", "libwinpthread"})
        expect(fatal(!s::contains_nocase(imports, dll))) << what << " imports a toolchain DLL (" << dll << "): " << imports;
}

E2E mingw{"mingw", [] {
    "97 mingw toolchain"_test[s::windows][resource{"machine"}] = [] {
        // `toolchain install mingw 16.1.0` installs the payload; `toolchain default mingw@16.1.0`
        // persists gcc@16.1.0 and x86_64-windows-gnu, both starred by `list`; a project with a named
        // module and `import std` builds and runs; the program and the build.mcpp host helper import
        // no toolchain DLL and run without the toolchain on PATH (#299).
        s::default_kept kept;
        const auto cwd = s::case_dir();
        auto r = s::mcpp({"toolchain", "install", "mingw", "16.1.0"}, cwd);
        expect(fatal(e2e::succeeded(r))) << "install mingw: " << s::text(r);
        r = s::mcpp({"toolchain", "default", "mingw@16.1.0"}, cwd);
        expect(fatal(e2e::succeeded(r))) << "default mingw@16.1.0: " << s::text(r);
        const auto list = s::text(s::mcpp({"toolchain", "list"}, cwd));
        expect(fatal(std::regex_search(list, std::regex(R"(\*\s*gcc 16\.1\.0)")))) << "gcc 16.1.0 is not starred among the toolchains: " << list;
        expect(fatal(std::regex_search(list, std::regex(R"(\*\s*x86_64-windows-gnu)")))) << "x86_64-windows-gnu is not starred among the targets: " << list;
        r = s::mcpp({"new", "hello_mingw"}, cwd);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        s::project p(cwd / "hello_mingw", {{"src/greet.cppm", "export module hello.greet;\nimport std;\nexport namespace hello {\nstd::string greet() { return \"mingw-ok\"; }\n}\n"},
                                           {"src/main.cpp", "import std;\nimport hello.greet;\nint main() { std::println(\"{}\", hello::greet()); return 0; }\n"}});
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build: " << s::text(r);
        r = p.mcpp({"run"});
        expect(fatal(e2e::succeeded(r) && e2e::contains(s::text(r), "mingw-ok"))) << "run: " << s::text(r);
        std::optional<fs::path> exe;
        for (const auto& f : s::find_files(p.at("target"), "hello_mingw\\.exe"))
            if (f.parent_path().generic_string().contains("/bin")) exe = f;
        expect(fatal(exe.has_value())) << "no exe produced";
        self_contained(imports_of(*exe), "the program");
        auto alone = run_alone(*exe, "iso");
        expect(fatal(e2e::succeeded(alone) && alone.out.contains("mingw-ok"))) << "the program alone: " << s::text(alone);
        p.write("build.mcpp", "#include <cstdio>\nint main() {\n    FILE* marker = std::fopen(\"helper-ran\", \"w\");\n    if (!marker) return 2;\n    std::fputs(\"ok\\n\", marker);\n    std::fclose(marker);\n    std::puts(\"mcpp:rerun-if-changed=build.mcpp\");\n    return 0;\n}\n");
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r) && p.exists("helper-ran"))) << "build with build.mcpp: " << s::text(r);
        const auto helper = p.at("target/.build-mcpp/build.mcpp.exe");
        expect(fatal(fs::exists(helper))) << "no helper binary at " << helper.string();
        self_contained(imports_of(helper), "the build.mcpp helper");
        alone = run_alone(helper, "hiso");
        expect(fatal(e2e::succeeded(alone))) << "the build.mcpp helper does not run without the toolchain on PATH: " << s::text(alone);
    };

    "182 windows no msvc fallback"_test[s::windows][s::no_msvc] = [] {
        // A Windows machine without Visual Studio (the CI job masks it) builds on first use: the
        // default becomes the GNU-ABI row, the build says so, `list` stars gcc and
        // x86_64-windows-gnu, a second build is silent, the program runs alone, and a project that
        // wrote the MSVC ABI with llvm is refused, naming the working alternative. Step 0 holds the
        // masking to account: msvc@system must not resolve.
        const s::options fresh{.where = s::home::fresh};
        const auto cwd = s::case_dir();
        auto r = s::mcpp({"toolchain", "default", "msvc"}, cwd, fresh);
        expect(fatal(failed(r))) << "0: msvc@system still resolves -- the masking of Visual Studio is incomplete, so nothing here would test the path without it";
        r = s::mcpp({"new", "bare_win"}, cwd, fresh);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto bare = cwd / "bare_win";
        r = s::mcpp({"build"}, bare, fresh);
        if (!e2e::succeeded(r)) {
            // What a failure of the payload's driver needs to be told apart: each g++ the fresh home
            // holds, its version and its frontend.
            for (const auto& gxx : s::find_files(s::case_home(s::home::fresh) / "registry/data/xpkgs", "g\\+\\+\\.exe")) {
                std::println("driver {}: {}", gxx.string(), s::text(s::run(gxx, {"--version"})));
                std::println("  cc1plus: {}", s::text(s::run(gxx, {"-print-prog-name=cc1plus"})));
            }
        }
        expect(fatal(e2e::succeeded(r))) << "1: the first build on a machine without Visual Studio: " << s::text(r);
        expect(fatal(s::contains_nocase(s::text(r), "x86_64-windows-gnu"))) << "1: the fallback is not reported: " << s::text(r);
        auto run = s::mcpp({"run"}, bare, fresh);
        expect(fatal(e2e::succeeded(run) && s::contains_nocase(s::text(run), "hello"))) << "1: run: " << s::text(run);
        const auto list = s::text(s::mcpp({"toolchain", "list"}, cwd, fresh));
        expect(fatal(std::regex_search(list, std::regex(R"(\*\s*gcc)")))) << "2: no gcc starred among the toolchains: " << list;
        expect(fatal(std::regex_search(list, std::regex(R"(\*\s*x86_64-windows-gnu)")))) << "2: x86_64-windows-gnu is not starred: " << list;
        r = s::mcpp({"build"}, bare, fresh);
        expect(fatal(e2e::succeeded(r))) << "2: the second build: " << s::text(r);
        std::optional<fs::path> exe;
        for (const auto& f : s::find_files(bare / "target", "bare_win\\.exe"))
            if (f.parent_path().generic_string().contains("/bin")) exe = f;
        expect(fatal(exe.has_value())) << "3: no exe produced";
        auto alone = run_alone(*exe, "iso");
        expect(fatal(e2e::succeeded(alone))) << "3: the program does not run without the toolchain on PATH: " << s::text(alone);
        // 4. mcpp revises a default it chose; it does not swap the ABI of a project that wrote it.
        r = s::mcpp({"new", "explicit_msvc"}, cwd, fresh);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto explicit_msvc = cwd / "explicit_msvc";
        s::project(explicit_msvc, {{"mcpp.toml", s::read_file(explicit_msvc / "mcpp.toml") +
                                                     std::format("\n[toolchain]\nwindows = \"llvm@{}\"\n\n[build]\ntarget = \"x86_64-windows-msvc\"\n", s::llvm_version())}});
        r = s::mcpp({"build"}, explicit_msvc, fresh);
        expect(fatal(failed(r))) << "4: an explicit MSVC target with [toolchain] windows = llvm was silently replaced: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "x86_64-windows-gnu"))) << "4: the error does not point at the working alternative: " << s::text(r);
    };
}};

}  // namespace
