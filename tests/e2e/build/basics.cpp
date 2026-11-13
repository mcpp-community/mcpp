// The build basics: a new project built and run, incremental rebuilds, static and shared
// libraries, convention mode, C sources, profiles, serial and concurrent plans, parent-directory
// glob inputs, a workspace root's profile by position, release path trimming and NDEBUG, one
// variant per program (formerly tests/e2e/02, 04, 07, 08, 25, 26, 68, 87, 861, 882, 894, 929,
// 1003).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

// The first build.ninja of the project's target, outside the build program's tree.
std::optional<fs::path> graph_of(const s::project& p) {
    for (const auto& f : s::find_files(p.at("target"), R"(build\.ninja)"))
        if (f.generic_string().find("/.build-mcpp/") == std::string::npos) return f;
    return std::nullopt;
}

// The text of the project's build.ninja (the case fails when there is none).
std::string graph_text(const s::project& p) {
    const auto g = graph_of(p);
    expect(fatal(g.has_value())) << "no build.ninja generated";
    return s::read_file(*g);
}

// The entries of `dir`, sorted (`ls -d <dir>/*`).
std::vector<fs::path> entries(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (fs::is_directory(dir, ec))
        for (const auto& e : fs::directory_iterator(dir)) out.push_back(e.path());
    std::ranges::sort(out);
    return out;
}

// `[ -x file ]`.
bool executable(const fs::path& f) {
    std::error_code ec;
    return fs::is_regular_file(f, ec) && (fs::status(f, ec).permissions() & fs::perms::owner_exec) != fs::perms::none;
}

// A program's standard output without its trailing newline (`$(program)`).
std::string output_of(const fs::path& program) {
    auto out = s::run(program).out;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

// The lines of `text` (`while read line`).
std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> out;
    std::istringstream in{std::string(text)};
    for (std::string l; std::getline(in, l);) out.push_back(l);
    return out;
}

// A file's modification time moved past its present one (`sleep 1; touch file`).
void touch(const fs::path& f) { fs::last_write_time(f, fs::last_write_time(f) + std::chrono::seconds(2)); }

// The first file under `root` whose path ends with `tail` (`find <root> -path '*<tail>'`).
std::optional<fs::path> path_ending(const fs::path& root, std::string_view tail) {
    for (const auto& f : s::find_files(root, ".*"))
        if (f.generic_string().ends_with(tail)) return f;
    return std::nullopt;
}

E2E basics{"basics", [] {
    "02 new build run"_test[s::fresh_sandbox] = [] {
        // `mcpp new` writes a project that builds into target/ and runs, passing its arguments.
        auto p = s::project::scaffold("hello");
        for (std::string_view f : {"mcpp.toml", "src/main.cpp", ".gitignore"})
            expect(fatal(p.exists(f))) << "missing " << f;
        expect(fatal(e2e::contains(p.read("src/main.cpp"), "import std"))) << "main.cpp missing 'import std'";
        expect(fatal(e2e::contains(p.read("src/main.cpp"), "std::println"))) << "main.cpp missing 'std::println'";
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed\n" << s::text(r);
        expect(fatal(p.exists("target"))) << "no target/ dir";
        const auto binary = s::find_file(p.at("target"), s::exe("hello"));
        expect(fatal(binary.has_value())) << "binary not produced";
        expect(fatal(executable(*binary))) << "binary not executable";
        r = p.mcpp({"run"});
        expect(fatal(e2e::contains(s::text(r), "Hello from hello"))) << "unexpected run output: " << s::text(r);
        r = p.mcpp({"run", "--", "foo", "bar"});
        expect(e2e::contains(s::text(r), "foo") && e2e::contains(s::text(r), "bar")) << "args passthrough failed: " << s::text(r);
    };

    "04 incremental"_test = [] {
        // A rebuild finishes; a touched main.cpp is rebuilt and the std module is not.
        auto p = s::project::scaffold("inc");
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "build failed";
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::contains(s::text(r), "Finished") || e2e::contains(s::text(r), "Build OK"))) << "rebuild failed: " << s::text(r);
        touch(p.at("src/main.cpp"));
        r = p.mcpp({"build", "--verbose"});
        expect(fatal(e2e::contains(s::text(r), "main.cpp"))) << "main.cpp not rebuilt: " << s::text(r);
        expect(s::lacks(s::text(r), "std.cc")) << "std module re-built when it shouldn't be";
    };

    "07 static library"_test[s::elf] = [] {
        // A `kind = "lib"` target is archived (cxx_archive) into a non-empty libmathlib.a of objects.
        auto p = s::project::scaffold("mathlib");
        p.write("src/math.cppm", R"cpp(export module mathlib;
import std;
export int add(int a, int b) { return a + b; }
export int mul(int a, int b) { return a * b; }
)cpp");
        p.remove("src/main.cpp");
        p.write("mcpp.toml", R"cpp([package]
name        = "mathlib"
version     = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
[modules]
sources = ["src/**/*.cppm"]
[targets.mathlib]
kind = "lib"
)cpp");
        auto r = p.mcpp({"build"});
        const auto archive = s::find_file(p.at("target"), "libmathlib.a");
        expect(fatal(archive.has_value())) << "no .a produced\n" << s::text(r);
        expect(fatal(fs::file_size(*archive) > 0)) << "empty .a";
        expect(fatal(std::ranges::any_of(s::archive_members(*archive), [](const std::string& m) { return m.ends_with(".o"); }))) << ".a contains no objects";
        expect(e2e::contains(graph_text(p), "bin/libmathlib.a : cxx_archive")) << "build.ninja did not use cxx_archive for the lib target";
    };

    "08 shared library"_test[s::elf] = [] {
        // A `kind = "shared"` target links (cxx_shared) an ELF shared object, compiled with -fPIC.
        auto p = s::project::scaffold("mathlib");
        p.write("src/math.cppm", R"cpp(export module mathlib;
import std;
export int add(int a, int b) { return a + b; }
)cpp");
        p.remove("src/main.cpp");
        p.write("mcpp.toml", R"cpp([package]
name        = "mathlib"
version     = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
[modules]
sources = ["src/**/*.cppm"]
[targets.mathlib]
kind = "shared"
)cpp");
        auto r = p.mcpp({"build"});
        const auto so = s::find_file(p.at("target"), "libmathlib.so");
        expect(fatal(so.has_value())) << "no .so produced\n" << s::text(r);
        const auto h = s::read_header(*so);
        expect(fatal(h.kind == s::format::elf && h.elf_type == 3)) << "not a valid .so";
        const auto g = graph_text(p);
        expect(fatal(e2e::contains(g, "bin/libmathlib.so : cxx_shared"))) << "build.ninja did not use cxx_shared for the shared lib target";
        expect(s::has_line(g, "^cxxflags.*-fPIC")) << "cxxflags did not include -fPIC for shared-lib build";
    };

    "25 convention mode"_test = [] {
        // A manifest with only [package] infers its sources, a bin from src/main.cpp, include/
        // and a lib from a .cppm in src/, each announced; a legacy explicit schema infers nothing.
        const auto tmp = s::case_dir();
        auto program = [](const s::project& p, std::string_view name) {
            // `ls -d target/*/ | head -1`, then the one directory below it.
            auto first_dir = [](const fs::path& d) {
                for (const auto& e : entries(d))
                    if (fs::is_directory(e) && !e.filename().string().starts_with('.')) return e;
                expect(fatal(false)) << "no directory under " << d.string();
                return fs::path{};
            };
            return output_of(first_dir(first_dir(p.at("target"))) / "bin" / s::exe(name));
        };
        s::project tiny{tmp / "tinyapp", {{"mcpp.toml", R"cpp([package]
name    = "tinyapp"
version = "0.1.0"
)cpp"}, {"src/main.cpp", R"cpp(import std;
int main() { std::println("convention-mode bin OK"); return 0; }
)cpp"}}};
        auto r = tiny.mcpp({"build"});
        expect(fatal(s::has_line(s::text(r), R"(Inferred sources \[src/\*\*)"))) << "no Inferred sources line\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Inferred target tinyapp (bin from src/main.cpp)"))) << "no Inferred target line\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Compiling tinyapp v0.1.0"))) << "not compiling\n" << s::text(r);
        expect(fatal(program(tiny, "tinyapp") == "convention-mode bin OK")) << "runtime out='" << program(tiny, "tinyapp") << "'";
        s::project inc{tmp / "inc", {{"mcpp.toml", R"cpp([package]
name    = "inc"
version = "0.1.0"
)cpp"}, {"include/inc/api.hpp", R"cpp(#pragma once
inline int answer() { return 42; }
)cpp"}, {"src/main.cpp", R"cpp(#include <inc/api.hpp>
import std;
int main() { std::println("answer = {}", answer()); return 0; }
)cpp"}}};
        r = inc.mcpp({"build"});
        expect(fatal(s::has_line(s::text(r), R"(Inferred include_dirs \[include\])"))) << "include/ not auto-picked\n" << s::text(r);
        expect(fatal(program(inc, "inc") == "answer = 42")) << "include resolution: " << program(inc, "inc");
        s::project lib{tmp / "mylib", {{"mcpp.toml", R"cpp([package]
name    = "mylib"
version = "0.1.0"
)cpp"}, {"src/mylib.cppm", R"cpp(export module mylib;
import std;
export auto greet() -> void { std::println("from auto-lib"); }
)cpp"}}};
        r = lib.mcpp({"build"});
        expect(fatal(e2e::contains(s::text(r), "Inferred target mylib (lib from .cppm in src/)"))) << "lib auto-target not inferred\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Finished"))) << "lib didn't finish\n" << s::text(r);
        s::project legacy{tmp / "legacy", {{"mcpp.toml", R"cpp([package]
name    = "legacy"
version = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
[modules]
sources = ["src/**/*.cpp"]
[targets.legacy]
kind = "bin"
main = "src/main.cpp"
)cpp"}, {"src/main.cpp", R"cpp(import std;
int main() { std::println("legacy schema OK"); return 0; }
)cpp"}}};
        r = legacy.mcpp({"build"});
        expect(fatal(s::lacks(s::text(r), "Inferred"))) << "legacy schema fired Inferred banner unexpectedly";
        expect(program(legacy, "legacy") == "legacy schema OK") << "legacy runtime: " << program(legacy, "legacy");
    };

    "26 c language support"_test[s::gcc] = [] {
        // C sources route to the c_object rule with the C compiler, c_standard and the package's
        // cflags, beside the C++ rule's own; the mixed program links and runs.
        auto p = s::project::scaffold("cmix");
        p.write("src/cmix_core.c", R"cpp(#include <stdlib.h>
#include <string.h>
int *cmix_dup(const int *restrict src, size_t n) {
    int *out = malloc(n * sizeof(int));
    if (!out) return 0;
    memcpy(out, src, n * sizeof(int));
    return out;
}
int cmix_sum(const int *p, size_t n) {
    int s = 0;
    for (size_t i = 0; i < n; ++i) s += p[i];
    return s;
}
)cpp");
        p.write("src/main.cpp", R"cpp(import std;
extern "C" int *cmix_dup(const int *src, std::size_t n);
extern "C" int  cmix_sum(const int *p,  std::size_t n);
int main() {
    int data[] = {1, 2, 3, 4, 5};
    int *copy  = cmix_dup(data, 5);
    int  s     = cmix_sum(copy, 5);
    std::println("cmix sum = {}", s);
    std::free(copy);
    return s == 15 ? 0 : 1;
}
)cpp");
        p.write("mcpp.toml", R"cpp([package]
name        = "cmix"
version     = "0.1.0"
[build]
cflags      = ["-DCMIX_C_BUILD=1"]
cxxflags    = ["-DCMIX_CXX_BUILD=1"]
c_standard  = "c11"
)cpp");
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed\n" << s::text(r);
        const auto g = graph_text(p);
        expect(fatal(s::has_line(g, "^rule c_object"))) << "missing c_object rule\n" << g;
        expect(fatal(s::has_line(g, "^cc        = .*(gcc|cc|clang)"))) << "cc variable not pointing at a C compiler";
        expect(fatal(s::has_line(g, "^cflags    = -std=c11"))) << "cflags missing -std=c11 baseline";
        expect(fatal(s::has_line(g, R"(^cxxflags  = -std=c\+\+23)"))) << "cxxflags missing -std=c++23 baseline";
        expect(fatal(e2e::contains(g, "unit_cflags = -DCMIX_C_BUILD=1"))) << "unit cflags missing user C flag";
        expect(fatal(e2e::contains(g, "unit_cxxflags = -DCMIX_CXX_BUILD=1"))) << "unit cxxflags missing user C++ flag";
        expect(fatal(s::has_line(g, R"(build obj/cmix_core\.o : c_object .*cmix_core\.c)"))) << "cmix_core.c not routed to c_object rule";
        expect(fatal(s::no_line(g, R"(build obj/cmix_core\.o : cxx_object)"))) << "cmix_core.c was incorrectly routed to cxx_object";
        r = p.mcpp({"run"});
        expect(s::last_line(r.out) == "cmix sum = 15") << "unexpected output: " << s::last_line(r.out);
    };

    "68 profile passthrough"_test = [] {
        // release builds -O2, dev (the default) -O0 -g, and a [profile.dist] table its opt
        // level and passthrough flags; the dist program runs.
        auto p = s::project::scaffold("profapp");
        p.write("mcpp.toml", p.read("mcpp.toml") + R"cpp(
[profile.dist]
opt      = 3
strip    = true
cxxflags = ["-fno-plt"]
)cpp");
        auto r = p.mcpp({"build", "--release", "--verbose"});
        expect(fatal(e2e::succeeded(r))) << "release build failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "-O2"))) << "release missing -O2";
        p.remove("target");
        r = p.mcpp({"build", "--verbose"});
        expect(fatal(e2e::succeeded(r))) << "dev build failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "-O0"))) << "default (dev) missing -O0";
        expect(fatal(e2e::contains(s::text(r), "-g"))) << "default (dev) missing -g";
        p.remove("target");
        r = p.mcpp({"build", "--profile", "dist", "--verbose"});
        expect(fatal(e2e::succeeded(r))) << "dist build failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "-O3"))) << "dist missing -O3";
        expect(fatal(e2e::contains(s::text(r), "-fno-plt"))) << "dist missing passthrough -fno-plt";
        const auto binary = s::find_file(p.at("target"), s::exe("profapp"));
        expect(fatal(binary.has_value())) << "no binary";
        expect(e2e::succeeded(s::run(*binary))) << "dist binary does not run";
    };

    "87 build default profile"_test = [] {
        // The global default profile is dev (-O0 -g) and --release gives -O2; a project's
        // [build] default-profile = "release" gives -O2 and --dev overrides it.
        const auto tmp = s::case_dir();
        const s::options cdb{.env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};
        s::project plain{tmp / "plain", {{"mcpp.toml", "[package]\nname    = \"plain\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", "int main() { return 0; }\n"}}};
        auto cc = [](const s::project& p) { return p.read("compile_commands.json"); };
        expect(fatal(e2e::succeeded(plain.mcpp({"build"}, cdb)))) << "build errored";
        expect(fatal(e2e::contains(cc(plain), "-O0"))) << "global default is not dev (-O0)\n" << cc(plain);
        expect(fatal(std::regex_search(cc(plain), std::regex{R"(-g\b)"}))) << "global default dev lacks -g";
        expect(fatal(e2e::succeeded(plain.mcpp({"build", "--release"}, cdb)))) << "--release errored";
        expect(fatal(e2e::contains(cc(plain), "-O2"))) << "--release did not yield -O2\n" << cc(plain);
        s::project opt{tmp / "opt", {{"mcpp.toml", "[package]\nname    = \"opt\"\nversion = \"0.1.0\"\n[build]\ndefault-profile = \"release\"\n"}, {"src/main.cpp", "int main() { return 0; }\n"}}};
        expect(fatal(e2e::succeeded(opt.mcpp({"build"}, cdb)))) << "build errored";
        expect(fatal(e2e::contains(cc(opt), "-O2"))) << "[build].default-profile=release did not yield -O2\n" << cc(opt);
        expect(fatal(e2e::succeeded(opt.mcpp({"build", "--dev"}, cdb)))) << "--dev errored";
        expect(e2e::contains(cc(opt), "-O0")) << "--dev did not override project default to -O0\n" << cc(opt);
    };
    "861 concurrent compiles plan what serial compiles plan"_test[s::unix_shell] = [] {
        // A workspace built at -j 4 writes the same build.ninja files and build-program caches
        // as at -j 1 (A), runs the same three build programs in the same order (B), core's first,
        // and links their three link flags in the same order (C).
        const s::options o{.where = s::home::inherited};
        s::project ws{s::files{{"mcpp.toml", R"cpp([workspace]
members = ["core", "cli", "gui", "app"]
)cpp"}, {"core/mcpp.toml", R"cpp([package]
name    = "core"
version = "0.1.0"

[targets.core]
kind = "lib"
)cpp"},
                               {"core/src/core.cppm", "export module t861_core;\nexport int core_v() { return 1; }\n"},
                               {"app/mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"

[dependencies]
cli = { path = "../cli" }
gui = { path = "../gui" }

[targets.app]
kind = "bin"
main = "src/main.cpp"
)cpp"},
                               {"app/src/main.cpp", "import t861_cli;\nimport t861_gui;\nint main() { return cli_v() + gui_v() == 2 ? 0 : 1; }\n"}}};
        for (std::string_view m : {"cli", "gui"}) {
            ws.write(std::format("{}/mcpp.toml", m), std::format(R"cpp([package]
name    = "{0}"
version = "0.1.0"

[dependencies]
core = {{ path = "../core" }}

[targets.{0}]
kind = "lib"
)cpp", m));
            ws.write(std::format("{0}/src/{0}.cppm", m), std::format("export module t861_{0};\nimport t861_core;\nexport int {0}_v() {{ return core_v(); }}\n", m));
        }
        for (std::string_view m : {"core", "cli", "gui"}) ws.write(std::format("{}/build.mcpp", m), std::format(R"cpp(import mcpp;
int main() {{
    mcpp::define("T861_{0}=1");
    mcpp::link_flag("-Wl,-rpath,/t861/{0}");
    return 0;
}}
)cpp", m));
        struct snapshot { std::vector<std::string> list; std::vector<std::string> ninjas; std::map<std::string, std::string> caches; };
        auto take = [&] {
            snapshot sn;
            for (const auto& f : s::find_files(ws.at("target"), R"(build\.ninja)"))
                if (f.generic_string().find("/.build-mcpp/") == std::string::npos) sn.list.push_back(f.generic_string());
            std::ranges::sort(sn.list);
            expect(fatal(!sn.list.empty())) << "no build.ninja was written";
            for (const auto& f : sn.list) sn.ninjas.push_back(s::read_file(f));
            for (std::string_view m : {"core", "cli", "gui"}) sn.caches[std::string(m)] = ws.read(std::format("{}/target/.build-mcpp/build.mcpp.cache", m));
            return sn;
        };
        auto ran_lines = [](std::string_view log) {
            std::vector<std::string> out;
            const std::regex re{R"(^ *build\.mcpp ([a-z]+) .*ran [0-9])"};
            for (const auto& l : lines_of(log)) { std::smatch m; if (std::regex_search(l, m, re)) out.push_back(m[1].str()); }
            return out;
        };
        auto link_order = [](std::string_view ninja) {
            std::vector<std::string> out;
            const std::string t{ninja};
            const std::regex re{R"(-Wl,-rpath,/t861/([a-z]+))"};
            for (auto it = std::sregex_iterator(t.begin(), t.end(), re); it != std::sregex_iterator(); ++it)
                if (!std::ranges::contains(out, (*it)[1].str())) out.push_back((*it)[1].str());
            return out;
        };
        auto serial_log = ws.mcpp({"build", "--workspace", "-j", "1"}, o);
        expect(fatal(e2e::succeeded(serial_log))) << "the build at -j 1 failed\n" << s::text(serial_log);
        const auto serial = take();
        ws.remove("target");
        for (std::string_view m : {"core", "cli", "gui", "app"}) ws.remove(std::format("{}/target", m));
        auto concurrent_log = ws.mcpp({"build", "--workspace", "-j", "4"}, o);
        expect(fatal(e2e::succeeded(concurrent_log))) << "the build at -j 4 failed\n" << s::text(concurrent_log);
        const auto concurrent = take();
        expect(fatal(serial.list == concurrent.list)) << "A: the two builds wrote build.ninja in different places";
        for (std::size_t i = 0; i < serial.ninjas.size(); ++i)
            expect(fatal(serial.ninjas[i] == concurrent.ninjas[i])) << "A: build.ninja." << i + 1 << " differs between the build at -j 1 and the build at -j 4";
        for (const auto& [m, cache] : serial.caches)
            expect(fatal(cache == concurrent.caches.at(m))) << "A: " << m << ".cache differs between the build at -j 1 and the build at -j 4";
        const auto ran = ran_lines(s::text(serial_log));
        expect(fatal(ran.size() == 3u)) << "B: expected three programs to run\n" << s::text(serial_log);
        expect(fatal(ran == ran_lines(s::text(concurrent_log)))) << "B: the programs ran in another order at -j 4";
        expect(fatal(ran.front() == "core")) << "C: core's program did not run first";
        expect(fatal(link_order(serial.ninjas.front()).size() == 3u)) << "C: the link line does not carry the three programs' link flags";
        expect(link_order(serial.ninjas.front()) == link_order(concurrent.ninjas.front())) << "C: the link flags are in another order at -j 4";
    };

    "882 parent directory glob inputs"_test = [] {
        // mcpp::rerun_if_changed_glob over a parent directory reruns the build program when the
        // membership changes (appears, added, removed) and not for content or non-members;
        // an absolute pattern works alike; a glob under target/ is reported as unwatchable; a
        // git dependency's glob that leaves its package is refused.
        const auto tmp = s::case_dir();
        const auto REGISTRY_HOST = (s::shared_home() / "registry").generic_string();
        const auto home = tmp / "mcpp-home";
        s::project{home, {{"config.toml", std::format(R"cpp([xlings]
home = "{0}"
)cpp", REGISTRY_HOST)}}};
        const s::options o{.env = {{"MCPP_HOME", home.string()}}};
        s::project app{tmp / "ws/app", {{"mcpp.toml", R"cpp([package]
name = "parent-glob"
version = "0.1.0"

[targets.parent-glob]
kind = "bin"
main = "src/main.cpp"
)cpp"}, {"src/main.cpp", R"cpp(#include <cstdio>
int count_inputs();
int main() { std::printf("COUNT=%d\n", count_inputs()); }
)cpp"}, {"build.mcpp", R"cpp(import std;
import mcpp;
int main() {
    namespace fs = std::filesystem;
    const fs::path inputs = fs::path(mcpp::manifest_dir()) / "../inputs";
    mcpp::rerun_if_changed_glob("../inputs/**/*.in");
    int count = 0;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(inputs, ec))
        if (entry.is_regular_file() && entry.path().extension() == ".in") ++count;
    const std::string output = std::string(mcpp::out_dir()) + "/count.cpp";
    { std::ofstream out(output); out << "int count_inputs() { return " << count << "; }\n"; }
    mcpp::generated(output.c_str());
    return 0;
}
)cpp"}}};
        const auto inputs = tmp / "ws/inputs";
        std::map<std::string, std::string> logs;
        auto build_count = [&](std::string_view step, int expected) {
            auto r = app.mcpp({"build"}, o);
            logs[std::string(step)] = s::text(r);
            expect(fatal(e2e::succeeded(r))) << "build " << step << " failed\n" << s::text(r);
            const auto got = s::last_starting(app.mcpp({"run"}, o).out, "COUNT=");
            expect(fatal(got == std::format("COUNT={}", expected))) << step << " expected COUNT=" << expected << ", got " << got;
        };
        build_count("missing", 0);
        s::project{inputs, {{"a.in", "a\n"}}};
        build_count("appeared", 1);
        s::project{inputs, {{"nested/b.in", "b\n"}}};
        build_count("added", 2);
        fs::remove(inputs / "nested/b.in");
        build_count("removed", 1);
        s::project{inputs, {{"a.in", "different content\n"}, {"ignored.txt", "not an input\n"}}};
        build_count("content", 1);
        build_count("unchanged", 1);
        for (std::string_view step : {"content", "unchanged"})
            expect(fatal(s::no_line(logs[std::string(step)], R"(^ *build\.mcpp .* ran [0-9])"))) << step << " reran the build program despite unchanged membership";
        const auto original = app.read("build.mcpp");
        const auto INPUTS_HOST = fs::canonical(inputs).generic_string();
        auto absolute = original;
        absolute.replace(absolute.find("\"../inputs/**/*.in\""), 19, "\"" + INPUTS_HOST + "/**/*.in\"");
        app.write("build.mcpp", absolute);
        expect(fatal(e2e::contains(app.read("build.mcpp"), INPUTS_HOST + "/**/*.in"))) << "the fixture did not take the absolute pattern";
        build_count("absolute", 1);
        s::project{inputs, {{"c.in", "c\n"}}};
        build_count("absolute-added", 2);
        fs::remove(inputs / "c.in");
        std::string second;
        for (const auto& l : lines_of(original)) {
            second += l + "\n";
            if (std::regex_search(l, std::regex{R"(rerun_if_changed_glob\("\.\./inputs)"})) second += "    mcpp::rerun_if_changed_glob(\"target/**/*.in\");\n";
        }
        app.write("build.mcpp", second);
        expect(fatal(e2e::contains(app.read("build.mcpp"), "rerun_if_changed_glob(\"target/"))) << "the fixture did not take the second glob";
        auto r = app.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "build with an unwatchable glob failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "rerun_if_changed_glob(\"target/**/*.in\") can never re-run it"))) << "the unwatchable glob was not reported\n" << s::text(r);
        app.write("build.mcpp", original);

        s::project origin{tmp / "origin", {{"mcpp.toml", R"cpp([package]
name = "sealed"
version = "0.1.0"

[targets.sealed]
kind = "lib"
)cpp"}, {"src/sealed.cppm", "export module t882_sealed;\nexport int sealed_value() { return 1; }\n"}, {"build.mcpp", R"cpp(import mcpp;
int main() {
    mcpp::rerun_if_changed_glob("../outside/**/*.in");
    return 0;
}
)cpp"}}};
        auto git = [&](std::vector<std::string> args) {
            args.insert(args.begin(), "git");
            auto g = e2e::run({.argv = args, .cwd = origin.dir()});
            expect(fatal(e2e::succeeded(g))) << "git failed\n" << s::text(g);
            return g.out;
        };
        git({"init", "--quiet"});
        git({"config", "user.email", "test@local"});
        git({"config", "user.name", "test"});
        git({"add", "-A"});
        git({"commit", "--quiet", "-m", "init"});
        auto REV = git({"rev-parse", "HEAD"});
        while (!REV.empty() && (REV.back() == '\n' || REV.back() == '\r')) REV.pop_back();
        const auto ORIGIN_HOST = origin.dir().generic_string();
        s::project consumer{tmp / "consumer", {{"mcpp.toml", std::format(R"cpp([package]
name = "consumer"
version = "0.1.0"

[dependencies]
sealed = {{ git = "{0}", rev = "{1}" }}
)cpp", ORIGIN_HOST, REV)}, {"src/main.cpp", "import t882_sealed;\nint main() { return sealed_value() == 1 ? 0 : 1; }\n"}}};
        r = consumer.mcpp({"build"}, o);
        expect(fatal(failed(r))) << "a git dependency's glob that leaves its package was accepted\n" << s::text(r);
        expect(e2e::contains(s::text(r), "which leaves the package")) << "the refusal did not name the escaping glob\n" << s::text(r);
    };
    "894 the position reading is kept and reported"_test[s::gcc] = [] {
        // A virtual workspace root's [profile.<name>] still reaches a member by its position and
        // says so naming [workspace.profile.<name>]; a member's own table replaces it whole; a
        // root package's own table is not a position reading, while its member is told.
        const auto tmp = s::case_dir();
        const s::options o{.where = s::home::inherited};
        auto flags_of = [](const s::project& ws, std::string_view suffix) {
            std::string out;
            for (const auto& db : s::find_files(ws.at("target"), R"(compile_commands\.json)"))
                for (auto& e : s::json_file(db)) {
                    auto file = e["file"].get<std::string>();
                    std::ranges::replace(file, '\\', '/');
                    if (!file.ends_with(suffix)) continue;
                    if (e.contains("arguments") && !e["arguments"].empty()) for (auto& a : e["arguments"]) out += a.get<std::string>() + " ";
                    else out += e["command"].get<std::string>() + " ";
                    out += "\n";
                }
            return out;
        };
        auto member = [](const s::project& ws, std::string_view dir, std::string_view extra) {
            ws.write(std::format("{}/mcpp.toml", dir), std::format("[package]\nname = \"{0}\"\nversion = \"0.1.0\"\n[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n{1}\n", dir, extra));
            ws.write(std::format("{}/src/main.cpp", dir), "int main() { return 0; }\n");
        };
        s::project virt{tmp / "virtual", {{"mcpp.toml", "[workspace]\nmembers = [\"plain\", \"own\"]\n\n[profile.release]\ncxxflags = [\"-DROOT_RELEASE=1\"]\n"}}};
        member(virt, "plain", "");
        member(virt, "own", "[profile.release]\ncxxflags = [\"-DOWN_RELEASE=1\"]");
        auto r = virt.mcpp({"build", "-p", "plain", "--release"}, o);
        expect(fatal(e2e::succeeded(r))) << "plain\n" << s::text(r);
        expect(fatal(e2e::contains(flags_of(virt, "plain/src/main.cpp"), "-DROOT_RELEASE=1"))) << "the root's [profile.release] no longer reaches the member by position";
        expect(fatal(e2e::contains(s::text(r), "[profile.<name>] on the workspace root reaches every member by its position"))) << "the position reading said nothing\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "[workspace.profile.<name>]"))) << "the warning names no canonical spelling\n" << s::text(r);
        r = virt.mcpp({"build", "-p", "own", "--release"}, o);
        expect(fatal(e2e::succeeded(r))) << "own\n" << s::text(r);
        const auto got = flags_of(virt, "own/src/main.cpp");
        expect(fatal(e2e::contains(got, "-DOWN_RELEASE=1"))) << "the member's own [profile.release] was lost: " << got;
        expect(fatal(s::lacks(got, "-DROOT_RELEASE=1"))) << "the position reading merged a named table it used to replace whole: " << got;
        s::project rooted{tmp / "rooted", {{"mcpp.toml", "[package]\nname = \"root\"\nversion = \"0.1.0\"\n[targets.root]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n[workspace]\nmembers = [\"sub\"]\n\n[profile.release]\ncxxflags = [\"-DROOT_RELEASE=1\"]\n"},
                                           {"src/main.cpp", "int main() { return 0; }\n"}}};
        member(rooted, "sub", "");
        r = rooted.mcpp({"build", "--release"}, o);
        expect(fatal(e2e::succeeded(r))) << "root package\n" << s::text(r);
        expect(fatal(s::lacks(s::text(r), "workspace-position") && s::lacks(s::text(r), "by its position"))) << "the root package's own table was reported as a position reading\n" << s::text(r);
        r = rooted.mcpp({"build", "-p", "sub", "--release"}, o);
        expect(fatal(e2e::succeeded(r))) << "sub\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "reaches member 'sub' by its position"))) << "a member that received the root's table by position was not told\n" << s::text(r);
        expect(e2e::contains(flags_of(rooted, "sub/src/main.cpp"), "-DROOT_RELEASE=1")) << "the position reading changed for a rooted workspace";
    };
    "929 release trims paths and defines ndebug"_test[s::gcc][s::elf][s::unix_shell] = [] {
        // A release trims the paths an artifact carries (__FILE__, source_location: the same in
        // two checkouts, no project path) and defines NDEBUG; dev keeps both; [profile.release]
        // ndebug/trim_paths = false keep them; debug = true writes a source map back to the root.
        const auto work = s::case_dir();
        const s::options o{.env = {{"NO_COLOR", "1"}}};
        const auto GCC_VERSION = s::gcc_version();
        auto mk = [&](std::string_view dir, std::string_view extra) {
            const std::string two{extra};
            return s::project{work / dir, {{"mcpp.toml", std::format(R"cpp([package]
name    = "app"
version = "1.2.0"
[toolchain]
default = "gcc@{0}"
{1}
)cpp", GCC_VERSION, two)}, {"src/main.cpp", R"cpp(#include <cassert>
import std;
int main() {
    std::cout << "file " << __FILE__ << ' ' << std::source_location::current().file_name()
              << std::endl;   // flushed: the assert below aborts the process
    assert(false && "assert fired");
}
)cpp"}}};
        };
        auto strings = [](const fs::path& f) {  // `strings -a`: the printable runs of four or more
            std::vector<std::string> out;
            std::string run;
            for (char ch : s::read_file(f)) {
                if (ch >= 0x20 && ch < 0x7f) { run += ch; continue; }
                if (run.size() >= 4) out.push_back(run);
                run.clear();
            }
            if (run.size() >= 4) out.push_back(run);
            return out;
        };
        auto newest_app = [](const s::project& p) {
            std::optional<fs::path> newest;
            for (const auto& f : s::glob(p.at("target"), "*/*/bin/app"))
                if (!newest || fs::last_write_time(f) > fs::last_write_time(*newest)) newest = f;
            expect(fatal(newest.has_value())) << "no target/*/*/bin/app";
            return *newest;
        };
        auto pa = mk("a", "");
        auto r = pa.mcpp({"run", "--release"}, o);
        expect(fatal(e2e::succeeded(r))) << "R2b: release ran the assert\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "file app@1.2.0/src/main.cpp app@1.2.0/src/main.cpp"))) << "R2: release did not trim __FILE__ and source_location\n" << s::text(r);
        const auto a = newest_app(pa);
        expect(fatal(std::ranges::none_of(strings(a), [&](const std::string& t) { return t.find(work.string()) != std::string::npos; }))) << "R2: the release artifact still carries the project's path";
        auto pb = mk("b", "");
        expect(fatal(e2e::succeeded(pb.mcpp({"build", "--release"}, o)))) << "R2: the second checkout failed";
        auto with_name = [&](const fs::path& f) { std::vector<std::string> v; for (auto& t : strings(f)) if (t.find("app@1.2.0") != std::string::npos) v.push_back(t); return v; };
        expect(fatal(with_name(a) == with_name(newest_app(pb)))) << "R2: two checkouts embed different paths";
        r = pa.mcpp({"run"}, o);
        expect(fatal(r.status != 0)) << "R2b: dev did not run the assert\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "file " + (work / "a/src/main.cpp").string()))) << "R2: dev trimmed a path\n" << s::text(r);
        auto pc = mk("c", "[profile.release]\nndebug     = false\ntrim_paths = false");
        r = pc.mcpp({"run", "--release"}, o);
        expect(fatal(r.status != 0)) << "R2b: ndebug = false did not keep the assert\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "file " + (work / "c/src/main.cpp").string()))) << "R2: trim_paths = false still trimmed\n" << s::text(r);
        auto pd = mk("d", "[profile.release]\ndebug = true");
        expect(fatal(e2e::succeeded(pd.mcpp({"build", "--release"}, o)))) << "release with debug failed";
        const auto g = s::glob(pd.at("target"), "*/*/source-map.gdbinit");
        expect(fatal(!g.empty())) << "no source-map.gdbinit";
        expect(e2e::contains(s::read_file(g.front()), "set substitute-path app@1.2.0 " + (work / "d").string())) << "source-map.gdbinit does not map the root package back";
    };
    "1003 one program one variant"_test[s::gcc] = [] {
        // Two programs of a workspace may reach two patched variants of one package, each with a
        // source tree of its own; one program reaching both is refused naming the package and
        // both variants.
        const s::options o{.where = s::home::inherited};
        auto mark = [](int value) {
            return std::format("--- a/zlib-1.3.2/zlib.h\n+++ b/zlib-1.3.2/zlib.h\n@@ -31,4 +31,6 @@\n #ifndef ZLIB_H\n #define ZLIB_H\n \n+#define MCPP_MARK {}\n+\n #ifdef ZLIB_BUILD\n", value);
        };
        s::project ws{s::files{{"patches/p1.patch", mark(1)}, {"patches/p2.patch", mark(2)}, {"mcpp.toml", "[workspace]\nmembers = [\"a\", \"one\", \"two\"]\n"},
                               {"a/mcpp.toml", R"cpp([package]
name    = "a"
version = "0.1.0"

[dependencies]
"compat.zlib" = { version = "1.3.2", patches = ["../patches/p1.patch"] }
)cpp"}, {"a/src/a.cppm", R"cpp(module;
#include <zlib.h>
export module a;
export int a_mark() { return MCPP_MARK; }
)cpp"},
                               {"one/mcpp.toml", "[package]\nname = \"one\"\nversion = \"0.1.0\"\n\n[dependencies]\na = { path = \"../a\" }\n"},
                               {"one/src/main.cpp", "import std;\nimport a;\nint main() { std::println(\"one {}\", a_mark()); }\n"},
                               {"two/mcpp.toml", R"cpp([package]
name    = "two"
version = "0.1.0"

[dependencies]
"compat.zlib" = { version = "1.3.2", patches = ["../patches/p2.patch"] }
)cpp"},
                               {"two/src/main.cpp", "#include <zlib.h>\n#include <cstdio>\nint main() { std::printf(\"two %d %s\\n\", MCPP_MARK, zlibVersion()); }\n"}}};
        auto r = ws.mcpp({"build", "--workspace"}, o);
        expect(fatal(e2e::succeeded(r))) << "two programs with two variants did not build\n" << s::text(r);
        r = ws.mcpp({"run", "-p", "one"}, o);
        expect(fatal(e2e::succeeded(r))) << "one did not run\n" << s::text(r);
        expect(fatal(s::has_line(s::text(r), "^one 1"))) << "one did not see its patch\n" << s::text(r);
        r = ws.mcpp({"run", "-p", "two"}, o);
        expect(fatal(e2e::succeeded(r))) << "two did not run\n" << s::text(r);
        expect(fatal(s::has_line(s::text(r), "^two 2 1.3.2"))) << "two did not see its patch\n" << s::text(r);
        std::size_t trees = 0;
        for (const auto& e : entries(s::case_home(s::home::inherited) / "cache/src/compat.zlib"))
            trees += e.filename().string().starts_with("1.3.2+p");
        expect(fatal(trees == 2u)) << "the two variants do not have a tree each (" << trees << ")";
        ws.write("mcpp.toml", "[workspace]\nmembers = [\"a\", \"one\", \"two\", \"three\"]\n");
        ws.write("three/mcpp.toml", R"cpp([package]
name    = "three"
version = "0.1.0"

[dependencies]
a = { path = "../a" }
"compat.zlib" = { version = "1.3.2", patches = ["../patches/p2.patch"] }
)cpp");
        ws.write("three/src/main.cpp", "import a;\nint main() { return a_mark() == 0; }\n");
        r = ws.mcpp({"build", "-p", "three"}, o);
        expect(fatal(failed(r))) << "a program with two variants was accepted\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "reaches two variants of 'compat.zlib'"))) << "the refusal does not name the package\n" << s::text(r);
        expect(s::count_lines(s::text(r), "compat.zlib+p") >= 2u) << "the refusal does not name both variants\n" << s::text(r);
    };
}};

}  // namespace
