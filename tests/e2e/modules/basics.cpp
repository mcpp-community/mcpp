// Modules of a program: several modules and a partition built and run, the p1689 scanner and
// ninja's dyndep against the static plan, module units in .cpp files, transitive and indirect
// imports (formerly tests/e2e/03, 20, 21, 898, 924, 940).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

std::optional<fs::path> graph_of(const fs::path& root) {
    auto all = s::find_files(root / "target", R"(build\.ninja)");
    std::ranges::sort(all);
    for (const auto& f : all)
        if (!f.generic_string().contains("/.build-mcpp/")) return f;
    return std::nullopt;
}

// A program's standard output without its trailing newlines (`$(program)`).
std::string output_of(const fs::path& program) {
    auto out = s::run(program).out;
    std::erase(out, '\r');
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

// The only build directory of the project's x86_64 Linux target (`ls -d target/x86_64-linux-*/`,
// then `ls` of it).
fs::path build_dir(const s::project& p) {
    const auto dirs = s::glob(p.at("target"), "x86_64-linux-*/*");
    expect(fatal(!dirs.empty())) << "no build directory under target/x86_64-linux-*";
    return dirs.front();
}

// The basenames of every BMI below target, sorted and unique (`find -name '*.gcm' | xargs basename | sort -u`).
std::set<std::string> bmis(const s::project& p) {
    std::set<std::string> out;
    for (const auto& f : s::find_files(p.at("target"), R"(.*\.gcm)")) out.insert(f.filename().string());
    return out;
}

// The number of `[n/` step lines of a verbose build (`grep -cE '^\[[0-9]+/'`).
std::size_t steps(std::string_view text) { return s::count_matching(text, R"(^\[[0-9]+/)"); }

constexpr std::string_view lib_cppm = "export module myapp.lib;\nimport std;\nexport import :greet;\nexport auto banner() -> void { std::println(\"== myapp ==\"); }\n";
constexpr std::string_view greet_cppm = "export module myapp.lib:greet;\nimport std;\nexport auto greet(std::string_view who) -> void {\n    std::println(\"hello, {}\", who);\n}\n";
constexpr std::string_view myapp_toml = R"([package]
name        = "myapp"
version     = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
[modules]
sources = ["src/**/*.cppm", "src/**/*.cpp"]
[targets.myapp]
kind = "bin"
main = "src/main.cpp"
)";

E2E basics{"basics", [] {
    "03 multi module"_test[s::elf][s::gcc] = [] {
        // A package's module and main build and run; the graph names its BMI and the std BMI.
        auto p = s::project::scaffold("multi");
        p.write("src/greeter.cppm", "export module multi.greeter;\nimport std;\nexport auto greet(std::string_view who) -> void {\n    std::println(\"Hello, {}! (from multi.greeter)\", who);\n}\n");
        p.write("src/main.cpp", "import std;\nimport multi.greeter;\nint main() {\n    greet(\"modular C++23\");\n    std::println(\"multi-module build OK\");\n    return 0;\n}\n");
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto out = s::text(p.mcpp({"run"}));
        expect(fatal(e2e::contains(out, "from multi.greeter"))) << "module greet not invoked: " << out;
        expect(fatal(e2e::contains(out, "multi-module build OK"))) << "main println missing: " << out;
        const auto g = graph_of(p.dir());
        expect(fatal(g.has_value())) << "no build.ninja";
        const auto text = s::read_file(*g);
        expect(fatal(e2e::contains(text, "gcm.cache/multi.greeter.gcm"))) << "ninja missing greeter BMI";
        expect(fatal(e2e::contains(text, "gcm.cache/std.gcm"))) << "ninja missing std BMI";
    };

    "20 p1689 scanner"_test[s::elf][s::gcc] = [] {
        // The p1689 scanner builds what the regex scanner builds: the same output, the same BMIs.
        auto p = s::project::scaffold("myapp");
        p.write("src/lib.cppm", lib_cppm);
        p.write("src/lib-greet.cppm", greet_cppm);
        p.write("src/main.cpp", "import std;\nimport myapp.lib;\nint main() {\n    banner();\n    greet(\"p1689\");\n    return 0;\n}\n");
        p.write("mcpp.toml", myapp_toml);
        (void)p.mcpp({"build"});
        const auto out_regex = output_of(build_dir(p) / "bin/myapp");
        const auto gcm_regex = bmis(p);
        p.remove("target");
        (void)p.mcpp({"build"}, {.env = {{"MCPP_SCANNER", "p1689"}}});
        const auto out_p1689 = output_of(build_dir(p) / "bin/myapp");
        const auto gcm_p1689 = bmis(p);
        expect(fatal(out_regex == out_p1689)) << "runtime output differs\n  regex: " << out_regex << "\n  p1689: " << out_p1689;
        expect(fatal(gcm_regex == gcm_p1689)) << "compiled BMI set differs";
        expect(fatal(gcm_p1689.contains("myapp.lib.gcm"))) << "missing myapp.lib.gcm";
        expect(fatal(gcm_p1689.contains("myapp.lib-greet.gcm"))) << "missing partition gcm";
    };

    "21 ninja dyndep"_test[s::elf][s::gcc] = [] {
        // MCPP_NINJA_DYNDEP=1 builds what the static plan builds, through scan and dyndep edges
        // and their files; a rebuild finishes.
        auto p = s::project::scaffold("myapp");
        p.write("src/lib.cppm", lib_cppm);
        p.write("src/lib-greet.cppm", greet_cppm);
        p.write("src/main.cpp", "import std;\nimport myapp.lib;\nint main() { banner(); greet(\"dyndep\"); return 0; }\n");
        p.write("mcpp.toml", myapp_toml);
        const auto static_log = s::text(p.mcpp({"build"}));
        const auto out_static = output_of(build_dir(p) / "bin/myapp");
        p.remove("target");
        const s::options dyndep{.env = {{"MCPP_NINJA_DYNDEP", "1"}}};
        (void)p.mcpp({"build"}, dyndep);
        const auto dir = build_dir(p);
        const auto out_dyndep = output_of(dir / "bin/myapp");
        expect(fatal(out_static == out_dyndep)) << "runtime output differs\n  static: " << out_static << "\n  dyndep: " << out_dyndep;
        const auto dd = s::find_files(dir, R"(.*\.ddi\.dd)");
        expect(fatal(!dd.empty())) << "no per-file .ddi.dd files produced under MCPP_NINJA_DYNDEP=1";
        const auto graph = s::read_file(dir / "build.ninja");
        expect(fatal(s::has_line(graph, "^rule cxx_scan"))) << "build.ninja missing cxx_scan rule";
        expect(fatal(s::has_line(graph, "^rule cxx_dyndep"))) << "build.ninja missing cxx_dyndep rule";
        expect(fatal(e2e::contains(graph, "  dyndep = "))) << "compile edges missing dyndep =";
        expect(fatal(s::no_line(static_log, "^rule cxx_scan"))) << "static mode leaked cxx_scan";
        auto ddis = s::find_files(p.at("target"), R"(.*\.cppm\.ddi)");
        std::ranges::sort(ddis);
        expect(fatal(!ddis.empty())) << "no .ddi file produced";
        const auto ddi = s::read_file(ddis.front());
        expect(fatal(e2e::contains(ddi, "\"rules\""))) << ".ddi missing rules";
        expect(fatal(e2e::contains(ddi, "\"primary-output\""))) << ".ddi missing primary-output";
        auto dds = dd;
        std::ranges::sort(dds);
        expect(fatal(e2e::contains(s::read_file(dds.front()), "ninja_dyndep_version = 1"))) << "dyndep file missing version header";
        const auto out2 = s::text(p.mcpp({"build"}, dyndep));
        expect(fatal(e2e::contains(out2, "Finished"))) << "incremental rebuild output: " << out2;
    };

    "898 module units in cpp files"_test[s::gcc][s::llvm] = [] {
        // Under Clang and GCC: an implementation partition in a .cpp, imported, builds and the
        // second build does nothing (M1, M3); an interface unit in a .cpp writes its BMI (M2).
        const s::options home{.where = s::home::inherited};
        for (std::string_view tc : {"llvm@23.1.3", "gcc@16.1.0"}) {
            s::project parts(s::case_dir() / tc / "parts",
                             {{"src/repro.cppm", "export module repro;\nexport import :api;\nimport :api_impl;\n"},
                              {"src/api.cppm", "export module repro:api;\nexport auto answer() -> int;\n"},
                              {"src/api_impl.cpp", "module repro:api_impl;\nimport :api;\nauto answer() -> int { return 42; }\n"},
                              {"src/main.cpp", "import repro;\nimport std;\nauto main() -> int { std::println(\"{}\", answer()); }\n"}});
            parts.write("mcpp.toml", std::format("[package]\nname = \"repro\"\nversion = \"0.1.0\"\n[toolchain]\ndefault = \"{}\"\n[targets.repro]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", tc));
            auto r = parts.mcpp({"build"}, home);
            expect(fatal(e2e::succeeded(r))) << tc << ": a .cpp implementation partition, imported\n" << s::text(r);
            r = parts.mcpp({"build", "--verbose"}, home);
            expect(fatal(e2e::succeeded(r))) << tc << ": second build\n" << s::text(r);
            expect(fatal(steps(s::text(r)) == 0u)) << tc << ": the second build did " << steps(s::text(r)) << " steps\n" << s::text(r);
            std::string answers;
            for (const auto& f : s::find_files(parts.at("target"), "repro.*"))
                if (f.generic_string().contains("/bin/repro")) answers += output_of(f);
            expect(fatal(answers == "42")) << tc << ": wrong answer: " << answers;

            s::project iface(s::case_dir() / tc / "iface",
                             {{"src/r5.cpp", "export module r5;\nexport auto answer() -> int { return 42; }\n"},
                              {"src/main.cpp", "import r5;\nimport std;\nauto main() -> int { std::println(\"{}\", answer()); }\n"}});
            iface.write("mcpp.toml", std::format("[package]\nname = \"r5\"\nversion = \"0.1.0\"\n[toolchain]\ndefault = \"{}\"\n[targets.r5]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", tc));
            r = iface.mcpp({"build"}, home);
            expect(fatal(e2e::succeeded(r))) << tc << ": an interface unit in a .cpp\n" << s::text(r);
            r = iface.mcpp({"build", "--verbose"}, home);
            expect(fatal(e2e::succeeded(r))) << tc << ": second build\n" << s::text(r);
            expect(fatal(steps(s::text(r)) == 0u)) << tc << ": the second build did " << steps(s::text(r)) << " steps\n" << s::text(r);
        }
    };

    "924 visible imports resolve transitive modules"_test[s::python3] = [] {
        // A consumer's module map names the dependency it reaches through a visible import.
        const s::options home{.where = s::home::inherited};
        s::project root({{"dependency/mcpp.toml", R"([package]
name = "dependency"
version = "0.1.0"
[language]
standard = "c++23"
modules = true
import_std = false
[modules]
sources = ["src/*.cppm"]
exports = ["dependency"]
[targets.dependency]
kind = "lib"
)"},
                         {"dependency/src/dependency.cppm", "export module dependency;\nexport int dep_value() { return 42; }\n"},
                         {"probe/mcpp.toml", R"([package]
name = "probe"
version = "0.1.0"
[language]
standard = "c++23"
modules = true
import_std = false
[modules]
sources = ["src/*.cppm", "src/*.cpp"]
exports = ["probe"]
[dependencies]
dependency = {path = "../dependency"}
[targets.probe]
kind = "bin"
main = "src/main.cpp"
)"},
                         {"probe/src/probe.cppm", "export module probe;\nimport dependency;\nexport int value() { return dep_value(); }\n"},
                         {"probe/src/main.cpp", "import probe;\nint main() { return value() == 42 ? 0 : 1; }\n"}});
        auto r = root.mcpp_in("probe", {"build"}, home);
        expect(fatal(e2e::succeeded(r))) << "visible transitive module build\n" << s::text(r);
        r = root.mcpp_in("probe", {"run"}, home);
        expect(fatal(e2e::succeeded(r))) << "transitive module program\n" << s::text(r);
        bool reaches = false;
        std::vector<std::string> maps;
        for (const auto& m : s::find_files(root.at("probe/target"), R"(probe-.*\.map)")) {
            if (m.parent_path().filename() != "modmap") continue;
            maps.push_back(m.string());
            const auto text = s::read_file(m);
            reaches = reaches || (text.contains("dependency ") && text.contains("probe "));
        }
        expect(fatal(reaches)) << "consumer map does not reach dependency" << maps;
        r = root.mcpp_in("probe", {"build"}, home);
        expect(fatal(e2e::succeeded(r))) << "warm transitive module build\n" << s::text(r);
    };

    "940 an indirect import is found whatever map its importer has"_test = [] {
        // Units with and without a module map reach a root BMI's imports, std.compat among them.
        s::project root({{"dep/mcpp.toml", "[package]\nname    = \"dep\"\nversion = \"0.1.0\"\n[targets.dep]\nkind = \"lib\"\n"},
                         {"dep/src/lib.cppm", "export module dep.lib;\nexport int dep_value() { return 2; }\n"},
                         {"app/mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[dependencies]\ndep = { path = \"../dep\" }\n"},
                         {"app/src/os.cppm", "export module app.os;\nexport inline constexpr int os_value = 1;\n"},
                         {"app/src/process.cppm", "export module app.process;\nimport app.os;\nexport int process_value() { return os_value; }\n"},
                         {"app/src/jobs.cppm", "export module app.jobs;\nimport app.process;\nimport dep.lib;\nexport int jobs_value() { return process_value() + dep_value(); }\n"},
                         {"app/src/view.cppm", "export module app.view;\nimport app.jobs;\nexport int view_value() { return jobs_value() * 10; }\n"},
                         {"app/src/compat.cppm", "export module app.compat;\nimport std.compat;\nimport dep.lib;\nexport int compat_value() { return static_cast<int>(::strlen(\"ab\")) + dep_value(); }\n"},
                         {"app/src/main.cpp", "#include <cstdio>\nimport app.view;\nimport app.compat;\nint main() { std::printf(\"%d\\n\", view_value() + compat_value()); }\n"}});
        auto r = root.mcpp_in("app", {"build"});
        expect(fatal(e2e::succeeded(r))) << "a unit with a map could not read a root BMI's import\n" << s::text(r);
        r = root.mcpp_in("app", {"run"});
        auto out = r.out;
        std::erase(out, '\r');
        expect(fatal(s::last_line(out) == "34")) << "the program printed '" << s::last_line(out) << "', expected 34\n" << r.err;
    };
}};

}  // namespace
