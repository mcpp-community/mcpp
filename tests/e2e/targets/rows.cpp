// The rows a target names: the bare OS aliases of a conditional table, the vocabulary's refusals
// and spellings, the sysroot override and the target queries a board package asks, the variable
// that says a target was named, the cache slot of `run --target`, the optional env segment, the
// aarch64-linux-gnu cross served by a sysroot payload, and a program of another system that is
// built and not run (formerly tests/e2e/91, 103, 134, 267, 283, 284, 1076, 1077).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// The entries of a directory that `ls` prints (no leading dot), sorted.
std::vector<std::string> entries(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        auto name = e.path().filename().string();
        if (!name.starts_with(".")) out.push_back(std::move(name));
    }
    std::ranges::sort(out);
    return out;
}

// `find <root> -path '*/bin/<name>*' -type f | head -1`.
std::optional<fs::path> program_under(const fs::path& root, std::string_view name, bool prefix = false) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return std::nullopt;
    std::vector<fs::path> found;
    for (const auto& e : fs::recursive_directory_iterator(root, ec)) {
        if (!e.is_regular_file() || e.path().parent_path().filename() != "bin") continue;
        const auto file = e.path().filename().string();
        if (prefix ? file.starts_with(name) : file == name) found.push_back(e.path());
    }
    if (found.empty()) return std::nullopt;
    std::ranges::sort(found);
    return found.front();
}

// The text contains `a`, and `b` after it (a shell `case` pattern `*a*b*`).
bool in_order(std::string_view text, std::string_view a, std::string_view b) {
    const auto at = text.find(a);
    return at != std::string_view::npos && text.find(b, at + a.size()) != std::string_view::npos;
}

constexpr std::string_view reporting_build_program = R"(import mcpp;
import std;
int main() {
    std::cerr << "builtins=[" << (mcpp::target_builtins_lib() ?: "") << "]\n"
              << "profile=["  << (mcpp::target_libc_profile() ?: "") << "]\n"
              << "libc=["     << (mcpp::target_libc()         ?: "") << "]\n";
    return 1;
}
)";

E2E rows{"rows", [] {
    "91 target bare alias"_test = [] {
        // `[target.linux.build]` is `[target.'cfg(linux)'.build]`: exactly one OS alias applies
        // (the host's), and each bare alias agrees with its cfg() form on whichever host runs.
        s::project p({{"mcpp.toml", R"([package]
name    = "app"
version = "0.1.0"

# Bare-alias forms (the sugar under test).
[target.linux.build]
cxxflags = ["-DBARE_LINUX=1"]
[target.macos.build]
cxxflags = ["-DBARE_MACOS=1"]
[target.windows.build]
cxxflags = ["-DBARE_WIN=1"]
[target.unix.build]
cxxflags = ["-DBARE_UNIX=1"]

# cfg(...) forms — must produce identical results (parity).
[target.'cfg(linux)'.build]
cxxflags = ["-DCFG_LINUX=1"]
[target.'cfg(macos)'.build]
cxxflags = ["-DCFG_MACOS=1"]
[target.'cfg(windows)'.build]
cxxflags = ["-DCFG_WIN=1"]
[target.'cfg(unix)'.build]
cxxflags = ["-DCFG_UNIX=1"]
)"},
                      {"src/main.cpp", R"(// Exactly one OS bare-alias must apply — the host's — on any platform.
#if (defined(BARE_LINUX) + defined(BARE_MACOS) + defined(BARE_WIN)) != 1
#error "exactly one bare OS alias (linux/macos/windows) must apply on any host"
#endif
// Bare alias and cfg() must agree per OS (parity).
#if defined(BARE_LINUX) != defined(CFG_LINUX)
#error "[target.linux] disagrees with [target.'cfg(linux)']"
#endif
#if defined(BARE_MACOS) != defined(CFG_MACOS)
#error "[target.macos] disagrees with [target.'cfg(macos)']"
#endif
#if defined(BARE_WIN) != defined(CFG_WIN)
#error "[target.windows] disagrees with [target.'cfg(windows)']"
#endif
#if defined(BARE_UNIX) != defined(CFG_UNIX)
#error "[target.unix] disagrees with [target.'cfg(unix)']"
#endif
// unix family applies iff not windows.
#if defined(BARE_WIN) && defined(BARE_UNIX)
#error "unix wrongly applied on a windows host"
#endif
#if !defined(BARE_WIN) && !defined(BARE_UNIX)
#error "unix should apply on a non-windows host"
#endif
int main() { return 0; }
)"}});
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: build errored (bare alias not honored?): " << s::text(r);
    };

    "103 target vocabulary"_test[s::gcc] = [] {
        auto p = s::project::scaffold("vocab");
        const auto original = p.read("mcpp.toml");
        // 1. A typo is a hard error with a did-you-mean, never a silent host build.
        auto r = p.mcpp({"build", "--target", "x86_64-linux-mus"});
        expect(fatal(!r)) << "FAIL: typo'd --target built successfully (silent host fallthrough): " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "unknown target 'x86_64-linux-mus'"))) << "FAIL: missing unknown-target error: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "did you mean 'x86_64-linux-musl'"))) << "FAIL: missing did-you-mean suggestion: " << s::text(r);
        // 2. A planned row is refused, not built for the host; a toolchain named for the build
        //    opts it in (2026.11.13.1), whatever the toolchain then makes of the row.
        r = p.mcpp({"build", "--target", "riscv64-linux-musl"});
        expect(fatal(!r)) << "FAIL: planned-tier target built: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "not yet supported"))) << "FAIL: missing planned-tier error: " << s::text(r);
        r = p.mcpp({"build", "--target", "riscv64-linux-musl", "--toolchain", "gcc@16.1.0"});
        expect(fatal(s::lacks(s::text(r), "not yet supported"))) << "FAIL: --toolchain did not opt a planned row in";
        // 3. A custom triple with an explicit [target.X] section proceeds.
        p.write("mcpp.toml", original + "\n[target.myboard-custom-elf]\ntoolchain = \"gcc@16.1.0\"\n");
        r = p.mcpp({"build", "--target", "myboard-custom-elf"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: explicit [target.X] escape hatch rejected: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "unknown target"))) << "FAIL: escape hatch still errored";
        // 4. [build] target is the project's default build target.
        p.write("mcpp.toml", original);
        const auto home = s::env("HOME").value_or("");
        if (!fs::is_directory(fs::path(home) / ".xlings/data/xpkgs/xim-x-musl-gcc") &&
            !fs::is_directory(s::shared_home() / "registry/data/xpkgs/xim-x-musl-gcc")) {
            std::println("(musl toolchain not installed — steps 4-5 skipped)");
            return;
        }
        p.write("mcpp.toml", original + "\n[build]\ntarget = \"x86_64-linux-musl\"\n");
        p.remove("target");
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: [build] target build failed: " << s::text(r);
        expect(fatal(fs::is_directory(p.at("target/x86_64-linux-musl")))) << "FAIL: [build] target did not select the musl target dir";
        // 5. An alias spelling lands in the same canonical directory.
        r = p.mcpp({"build", "--target", "x86_64-unknown-linux-musl"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: alias-spelling build failed: " << s::text(r);
        expect(fatal(!fs::is_directory(p.at("target/x86_64-unknown-linux-musl")))) << "FAIL: alias spelling minted its own target dir";
    };

    "134 target sysroot and queries"_test[s::llvm][s::unix_shell] = [] {
        // `[target.<triple>].sysroot` and the three target queries, each in both states: with
        // the override and without, with a C library and without one.
        auto p = s::project::scaffold("probe");
        for (const auto& t : s::glob(p.at("tests"), "*.cpp")) fs::remove(t);
        p.write("build.mcpp", reporting_build_program);
        p.write("src/main.cpp", "extern \"C\" void _start() { for (;;) {} }\n");
        auto manifest = [&](std::string_view extra) {
            p.write("mcpp.toml", std::format("[package]\nname    = \"probe\"\nversion = \"0.1.0\"\n\n[build]\ntarget = \"riscv64-none-elf\"\n{}\n", extra));
        };
        // 1. The target row's C library, and the queries derived from it.
        manifest("");
        auto q = s::text(p.mcpp({"build"}));
        expect(fatal(e2e::contains(q, "builtins=[clang_rt.builtins-riscv64]"))) << "rv64 builtins query wrong: " << q;
        expect(fatal(e2e::contains(q, "profile=[rv64gc/lp64d]"))) << "rv64 libc profile query wrong: " << q;
        expect(fatal(e2e::contains(q, "libc=[picolibc-riscv]"))) << "libc name query wrong: " << q;
        // 2. The same manifest at the other width.
        q = s::text(p.mcpp({"build", "--target", "riscv32-none-elf"}));
        expect(fatal(e2e::contains(q, "builtins=[clang_rt.builtins-riscv32]"))) << "rv32 builtins query did not follow the target: " << q;
        expect(fatal(e2e::contains(q, "profile=[rv32imac/ilp32]"))) << "rv32 libc profile query did not follow the target: " << q;
        // 3. The C library is reachable without the override (the control of step 4).
        p.write("src/main.cpp", "#include <stdio.h>\nextern \"C\" void _start() { (void)sizeof(FILE); for (;;) {} }\n");
        p.write("build.mcpp", "import mcpp;\nint main() { return 0; }\n");
        manifest("");
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the target row's C library should have been usable: " << s::text(r);
        // 4. `sysroot = ""` really removes it.
        manifest("\n[target.riscv64-none-elf]\nsysroot = \"\"");
        r = p.mcpp({"build"});
        expect(fatal(!r)) << "sysroot = \"\" did not remove the C library — <stdio.h> still resolved: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "stdio.h"))) << "build failed, but not because the C library was gone: " << s::text(r);
        // 5. The zero-libc tier still produces an image.
        p.write("src/main.cpp", R"(static volatile unsigned char* const UART =
    reinterpret_cast<unsigned char*>(0x10000000);
extern "C" void kmain() {
    for (const char* p = "zero-libc\n"; *p; ++p)
        *UART = static_cast<unsigned char>(*p);
    *reinterpret_cast<volatile unsigned int*>(0x100000) = 0x5555;
    for (;;) {}
}
asm(".section .text.entry\n.globl _start\n_start:\n"
    "  la sp, __stack_top\n  call kmain\n1: j 1b\n");
)");
        p.write("link.ld", R"(ENTRY(_start)
SECTIONS {
  . = 0x80000000;
  .text : { *(.text.entry) *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss : { *(.bss*) *(COMMON) }
  . = ALIGN(16); . = . + 0x1000; __stack_top = .;
}
)");
        manifest(std::format("ldflags = [\"-T\", \"{}\"]\n\n[target.riscv64-none-elf]\nsysroot = \"\"", p.at("link.ld").generic_string()));
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "a self-contained zero-libc image should still build: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Size probe"))) << "no size summary — the freestanding link path was not taken: " << s::text(r);
        // 6. On the zero-libc tier the libc-facing queries are empty; builtins, a compiler fact,
        //    stays.
        p.write("build.mcpp", reporting_build_program);
        q = s::text(p.mcpp({"build"}));
        expect(fatal(e2e::contains(q, "profile=[]"))) << "libc profile should be empty with no C library: " << q;
        expect(fatal(e2e::contains(q, "libc=[]"))) << "libc name should be empty with no C library: " << q;
        expect(fatal(e2e::contains(q, "builtins=[clang_rt.builtins-riscv64]"))) << "builtins is a compiler fact and must survive: " << q;
        // 7. A bare name is rejected at parse time.
        manifest("\n[target.riscv64-none-elf]\nsysroot = \"newlib\"");
        r = p.mcpp({"build"});
        expect(fatal(!r)) << "a bare sysroot name should have been rejected: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "is not a sysroot. A sysroot is a package"))) << "rejection did not explain the expected form: " << s::text(r);
    };

    "267 target requested is empty for a native build"_test[s::unix_shell] = [] {
        // MCPP_TARGET_REQUESTED is empty for a native build and the named triple otherwise, the
        // host's own triple included. The build program reports through a non-zero exit, whose
        // output mcpp prints.
        s::project p({{"mcpp.toml", "[package]\nname    = \"target-requested-probe\"\nversion = \"0.1.0\"\n"},
                      {"src/main.cpp", "int main() { return 0; }\n"},
                      {"build.mcpp", R"(import mcpp;
import std;
int main() {
    const char* t = std::getenv("MCPP_TARGET");
    const char* r = std::getenv("MCPP_TARGET_REQUESTED");
    std::cerr << "PROBE target=[" << (t ? t : "") << "] "
              << "requested=[" << (r ? r : "") << "]\n";
    return 1;
}
)"}});
        const auto native = s::text(p.mcpp({"build"}));
        expect(fatal(e2e::contains(native, "PROBE "))) << "the build program did not report; mcpp prints a failing program's output: " << native;
        expect(fatal(s::has_line(native, R"(requested=\[\])"))) << "MCPP_TARGET_REQUESTED should be empty for a native build";
        expect(fatal(s::has_line(native, R"(target=\[[a-z0-9_]+-[a-z0-9-]+\])"))) << "MCPP_TARGET should carry the host triple for a native build";
        std::smatch m;
        std::string host_triple;
        static const std::regex probe{R"(PROBE target=\[([^\]]*)\])"};
        if (std::regex_search(native, m, probe)) host_triple = m[1].str();
        expect(fatal(!host_triple.empty())) << "could not read the host triple from the probe";
        const auto cross = s::text(p.mcpp({"build", "--target", host_triple}));
        expect(fatal(e2e::contains(cross, "PROBE "))) << "the build program did not report on the cross build: " << cross;
        expect(fatal(e2e::contains(cross, std::format("requested=[{}]", host_triple)))) << "MCPP_TARGET_REQUESTED should carry the triple that was named: " << cross;
    };

    "283 run target flag owns its cache slot"_test[s::gcc] = [] {
        // `mcpp run --target X` does not write X's build into the host's cache slot.
        s::project p({{"mcpp.toml", "[package]\nname    = \"runslot\"\nversion = \"0.1.0\"\n"},
                      {"src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"host-artifact\\n\"); }\n"}});
        const std::string cross = "x86_64-linux-musl";
        if (!p.mcpp({"build", "--target", cross})) skip("this machine cannot build " + cross);
        p.remove("target");
        if (!p.mcpp({"run", "--target", cross})) skip(cross + " builds but does not run here");
        auto r = p.mcpp({"run"});
        const auto out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "FAIL: a bare `mcpp run` after a cross run exited non-zero: " << out;
        std::smatch m;
        static const std::regex running{"Running `([^`]*)`"};
        std::string ran;
        if (std::regex_search(out, m, running)) ran = m[1].str();
        expect(fatal(!ran.empty())) << "FAIL: no `Running` line in the output: " << out;
        expect(fatal(s::lacks(ran, "/" + cross + "/"))) << "FAIL: a bare `mcpp run` exec'd the " << cross << " artifact; the cross build was recorded under the host's cache key";
        expect(fatal(e2e::contains(out, "host-artifact"))) << "FAIL: the host program's own output is missing: " << out;
    };

    "284 env segment is optional everywhere"_test[s::gcc] = [] {
        // A target may decline its env segment on every platform, and declining it changes no
        // identity: the short and the canonical spelling build into one fingerprint.
        s::project p({{"mcpp.toml", "[package]\nname    = \"envopt\"\nversion = \"0.1.0\"\n"},
                      {"src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"ok\\n\"); }\n"}});
        auto check_pair = [&](const std::string& shortname, const std::string& canonical) {
            p.remove("target");
            auto r = p.mcpp({"build", "--target", shortname});
            const auto out = s::text(r);
            if (!r) {
                expect(fatal(s::lacks(out, "unknown target"))) << "FAIL: `" << shortname << "` was rejected";
                std::println("SKIP: {} cannot build here (no payload/toolchain)", shortname);
                return;
            }
            const auto dir = p.at("target/" + canonical);
            expect(fatal(fs::is_directory(dir))) << "FAIL: `" << shortname << "` did not build into target/" << canonical;
            const auto first = entries(dir);
            expect(fatal(e2e::succeeded(p.mcpp({"build", "--target", canonical})))) << "FAIL: canonical `" << canonical << "` failed after `" << shortname << "` succeeded";
            const auto after = entries(dir);
            expect(fatal(after.size() == 1u)) << "FAIL: the two spellings produced " << after.size() << " fingerprints, not one";
            expect(fatal(e2e::contains(out, "Target " + shortname))) << "FAIL: the report did not head with `" << shortname << "`";
            std::println("  ok  {} → {}  (one fingerprint: {})", shortname, canonical, first.empty() ? "" : first.front());
        };
        check_pair("x86_64-linux", "x86_64-linux-gnu");
        // On Windows a host with MSVC fills the segment with msvc; elsewhere with musl.
        const std::string win = s::on_windows() && bool(s::msvc) ? "x86_64-windows-msvc" : "x86_64-windows-musl";
        check_pair("x86_64-windows", win);
        check_pair("riscv64-none", "riscv64-none-elf");
        // mcpp names a C library LLVM cannot spell, and its provider builds it.
        const auto out = s::text(p.mcpp({"build", "--target", "x86_64-windows-musl"}));
        expect(fatal(s::lacks(out, "unknown target"))) << "FAIL: x86_64-windows-musl is not in the target table";
        expect(fatal(in_order(out, "Platform openkal-platform", "built-in, hosted"))) << "FAIL: the build was not served by openkal-platform: " << out;
        expect(fatal(e2e::contains(out, "→ x86_64-windows-musl →") && e2e::contains(out, "llvm"))) << "FAIL: the toolchain line did not carry mcpp's own name for the target: " << out;
        expect(fatal(e2e::contains(out, "Target x86_64-windows-musl → x86_64-w64-windows-gnu"))) << "FAIL: the report does not show the compiler's triple: " << out;
    };

    "1076 an aarch64 linux gnu cross comes from a sysroot payload"_test[s::llvm][s::elf][s::x86_64_host] = [] {
        // aarch64-linux-gnu is served by the aarch64-linux-gnu-sysroot payload: C, C++ and
        // import std programs build, name the row's loader, carry no path of this machine, and
        // run through the provider's runner; a static program names no interpreter.
        const std::string T = "aarch64-linux-gnu";
        const std::string loader = "/lib/ld-linux-aarch64.so.1";
        const auto tmp = s::case_dir();
        auto project = [&](std::string_view dir, std::string_view main, std::string_view source) {
            return s::project(tmp / dir, {{"mcpp.toml", std::format("[package]\nname    = \"cross\"\nversion = \"0.1.0\"\n\n[targets.cross]\nkind = \"bin\"\nmain = \"src/{}\"\n", main)},
                                          {std::format("src/{}", main), source}});
        };
        auto c = project("c", "main.c", "#include <stdio.h>\nint main(void) { printf(\"cross-c\\n\"); return 42; }\n");
        auto cxx = project("cxx", "main.cpp", "#include <print>\nint main() { std::println(\"cross-cxx\"); return 43; }\n");
        auto stdp = project("std", "main.cpp", "import std;\nint main() { std::println(\"cross-std\"); return 44; }\n");
        auto r = c.mcpp({"why", "target", "--target", "aarch64-linux"});
        expect(fatal(e2e::succeeded(r))) << "why target refused: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "aarch64-linux-musl"))) << "aarch64-linux is no longer answered by the musl identity: " << s::text(r);
        const auto store = s::shared_home().string();
        auto check = [&](const s::project& p, std::string_view label, std::string_view word, int status) {
            auto b = p.mcpp({"build", "--target", T});
            expect(fatal(e2e::succeeded(b))) << label << ": " << T << " did not build over the base package: " << s::text(b);
            expect(fatal(e2e::contains(s::text(b), "glibc-sysroot") || e2e::contains(s::text(b), "aarch64-linux-gnu-sysroot"))) << label << ": the base package is not what served it: " << s::text(b);
            const auto exe = program_under(p.at("target/" + T), "cross");
            expect(fatal(exe.has_value())) << label << ": no program: " << s::text(b);
            const auto h = s::read_header(*exe);
            expect(fatal(h.kind == s::format::elf && h.machine == s::elf_machine::aarch64)) << label << ": the artifact is not an aarch64 ELF (machine " << h.machine << ")";
            expect(fatal(h.interpreter == loader)) << label << ": the interpreter is '" << h.interpreter << "', the row's loader is " << loader;
            expect(fatal(!h.elf_rpath && !h.elf_runpath)) << label << ": the artifact carries a run-time path of this machine: " << h.runpath;
            expect(fatal(s::lacks(s::read_file(*exe), store))) << label << ": a path of this machine's store is in the artifact";
            auto run = p.mcpp({"run", "--target", T});
            expect(fatal(run.status == status)) << label << ": mcpp run exited " << run.status << ", the program returns " << status << ": " << s::text(run);
            expect(fatal(s::has_line(s::text(run), std::format("^{}$", word)))) << label << ": the program's output did not arrive: " << s::text(run);
            expect(fatal(s::has_line(s::text(run), "Running .*qemu-aarch64"))) << label << ": the run did not go through the provider's runner: " << s::text(run);
        };
        check(c, "c", "cross-c", 42);
        check(cxx, "cxx", "cross-cxx", 43);
        check(stdp, "std", "cross-std", 44);
        stdp.write("mcpp.toml", stdp.read("mcpp.toml") + std::format("\n[target.{}]\nlinkage = \"static\"\n", T));
        const auto manifest_time = fs::last_write_time(stdp.at("mcpp.toml"));
        r = stdp.mcpp({"build", "--target", T});
        expect(fatal(e2e::succeeded(r))) << "the static import-std program did not build: " << s::text(r);
        const auto exe = program_under(stdp.at("target/" + T), "cross");
        expect(fatal(exe.has_value() && fs::last_write_time(*exe) > manifest_time)) << "no static program: " << s::text(r);
        expect(fatal(!s::read_header(*exe).elf_interpreter)) << "a static program names an interpreter";
        auto run = stdp.mcpp({"run", "--target", T});
        expect(fatal(run.status == 44)) << "static: mcpp run exited " << run.status << ", the program returns 44: " << s::text(run);
    };

    "1077 a program of another system is built not run"_test[s::llvm] = [] {
        // `mcpp run` builds a FreeBSD program on this host and refuses to start it, naming the
        // artifact's system and the runner declaration; --no-runner is believed.
        const std::string T = std::string(s::on_x86_64() ? "x86_64" : "aarch64") + "-freebsd-musl";
        auto p = s::project::scaffold("app");
        auto r = p.mcpp({"build", "--target", T});
        expect(fatal(e2e::succeeded(r))) << "the static FreeBSD program did not build: " << s::text(r);
        expect(fatal(program_under(p.at("target/" + T), "app", true).has_value())) << "no program: " << s::text(r);
        r = p.mcpp({"run", "--target", T});
        const auto run = s::text(r);
        expect(fatal(e2e::exited(r, 2))) << "run exited " << r.status << ", expected the refusal's 2: " << run;
        expect(fatal(e2e::contains(run, "built, not run"))) << "the run does not say built, not run: " << run;
        expect(fatal(e2e::contains(run, "artifacts run on freebsd"))) << "the artifact's system is not named: " << run;
        expect(fatal(e2e::contains(run, std::format("[target.{}] runner = ", T)))) << "the runner declaration does not name the row: " << run;
        expect(fatal(s::lacks(run, "Running"))) << "the artifact was started: " << run;
        r = p.mcpp({"run", "--target", T, "--no-runner"});
        expect(fatal(s::lacks(s::text(r), "built, not run"))) << "--no-runner was not believed: " << s::text(r);
    };
}};

}  // namespace
