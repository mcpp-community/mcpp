// Libraries on the MSVC ABI: packing with msvc@system runs `lib.exe /REMOVE:` (one flag per member,
// the archive last); a fat Windows package carries an MSVC leg and a MinGW leg under their own names,
// each chosen by its predicate; a DLL exports without `__declspec(dllexport)` through a generated
// `.def`, defers to an annotated surface, and its data still needs `__declspec(dllimport)`; native
// cl.exe links a package through the neutral link intent (formerly tests/e2e/255, 256, 258, 262).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view mathkit_interface = "export module mathkit;\nexport namespace mk { int answer(); }\n";
constexpr std::string_view mathkit_impl = "module mathkit;\nnamespace mk { int answer() { return 42; } }\n";

std::string library_manifest(std::string_view name, std::string_view kind, bool msvc) {
    return std::format("[package]\nname    = \"{0}\"\nversion = \"0.1.0\"\n[build]\nsources = [\"src/*.cppm\", \"src/*.cpp\"]\n[targets.{0}]\nkind = \"{1}\"\n{2}",
                       name, kind, msvc ? "[toolchain]\nwindows = \"msvc@system\"\n" : "");
}

std::string app_manifest(std::string_view name, std::string_view dependency, const fs::path& at, bool msvc) {
    return std::format("[package]\nname    = \"{0}\"\nversion = \"0.1.0\"\n[dependencies]\n{1} = {{ path = \"{2}\" }}\n[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n{3}",
                       name, dependency, at.generic_string(), msvc ? "[toolchain]\nwindows = \"msvc@system\"\n" : "");
}

// The package directory `mcpp pack` wrote for mathkit 0.1.0 (`mathkit-0.1.0-<tag>`).
fs::path package_of(const s::project& p) {
    for (const auto& e : fs::directory_iterator(p.at("target/dist")))
        if (e.is_directory() && e.path().filename().string().starts_with("mathkit-0.1.0")) return e.path();
    expect(fatal(false)) << "no package directory under target/dist";
    return {};
}

std::size_t exported_lines(const fs::path& def) {
    std::size_t n = 0;
    for (auto line : s::lines_with(s::read_file(def), "")) {
        std::erase(line, '\r');
        if (!line.empty() && !line.starts_with("LIBRARY") && !line.starts_with("EXPORTS")) ++n;
    }
    return n;
}

E2E libraries{"libraries", [] {
    "255 pack library msvc archiver"_test[s::msvc] = [] {
        // The packer drops the published interface's objects from the archive; lib.exe takes
        // `/REMOVE:` once per member and the archive last, and a wrong spelling makes the pack refuse,
        // quoting the command. So the pack succeeding is the assertion, and the branch was taken: the
        // archive is mathkit.lib (lib.exe's naming), and two interface units were published.
        s::project p{{"src/mathkit.cppm", "export module mathkit;\nexport import :api;\n"},
                     {"src/api.cppm", "export module mathkit:api;\nexport namespace mk { int answer(); }\n"},
                     {"src/impl.cpp", mathkit_impl},
                     {"mcpp.toml", library_manifest("mathkit", "lib", true)}};
        auto r = p.mcpp({"pack", "mathkit"});
        expect(fatal(e2e::succeeded(r))) << "packing with msvc@system failed; 'cannot drop published interface objects' means the lib.exe spelling is wrong: " << s::text(r);
        const auto pkg = package_of(p);
        expect(fatal(s::find_file(pkg / "lib", "mathkit.lib").has_value())) << "no mathkit.lib in the package: the lib.exe branch was not taken";
        expect(fatal(fs::exists(pkg / "interface/mathkit.cppm") && fs::exists(pkg / "interface/api.cppm"))) << "nothing was published as source, so nothing had to be removed";
    };

    "256 pack library fat windows"_test[s::msvc][s::mingw] = [] {
        // One package, an MSVC leg (lib/<arch>-windows-msvc/mathkit.lib) and a MinGW leg
        // (lib/<arch>-windows-gnu/libmathkit.a): lib/ is keyed by triple. The legs are chosen by
        // cfg() predicates with an env axis, never by a bare triple, and a consumer built natively,
        // for the MSVC row and for the GNU row takes its own leg alone.
        const auto tmp = s::case_dir();
        s::project lib(tmp / "mathkit", {{"src/mathkit.cppm", mathkit_interface}, {"src/impl.cpp", mathkit_impl},
                                         {"mcpp.toml", library_manifest("mathkit", "lib", false)}});
        auto r = lib.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the probe build failed: " << s::text(r);
        std::string host_triple;
        for (const auto& e : fs::directory_iterator(lib.at("target")))
            if (e.is_directory() && e.path().filename() != "dist") host_triple = e.path().filename().string();
        expect(fatal(host_triple.contains("windows"))) << "expected a windows host triple, read '" << host_triple << "'";
        const auto arch = host_triple.substr(0, host_triple.find('-'));
        const auto msvc_leg = arch + "-windows-msvc", mingw_leg = arch + "-windows-gnu";
        fs::remove_all(lib.at("target"));
        r = lib.mcpp({"pack", "mathkit", "--target", msvc_leg, "--target", mingw_leg});
        expect(fatal(e2e::succeeded(r))) << "the fat pack failed: " << s::text(r);
        const auto pkg = lib.at("target/dist/mathkit-0.1.0");
        expect(fatal(fs::exists(pkg / "lib" / msvc_leg / "mathkit.lib"))) << "the MSVC leg is not lib/" << msvc_leg << "/mathkit.lib";
        expect(fatal(fs::exists(pkg / "lib" / mingw_leg / "libmathkit.a"))) << "the MinGW leg is not lib/" << mingw_leg << "/libmathkit.a";
        const auto m = s::read_file(pkg / "mcpp.toml");
        expect(fatal(e2e::contains(m, "target.'cfg("))) << "the legs are not selected by cfg() predicates: " << m;
        expect(fatal(!s::has_line(m, std::format("^\\[target\\.'{}-", arch)))) << "a leg is selected by a bare triple, inert on a native build: " << m;
        expect(fatal(std::regex_search(m, std::regex(R"(env\s*=)")))) << "the predicates carry no env axis, so both legs match both targets: " << m;
        s::project app(tmp / "app", {{"src/main.cpp", "#include <cstdio>\nimport mathkit;\nint main(){ std::printf(\"ok=%d\\n\", mk::answer()); return 0; }\n"},
                                     {"mcpp.toml", app_manifest("app", "mathkit", pkg, false)}});
        const auto takes = [&](std::string_view label, std::string_view want, std::vector<std::string> args) {
            fs::remove_all(app.at("target"));
            args.insert(args.begin(), "build");
            auto b = app.mcpp(args);
            expect(fatal(e2e::succeeded(b))) << label << ": the build failed: " << s::text(b);
            const auto nj = s::find_file(app.at("target"), "build.ninja");
            expect(fatal(nj.has_value())) << label << ": no build.ninja";
            std::set<std::string> legs;
            const auto text = s::read_file(*nj);
            static const std::regex leg(R"(mathkit-0\.1\.0[\\/]lib[\\/]([A-Za-z0-9_-]+))");
            for (auto it = std::sregex_iterator(text.begin(), text.end(), leg); it != std::sregex_iterator{}; ++it) legs.insert((*it)[1].str());
            expect(fatal(legs.size() == 1)) << label << " saw " << legs.size() << " leg(s), expected one" << std::vector<std::string>(legs.begin(), legs.end());
            expect(fatal(*legs.begin() == want)) << label << " took the wrong leg: " << *legs.begin();
        };
        takes("native", msvc_leg, {});
        takes("msvc", msvc_leg, {"--target", msvc_leg});
        takes("mingw", mingw_leg, {"--target", mingw_leg});
    };

    "258 shared library msvc auto def"_test[s::msvc] = [] {
        // A DLL with no `__declspec(dllexport)` exports through a `.def` that a `coff_def` edge
        // writes from the objects the link consumes, non-empty, beside the DLL and its import
        // library; a consumer resolves a function through it. An annotated library keeps its own
        // surface (an empty generated `.def`). Exported data is marked DATA and still needs
        // `__declspec(dllimport)` on the consumer's declaration: with it the value is read, without
        // it not (the limit docs/12 states, reproduced).
        const auto tmp = s::case_dir();
        s::project lib(tmp / "mathkit", {{"src/mathkit.cppm", mathkit_interface}, {"src/impl.cpp", mathkit_impl},
                                         {"mcpp.toml", library_manifest("mathkit", "shared", true)}});
        auto r = lib.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "a shared target did not build on the MSVC ABI: " << s::text(r);
        std::optional<fs::path> nj;
        for (const auto& f : s::find_files(lib.at("target"), "build\\.ninja"))
            if (!f.generic_string().contains("/.build-mcpp/")) nj = f;
        expect(fatal(nj && s::has_line(s::read_file(*nj), R"(^build .*mathkit\.def : coff_def )"))) << "no coff_def edge: the .def is not produced from the objects the link consumes";
        const auto def = s::find_file(lib.at("target"), "mathkit.def");
        expect(fatal(def.has_value())) << "no .def was written";
        expect(fatal(s::has_line(s::read_file(*def), "^EXPORTS"))) << "a malformed .def: " << s::read_file(*def);
        expect(fatal(exported_lines(*def) > 0)) << "the .def exports nothing, so the import library is empty: " << s::read_file(*def);
        expect(fatal(s::find_file(lib.at("target"), "mathkit.dll").has_value())) << "no DLL";
        expect(fatal(s::find_file(lib.at("target"), "mathkit.lib").has_value())) << "no import library beside the DLL";
        r = lib.mcpp({"pack", "mathkit"});
        expect(fatal(e2e::succeeded(r))) << "pack: " << s::text(r);
        s::project app(tmp / "app", {{"src/main.cpp", "#include <cstdio>\nimport mathkit;\nint main(){ std::printf(\"msvc-dll=%d\\n\", mk::answer()); return 0; }\n"},
                                     {"mcpp.toml", app_manifest("app", "mathkit", package_of(lib), true)}});
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r) && e2e::contains(s::text(r), "msvc-dll=42"))) << "the consumer could not link or start against the MSVC DLL: " << s::text(r);

        s::project annotated(tmp / "annotated", {{"src/a.cppm", "export module annkit;\nexport namespace ak { __declspec(dllexport) int chosen(); }\n"},
                                                 {"src/a.cpp", "module annkit;\nnamespace ak { __declspec(dllexport) int chosen() { return 7; } }\n"},
                                                 {"mcpp.toml", library_manifest("annkit", "shared", true)}});
        r = annotated.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the annotated library did not build: " << s::text(r);
        const auto anndef = s::find_file(annotated.at("target"), "annkit.def");
        expect(fatal(anndef.has_value())) << "no .def edge for the annotated library";
        expect(fatal(exported_lines(*anndef) == 0)) << "an export list was generated for a library that declares its own: " << s::read_file(*anndef);

        s::project datalib(tmp / "datalib", {{"src/datalib.cppm", "export module datalib;\nexport extern \"C\" int dl_value;\n"},
                                             {"src/datalib.cpp", "module datalib;\nextern \"C\" int dl_value = 99;\n"},
                                             {"mcpp.toml", library_manifest("datalib", "shared", true)}});
        r = datalib.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the data library did not build: " << s::text(r);
        const auto datadef = s::find_file(datalib.at("target"), "datalib.def");
        expect(fatal(datadef && e2e::contains(s::read_file(*datadef), "dl_value DATA"))) << "the exported variable is not marked DATA";
        const auto consume = [&](std::string_view declaration) {
            fs::remove_all(tmp / "dataapp");
            s::project dataapp(tmp / "dataapp", {{"src/main.cpp", std::format("#include <cstdio>\n{}\nint main(){{ std::printf(\"data=%d\\n\", dl_value); return 0; }}\n", declaration)},
                                                 {"mcpp.toml", app_manifest("dataapp", "datalib", datalib.dir(), true)}});
            return dataapp.mcpp({"run"});
        };
        const auto plain = consume("extern \"C\" int dl_value;");
        const auto imported = consume("extern \"C\" __declspec(dllimport) int dl_value;");
        expect(fatal(e2e::succeeded(imported) && e2e::contains(s::text(imported), "data=99"))) << "even with __declspec(dllimport) the data symbol was unusable: " << s::text(imported);
        expect(fatal(!(e2e::succeeded(plain) && e2e::contains(s::text(plain), "data=99"))))
            << "reading exported data without __declspec(dllimport) worked: the limit docs/12 states no longer holds, and the documentation is what is wrong";
    };

    "262 pack consumed by native cl"_test[s::msvc] = [] {
        // The package states each leg's link line twice: the GNU ldflags (`-Llib/<triple>
        // -l<name>`) for older clients, and the neutral `link_library_dirs` / `libraries` of a
        // conditional [runtime], which mcpp renders as /LIBPATH: and <name>.lib for link.exe. A
        // consumer pinned to msvc@system, the one toolchain that cannot survive a stray -L, runs, and
        // no GNU -L for the package's leg reaches its link line.
        const auto tmp = s::case_dir();
        s::project lib(tmp / "mathkit", {{"src/mathkit.cppm", mathkit_interface}, {"src/impl.cpp", mathkit_impl},
                                         {"mcpp.toml", library_manifest("mathkit", "lib", true)}});
        auto r = lib.mcpp({"pack", "mathkit"});
        expect(fatal(e2e::succeeded(r))) << "pack failed: " << s::text(r);
        const auto pkg = package_of(lib);
        const auto m = s::read_file(pkg / "mcpp.toml");
        expect(fatal(e2e::contains(m, ".runtime]"))) << "no conditional [runtime] block: a cl.exe consumer would have only the GNU ldflags: " << m;
        expect(fatal(e2e::contains(m, "link_library_dirs") && e2e::contains(m, "libraries"))) << "the neutral form is incomplete: " << m;
        expect(fatal(e2e::contains(m, "ldflags"))) << "the ldflags are gone, which an older mcpp reads alone: " << m;
        s::project app(tmp / "app", {{"src/main.cpp", "#include <cstdio>\nimport mathkit;\nint main(){ std::printf(\"cl-ok=%d\\n\", mk::answer()); return 0; }\n"},
                                     {"mcpp.toml", app_manifest("app", "mathkit", pkg, true)}});
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "native cl.exe could not consume the package (a message naming '-L' means the GNU ldflags reached the command line): " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "cl-ok=42"))) << "wrong answer: " << s::text(r);
        const auto nj = s::find_file(app.at("target"), "build.ninja");
        expect(fatal(nj.has_value())) << "no build.ninja";
        for (const auto& line : s::lines_with(s::read_file(*nj), "ldflags"))
            expect(fatal(!line.contains("-Llib/"))) << "a GNU -L for the package's leg is on the link line: " << line;
    };
}};

}  // namespace
