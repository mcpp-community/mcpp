// Sources: assembly, brace globs, colliding basenames, a dependency's same-named main, spaced
// paths, explicitly empty sources, a link unit with none, paths with no UTF-8 spelling, the
// library interface, deprecated readings (formerly tests/e2e/104, 105, 115, 117, 123, 179, 246,
// 316, 776, 907, 909).
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

E2E sources{"sources", [] {
    "104 asm sources gas"_test[tag{"requires:gcc"}] = [] {
        // GAS sources (.S) are globbed by default, routed to asm_object (no -std), preprocessed,
        // linked, rebuilt when touched, and covered on the test path too (x86_64 only).
        s::needs({"gcc"});
        if (!s::has("x86_64-host")) skip("x86_64-only asm");
        auto p = s::project::scaffold("gasmix");
        p.write("src/add3.S", R"cpp(#define ASM_BIAS 7
    .text
    .globl asm_add3
#if defined(__ELF__)
    .type asm_add3, @function
#endif
asm_add3:
    leaq (%rdi,%rsi), %rax
    addq %rdx, %rax
    addq $ASM_BIAS, %rax
    ret
#if defined(__ELF__)
    .section .note.GNU-stack,"",@progbits
#endif
)cpp");
        p.write("src/main.cpp", R"cpp(import std;
extern "C" long asm_add3(long a, long b, long c);
int main() {
    std::println("asm add3 = {}", asm_add3(1, 2, 3));
    return asm_add3(1, 2, 3) == 13 ? 0 : 1;
}
)cpp");
        p.write("mcpp.toml", R"cpp([package]
name    = "gasmix"
version = "0.1.0"
)cpp");
        p.remove("tests/test_smoke.cpp");
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed\n" << s::text(r);
        const auto g = graph_text(p);
        expect(fatal(s::has_line(g, "^rule asm_object"))) << "missing asm_object rule\n" << g;
        expect(fatal(s::has_line(g, R"(build obj/add3\.S\.o : asm_object .*add3\.S)"))) << "add3.S not routed to asm_object\n" << g;
        for (const auto& l : s::lines_with(g, "asmflags  ="))
            if (l.starts_with("asmflags  =")) expect(fatal(s::lacks(l, "-std="))) << "asmflags carries a -std flag";
        r = p.mcpp({"run"});
        expect(fatal(s::last_line(r.out) == "asm add3 = 13")) << "unexpected output: " << s::last_line(r.out);
        touch(p.at("src/add3.S"));
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "rebuild failed\n" << s::text(r);
        r = p.mcpp({"run"});
        expect(fatal(s::last_line(r.out) == "asm add3 = 13")) << "post-rebuild output: " << s::last_line(r.out);
        p.write("tests/test_asm.cpp", R"cpp(extern "C" long asm_add3(long a, long b, long c);
int main() { return asm_add3(2, 3, 4) == 16 ? 0 : 1; }
)cpp");
        r = p.mcpp({"test"});
        expect(e2e::succeeded(r)) << "mcpp test failed\n" << s::text(r);
    };
    "105 asm sources nasm"_test[tag{"requires:gcc"}][tag{"requires:nasm"}] = [] {
        // NASM sources (.asm) are globbed by default, routed to nasm_object with -f elf64, their
        // %include tracked through the depfile, kept out of compile_commands.json, and linked.
        // A broken home's bootstrap error is not swallowed into "no usable nasm" (#232, where a
        // host C++ compiler exists); with a too-old nasm on PATH, nasm is provisioned into the
        // home or the real provisioning failure is named.
        s::needs({"gcc", "nasm"});
        if (!s::has("x86_64-host")) skip("x86-only NASM");
        const auto tmp = temp_dir();
        const s::options cdb{.env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};
        auto p = s::project::scaffold("nasmix");
        p.write("inc/consts.inc", R"cpp(%define MUL_BIAS 5
)cpp");
        p.write("src/mul2.asm", R"cpp(%include "consts.inc"
section .text
global asm_mul2
asm_mul2:
    lea rax, [rdi*2 + MUL_BIAS]
    ret
)cpp");
        p.write("src/main.cpp", R"cpp(import std;
extern "C" long asm_mul2(long a);
int main() {
    std::println("asm mul2 = {}", asm_mul2(10));
    return asm_mul2(10) == 25 ? 0 : 1;
}
)cpp");
        p.write("mcpp.toml", R"cpp([package]
name    = "nasmix"
version = "0.1.0"

[build]
include_dirs = ["inc"]
)cpp");
        auto r = p.mcpp({"build"}, cdb);
        expect(fatal(e2e::succeeded(r))) << "build failed\n" << s::text(r);
        const auto g = graph_text(p);
        expect(fatal(s::has_line(g, "^rule nasm_object"))) << "missing nasm_object rule\n" << g;
        expect(fatal(s::has_line(g, "^nasmfmt   = elf64"))) << "nasmfmt not derived as elf64\n" << g;
        expect(fatal(s::has_line(g, R"(build obj/mul2\.asm\.o : nasm_object .*mul2\.asm)"))) << "mul2.asm not routed to nasm_object\n" << g;
        if (p.exists("compile_commands.json"))
            expect(fatal(s::lacks(p.read("compile_commands.json"), "mul2.asm"))) << "mul2.asm leaked into compile_commands.json";
        r = p.mcpp({"run"}, cdb);
        expect(fatal(s::last_line(r.out) == "asm mul2 = 25")) << "unexpected output: " << s::last_line(r.out);
        p.write("inc/consts.inc", R"cpp(%define MUL_BIAS 6
)cpp");
        r = p.mcpp({"build"}, cdb);
        expect(fatal(e2e::succeeded(r))) << "rebuild failed\n" << s::text(r);
        r = p.mcpp({"run"}, cdb);
        expect(fatal(s::last_line(r.out) == "asm mul2 = 26")) << "stale after %include change: " << s::last_line(r.out) << " (depfile tracking broken)";

        std::string hostcxx;
        for (std::string_view c : {"/usr/bin/clang++", "/usr/bin/g++"})
            if (hostcxx.empty() && executable(c)) hostcxx = c;
        for (std::string_view c : {"clang++", "g++", "c++"})
            if (hostcxx.empty()) if (auto found = s::on_path(c)) hostcxx = found->string();
        if (!hostcxx.empty()) {
            s::project cold{tmp / "cold/nasmix-cold", {{"inc/consts.inc", R"cpp(%define MUL_BIAS 5
)cpp"}, {"src/mul2.asm", R"cpp(%include "consts.inc"
section .text
global asm_mul2
asm_mul2:
    lea rax, [rdi*2 + MUL_BIAS]
    ret
)cpp"}, {"src/main.cpp", R"cpp(#include <cstdio>
extern "C" long asm_mul2(long a);
int main() {
    printf("asm mul2 = %ld\n", asm_mul2(10));
    return asm_mul2(10) == 25 ? 0 : 1;
}
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "nasmix-cold"
version = "0.1.0"

[toolchain]
linux = "system"

[build]
include_dirs = ["inc"]
)cpp"}}};
            s::project{tmp / "cold", {{"brokenhome", ""}}};
            auto c = e2e::run({.argv = {s::mcpp_path(), "build"}, .cwd = cold.dir(),
                               .env = {{"MCPP_HOME", (tmp / "cold/brokenhome").string()}, {"CXX", hostcxx}, {"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}});
            expect(fatal(failed(c))) << "cold-env (#232) build unexpectedly succeeded:\n" << s::text(c);
            expect(fatal(s::contains_nocase(s::text(c), "cannot create"))) << "cold-env build failed, but not with the real bootstrap error:\n" << s::text(c);
            expect(fatal(s::lacks(s::text(c), "no usable nasm") && s::lacks(s::text(c), "No usable nasm")))
                << "REGRESSION (#232): the guard swallowed the config-bootstrap error and fell back to the generic 'no usable nasm' message:\n" << s::text(c);
        }

        const auto home = s::case_home(s::home::inherited);
        fs::remove_all(home / "registry/data/xpkgs/xim-x-nasm");
        s::write_executable(tmp / "decoy/nasm", R"cpp(#!/usr/bin/env bash
# Deliberately too old (< 2.16): the version gate must reject it so the
# PATH probe falls through to sandbox lookup / provisioning, never a
# false "found".
[[ "$1" == "-v" ]] && echo "NASM version 2.10.00 compiled on Jan  1 2020"
exit 0
)cpp");
        const auto path = (tmp / "decoy").string() + ":" + s::env("PATH").value_or("");
        const s::options warm{.where = s::home::inherited, .env = {{"PATH", path}, {"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};
        s::project w{tmp / "warm/proj/nasmix-warm", {{"inc/consts.inc", R"cpp(%define MUL_BIAS 5
)cpp"}, {"src/mul2.asm", R"cpp(%include "consts.inc"
section .text
global asm_mul2
asm_mul2:
    lea rax, [rdi*2 + MUL_BIAS]
    ret
)cpp"}, {"src/main.cpp", R"cpp(import std;
extern "C" long asm_mul2(long a);
int main() {
    std::println("asm mul2 = {}", asm_mul2(10));
    return asm_mul2(10) == 25 ? 0 : 1;
}
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "nasmix-warm"
version = "0.1.0"

[build]
include_dirs = ["inc"]
)cpp"}}};
        r = w.mcpp({"build"}, warm);
        if (e2e::succeeded(r)) {
            expect(fatal(!s::find_files(home / "registry/data/xpkgs/xim-x-nasm", "nasm.*").empty()))
                << "build succeeded but nasm never landed in the sandbox (#232 gate not exercised):\n" << s::text(r);
            auto run = w.mcpp({"run"}, warm);
            expect(s::last_line(run.out) == "asm mul2 = 25") << "cold-provisioned build produced wrong output: " << s::last_line(run.out);
        } else {
            expect(fatal(!s::contains_nocase(s::text(r), "no usable nasm"))) << "REGRESSION (#232): fell back to the old generic 'no usable nasm' message instead of the real provisioning failure:\n" << s::text(r);
            expect(s::contains_nocase(s::text(r), "provisioning failed") || s::contains_nocase(s::text(r), "xlings install"))
                << "cold-env build failed without a real provisioning error message:\n" << s::text(r);
        }
    };
    "115 manifest syntax aot brace"_test[tag{"requires:gcc"}] = [] {
        // A brace glob in [build] sources and in an array-of-tables [[build.flags]] entry
        // matches exactly its alternatives.
        s::needs({"gcc"});
        auto p = s::project::scaffold("aotbrace");
        p.write("src/p/aac/aac.cpp", R"cpp(#ifndef CODEC
#error "per-glob define (via [[build.flags]] AOT) did not reach aac.cpp"
#endif
extern "C" int aac_id() { return 1; }
)cpp");
        p.write("src/p/bsf/bsf.cpp", R"cpp(#ifndef CODEC
#error "per-glob define (via [[build.flags]] AOT) did not reach bsf.cpp"
#endif
extern "C" int bsf_id() { return 2; }
)cpp");
        p.write("src/p/opus/opus.cpp", R"cpp(#ifdef CODEC
#error "brace glob over-matched: opus.cpp is outside {aac,bsf}"
#endif
extern "C" int opus_id() { return 3; }
)cpp");
        p.write("src/main.cpp", R"cpp(import std;
extern "C" int aac_id();
extern "C" int bsf_id();
extern "C" int opus_id();
int main() {
    std::println("ids = {}", aac_id() + bsf_id() + opus_id());
    return 0;
}
)cpp");
        p.write("mcpp.toml", R"cpp([package]
name    = "aotbrace"
version = "0.1.0"

[build]
sources = ["src/p/{aac,bsf}/**/*.cpp", "src/p/opus/**/*.cpp", "src/main.cpp"]

[[build.flags]]
glob    = "src/p/{aac,bsf}/**"
defines = ["CODEC"]
)cpp");
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed\n" << s::text(r);
        r = p.mcpp({"run"});
        expect(s::last_line(r.out) == "ids = 6") << "unexpected output: " << s::last_line(r.out) << " (want ids = 6)";
    };

    "117 object path collision"_test = [] {
        // Two sources of one basename in different directories compile to two objects.
        auto p = s::project::scaffold("objcollide");
        p.write("a/src/util.cpp", R"cpp(int fa() { return 1; }
)cpp");
        p.write("b/src/util.cpp", R"cpp(int fb() { return 2; }
)cpp");
        p.write("src/main.cpp", R"cpp(import std;
extern int fa();
extern int fb();
int main() {
    int sum = fa() + fb();
    std::println("sum={}", sum);
    return sum == 3 ? 0 : 1;
}
)cpp");
        p.write("mcpp.toml", R"cpp([package]
name    = "objcollide"
version = "0.1.0"
[modules]
sources = ["src/**/*.cppm", "src/**/*.cpp", "a/src/**/*.cpp", "b/src/**/*.cpp"]
[targets.objcollide]
kind = "bin"
main = "src/main.cpp"
)cpp");
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed (expected: colliding basenames must not collapse to the same object path)\n" << s::text(r);
        const auto g = graph_text(p);
        std::set<std::string> objects;
        const std::regex edge{R"(build ([^ ]*util[^ ]*\.o) : cxx_object)"};
        for (auto it = std::sregex_iterator(g.begin(), g.end(), edge); it != std::sregex_iterator(); ++it) objects.insert((*it)[1].str());
        expect(fatal(objects.size() == 2u)) << "expected 2 distinct util.cpp object outputs, got " << objects.size() << "\n" << g;
        r = p.mcpp({"run"});
        expect(s::last_line(r.out) == "sum=3") << "unexpected output: " << s::last_line(r.out);
    };

    "123 same named main across dep"_test[tag{"requires:elf"}][tag{"requires:gcc"}] = [] {
        // A dependency's src/main.cpp and the consumer's have objects of their own, and every
        // object the consumer's link names is produced by an edge.
        s::needs({"elf", "gcc"});
        const auto tmp = temp_dir();
        s::project{tmp / "mydep", {{"src/main.cpp", R"cpp(int dep_helper() { return 41; }
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "mydep"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[targets.mydep]
kind = "lib"
)cpp"}}};
        s::project p{tmp / "consumer", {{"src/main.cpp", R"cpp(import std;
extern int dep_helper();
int main() {
    std::println("val={}", dep_helper());
    return dep_helper() == 41 ? 0 : 1;
}
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "consumer"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[dependencies]
mydep = { path = "../mydep" }
[targets.consumer]
kind = "bin"
main = "src/main.cpp"
)cpp"}}};
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed (expected: same-named main across dep must link the disambiguated object)\n" << s::text(r);
        const auto g = graph_text(p);
        std::string link_line;
        for (const auto& l : lines_of(g))
            if (std::regex_search(l, std::regex{R"(^build bin/consumer *:)"})) link_line += l;
        expect(fatal(!link_line.empty())) << "no link edge for bin/consumer\n" << g;
        const auto inputs = std::regex_replace(link_line, std::regex{R"(^build bin/consumer *: *cxx_link )"}, "");
        for (auto part : std::views::split(inputs, ' ')) {
            const std::string obj(part.begin(), part.end());
            if (!obj.ends_with(".o")) continue;
            expect(fatal(std::ranges::any_of(lines_of(g), [&](const std::string& l) { return l.starts_with("build " + obj + " :") || l.starts_with("build " + obj + ":"); })))
                << "link input '" << obj << "' is not produced by any edge\n" << link_line;
        }
        expect(fatal(s::has_line(g, R"(^build obj/mcpplibs_mydep/.*main\.o *:)"))) << "the dependency's src/main.cpp has no object of its own";
        r = p.mcpp({"run"});
        expect(s::last_line(r.out) == "val=41") << "unexpected output: " << s::last_line(r.out);
    };
    "179 spaced paths"_test = [] {
        // A project in a directory with spaces, with an include directory with spaces and a
        // build program, builds and runs; the include flag is quoted whole with its prefix.
        const auto tmp = temp_dir();
        fs::create_directories(tmp / "my work dir");
        expect(fatal(e2e::succeeded(s::mcpp({"new", "spaced_proj"}, tmp / "my work dir")))) << "mcpp new failed";
        s::project p{tmp / "my work dir/spaced_proj", {{"vendor inc/deep dir/spaced_header.hpp", R"cpp(#pragma once
inline int spaced_header_value() { return 4242; }
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "spaced_proj"
version = "0.1.0"

[build]
include_dirs = ["vendor inc/deep dir"]
)cpp"},
                                                       {"build.mcpp", R"cpp(#include <cstdio>
int main() {
    std::puts("mcpp:cfg=SPACED_BUILD_PROGRAM_RAN");
    std::puts("mcpp:rerun-if-changed=build.mcpp");
    return 0;
}
)cpp"}, {"src/main.cpp", R"cpp(#include <spaced_header.hpp>
import std;
int main() {
#ifndef SPACED_BUILD_PROGRAM_RAN
    std::println("build.mcpp directive missing");
    return 1;
#endif
    if (spaced_header_value() != 4242) {
        std::println("wrong header value");
        return 1;
    }
    std::println("spaced-ok");
    return 0;
}
)cpp"}}};
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build in a spaced path: " << s::text(r);
        r = p.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "run: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "spaced-ok"))) << "run output: " << s::text(r);
        const auto g = graph_text(p);
        expect(fatal(e2e::contains(g, "vendor"))) << "include dir absent from build.ninja";
        expect(s::has_line(g, R"(['"]-I[^'"]*vendor\$ inc)")) << "include dir not shell-quoted with its prefix";
    };
    "246 explicit empty sources"_test = [] {
        // `sources = []` compiles nothing beyond the entry, and an absent sources key keeps the
        // default glob.
        s::project p{s::files{{"src/leftover.cpp", "int mcpp_leftover_probe(void) { return 1; }\n"}, {"main.cpp", "int main() { return 0; }\n"}}};
        auto write_manifest = [&](std::string_view body) { p.write("mcpp.toml", std::format(R"cpp([package]
name    = "probe"
version = "0.1.0"
[build]
{0}
[targets.probe]
kind = "bin"
main = "main.cpp"
)cpp", body)); };
        auto hits = [&] { return s::count_lines(graph_text(p), "leftover"); };
        write_manifest("sources = []");
        p.remove("target");
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed\n" << s::text(r);
        expect(fatal(hits() == 0u)) << "'sources = []' still compiled the leftover source (" << hits() << " references)";
        write_manifest("# no sources key at all");
        p.remove("target");
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed\n" << s::text(r);
        expect(hits() > 0u) << "omitting 'sources' stopped applying the default glob";
    };

    "316 link unit with no inputs is refused"_test[tag{"requires:gcc"}] = [] {
        // A shared or static library target with no sources is refused at plan time, naming
        // the target and its kind, producing nothing; one with a source builds.
        s::needs({"gcc"});
        const auto tmp = temp_dir();
        auto check_kind = [&](std::string_view kind, std::string_view human, std::string_view artifact) {
            s::project p{tmp / kind, {{"mcpp.toml", std::format(R"cpp([package]
name    = "ghost"
version = "0.1.0"

[targets.ghost]
kind = "{0}"
)cpp", kind)}}};
            fs::create_directories(p.at("src"));
            auto r = p.mcpp({"build"});
            expect(fatal(failed(r))) << "(" << kind << "): a target with no sources built successfully";
            expect(fatal(e2e::contains(s::text(r), "target 'ghost'"))) << "(" << kind << "): the error does not name the target:\n" << s::text(r);
            expect(fatal(e2e::contains(s::text(r), human))) << "(" << kind << "): the error does not say it is a " << human << ":\n" << s::text(r);
            expect(fatal(s::lacks(s::text(r), "-shared: not found"))) << "(" << kind << "): the build reached the shell and reported the old symptom";
            expect(fatal(!s::contains_nocase(s::text(r), "no input files"))) << "(" << kind << "): the build reached the compiler driver instead of being refused at plan time";
            expect(fatal(s::find_files(p.dir(), std::string(artifact)).empty())) << "(" << kind << "): an artifact was produced by a refused target";
        };
        check_kind("shared", "shared library", R"(libghost\.so.*)");
        check_kind("lib", "static library", R"(libghost\.a)");
        s::project ok{tmp / "ok", {{"mcpp.toml", R"cpp([package]
name    = "ok"
version = "0.1.0"

[targets.ok]
kind = "shared"
)cpp"}, {"src/ok.c", "int ok_value(void) { return 7; }\n"}}};
        auto r = ok.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "a library target WITH sources was refused\n" << s::text(r);
        expect(!s::find_files(ok.dir(), R"(libok\.so.*)").empty()) << "the control target produced no library";
    };
    "776 a path with no utf8 spelling is named"_test[tag{"requires:elf"}][tag{"requires:gcc"}] = [] {
        // A path with no UTF-8 spelling is named where it enters: a project directory is refused
        // by name; a source file is skipped and reported under the nearest nameable directory
        // while the rest builds; a build program's directive is refused by its key; an MCPP_HOME
        // is refused before anything is written into it.
        s::needs({"elf", "gcc"});
        const auto tmp = temp_dir();
        const std::string bad = "caf\xe9";  // Latin-1 for U+00E9; not UTF-8
        const s::options cdb{.env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};
        auto put = [](const fs::path& f, std::string_view text) {
            fs::create_directories(f.parent_path());
            std::ofstream{f, std::ios::binary} << text;
        };
        const auto bad_dir = tmp / fs::path(bad);
        put(bad_dir / "mcpp.toml", R"cpp([package]
name    = "latin1dir"
version = "0.1.0"
)cpp");
        put(bad_dir / "src/main.cpp", "int main() { return 0; }\n");
        auto r = s::mcpp({"build"}, bad_dir, cdb);
        expect(fatal(failed(r))) << "a project directory with no UTF-8 spelling built\n" << s::text(r);
        expect(fatal(s::lacks(s::text(r), "internal: unhandled exception"))) << "the directory still reaches a JSON writer";
        expect(fatal(e2e::contains(s::text(r), "has no UTF-8 spelling"))) << "the refusal does not say what is wrong\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "caf\\xE9"))) << "the refusal does not name the directory\n" << s::text(r);

        s::project ok{tmp / "ok", {{"mcpp.toml", R"cpp([package]
name    = "latin1file"
version = "0.1.0"
)cpp"}, {"src/main.cpp", "int main() { return 0; }\n"}}};
        put(ok.at("src") / fs::path(bad + ".cpp"), "int main() { return 7; }\n");
        r = ok.mcpp({"build"}, cdb);
        expect(fatal(e2e::succeeded(r))) << "a project holding such a name must still build\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "contains names that have no UTF-8 spelling"))) << "the skipped file is not reported\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "'" + ok.at("src").string() + "'"))) << "the report does not name the nearest nameable directory\n" << s::text(r);
        expect(fatal(e2e::succeeded(ok.mcpp({"run"}, cdb)))) << "the program did not run";
        const auto db = s::find_file(ok.dir(), "compile_commands.json");
        expect(fatal(db.has_value())) << "no compile_commands.json was written";
        expect(fatal(e2e::contains(s::read_file(*db), "main.cpp"))) << "the compile database lost the ordinary source";

        s::project bp{tmp / "bp", {{"mcpp.toml", R"cpp([package]
name    = "latin1directive"
version = "0.1.0"
)cpp"}, {"src/main.cpp", "int main() { return 0; }\n"}, {"build.mcpp", R"cpp(#include <cstdio>
int main() {
    std::printf("mcpp:cfg=FROM_BUILD_PROGRAM\n");
    std::printf("mcpp:include-dir=inc/caf\xE9\n");
    return 0;
}
)cpp"}}};
        r = bp.mcpp({"build"}, cdb);
        expect(fatal(failed(r))) << "a directive with no UTF-8 spelling was applied\n" << s::text(r);
        expect(fatal(s::lacks(s::text(r), "internal: unhandled exception"))) << "the directive still reaches a JSON writer";
        expect(fatal(e2e::contains(s::text(r), "whose text is not UTF-8: mcpp:include-dir"))) << "the refusal does not name the directive\n" << s::text(r);

        const auto home = tmp / fs::path("home-" + bad);
        r = e2e::run({.argv = {s::mcpp_path(), "build"}, .cwd = ok.dir(),
                      .env = {{"MCPP_HOME", home.string()}, {"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}});
        expect(fatal(failed(r))) << "an MCPP_HOME with no UTF-8 spelling was used\n" << s::text(r);
        expect(fatal(s::has_line(s::text(r), "the mcpp home .* has no UTF-8 spelling"))) << "the refusal does not name the home\n" << s::text(r);
        expect(!fs::exists(home / "registry")) << "the refused home was written into";
    };
    "907 the library interface is stated once"_test[tag{"requires:elf"}] = [] {
        // A [lib] path naming no file is refused in the package's own build, naming it, and only
        // reported to a consumer, which builds; [modules] exports must list every public module
        // and may leave a private one out; importing a module that is not public is warned about.
        s::needs({"elf"});
        const auto tmp = temp_dir();
        const s::options o{.where = s::home::inherited};
        s::project lib{tmp / "lib", {{"src/lib.cppm", "export module acme.lib;\nexport import acme.lib.part;\nexport int lib_value() { return 40; }\n"},
                                     {"src/part.cppm", "export module acme.lib.part;\nexport int part_value() { return 2; }\n"},
                                     {"src/detail.cppm", "export module acme.lib.detail;\nexport int detail_value() { return 0; }\n"}, {"mcpp.toml", R"cpp([package]
namespace = "acme"
name      = "lib"
version   = "0.1.0"
[lib]
path = "src/missing.cppm"
[targets.lib]
kind = "lib"
)cpp"}}};
        auto r = lib.mcpp({"build"}, o);
        expect(fatal(failed(r))) << "A: a [lib] path naming no file was accepted in the package's own build\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "missing.cppm"))) << "A: the error does not name the path\n" << s::text(r);
        s::project app{tmp / "app", {{"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n[dependencies.acme]\nlib = { path = \"../lib\" }\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                     {"src/main.cpp", "#include <cstdio>\nimport acme.lib;\nint main() { std::printf(\"V=%d\\n\", lib_value() + part_value()); }\n"}}};
        r = app.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "B: a dependency's missing [lib] path failed its consumer\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "names no file"))) << "B: the consumer was not told\n" << s::text(r);
        expect(fatal(s::last_starting(app.mcpp({"run"}, o).out, "V=") == "V=42")) << "B: value";
        auto m = lib.read("mcpp.toml");
        m.replace(m.find("path = \"src/missing.cppm\""), 25, "path = \"src/lib.cppm\"");
        lib.write("mcpp.toml", m + "[modules]\nexports = [\"acme.lib\"]\n");
        r = lib.mcpp({"build"}, o);
        expect(fatal(failed(r))) << "C: a public module missing from [modules] exports was accepted\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "acme.lib.part"))) << "C: the error does not name the public module\n" << s::text(r);
        m = lib.read("mcpp.toml");
        m.replace(m.find("exports = [\"acme.lib\"]"), 22, "exports = [\"acme.lib\", \"acme.lib.part\"]");
        lib.write("mcpp.toml", m);
        r = lib.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "C: exports listing the public modules, and not the private one, was refused\n" << s::text(r);
        app.write("src/main.cpp", "#include <cstdio>\nimport acme.lib;\nimport acme.lib.detail;\nint main() { std::printf(\"V=%d\\n\", lib_value() + part_value() + detail_value()); }\n");
        r = app.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "D: the build failed\n" << s::text(r);
        expect(e2e::contains(s::text(r), "acme.lib.detail") && s::contains_nocase(s::text(r), "not one of that package's public modules")) << "D: importing a module that is not public was not warned about\n" << s::text(r);
    };

    "909 deprecated readings are registered and kept"_test[tag{"requires:elf"}] = [] {
        // The deprecated host-module spellings still build (the plugin imports from its
        // [build-dependencies]) with a warning stating both releases and the canonical spelling,
        // the package stays off the target, `mcpp why compat` lists the use (JSON states the
        // floor), and --strict refuses the deprecated readings.
        s::needs({"elf"});
        const auto tmp = temp_dir();
        const s::options o{.where = s::home::inherited};
        s::project{tmp / "inner", {{"src/inner.cppm", "export module inner;\nexport int inner_value() { return 40; }\n"},
                                   {"mcpp.toml", "[package]\nname = \"inner\"\nversion = \"0.1.0\"\n[targets.inner]\nkind = \"lib\"\n"}}};
        s::project{tmp / "rule", {{"src/rule.cppm", "export module rule;\nimport std;\nimport mcpp;\nimport inner;\nexport namespace rule { inline void go() { mcpp::cxxflag(std::format(\"-DV={}\", inner_value() + 2).c_str()); } }\n"},
                                  {"mcpp.toml", "[package]\nname = \"rule\"\nversion = \"0.1.0\"\n[build-dependencies]\ninner = { path = \"../inner\", host-module = true }\n[targets.rule]\nkind = \"lib\"\n"}}};
        s::project app{tmp / "app", {{"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n[dependencies]\nrule = { path = \"../rule\", host-module = true }\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                     {"src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"V=%d\\n\", V); }\n"},
                                     {"build.mcpp", "import mcpp;\nimport rule;\nint main() { rule::go(); }\n"}}};
        auto r = app.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "A/D: the deprecated spellings stopped building\n" << s::text(r);
        expect(fatal(s::last_starting(app.mcpp({"run"}, o).out, "V=") == "V=42")) << "A/D: value";
        expect(fatal(e2e::contains(s::text(r), "deprecated since mcpp 2026.10.11.1 and removed from mcpp 2027.1"))) << "A: the warning does not state both releases\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "[build-dependencies]"))) << "A: the warning does not name the canonical spelling";
        expect(fatal(e2e::contains(s::text(r), "plugin-imports-from-build-dependencies") || e2e::contains(s::text(r), "[build-dependencies] inner"))) << "D: the plugin's reading was not reported";
        for (const auto& f : s::find_files(app.at("target"), R"(rule\.m\.o)"))
            expect(fatal(f.generic_string().find("/obj/") == std::string::npos || f.generic_string().find("/.build-mcpp/") != std::string::npos)) << "A: the package reached the target";
        r = app.mcpp({"why", "compat"}, o);
        expect(fatal(e2e::succeeded(r))) << "B: mcpp why compat failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "host-module-in-dependencies"))) << "B: the use is not listed\n" << s::text(r);
        r = app.mcpp({"why", "compat", "--format", "json"}, o);
        expect(fatal(e2e::succeeded(r))) << "B: --format json failed";
        expect(fatal(e2e::contains(r.out, "\"floor\": \"2027.1\""))) << "B: the JSON does not state the floor\n" << r.out;
        r = app.mcpp({"build", "--strict"}, o);
        expect(fatal(failed(r))) << "C: --strict accepted a deprecated reading\n" << s::text(r);
        expect(e2e::contains(s::text(r), "refuses the deprecated readings")) << "C: the refusal does not say why\n" << s::text(r);
    };
}};

}  // namespace
