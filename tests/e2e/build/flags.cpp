// Flags: a package's own build flags and link flags, per-target and per-glob flags, include
// families and directories, a link flag a build program computes, a consumer's include
// directories, each package's C standard, an artifact's own flags on shared sources, programs
// sharing an entry (formerly tests/e2e/50, 54, 70, 73, 109, 116, 141, 148, 620, 765, 777, 1047,
// 1054).
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

E2E flags{"flags", [] {
    "50 package owned build flags"_test[tag{"requires:gcc"}] = [] {
        // A dependency's own cflags and cxxflags compile its C and C++ sources; the consumer
        // declares neither, and the graph carries both.
        s::needs({"gcc"});
        const auto tmp = temp_dir();
        s::project{tmp / "depC", {{"src/dep_core.c", R"cpp(#ifndef DEP_C_BUILD
#error "missing dependency-owned C flag"
#endif
int dep_c_answer(void) {
    return DEP_C_BUILD;
}
)cpp"}, {"src/dep_extra.cpp", R"cpp(#ifndef DEP_CXX_BUILD
#error "missing dependency-owned C++ flag"
#endif
extern "C" int dep_cxx_answer(void) {
    return DEP_CXX_BUILD;
}
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "depC"
version = "0.1.0"

[build]
sources  = ["src/*.c", "src/*.cpp"]
cflags   = ["-DDEP_C_BUILD=7"]
cxxflags = ["-DDEP_CXX_BUILD=35"]

[targets.depC]
kind = "lib"
)cpp"}}};
        s::project app{tmp / "app", {{"src/main.cpp", R"cpp(extern "C" int dep_c_answer(void);
extern "C" int dep_cxx_answer(void);

int main() {
    return dep_c_answer() + dep_cxx_answer() == 42 ? 0 : 1;
}
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"

[build]
sources = ["src/*.cpp"]

[targets.app]
kind = "bin"
main = "src/main.cpp"

[dependencies.depC]
path = "../depC"
)cpp"}}};
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed\n" << s::text(r);
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "run failed\n" << s::text(r);
        const auto g = graph_text(app);
        expect(fatal(e2e::contains(g, "-DDEP_C_BUILD=7"))) << "dep cflags missing from build.ninja";
        expect(e2e::contains(g, "-DDEP_CXX_BUILD=35")) << "dep cxxflags missing from build.ninja";
    };

    "54 package owned ldflags"_test[tag{"requires:gcc"}] = [] {
        // A dependency's ldflags reach the consumer's link: the program links against the
        // archive they name and runs, and the graph carries both flags.
        s::needs({"gcc"});
        const auto tmp = temp_dir();
        s::project native{tmp / "native", {{"answer.c", R"cpp(int dep_link_answer(void) {
    return 42;
}
)cpp"}}};
        auto cc = e2e::run({.argv = {"gcc", "-c", "answer.c", "-o", "answer.o"}, .cwd = native.dir()});
        expect(fatal(e2e::succeeded(cc))) << "gcc -c failed\n" << s::text(cc);
        auto ar = e2e::run({.argv = {"ar", "rcs", "libanswer.a", "answer.o"}, .cwd = native.dir()});
        expect(fatal(e2e::succeeded(ar))) << "ar failed\n" << s::text(ar);
        const auto NATIVE_DIR = native.dir().string();
        s::project{tmp / "depLink", {{"mcpp.toml", std::format(R"cpp([package]
name    = "depLink"
version = "0.1.0"

[build]
ldflags = ["-L{0}", "-lanswer"]
)cpp", NATIVE_DIR)}}};
        s::project app{tmp / "app", {{"src/main.cpp", R"cpp(extern "C" int dep_link_answer(void);

int main() {
    return dep_link_answer() == 42 ? 0 : 1;
}
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"

[build]
sources = ["src/*.cpp"]

[targets.app]
kind = "bin"
main = "src/main.cpp"

[dependencies.depLink]
path = "../depLink"
)cpp"}}};
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed\n" << s::text(r);
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "run failed\n" << s::text(r);
        const auto g = graph_text(app);
        expect(fatal(e2e::contains(g, "-L" + NATIVE_DIR))) << "dep -L flag missing from build.ninja\n" << g;
        expect(e2e::contains(g, "-lanswer")) << "dep -l flag missing from build.ninja\n" << g;
    };

    "70 per target flags"_test[tag{"requires:gcc"}] = [] {
        // Two bin targets of one package carry their own defines and cxxflags on their own
        // entry only; a shared source compiled once sees neither (#131).
        s::needs({"gcc"});
        s::project p{s::files{{"src/common.cpp", R"cpp(#if defined(ROLE_SERVER) || defined(ROLE_CLIENT)
#error "per-target macro leaked into a shared compile unit"
#endif
extern "C" int shared_answer(void) { return 7; }
)cpp"}, {"src/server.cpp", R"cpp(#ifndef ROLE_SERVER
#error "server target did not receive its per-target define"
#endif
extern "C" int shared_answer(void);
int main() { return (shared_answer() == 7 && ROLE_SERVER == 1) ? 0 : 1; }
)cpp"}, {"src/client.cpp", R"cpp(#ifndef ROLE_CLIENT
#error "client target did not receive its per-target define"
#endif
#ifndef VIA_CXXFLAG
#error "client target did not receive its per-target cxxflag"
#endif
extern "C" int shared_answer(void);
int main() { return (shared_answer() == 7 && ROLE_CLIENT == 2) ? 0 : 1; }
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "proj"
version = "0.1.0"

[targets.server]
kind    = "bin"
main    = "src/server.cpp"
defines = ["ROLE_SERVER=1"]

[targets.client]
kind     = "bin"
main     = "src/client.cpp"
defines  = ["ROLE_CLIENT=2"]
cxxflags = ["-DVIA_CXXFLAG"]
)cpp"}}};
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed\n" << s::text(r);
        const auto g = graph_text(p);
        expect(fatal(e2e::contains(g, "-DROLE_SERVER=1"))) << "server define missing from ninja";
        expect(fatal(e2e::contains(g, "-DROLE_CLIENT=2"))) << "client define missing from ninja";
        expect(fatal(e2e::contains(g, "-DVIA_CXXFLAG"))) << "client cxxflag missing from ninja";
        expect(fatal(e2e::succeeded(p.mcpp({"run", "server"})))) << "server run failed (wrong/missing macro)";
        expect(e2e::succeeded(p.mcpp({"run", "client"}))) << "client run failed (wrong/missing macro)";
    };

    "73 issue131 per target cxxflag"_test[tag{"requires:gcc"}] = [] {
        // A single bin target's codegen cxxflag (-fno-exceptions) reaches only its own entry,
        // not the shared code (#131 scenario 1).
        s::needs({"gcc"});
        s::project p{s::files{{"src/shared.cpp", R"cpp(#ifndef __EXCEPTIONS
#error "per-target cxxflag leaked into a shared compile unit"
#endif
extern "C" int shared_val(void) { return 42; }
)cpp"}, {"tests/test_contracts.cpp", R"cpp(#ifdef __EXCEPTIONS
#error "per-target cxxflag did not reach this target's own entry"
#endif
extern "C" int shared_val(void);
int main() { return shared_val() == 42 ? 0 : 1; }
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "proj"
version = "0.1.0"

[targets.test_contracts]
kind     = "bin"
main     = "tests/test_contracts.cpp"
cxxflags = ["-fno-exceptions"]
)cpp"}}};
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed\n" << s::text(r);
        expect(fatal(e2e::contains(graph_text(p), "-fno-exceptions"))) << "per-target cxxflag missing from build.ninja";
        expect(e2e::succeeded(p.mcpp({"run", "test_contracts"}))) << "run failed";
    };

    "109 per glob flags"_test[tag{"requires:gcc"}] = [] {
        // [build] flags = [{ glob, ... }]: matched units get the flags, unmatched do not, a later
        // entry wins, and a glob matching nothing warns.
        s::needs({"gcc"});
        auto p = s::project::scaffold("globf");
        p.write("src/tagged.cpp", R"cpp(#ifndef TAG
#error "per-glob define did not reach tagged.cpp"
#endif
extern "C" int tag_value() { return TAG; }
)cpp");
        p.write("src/plain.cpp", R"cpp(#ifdef TAG
#error "per-glob define leaked into plain.cpp"
#endif
extern "C" int plain_ok() { return 1; }
)cpp");
        p.write("src/main.cpp", R"cpp(import std;
extern "C" int tag_value();
extern "C" int plain_ok();
int main() {
    std::println("tag = {}", tag_value() + plain_ok());
    return 0;
}
)cpp");
        p.write("mcpp.toml", R"cpp([package]
name    = "globf"
version = "0.1.0"

[build]
flags = [
  { glob = "src/tag*.cpp", cxxflags = ["-DTAG=1"] },
  { glob = "src/tagged.cpp", cxxflags = ["-DTAG=2"] },
  { glob = "src/never/**", cxxflags = ["-DNOPE"] },
]
)cpp");
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "matched no source file"))) << "missing zero-hit glob warning\n" << s::text(r);
        r = p.mcpp({"run"});
        expect(s::last_line(r.out) == "tag = 3") << "unexpected output: " << s::last_line(r.out) << " (want later-entry override → 2+1)";
    };
    "116 include family and define spaces"_test[tag{"requires:gcc"}] = [] {
        // An -iquote flag in cflags finds a quoted header, and a per-glob define whose value
        // holds a space (T=long long) reaches its unit whole.
        s::needs({"gcc"});
        auto p = s::project::scaffold("incfam");
        p.write("hdr/magic.h", R"cpp(#define MAGIC 42
)cpp");
        p.write("src/magic_user.c", R"cpp(#include "magic.h"
int get_magic(void) { return MAGIC; }
)cpp");
        p.write("src/main.cpp", R"cpp(import std;
extern "C" int get_magic();
extern long long get_big();

int main() {
    std::println("magic={} big={}", get_magic(), get_big());
    return 0;
}
)cpp");
        p.write("src/typed_user.cpp", R"cpp(typedef T MyLong;
long long get_big() {
    MyLong big = 123456789012LL;
    return static_cast<long long>(big);
}
)cpp");
        p.write("mcpp.toml", R"cpp([package]
name    = "incfam"
version = "0.1.0"

[build]
cflags = ["-iquotehdr"]
flags = [
  { glob = "src/typed_user.cpp", defines = ["T=long long"] },
]
)cpp");
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed\n" << s::text(r);
        r = p.mcpp({"run"});
        expect(s::last_line(r.out) == "magic=42 big=123456789012") << "unexpected output: " << s::last_line(r.out);
    };

    "141 include dirs after"_test[tag{"requires:elf"}][tag{"requires:gcc"}] = [] {
        // A dependency's include_dirs_after reaches its consumer as -idirafter, behind the
        // system headers (its poisoned stdlib.h is not found); as include_dirs it would be.
        s::needs({"elf", "gcc"});
        auto lib = s::project::scaffold("poisonlib");
        lib.remove("src/main.cpp");
        lib.write("src/lib.cppm", R"cpp(export module poisonlib.lib;
export int poisonlib_anchor() { return 1; }
)cpp");
        lib.write("inc/stdlib.h", R"cpp(#error poisoned
)cpp");
        lib.write("inc/mylib.h", R"cpp(#pragma once
inline int mylib_answer() { return 42; }
)cpp");
        lib.write("mcpp.toml", R"cpp([package]
name    = "poisonlib"
version = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
[modules]
sources = ["src/**/*.cppm"]
[build]
include_dirs_after = ["inc"]
[targets.poisonlib]
kind = "lib"
)cpp");
        auto p = s::project::scaffold("consumer");
        p.write("src/main.cpp", R"cpp(#include <stdlib.h>
#include <mylib.h>
import std;
int main() {
    std::println("answer = {}", mylib_answer());
    return 0;
}
)cpp");
        p.write("mcpp.toml", R"cpp([package]
name    = "consumer"
version = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
[modules]
sources = ["src/**/*.cppm", "src/**/*.cpp"]
[targets.consumer]
kind = "bin"
main = "src/main.cpp"
[dependencies.poisonlib]
path = "../poisonlib"
)cpp");
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "include_dirs_after build failed\n" << s::text(r);
        r = p.mcpp({"run"});
        expect(fatal(e2e::contains(s::text(r), "answer = 42"))) << "dep header not found via -idirafter: " << s::text(r);
        expect(fatal(s::has_line(graph_text(p), "-idirafter[^ ]*poisonlib[^ ]*inc"))) << "ninja missing -idirafter for dep include root";
        auto m = lib.read("mcpp.toml");
        m.replace(m.find("include_dirs_after = [\"inc\"]"), 28, "include_dirs = [\"inc\"]");
        lib.write("mcpp.toml", m);
        p.remove("target");
        r = p.mcpp({"build"});
        expect(fatal(failed(r))) << "expected poisoned build to fail";
        expect(e2e::contains(s::text(r), "poisoned")) << "wrong failure (not the poison)\n" << s::text(r);
    };

    "148 wide include list"_test = [] {
        // Sixty-four include directories with long names all reach the compile.
        auto p = s::project::scaffold("wideinc");
        constexpr int n = 64;
        std::string incs, wide;
        for (int i = 1; i <= n; ++i) {
            const auto d = std::format("vendor/dir_with_a_deliberately_long_name_{}/include", i);
            p.write(std::format("{}/wide_{}.h", d, i), std::format("#define WIDE_INC_{} {}\n", i, i));
            incs += std::format("\"{}\", ", d);
            wide += std::format("#include <wide_{}.h>\n", i);
        }
        incs.resize(incs.size() - 2);
        wide += "extern \"C\" int wide_sum() { return 0\n";
        for (int i = 1; i <= n; ++i) wide += std::format("    + WIDE_INC_{}\n", i);
        wide += "    ; }\n";
        p.write("src/wide.cpp", wide);
        p.write("src/main.cpp", R"cpp(import std;
extern "C" int wide_sum();
int main() {
    std::println("wide_sum = {}", wide_sum());
    return wide_sum() == (64 * 65) / 2 ? 0 : 1;
}
)cpp");
        auto m = p.read("mcpp.toml");
        const auto at = m.find("\n[build]\n");
        if (at != std::string::npos) m.insert(at + 9, std::format("include_dirs = [{}]\n", incs));
        else if (m.starts_with("[build]\n")) m.insert(8, std::format("include_dirs = [{}]\n", incs));
        if (!e2e::contains(m, "include_dirs")) m += std::format("\n[build]\ninclude_dirs = [{}]\n", incs);
        p.write("mcpp.toml", m);
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "wide include build failed\n" << s::text(r);
        r = p.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "wide include run failed\n" << s::text(r);
        expect(e2e::contains(s::text(r), "wide_sum = 2080")) << "expected wide_sum = 2080 (every -I must be present)\n" << s::text(r);
    };
    "620 link flag reaches the link line"_test[tag{"requires:gcc"}] = [] {
        // A link flag a build program computes (mcpp::link_flag) reaches the linker.
        s::needs({"gcc"});
        s::project p{s::files{{"mcpp.toml", R"cpp([package]
name    = "linkflag"
version = "0.1.0"

[targets.linkflag]
kind = "bin"
main = "src/main.cpp"
)cpp"}, {"build.mcpp", R"cpp(#include <cstdio>
#include <string>
import mcpp;
int main() {
    int computed = 40 + 2;
    std::string flag = "-Wl,--defsym=mcpp_e2e_620=" + std::to_string(computed);
    mcpp::link_flag(flag.c_str());
    return 0;
}
)cpp"}, {"src/main.cpp", R"cpp(#include <cstdio>
extern "C" char mcpp_e2e_620;
int main() {
    // The linker put the value in the SYMBOL'S ADDRESS, which is how --defsym
    // works; reading the object would read memory that was never written.
    std::printf("defsym=%lld\n",
                (long long)(unsigned long long)(void*)&mcpp_e2e_620);
    return 0;
}
)cpp"}}};
        auto r = p.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "the project did not build or run\n" << s::text(r);
        expect(e2e::contains(s::text(r), "defsym=42")) << "the computed link flag did not reach the linker\n" << s::text(r);
    };
    "765 a consumer include directory stays in the consumer"_test[tag{"requires:python3"}] = [] {
        // The root's include directories reach its own units only: a private one does not
        // reach a dependency (a), the compile database gives the dependency's units none of the
        // root's directories and the root each of its own once plus the dependency's public one
        // (b), the dependency's words are the same under two roots (c), and a dependency that
        // needed its consumer's header fails naming the header and the consumer's directory (d).
        s::needs({"python3"});
        const auto tmp = temp_dir();
        const s::options o{.where = s::home::inherited, .env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};
        s::project{tmp / "dep", {{"include/dep/api.h", R"cpp(#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int dep_c_value(void);
#ifdef __cplusplus
}
#endif
int dep_cxx_value();
)cpp"}, {"src/dep.cpp", R"cpp(#include <limits.h>
#include <dep/api.h>
int dep_cxx_value() { return INT_MAX > 0 ? 3 : 0; }
)cpp"}, {"src/dep_c.c", R"cpp(#include <limits.h>
#include <dep/api.h>
int dep_c_value(void) { return CHAR_BIT == 8 ? 4 : 0; }
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "dep"
version = "0.1.0"

[build]
include_dirs = ["include"]
sources      = ["src/dep.cpp", "src/dep_c.c"]

[targets.dep]
kind = "lib"
)cpp"}}};
        s::project app{tmp / "app", {{"appinc/appcfg.h", "#define APP_CFG 7\n"}, {"appprivinc/apppriv.h", "#define APP_PRIV 1\n"},
                                     {"appprivinc/limits.h", "#error ROOT PRIVATE HEADER REACHED A DEPENDENCY\n"}, {"src/main.cpp", R"cpp(#include <dep/api.h>
#include "appcfg.h"
#include "apppriv.h"
int main() { return dep_cxx_value() + dep_c_value() == APP_CFG + APP_PRIV - 1 ? 0 : 1; }
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"

[dependencies]
dep = { path = "../dep" }

[build]
include_dirs         = ["appinc", "appprivinc"]
private_include_dirs = ["appprivinc"]

[targets.app]
kind = "bin"
main = "src/main.cpp"
)cpp"}}};
        s::project app2{tmp / "app2", {{"other/other.h", "#define OTHER 1\n"}, {"src/main.cpp", R"cpp(#include <dep/api.h>
#include "other.h"
int main() { return dep_cxx_value() + dep_c_value() + OTHER == 8 ? 0 : 1; }
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "app2"
version = "0.1.0"

[dependencies]
dep = { path = "../dep" }

[build]
include_dirs = ["other"]

[targets.app2]
kind = "bin"
main = "src/main.cpp"
)cpp"}}};
        auto r = app.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "(a): the root's private include directory reached a dependency unit\n" << s::text(r);
        r = app2.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "app2 build\n" << s::text(r);
        auto norm = [](std::string x) { std::ranges::replace(x, '\\', '/'); return x; };
        auto args = [](nlohmann::json& e) {
            std::vector<std::string> out;
            if (e.contains("arguments") && !e["arguments"].empty()) for (auto& a : e["arguments"]) out.push_back(a.get<std::string>());
            else for (auto part : std::views::split(e["command"].get<std::string>(), ' ')) if (!std::ranges::empty(part)) out.emplace_back(part.begin(), part.end());
            return out;
        };
        auto count = [&](const std::vector<std::string>& a, std::string_view needle) {
            return std::ranges::count_if(a, [&](const std::string& x) { return norm(x).find(needle) != std::string::npos; });
        };
        auto basename = [&](nlohmann::json& e) { const auto f = norm(e["file"].get<std::string>()); return f.substr(f.rfind('/') + 1); };
        auto db = s::json_file(app.at("compile_commands.json"));
        std::vector<nlohmann::json*> dep_units, main_units;
        for (auto& e : db) {
            if (basename(e) == "dep.cpp" || basename(e) == "dep_c.c") dep_units.push_back(&e);
            if (norm(e["file"].get<std::string>()).ends_with("app/src/main.cpp")) main_units.push_back(&e);
        }
        expect(dep_units.size() == 2u) << "(b) expected 2 dependency entries, found " << dep_units.size();
        expect(main_units.size() == 1u) << "(b) expected 1 root entry, found " << main_units.size();
        for (auto* e : dep_units)
            for (std::string_view d : {"app/appinc", "app/appprivinc"})
                expect(count(args(*e), d) == 0) << "(b) " << norm((*e)["file"].get<std::string>()) << " carries the root directory " << d;
        for (auto* e : main_units) {
            const auto a = args(*e);
            for (std::string_view d : {"app/appinc", "app/appprivinc"})
                expect(count(a, d) == 1) << "(b) root entry carries " << d << " " << count(a, d) << " times, expected once";
            expect(count(a, "dep/include") >= 1) << "(b) the dependency's public include directory does not reach the root";
        }
        auto words = [&](const s::project& p, std::string_view root) {
            std::map<std::string, std::vector<std::string>> out;
            auto entries = s::json_file(p.at("compile_commands.json"));
            const auto own = std::format("/{}/target/", root);
            for (auto& e : entries) {
                const auto f = basename(e);
                if (f != "dep.cpp" && f != "dep_c.c") continue;
                std::vector<std::string> w;
                for (const auto& x : args(e))
                    if ((x.starts_with("-I") || x.starts_with("-D") || x.starts_with("-f") || x.starts_with("-idirafter") || x.starts_with("/I") || x.starts_with("/D"))
                        && norm(x).find(own) == std::string::npos)
                        w.push_back(norm(x));
                std::ranges::sort(w);
                out[f] = w;
            }
            return out;
        };
        const auto w1 = words(app, "app"), w2 = words(app2, "app2");
        expect(fatal(!w1.empty() && w1 == w2)) << "(c) the dependency's words differ between two roots";

        s::project{tmp / "leaner", {{"src/leaner.cpp", R"cpp(#include <needs_consumer.h>
int leaner_value() { return FROM_CONSUMER; }
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "leaner"
version = "0.1.0"

[build]
sources = ["src/leaner.cpp"]

[targets.leaner]
kind = "lib"
)cpp"}}};
        s::project app3{tmp / "app3", {{"appinc/needs_consumer.h", "#define FROM_CONSUMER 1\n"}, {"src/main.cpp", R"cpp(int leaner_value();
int main() { return leaner_value() == 1 ? 0 : 1; }
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "app3"
version = "0.1.0"

[dependencies]
leaner = { path = "../leaner" }

[build]
include_dirs = ["appinc"]

[targets.app3]
kind = "bin"
main = "src/main.cpp"
)cpp"}}};
        for (std::string_view pass : {"full", "fast"}) {
            r = app3.mcpp({"build"}, o);
            expect(fatal(failed(r))) << "(d, " << pass << "): a dependency compiled against its consumer's header\n" << s::text(r);
            expect(fatal(e2e::contains(s::text(r), "needs_consumer.h"))) << "(d, " << pass << "): failed for an unexpected reason\n" << s::text(r);
            expect(fatal(e2e::contains(s::text(r), "no longer") && e2e::contains(s::text(r), "appinc"))) << "(d, " << pass << "): the advice naming the consumer's directory is missing\n" << s::text(r);
        }
    };
    "777 c standard applies to the package that declares it"_test[tag{"requires:gcc"}] = [] {
        // Each package's C units compile at its own c_standard (the root c99, a dependency
        // gnu11, one declaring none the default c11), in the compile database too.
        s::needs({"gcc"});
        const auto tmp = temp_dir();
        const s::options cdb{.env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};
        s::project{tmp / "cdep", {{"mcpp.toml", R"cpp([package]
name    = "cdep"
version = "0.1.0"

[build]
c_standard = "gnu11"

[targets.cdep]
kind = "lib"
)cpp"}, {"src/cdep.c", R"cpp(#if __STDC_VERSION__ != 201112L
#error "cdep declares gnu11 and was compiled at another standard"
#endif
#ifdef __STRICT_ANSI__
#error "cdep declares gnu11 and was compiled at a strict standard"
#endif
int cdep_value(void) { return 11; }
)cpp"}}};
        s::project{tmp / "plain", {{"mcpp.toml", R"cpp([package]
name    = "plain"
version = "0.1.0"

[targets.plain]
kind = "lib"
)cpp"}, {"src/plain.c", R"cpp(#if __STDC_VERSION__ != 201112L || !defined(__STRICT_ANSI__)
#error "plain declares no C standard and was not compiled at the default, c11"
#endif
int plain_value(void) { return 1; }
)cpp"}}};
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"

[build]
c_standard = "c99"

[dependencies]
cdep  = { path = "../cdep" }
plain = { path = "../plain" }
)cpp"}, {"src/root.c", R"cpp(#if __STDC_VERSION__ != 199901L
#error "app declares c99 and was compiled at another standard"
#endif
int root_value(void) { return 99; }
)cpp"}, {"src/main.cpp", R"cpp(extern "C" int cdep_value();
extern "C" int plain_value();
extern "C" int root_value();
int main() { return cdep_value() == 11 && plain_value() == 1 && root_value() == 99 ? 0 : 1; }
)cpp"}}};
        auto r = app.mcpp({"build"}, cdb);
        expect(fatal(e2e::succeeded(r))) << "each package's C units must compile at its own standard\n" << s::text(r);
        expect(fatal(e2e::succeeded(app.mcpp({"run"}, cdb)))) << "the program did not run";
        const auto db = s::find_file(app.dir(), "compile_commands.json");
        expect(fatal(db.has_value())) << "no compile_commands.json";
        expect(fatal(e2e::contains(s::read_file(*db), "-std=gnu11"))) << "the database does not carry cdep's standard";
        expect(e2e::contains(s::read_file(*db), "-std=c99")) << "the database does not carry app's standard";
    };
    "1047 an artifact compiles its shared sources with its own flags"_test[tag{"requires:elf"}][tag{"requires:gcc"}] = [] {
        // An artifact with build flags of its own compiles the package's shared sources as a
        // variant with them; one without links the package's object and has no variant.
        s::needs({"elf", "gcc"});
        s::project p{s::files{{"mcpp.toml", R"cpp([package]
name    = "shell"
version = "0.1.0"

[targets.cmd]
kind = "bin"
main = "src/cmd.cpp"

[targets.COMMAND]
kind = "bin"
main = "src/command.cpp"

[targets.COMMAND.build]
defines = ["ENABLE_AUTOEXEC=1"]
)cpp"}, {"src/base.cppm", R"cpp(export module shell.base;
export int base_value() { return 4; }
)cpp"}, {"src/shell.cpp", R"cpp(import shell.base;
int autoexec() {
#ifdef ENABLE_AUTOEXEC
    return base_value() + 1;
#else
    return base_value();
#endif
}
)cpp"},
                              {"src/cmd.cpp", "int autoexec(); int main() { return autoexec(); }\n"},
                              {"src/command.cpp", "int autoexec(); int main() { return autoexec(); }\n"}}};
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the build failed\n" << s::text(r);
        const auto cmd = path_ending(p.at("target"), "/bin/cmd");
        expect(fatal(cmd.has_value())) << "no bin/cmd";
        const auto a = s::run(*cmd).status, b = s::run(cmd->parent_path() / "COMMAND").status;
        expect(fatal(a == 4)) << "cmd returned " << a << ", not the shared source's plain 4";
        expect(fatal(b == 5)) << "COMMAND returned " << b << ", not 5 from its own define";
        bool variant = false, cmd_variant = false;
        for (const auto& f : s::find_files(p.at("target"), ".*")) {
            const auto g = f.generic_string();
            variant = variant || (g.find("/obj/_variant/COMMAND/") != std::string::npos && f.filename() == "shell.o");
            cmd_variant = cmd_variant || g.find("/obj/_variant/cmd") != std::string::npos;
        }
        for (const auto& e : fs::recursive_directory_iterator(p.at("target")))
            cmd_variant = cmd_variant || e.path().generic_string().find("/obj/_variant/cmd") != std::string::npos;
        expect(fatal(variant)) << "no variant object of COMMAND";
        expect(!cmd_variant) << "an artifact without flags of its own has a variant";
    };
    "1054 programs that share an entry keep their own flags"_test[tag{"requires:gcc"}] = [] {
        // Three programs with one entry keep their own flags: plain, a target define on the
        // entry only, a [targets.<n>.build] define on the entry and the shared sources; the
        // shared entry object carries no program's define.
        s::needs({"gcc"});
        s::project p{s::files{{"mcpp.toml", R"cpp([package]
name    = "twin"
version = "0.1.0"

[build]
sources = ["src/*.c", "!src/main.c"]

[targets.plain]
kind = "bin"
main = "src/main.c"

[targets.flagged]
kind    = "bin"
main    = "src/main.c"
defines = ["FLAVOUR=2"]

[targets.variant]
kind = "bin"
main = "src/main.c"

[targets.variant.build]
defines = ["FLAVOUR=3"]
)cpp"}, {"src/main.c", R"cpp(#include <stdio.h>
#ifndef FLAVOUR
#define FLAVOUR 1
#endif
int helper(void);
int main(void) { printf("%d %d\n", FLAVOUR, helper()); return 0; }
)cpp"}, {"src/helper.c", R"cpp(#ifndef FLAVOUR
#define FLAVOUR 1
#endif
int helper(void) { return FLAVOUR * 10; }
)cpp"}}};
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the build failed\n" << s::text(r);
        for (auto [name, want] : {std::pair{"plain", "1 10"}, std::pair{"flagged", "2 10"}, std::pair{"variant", "3 30"}}) {
            const auto exe = path_ending(p.at("target"), std::string("/bin/") + name);
            expect(fatal(exe.has_value())) << "no program " << name;
            expect(fatal(output_of(*exe) == want)) << name << " printed '" << output_of(*exe) << "', not '" << want << "'";
        }
        const auto ninjas = s::find_files(p.at("target"), R"(build\.ninja)");
        expect(fatal(!ninjas.empty())) << "no build.ninja";
        std::string stanza;
        bool on = false;
        for (const auto& l : lines_of(s::read_file(ninjas.front()))) {
            if (l.starts_with("build obj/twin/src/main.o ")) { on = true; stanza += l + "\n"; continue; }
            if (on && l.starts_with("build ")) break;
            if (on) stanza += l + "\n";
        }
        expect(fatal(!stanza.empty())) << "no shared entry object in build.ninja";
        expect(s::lacks(stanza, "FLAVOUR")) << "the shared entry object carries a program's define\n" << stanza;
    };
}};

}  // namespace
