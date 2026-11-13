// Target-conditional configuration evaluated against the resolved target: build flags and
// dependencies, sources, a dependency's sources, glob flags,
// feature-deps, a bare triple on a native build, a conditional dependency
// replacing the unconditional one, a root-only conditional dialect, members' conditional rows
// (formerly tests/e2e/85, 86, 108, 119, 149, 195, 247, 677, 813, 895).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

// The build.ninja files of a build, outside the build program's own tree (`find target -name
// build.ninja -not -path '*/.build-mcpp/*'`).
std::vector<fs::path> ninjas(const s::project& p) {
    std::vector<fs::path> out;
    for (const auto& f : s::find_files(p.at("target"), R"(build\.ninja)"))
        if (f.generic_string().find("/.build-mcpp/") == std::string::npos) out.push_back(f);
    return out;
}

// The text of a file, or nothing when it does not exist (`grep file` then reads no line).
std::string text_of(const fs::path& f) { return fs::exists(f) ? s::read_file(f) : std::string{}; }

E2E conditional{"conditional", [] {
    "85 target cfg build flags"_test = [] {
        // `[target.'cfg(...)'.build]` applies exactly the host's os/arch predicates.
        s::project app{s::files{{"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"

[target.'cfg(linux)'.build]
cxxflags = ["-DCOND_LINUX=1"]
[target.'cfg(macos)'.build]
cxxflags = ["-DCOND_MACOS=1"]
[target.'cfg(windows)'.build]
cxxflags = ["-DCOND_WIN=1"]
[target.'cfg(unix)'.build]
cxxflags = ["-DCOND_UNIX=1"]
[target.'cfg(arch = "x86_64")'.build]
cxxflags = ["-DCOND_X64=1"]
[target.'cfg(arch = "aarch64")'.build]
cxxflags = ["-DCOND_ARM64=1"]
# Boolean combinator: any(linux, macos) == unix-family.
[target.'cfg(any(linux, macos))'.build]
cxxflags = ["-DCOND_UNIXLIKE=1"]
)cpp"}, {"src/main.cpp", R"cpp(// Exactly one OS predicate must apply (the host's), regardless of platform.
#if (defined(COND_LINUX) + defined(COND_MACOS) + defined(COND_WIN)) != 1
#error "exactly one of cfg(linux)/cfg(macos)/cfg(windows) must apply on any host"
#endif
// cfg(unix) applies iff not windows.
#if defined(COND_WIN) && defined(COND_UNIX)
#error "cfg(unix) wrongly applied on a windows host"
#endif
#if !defined(COND_WIN) && !defined(COND_UNIX)
#error "cfg(unix) should apply on a non-windows host"
#endif
// any(linux, macos) must agree with unix on the CI platforms.
#if defined(COND_UNIXLIKE) != defined(COND_UNIX)
#error "cfg(any(linux,macos)) disagreed with cfg(unix)"
#endif
// Exactly one arch predicate must apply on the CI runners (x86_64 or aarch64).
#if (defined(COND_X64) + defined(COND_ARM64)) != 1
#error "exactly one of cfg(arch=x86_64)/cfg(arch=aarch64) must apply"
#endif
// Mutual exclusion: the non-host OS predicate must NOT leak in.
int main() { return 0; }
)cpp"}}};
        expect(e2e::succeeded(app.mcpp({"build"}))) << "conditional cfg flags mis-applied for this host";
    };

    "86 target cfg dependencies"_test = [] {
        // `[target.'cfg(...)'.dependencies]`: a matching predicate's dependency resolves, a
        // non-matching one is never resolved (its path does not exist).
        const auto tmp = s::case_dir();
        s::project{tmp / "widget", {{"mcpp.toml", R"cpp([package]
name    = "widget"
version = "0.1.0"
[targets.widget]
kind = "lib"
)cpp"}, {"src/widget.cppm", R"cpp(export module widget;
export int widget_anchor() { return 0; }
)cpp"}}};
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"

# widget is pulled under whichever family matches the host — so exactly one of
# these applies and widget is ALWAYS resolved (on any of the 3 platforms).
[target.'cfg(unix)'.dependencies]
widget = { path = "../widget" }
[target.'cfg(windows)'.dependencies]
widget = { path = "../widget" }

# A never-matching predicate (no such arch) points at a non-existent path. If the
# conditional merge wrongly pulled it, resolution of the bogus path would FAIL the
# build — so a clean build proves non-matching deps are excluded.
[target.'cfg(arch = "no_such_arch")'.dependencies]
ghost = { path = "../this_path_does_not_exist" }
)cpp"}, {"src/main.cpp", R"cpp(import widget;  // fails to build unless the cfg-matched widget dep was resolved
int main() { return widget_anchor(); }
)cpp"}}};
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "conditional dep not resolved, or non-matching dep wrongly pulled";
        if (app.exists("mcpp.lock")) {
            expect(e2e::contains(app.read("mcpp.lock"), "widget")) << "widget missing from lockfile";
            expect(s::lacks(app.read("mcpp.lock"), "ghost")) << "non-matching cfg dependency 'ghost' leaked into the resolve";
        }
    };
    "108 cfg conditional sources"_test[tag{"requires:gcc"}] = [] {
        // `[target.'cfg(...)'.build] sources`: the host arch's file is compiled, the other's not.
        s::needs({"gcc"});
        std::string this_arch, other_arch;  // the host's (`uname -m`), and the other one
#if defined(__x86_64__) || defined(_M_X64)
        this_arch = "x86_64", other_arch = "aarch64";
#elif defined(__aarch64__) || defined(_M_ARM64)
        this_arch = "aarch64", other_arch = "x86_64";
#else
        skip("unmapped arch");
#endif
        auto p = s::project::scaffold("condsrc");
        p.write("arch/this.cpp", R"cpp(extern "C" int arch_answer() { return 7; }
)cpp");
        p.write("arch/other.cpp", R"cpp(extern "C" int arch_answer() { return 8; }   // would double-define if both compiled
)cpp");
        p.write("src/main.cpp", R"cpp(import std;
extern "C" int arch_answer();
int main() {
    std::println("arch = {}", arch_answer());
    return 0;
}
)cpp");
        p.write("mcpp.toml", std::format("[package]\nname    = \"condsrc\"\nversion = \"0.1.0\"\n\n[target.'cfg(arch = \"{}\")'.build]\nsources = [\"arch/this.cpp\"]\n\n[target.'cfg(arch = \"{}\")'.build]\nsources = [\"arch/other.cpp\"]\n", this_arch, other_arch));
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "build failed";
        expect(eq(s::last_line(p.mcpp({"run"}).out), std::string{"arch = 7"})) << "unexpected output";
        expect(s::find_files(p.at("target"), R"(other\.o)").empty()) << "other-arch source compiled";
    };

    "119 dep cfg sources"_test[tag{"requires:gcc"}] = [] {
        // A path dependency's `[target.'cfg(...)'.build]` sources are compiled under build and
        // under test (#229).
        s::needs({"gcc"});
        const auto tmp = s::case_dir();
        s::project{tmp / "mylib", {{"mcpp.toml", R"cpp([package]
name    = "mylib"
version = "0.1.0"

[targets.mylib]
kind = "lib"

[build]
sources = ["src/**/*.cppm"]

# impl.cpp is reachable ONLY through these conditional blocks — never through
# the unconditional [build] sources glob above. Exactly one of unix/windows
# always matches the resolved host target.
[target.'cfg(unix)'.build]
sources = ["src/impl.cpp"]
[target.'cfg(windows)'.build]
sources = ["src/impl.cpp"]
)cpp"}, {"src/mylib.cppm", R"cpp(export module mylib;

// Defined in src/impl.cpp, which is pulled in ONLY via cfg-conditional
// [target.'cfg(...)'.build] sources — extern "C" so the module boundary
// doesn't affect the link name.
extern "C" int impl_value();

export int mylib_answer() { return impl_value(); }
)cpp"}, {"src/impl.cpp", R"cpp(extern "C" int impl_value() { return 42; }
)cpp"}}};
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"

[dependencies]
mylib = { path = "../mylib" }
)cpp"}, {"src/main.cpp", R"cpp(import mylib;
import std;
int main() {
    std::println("value = {}", mylib_answer());
    return mylib_answer() == 42 ? 0 : 1;
}
)cpp"}, {"tests/t.cpp", R"cpp(import mylib;
int main() {
    return mylib_answer() == 42 ? 0 : 1;
}
)cpp"}}};
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "build did not compile the path dep's cfg-conditional sources";
        expect(fatal(eq(s::last_line(app.mcpp({"run"}).out), std::string{"value = 42"}))) << "unexpected run output";
        expect(e2e::succeeded(app.mcpp({"test"}))) << "test mode did not compile the path dep's cfg-conditional sources";
    };
    "149 conditional glob flags"_test = [] {
        // Per-glob `flags` inside `[target.'cfg(<os>)'.build]`: the host's entry reaches the TU
        // and overrides the base table; an off-OS entry does not exist (no dead-glob warning); a
        // real dead glob in [build].flags still warns.
        const std::string this_os = s::on_linux() ? "linux" : s::on_macos() ? "macos" : "windows";
        const std::string other_os = s::on_linux() || s::on_macos() ? "windows" : "linux";
        auto p = s::project::scaffold("condflags");
        p.write("src/zlib/probe.cpp", R"cpp(#ifndef BASE_DEFINE
#error "base [build].flags entry did not reach the TU"
#endif
#ifndef COND_DEFINE
#error "conditional [target.cfg(os)] flags entry did not reach the TU"
#endif
#ifdef REMOVED_DEFINE
#error "conditional entry failed to override the base table (removal case)"
#endif
extern "C" int probe_ok() { return 1; }
)cpp");
        p.write("src/main.cpp", R"cpp(import std;
extern "C" int probe_ok();
int main() { std::println("probe = {}", probe_ok()); return 0; }
)cpp");
        p.write("mcpp.toml", std::format("[package]\nname    = \"condflags\"\nversion = \"0.1.0\"\n\n[build]\nflags = [\n  {{ glob = \"src/zlib/**\", defines = [\"BASE_DEFINE=1\", \"REMOVED_DEFINE=1\"] }},\n]\n\n[target.'cfg({0})'.build]\nflags = [\n  {{ glob = \"src/zlib/**\", defines = [\"COND_DEFINE=1\"], cxxflags = [\"-UREMOVED_DEFINE\"] }},\n]\n\n[target.'cfg({1})'.build]\nflags = [\n  {{ glob = \"src/{1}_only/**\", defines = [\"OFF_OS=1\"] }},\n]\n", this_os, other_os));
        auto b = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(b))) << "conditional glob flags build failed";
        expect(fatal(eq(s::last_line(p.mcpp({"run"}).out), std::string{"probe = 1"}))) << "unexpected output";
        expect(fatal(s::lacks(s::text(b), "matched no source file"))) << "an off-OS conditional flags entry produced a dead-glob warning";
        p.write("mcpp.toml", p.read("mcpp.toml") + R"cpp(
[[build.flags]]
glob = "src/nonexistent_dir/**"
defines = ["NEVER=1"]
)cpp");
        auto b2 = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(b2))) << "build failed";
        expect(e2e::contains(s::text(b2), "matched no source file")) << "a real dead glob in [build].flags must still warn";
    };

    "195 target cfg feature deps"_test = [] {
        // `[target.<sel>.feature-deps.<f>]`: a matching section is merged, a non-matching one is
        // not; an inactive feature's conditional dep is not resolved; a feature populated only
        // by non-matching sections is still declared.
        const auto tmp = s::case_dir();
        s::project{tmp / "widget", {{"mcpp.toml", R"cpp([package]
name    = "widget"
version = "0.1.0"
[targets.widget]
kind = "lib"
)cpp"}, {"src/widget.cppm", R"cpp(export module widget;
export int widget_anchor() { return 0; }
)cpp"}}};
        s::project{tmp / "lib", {{"mcpp.toml", R"cpp([package]
name    = "mylib"
version = "0.1.0"

[build]
sources = ["src/mylib.cpp"]

# Exactly one of these matches on any of the three CI platforms, so the
# `codegen` feature always pulls widget in.
[target.'cfg(unix)'.feature-deps.codegen]
widget = { path = "../widget", visibility = "public" }
[target.'cfg(windows)'.feature-deps.codegen]
widget = { path = "../widget", visibility = "public" }

# Never matches. If a non-matching section were merged, resolving this bogus
# path would fail the build.
[target.'cfg(arch = "no_such_arch")'.feature-deps.codegen]
ghost = { path = "../this_path_does_not_exist" }
)cpp"}, {"src/mylib.cpp", "int mylib_fn(){return 1;}\n"}}};
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"

[dependencies]
mylib = { path = "../lib", features = ["codegen"] }
)cpp"}, {"src/main.cpp", R"cpp(import widget;   // only reachable if the cfg-matched feature-dep resolved
int main() { return widget_anchor(); }
)cpp"}}};
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "a matching [target.*.feature-deps] was not merged";
        expect(fatal(!app.exists("mcpp.lock") || s::lacks(app.read("mcpp.lock"), "ghost"))) << "a non-matching predicate's feature-dep was pulled in";
        s::project plain{tmp / "plain", {{"mcpp.toml", R"cpp([package]
name    = "plain"
version = "0.1.0"

[dependencies]
mylib = { path = "../lib" }
)cpp"}, {"src/main.cpp", "int main(){}\n"}}};
        expect(fatal(e2e::succeeded(plain.mcpp({"build"})))) << "plain build failed";
        expect(fatal(!plain.exists("mcpp.lock") || s::lacks(plain.read("mcpp.lock"), "widget"))) << "an inactive feature's conditional dep was resolved anyway";
        s::project{tmp / "nomatch", {{"mcpp.toml", R"cpp([package]
name    = "nomatchlib"
version = "0.1.0"

[build]
sources = ["src/nm.cpp"]

[target.'cfg(arch = "no_such_arch")'.feature-deps.codegen]
ghost = { path = "../this_path_does_not_exist" }
)cpp"}, {"src/nm.cpp", "int nm(){return 1;}\n"}}};
        s::project nmapp{tmp / "nmapp", {{"mcpp.toml", R"cpp([package]
name    = "nmapp"
version = "0.1.0"

[dependencies]
nomatchlib = { path = "../nomatch", features = ["codegen"] }
)cpp"}, {"src/main.cpp", "int main(){}\n"}}};
        auto b3 = nmapp.mcpp({"build"});
        expect(fatal(e2e::succeeded(b3))) << "requesting a conditionally-populated feature failed where no predicate matches";
        expect(s::lacks(s::text(b3), "does not declare requested feature")) << "the feature was reported as undeclared where no predicate matches";
    };
    "247 bare triple conditional native"_test = [] {
        // `[target.'<triple>'.build]` applies to a native build, not only to one with an
        // explicit `--target` (the macro NAME is read: MSVC spells `-D` as `/D`).
        s::project probe{s::files{{"src/main.cpp", "int main() { return 0; }\n"},
                                    {"mcpp.toml", "[package]\nname    = \"probe\"\nversion = \"0.1.0\"\n[targets.probe]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}}};
        expect(fatal(e2e::succeeded(probe.mcpp({"build"})))) << "discovery build failed";
        std::string host_triple;
        for (const auto& e : fs::directory_iterator(probe.at("target")))
            if (const auto n = e.path().filename().string(); !n.starts_with(".") && (host_triple.empty() || n < host_triple)) host_triple = n;
        expect(fatal(!host_triple.empty())) << "no target/<triple> dir";
        probe.write("mcpp.toml", std::format("[package]\nname    = \"probe\"\nversion = \"0.1.0\"\n[targets.probe]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n[target.'{}'.build]\ncxxflags = [\"-DMCPP_BARE_TRIPLE=1\"]\n\n[target.'cfg(arch = \"{}\")'.build]\ncxxflags = [\"-DMCPP_CFG_ALIAS=1\"]\n", host_triple, host_triple.substr(0, host_triple.find('-'))));
        auto count = [&](std::string_view macro) {
            for (const auto& f : s::find_files(probe.at("target"), R"(build\.ninja)"))
                if (f.generic_string().find("/.build-mcpp/") == std::string::npos) return s::count_lines(s::read_file(f), macro);
            return std::size_t{0};
        };
        probe.remove("target");
        expect(fatal(e2e::succeeded(probe.mcpp({"build"})))) << "native build failed";
        expect(fatal(count("MCPP_CFG_ALIAS") > 0)) << "cfg() did not apply on a native build";
        expect(fatal(count("MCPP_BARE_TRIPLE") > 0)) << "[target.'" << host_triple << "'.build] was inert on a native build";
        probe.remove("target");
        expect(fatal(e2e::succeeded(probe.mcpp({"build", "--target", host_triple})))) << "explicit build failed";
        expect(count("MCPP_BARE_TRIPLE") > 0) << "bare triple inert with --target";
        expect(count("MCPP_CFG_ALIAS") > 0) << "cfg() inert with --target";
    };
    "677 a conditional dependency replaces the unconditional one"_test[tag{"requires:elf"}] = [] {
        // A matching `[target.<sel>.dependencies]` declaration replaces the unconditional one of
        // the same identity (its linkage applies, the graph records its table); a non-matching
        // one does not; a table naming no source is refused with the restated line; a misspelt
        // section is reported.
        s::needs({"elf"});
        const auto tmp = s::case_dir();
        s::project{tmp / "fw", {{"mcpp.toml", R"cpp([package]
namespace = "demo"
name      = "fw"
version   = "0.1.0"
[targets.fw]
kind = "lib"
)cpp"}, {"src/fw.cppm", "export module fw;\nexport int fw_anchor() { return 41; }\n"}}};
        auto make_app = [&](std::string_view dir, std::string_view tail) {
            return s::project{tmp / dir, {{"mcpp.toml", std::format("[package]\nname = \"{}\"\nversion = \"0.1.0\"\n{}", dir, tail)},
                                          {"src/main.cpp", "import fw;\nint main() { return fw_anchor() == 41 ? 0 : 1; }\n"}}};
        };
        auto table_of = [](const s::project& p, std::string_view canonical) {
            const auto json = s::find_file(p.at("target"), "resolution.json");
            expect(fatal(json.has_value())) << "no resolution.json";
            std::string out;
            for (const auto& pkg : s::json_file(*json)["graph"]["packages"])
                if (pkg["package"]["canonical"].get<std::string>() == canonical)
                    for (const auto& r : pkg["requested_by"]) out += (out.empty() ? "" : " ") + r["table"].get<std::string>();
            return out;
        };
        auto matching = make_app("matching", "[dependencies]\ndemo.fw = { path = \"../fw\" }\n[target.'cfg(os = \"linux\")'.dependencies]\ndemo.fw = { path = \"../fw\", linkage = \"shared\" }\n");
        expect(fatal(e2e::succeeded(matching.mcpp({"build"})))) << "A: build failed";
        expect(fatal(s::find_file(matching.at("target"), "libfw.so").has_value())) << "A: the conditional linkage did not produce libfw.so";
        const auto exe = s::find_file(matching.at("target"), "matching");
        expect(fatal(exe.has_value())) << "A: no program";
        bool needs = false;
        for (const auto& n : s::read_header(*exe).needed) needs = needs || n.find("libfw.so") != std::string::npos;
        expect(fatal(needs)) << "A: the program does not NEED libfw.so";
        expect(fatal(eq(table_of(matching, "demo.fw@0.1.0"), std::string{"[target.'cfg(os = \"linux\")'.dependencies]"}))) << "A: graph table";
        auto other = make_app("other", "[dependencies]\ndemo.fw = { path = \"../fw\" }\n[target.'cfg(os = \"windows\")'.dependencies]\ndemo.fw = { path = \"../fw\", linkage = \"shared\" }\n");
        expect(fatal(e2e::succeeded(other.mcpp({"build"})))) << "B: build failed";
        expect(fatal(!s::find_file(other.at("target"), "libfw.so").has_value())) << "B: a non-matching selector produced libfw.so";
        expect(fatal(eq(table_of(other, "demo.fw@0.1.0"), std::string{"[dependencies]"}))) << "B: graph table";
        auto modifier = make_app("modifier", "[dependencies]\ndemo.fw = { path = \"../fw\" }\n[target.'cfg(os = \"linux\")'.dependencies]\ndemo.fw = { linkage = \"shared\" }\n");
        auto c = modifier.mcpp({"build"});
        expect(fatal(failed(c))) << "C: a table that names no source built";
        expect(fatal(e2e::contains(s::text(c), "[target.'cfg(os = \"linux\")'.dependencies] demo.fw.linkage = \"shared\""))) << "C: the warning does not name the conditional table";
        expect(fatal(e2e::contains(s::text(c), "demo.fw = { path = \"../fw\", linkage = \"shared\" }"))) << "C: the warning does not restate the dependency with its source";
        auto typo = make_app("typo", "[dependencies]\ndemo.fw = { path = \"../fw\" }\n[target.'cfg(os = \"linux\")'.dependecies]\ndemo.fw = { path = \"../fw\", linkage = \"shared\" }\n");
        auto dd = typo.mcpp({"build"});
        expect(fatal(e2e::succeeded(dd))) << "D: build failed";
        expect(e2e::contains(s::text(dd), "[target.'cfg(os = \"linux\")'.dependecies] is not a section mcpp reads")) << "D: the misspelled section was not reported";
    };
    "813 conditional dialect cxxflags root only"_test[tag{"requires:gcc"}] = [] {
        // `[target.<selector>.build] dialect_cxxflags` is a graph-wide switch under a target
        // condition (#717): a matching row reaches the std BMI, the scan and every TU after the
        // root's own [build]; a non-matching one contributes nothing; switching rows rebuilds the
        // std BMI and switching back reuses it; a dependency's own key reaches no command.
        s::needs({"gcc"});
        const auto tmp = s::case_dir();
        const s::options o{.where = s::home::inherited, .env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};
        const std::vector<std::string> build{"build", "--toolchain", "gcc@16.1.0"};
        const std::string main_cpp = "import std;\nint main() { std::println(\"ok\"); return 0; }\n";
        // The first std-module record of the home whose text holds `needle` (`grep -rl | head -1`).
        auto std_module_json_for = [](std::string_view needle) -> std::optional<fs::path> {
            const auto root = s::case_home(s::home::inherited) / "build-cache/v1/std";
            if (!fs::exists(root)) return std::nullopt;
            std::vector<fs::path> files;
            for (const auto& e : fs::recursive_directory_iterator(root))
                if (e.is_regular_file()) files.push_back(e.path());
            std::ranges::sort(files);
            for (const auto& f : files)
                if (s::read_file(f).find(needle) != std::string::npos) return f;
            return std::nullopt;
        };

        // 1: a matching row reaches the std BMI, the scan and every TU, after the root's own [build].
        s::project one{tmp / "one", {{"mcpp.toml", R"cpp([package]
name     = "app717a"
version  = "0.1.0"
standard = "c++23"

[build]
dialect_cxxflags = ["-DBASE717"]

[targets.app717a]
kind = "bin"
main = "src/main.cpp"

[target.linux.build]
dialect_cxxflags = ["-DX717A"]
)cpp"}, {"src/main.cpp", main_cpp}}};
        expect(fatal(e2e::succeeded(one.mcpp(build, o)))) << "the build with a matching [target.linux.build] row failed";
        const auto n1 = ninjas(one);
        expect(fatal(!n1.empty())) << "no build.ninja produced";
        std::string cxxflags_line;
        for (const auto& l : s::lines_with(s::read_file(n1.front()), "cxxflags"))
            if (l.starts_with("cxxflags")) { cxxflags_line = l; break; }
        expect(fatal(e2e::contains(cxxflags_line, "-DX717A"))) << "the row's flag is not in build.ninja's global cxxflags (reaches neither scan nor TUs)";
        expect(fatal(e2e::contains(cxxflags_line, "-DBASE717"))) << "the root's own unconditional flag is missing from cxxflags";
        expect(fatal(cxxflags_line.find("-DBASE717") < cxxflags_line.find("-DX717A"))) << "the conditional row's flag does not follow the root's own [build] flag";
        expect(fatal(e2e::contains(text_of(one.at("compile_commands.json")), "-DX717A"))) << "-DX717A is missing from compile_commands.json";
        const auto stdA = std_module_json_for("-DX717A");
        expect(fatal(stdA.has_value())) << "no std-module.json records a std_flag containing -DX717A";
        expect(fatal(e2e::contains(s::read_file(*stdA), "-DBASE717"))) << "the std module's cached prebuild command is missing the root's unconditional flag";

        // 2: a selector that does not match the resolved target contributes nothing.
        s::project two{tmp / "two", {{"mcpp.toml", R"cpp([package]
name     = "app717b"
version  = "0.1.0"
standard = "c++23"

[build]
dialect_cxxflags = ["-DBASE717"]

[targets.app717b]
kind = "bin"
main = "src/main.cpp"

[target.windows.build]
dialect_cxxflags = ["-DX717A"]
)cpp"}, {"src/main.cpp", main_cpp}}};
        expect(fatal(e2e::succeeded(two.mcpp(build, o)))) << "the build with a non-matching [target.windows.build] row failed";
        const auto n2 = ninjas(two);
        expect(fatal(!n2.empty())) << "no build.ninja produced";
        const auto ninja2 = s::read_file(n2.front());
        expect(fatal(s::lacks(ninja2 + text_of(two.at("compile_commands.json")), "-DX717A"))) << "-DX717A from a non-matching [target.windows.build] row reached a Linux build";
        bool base = false;
        for (const auto& l : s::lines_with(ninja2, "-DBASE717")) base = base || l.starts_with("cxxflags");
        expect(fatal(base)) << "the root's own unconditional flag is missing";

        // 3: switching between a matching and a non-matching row rebuilds the std BMI (A, B, A).
        s::project cyc{tmp / "cyc", {{"src/main.cpp", main_cpp}}};
        auto write_cyc = [&](std::string_view selector) { cyc.write("mcpp.toml", std::format(R"cpp([package]
name     = "app717c"
version  = "0.1.0"
standard = "c++23"

[targets.app717c]
kind = "bin"
main = "src/main.cpp"

[target.{0}.build]
dialect_cxxflags = ["-DCYC717"]
)cpp", selector)); };
        auto cyc_build = [&](std::string_view selector) {
            write_cyc(selector);
            cyc.remove("target");
            cyc.remove("compile_commands.json");
            auto r = cyc.mcpp(build, o);
            const auto n = ninjas(cyc);
            return std::pair{e2e::succeeded(r), n.empty() ? std::string{} : s::read_file(n.front())};
        };
        auto [a1_ok, a1_ninja] = cyc_build("linux");
        expect(fatal(a1_ok)) << "build A (matching) failed";
        expect(fatal(e2e::contains(a1_ninja, "-DCYC717"))) << "build A did not carry -DCYC717";
        const auto stdA1 = std_module_json_for("-DCYC717");
        expect(fatal(stdA1.has_value())) << "no std-module.json for build A";
        auto [b_ok, b_ninja] = cyc_build("windows");
        expect(fatal(b_ok)) << "build B (non-matching) failed";
        expect(fatal(s::lacks(b_ninja + text_of(cyc.at("compile_commands.json")), "-DCYC717"))) << "build B (windows, non-matching on a Linux host) still carries -DCYC717";
        auto [a2_ok, a2_ninja] = cyc_build("linux");
        expect(fatal(a2_ok)) << "build A2 (matching again) failed";
        expect(fatal(e2e::contains(a2_ninja, "-DCYC717"))) << "build A2 did not carry -DCYC717";
        const auto stdA2 = std_module_json_for("-DCYC717");
        expect(fatal(stdA2.has_value())) << "no std-module.json for build A2";
        expect(fatal(*stdA2 == *stdA1)) << "switching back to the matching row did not reuse build A's std BMI cache entry: " << stdA1->string() << " then " << stdA2->string();

        // 4: a dependency's own dialect_cxxflags reaches no command and does not move the fingerprint.
        s::project dep{tmp / "dep717", {{"mcpp.toml", R"cpp([package]
name     = "dep717"
version  = "0.1.0"
standard = "c++23"

[targets.dep717]
kind = "lib"

[build]
dialect_cxxflags = ["-DDEPFLAG_V1"]
)cpp"}, {"src/dep717.cppm", "export module dep717;\nexport int value() { return 42; }\n"}}};
        s::project four{tmp / "four", {{"mcpp.toml", R"cpp([package]
name     = "app717d"
version  = "0.1.0"
standard = "c++23"

[dependencies]
dep717 = { path = "../dep717" }

[targets.app717d]
kind = "bin"
main = "src/main.cpp"
)cpp"}, {"src/main.cpp", "import dep717;\nint main() { return value() == 42 ? 0 : 1; }\n"}}};
        expect(fatal(e2e::succeeded(four.mcpp(build, o)))) << "the first build (dependency carries -DDEPFLAG_V1) failed";
        const auto n4a = ninjas(four);
        expect(fatal(n4a.size() == 1u)) << "more than one build.ninja after the first build";
        expect(fatal(s::lacks(s::read_file(n4a.front()) + text_of(four.at("compile_commands.json")), "-DDEPFLAG_V1"))) << "the dependency's own dialect_cxxflags reached a command";
        auto m = dep.read("mcpp.toml");
        m.replace(m.find("-DDEPFLAG_V1"), 12, "-DDEPFLAG_V2");
        dep.write("mcpp.toml", m);
        expect(fatal(e2e::succeeded(four.mcpp(build, o)))) << "the second build (dependency's flag changed) failed";
        const auto n4b = ninjas(four);
        expect(fatal(n4b.size() == 1u)) << "changing the dependency's own dialect_cxxflags left a second output directory behind (the fingerprint moved)";
        expect(fatal(n4b.front() == n4a.front())) << "changing the dependency's own dialect_cxxflags moved the graph's output directory";
        expect(s::lacks(s::read_file(n4b.front()) + text_of(four.at("compile_commands.json")), "-DDEPFLAG_V2")) << "the dependency's changed dialect_cxxflags reached a command";
    };

    "895 conditional configuration rows count as they evaluate"_test[tag{"requires:gcc"}] = [] {
        // A member's conditional rows count as they evaluate for the build (#786): its own
        // `[target.<sel>.build] dialect_cxxflags` reaches the build under a virtual root (R1); a
        // row for another target does not split the members (R2); rows that evaluate
        // differently do, each member keeping its own dialect (R3).
        s::needs({"gcc"});
        const auto tmp = s::case_dir();
        const s::options inherited{.where = s::home::inherited};
        const std::string host = R"(cfg(any(os = "linux", os = "macos", os = "windows")))";
        auto member = [](const s::project& ws, std::string_view name, std::string_view extra) {
            ws.write(std::format("{0}/mcpp.toml", name),
                     std::format("[package]\nname = \"{0}\"\nversion = \"0.1.0\"\n[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n{1}\n", name, extra));
            ws.write(std::format("{}/src/main.cpp", name),
                     "#include <cstdio>\n#ifndef DIAL\n#define DIAL 0\n#endif\nint main() { std::printf(\"DIAL=%d\\n\", DIAL); }\n");
        };
        auto run_of = [](const s::project& ws, std::string_view name) -> std::string {
            for (const auto& f : s::find_files(ws.at("target"), std::format("{0}|{0}\\.exe", name))) {
                const auto g = f.generic_string();
                if (g.find(std::format("/bin/{}", name)) == std::string::npos) continue;
                auto r = s::run(f);
                auto out = r.out;
                std::erase(out, '\r');
                while (!out.empty() && out.back() == '\n') out.pop_back();
                return out;
            }
            return "(no binary)";
        };

        s::project r1{tmp / "r1", {{"mcpp.toml", "[workspace]\nmembers = [\"app\"]\n"}}};
        member(r1, "app", std::format("[target.'{}'.build]\ndialect_cxxflags = [\"-DDIAL=2\"]", host));
        expect(fatal(e2e::succeeded(r1.mcpp({"build", "-p", "app"}, inherited)))) << "build";
        const auto got = run_of(r1, "app");
        expect(fatal(got == "DIAL=2")) << "the member's own conditional dialect_cxxflags did not reach the build: " << got;

        s::project r2{tmp / "r2", {{"mcpp.toml", "[workspace]\nmembers = [\"a\", \"b\"]\n"}}};
        member(r2, "a", "[target.'cfg(os = \"none\")'.build]\ndialect_cxxflags = [\"-DNEVER=1\"]");
        member(r2, "b", "");
        expect(fatal(e2e::succeeded(r2.mcpp({"build", "--workspace"}, inherited)))) << "--workspace";
        expect(fatal(ninjas(r2).size() == 1u)) << "a row for another target split the members into " << ninjas(r2).size() << " configurations";

        s::project r3{tmp / "r3", {{"mcpp.toml", "[workspace]\nmembers = [\"a\", \"b\"]\n"}}};
        member(r3, "a", std::format("[target.'{}'.build]\ndialect_cxxflags = [\"-DDIAL=3\"]", host));
        member(r3, "b", "");
        expect(fatal(e2e::succeeded(r3.mcpp({"build", "--workspace"}, inherited)))) << "--workspace";
        expect(fatal(ninjas(r3).size() == 2u)) << "members whose rows evaluate differently share " << ninjas(r3).size() << " configuration(s)";
        const auto a = run_of(r3, "a");
        expect(fatal(a == "DIAL=3")) << "member a lost its dialect: " << a;
        const auto b = run_of(r3, "b");
        expect(b == "DIAL=0") << "member b received a's dialect: " << b;
    };
}};

}  // namespace
