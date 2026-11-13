// A project's build program (`build.mcpp`): the `mcpp:` directives it prints, the typed
// `import mcpp;` library that prints them, the environment it is given and the cache that decides
// when it runs again (formerly tests/e2e/89, 92, 110).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view ran_again = R"(^ *build\.mcpp .* ran [0-9])";

E2E protocol{"protocol", [] {
    "89 build mcpp"_test[s::gcc] = [] {
        // mcpp compiles build.mcpp with the host toolchain and runs it before the build; its
        // directives add a define and a generated source; a declared-input cache runs it again
        // only when its source, inputs or environment change.
        s::project p{{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n"},
                     {"src/main.cpp", R"cpp(#ifndef FROM_BUILD_MCPP
#error "build.mcpp cxxflag did not reach the translation unit"
#endif
int generated_value();
int main() { return generated_value() == 42 ? 0 : 1; }
)cpp"},
                     {"build.mcpp", R"cpp(#include <cstdio>
#include <fstream>
int main() {
    std::ofstream("src/generated.cpp") << "int generated_value() { return 42; }\n";
    std::puts("mcpp:cxxflag=-DFROM_BUILD_MCPP=1");
    std::puts("mcpp:generated=src/generated.cpp");
    std::puts("mcpp:rerun-if-env-changed=MCPP_TEST_TOGGLE");
    return 0;
}
)cpp"}};
        auto b1 = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(b1))) << "build 1 errored";
        expect(e2e::contains(s::text(b1), "build.mcpp")) << "build.mcpp not invoked";
        expect(p.exists("src/generated.cpp")) << "generated source not written";
        expect(p.exists("target/.build-mcpp/build.mcpp.cache")) << "cache not written under target/";
        // The program returns 0 only if both the define and the generated source took effect.
        expect(e2e::succeeded(p.mcpp({"run"}))) << "run returned non-zero (define/generated source missing)";

        // A touched source makes prepare run; the program's inputs are unchanged, so its cache
        // answers (a reused program is stated under --verbose only).
        fs::last_write_time(p.at("src/main.cpp"), fs::file_time_type::clock::now());
        auto b2 = p.mcpp({"build", "-v"});
        expect(fatal(e2e::succeeded(b2))) << "build 2 errored";
        expect(s::has_line_nocase(s::text(b2), R"(build\.mcpp.*cached)")) << "build.mcpp did not short-circuit";

        // A declared environment input changed: the program runs again.
        fs::last_write_time(p.at("src/main.cpp"), fs::file_time_type::clock::now());
        auto b3 = p.mcpp({"build"}, {.env = {{"MCPP_TEST_TOGGLE", "1"}}});
        expect(fatal(e2e::succeeded(b3))) << "build 3 errored";
        expect(s::has_line(s::text(b3), ran_again)) << "changed env did not force build.mcpp re-run";

        // The program runs in the project root even when mcpp is started in a subdirectory.
        s::project sub{p.dir().parent_path() / "sub",
                       {{"mcpp.toml", "[package]\nname    = \"sub\"\nversion = \"0.1.0\"\n"},
                        {"src/main.cpp", "#ifndef FROM_BUILD_MCPP\n#error \"define missing\"\n#endif\n"
                                         "int gen();\nint main() { return gen() == 7 ? 0 : 1; }\n"},
                        {"build.mcpp", R"cpp(#include <fstream>
#include <cstdio>
int main() {
    std::ofstream("src/gen.cpp") << "int gen(){return 7;}\n";
    std::puts("mcpp:generated=src/gen.cpp");
    std::puts("mcpp:cxxflag=-DFROM_BUILD_MCPP=1");
    return 0;
}
)cpp"}}};
        expect(e2e::succeeded(sub.mcpp_in("src", {"build"}))) << "build from subdir errored (cwd not project root?)";
        expect(sub.exists("src/gen.cpp")) << "generated source not written to project root";
    };

    "92 build mcpp import"_test[s::gcc] = [] {
        // A modules-first build program (no #include, no `import std;`) calls the typed API of
        // the bundled `import mcpp;`, which prints the same directives.
        s::project p{{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n"},
                     {"build.mcpp", R"cpp(import mcpp;
int main() {
    mcpp::generated("src/gen.cpp");
    mcpp::cxxflag("-DVIA_MODULE=1");
    mcpp::define("MODULE_DEFINE");
    mcpp::rerun_if_changed("build.mcpp");
    return 0;
}
)cpp"},
                     {"src/gen.cpp", "int gen_value() { return 99; }\n"},
                     {"src/main.cpp", R"cpp(#if !defined(VIA_MODULE) || !defined(MODULE_DEFINE)
#error "import mcpp typed directives did not reach the build"
#endif
int gen_value();
int main() { return gen_value() == 99 ? 0 : 1; }
)cpp"}};
        auto b = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(b))) << "import mcpp build errored";
        expect(e2e::contains(s::text(b), "build.mcpp")) << "build.mcpp not invoked";
        // The bundled module is compiled once and kept by key: in the global cache, or, when the
        // cache mode asks for no global entry, in the build programs' graph under target/
        // (#748, #797).
        auto dir = p.mcpp({"cache", "dir"});
        const fs::path cache_root = dir.out.substr(0, dir.out.find_first_of("\r\n"));
        bool compiled = false;
        for (const auto& root : {cache_root / "pkg/_engine", p.at("target/.build-mcpp/host")})
            compiled = compiled || !s::find_files(root, R"(mcpp\.m\.o(bj)?)").empty();
        expect(compiled) << "bundled mcpp module not compiled";
        expect(e2e::succeeded(p.mcpp({"run"}))) << "run non-zero (module directives/generated source missing)";
    };

    "110 build mcpp env contract"_test[s::gcc] = [] {
        // The program is given MCPP_TARGET, HOST, PROFILE, OUT_DIR, MANIFEST_DIR and FEATURES;
        // they are part of its cache key (a feature change runs it again, an identical build does
        // not).
        auto p = s::project::scaffold("envbp");
        p.write("build.mcpp", R"cpp(#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
static const char* env_or(const char* n) { const char* v = std::getenv(n); return v ? v : "<unset>"; }
int main() {
    std::ofstream f("src/contract_gen.cpp");
    f << "extern \"C\" const char* bp_target()   { return \"" << env_or("MCPP_TARGET") << "\"; }\n";
    f << "extern \"C\" const char* bp_host()     { return \"" << env_or("MCPP_HOST") << "\"; }\n";
    f << "extern \"C\" const char* bp_profile()  { return \"" << env_or("MCPP_PROFILE") << "\"; }\n";
    f << "extern \"C\" const char* bp_tos()      { return \"" << env_or("MCPP_TARGET_OS") << "\"; }\n";
    f << "extern \"C\" const char* bp_tarch()    { return \"" << env_or("MCPP_TARGET_ARCH") << "\"; }\n";
    f << "extern \"C\" int bp_tenv_set() { return "
      << (std::getenv("MCPP_TARGET_ENV") ? 1 : 0) << "; }\n";
    f << "extern \"C\" const char* bp_features() { return \"" << env_or("MCPP_FEATURES") << "\"; }\n";
    f << "extern \"C\" int bp_has_extra() { return "
      << (std::getenv("MCPP_FEATURE_EXTRA") ? 1 : 0) << "; }\n";
    if (!f) return 1;
    const char* out = std::getenv("MCPP_OUT_DIR");
    if (!out) { std::fprintf(stderr, "MCPP_OUT_DIR unset\n"); return 1; }
    std::ofstream probe(std::string(out) + "/probe.txt");
    if (!probe) { std::fprintf(stderr, "MCPP_OUT_DIR not writable\n"); return 1; }
    std::printf("mcpp:generated=src/contract_gen.cpp\n");
    return 0;
}
)cpp");
        p.write("src/main.cpp", R"cpp(import std;
extern "C" const char* bp_target();
extern "C" const char* bp_host();
extern "C" const char* bp_profile();
extern "C" const char* bp_features();
extern "C" int bp_has_extra();
extern "C" const char* bp_tos();
extern "C" const char* bp_tarch();
extern "C" int bp_tenv_set();
int main() {
    std::println("target={} host={} profile={} features=[{}] extra={} tos={} tarch={} tenvset={}",
                 bp_target(), bp_host(), bp_profile(), bp_features(), bp_has_extra(),
                 bp_tos(), bp_tarch(), bp_tenv_set());
    return 0;
}
)cpp");
        p.write("mcpp.toml", "[package]\nname    = \"envbp\"\nversion = \"0.1.0\"\n\n[features]\ndefault = []\nextra   = []\n");
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "build 1 failed";
        const auto out = s::last_line(p.mcpp({"run"}).out);  // the program's line, after mcpp's own
        const std::string triple = R"([a-z0-9_]+-(linux|macos|windows)(-[a-z]+)?)";
        expect(e2e::matches(out, "target=" + triple + " host=" + triple + R"( profile=dev features=\[\] extra=0)"))
            << "unexpected contract values";
        // OS is one of the canonical spellings, ARCH is not empty, ENV is always set (empty on
        // macOS), and they agree with the triple.
        std::smatch m;
        expect(fatal(std::regex_search(out, m, std::regex(R"(tos=(linux|macos|windows) tarch=([a-z0-9_]+) tenvset=1)"))))
            << "target split vars missing/odd: " << out;
        expect(e2e::contains(out, "target=" + m[2].str() + "-" + m[1].str())) << "target split disagrees with MCPP_TARGET";

        // An identical build does not run the program again.
        auto b2 = p.mcpp({"build"});
        expect(s::no_line(s::text(b2), ran_again)) << "identical build re-ran build.mcpp";

        // A feature changes the contract: the program runs again and sees it.
        auto b3 = p.mcpp({"build", "--features", "extra"});
        expect(fatal(e2e::succeeded(b3))) << "build 3 failed";
        expect(s::has_line(s::text(b3), ran_again)) << "feature change did not re-run build.mcpp";
        auto bin = s::newest_file(p.at("target"), "envbp");
        expect(fatal(bin.has_value()));
        expect(e2e::contains(s::last_line(s::run(*bin).out), "features=[extra] extra=1")) << "feature not visible to build.mcpp";

        // So does the profile.
        auto b4 = p.mcpp({"build", "--release"});
        expect(fatal(e2e::succeeded(b4))) << "build 4 failed";
        expect(s::has_line(s::text(b4), ran_again)) << "profile change did not re-run build.mcpp";
        bin = s::newest_file(p.at("target"), "envbp");
        expect(fatal(bin.has_value()));
        expect(e2e::contains(s::last_line(s::run(*bin).out), "profile=release")) << "profile not visible";
    };

    "139 build program advisory"_test[s::unix_shell][resource{"machine"}] = [] {
        // A program that succeeds and still says something (`mcpp:warning=`). The second build is
        // the test: the program's result is cached, and an advisory printed only where the
        // program ran would appear once and never again.
        s::project p{{"mcpp.toml", "[package]\nname    = \"advisory\"\nversion = \"0.1.0\"\n"},
                     {"src/main.cpp", "int main() { return 0; }\n"},
                     {"build.mcpp", R"cpp(import mcpp;
import std;
int main() {
    mcpp::warning("no emulator found; `xlings install qemu-riscv -y` enables `mcpp run`");
    mcpp::define("ADVISORY_RAN");
    return 0;
}
)cpp"}};
        // 1. The advisory reaches the user on a build that succeeded, named by its package.
        const auto first = s::text(p.mcpp({"build"}));
        expect(e2e::contains(first, "no emulator found")) << "the advisory did not reach the output";
        expect(e2e::contains(first, "advisory: no emulator found")) << "the advisory was not attributed to its package";
        expect(s::contains_nocase(first, "Finished")) << "the build did not finish";
        // 2. A touched source makes the project stale and leaves the program's inputs unchanged:
        // a cache hit, which still carries the line (an unmodified build would take the
        // whole-project fast path and reach neither).
        fs::last_write_time(p.at("src/main.cpp"), fs::file_time_type::clock::now());
        const auto second = s::text(p.mcpp({"build", "-v"}));
        expect(s::has_line(second, R"(^ *build\.mcpp .* cached)")) << "the second build re-ran the program; the replay path was not exercised";
        expect(e2e::contains(second, "advisory: no emulator found")) << "the advisory was lost on a cache hit";
        // 3. Two members, two advisories, each named.
        s::project ws{s::case_dir() / "ws", {{"mcpp.toml", "[workspace]\nmembers = [\"a\", \"b\"]\n"}}};
        for (std::string m : {"a", "b"}) {
            ws.write(m + "/mcpp.toml", std::format("[package]\nname    = \"{}\"\nversion = \"0.1.0\"\n", m));
            ws.write(m + "/src/main.cpp", "int main() { return 0; }\n");
            ws.write(m + "/build.mcpp", std::format("import mcpp;\nimport std;\nint main() {{ mcpp::warning(\"advisory from {}\"); return 0; }}\n", m));
        }
        const auto all = s::text(ws.mcpp({"build"}));
        expect(e2e::contains(all, "a: advisory from a")) << "member a's advisory is missing or unattributed";
        expect(e2e::contains(all, "b: advisory from b")) << "member b's advisory is missing or unattributed";
    };

    "143 build mcpp source directive"_test[s::gcc] = [] {
        // `mcpp:source=` selects a source that already exists, outside every sources glob, into
        // the compile set (typed: mcpp::source); the cached record keeps it there.
        const std::string manifest = "[package]\nname    = \"srcsel\"\nversion = \"0.1.0\"\n\n[modules]\nsources = [\"src/**/*.cpp\"]\n\n[targets.srcsel]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n";
        s::project p{{"extra/impl.cpp", "extern \"C\" int selected_value() { return 42; }\n"},
                     {"build.mcpp", "import mcpp;\nint main() {\n    mcpp::source(\"extra/impl.cpp\");\n    return 0;\n}\n"},
                     {"src/main.cpp", "import std;\nextern \"C\" int selected_value();\nint main() {\n    std::println(\"SELECTED={}\", selected_value());\n    return 0;\n}\n"},
                     {"mcpp.toml", manifest}};
        auto b1 = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(b1))) << "build failed";
        expect(s::lacks(s::text(b1), "ignoring unknown directive")) << "mcpp:source= not recognized";
        expect(s::has_line(s::text(p.mcpp({"run"})), "^SELECTED=42$")) << "selected source not linked";
        // A changed main.cpp leaves the program's inputs alone: the cached `d source` record applies.
        p.write("src/main.cpp", "import std;\nextern \"C\" int selected_value();\nint main() {\n    std::println(\"SELECTED2={}\", selected_value());\n    return 0;\n}\n");
        auto b2 = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(b2))) << "rebuild failed";
        expect(s::no_line(s::text(b2), ran_again)) << "unchanged build.mcpp re-ran";
        expect(s::has_line(s::text(p.mcpp({"run"})), "^SELECTED2=42$")) << "cached source record lost";
        // A selected source that does not exist is an error that says so.
        p.write("build.mcpp", "import mcpp;\nint main() {\n    mcpp::source(\"extra/nope.cpp\");\n    return 0;\n}\n");
        auto b3 = p.mcpp({"build"});
        expect(b3.started && b3.status != 0) << "missing source did not fail";
        expect(e2e::contains(s::text(b3), "selected source")) << "missing-source error not surfaced";
    };

    "144 build mcpp include dir"_test[s::gcc] = [] {
        // `mcpp:include-dir=` adds a private include directory (here a header the program wrote
        // under MCPP_OUT_DIR); `mcpp:include-dir-after=` reaches the compile edges as -idirafter;
        // both survive the directive cache.
        s::project p{{"build.mcpp", R"cpp(#include <cstdio>
#include <cstdlib>
#include <string>
#include <fstream>
#include <filesystem>
import mcpp;
int main() {
    std::string inc = std::string(mcpp::out_dir()) + "/inc";
    std::filesystem::create_directories(inc);
    std::ofstream(inc + "/bp_config.h") << "#define BP_ANSWER 42\n";
    mcpp::include_dir(inc.c_str());
    mcpp::include_dir_after("vendor/sysinc");
    return 0;
}
)cpp"},
                     {"src/main.cpp", "import std;\n#include <bp_config.h>\nint main() {\n    std::println(\"ANSWER={}\", BP_ANSWER);\n    return 0;\n}\n"},
                     {"mcpp.toml", "[package]\nname    = \"incdir\"\nversion = \"0.1.0\"\n\n[modules]\nsources = [\"src/**/*.cpp\"]\n\n[targets.incdir]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}};
        fs::create_directories(p.at("vendor/sysinc"));
        auto b1 = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(b1))) << "build failed";
        expect(s::lacks(s::text(b1), "ignoring unknown directive")) << "include-dir directive not recognized";
        expect(s::has_line(s::text(p.mcpp({"run"})), "^ANSWER=42$")) << "include dir not on the -I chain";
        std::optional<fs::path> ninja;
        for (const auto& f : s::find_files(p.at("target"), R"(build\.ninja)"))
            if (!ninja && !f.generic_string().contains("/.build-mcpp/")) ninja = f;
        expect(fatal(ninja.has_value()));
        expect(s::has_line(s::read_file(*ninja), "-idirafter.*vendor/sysinc")) << "include-dir-after not emitted as -idirafter in build.ninja";
        p.write("src/main.cpp", "import std;\n#include <bp_config.h>\nint main() {\n    std::println(\"ANSWER2={}\", BP_ANSWER);\n    return 0;\n}\n");
        auto b2 = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(b2))) << "rebuild failed";
        expect(s::no_line(s::text(b2), ran_again)) << "unchanged build.mcpp re-ran";
        expect(s::has_line(s::text(p.mcpp({"run"})), "^ANSWER2=42$")) << "cached include-dir record lost";
    };

    "181 build mcpp import std"_test[s::unix_shell] = [] {
        // build.mcpp can `import std;`: alone, beside `import mcpp;`, and an #include-only program
        // that mentions `import std;` in a comment still takes the plain path.
        auto p = s::project::scaffold("imp_std");
        auto step = [&](std::string_view build, std::string_view main, std::string_view said) {
            p.write("build.mcpp", build);
            p.write("src/main.cpp", main);
            auto b = p.mcpp({"build"});
            expect(fatal(e2e::succeeded(b))) << "build: " << said;
            auto r = p.mcpp({"run"});
            expect(e2e::succeeded(r)) << "run: " << said;
            expect(e2e::contains(s::text(r), said));
        };
        step(R"cpp(import std;
int main() {
    std::vector<std::string> defines{"MCPP_FROM_IMPORT_STD", "MCPP_STD_COUNT_2"};
    std::ranges::sort(defines);
    for (auto const& d : defines) std::println("mcpp:cfg={}", d);
    std::println("mcpp:rerun-if-changed=build.mcpp");
    return 0;
}
)cpp", R"cpp(import std;
int main() {
#if defined(MCPP_FROM_IMPORT_STD) && defined(MCPP_STD_COUNT_2)
    std::println("import-std-ok");
    return 0;
#else
    std::println("defines missing");
    return 1;
#endif
}
)cpp", "import-std-ok");
        step(R"cpp(import std;
import mcpp;
int main() {
    std::string tag = std::format("MCPP_BOTH_{}", 1 + 1);
    mcpp::define(tag.c_str());
    mcpp::rerun_if_changed("build.mcpp");
    return 0;
}
)cpp", R"cpp(import std;
int main() {
#ifdef MCPP_BOTH_2
    std::println("both-modules-ok");
    return 0;
#else
    std::println("define missing");
    return 1;
#endif
}
)cpp", "both-modules-ok");
        step(R"cpp(#include <cstdio>
// This program deliberately mentions import std; in a comment.
int main() {
    std::puts("mcpp:cfg=MCPP_PLAIN_PATH");
    std::puts("mcpp:rerun-if-changed=build.mcpp");
    return 0;
}
)cpp", R"cpp(import std;
int main() {
#ifdef MCPP_PLAIN_PATH
    std::println("plain-path-ok");
    return 0;
#else
    return 1;
#endif
}
)cpp", "plain-path-ok");
    };

    "186 build mcpp protocol and bound"_test[s::gcc] = [] {
        // The contract's hardening: a program that announces a protocol has an unknown directive
        // refused, a printf program keeps warn-and-ignore, a newer protocol is refused with an
        // upgrade hint, a hanging program is bounded, and the cache's epoch guards its replay.
        const std::string manifest = "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n";
        s::project p{{"mcpp.toml", manifest},
                     {"src/main.cpp", "#ifndef FROM_BUILD_MCPP\n#error \"define missing\"\n#endif\nint main() { return 0; }\n"}};
        auto fresh = [&] { p.remove("target"); };
        auto failed = [](const e2e::outcome& r) { return r.started && !r.timed_out && r.status != 0; };

        // 1. `import mcpp;` announces a protocol: an unknown directive is fatal, and says why.
        p.write("build.mcpp", "#include <cstdio>\nimport mcpp;\nint main() {\n    mcpp::cxxflag(\"-DFROM_BUILD_MCPP=1\");\n    std::printf(\"mcpp:no-such-directive=1\\n\");\n}\n");
        fresh();
        auto b1 = p.mcpp({"build"});
        expect(failed(b1)) << "unknown directive from an announcing program was accepted";
        expect(e2e::contains(s::text(b1), "no-such-directive")) << "error does not name the offending directive";
        expect(s::contains_nocase(s::text(b1), "protocol")) << "error does not mention the protocol";

        // 2. A hand-written printf program keeps warn-and-ignore.
        p.write("build.mcpp", "#include <cstdio>\nint main() {\n    std::printf(\"mcpp:cxxflag=-DFROM_BUILD_MCPP=1\\n\");\n    std::printf(\"mcpp:no-such-directive=1\\n\");\n}\n");
        fresh();
        auto b2 = p.mcpp({"build"});
        expect(e2e::succeeded(b2)) << "legacy printf program was rejected";
        expect(e2e::contains(s::text(b2), "ignoring unknown directive")) << "legacy program lost its warn-and-ignore behaviour";

        // 3. A newer protocol is refused with an upgrade hint.
        p.write("build.mcpp", "#include <cstdio>\nint main() {\n    std::printf(\"mcpp:protocol=999\\n\");\n    std::printf(\"mcpp:cxxflag=-DFROM_BUILD_MCPP=1\\n\");\n}\n");
        fresh();
        auto b3 = p.mcpp({"build"});
        expect(failed(b3)) << "a newer-protocol program was accepted";
        expect(e2e::contains(s::text(b3), "999")) << "error does not name the claimed protocol";
        expect(s::contains_nocase(s::text(b3), "upgrade")) << "error gives no actionable next step";

        // 4. A hanging program is killed; the error names the package, the variable and the manifest.
        p.write("build.mcpp", "#include <cstdio>\n#include <unistd.h>\nint main() {\n    std::printf(\"mcpp:cxxflag=-DFROM_BUILD_MCPP=1\\n\");\n    std::fflush(stdout);\n    for (;;) ::sleep(60);\n}\n");
        fresh();
        auto b4 = p.mcpp({"build"}, {.env = {{"MCPP_BUILD_PROGRAM_TIMEOUT", "3"}}});
        expect(failed(b4)) << "a hanging build.mcpp did not fail the build";
        expect(lt(b4.duration, std::chrono::milliseconds(std::chrono::seconds(60)))) << "the bound did not fire";
        expect(e2e::contains(s::text(b4), "time limit")) << "no timeout diagnostic";
        expect(e2e::contains(s::text(b4), "'app'")) << "timeout error does not name the package";
        expect(e2e::contains(s::text(b4), "MCPP_BUILD_PROGRAM_TIMEOUT")) << "timeout error does not say how to change the bound";
        expect(e2e::contains(s::text(b4), "mcpp.toml")) << "timeout error does not name the manifest to edit";

        // 4b. The manifest's bound is read; the variable overrides it; a negative one is refused.
        p.write("mcpp.toml", manifest + "\n[build]\nbuild_program_timeout = 2\n");
        fresh();
        auto b4b = p.mcpp({"build"});
        expect(failed(b4b)) << "[build] build_program_timeout did not bound the run";
        expect(lt(b4b.duration, std::chrono::milliseconds(std::chrono::seconds(60)))) << "the manifest bound did not fire";
        expect(e2e::contains(s::text(b4b), "exceeded its 2s time limit")) << "the manifest value is not the effective bound";
        fresh();
        auto b4c = p.mcpp({"build"}, {.env = {{"MCPP_BUILD_PROGRAM_TIMEOUT", "1"}}});
        expect(failed(b4c)) << "build succeeded under a 1s bound";
        expect(e2e::contains(s::text(b4c), "exceeded its 1s time limit")) << "MCPP_BUILD_PROGRAM_TIMEOUT did not override the manifest";
        p.write("mcpp.toml", manifest + "\n[build]\nbuild_program_timeout = -1\n");
        auto b4d = p.mcpp({"build"});
        expect(failed(b4d)) << "a negative build_program_timeout was accepted";
        expect(e2e::contains(s::text(b4d), "build_program_timeout")) << "the error does not name the key";
        p.write("mcpp.toml", manifest);

        // 5. The cache entry carries an epoch; a foreign epoch, or a record this mcpp cannot read,
        // runs the program again.
        p.write("build.mcpp", "#include <cstdio>\nint main() { std::printf(\"mcpp:cxxflag=-DFROM_BUILD_MCPP=1\\n\"); }\n");
        fresh();
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "build failed";
        const std::string cache = "target/.build-mcpp/build.mcpp.cache";
        expect(fatal(p.exists(cache))) << "no build.mcpp cache written";
        const auto good = p.read(cache);
        expect(fatal(s::has_line(good, "^epoch "))) << "cache carries no epoch";
        auto touch = [&] { fs::last_write_time(p.at("src/main.cpp"), fs::file_time_type::clock::now()); };
        touch();
        auto b6 = p.mcpp({"build", "-v"});
        expect(e2e::succeeded(b6));
        expect(s::has_line(s::text(b6), R"(^ *build\.mcpp .* cached)")) << "an unchanged build.mcpp was re-run";
        p.write(cache, std::regex_replace(good, std::regex("(^|\n)epoch [^\n]*"), "$1epoch 987654"));
        touch();
        auto b7 = p.mcpp({"build"});
        expect(e2e::succeeded(b7));
        expect(s::has_line(s::text(b7), ran_again)) << "a foreign cache epoch did not force a re-run";
        p.write(cache, good + "d some-future-tag /x\n");
        touch();
        auto b8 = p.mcpp({"build"});
        expect(e2e::succeeded(b8));
        expect(s::has_line(s::text(b8), ran_again)) << "an unknown cache record did not force a re-run";
    };

    "194 build mcpp glob input"_test[s::gcc] = [] {
        // #359: an input that is a set of files. A file that appears or goes runs the program
        // again (through the build.mcpp cache and the project's fast path); an edited one does
        // not on the glob's account; the program's own outputs are never in the set.
        const std::string program = R"cpp(import std;
import mcpp;
int main() {
    namespace fs = std::filesystem;
    mcpp::rerun_if_changed_glob("inputs/**/*.in");
    std::string root = mcpp::manifest_dir();
    int n = 0;
    std::error_code ec;
    for (auto const& e : fs::recursive_directory_iterator(root + "/inputs", ec))
        if (e.path().extension() == ".in") ++n;
    std::string out = std::string(mcpp::out_dir()) + "/count.cpp";
    { std::ofstream os(out); os << "int count_inputs() { return " << n << "; }\n"; }
    { std::ofstream os(root + "/marker.txt"); os << n; }
    mcpp::generated(out.c_str());
    return 0;
}
)cpp";
        s::project p{{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n"},
                     {"src/main.cpp", "#include <cstdio>\nint count_inputs();\nint main() { std::printf(\"COUNT=%d\\n\", count_inputs()); }\n"},
                     {"build.mcpp", program},
                     {"inputs/a.in", "a\n"}};
        auto ran = [](const e2e::outcome& r) { return s::has_line(s::text(r), ran_again); };
        auto count = [&] {
            std::string last;
            for (const auto& l : s::lines_with(s::text(p.mcpp({"run"})), "COUNT="))
                if (l.starts_with("COUNT=")) last = l;
            return last;
        };
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "first build failed";
        expect(eq(count(), std::string("COUNT=1")));
        // 1. A new matching file.
        p.write("inputs/b.in", "b\n");
        auto b2 = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(b2)));
        expect(ran(b2)) << "adding a matching file did not re-run build.mcpp";
        expect(eq(count(), std::string("COUNT=2")));
        // 2. A removed one.
        p.remove("inputs/b.in");
        auto b3 = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(b3)));
        expect(ran(b3)) << "removing a matching file did not re-run build.mcpp";
        expect(eq(count(), std::string("COUNT=1")));
        // 3. An edited one: the set is unchanged.
        p.write("inputs/a.in", "a totally different body\n");
        auto b4 = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(b4)));
        expect(!ran(b4)) << "editing a matched file's content re-ran the program via the glob";
        // 4. A maximal pattern: the program's outputs (marker.txt, target/) are not in the set.
        p.write("build.mcpp", std::regex_replace(program, std::regex(R"("inputs/\*\*/\*\.in")"), "\"**\""));
        auto b5 = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(b5))) << "wide-glob build failed";
        expect(ran(b5)) << "the edited program did not run";
        auto b6 = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(b6))) << "second wide-glob build failed";
        expect(!ran(b6)) << "a wide glob re-runs forever — the build output tree is in its set";
    };

    "612 build program file input fast path"_test[s::gcc] = [] {
        // A file declared with rerun_if_changed is compared on the fast path (a data file is not
        // a source, so the mtime sweep cannot see it); an unchanged one still takes the fast path.
        // The reading is the program's output.
        auto p = s::project::scaffold("probe");
        for (const auto& m : s::glob(p.at("src"), "*.cppm")) fs::remove(m);
        p.write("data/value.txt", "first\n");
        p.write("mcpp.toml", "[package]\nname = \"probe\"\nversion = \"0.1.0\"\n\n[language]\nstandard = \"c++23\"\nmodules = true\nimport_std = true\n\n[targets.probe]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n");
        p.write("build.mcpp", R"cpp(import std;
import mcpp;
int main() {
    const std::filesystem::path root(mcpp::manifest_dir());
    const std::filesystem::path gen(std::string(mcpp::out_dir()) + "/include");
    std::error_code ec;
    std::filesystem::create_directories(gen, ec);
    std::ifstream in(root / "data" / "value.txt");
    std::string value;
    std::getline(in, value);
    std::ofstream out(gen / "value.h", std::ios::trunc);
    out << "#pragma once\n#define PROBE_VALUE \"" << value << "\"\n";
    mcpp::rerun_if_changed((root / "data" / "value.txt").string().c_str());
    mcpp::include_dir(gen.string().c_str());
    return 0;
}
)cpp");
        p.write("src/main.cpp", "#include <cstdio>\n#include \"value.h\"\nint main() { std::printf(\"VALUE=%s\\n\", PROBE_VALUE); }\n");
        auto value = [&] { return s::last_starting(s::text(p.mcpp({"run"})), "VALUE="); };
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "the first build failed";
        expect(eq(value(), std::string("VALUE=first")));
        p.write("data/value.txt", "second\n");
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "the second build failed";
        expect(eq(value(), std::string("VALUE=second"))) << "a declared file input changed and the build replayed the old header";
        // Nothing touched: the second of two builds takes the fast path.
        expect(s::has_line(s::text(p.mcpp({"build"})), "Finished .* in ")) << "the third build did not finish";
        expect(s::lacks(s::text(p.mcpp({"build"})), "Compiling probe")) << "an unchanged input recompiled the project";
    };

    "617 build program knows the cxx stdlib"_test[s::elf][s::gcc] = [] {
        // mcpp::cxx_stdlib(): the C++ standard library the engine resolved (clang answers "clang"
        // with libc++ or libstdc++). The program writes what it read to a file (its stdout is
        // shown only when it fails), compared with resolution.json and with the compiler family.
        s::project p{{"src/main.cpp", "int main() { return 0; }\n"},
                     {"build.mcpp", R"cpp(#include <cstdio>
#include <cstdlib>
#include <string>
import mcpp;
int main() {
    const char* stdlib   = mcpp::cxx_stdlib();
    const char* compiler = mcpp::compiler();
    std::string out = std::string(mcpp::manifest_dir()) + "/answer.txt";
    std::FILE* f = std::fopen(out.c_str(), "w");
    if (f == nullptr) return 1;
    std::fprintf(f, "compiler=%s\nstdlib=%s\n",
                 compiler == nullptr ? "" : compiler,
                 stdlib   == nullptr ? "" : stdlib);
    std::fclose(f);
    return 0;
}
)cpp"},
                     {"mcpp.toml", "[package]\nname    = \"stdlibq\"\nversion = \"0.1.0\"\n\n[targets.stdlibq]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}};
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "build";
        expect(fatal(p.exists("answer.txt"))) << "the build program wrote no answer";
        const auto answer = p.read("answer.txt");
        const auto compiler = s::last_starting(answer, "compiler=").substr(9);
        const auto stdlib = s::last_starting(answer, "stdlib=").substr(7);
        expect(fatal(!stdlib.empty())) << "mcpp::cxx_stdlib() is empty";
        std::optional<fs::path> res;
        for (const auto& f : s::find_files(p.at("target"), R"(resolution\.json)"))
            if (!res && !f.generic_string().contains("/.build-mcpp/")) res = f;
        expect(fatal(res.has_value())) << "no resolution.json under target/";
        std::smatch m;
        const auto doc = s::read_file(*res);
        expect(fatal(std::regex_search(doc, m, std::regex(R"re("stdlib"[ \t]*:[ \t]*"([^"]*)")re"))));
        expect(eq(stdlib, m[1].str())) << "the build program and the engine disagree";
        expect(eq(compiler, std::string("gcc"))) << "expected the gcc toolchain";
        expect(eq(stdlib, std::string("libstdc++"))) << "gcc's C++ standard library is libstdc++";
    };

    "632 a build program is told its package name"_test[s::gcc] = [] {
        // mcpp::package_name() and package_namespace() answer the [package] identity; the
        // directory (`app`) and the package (`vulkan-saxpy`) differ on purpose, or the case would
        // pass against a directory-derived answer. The answer is written to a file.
        s::project p{s::case_dir() / "app",
                     {{"src/main.cpp", "int main() { return 0; }\n"},
                      {"build.mcpp", R"cpp(#include <cstdio>
#include <string>
import mcpp;
int main() {
    std::string out = std::string(mcpp::manifest_dir()) + "/answered.txt";
    std::FILE* f = std::fopen(out.c_str(), "w");
    if (f == nullptr) return 3;
    std::fprintf(f, "name=%s\n", mcpp::package_name());
    std::fprintf(f, "namespace=%s\n", mcpp::package_namespace());
    std::fclose(f);
    return 0;
}
)cpp"},
                      {"mcpp.toml", "[package]\nname      = \"vulkan-saxpy\"\nnamespace = \"example\"\nversion   = \"0.1.0\"\n\n[build]\nsources = [\"src/*.cpp\"]\n\n[targets.vulkan-saxpy]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}}};
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "build";
        expect(fatal(p.exists("answered.txt"))) << "the build program wrote no answer";
        expect(s::has_line(p.read("answered.txt"), "^name=vulkan-saxpy$")) << "package_name() did not answer the [package] name";
        expect(s::has_line(p.read("answered.txt"), "^namespace=example$")) << "package_namespace() did not answer the [package] namespace";
        expect(eq(p.dir().filename().string(), std::string("app"))) << "this fixture no longer distinguishes the two derivations";
    };

    "651 a build program deploys what it generated"_test[s::elf][s::gcc] = [] {
        // mcpp::deploy(from, to) (#622 A4) places a file the program produced beside the artifact:
        // `from` may be an action's absolute output; the copy edge's input is that output (read in
        // build.ninja); a changed input changes it; a cache hit restores it; pack stages it;
        // `to = "../x"` is refused naming the directive and the package; and
        // MCPP_TARGET_MIN_PLATFORM_VERSION (#622 A11) is empty on Linux.
        const std::string build = R"cpp(import mcpp;
#include <cstdio>
#include <string>
int main() {
    const std::string root = mcpp::manifest_dir();
    const std::string src  = root + "/res.txt";
    const std::string out  = std::string(mcpp::out_dir()) + "/gen/res.bin";
    mcpp::action a;
    a.id   = "gen-res";
    a.role = "source";
    a.arg((root + "/gen.sh").c_str()).arg(src.c_str()).arg(out.c_str())
     .input(src.c_str())
     .output(out.c_str())
     .submit();
    mcpp::deploy(out.c_str(), "app.resources");
    if (FILE* f = std::fopen((std::string(mcpp::out_dir()) + "/minplat.txt").c_str(), "w")) {
        std::fputs(mcpp::min_platform_version(), f);
        std::fclose(f);
    }
    return 0;
}
)cpp";
        const std::string manifest = "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[toolchain]\nlinux = \"gcc@16.1.0\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n";
        s::project p{s::case_dir() / "app", {{"mcpp.toml", manifest},
                                          {"src/main.cpp", "#include <cstdio>\nint main() { std::puts(\"app\"); return 0; }\n"},
                                          {"res.txt", "hello resource\n"},
                                          {"build.mcpp", build}}};
        s::write_executable(p.at("gen.sh"), "#!/usr/bin/env bash\nset -e\ncp \"$1\" \"$2\"\n");
        auto touch = [&] { fs::last_write_time(p.at("src/main.cpp"), fs::file_time_type::clock::now()); };
        // 1. A plain build deploys the generated file beside the executable.
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "build failed";
        std::optional<fs::path> deployed;
        for (const auto& f : s::find_files(p.at("target"), R"(res\.bin)"))
            if (!deployed && f.generic_string().ends_with("/bin/app.resources/res.bin")) deployed = f;
        expect(fatal(deployed.has_value())) << "the deployed file does not exist";
        const auto bindir = deployed->parent_path().parent_path();
        expect(eq(s::read_file(*deployed), std::string("hello resource\n"))) << "the deployed file does not carry the action's output";
        auto minplat = s::find_file(p.at("target"), "minplat.txt");
        expect(fatal(minplat.has_value())) << "the program never wrote minplat.txt";
        expect(eq(s::read_file(*minplat), std::string())) << "MCPP_TARGET_MIN_PLATFORM_VERSION was not empty on Linux";
        // 2. The copy edge's input is the action's declared output.
        std::optional<fs::path> graph;
        for (const auto& f : s::find_files(p.at("target"), R"(build\.ninja)"))
            if (!graph && !f.generic_string().contains("/.build-mcpp/")) graph = f;
        expect(fatal(graph.has_value())) << "no build.ninja";
        const auto g = s::read_file(*graph);
        std::string action_out, stage;
        for (const auto& l : s::lines_with(g, " : mcpp_action_"))
            if (action_out.empty() && l.contains("gen/res.bin") && l.starts_with("build "))
                action_out = l.substr(6, l.find(' ', 6) - 6);
        for (const auto& l : s::lines_with(g, "stage_file"))
            if (stage.empty() && l.contains("app.resources/res.bin")) stage = l;
        expect(fatal(!action_out.empty())) << "no build.ninja action edge names gen/res.bin";
        expect(fatal(!stage.empty())) << "no stage_file edge for app.resources/res.bin";
        expect(e2e::contains(stage, action_out)) << "the copy edge's input is not the action's own output path";
        // 3. A changed input changes the deployed file.
        p.write("res.txt", "changed resource\n");
        touch();
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "second build failed";
        expect(eq(s::read_file(*deployed), std::string("changed resource\n"))) << "touching the action's input did not change the deployed file";
        // 4. bin/ deleted, rebuilt on a build.mcpp cache hit.
        fs::remove_all(bindir);
        touch();
        auto b3 = p.mcpp({"build", "-v"});
        expect(fatal(e2e::succeeded(b3))) << "third build failed";
        expect(s::has_line(s::text(b3), R"(^ *build\.mcpp .* cached)")) << "the third build re-ran build.mcpp; the replay path was not exercised";
        expect(fatal(fs::is_regular_file(*deployed))) << "the deployed file was not restored on a build.mcpp cache hit";
        expect(eq(s::read_file(*deployed), std::string("changed resource\n"))) << "the restored file does not carry the action's latest output";
        // 5. pack --pkg-format dir stages it.
        expect(fatal(e2e::succeeded(p.mcpp({"pack", "--pkg-format", "dir"})))) << "pack --pkg-format dir failed";
        auto staged = s::find_file(p.at("target/dist"), "res.bin");
        expect(fatal(staged.has_value())) << "mcpp pack --pkg-format dir did not stage the deployed file";
        expect(staged->generic_string().ends_with("/bin/app.resources/res.bin")) << "the staged file is not at bin/app.resources/res.bin";
        // 6. `to = "../x"` is refused.
        fs::copy(p.dir(), s::case_dir() / "escapes", fs::copy_options::recursive);
        s::project esc{s::case_dir() / "escapes", {}};
        esc.write("mcpp.toml", std::regex_replace(manifest, std::regex("name    = \"app\""), "name    = \"escapes\""));
        esc.write("build.mcpp", std::regex_replace(build, std::regex(R"re(mcpp::deploy\(out\.c_str\(\), "app\.resources"\))re"), "mcpp::deploy(out.c_str(), \"../x\")"));
        auto b5 = esc.mcpp({"build"});
        expect(b5.started && b5.status != 0) << "a deploy 'to' escaping the executable directory was accepted";
        expect(e2e::contains(s::text(b5), "deploy")) << "the refusal does not name the directive";
        expect(e2e::contains(s::text(b5), "escapes")) << "the refusal does not name the package";
        expect(e2e::contains(s::text(b5), "component")) << "the refusal does not name the path problem";
    };

    "683 a build program reads the payload pkg config view"_test = [] {
        // mcpp::pkg_config_libdir() names the registry SubOS's usr/lib/pkgconfig and
        // usr/share/pkgconfig, in that order (#634 A7); it is an accessor, not an environment
        // default, so the program's environment carries no PKG_CONFIG_LIBDIR.
        s::project p{{"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"},
                     {"src/main.cpp", "int main() { return 0; }\n"},
                     {"build.mcpp", R"cpp(import std;
import mcpp;
int main() {
    std::string line = std::string("libdir=") + mcpp::pkg_config_libdir();
    const char* inherited = std::getenv("PKG_CONFIG_LIBDIR");
    line += std::string(" inherited=") + (inherited ? inherited : "(unset)");
    mcpp::warning(line.c_str());
    return 0;
}
)cpp"}};
        auto b = p.mcpp({"build"}, {.env = {{"PKG_CONFIG_LIBDIR", std::nullopt}}});
        expect(fatal(e2e::succeeded(b))) << "build failed";
        std::smatch m;
        const auto env = p.mcpp({"self", "env"}).out;
        expect(fatal(std::regex_search(env, m, std::regex("(^|\n)xlings home *= *([^\r\n]*)")))) << "cannot read the registry from mcpp self env";
        // `self env` prints the native spelling; the accessor joins generic paths with the list
        // separator, which `.` matches.
        auto registry = std::regex_replace(m[2].str(), std::regex(R"(\\)"), "/");
        const auto quoted = std::regex_replace(registry, std::regex(R"([.^$|()\[\]{}*+?\\])"), R"(\$&)");
        const auto text = s::text(b);
        const auto at = text.find("libdir=");
        expect(fatal(at != std::string::npos)) << "the build program printed no libdir";
        const auto line = text.substr(at, text.find('\n', at) - at);
        expect(std::regex_match(line, std::regex("libdir=" + quoted + "/subos/default/usr/lib/pkgconfig." + quoted +
                                                 "/subos/default/usr/share/pkgconfig inherited=\\(unset\\)\r?")))
            << "A/B: the build program read '" << line << "'";
    };

    "823 mcpp core and mcpp name one interface"_test[s::unix_shell] = [] {
        // #734 E8: the build-program interface is `mcpp.core` (SPEC-007) and `mcpp` its permanent
        // equivalent; a program may import either, or both.
        for (auto [label, imports] : {std::pair{"C1", "import mcpp.core;"}, std::pair{"C2", "import mcpp;"},
                                      std::pair{"C3", "import mcpp.core;\nimport mcpp;"}}) {
            s::project p{s::case_dir() / label, {{"mcpp.toml", "[package]\nname = \"core823\"\nversion = \"0.1.0\"\n"},
                                              {"src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"%d\\n\", CORE823); return 0; }\n"}}};
            p.write("build.mcpp", std::format("{}\nint main() {{\n    mcpp::cxxflag(\"-DCORE823=823\");\n    return 0;\n}}\n", imports));
            expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << label << ": the build failed";
            auto r = p.mcpp({"run"});
            expect(e2e::succeeded(r)) << label << ": the program did not run";
            expect(s::has_line(s::text(r), "^823$")) << label << ": the directive did not take effect";
        }
    };

    "824 a build program reads the resolved toolchain"_test[s::unix_shell] = [] {
        // #734 E2 (protocol 14): the resolved toolchain as facts. B1 tool("cxx") and tool("cc")
        // exist, tool("ld") is the driver; B2 abi_tool equals tool off the MSVC ABI; B3 the MSVC
        // facts are empty; B4 the identity is "<family> <version>" with no path; B5
        // ninja_program() runs; B6 cxx_runtime() follows the manifest.
        s::project p{{"src/main.cpp", "int main() { return 0; }\n"},
                     {"build.mcpp", R"cpp(import std;
import mcpp.core;
int main() {
    std::ofstream out(std::string(mcpp::out_dir()) + "/info.txt");
    for (const char* r : {"cc", "cxx", "ld", "ar", "rc", "as", "mt"})
        out << "tool." << r << "=" << mcpp::tool(r) << "\n"
            << "abi." << r << "=" << mcpp::abi_tool(r) << "\n";
    out << "env=" << mcpp::tool_env() << "\n"
        << "identity=" << mcpp::toolset_identity() << "\n"
        << "instance=" << mcpp::msvc_instance_dir() << "\n"
        << "ninja=" << mcpp::ninja_program() << "\n"
        << "runtime=" << mcpp::cxx_runtime() << "\n"
        << "crt=" << mcpp::msvc_crt_linkage() << "\n";
    return 0;
}
)cpp"},
                     {"mcpp.toml", "[package]\nname = \"info824\"\nversion = \"0.1.0\"\n"}};
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "the build failed";
        auto info = s::find_file(p.at("target"), "info.txt");
        expect(fatal(info.has_value() && fs::file_size(*info) > 0)) << "the build program wrote no info.txt";
        auto val = [](const fs::path& f, std::string_view key) {
            for (const auto& l : s::lines_with(s::read_file(f), "="))
                if (l.starts_with(std::string(key) + "=")) return l.substr(key.size() + 1);
            return std::string();
        };
        for (auto r : {"cxx", "cc"}) {
            const fs::path t = val(*info, std::string("tool.") + r);
            expect(!t.empty() && fs::is_regular_file(t) && (fs::status(t).permissions() & fs::perms::owner_exec) != fs::perms::none)
                << "B1: tool(" << r << ") is not an executable file";
        }
        expect(eq(val(*info, "tool.ld"), val(*info, "tool.cxx"))) << "B1: tool(ld) is not the driver";
        for (std::string r : {"cc", "cxx", "ld", "ar", "rc", "as", "mt"})
            expect(eq(val(*info, "abi." + r), val(*info, "tool." + r))) << "B2: abi_tool(" << r << ") differs from tool(" << r << ") off the MSVC ABI";
        for (auto k : {"env", "instance", "crt"})
            expect(eq(val(*info, k), std::string())) << "B3: " << k << " is not empty off the MSVC ABI";
        const auto id = val(*info, "identity");
        expect(e2e::matches(id, "^(clang|gcc) [0-9]")) << "B4: identity '" << id << "' is not '<family> <version>'";
        expect(s::lacks(id, "/")) << "B4: identity holds a path";
        expect(e2e::succeeded(s::run(val(*info, "ninja"), {"--version"}))) << "B5: ninja_program() does not run";
        expect(eq(val(*info, "runtime"), std::string("self-contained"))) << "B6: the default contract";
        const auto first = fs::last_write_time(*info);
        p.write("mcpp.toml", "[package]\nname = \"info824\"\nversion = \"0.1.0\"\n\n[build]\ncxx_runtime = \"host-coupled\"\n");
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "the second build failed";
        // The file written by this build (one newer than the first), else the one there is.
        std::optional<fs::path> again;
        for (const auto& f : s::find_files(p.at("target"), R"(info\.txt)"))
            if (!again && fs::last_write_time(f) > first) again = f;
        if (!again) again = s::find_file(p.at("target"), "info.txt");
        expect(eq(val(*again, "runtime"), std::string("host-coupled"))) << "B6: the stated contract";
    };

    "958 a build program runs again when its output is gone"_test[s::gcc] = [] {
        // A cached run is replayed only while what the program wrote is there: with its include
        // directory deleted, a replay would hand the compile an empty directory, so the program
        // runs again. A: a second build replays and compiles nothing; B: output deleted, it runs.
        s::project p{{"build.mcpp", R"cpp(#include <filesystem>
#include <fstream>
#include <string>
import mcpp;
int main() {
    const std::string inc = std::string(mcpp::out_dir()) + "/inc";
    std::filesystem::create_directories(inc);
    std::ofstream(inc + "/generated.h") << "#define GENERATED 7\n";
    mcpp::include_dir(inc.c_str());
    return 0;
}
)cpp"},
                     {"src/main.cpp", "#include <generated.h>\n#include <cstdio>\nint main() { std::printf(\"generated %d\\n\", GENERATED); return 0; }\n"},
                     {"mcpp.toml", "[package]\nname    = \"regen\"\nversion = \"0.1.0\"\n"}};
        auto a1 = p.mcpp({"run"});
        expect(fatal(e2e::succeeded(a1))) << "A: the first build";
        expect(e2e::contains(s::text(a1), "generated 7")) << "A: the header did not arrive";
        auto a2 = p.mcpp({"build", "-v"});
        expect(fatal(e2e::succeeded(a2))) << "A: the second build";
        expect(s::lacks(s::text(a2), "Compiling regen")) << "A: the second build compiled";
        auto out = s::find_file(p.at("target"), "generated.h");
        expect(fatal(out.has_value())) << "B: the generated header is not under target/";
        fs::remove_all(out->parent_path());
        for (const auto& d : s::glob(p.at("target"), "*/*/bin")) fs::remove_all(d);
        for (const auto& d : s::glob(p.at("target"), "*/*/obj")) fs::remove_all(d);
        auto b = p.mcpp({"run", "-v"});
        expect(fatal(e2e::succeeded(b))) << "B: the build after the output was deleted";
        expect(e2e::contains(s::text(b), "generated 7")) << "B: the program did not run again";
    };

    "1036 a context states the engine values in process"_test[s::gcc] = [] {
        // docs/30 "Testing a program's logic": within a mcpp::context the accessors read the
        // values it states, the innermost first, the rest from the context around it or the
        // environment; has_feature and dep_dir answer within them.
        auto p = s::project::scaffold("ctx");
        p.write("build.mcpp", R"cpp(import std;
import mcpp;
int main() {
    const std::string os = mcpp::target_os();
    std::string seen;
    {
        mcpp::context outer({{"MCPP_TARGET_OS", "plan9"}, {"MCPP_FEATURE_FAST", "1"}});
        seen += mcpp::target_os();
        seen += mcpp::has_feature("fast") ? " fast" : " slow";
        {
            mcpp::context inner({{"MCPP_PROFILE", "inner"}, {"MCPP_DEP_ZLIB_DIR", "/z"}});
            seen += std::format(" {} {} {}", mcpp::target_os(), mcpp::profile(), mcpp::dep_dir("zlib"));
        }
        seen += std::format(" {}", std::string(mcpp::profile()) == "inner" ? "leaked" : "restored");
    }
    seen += std::string(mcpp::target_os()) == os ? " environment" : " leaked";
    std::ofstream("src/seen.cpp") << "extern \"C\" const char* seen() { return \"" << seen << "\"; }\n";
    mcpp::generated("src/seen.cpp");
    return 0;
}
)cpp");
        p.write("src/main.cpp", "import std;\nextern \"C\" const char* seen();\nint main() { std::println(\"{}\", seen()); return 0; }\n");
        auto r = p.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "the build or the program failed";
        expect(e2e::contains(s::text(r), "plan9 fast plan9 inner /z restored environment")) << "the values in scope";
    };
}};

}  // namespace
