// What the module scanner reads: the walk pruned at `.mcpp`, defines that reach the scan, a
// private module fragment, a byte-order mark, an implementation unit in a module file, comments
// and raw strings, scan overrides of files a build excludes (formerly tests/e2e/113, 167, 634,
// 635, 636, 639, 1064).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

std::optional<fs::path> graph_of(const fs::path& root) {
    auto all = s::find_files(root / "target", R"(build\.ninja)");
    std::ranges::sort(all);
    for (const auto& f : all)
        if (!f.generic_string().contains("/.build-mcpp/")) return f;
    return std::nullopt;
}

// The first executable regular file under target named `name` (`find -type f -name <name> -perm -u+x`).
std::optional<fs::path> program(const s::project& p, std::string_view name) {
    auto all = s::find_files(p.at("target"), std::string(name));
    std::ranges::sort(all);
    for (const auto& f : all)
        if ((fs::status(f).permissions() & fs::perms::owner_exec) != fs::perms::none) return f;
    return std::nullopt;
}

E2E scanner{"scanner", [] {
    "113 scanner mcpp dir prune"_test[tag{"requires:gcc"}] = [] {
        // `.mcpp` is pruned from a glob walk: a file reachable only through its index link is not
        // compiled; a file whose name has no narrow spelling does not abort the build.
        s::needs({"gcc"});
        s::project root({{"outside/sub/extra.cpp", "int main() { return 9; }   // double-defines main if the walk escapes\n"},
                         {"outside/sub/问题反馈.cpp", "int cjk_never_compiled() { return 2; }\n"},
                         {"proj/src/main.cpp", "int main() { return 0; }\n"},
                         {"proj/mcpp.toml", "[package]\nname = \"prunetest\"\nversion = \"0.1.0\"\n[build]\nsources = [\"**/*.cpp\"]\n"}});
        fs::create_directories(root.at("proj/.mcpp/.xlings/data"));
        fs::create_directory_symlink(root.at("outside"), root.at("proj/.mcpp/.xlings/data/compat"));
        auto r = root.mcpp_in("proj", {"build"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        r = root.mcpp_in("proj", {"run"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
    };

    "167 build defines module scan"_test = [] {
        // [build].defines reach the scan and the compile of module and ordinary units (a value,
        // a value with a space), a cfg section adds to them, a dependency keeps its own, and an
        // unknown [build] key warns (and is refused under --strict).
        s::project root({{"scan/src/lib.cppm", "export module lib;\nexport int lib_value() { return 42; }\n"},
                         {"scan/src/mid.cppm", "export module mid;\n#ifdef USE_LIB\nimport lib;\n#else\n#error \"[build].defines did not reach the module interface unit mid.cppm\"\n#endif\nexport int mid_value() { return lib_value(); }\n"},
                         {"scan/src/main.cpp", R"(import mid;
// The same macro must reach an ordinary TU, and must carry a value and a
// value CONTAINING A SPACE (the -D quoting path, where the space is part of
// one argv token rather than a token boundary — mcpp#234).
#ifndef USE_LIB
#error "[build].defines did not reach main.cpp"
#endif
#if LEVEL != 3
#error "[build].defines dropped or mangled a NAME=value entry"
#endif
BIGINT wide = 1;
int main() { return (mid_value() == 42 && wide == 1) ? 0 : 1; }
)"},
                         {"scan/mcpp.toml", R"([package]
name    = "scan"
version = "0.1.0"
[build]
defines = ["USE_LIB", "LEVEL=3", "BIGINT=long long"]
[scan_overrides."src/lib.cppm"]
provides = ["lib"]
[scan_overrides."src/mid.cppm"]
provides = ["mid"]
imports  = ["lib"]
[scan_overrides."src/main.cpp"]
imports  = ["mid"]
)"},
                         {"cond/src/main.cpp", R"(#if (defined(COND_LINUX) + defined(COND_MACOS) + defined(COND_WIN)) != 1
#error "exactly one conditional [build].defines entry must apply on any host"
#endif
#ifndef BASE_DEFINE
#error "the unconditional [build].defines entry was lost when a cfg section merged"
#endif
int main() { return 0; }
)"},
                         {"cond/mcpp.toml", R"([package]
name    = "cond"
version = "0.1.0"
[build]
defines = ["BASE_DEFINE"]
[target.'cfg(linux)'.build]
defines = ["COND_LINUX"]
[target.'cfg(macos)'.build]
defines = ["COND_MACOS"]
[target.'cfg(windows)'.build]
defines = ["COND_WIN"]
)"},
                         {"dep/src/dep.cppm", "export module dep;\n#ifndef DEP_OWN_DEFINE\n#error \"the dependency's own [build].defines did not reach its module unit\"\n#endif\nexport int dep_value() { return 7; }\n"},
                         {"dep/mcpp.toml", "[package]\nname    = \"dep\"\nversion = \"0.1.0\"\n[build]\ndefines = [\"DEP_OWN_DEFINE\"]\n[targets.dep]\nkind = \"lib\"\n"},
                         {"app/src/main.cpp", "import dep;\n// A dependency's defines are PRIVATE build inputs — they must not leak into\n// the consumer (propagating macros is what feature `defines` are for).\n#ifdef DEP_OWN_DEFINE\n#error \"a dependency's [build].defines leaked into the consumer\"\n#endif\nint main() { return dep_value() == 7 ? 0 : 1; }\n"},
                         {"app/mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[dependencies]\ndep = { path = \"../dep\" }\n"},
                         {"unknown/src/main.cpp", "int main() { return 0; }\n"},
                         {"unknown/mcpp.toml", "[package]\nname    = \"unknown\"\nversion = \"0.1.0\"\n[build]\ndefnes = [\"TYPO\"]\n"}});
        auto r = root.mcpp_in("scan", {"build"});
        expect(fatal(e2e::succeeded(r))) << "[build].defines did not reach the scan and/or the compile\n" << s::text(r);
        r = root.mcpp_in("scan", {"run"});
        expect(fatal(e2e::succeeded(r))) << "built binary did not run\n" << s::text(r);
        r = root.mcpp_in("cond", {"build"});
        expect(fatal(e2e::succeeded(r))) << "conditional [build].defines did not reach the TU\n" << s::text(r);
        r = root.mcpp_in("app", {"build"});
        expect(fatal(e2e::succeeded(r))) << "a dependency's [build].defines did not reach its own TUs\n" << s::text(r);
        r = root.mcpp_in("unknown", {"build"});
        expect(fatal(e2e::succeeded(r))) << "an unknown [build] key must warn, not fail the build\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "defnes"))) << "unknown [build] key 'defnes' was dropped in silence\n" << s::text(r);
        r = root.mcpp_in("unknown", {"build", "--strict"});
        expect(fatal(failed(r))) << "--strict must reject an unknown [build] key\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "defnes"))) << "--strict rejection did not name the offending key\n" << s::text(r);
    };

    "634 a private module fragment is not a partition"_test[tag{"requires:gcc"}] = [] {
        // `module : private;` and `module :private;` are scanned as a fragment: no partition, no
        // malformed name, a module edge; the program runs, or the compiler declines at line 4.
        s::needs({"gcc"});
        int k = 0;
        for (std::string_view spelling : {"module : private;", "module :private;"}) {
            s::project p(s::case_dir() / std::format("proj{}", k++),
                         {{"src/main.cpp", "import pf;\nint main() { return pf() == 42 ? 0 : 1; }\n"},
                          {"mcpp.toml", "[package]\nname    = \"pf\"\nversion = \"0.1.0\"\n"}});
            p.write("src/pf.cppm", std::format("export module pf;\nexport int pf();\n\n{}\n\nint pf() {{ return 42; }}\n", spelling));
            auto r = p.mcpp({"build"});
            const auto log = s::text(r);
            expect(fatal(s::lacks(log, "cannot also provide"))) << "[" << spelling << "]: the private module fragment was read as a partition\n" << log;
            expect(fatal(s::lacks(log, "is not a module name"))) << "[" << spelling << "]: the fragment was rejected as a malformed identity\n" << log;
            const auto graph = graph_of(p.dir());
            expect(fatal(graph.has_value())) << "[" << spelling << "]: no build graph was written\n" << log;
            expect(fatal(s::has_line(s::read_file(*graph), R"(cxx_module .*pf\.cppm)"))) << "[" << spelling << "]: no module edge for pf.cppm";
            if (r.status == 0) {
                const auto bin = program(p, "pf");
                expect(fatal(bin.has_value())) << "[" << spelling << "]: build succeeded with no binary";
                expect(fatal(e2e::succeeded(s::run(*bin)))) << "[" << spelling << "]: the program did not return 42";
            } else {
                expect(fatal(e2e::contains(log, "pf.cppm:4"))) << "[" << spelling << "]: build failed, but not at the fragment\n" << log;
                expect(fatal(s::lacks(log, "scanner errors"))) << "[" << spelling << "]: mcpp's own scan refused the file\n" << log;
            }
        }
    };

    "635 a byte order mark does not hide a module"_test[tag{"requires:gcc"}] = [] {
        // A module interface and a manifest that begin with a UTF-8 byte-order mark are read.
        s::needs({"gcc"});
        const std::string bom = "\xEF\xBB\xBF";
        s::project p({{"src/main.cpp", "import bom;\nint main() { return bom() == 42 ? 0 : 1; }\n"},
                      {"mcpp.toml", "[package]\nname    = \"bomtest\"\nversion = \"0.1.0\"\n"}});
        p.write("src/bom.cppm", bom + "export module bom;\nexport int bom() { return 42; }\n");
        expect(fatal(p.read("src/bom.cppm").starts_with(bom))) << "the fixture lost its byte-order mark; this test would pass vacuously";
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "a BOM'd module interface did not build\n" << s::text(r);
        expect(fatal(s::lacks(s::text(r), "imported but not provided"))) << "the module declaration behind the BOM was not seen";
        const auto bin = program(p, "bomtest");
        expect(fatal(bin.has_value())) << "no binary";
        expect(fatal(e2e::succeeded(s::run(*bin)))) << "the program did not return 42";
        p.write("mcpp.toml", bom + "[package]\nname    = \"bomtest\"\nversion = \"0.1.0\"\n");
        p.remove("target");
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "a BOM'd mcpp.toml was not read\n" << s::text(r);
    };

    "636 a module extension file need not provide a module"_test[tag{"requires:gcc"}] = [] {
        // An implementation unit in a .cppm builds; every module edge states its language; no
        // empty -fmodule-output= reaches the graph.
        s::needs({"gcc"});
        s::project p({{"src/foo.cppm", "export module foo;\nexport int foo();\n"},
                      {"src/foo_impl.cppm", "module foo;\nint foo() { return 5; }\n"},
                      {"src/main.cpp", "import foo;\nint main() { return foo() == 5 ? 0 : 1; }\n"},
                      {"mcpp.toml", "[package]\nname    = \"implunit\"\nversion = \"0.1.0\"\n"}});
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "an implementation unit in a .cppm did not build\n" << s::text(r);
        const auto bin = program(p, "implunit");
        expect(fatal(bin.has_value())) << "no binary";
        expect(fatal(e2e::succeeded(s::run(*bin)))) << "the program did not return 5";
        const auto graph = graph_of(p.dir());
        expect(fatal(graph.has_value())) << "no build graph";
        const auto text = s::read_file(*graph);
        const auto edges = s::count_lines(text, " : cxx_module ");
        const auto langs = s::count_matching(text, "^  module_lang =");
        expect(fatal(edges >= 2u)) << "expected at least 2 module edges, found " << edges;
        expect(fatal(edges == langs)) << edges << " module edges but " << langs << " stated a language";
        expect(fatal(s::no_line(text, R"(-fmodule-output=(\s|$))"))) << "an empty -fmodule-output= reached the graph";
    };

    "639 the scanner does not read inside a comment"_test[tag{"requires:gcc"}] = [] {
        // A comment's body is neither a module declaration nor an import, a comment opens no raw
        // string, code after a closed comment is code, a raw string's body is not code, and no
        // BMI edge exists for a module named only in a comment.
        s::needs({"gcc"});
        int n = 0;
        const auto sees_import = [&](std::string_view source) {
            s::project p(s::case_dir() / std::format("s{}", n++),
                         {{"mcpp.toml", "[package]\nname = \"s\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", source}});
            return s::text(p.mcpp({"build"})).contains("imported but not provided");
        };
        s::project refusal(s::case_dir() / "refusal",
                           {{"mcpp.toml", "[package]\nname = \"r\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", "/*\n  module (exe)\n*/\nint main() { return 0; }\n"}});
        auto r = refusal.mcpp({"build"});
        expect(e2e::succeeded(r) && s::lacks(s::text(r), "scanner errors")) << "the four-line file from the issue was refused:\n" << s::text(r);
        expect(!sees_import("/*\n  import x;\n*/\nint main() { return 0; }\n")) << "a block comment's body is not an import — the scanner recorded an import that is not code";
        expect(sees_import("// R\"(\nimport x;\nint main() { return 0; }\n")) << "a // comment does not open a raw string — the scanner did not record the import";
        expect(sees_import("/*\n  R\"(\n*/\nimport x;\nint main() { return 0; }\n")) << "a block comment does not open a raw string — the scanner did not record the import";
        expect(sees_import("/* */ import x;\nint main() { return 0; }\n")) << "code after a closed block comment is still code — the scanner did not record the import";
        expect(sees_import("const char* s = \"a /* b\";\nimport x;\nint main() { return 0; }\n")) << "a /* inside a string literal opens nothing — the scanner did not record the import";
        expect(!sees_import("const char* s = R\"(\nimport x;\n)\";\nint main() { return 0; }\n")) << "a raw-string body is still not code — the scanner recorded an import that is not code";
        s::project g(s::case_dir() / "graph",
                     {{"mcpp.toml", "[package]\nname = \"g\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", "/*\n  export module y;\n*/\nint main() { return 0; }\n"}});
        (void)g.mcpp({"build"});
        const auto graphs = s::find_files(g.at("target"), R"(build\.ninja)");
        expect(fatal(!graphs.empty())) << "no build.ninja was generated";
        auto first = graphs;
        std::ranges::sort(first);
        expect(s::no_line(s::read_file(first.front()), R"(y\.gcm|y\.pcm|y\.ifc)")) << "the graph names a BMI for 'y', which only a comment mentions";
    };

    "1064 a scan override of a file this build excludes is kept"_test[tag{"requires:llvm"}] = [] {
        // Overrides of a file a feature leaves out and of one a target rule removes are kept;
        // with the feature on the overridden unit builds; a glob naming no file is refused.
        s::needs({"llvm"});
        const auto tc = "llvm@" + s::llvm_version();
        s::project p({{"mcpp.toml", R"([package]
name    = "sorepro"
version = "0.1.0"
[build]
sources = ["src/core.cppm", "src/extra.cppm", "src/only_elsewhere.cppm"]
[features]
default = []
[features.extra]
sources = ["src/extra.cppm"]
[target.'cfg(unix)'.build]
sources = ["!src/only_elsewhere.cppm"]
[scan_overrides."src/extra.cppm"]
provides = ["sorepro.extra"]
imports  = []
[scan_overrides."src/only_elsewhere.cppm"]
provides = ["sorepro.elsewhere"]
imports  = []
[lib]
path = "src/core.cppm"
)"},
                      {"src/core.cppm", "export module sorepro; export int core() { return 1; }\n"},
                      {"src/extra.cppm", "module;\n#ifdef SOREPRO_USE_STD_MODULE\nimport std;\n#endif\nexport module sorepro.extra;\nexport int extra() { return 2; }\n"},
                      {"src/only_elsewhere.cppm", "module;\n#ifdef SOREPRO_USE_STD_MODULE\nimport std;\n#endif\nexport module sorepro.elsewhere;\nexport int elsewhere() { return 3; }\n"}});
        auto r = p.mcpp({"build", "--toolchain", tc});
        expect(fatal(e2e::succeeded(r))) << "the build with the feature off failed\n" << s::text(r);
        r = p.mcpp({"build", "--toolchain", tc, "--features", "extra"});
        expect(fatal(e2e::succeeded(r))) << "the build with the feature on failed\n" << s::text(r);
        p.write("mcpp.toml", p.read("mcpp.toml") + "[scan_overrides.\"src/nowhere.cppm\"]\nprovides = [\"sorepro.nowhere\"]\nimports  = []\n");
        r = p.mcpp({"build", "--toolchain", tc});
        expect(fatal(failed(r))) << "a glob naming no file was accepted\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "scan_overrides glob 'src/nowhere.cppm' matches no file of the package"))) << "the refusal does not name the glob\n" << s::text(r);
    };
}};

}  // namespace
