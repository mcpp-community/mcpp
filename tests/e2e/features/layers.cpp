// Layer predicates and runtime tables in target-conditional configuration, a requirement on
// the target axis, a requirement a layer decides (formerly tests/e2e/328, 637, 654, 1007).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

const s::options cdb{.env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};

E2E layers{"layers", [] {
    "328 layer predicates apply and unknown ones say so"_test = [] {
        // `cfg(c-abi = ...)` applies when the resolved c-abi matches and not otherwise, once
        // beside a triple leg, in a dependency too; an unknown cfg key is named with the
        // vocabulary (an error under --strict); a layer-conditioned dependency is reported while
        // the section's build inputs still apply.
        const auto tmp = s::case_dir();
        s::project probe{tmp / "probe", {{"mcpp.toml", R"cpp([package]
name    = "probe"
version = "0.1.0"
)cpp"}, {"src/main.cpp", "int main() { return 0; }\n"}}};
        auto report = probe.mcpp({"build"}, {.env = {{"MCPP_VERBOSE", "1"}, {"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}});
        expect(fatal(e2e::succeeded(report))) << "the probe build errored";
        std::smatch m;
        const auto text = s::text(report);
        expect(fatal(std::regex_search(text, m, std::regex{R"(c-abi  *([A-Za-z0-9_+-]*))"}))) << "could not read the resolved c-abi";
        const std::string cabi = m[1].str(), notcabi = "not-" + cabi;
        s::project layers{tmp / "layers", {{"src/main.cpp", R"cpp(#ifndef PROBE_MATCHED
#error "a matching cfg(c-abi = ...) section did not apply"
#endif
#ifdef PROBE_MUST_NOT_APPLY
#error "a NON-matching cfg(c-abi = ...) section applied"
#endif
#ifndef PROBE_COMBINED
#error "cfg(all(unix, c-abi = ...)) did not apply"
#endif
int main() { return 0; }
)cpp"}}};
        layers.write("mcpp.toml", std::format(R"cpp([package]
name    = "layers"
version = "0.1.0"

[target.'cfg(c-abi = "{0}")'.build]
defines = ["PROBE_MATCHED=1"]

[target.'cfg(c-abi = "{1}")'.build]
defines = ["PROBE_MUST_NOT_APPLY=1"]

[target.'cfg(all(any(unix, windows), c-abi = "{0}"))'.build]
defines = ["PROBE_COMBINED=1"]
)cpp", cabi, notcabi));
        expect(fatal(e2e::succeeded(layers.mcpp({"build"}, cdb)))) << "layer predicates did not behave";
        s::project once{tmp / "once", {{"src/main.cpp", "int main() { return 0; }\n"}}};
        once.write("mcpp.toml", std::format(R"cpp([package]
name    = "once"
version = "0.1.0"

[target.'cfg(any(unix, windows, c-abi = "{0}"))'.build]
defines = ["PROBE_ONCE=1"]
)cpp", cabi));
        expect(fatal(e2e::succeeded(once.mcpp({"build"}, cdb)))) << "the mixed-predicate build errored";
        const auto db = once.read("compile_commands.json");
        const std::regex flag{R"([-/]DPROBE_ONCE=1)"};
        const auto n = std::distance(std::sregex_iterator(db.begin(), db.end(), flag), std::sregex_iterator());
        expect(fatal(eq(n, std::ptrdiff_t{1}))) << "a predicate with both a triple leg and a layer leg contributed " << n << " times, expected 1";
        s::project lib{tmp / "dep/lib", {{"src/layerlib.cppm", R"cpp(export module layerlib;
#ifndef LIB_MATCHED
#error "a DEPENDENCY's matching cfg(c-abi = ...) section did not apply"
#endif
#ifdef LIB_MUST_NOT_APPLY
#error "a DEPENDENCY's non-matching cfg(c-abi = ...) section applied"
#endif
export int lib_answer() { return 42; }
)cpp"}}};
        lib.write("mcpp.toml", std::format(R"cpp([package]
name    = "layerlib"
version = "0.1.0"

[target.'cfg(c-abi = "{0}")'.build]
defines = ["LIB_MATCHED=1"]

[target.'cfg(c-abi = "{1}")'.build]
defines = ["LIB_MUST_NOT_APPLY=1"]
)cpp", cabi, notcabi));
        s::project app{tmp / "dep/app", {{"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"

[dependencies]
layerlib = { path = "../lib" }
)cpp"}, {"src/main.cpp", R"cpp(import layerlib;
int main() { return lib_answer() == 42 ? 0 : 1; }
)cpp"}}};
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, cdb)))) << "layer predicates did not reach a dependency";
        s::project unknown{tmp / "unknown", {{"mcpp.toml", R"cpp([package]
name    = "unknown"
version = "0.1.0"

[target.'cfg(no-such-key = "x")'.build]
defines = ["NEVER"]
)cpp"}, {"src/main.cpp", "int main() { return 0; }\n"}}};
        auto w = unknown.mcpp({"build"}, cdb);
        expect(fatal(e2e::succeeded(w))) << "an unknown cfg key must warn, not fail";
        expect(fatal(e2e::contains(s::text(w), "no-such-key"))) << "the unknown cfg key was not named";
        expect(fatal(e2e::contains(s::text(w), "c-abi"))) << "the message does not list the supported keys";
        expect(fatal(failed(unknown.mcpp({"build", "--strict"}, cdb)))) << "--strict did not turn the warning into an error";
        s::project depcond{tmp / "depcond", {{"src/main.cpp", R"cpp(#ifndef INPUTS_STILL_APPLY
#error "the warning about dependencies also suppressed the build inputs"
#endif
int main() { return 0; }
)cpp"}}};
        depcond.write("mcpp.toml", std::format(R"cpp([package]
name    = "depcond"
version = "0.1.0"

[target.'cfg(c-abi = "{1}")'.dependencies]
never-resolved = "1.0.0"

[target.'cfg(c-abi = "{0}")'.build]
defines = ["INPUTS_STILL_APPLY=1"]
)cpp", cabi, notcabi));
        auto dc = depcond.mcpp({"build"}, cdb);
        expect(fatal(e2e::succeeded(dc))) << "a layer-conditioned dependency must warn, not fail";
        expect(e2e::contains(s::text(dc), "conditions dependencies on a target-side layer")) << "no diagnostic for a layer-conditioned dependency";
    };

    "637 a conditional runtime table alone is applied"_test[s::gcc] = [] {
        // `[target.<pred>.runtime] libraries` reaches the link line when it is alone under its
        // predicate and beside another table; a non-matching predicate contributes nothing; a
        // typo in either runtime table is reported while the neighbouring key applies.
        const auto tmp = s::case_dir();
        auto graph = [](const s::project& p) {
            const auto nj = s::find_file(p.at("target"), "build.ninja");
            expect(fatal(nj.has_value())) << "no build graph";
            return s::read_file(*nj);
        };
        for (auto [leg, extra] : {std::pair<std::string_view, std::string_view>{"alone", ""}, {"sibling", "[target.linux.build]\ndefines = [\"UNRELATED_TO_LIBRARIES\"]"}}) {
            s::project p{tmp / leg, {{"src/main.cpp", R"cpp(int main() { return 0; }
)cpp"}}};
            p.write("mcpp.toml", std::string(R"cpp([package]
name    = "condrt"
version = "0.1.0"

[target.linux.runtime]
libraries = ["m"]
)cpp") + std::string(extra) + "\n");
            expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "[" << leg << "] build";
            expect(fatal(s::has_line(graph(p), R"(-lm\b)"))) << "[" << leg << "] [target.linux.runtime] libraries did not reach the link line";
        }
        s::project offhost{tmp / "offhost", {{"src/main.cpp", R"cpp(int main() { return 0; }
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "condrt"
version = "0.1.0"

[target.windows.runtime]
libraries = ["user32"]
)cpp"}}};
        expect(fatal(e2e::succeeded(offhost.mcpp({"build"})))) << "[offhost] build";
        expect(fatal(s::lacks(graph(offhost), "-luser32"))) << "[offhost] a windows predicate contributed to a linux link line";
        s::project typo{tmp / "typo", {{"src/main.cpp", R"cpp(int main() { return 0; }
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "condrt"
version = "0.1.0"

[runtime]
dlopen_lib = ["one"]

[target.linux.runtime]
libraries = ["m"]
framework = ["Cocoa"]
)cpp"}}};
        auto t = typo.mcpp({"build"});
        expect(fatal(e2e::succeeded(t))) << "[typo] build";
        expect(e2e::contains(s::text(t), "unsupported key 'dlopen_lib'")) << "[typo] a plausible typo in [runtime] was accepted in silence";
        expect(e2e::contains(s::text(t), "unsupported key 'framework'")) << "[typo] a plausible typo in [target.<pred>.runtime] was accepted in silence";
        expect(s::has_line(graph(typo), R"(-lm\b)")) << "[typo] the correctly spelled key stopped working";
    };
    "654 a requirement on the target axis"_test[s::elf][s::gcc] = [] {
        // `requires_abi` under `[target.<sel>]` and `[target.<sel>.feature-requires-abi]` joins
        // the package's requirements only when the selector matches: a non-matching one imposes
        // nothing; a matching one is refused without the root's table, naming the dependency and
        // the selector, and satisfied by it (-pthread, its own fingerprint); per feature too.
        const auto tmp = s::case_dir();
        s::project dep{tmp / "tgtdep", {{"src/tgtdep.cppm", "export module tgtdep;\nexport int tgt_value() { return 7; }\n"}}};
        s::project app{tmp / "app", {{"src/main.cpp", R"cpp(import tgtdep;
int main() { return tgt_value() == 7 ? 0 : 1; }
)cpp"}}};
        auto write_dep = [&](std::string_view extra) { dep.write("mcpp.toml", std::format(R"cpp([package]
name    = "tgtdep"
version = "0.1.0"

[targets.tgtdep]
kind = "lib"

[features]
mt = {{}}

{0}
)cpp", extra)); };
        auto write_app = [&](std::string_view table, std::string_view trailing) { app.write("mcpp.toml", std::format(R"cpp([package]
name    = "app"
version = "0.1.0"

[dependencies]
tgtdep = {0}

{1}
)cpp", table, trailing)); };
        auto newest_ninja = [&] {
            std::optional<fs::path> best;
            for (const auto& f : s::find_files(app.at("target"), R"(build\.ninja)"))
                if (f.generic_string().find("/.build-mcpp/") == std::string::npos && (!best || fs::last_write_time(f) > fs::last_write_time(*best))) best = f;
            expect(fatal(best.has_value())) << "no build.ninja";
            return *best;
        };
        auto pthread_in_dep_command = [&] {
            auto db = newest_ninja().parent_path() / "compile_commands.json";
            if (!fs::exists(db)) db = s::newest_file(app.dir(), "compile_commands.json").value_or(db);
            auto text = s::read_file(db);
            std::erase(text, '\n');
            const std::regex unit{R"(\{[^{}]*tgtdep\.cppm[^{}]*\})"};
            for (std::sregex_iterator it(text.begin(), text.end(), unit), end; it != end; ++it)
                if (it->str().find("-pthread") != std::string::npos) return true;
            return false;
        };
        const std::string threads_table = "[target.'cfg(linux)'.abi]\nthreads = true\n";
        const std::string refusal = "`tgtdep` requires the artefact's ABI to have threads ([target.'cfg(linux)'])";
        write_dep("[target.'cfg(windows)']\nrequires_abi = { threads = true }\n");
        write_app("{ path = \"../tgtdep\" }", "");
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, cdb)))) << "a windows-only target-axis requirement failed a host build";
        const auto plain_dir = newest_ninja().parent_path();
        expect(fatal(!pthread_in_dep_command())) << "control: -pthread is on the dependency's compile command with no root table";
        write_dep("[target.'cfg(linux)']\nrequires_abi = { threads = true }\n");
        auto refused = app.mcpp({"build"}, cdb);
        expect(fatal(failed(refused))) << "a target-axis threads requirement was built without the root's table";
        expect(fatal(e2e::contains(s::text(refused), refusal))) << "the refusal does not name the dependency and the selector";
        write_app("{ path = \"../tgtdep\" }", threads_table);
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, cdb)))) << "the build with the root's table failed";
        expect(fatal(pthread_in_dep_command())) << "-pthread did not reach the dependency's compile command";
        expect(fatal(newest_ninja().parent_path() != plain_dir)) << "the build with threads reused the fingerprint directory of the plain build";
        expect(fatal(e2e::succeeded(app.mcpp({"run"}, cdb)))) << "the program did not run";
        app.remove("target");
        write_dep("[target.'cfg(linux)'.feature-requires-abi]\nmt = { threads = true }\n");
        write_app("{ path = \"../tgtdep\" }", "");
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, cdb)))) << "an inactive feature's target-axis requirement failed the build";
        expect(fatal(!pthread_in_dep_command())) << "an inactive feature's requirement put -pthread on the compile command";
        write_app("{ path = \"../tgtdep\", features = [\"mt\"] }", "");
        auto active = app.mcpp({"build"}, cdb);
        expect(fatal(failed(active))) << "an active feature's target-axis requirement was built without the root's table";
        expect(fatal(e2e::contains(s::text(active), refusal))) << "the refusal does not name the selector for the active feature's requirement";
        write_app("{ path = \"../tgtdep\", features = [\"mt\"] }", threads_table);
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, cdb)))) << "the active feature's requirement was not satisfied by the root's table";
        expect(pthread_in_dep_command()) << "-pthread did not reach the dependency's compile command";
    };
    "1007 a requirement a layer decides is checked"_test[s::unix_shell] = [] {
        // A requirement stated in a table whose selector names a layer is checked once the
        // layers are resolved, as a requirement of [package] is: refused before anything
        // compiles, naming the dependency and what it requires; a non-matching table states nothing.
        const auto tmp = s::case_dir();
        s::project needs{tmp / "needs", {{"src/needs.cpp", "int needs() { return 7; }\n"}}};
        auto write_needs = [&](std::string_view selector) { needs.write("mcpp.toml", std::format(R"cpp([package]
name    = "needs"
version = "0.1.0"

[target.'{0}']
requires = ["mcpp:compiler-runtime=nonesuch"]

[targets.needs]
kind = "lib"
)cpp", selector)); };
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"

[dependencies]
needs = { path = "../needs" }
)cpp"}, {"src/main.cpp", "int needs();\nint main() { return needs() == 7 ? 0 : 1; }\n"}}};
        write_needs(R"(cfg(not(compiler = "nonesuch")))");
        auto a = app.mcpp({"build"});
        expect(fatal(failed(a))) << "A: a requirement a layer decides was not checked\n" << s::text(a);
        expect(fatal(e2e::contains(s::text(a), "needs@0.1.0` requires the compiler-runtime to be `nonesuch`"))) << "A: the refusal does not name the dependency, the layer and what it requires\n" << s::text(a);
        expect(fatal(s::lacks(s::text(a), "Compiling"))) << "A: something was compiled before the refusal";
        write_needs(R"(cfg(compiler = "nonesuch"))");
        app.remove("target");
        auto b = app.mcpp({"build"});
        expect(e2e::succeeded(b)) << "B: a table this build does not match stated a requirement\n" << s::text(b);
    };
}};

}  // namespace
