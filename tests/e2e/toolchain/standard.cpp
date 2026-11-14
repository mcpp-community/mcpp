// The C++ standard a toolchain is driven at and the std module it builds: `[package].standard` in
// the flags, the compile database and the std BMI, dialect flags that reach the std BMI, `c++fly`
// and `c++latest` on GCC, the freestanding import-std diagnostic's advice, c++20 as a first-class
// level with its own std BMI, std.o only where std is reached, a dialect flag refused where it
// would miss the std BMI, std.compat built when imported, and the platform's words on the std
// module (formerly tests/e2e/59, 98, 100, 135, 175, 176, 235, 322, 886, 1010).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// `find <root> -type f -path '*/bin/<name>' | head -1`.
std::optional<fs::path> program_under(const fs::path& root, std::string_view name) {
    for (const auto& f : s::find_files(root, ".*"))
        if (f.parent_path().filename() == "bin" && f.filename() == name) return f;
    return std::nullopt;
}

std::string output_of(const fs::path& program) {
    auto out = s::run(program).out;
    while (out.ends_with('\n') || out.ends_with('\r')) out.pop_back();
    return out;
}

// `find target -name build.ninja -not -path '*/.build-mcpp/*' | head -1`, read.
std::string build_ninja(const fs::path& target) {
    for (const auto& f : s::find_files(target, R"(build\.ninja)"))
        if (!f.generic_string().contains("/.build-mcpp/")) return s::read_file(f);
    return {};
}

std::string read_if(const fs::path& f) { return fs::is_regular_file(f) ? s::read_file(f) : std::string(); }

// `grep -rl <needle> <root>`: the regular files under `root` that contain `needle`.
std::vector<fs::path> files_containing(const fs::path& root, std::string_view needle) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    for (const auto& e : fs::recursive_directory_iterator(root, ec))
        if (e.is_regular_file(ec) && s::read_file(e.path()).contains(needle)) out.push_back(e.path());
    return out;
}

// The same for an expression.
std::vector<fs::path> files_matching(const fs::path& root, const std::regex& pattern) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    for (const auto& e : fs::recursive_directory_iterator(root, ec))
        if (e.is_regular_file(ec)) {
            const auto text = s::read_file(e.path());
            if (std::regex_search(text, pattern)) out.push_back(e.path());
        }
    return out;
}

// The g++ of the newest-sorting gcc payload of the shared home, if it accepts -freflection.
std::optional<fs::path> reflection_gxx() {
    std::vector<fs::path> found;
    for (const auto& f : s::find_files(s::shared_home() / "registry/data/xpkgs/xim-x-gcc", R"(g\+\+)"))
        if (f.parent_path().filename() == "bin") found.push_back(f);
    if (found.empty()) return std::nullopt;
    std::ranges::sort(found);
    const auto gxx = found.back();
    auto r = e2e::run({.argv = {gxx.string(), "-freflection", "-std=c++26", "-x", "c++", "-", "-fsyntax-only", "-o", "/dev/null"}, .input = "int main(){}\n"});
    if (!r) return std::nullopt;
    return gxx;
}

constexpr std::string_view print_struct = R"(void print_struct(auto &&value) {
  constexpr auto info = std::meta::remove_cvref(^^decltype(value));
  constexpr auto no_check = std::meta::access_context::unchecked();
  static constexpr auto members = std::define_static_array(
      std::meta::nonstatic_data_members_of(info, no_check));
  template for (constexpr auto e : members) {
    auto &&member = value.[:e:];
    std::println("{} {}", identifier_of(e), member);
  }
}

struct Point { double x{}; double y{}; };
)";

E2E standard{"standard", [] {
    "59 cpp standard config"_test[s::gcc] = [] {
        // `[package].standard` drives the build flags, the compile database and the std BMI's
        // metadata, through the P1689 scanner too.
        const auto home = s::case_home(s::home::inherited);
        const s::options o{.where = s::home::inherited, .env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};
        s::project p({{"mcpp.toml", "[package]\nname     = \"cpp26_std\"\nversion  = \"0.1.0\"\nstandard = \"c++26\"\n"},
                      {"src/main.cpp", "import std;\n\nint main() {\n    std::println(\"cpp standard {}\", 26);\n    return 0;\n}\n"}});
        auto r = p.mcpp({"build", "--no-cache"}, o);
        expect(fatal(e2e::succeeded(r))) << "FAIL: C++26 import-std build failed: " << s::text(r);
        const auto binary = program_under(p.at("target"), "cpp26_std");
        expect(fatal(binary.has_value())) << "FAIL: cpp26_std binary missing";
        expect(fatal(output_of(*binary) == "cpp standard 26")) << "FAIL: wrong runtime output: " << output_of(*binary);
        auto ninja = build_ninja(p.at("target"));
        expect(fatal(!ninja.empty())) << "FAIL: build.ninja missing";
        expect(fatal(s::has_line(ninja, R"(^cxxflags  = -std=c\+\+26)"))) << "FAIL: build.ninja missing C++26 standard flag";
        expect(fatal(s::lacks(ninja, "-std=c++23"))) << "FAIL: build.ninja still contains C++23";
        auto cdb = read_if(p.at("compile_commands.json"));
        expect(fatal(e2e::contains(cdb, "\"-std=c++26\""))) << "FAIL: compile_commands.json missing C++26 standard flag: " << cdb;
        expect(fatal(s::lacks(cdb, "-std=c++23"))) << "FAIL: compile_commands.json still contains C++23";
        const auto metadata = files_containing(home / "build-cache/v1/std", "\"cpp_standard\": \"c++26\"");
        expect(fatal(!metadata.empty())) << "FAIL: no std module metadata records c++26";
        expect(fatal(e2e::contains(s::read_file(metadata.front()), "\"std_flag\": \"-std=c++26\""))) << "FAIL: std module metadata missing C++26 flag: " << s::read_file(metadata.front());
        p.remove("target");
        p.remove("compile_commands.json");
        s::options scanner = o;
        scanner.env.emplace_back("MCPP_SCANNER", "p1689");
        r = p.mcpp({"build", "--no-cache"}, scanner);
        expect(fatal(e2e::succeeded(r))) << "FAIL: C++26 P1689 scanner build failed: " << s::text(r);
        ninja = build_ninja(p.at("target"));
        expect(fatal(s::has_line(ninja, R"(^cxxflags  = -std=c\+\+26)"))) << "FAIL: P1689 build.ninja missing C++26 standard flag";
        cdb = read_if(p.at("compile_commands.json"));
        expect(fatal(s::lacks(s::text(r), "-std=c++23") && s::lacks(ninja, "-std=c++23") && s::lacks(cdb, "-std=c++23"))) << "FAIL: P1689 path still contains C++23";
    };

    "98 reflection import std"_test[s::gcc] = [] {
        // A dialect flag (-freflection) reaches the std BMI and the whole module graph (#210).
        if (!reflection_gxx()) skip("no -freflection-capable gcc payload available");
        s::project p({{"meta_dep/mcpp.toml", "[package]\nname     = \"meta_dep\"\nversion  = \"0.1.0\"\nstandard = \"c++26\"\n\n[targets.meta_dep]\nkind = \"lib\"\n"},
                      {"meta_dep/src/meta_dep.cppm", "export module meta_dep;\nimport std;\nexport namespace meta_dep {\nstd::string tag() { return \"dep-ok\"; }\n}\n"},
                      {"mcpp.toml", "[package]\nname     = \"refl\"\nversion  = \"0.1.0\"\nstandard = \"c++26\"\n\n[build]\ncxxflags = [\"-freflection\"]\n\n[dependencies]\nmeta_dep = { path = \"meta_dep\" }\n"}});
        p.write("src/main.cpp", std::format("import std;\nimport meta_dep;\n\n{}\nint main() {{\n  print_struct(Point{{2, 3}});\n  std::println(\"{{}}\", meta_dep::tag());\n}}\n", print_struct));
        auto r = p.mcpp({"run"});
        const auto out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "FAIL: build/run: " << out;
        expect(fatal(e2e::contains(out, "x 2") && e2e::contains(out, "y 3"))) << "FAIL: reflection output: " << out;
        expect(fatal(e2e::contains(out, "dep-ok"))) << "FAIL: dep module output: " << out;
        static const std::regex flag{R"("std_flag": "[^"]*-freflection)"};
        expect(fatal(!files_matching(s::shared_home() / "build-cache/v1/std", flag).empty())) << "FAIL: no std-module.json records -freflection in std_flag";
    };

    "100 cppfly reflection"_test[s::gcc] = [] {
        // `standard = "c++fly"` is the latest standard with the experimental gates on (reflection
        // and contracts on gcc 16), never spelled -std=c++fly; `c++latest` resolves to -std=c++26.
        const auto gxx = reflection_gxx();
        if (!gxx) skip("no -freflection-capable gcc payload available");
        const auto gcc_ver = gxx->parent_path().parent_path().filename().string();
        const auto home = s::case_home(s::home::inherited);
        const s::options o{.where = s::home::inherited, .env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};
        const auto tmp = s::case_dir();
        s::project p(tmp / "proj", {{"mcpp.toml", std::format("[package]\nname     = \"flydemo\"\nversion  = \"0.1.0\"\nstandard = \"c++fly\"\n\n[toolchain]\ndefault = \"gcc@{}\"\n", gcc_ver)}});
        p.write("src/main.cpp", std::format("import std;\n\n{}\nint main() {{\n  print_struct(Point{{2, 3}});\n}}\n", print_struct));
        auto r = p.mcpp({"build", "--no-cache"}, o);
        const auto log = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "FAIL: c++fly build failed: " << log;
        expect(fatal(e2e::contains(log, "c++fly on gcc"))) << "FAIL: build log missing c++fly summary line: " << log;
        expect(fatal(e2e::contains(log, "reflection (-freflection)"))) << "FAIL: summary missing enabled reflection gate: " << log;
        expect(fatal(s::has_line(log, "enabled: .*contracts"))) << "FAIL: summary missing enabled contracts (on by -std=c++26 on gcc16)";
        const auto ninja = build_ninja(p.at("target"));
        expect(fatal(s::has_line(ninja, R"(^cxxflags  = -std=c\+\+26 -freflection)"))) << "FAIL: build.ninja cxxflags lacks -std=c++26 -freflection";
        const auto cdb = read_if(p.at("compile_commands.json"));
        expect(fatal(s::lacks(ninja, "-std=c++fly") && s::lacks(cdb, "-std=c++fly"))) << "FAIL: raw c++fly canonical leaked into a command line";
        expect(fatal(e2e::contains(cdb, "\"-freflection\""))) << "FAIL: compile_commands.json missing -freflection";
        const auto binary = program_under(p.at("target"), "flydemo");
        expect(fatal(binary.has_value())) << "no flydemo";
        const auto out = output_of(*binary);
        expect(fatal(e2e::contains(out, "x 2") && e2e::contains(out, "y 3"))) << "FAIL: reflection output: " << out;
        expect(fatal(!files_containing(home / "build-cache/v1/std", "\"std_flag\": \"-std=c++26 -freflection").empty())) << "FAIL: no std-module.json records std_flag -std=c++26 -freflection";
        s::project latest(tmp / "latest", {{"mcpp.toml", std::format("[package]\nname     = \"latestdemo\"\nversion  = \"0.1.0\"\nstandard = \"c++latest\"\n\n[toolchain]\ndefault = \"gcc@{}\"\n", gcc_ver)},
                                           {"src/main.cpp", "import std;\nint main() { std::println(\"latest ok\"); }\n"}});
        r = latest.mcpp({"build", "--no-cache"}, o);
        expect(fatal(e2e::succeeded(r))) << "FAIL: c++latest build failed: " << s::text(r);
        const auto latest_ninja = build_ninja(latest.at("target"));
        expect(fatal(s::has_line(latest_ninja, R"(^cxxflags  = -std=c\+\+26)"))) << "FAIL: c++latest did not resolve to -std=c++26";
        expect(fatal(s::lacks(latest_ninja, "-std=c++latest") && s::lacks(read_if(latest.at("compile_commands.json")), "-std=c++latest"))) << "FAIL: invalid -std=c++latest spelling reached a command line";
    };

    "135 diagnostic promise resolves"_test[s::llvm][s::unix_shell] = [] {
        // The freestanding import-std diagnostic prints a dependency line to paste, and that line
        // resolves and enters the build graph.
        auto p = s::project::scaffold("promise");
        for (const auto& t : s::glob(p.at("tests"), "*.cpp")) fs::remove(t);
        const std::string manifest = "[package]\nname    = \"promise\"\nversion = \"0.1.0\"\n\n[build]\ntarget = \"riscv64-none-elf\"\n";
        p.write("mcpp.toml", manifest);
        p.write("src/main.cpp", "import std;\nextern \"C\" int main() { return 0; }\n");
        auto r = p.mcpp({"build"});
        expect(fatal(!r)) << "`import std;` on a freestanding target should have been rejected: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "is not available on"))) << "the freestanding import-std diagnostic did not appear: " << s::text(r);
        std::smatch m;
        const auto diag = s::text(r);
        static const std::regex line{R"([a-z0-9.-]+[ \t\f\v\r]*=[ \t\f\v\r]*"[^"\n]+")"};
        std::string advice;
        if (std::regex_search(diag, m, line)) advice = m.str();
        expect(fatal(!advice.empty())) << "the diagnostic printed no dependency line to paste — if the advice was removed on purpose, remove this test with it: " << diag;
        std::println("advice: {}", advice);
        p.write("mcpp.toml", manifest + "\n[dependencies]\n" + advice + "\n");
        p.write("src/main.cpp", "import mcpplibs.std.freestanding;\nextern \"C\" int main() { return static_cast<int>(std::array<int, 3>{}.size()); }\n");
        const auto paste = s::text(p.mcpp({"build", "-v"}));
        expect(fatal(s::no_line(paste, "not found in the synced index|package not found|E_NOT_FOUND"))) << "the diagnostic's copy-pasteable line does not resolve; advice was: " << advice;
        expect(fatal(s::has_line(paste, "(Downloading|Compiling).*std-freestanding"))) << "the pasted dependency never entered the build graph";
    };

    "175 cpp20 import std"_test[s::gcc] = [] {
        // `standard = "c++20"` is a first-class level: import std works at it, -std=c++20 is on
        // every command and the std BMI records it; gnu++20 reaches cxxflags; c++17 is refused,
        // naming the c++20 floor.
        const auto home = s::case_home(s::home::inherited);
        const s::options o{.where = s::home::inherited, .env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};
        const auto tmp = s::case_dir();
        s::project p(tmp / "proj", {{"mcpp.toml", "[package]\nname     = \"cpp20demo\"\nversion  = \"0.1.0\"\nstandard = \"c++20\"\n"},
                                    {"src/util.cppm", "export module cpp20demo.util;\nimport std;\n\nexport namespace cpp20demo {\ninline int count_evens(const std::vector<int>& v) {\n    return static_cast<int>(std::ranges::count_if(v, [](int x) { return x % 2 == 0; }));\n}\n}\n"},
                                    {"src/main.cpp", "import std;\nimport cpp20demo.util;\n\nint main() {\n    std::vector<int> v{1, 2, 3, 4, 5, 6};\n    std::cout << \"evens=\" << cpp20demo::count_evens(v) << \"\\n\";\n    static_assert(__cplusplus == 202002L, \"expected C++20 mode\");\n    return 0;\n}\n"}});
        auto r = p.mcpp({"build", "--no-cache"}, o);
        expect(fatal(e2e::succeeded(r))) << "FAIL: c++20 build failed: " << s::text(r);
        const auto binary = program_under(p.at("target"), "cpp20demo");
        expect(fatal(binary.has_value())) << "no cpp20demo";
        expect(fatal(output_of(*binary) == "evens=3")) << "FAIL: unexpected output: " << output_of(*binary);
        const auto ninja = build_ninja(p.at("target"));
        expect(fatal(s::has_line(ninja, R"(^cxxflags  = -std=c\+\+20)"))) << "FAIL: build.ninja cxxflags is not -std=c++20";
        const auto cdb = read_if(p.at("compile_commands.json"));
        expect(fatal(s::lacks(ninja, "-std=c++23") && s::lacks(cdb, "-std=c++23"))) << "FAIL: -std=c++23 leaked into a command line under standard = c++20";
        expect(fatal(e2e::contains(cdb, "-std=c++20"))) << "FAIL: compile_commands.json missing -std=c++20";
        expect(fatal(!files_containing(home / "build-cache/v1/std", "\"std_flag\": \"-std=c++20\"").empty())) << "FAIL: no std-module.json records std_flag -std=c++20";
        s::project gnu(tmp / "gnu", {{"mcpp.toml", "[package]\nname     = \"gnu20demo\"\nversion  = \"0.1.0\"\nstandard = \"gnu++20\"\n"},
                                     {"src/main.cpp", "import std;\nint main() { std::cout << \"gnu20 ok\\n\"; }\n"}});
        r = gnu.mcpp({"build", "--no-cache"}, o);
        expect(fatal(e2e::succeeded(r))) << "FAIL: gnu++20 build failed: " << s::text(r);
        expect(fatal(s::has_line(build_ninja(gnu.at("target")), R"(^cxxflags  = -std=gnu\+\+20)"))) << "FAIL: gnu++20 did not reach cxxflags";
        s::project old(tmp / "old", {{"mcpp.toml", "[package]\nname     = \"olddemo\"\nversion  = \"0.1.0\"\nstandard = \"c++17\"\n"},
                                     {"src/main.cpp", "int main() { return 0; }\n"}});
        r = old.mcpp({"build", "--no-cache"}, o);
        expect(fatal(!r)) << "FAIL: standard = c++17 was accepted";
        expect(fatal(e2e::contains(s::text(r), "c++20"))) << "FAIL: rejection message does not advertise the c++20 floor: " << s::text(r);
    };

    "176 cpp20 std bmi isolation"_test[s::gcc] = [] {
        // Switching `[package].standard` between c++20 and c++23 keeps a std BMI and a target
        // directory per level.
        const auto home = s::case_home(s::home::inherited);
        const s::options o{.where = s::home::inherited};
        s::project p({{"src/main.cpp", "import std;\nint main() { std::cout << \"level ok\\n\"; return 0; }\n"}});
        auto build_at = [&](std::string_view level) {
            p.write("mcpp.toml", std::format("[package]\nname     = \"leveldemo\"\nversion  = \"0.1.0\"\nstandard = \"{}\"\n", level));
            const auto written = fs::last_write_time(p.at("mcpp.toml"));
            auto r = p.mcpp({"build"}, o);
            expect(fatal(e2e::succeeded(r))) << "FAIL: build at " << level << " failed: " << s::text(r);
            std::optional<fs::path> binary;
            for (const auto& f : s::find_files(p.at("target"), "leveldemo"))
                if (f.parent_path().filename() == "bin" && fs::last_write_time(f) > written) { binary = f; break; }
            if (!binary) binary = program_under(p.at("target"), "leveldemo");
            expect(fatal(binary.has_value())) << "no leveldemo";
            expect(fatal(output_of(*binary) == "level ok")) << "FAIL: bad output at " << level << ": " << output_of(*binary);
        };
        build_at("c++23");
        build_at("c++20");
        build_at("c++23");
        const auto std20 = files_containing(home / "build-cache/v1/std", "\"std_flag\": \"-std=c++20\"").size();
        const auto std23 = files_containing(home / "build-cache/v1/std", "\"std_flag\": \"-std=c++23\"").size();
        expect(fatal(std20 >= 1u && std23 >= 1u)) << "FAIL: expected a std BMI for each level (c++20=" << std20 << " c++23=" << std23 << ")";
        std::size_t dirs = 0;
        for (const auto& triple : fs::directory_iterator(p.at("target")))
            if (triple.is_directory())
                for (const auto& d : fs::directory_iterator(triple.path()))
                    if (d.is_directory()) ++dirs;
        expect(fatal(dirs >= 2u)) << "FAIL: expected separate target dirs per standard, found " << dirs;
    };

    "235 std object only when needed"_test[s::elf] = [] {
        // std.o is in the graph of a project that reaches std, transitively too, and not in one
        // that imports it nowhere (#416).
        const auto tmp = s::case_dir();
        const auto toolchain = std::format("[toolchain]\ndefault = \"gcc@16.1.0\"\nmacos   = \"llvm@{0}\"\nwindows = \"llvm@{0}\"\n", s::llvm_version());
        s::project nostd(tmp / "nostd", {{"mcpp.toml", "[package]\nname = \"nostd\"\nversion = \"0.1.0\"\n" + toolchain},
                                         {"src/x.cppm", "export module nostd.x;\nexport int v() { return 3; }\n"},
                                         {"src/main.cpp", "import nostd.x;\nint main() { return v() - 3; }\n"}});
        auto r = nostd.mcpp({"build", "--release"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: the no-std project did not build: " << s::text(r);
        auto ninja = s::find_file(nostd.at("target"), "build.ninja");
        expect(fatal(ninja.has_value())) << "FAIL: no build.ninja produced";
        expect(fatal(s::lacks(s::read_file(*ninja), "obj/std.o"))) << "FAIL: std.o is referenced by a project that imports std nowhere";
        s::project trans(tmp / "trans", {{"mcpp.toml", "[package]\nname = \"trans\"\nversion = \"0.1.0\"\n" + toolchain},
                                         {"src/a.cppm", "export module trans.a;\nimport std;\nexport std::string greet() { return \"hi\"; }\n"},
                                         {"src/b.cppm", "export module trans.b;\nimport trans.a;\nexport int n() { return static_cast<int>(greet().size()); }\n"},
                                         {"src/main.cpp", "import trans.b;\nint main() { return n() - 2; }\n"}});
        r = trans.mcpp({"build", "--release"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: the transitive project did not build (an undefined `_ZGIW3std` means the reachability walk did not follow a <- b <- main): " << s::text(r);
        ninja = s::find_file(trans.at("target"), "build.ninja");
        expect(fatal(ninja.has_value() && e2e::contains(s::read_file(*ninja), "obj/std.o"))) << "FAIL: std.o is NOT in the graph, but this project reaches std through trans.b -> trans.a -> std";
        const auto bin = program_under(trans.at("target"), "trans");
        expect(fatal(bin.has_value())) << "FAIL: no binary produced by the transitive project";
        const auto ran = s::run(*bin);
        expect(fatal(e2e::succeeded(ran))) << "FAIL: the transitive binary exited " << ran.status;
    };

    "322 dialect flag refusal"_test[s::gcc] = [] {
        // A dialect flag in cxxflags (or a profile's) beside import std is refused before the
        // compiler runs, naming dialect_cxxflags; dialect_cxxflags, an auto-promoted flag and a
        // project without import std build (#527).
        s::project p;
        auto writes_manifest = [&](std::string_view body) {
            p.write("mcpp.toml", std::format("[package]\nname = \"dia\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n\n[build]\n{}\n", body));
        };
        const std::string uses_import_std = "import std;\nint main(){ std::println(\"ok\"); return 0; }\n";
        const std::string no_import_std = "#include <cstdio>\nint main(){ std::printf(\"ok\\n\"); return 0; }\n";
        p.write("src/main.cpp", uses_import_std);
        writes_manifest("cxxflags = [\"-fno-exceptions\"]");
        p.remove("target");
        auto r = p.mcpp({"build"});
        expect(fatal(!r)) << "FAIL: -fno-exceptions in cxxflags + import std should be refused";
        expect(fatal(e2e::contains(s::text(r), "dialect_cxxflags"))) << "FAIL: the refusal does not name `dialect_cxxflags`: " << s::text(r);
        expect(fatal(s::no_line(s::text(r), "^ *Compiling"))) << "FAIL: the compiler ran — the check fired too late";
        expect(fatal(!fs::is_directory(p.at("target")) || s::find_files(p.at("target"), R"(.*\.o)").empty())) << "FAIL: objects were produced before the refusal";
        writes_manifest("cxxflags = [\"-fno-rtti\"]");
        p.remove("target");
        expect(fatal(!p.mcpp({"build"}))) << "FAIL: -fno-rtti in cxxflags + import std should be refused";
        p.write("mcpp.toml", "[package]\nname = \"dia\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n\n[profile.dev]\ncxxflags = [\"-fno-exceptions\"]\n");
        p.remove("target");
        expect(fatal(!p.mcpp({"build"}))) << "FAIL: a dialect flag in [profile.dev] cxxflags should be refused too";
        writes_manifest("dialect_cxxflags = [\"-fno-exceptions\"]");
        p.remove("target");
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: dialect_cxxflags is the documented fix and must build: " << s::text(r);
        writes_manifest("cxxflags = [\"-D_GLIBCXX_USE_CXX11_ABI=0\"]");
        p.remove("target");
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: an auto-promoted dialect flag must still build: " << s::text(r);
        p.write("src/main.cpp", no_import_std);
        writes_manifest("cxxflags = [\"-fno-exceptions\"]");
        p.remove("target");
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: without import std, -fno-exceptions is an ordinary per-unit flag: " << s::text(r);
    };

    "886 std compat is built when imported"_test[s::gcc] = [] {
        // On the GCC row std.compat is built in the cache when a unit imports it, and not for a
        // build that imports only std.
        const auto home = s::case_home(s::home::fresh);
        s::project(home, {{"config.toml", std::format("[xlings]\nhome = \"{}\"\n", (s::shared_home() / "registry").generic_string())}});
        const s::options o{.where = s::home::fresh};
        auto compat_files = [&] {
            std::size_t n = 0;
            for (const auto& e : fs::recursive_directory_iterator(home))
                if (e.path().generic_string().contains("build-cache") && e.path().filename().string().starts_with("std.compat.")) ++n;
            return n;
        };
        const auto tmp = s::case_dir();
        s::project only(tmp / "only", {{"mcpp.toml", "[package]\nname = \"only\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"gcc@16.1.0\"\n"},
                                       {"src/main.cpp", "import std;\nint main() { std::println(\"std\"); }\n"}});
        s::project compat(tmp / "compat", {{"mcpp.toml", "[package]\nname = \"compat\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"gcc@16.1.0\"\n"},
                                           {"src/main.cpp", "import std.compat;\nint main() {\n    auto* memory = ::malloc(32);\n    if (!memory) return 1;\n    ::free(memory);\n    std::vector<int> values{1, 2, 3};\n    ::printf(\"compat=%zu\\n\", values.size());\n    return values.size() == 3 ? 0 : 1;\n}\n"}});
        auto r = only.mcpp({"run"}, o);
        expect(fatal(e2e::succeeded(r))) << "FAIL: import std failed: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), "^std$"))) << "FAIL: the std program did not run: " << s::text(r);
        expect(fatal(compat_files() == 0u)) << "FAIL: std.compat was built for a build that does not import it";
        r = compat.mcpp({"run"}, o);
        expect(fatal(e2e::succeeded(r))) << "FAIL: import std.compat failed on the GCC row: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "compat=3"))) << "FAIL: the std.compat program did not run: " << s::text(r);
        expect(fatal(compat_files() != 0u)) << "FAIL: std.compat was not built in the cache";
    };

    "1010 the std module takes the platforms flags"_test[s::llvm][s::unix_shell] = [] {
        // The std module is compiled with the compile words the platform states
        // (-fcf-protection=full), so a program importing std builds and runs under it.
        if (!s::on_x86_64()) skip("-fcf-protection is an x86_64 flag");
        const auto tmp = s::case_dir();
        s::project(tmp / "plat", {{"mcpp.toml", "[package]\nname    = \"cet-platform\"\nversion = \"0.1.0\"\n\n[platform]\ntargets = [\"x86_64-linux-gnu\"]\n\n[platform.build]\ncxxflags = [\"-fcf-protection=full\"]\n"}});
        s::project app(tmp / "app", {{"mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"llvm@{}\"\n\n[target.'cfg(linux)']\nplatform = {{ path = \"../plat\" }}\n", s::llvm_version())},
                                     {"src/main.cpp", "import std;\nint main() {\n    std::vector<int> v{1, 2, 3};\n    std::println(\"std with the platform's words: {}\", v.size());\n    return 0;\n}\n"}});
        auto r = app.mcpp({"build", "-v"});
        const auto log = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "FAIL: a program importing std did not build with the platform's flag: " << log;
        expect(fatal(s::lacks(log, "Control-Flow Branch Protection"))) << "FAIL: the std module was refused for the flag";
        bool named = static_cast<bool>(s::has_line(log, R"(std\.cppm.*-fcf-protection=full|-fcf-protection=full.*std\.cppm)"));
        for (const auto& n : s::glob(app.at("target"), "*/*/build.ninja"))
            if (e2e::contains(s::read_file(n), "fcf-protection=full")) named = true;
        expect(fatal(named)) << "FAIL: the std module's command does not name the platform's flag: " << log;
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: the program did not run: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "std with the platform's words: 3"))) << "FAIL: the program's output: " << s::text(r);
    };
}};

}  // namespace
