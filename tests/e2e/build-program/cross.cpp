// A build program under a cross target: it is compiled for and run on the host, and told the
// target (formerly tests/e2e/112).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

E2E cross{"cross", [] {
    "112 build mcpp cross"_test[tag{"requires:gcc"}][tag{"requires:mingw-cross"}] = [] {
        // Under `--target x86_64-windows-gnu` build.mcpp compiles and runs on the host and sees
        // MCPP_TARGET = the cross triple.
        s::needs({"gcc", "mingw-cross"});
        auto p = s::project::scaffold("crossbp");
        p.write("build.mcpp", R"cpp(#include <cstdio>
#include <cstdlib>
#include <fstream>
static const char* env_or(const char* n) { const char* v = std::getenv(n); return v ? v : "<unset>"; }
int main() {
    std::ofstream f("src/cross_gen.cpp");
    f << "extern \"C\" const char* bp_target() { return \"" << env_or("MCPP_TARGET") << "\"; }\n";
    if (!f) return 1;
    std::printf("mcpp:generated=src/cross_gen.cpp\n");
    return 0;
}
)cpp");
        p.write("src/main.cpp", "import std;\nextern \"C\" const char* bp_target();\nint main() {\n"
                                "    std::println(\"bp target = {}\", bp_target());\n    return 0;\n}\n");
        p.write("mcpp.toml", "[package]\nname    = \"crossbp\"\nversion = \"0.1.0\"\n");
        auto b = p.mcpp({"build", "--target", "x86_64-windows-gnu"});
        expect(fatal(e2e::succeeded(b))) << "cross build failed";
        expect(!s::contains_nocase(s::text(b), "skipped under a cross")) << "build.mcpp still skipped under cross";
        expect(e2e::contains(p.read("src/cross_gen.cpp"), "x86_64-windows-gnu")) << "MCPP_TARGET was not the cross triple";
        auto exe = s::find_file(p.at("target"), "crossbp.exe");
        expect(fatal(exe.has_value())) << "no PE artifact produced";
        // Under wine, when the machine has it, the loop is whole.
        if (s::has("wine")) {
            auto w = e2e::run({.argv = {"wine", exe->string()}, .env = {{"WINEDEBUG", "-all"}}});
            auto out = s::last_line(w.out);
            if (out.ends_with('\r')) out.pop_back();
            expect(eq(out, std::string("bp target = x86_64-windows-gnu"))) << "unexpected wine output";
        }

        // The std module staged for a build.mcpp is the host's: a helper built against the
        // target's could not run here, and the failure would be silent until it ran.
        p.write("build.mcpp", R"cpp(import std;
int main() {
    std::ofstream f("src/cross_gen.cpp");
    f << "extern \"C\" const char* bp_target() { return \""
      << (std::getenv("MCPP_TARGET") ? std::getenv("MCPP_TARGET") : "<unset>")
      << "\"; }\n";
    if (!f) return 1;
    std::println("mcpp:generated=src/cross_gen.cpp");
    return 0;
}
)cpp");
        p.remove("src/cross_gen.cpp");
        auto b2 = p.mcpp({"build", "--target", "x86_64-windows-gnu"});
        expect(fatal(e2e::succeeded(b2))) << "cross build with import std in build.mcpp failed";
        // The helper ran on the host: the file it was asked to write is there.
        expect(fatal(p.exists("src/cross_gen.cpp"))) << "import-std build.mcpp did not run on the host";
        expect(e2e::contains(p.read("src/cross_gen.cpp"), "x86_64-windows-gnu")) << "MCPP_TARGET wrong under import std";
    };

    "168 build mcpp musl host static"_test[tag{"requires:elf"}] = [] {
        // Under a musl host toolchain the build program, which the system executes directly, is
        // static: a dynamic musl program names /lib/ld-musl-<arch>.so.1, which no payload installs,
        // and dies as an unexplained 127 (#295). The newest installed musl payload is the one a
        // user gets; none installed is a skip (this case never installs ~200 MB).
        s::needs({"elf"});
        auto musl = s::newest_payload("xim-x-musl-gcc", "bin/*-linux-musl-g++");
        if (!musl)
            skip("no musl-gcc payload installed — not testing the musl host helper");
        auto p = s::project::scaffold("app");
        p.write("mcpp.toml", p.read("mcpp.toml") + std::format("\n[toolchain]\ndefault = \"gcc@{}-musl\"\n", *musl));
        p.write("build.mcpp", R"cpp(#include <cstdio>
int main() {
    FILE* marker = std::fopen("helper-ran", "w");
    if (!marker) return 2;
    std::fputs("ok\n", marker);
    std::fclose(marker);
    std::puts("mcpp:rerun-if-changed=build.mcpp");
    return 0;
}
)cpp");
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "musl build.mcpp helper did not run on this host";
        expect(p.exists("helper-ran")) << "build.mcpp did not run";
        const auto helper = p.at("target/.build-mcpp/build.mcpp.bin");
        expect(fatal(fs::is_regular_file(helper))) << "helper binary missing";
        // `file`'s "statically linked" and readelf's interpreter, read from the header.
        const auto h = s::read_header(helper);
        expect(eq(h.kind, s::format::elf));
        expect(h.needed.empty()) << "musl host helper is not statically linked";
        expect(!h.elf_interpreter) << "musl host helper contains PT_INTERP: " << h.interpreter;
    };

    "1051 a cross targets toolchain section is not the build programs"_test[tag{"requires:elf"}][tag{"requires:llvm"}][tag{"requires:gcc"}][tag{"requires:musl"}] = [] {
        // `[target.<t>] toolchain` names the target's compiler, not the build program's: under
        // --target x86_64-linux-musl the program is compiled by [toolchain] (llvm) and runs on
        // this machine, the target by the section's gcc (on Windows the program was compiled by
        // the musl cross gcc and could not start).
        s::needs({"elf", "llvm", "gcc", "musl"});
        if (!s::has("x86_64-host"))
            skip("an x86_64 host");
        s::project p{{"mcpp.toml", std::format("[package]\nname    = \"crossprog\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"llvm@{}\"\n\n[target.x86_64-linux-musl]\ntoolchain = \"gcc@16.1.0\"\n", s::llvm_version())},
                     {"build.mcpp", "import std;\nimport mcpp;\nint main() {\n    std::ofstream(std::string(mcpp::out_dir()) + \"/ran.txt\") << \"ran\\n\";\n}\n"},
                     {"src/main.cpp", "int main() { return 0; }\n"}};
        auto r = p.mcpp({"build", "--target", "x86_64-linux-musl"});
        const auto out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "the cross build failed";
        expect(out.contains("host toolchain for build.mcpp: llvm") || s::has_line(out, "Resolved host toolchain for build.mcpp: clang"))
            << "the build program is not compiled by the [toolchain] compiler";
        expect(e2e::contains(out, "x86_64-linux-musl-g++")) << "the target is not compiled by the section's gcc";
        expect(s::find_file(p.at("target"), "ran.txt").has_value()) << "the build program did not run";
    };
}};

}  // namespace
