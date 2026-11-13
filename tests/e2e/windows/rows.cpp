// Resources on each kind of Windows row: a GNU-ABI row compiles them with the tool its toolchain
// carries and reads "winres.h" from the toolchain's headers; a script's quoted include that nothing
// provides is the script's own matter; a row whose system the graph supplies reads the Windows
// headers the platform declares (formerly tests/e2e/1058, 1072, 1075).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

constexpr std::string_view verinfo_rc = "#include \"winres.h\"\n1 VERSIONINFO\nFILEVERSION 1,2,3,4\nBEGIN\n  BLOCK \"StringFileInfo\"\n  BEGIN\n    BLOCK \"040904b0\"\n    BEGIN\n      VALUE \"ProductName\", \"verinfo\"\n    END\n  END\nEND\n";

std::string manifest(std::string_view name) {
    return std::format("[package]\nname    = \"{0}\"\nversion = \"0.1.0\"\n\n[resources]\nfiles = [\"app.rc\"]\n\n[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.c\"\n", name);
}

// The program `name`.exe under target/<triple>, whose resources hold a VERSIONINFO, read by the
// llvm payload's llvm-readobj (the build's tools, not the host's).
void carries_versioninfo(const s::project& p, std::string_view triple, std::string_view name) {
    const auto exe = s::find_file(p.at("target") / std::string(triple), std::format("{}.exe", name));
    expect(fatal(exe.has_value())) << "no program under target/" << triple;
    const auto readobj = s::payload_root("xim-x-llvm", s::llvm_version()) / "bin" / s::exe("llvm-readobj");
    expect(fatal(fs::exists(readobj))) << "no llvm-readobj in the llvm payload (" << readobj.string() << ")";
    const auto r = s::run(readobj, {"--coff-resources", exe->string()});
    expect(fatal(e2e::succeeded(r) && e2e::contains(s::text(r), "VERSIONINFO"))) << "the program carries no VERSIONINFO: " << s::text(r);
}

E2E rows{"rows", [] {
    "1058 a gnu abi windows row compiles its resources"_test[s::mingw_cross][s::llvm] = [] {
        // llvm paired with MinGW-w64 finds the payload's windres (or llvm-rc beside clang) and the
        // payload's "winres.h"; the cross gcc of the `mingw` provider reads "winres.h" from its own
        // payload, not as a file of the project no action produces (design 2026-11-10 E3;
        // mcpp#911 N9).
        s::project p{{"mcpp.toml", manifest("verinfo")}, {"src/main.c", "int main(void) { return 0; }\n"}, {"app.rc", verinfo_rc}};
        auto r = p.mcpp({"build", "--target", "x86_64-windows-gnu", "--toolchain", "llvm@" + s::llvm_version()});
        expect(fatal(e2e::succeeded(r))) << "[resources] did not build on x86_64-windows-gnu with llvm: " << s::text(r);
        carries_versioninfo(p, "x86_64-windows-gnu", "verinfo");
        fs::remove_all(p.at("target"));
        r = p.mcpp({"build", "--target", "x86_64-windows-gnu"});
        expect(fatal(e2e::succeeded(r))) << "[resources] did not build on x86_64-windows-gnu with the cross gcc: " << s::text(r);
        carries_versioninfo(p, "x86_64-windows-gnu", "verinfo");
    };

    "1072 a resource include nothing provides is the scripts own"_test[s::mingw_cross] = [] {
        // A quoted include no directory holds and no action writes is left to the script's
        // preprocessing: under `__has_include` the script builds; unconditionally, the resource
        // compiler refuses it naming the script, not ninja naming a missing graph input.
        s::project p{{"mcpp.toml", manifest("rcinc")}, {"src/main.c", "int main(void) { return 0; }\n"},
                     {"app.rc", "#if __has_include(\"absent.h\")\n#include \"absent.h\"\n#endif\n1 VERSIONINFO\nFILEVERSION 1,0,0,0\nBEGIN\nEND\n"}};
        auto r = p.mcpp({"build", "--target", "x86_64-windows-gnu"});
        expect(fatal(e2e::succeeded(r))) << "a script that includes an absent header under __has_include did not build: " << s::text(r);
        expect(fatal(s::find_file(p.at("target/x86_64-windows-gnu"), "rcinc.exe").has_value())) << "no program";
        p.write("app.rc", "#include \"absent.h\"\n1 VERSIONINFO\nFILEVERSION 1,0,0,0\nBEGIN\nEND\n");
        r = p.mcpp({"build", "--target", "x86_64-windows-gnu"});
        expect(fatal(failed(r))) << "an absent header included unconditionally built: " << s::text(r);
        expect(fatal(!e2e::contains(s::text(r), "ninja: error"))) << "the build failed in ninja, not in the resource compiler: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "app.rc"))) << "the failure does not name the script: " << s::text(r);
    };

    "1075 a graph supplied windows row finds its resource headers"_test[s::llvm] = [] {
        // x86_64-windows-musl takes its system from the graph, so its toolchain has no paired
        // payload and its llvm no Windows headers; "winres.h" is read from the MinGW-w64 sysroot
        // payload a platform row declares for the architecture's GNU-ABI identity (2026.11.11.1,
        // G3).
        s::project p{{"mcpp.toml", manifest("verinfo")}, {"src/main.c", "int main(void) { return 0; }\n"}, {"app.rc", verinfo_rc}};
        const auto r = p.mcpp({"build", "--target", "x86_64-windows-musl", "--toolchain", "llvm@" + s::llvm_version()});
        expect(fatal(e2e::succeeded(r))) << "[resources] did not build on x86_64-windows-musl: " << s::text(r);
        carries_versioninfo(p, "x86_64-windows-musl", "verinfo");
    };
}};

}  // namespace
