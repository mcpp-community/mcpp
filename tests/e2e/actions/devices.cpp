// Device sources and the actions that consume them: constrained globs, chained actions, object
// actions in a static library, shader extensions (formerly tests/e2e/606, 607, 608, 609).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

// The build program of 606 and 609: a `check` action per device source (mcpp refuses a device
// source that reached no action), then a warning listing them; `{}` is the warning's argument.
constexpr std::string_view accounting = R"cpp(import std;
import mcpp;
int main() {{
    const char* d = mcpp::device_sources();
    std::string flat(d);
    std::size_t n = 0, start = 0;
    while (start <= flat.size()) {{
        auto nl = flat.find('\n', start);
        auto one = flat.substr(start, nl == std::string::npos ? flat.size() - start : nl - start);
        start = nl == std::string::npos ? flat.size() + 1 : nl + 1;
        if (one.empty()) continue;
        auto stamp = std::string(mcpp::out_dir()) + "/dev-" + std::to_string(n++) + ".stamp";
        mcpp::action a;
        a.id = "seen";
        a.role = "check";
        a.description = "account for a device source";
        auto abs = std::string(mcpp::manifest_dir()) + "/" + one;
        a.arg("cp").arg(abs.c_str()).arg(stamp.c_str());
        a.input(abs.c_str());
        a.output(stamp.c_str());
        a.submit();
    }}
    {}
    return 0;
}}
)cpp";

// `mcpp new <name>` without its module sources.
s::project scaffold(std::string_view name) {
    auto p = s::project::scaffold(name);
    for (const auto& f : s::glob(p.at("src"), "*.cppm")) fs::remove(f);
    return p;
}

E2E devices{"devices", [] {
    "606 constrained source globs narrow to the build"_test[tag{"requires:gcc"}] = [] {
        // A `[build] sources` entry may carry the accel it is for: covered, its C++ compiles and
        // its device sources reach the build program; under --no-accel it is left out; outside
        // the build's accel it is refused naming the glob and both sides (accel-mismatch); a
        // constrained glob matching nothing is refused.
        s::needs({"gcc"});
        auto p = scaffold("narrow");
        p.write("src/main.cpp", "int main() { return 0; }\n");
        p.write("src/dev/helper.cpp", "int widget_helper() { return 42; }\n");
        p.write("src/dev/kernel.cu", "__global__ void k() {}\n");
        auto manifest = [&](std::string_view build, std::string_view glob_accel, std::string_view glob) {
            p.write("mcpp.toml", std::format("[package]\nname = \"narrow\"\nversion = \"0.1.0\"\n[language]\nstandard = \"c++23\"\n[build]\naccel   = \"{}\"\nsources = [\"src/*.cpp\", {{ glob = \"{}\", accel = \"{}\" }}]\n[targets.narrow]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", build, glob, glob_accel));
            p.write("build.mcpp", std::vformat(accounting, std::make_format_args("mcpp::warning((*d ? d : \"(no device sources)\"));")));
        };
        manifest("widget9+{w1,w2}", "widget9+{w1}", "src/dev/**/*");
        auto covered = p.mcpp({"build", "-v"});
        expect(fatal(e2e::succeeded(covered))) << "a covered constraint failed the build";
        expect(e2e::contains(s::text(covered), "helper.cpp")) << "the constrained glob's C++ file was not compiled";
        expect(e2e::contains(s::text(covered), "src/dev/kernel.cu")) << "the device source did not reach the build program";
        auto none = p.mcpp({"build", "-v", "--no-accel"});
        expect(fatal(e2e::succeeded(none))) << "--no-accel failed the build";
        expect(s::lacks(s::text(none), "helper.cpp")) << "the constrained glob was compiled under --no-accel";
        expect(e2e::contains(s::text(none), "(no device sources)")) << "device sources were listed under --no-accel";
        manifest("widget9+{w2}", "widget9+{w1}", "src/dev/**/*");
        auto mismatch = p.mcpp({"build"});
        expect(fatal(failed(mismatch))) << "a constraint outside the build's accel was accepted";
        expect(e2e::contains(s::text(mismatch), "src/dev/**/*")) << "refusal does not name the glob";
        expect(e2e::contains(s::text(mismatch), "widget9+{w1}")) << "refusal does not name the constraint";
        expect(e2e::contains(s::text(mismatch), "widget9+{w2}")) << "refusal does not name what the build targets";
        const auto why = s::json(p.mcpp({"why", "toolchain", "--format", "json"}).out);
        const auto reason = why.contains("data") && why["data"].contains("reason") ? why["data"]["reason"].get<std::string>() : std::string{"-"};
        expect(eq(reason, std::string{"accel-mismatch"})) << "reason is '" << reason << "', expected accel-mismatch";
        manifest("widget9+{w1,w2}", "widget9+{w1}", "src/nowhere/**/*");
        auto empty = p.mcpp({"build"});
        expect(fatal(failed(empty))) << "a constrained glob matching nothing was accepted";
        expect(e2e::contains(s::text(empty), "src/nowhere")) << "refusal does not name the empty glob";
    };

    "607 chained actions form the device link"_test[tag{"requires:gcc"}] = [] {
        // One action consumes what another produced; the engine orders and fingerprints them:
        // an artifact action's output read by an object action whose output joins the link,
        // and an edit at the head of the chain reaches the link.
        s::needs({"gcc"});
        auto p = scaffold("chain");
        p.write("src/main.cpp", "extern \"C\" int joined_value();\nint main() { return joined_value() == 42 ? 0 : 1; }\n");
        p.write("src/parts/a.inc", "extern \"C\" int joined_value() { return\n");
        p.write("src/parts/b.inc", "42; }\n");
        p.write("mcpp.toml", "[package]\nname = \"chain\"\nversion = \"0.1.0\"\n[language]\nstandard = \"c++23\"\n[targets.chain]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n");
        p.write("build.mcpp", R"cpp(import std;
import mcpp;
int main() {
    mcpp::rerun_if_changed("src/parts/a.inc");
    mcpp::rerun_if_changed("src/parts/b.inc");
    const std::string root = mcpp::manifest_dir(), out = mcpp::out_dir();
    const std::string joined = out + "/joined.cpp", obj = out + "/joined.o";
    {   // Step 1: an ARTIFACT. Its output is data as far as the link is concerned.
        mcpp::action a;
        a.id = "join"; a.role = "artifact"; a.description = "join parts";
        a.arg("sh"); a.arg("-c");
        a.arg(("cat '" + root + "/src/parts/a.inc' '" + root + "/src/parts/b.inc' > '" + joined + "'").c_str());
        a.input((root + "/src/parts/a.inc").c_str());
        a.input((root + "/src/parts/b.inc").c_str());
        a.output(joined.c_str());
        a.submit();
    }
    {   // Step 2: an OBJECT that consumes step 1's output. The engine sees a
        // path; ninja orders the two by it.
        mcpp::action a;
        a.id = "compile-joined"; a.role = "object"; a.description = "compile joined";
        a.arg((std::string(mcpp::toolchain_dir()) + "/bin/g++").c_str());
        a.arg("-c"); a.arg(joined.c_str()); a.arg("-o"); a.arg(obj.c_str());
        a.input(joined.c_str());
        a.output(obj.c_str());
        a.submit();
    }
    return 0;
}
)cpp");
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "the chained build failed";
        expect(fatal(e2e::succeeded(p.mcpp({"run"})))) << "the linked object did not carry the joined value";
        p.write("src/parts/b.inc", "43; }\n");
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "rebuild after editing an input failed";
        expect(failed(p.mcpp({"run"}))) << "the edit at the head of the chain did not reach the link";
    };

    "608 object actions reach a static library"_test[tag{"requires:gcc"}] = [] {
        // An `object`-role action's outputs join a static library, not only an executable: the
        // archive holds the member and its symbol.
        s::needs({"gcc"});
        auto p = scaffold("archived");
        p.remove("src/main.cpp");
        p.write("src/host.c", "extern int from_action(void);\nint host_value(void) { return from_action(); }\n");
        p.write("mcpp.toml", "[package]\nname = \"archived\"\nversion = \"0.1.0\"\n[language]\nstandard = \"c++23\"\n[build]\nsources = [\"src/*.c\"]\n[targets.archived]\nkind = \"lib\"\n");
        p.write("build.mcpp", R"cpp(import std;
import mcpp;
int main() {
    const std::string out  = std::string(mcpp::out_dir());
    const std::string src  = out + "/piece.c";
    { std::ofstream f(src, std::ios::trunc);
      f << "int from_action(void) { return 7; }\n"; }
    const std::string obj = out + "/piece.o";
    mcpp::action a;
    a.id = "piece"; a.role = "object"; a.description = "compile the piece";
    a.arg((std::string(mcpp::toolchain_dir()) + "/bin/gcc").c_str());
    a.arg("-c"); a.arg(src.c_str()); a.arg("-o"); a.arg(obj.c_str());
    a.input(src.c_str());
    a.output(obj.c_str());
    a.submit();
    return 0;
}
)cpp");
        auto b = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(b))) << "the build failed";
        expect(s::lacks(s::text(b), "produces no target")) << "the action was dropped for want of a linked image";
        const auto lib = s::find_file(p.at("target"), "libarchived.a");
        expect(fatal(lib.has_value())) << "no static library was produced";
        const auto members = s::archive_members(*lib);
        expect(std::ranges::contains(members, std::string{"piece.o"})) << "piece.o is not a member of the archive";
        expect(std::ranges::contains(s::archive_symbols(*lib), std::string{"from_action"})) << "the archive has the member but not its symbol";
    };

    "609 shader sources are device sources"_test[tag{"requires:gcc"}] = [] {
        // A shader is a device source for every language a separate compiler consumes: all 18
        // extensions reach the build program and none is offered to the C++ compiler; an
        // extension with no role is refused by name; the default globs exclude them.
        s::needs({"gcc"});
        auto p = scaffold("shaders");
        p.write("src/main.cpp", "int main() { return 0; }\n");
        p.write("build.mcpp", std::vformat(accounting, std::make_format_args("for (auto& c : flat) if (c == '\\n') c = ' ';\n    mcpp::warning((\"device=[\" + flat + \"]\").c_str());")));
        auto manifest = [&](std::string_view accel, std::string_view sources) {
            p.write("mcpp.toml", std::format("[package]\nname = \"shaders\"\nversion = \"0.1.0\"\n[language]\nstandard = \"c++23\"\n[build]\n{}\nsources = [{}]\n[targets.shaders]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", accel, sources));
        };
        const std::vector<std::string> exts{"comp", "vert", "frag", "geom", "tesc", "tese", "mesh", "task", "rgen", "rint", "rahit", "rchit", "rmiss", "rcall", "glsl", "hlsl", "cl", "metal"};
        for (const auto& e : exts) p.write(std::format("shaders/s.{}", e), std::format("// {}\n", e));
        manifest("accel   = \"widget9+{w1}\"", "\"src/*.cpp\", { glob = \"shaders/*\", accel = \"widget9+{w1}\" }");
        auto all = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(all))) << "a project with shader sources failed to build";
        std::string missing;
        for (const auto& e : exts)
            if (!e2e::contains(s::text(all), std::format("shaders/s.{}", e))) missing += " " + e;
        expect(missing.empty()) << "not handed to the build program:" << missing;
        const auto verbose = s::text(p.mcpp({"build", "-v"}));
        for (const auto& e : exts)
            expect(!e2e::contains(verbose, std::format("s.{} -o", e)) && !e2e::contains(verbose, std::format("s.{}.o", e))) << "s." << e << " was compiled";
        p.write("shaders/s.wgsl", "// wgsl\n");
        manifest("accel   = \"widget9+{w1}\"", "\"src/*.cpp\", { glob = \"shaders/s.wgsl\", accel = \"widget9+{w1}\" }");
        auto unknown = p.mcpp({"build"});
        expect(fatal(failed(unknown))) << "an extension with no role was accepted";
        expect(e2e::contains(s::text(unknown), "no role for the extension")) << "refusal does not say the extension has no role";
        expect(e2e::contains(s::text(unknown), ".wgsl")) << "refusal does not name the extension";
        p.remove("shaders/s.wgsl");
        p.write("src/shaders/s.comp", p.read("shaders/s.comp"));
        p.write("mcpp.toml", "[package]\nname = \"shaders\"\nversion = \"0.1.0\"\n[language]\nstandard = \"c++23\"\n[targets.shaders]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n");
        auto def = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(def))) << "the default-glob build failed";
        expect(e2e::contains(s::text(def), "device=[]")) << "a shader under src/ reached a build program with no sources entry";
    };

    "611 variant switch is not replayed"_test[tag{"requires:gcc"}] = [] {
        // The device variant is part of the fingerprint: a plain build after `--no-accel`
        // builds and runs the device variant again (the fast path does not replay the last
        // directory), and each graph records the selection that wrote it.
        s::needs({"gcc"});
        auto p = scaffold("probe");
        p.write("mcpp.toml", "[package]\nname = \"probe\"\nversion = \"0.1.0\"\n[language]\nstandard = \"c++23\"\n[build]\naccel = \"widget9+{w1}\"\n[target.'cfg(accelerator = \"widget\")'.build]\ndefines = [\"WIDGET_ON=1\"]\n[targets.probe]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n");
        p.write("src/main.cpp", "#include <cstdio>\n#ifndef WIDGET_ON\n#define WIDGET_ON 0\n#endif\nint main() { std::printf(\"WIDGET=%d\\n\", WIDGET_ON); }\n");
        auto variant = [&](std::vector<std::string> extra = {}) {
            std::vector<std::string> args{"run"};
            args.insert(args.end(), extra.begin(), extra.end());
            return s::last_starting(p.mcpp(args).out, "WIDGET=");
        };
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "the device build failed";
        expect(fatal(eq(variant(), std::string{"WIDGET=1"}))) << "the first build is not the device variant";
        expect(fatal(e2e::succeeded(p.mcpp({"build", "--no-accel"})))) << "--no-accel failed";
        expect(fatal(eq(variant({"--no-accel"}), std::string{"WIDGET=0"}))) << "--no-accel did not produce the CPU variant";
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "the plain build after --no-accel failed";
        expect(eq(variant(), std::string{"WIDGET=1"})) << "a plain build after --no-accel handed back the CPU variant";
        p.mcpp({"build", "--no-accel"});
        expect(eq(variant({"--no-accel"}), std::string{"WIDGET=0"})) << "--no-accel after a plain build is not the CPU variant";
        std::size_t by_default = 0, by_override = 0;
        for (const auto& nj : s::glob(p.at("target"), "*/*/build.ninja")) {
            const auto text = s::read_file(nj);
            by_default += s::has_line(text, "^# mcpp:graph=normal;schedule=[a-z-]*;accel=default") ? 1 : 0;
            by_override += s::has_line(text, "^# mcpp:graph=normal;schedule=[a-z-]*;accel=override") ? 1 : 0;
        }
        expect(by_default == 1 && by_override == 1) << "expected one default and one override graph, got default=" << by_default << " override=" << by_override;
    };

    "613 sycl sources are device sources"_test[tag{"requires:gcc"}] = [] {
        // A SYCL unit is a device source because of the compiler that consumes it: a .sycl unit
        // in a constrained glob reaches the build program and is not compiled by mcpp; the same
        // content named .cpp is C++; --no-accel and the default globs leave .sycl out.
        s::needs({"gcc"});
        auto p = scaffold("sycltest");
        p.write("src/main.cpp", "int main() { return 0; }\n");
        p.write("build.mcpp", std::vformat(accounting, std::make_format_args("for (auto& c : flat) if (c == '\\n') c = ' ';\n    mcpp::warning((\"device=[\" + flat + \"]\").c_str());")));
        auto manifest = [&](std::string_view accel, std::string_view sources) {
            p.write("mcpp.toml", std::format("[package]\nname = \"sycltest\"\nversion = \"0.1.0\"\n[language]\nstandard = \"c++23\"\n[build]\n{}\nsources = [{}]\n[targets.sycltest]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", accel, sources));
        };
        p.write("src/kernels/saxpy.sycl", "// Ordinary C++. What makes this a device unit is which compiler consumes it.\nextern \"C\" int saxpy_device(int n) { return n; }\n");
        manifest("accel   = \"sycl\"", "\"src/*.cpp\", { glob = \"src/kernels/*.sycl\", accel = \"sycl\" }");
        auto one = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(one))) << "a project with a .sycl source failed to build";
        expect(s::has_line(s::text(one), R"(device=\[.*src/kernels/saxpy.sycl.*\])")) << "the .sycl unit did not reach MCPP_DEVICE_SOURCES";
        expect(s::no_line(s::text(p.mcpp({"build", "-v"})), R"(saxpy\.sycl -o|saxpy\.sycl\.o)")) << "the .sycl unit was offered to the C++ compiler";
        p.remove("src/kernels/saxpy.sycl");
        p.write("src/kernels/saxpy.cpp", "extern \"C\" int saxpy_device(int n) { return n; }\n");
        manifest("accel   = \"sycl\"", "\"src/*.cpp\", \"src/kernels/*.cpp\"");
        auto three = p.mcpp({"build", "-v"});
        expect(fatal(e2e::succeeded(three))) << "the .cpp spelling failed to build";
        expect(e2e::contains(s::text(three), "device=[]")) << "a .cpp was handed to the build program as a device source";
        expect(e2e::contains(s::text(three), "saxpy.cpp")) << "the .cpp was not compiled";
        p.remove("src/kernels/saxpy.cpp");
        p.write("src/kernels/saxpy.sycl", "extern \"C\" int saxpy_device(int n) { return n; }\n");
        manifest("accel   = \"sycl\"", "\"src/*.cpp\", { glob = \"src/kernels/*.sycl\", accel = \"sycl\" }");
        auto four = p.mcpp({"build", "--no-accel"});
        expect(fatal(e2e::succeeded(four))) << "--no-accel failed to build";
        expect(e2e::contains(s::text(four), "device=[]")) << "--no-accel still handed the .sycl unit to the build program";
        p.write("mcpp.toml", "[package]\nname = \"sycltest\"\nversion = \"0.1.0\"\n[language]\nstandard = \"c++23\"\n[targets.sycltest]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n");
        auto five = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(five))) << "the default-glob project failed to build";
        expect(e2e::contains(s::text(five), "device=[]")) << "a default glob picked up the .sycl source";
    };

    "623 device sources must reach an action"_test[tag{"requires:elf"}][tag{"requires:gcc"}] = [] {
        // A device-kind source no action compiles is refused naming the file: without a build
        // program, and with one that declared no action; --no-accel leaves it out and builds.
        s::needs({"elf", "gcc"});
        const auto tmp = temp_dir();
        auto mk = [&] {
            fs::remove_all(tmp / "p");
            return s::project{tmp / "p", {{"src/main.cpp", "extern \"C\" void k();\nint main() { return 0; }\n"},
                                          {"src/kernels/k.cu", "extern \"C\" __global__ void k() {}\n"},
                                          {"mcpp.toml", "[package]\nname    = \"orphan\"\nversion = \"0.1.0\"\naccelerators = [\"cuda\"]\n[build]\naccel = \"cuda12.9+{sm_89}\"\nsources = [\n  \"src/*.cpp\",\n  { glob = \"src/kernels/*.cu\", accel = \"cuda12.9+{sm_89}\" },\n]\n[targets.orphan]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}}};
        };
        auto p = mk();
        auto none = p.mcpp({"build"});
        expect(fatal(failed(none))) << "a package with an uncompilable device source built";
        expect(e2e::contains(s::text(none), "src/kernels/k.cu")) << "the refusal does not name the file";
        expect(e2e::contains(s::text(none), "no `build.mcpp`")) << "the refusal does not say the build program is missing";
        p = mk();
        p.write("build.mcpp", "import std;\nimport mcpp;\nint main() { return 0; }\n");
        auto unclaimed = p.mcpp({"build"});
        expect(fatal(failed(unclaimed))) << "an unclaimed device source built";
        expect(e2e::contains(s::text(unclaimed), "src/kernels/k.cu")) << "the refusal does not name the file";
        expect(e2e::contains(s::text(unclaimed), "ran but declared no action")) << "the refusal does not distinguish a program that claimed nothing";
        p = mk();
        expect(e2e::succeeded(p.mcpp({"build", "--no-accel"}))) << "--no-accel must not be refused; the constrained glob is left out";
    };
}};

}  // namespace
