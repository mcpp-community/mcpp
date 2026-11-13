// Export discovery on the MSVC ABI with LLVM: a DLL built with LTO exports from bitcode; source
// annotations, in bitcode or in COFF and in either order, define the whole surface; an opt-out
// removes the scanner from the graph; `exports` narrows discovery, warns beside annotations and is
// refused with discovery off; a DLL a consumer links must export something (formerly
// tests/e2e/881).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// A home of the case whose payloads are the shared home's registry (`[xlings] home`); the build
// writes the project-root compile database, which the case reads.
s::options registry_home() {
    const auto home = s::case_home(s::home::fresh);
    s::project(home, {{"config.toml", std::format("[xlings]\nhome = \"{}\"\n", (s::shared_home() / "registry").generic_string())}});
    return {.where = s::home::fresh, .env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};
}

std::string shared_manifest(std::string_view name, std::string_view extra) {
    return std::format("[package]\nname = \"{0}\"\nversion = \"0.1.0\"\n[toolchain]\nwindows = \"llvm@{1}\"\n[profile.release]\nlto = true\n[targets.{0}]\nkind = \"shared\"\n{2}",
                       name, s::llvm_version(), extra);
}

// The lines of a `.def` that export something.
std::size_t exported_lines(std::string_view def) {
    std::size_t n = 0;
    for (auto line : s::lines_with(def, "")) {
        std::erase(line, '\r');
        if (!line.empty() && !line.starts_with("LIBRARY") && !line.starts_with("EXPORTS") &&
            line.find_first_not_of(" \t") != std::string::npos)
            ++n;
    }
    return n;
}

// `python3 -c` reading `dll` through ctypes, independently of mcpp's consumers.
e2e::outcome ctypes(const fs::path& dll, std::string_view body) {
    return e2e::run(e2e::command{.argv = {"python3", "-c", std::format("import ctypes, sys\nlib = ctypes.CDLL(sys.argv[1])\n{}", body),
                                          fs::absolute(dll).string()}});
}

// The compiler of the first entry of a compile database (JSON-escaped backslashes undone).
fs::path compiler_of(const fs::path& db) {
    static const std::regex first(R"re("arguments"\s*:\s*\[\s*"((?:[^"\\]|\\.)*)")re");
    std::smatch m;
    const auto text = s::read_file(db);
    expect(fatal(std::regex_search(text, m, first))) << "no arguments in " << db.string();
    std::string path;
    const auto quoted = m[1].str();
    for (std::size_t i = 0; i < quoted.size(); ++i) path += quoted[i] == '\\' && i + 1 < quoted.size() ? quoted[++i] : quoted[i];
    return path;
}

E2E exports{"exports", [] {
    "881 pe auto exports accept llvm bitcode"_test[s::msvc][s::python3] = [] {
        const auto o = registry_home();
        const auto tmp = s::case_dir();
        const auto mcpp = [&](std::vector<std::string> args) { return s::mcpp(std::move(args), tmp, o); };
        const auto def_of = [](const fs::path& f) { return s::read_file(f); };

        // A: an unannotated module DLL, a separate implementation and exported data; a consumer
        // proves the module initializer and the code exports work with LTO, and ctypes reads the data.
        s::project automatic(tmp / "automatic", {{"mcpp.toml", shared_manifest("automatic", "")},
                                                 {"src/api.cppm", "export module t881_lto;\nexport int answer();\n"},
                                                 {"src/impl.cpp", "module t881_lto;\nint answer() { return 42; }\n"},
                                                 {"src/data.cpp", "extern \"C\" int exported_data = 9;\n"}});
        s::project app(tmp / "app", {{"mcpp.toml", std::format("[package]\nname = \"app\"\nversion = \"0.1.0\"\n[dependencies]\nautomatic = {{ path = \"../automatic\" }}\n[toolchain]\nwindows = \"llvm@{}\"\n", s::llvm_version())},
                                     {"src/main.cpp", "import t881_lto;\nint main() { return answer() == 42 ? 0 : 1; }\n"}});
        auto r = automatic.mcpp({"build", "--profile", "release"}, o);
        expect(fatal(e2e::succeeded(r))) << "A: the LTO DLL did not build: " << s::text(r);
        r = app.mcpp({"run", "--profile", "release"}, o);
        expect(fatal(e2e::succeeded(r))) << "A: the module consumer did not run: " << s::text(r);
        const auto automatic_dll = s::find_file(automatic.at("target"), "automatic.dll");
        expect(fatal(automatic_dll.has_value())) << "A: no automatic.dll";
        r = ctypes(*automatic_dll, "assert ctypes.c_int.in_dll(lib, 'exported_data').value == 9\n");
        expect(fatal(e2e::succeeded(r))) << "A: the exported data is not readable: " << s::text(r);

        // The selected LLVM, through its actual compile command (not another on PATH).
        const auto db = s::find_file(automatic.at("target"), "compile_commands.json");
        expect(fatal(db.has_value())) << "no compile database";
        const auto clang = compiler_of(*db).string();
        const auto nm = (fs::path(clang).parent_path() / "llvm-nm.exe").string();
        const auto compile = [&](std::string_view target, std::string_view flags, const fs::path& source, std::string_view object) {
            std::vector<std::string> argv{clang, "--driver-mode=g++", std::format("--target={}", target)};
            if (!flags.empty()) argv.emplace_back(flags);
            argv.insert(argv.end(), {"-c", source.string(), "-o", (tmp / object).string()});
            auto c = e2e::run(e2e::command{.argv = argv, .cwd = tmp});
            expect(fatal(e2e::succeeded(c))) << "compiling " << source.string() << ": " << s::text(c);
        };

        // B: annotations in bitcode define the whole surface, DATA included; a literal holding
        // export-like text is not an annotation.
        s::project annotated(tmp / "annotated", {{"mcpp.toml", shared_manifest("annotated", "")},
                                                 {"src/api.cpp", "extern \"C\" __declspec(dllexport) int chosen() { return 7; }\nextern \"C\" __declspec(dllexport) int chosen_data = 11;\nextern \"C\" int hidden() { return 3; }\n"}});
        r = annotated.mcpp({"build", "--profile", "release"}, o);
        expect(fatal(e2e::succeeded(r))) << "B: annotated bitcode did not build: " << s::text(r);
        const auto annotated_dll = s::find_file(annotated.at("target"), "annotated.dll");
        expect(fatal(annotated_dll.has_value())) << "B: no annotated.dll";
        r = ctypes(*annotated_dll, "assert lib.chosen() == 7\nassert ctypes.c_int.in_dll(lib, 'chosen_data').value == 11\n"
                                   "try:\n    lib.hidden\nexcept AttributeError:\n    pass\nelse:\n    raise AssertionError('the scanner widened the annotated surface')\n");
        expect(fatal(e2e::succeeded(r))) << "B: " << s::text(r);
        const auto annotated_def = s::find_file(annotated.at("target"), "annotated.def");
        expect(fatal(annotated_def && exported_lines(def_of(*annotated_def)) == 0)) << "B: an annotated library received automatic exports";

        // C: native-only export control never depends on coff-def, as the root or as a dependency.
        annotated.write("mcpp.toml", shared_manifest("annotated", "windows_auto_export = false\n"));
        r = annotated.mcpp({"build", "--profile", "release"}, o);
        expect(fatal(e2e::succeeded(r))) << "C: the opt-out did not build: " << s::text(r);
        const auto no_coff_def = [](const fs::path& project, std::string_view what) {
            const auto nj = s::find_file(project / "target", "build.ninja");
            expect(fatal(nj.has_value())) << what << ": no build.ninja";
            expect(fatal(!s::has_line(s::read_file(*nj), R"(^build .* : coff_def)"))) << what << ": the opt-out still schedules export discovery";
        };
        no_coff_def(annotated.dir(), "C");
        s::project consumer(tmp / "native-consumer", {{"mcpp.toml", std::format("[package]\nname = \"native-consumer\"\nversion = \"0.1.0\"\n[dependencies]\nannotated = {{ path = \"../annotated\" }}\n[toolchain]\nwindows = \"llvm@{}\"\n", s::llvm_version())},
                                                      {"src/main.cpp", "extern \"C\" __declspec(dllimport) int chosen();\nint main() { return chosen() == 7 ? 0 : 1; }\n"}});
        r = consumer.mcpp({"run", "--profile", "release"}, o);
        expect(fatal(e2e::succeeded(r))) << "C: the dependency opt-out did not run: " << s::text(r);
        no_coff_def(consumer.dir(), "C (dependency)");

        // D: an ordinary annotated COFF input takes precedence in either input order; LLVM tools
        // that do not exist prove the bitcode paths are not entered once COFF states the intent.
        compile("x86_64-pc-windows-msvc", "", annotated.at("src/api.cpp"), "annotated.obj");
        compile("x86_64-pc-windows-msvc", "-flto=thin", automatic.at("src/data.cpp"), "thin.obj");
        for (const auto& order : {std::pair{"annotated.obj", "thin.obj"}, std::pair{"thin.obj", "annotated.obj"}}) {
            r = mcpp({"coff-def", "--output", "mixed.def", "--name", "mixed", "--llvm-cxx", "absent-clang", "--llvm-nm", "absent-nm", order.first, order.second});
            expect(fatal(e2e::succeeded(r))) << "D: annotation precedence depends on input order: " << s::text(r);
            expect(fatal(exported_lines(def_of(tmp / "mixed.def")) == 0)) << "D: mixed objects widened the surface: " << def_of(tmp / "mixed.def");
        }

        // E: ThinLTO and textual export-like data: a string literal does not disable auto-export.
        s::project(tmp, {{"literal.cpp", "extern \"C\" int literal_value = 13;\nextern \"C\" const char* message = \"dllexport /EXPORT:pretend\";\n"},
                         {"pragma.cpp", "#pragma comment(linker, \"/EXPORT:pragma_api\")\nextern \"C\" int pragma_api() { return 19; }\nextern \"C\" int unrelated() { return 1; }\n"},
                         {"native.cpp", "extern \"C\" int native_api() { return 23; }\n"},
                         {"empty.cpp", "static int internal_only() { return 1; }\n"},
                         {"patterns.txt", "keep_*\n"}});
        compile("x86_64-pc-windows-msvc", "-flto=thin", tmp / "literal.cpp", "literal.obj");
        r = mcpp({"coff-def", "--output", "literal.def", "--name", "literal", "--llvm-cxx", clang, "--llvm-nm", nm, "--llvm-target", "x86_64-pc-windows-msvc", "literal.obj"});
        expect(fatal(e2e::succeeded(r))) << "E: ThinLTO candidates were not read: " << s::text(r);
        expect(fatal(e2e::contains(def_of(tmp / "literal.def"), "literal_value DATA"))) << "E: literal text suppressed automatic exports: " << def_of(tmp / "literal.def");

        // F: pragma linker options in ThinLTO suppress discovery without llvm-nm.
        compile("x86_64-pc-windows-msvc", "-flto=thin", tmp / "pragma.cpp", "pragma.obj");
        r = mcpp({"coff-def", "--output", "pragma.def", "--name", "pragma", "--llvm-cxx", clang, "--llvm-nm", "absent-nm", "--llvm-target", "x86_64-pc-windows-msvc", "pragma.obj"});
        expect(fatal(e2e::succeeded(r))) << "F: linker metadata did not suppress candidate discovery: " << s::text(r);
        expect(fatal(exported_lines(def_of(tmp / "pragma.def")) == 0)) << "F: linker metadata widened the surface: " << def_of(tmp / "pragma.def");

        // G: ordinary COFF and bitcode contribute to one unannotated surface.
        compile("x86_64-pc-windows-msvc", "", tmp / "native.cpp", "native.obj");
        r = mcpp({"coff-def", "--output", "union.def", "--name", "union", "--llvm-cxx", clang, "--llvm-nm", nm, "--llvm-target", "x86_64-pc-windows-msvc", "native.obj", "literal.obj"});
        expect(fatal(e2e::succeeded(r))) << "G: mixed candidate discovery failed: " << s::text(r);
        expect(fatal(s::has_line(def_of(tmp / "union.def"), "native_api\r?$"))) << "G: the COFF function was lost: " << def_of(tmp / "union.def");
        expect(fatal(e2e::contains(def_of(tmp / "union.def"), "literal_value DATA"))) << "G: the bitcode data was lost: " << def_of(tmp / "union.def");

        // H: the selected target, not the host's architecture, controls x86 decoration.
        compile("i686-pc-windows-msvc", "-flto=thin", tmp / "literal.cpp", "x86.obj");
        r = mcpp({"coff-def", "--output", "x86.def", "--name", "x86", "--llvm-cxx", clang, "--llvm-nm", nm, "--llvm-target", "i686-pc-windows-msvc", "x86.obj"});
        expect(fatal(e2e::succeeded(r))) << "H: x86 bitcode discovery failed: " << s::text(r);
        expect(fatal(e2e::contains(def_of(tmp / "x86.def"), "literal_value DATA"))) << "H: the x86 cdecl decoration was not normalised: " << def_of(tmp / "x86.def");

        // I: `exports` narrows what discovery finds: the matching symbols are published, data
        // keeps DATA, the rest is not exported.
        s::project narrowed(tmp / "narrowed", {{"mcpp.toml", std::format("[package]\nname = \"narrowed\"\nversion = \"0.1.0\"\n[toolchain]\nwindows = \"llvm@{}\"\n[targets.narrowed]\nkind = \"shared\"\nexports = [\"keep_*\"]\n", s::llvm_version())},
                                               {"src/api.cpp", "extern \"C\" int keep_api() { return 29; }\nextern \"C\" int keep_value = 31;\nextern \"C\" int drop_api() { return 37; }\n"}});
        r = narrowed.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "I: the narrowed DLL did not build: " << s::text(r);
        const auto narrowed_def = s::find_file(narrowed.at("target"), "narrowed.def");
        expect(fatal(narrowed_def.has_value())) << "I: no narrowed.def";
        const auto nd = def_of(*narrowed_def);
        expect(fatal(s::has_line(nd, "keep_api\r?$"))) << "I: a matching function was not exported: " << nd;
        expect(fatal(e2e::contains(nd, "keep_value DATA"))) << "I: matching data lost its DATA keyword: " << nd;
        expect(fatal(!e2e::contains(nd, "drop_api"))) << "I: a symbol outside the patterns was exported: " << nd;
        const auto narrowed_dll = s::find_file(narrowed.at("target"), "narrowed.dll");
        expect(fatal(narrowed_dll.has_value())) << "I: no narrowed.dll";
        r = ctypes(*narrowed_dll, "assert lib.keep_api() == 29\nassert ctypes.c_int.in_dll(lib, 'keep_value').value == 31\n"
                                  "try:\n    lib.drop_api\nexcept AttributeError:\n    pass\nelse:\n    raise AssertionError('drop_api is published')\n");
        expect(fatal(e2e::succeeded(r))) << "I: " << s::text(r);

        // J: beside source annotations `exports` cannot narrow, and says so.
        r = mcpp({"coff-def", "--output", "warned.def", "--name", "warned", "--exports-file", "patterns.txt", "annotated.obj"});
        expect(fatal(e2e::succeeded(r))) << "J: coff-def failed beside annotations: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "`exports` has no effect on this DLL"))) << "J: no warning beside annotations: " << s::text(r);

        // K: `exports` with discovery off has nothing to narrow, and is refused when an MSVC-ABI
        // row is planned.
        s::project contradictory(tmp / "contradictory", {{"mcpp.toml", std::format("[package]\nname = \"contradictory\"\nversion = \"0.1.0\"\n[toolchain]\nwindows = \"llvm@{}\"\n[targets.contradictory]\nkind = \"shared\"\nexports = [\"api_*\"]\nwindows_auto_export = false\n", s::llvm_version())},
                                                         {"src/api.cpp", "extern \"C\" int api_one() { return 1; }\n"}});
        r = contradictory.mcpp({"build"}, o);
        expect(fatal(!e2e::succeeded(r))) << "K: exports with windows_auto_export = false was accepted";
        expect(fatal(e2e::contains(s::text(r), "windows_auto_export = false"))) << "K: the refusal does not name the keys: " << s::text(r);

        // L: a DLL a consumer links must export something; one nothing links may not.
        compile("x86_64-pc-windows-msvc", "", tmp / "empty.cpp", "empty.obj");
        r = mcpp({"coff-def", "--output", "empty.def", "--name", "empty", "empty.obj"});
        expect(fatal(e2e::succeeded(r))) << "L: an empty surface without a consumer was refused: " << s::text(r);
        r = mcpp({"coff-def", "--output", "empty.def", "--name", "empty", "--required", "empty.obj"});
        expect(fatal(!e2e::succeeded(r))) << "L: an empty surface a consumer links was accepted";
        expect(fatal(e2e::contains(s::text(r), "exports no symbol, and a consumer in this build links it"))) << "L: the refusal was not stated: " << s::text(r);
    };
}};

}  // namespace
