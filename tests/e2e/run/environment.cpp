// The environment `run` and `test` give a program: [runtime] library_dirs as a run path and on
// LD_LIBRARY_PATH, the toolchain's runtime directories, no loader variable leaking in or out, no
// private glibc on LD_LIBRARY_PATH, and a run that replays the build's scan (formerly
// tests/e2e/62, 65, 74, 114, 156, 166).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::optional<fs::path> graph_of(const fs::path& root) {
    auto all = s::find_files(root / "target", R"(build\.ninja)");
    std::ranges::sort(all);
    for (const auto& f : all)
        if (!f.generic_string().contains("/.build-mcpp/")) return f;
    return std::nullopt;
}

// The `LDLP=[...]` a probe printed (`grep -oE 'LDLP=\[[^]]*\]' | head -1`).
std::string ldlp(std::string_view text) {
    std::match_results<std::string_view::const_iterator> m;
    static const std::regex re{R"(LDLP=\[[^\]]*\])"};
    return std::regex_search(text.begin(), text.end(), m, re) ? m[0].str() : std::string();
}

constexpr std::string_view probe_cpp = R"(#include <cstdio>
#include <cstdlib>
int main() {
    const char* p = std::getenv("LD_LIBRARY_PATH");
    std::printf("LDLP=[%s]\n", p ? p : "");
    return 0;
}
)";

std::string dlopen_main(int base) {
    return std::format(R"(#include <dlfcn.h>

using answer_fn = int (*)();

int main() {{
    void* handle = dlopen("libruntime_plugin.so", RTLD_NOW);
    if (!handle) {{
        return {};
    }}
    auto answer = reinterpret_cast<answer_fn>(dlsym(handle, "runtime_plugin_answer"));
    if (!answer) {{
        dlclose(handle);
        return {};
    }}
    int result = answer();
    dlclose(handle);
    return result == 42 ? 0 : {};
}}
)", base, base + 1, base + 2);
}

E2E environment{"environment", [] {
    "62 runtime library dirs"_test[s::gcc][s::unix_shell] = [] {
        // [runtime] library_dirs is a run path of the program (-Wl,-rpath), not a link search
        // directory (-L), and a library dlopen()ed from it is found by `run` and `test`.
        s::project app({{"runtime/plugin.c", "int runtime_plugin_answer(void) {\n    return 42;\n}\n"},
                        {"src/main.cpp", dlopen_main(10)},
                        {"tests/test_runtime_plugin.cpp", dlopen_main(20)},
                        {"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.cpp\"]\nldflags = [\"-ldl\"]\n\n"
                                      "[runtime]\nlibrary_dirs = [\"runtime\"]\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}});
        // The host's C compiler makes the plugin, as the script's gcc did.
        const auto host_cc = s::host_cc();
        expect(fatal(host_cc.has_value())) << "no host C compiler links a shared object";
        auto cc = s::run(*host_cc, {"-shared", "-fPIC", "runtime/plugin.c", "-o", "runtime/libruntime_plugin.so"}, app.dir());
        expect(fatal(e2e::succeeded(cc))) << "the host gcc did not make the plugin: " << s::text(cc);
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed: " << s::text(r);
        const auto ninja = s::read_file(*graph_of(app.dir()));
        const auto runtime = fs::canonical(app.at("runtime")).string();
        expect(fatal(e2e::contains(ninja, "-Wl,-rpath," + runtime))) << "runtime directory missing from RUNPATH intent";
        expect(fatal(s::lacks(ninja, "-L" + runtime))) << "runtime-only directory leaked into link-library search (-L)";
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "run failed: " << s::text(r);
        r = app.mcpp({"test"});
        expect(fatal(e2e::succeeded(r))) << "test failed: " << s::text(r);
    };

    "65 toolchain runtime dirs for run"_test[s::scan_deps][s::elf] = [] {
        // `run` puts the toolchain's own runtime directory (the llvm payload's lib) on the
        // program's LD_LIBRARY_PATH.
        if (!s::on_linux()) skip("LD_LIBRARY_PATH runtime-dir check is Linux-specific");
        const auto version = s::llvm_version();
        if (!fs::exists(s::payload_root("xim-x-llvm", version) / "bin/clang++")) skip("xlings llvm@" + version + " is not installed");
        const s::options home{.where = s::home::inherited};
        s::project p(s::case_dir() / "proj", {
            {"mcpp.toml", std::format("[package]\nname    = \"toolchain_runtime_env\"\nversion = \"0.1.0\"\n\n[toolchain]\nlinux = \"llvm@{}\"\n\n"
                                      "[targets.toolchain_runtime_env]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", version)},
            {"src/main.cpp", std::format(R"(#include <cstdlib>
#include <string>

int main() {{
    const char* value = std::getenv("LD_LIBRARY_PATH");
    if (value == nullptr) return 10;

    std::string path(value);
    if (path.find("xim-x-llvm/{}/lib") == std::string::npos) return 11;
    return 0;
}}
)", version)},
        });
        auto r = p.mcpp({"build"}, home);
        expect(fatal(e2e::succeeded(r))) << "build failed: " << s::text(r);
        r = p.mcpp({"run"}, home);
        expect(fatal(e2e::succeeded(r))) << "mcpp run did not expose toolchain runtime dirs: " << s::text(r);
    };

    "74 run no loader env leak"_test[s::gcc][s::elf] = [] {
        // A poisoned LD_LIBRARY_PATH does not reach the build's tools, and the program's
        // interpreter is the bundled loader (or none), not the host's.
        auto p = s::project::scaffold("leakcheck");
        const auto old = s::env("LD_LIBRARY_PATH");
        auto r = p.mcpp({"run"}, {.env = {{"LD_LIBRARY_PATH", "/nonexistent/poison:" + old.value_or("")}}});
        expect(fatal(e2e::succeeded(r))) << "run under a poisoned LD_LIBRARY_PATH failed: " << s::text(r);
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto bin = s::find_file(p.at("target"), "leakcheck");
        expect(fatal(bin.has_value())) << "leakcheck binary not found";
        // `file`'s "interpreter" is the header's PT_INTERP, read here.
        const auto interp = s::read_header(*bin).interpreter;
        const bool host = !interp.contains("/.mcpp/") && !interp.contains("/registry/") && !interp.contains("xpkgs") &&
                          (interp.starts_with("/lib64/") || interp.starts_with("/lib/"));
        expect(fatal(!host)) << "host loader " << interp;
    };

    "114 run scan scope"_test[s::gcc] = [] {
        // `run` after a build, and again, takes the fast path (no module scan) however many
        // files the project holds; a source edit makes it scan again.
        s::project p({{"mcpp.toml", "[package]\nname = \"runscope\"\nversion = \"0.1.0\"\n"},
                      {"src/main.cpp", "#include <cstdio>\nint main() { std::puts(\"hello from runscope\"); return 0; }\n"}});
        for (int i = 1; i <= 1500; ++i) p.write(std::format("blob/file_{}.txt", i), "");
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const s::options verbose{.env = {{"MCPP_VERBOSE", "1"}}};
        for (std::string_view which : {"first", "second"}) {
            r = p.mcpp({"run"}, verbose);
            expect(fatal(s::lacks(s::text(r), "scan: scanning module sources"))) << which << " mcpp run re-scanned module sources instead of reusing the build cache: " << s::text(r);
            expect(fatal(e2e::contains(s::text(r), "hello from runscope"))) << "mcpp run did not produce expected program output: " << s::text(r);
        }
        std::this_thread::sleep_for(1100ms);
        fs::last_write_time(p.at("src/main.cpp"), fs::file_time_type::clock::now());
        r = p.mcpp({"run"}, verbose);
        expect(fatal(e2e::contains(s::text(r), "scan: scanning module sources"))) << "mcpp run did not re-scan after a source change: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "hello from runscope"))) << "mcpp run did not produce expected program output after rebuild: " << s::text(r);
    };

    "156 nested ld library path"_test = [] {
        // `test` under an LD_LIBRARY_PATH that names the private glibc runs its tests.
        std::optional<fs::path> glibc;
        for (const auto& d : s::glob(s::shared_home() / "registry/data/xpkgs/xim-x-glibc", "*/lib64")) {
            glibc = d;
            break;
        }
        if (!glibc) skip("no private glibc payload installed");
        s::project p({{"mcpp.toml", "[package]\nname = \"nest\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n"},
                      {"tests/t.cpp", "int main() { return 0; }\n"}});
        auto r = p.mcpp({"test"}, {.env = {{"LD_LIBRARY_PATH", glibc->string()}}});
        expect(fatal(e2e::succeeded(r))) << "poisoned run failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "t ... ok"))) << "test did not pass under poison: " << s::text(r);
    };

    "166 run env no private glibc"_test = [] {
        // The program `run` starts is not handed the private glibc on LD_LIBRARY_PATH, with or
        // without a [runtime] library_dirs entry, which itself stays.
        s::project p({{"mcpp.toml", "[package]\nname = \"envprobe\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n"}, {"src/main.cpp", probe_cpp}});
        auto r = p.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "mcpp run failed: " << s::text(r);
        const auto line = ldlp(s::text(r));
        expect(fatal(!line.empty())) << "probe never printed LDLP: " << s::text(r);
        expect(fatal(s::lacks(line, "xim-x-glibc"))) << "the run target was handed the private glibc payload on LD_LIBRARY_PATH: " << line;
        // The script looked in $HOME/.mcpp, whatever MCPP_HOME named.
        bool store = false;
        if (const auto home = s::env("HOME"))
            store = !s::glob(fs::path(*home) / ".mcpp/registry/data/xpkgs/xim-x-glibc", "*").empty();
        if (!store) skip("second half: no private glibc payload installed");
        s::project p2(s::case_dir() / "pkg2", {{"mcpp.toml", "[package]\nname    = \"envprobe2\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n\n[runtime]\nlibrary_dirs = [\"runtime\"]\n"},
                                               {"src/main.cpp", probe_cpp}});
        fs::create_directories(p2.at("runtime"));
        r = p2.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "mcpp run failed (second half): " << s::text(r);
        const auto line2 = ldlp(s::text(r));
        expect(fatal(s::lacks(line2, "xim-x-glibc"))) << "a build with a dlopen-reachable dependency library dir still put the private glibc payload on LD_LIBRARY_PATH (mcpp#401): " << line2;
        expect(fatal(e2e::contains(line2, "runtime"))) << "the project's own [runtime] library_dirs entry disappeared too: " << line2;
    };
}};

}  // namespace
