// Actions a build program declares instead of doing: each role's edge, re-running only on a
// changed input, `$` in a command, target references, object actions in every image, malformed
// declarations (formerly tests/e2e/188).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

long long mtime(const fs::path& p) {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::file_clock::to_sys(fs::last_write_time(p)).time_since_epoch()).count();
}

std::optional<fs::path> ninja_file(const fs::path& target) {
    for (const auto& f : s::find_files(target, R"(build\.ninja)"))
        if (f.generic_string().find("/.build-mcpp/") == std::string::npos) return f;
    return std::nullopt;
}

E2E declared{"declared", [] {
    "188 build actions"_test[tag{"requires:gcc"}] = [] {
        // `mcpp:action=`: a build program declares work and the work becomes an edge: a
        // source action's output is compiled, a check's stamp and an artifact appear; an action
        // re-runs when its input or its command changes and not otherwise; a failing check
        // fails the build; a companion header is not a translation unit; a literal `$`
        // survives; an unknown target reference is refused (also beside a known one); an object
        // action reaches the link, the tests and is reported when no target takes it; a
        // malformed action is refused.
        s::needs({"gcc"});
        const auto tmp = s::case_dir();
        s::project app{tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n"},
                                     {"src/main.cpp", "#include <cstdio>\nint generated_value();\nint main() { std::printf(\"VALUE=%d\\n\", generated_value()); }\n"},
                                     {"data/value.txt", "11\n"},
                                     {"build.mcpp", R"cpp(#include <cstdio>
#include <string>
import mcpp;
int main() {
    const std::string root = mcpp::manifest_dir();
    const std::string out  = std::string(mcpp::out_dir()) + "/gen.cpp";

    mcpp::action a;
    a.id = "generate";
    a.role = "source";
    a.arg((root + "/gen.sh").c_str()).arg((root + "/data/value.txt").c_str()).arg(out.c_str())
     .input((root + "/data/value.txt").c_str())
     .output(out.c_str())
     .submit();

    mcpp::action c;
    c.id = "lint";
    c.role = "check";
    c.arg((root + "/check.sh").c_str()).arg("${mcpp.out_dir}/lint.stamp")
     .arg((root + "/FAIL_THE_CHECK").c_str())
     .output("${mcpp.out_dir}/lint.stamp")
     .submit();

    mcpp::action p;
    p.id = "package";
    p.role = "artifact";
    p.arg((root + "/pack.sh").c_str()).arg("${mcpp.target_file:app}").arg("${mcpp.out_dir}/app.pack")
     .input("${mcpp.target_file:app}")
     .output("${mcpp.out_dir}/app.pack")
     .submit();
}
)cpp"}}};
        s::write_executable(app.at("gen.sh"), R"cpp(#!/usr/bin/env bash
printf 'int generated_value() { return %s; }\n' "$(cat "$1")" > "$2"
)cpp");
        s::write_executable(app.at("check.sh"), R"cpp(#!/usr/bin/env bash
# The marker path is passed in: ninja runs commands with cwd = the BUILD dir,
# not the project root, so a bare relative name would never be found.
[ -f "$2" ] && { echo "check: refusing"; exit 1; }
: > "$1"
)cpp");
        s::write_executable(app.at("pack.sh"), R"cpp(#!/usr/bin/env bash
# $1 = the linked binary, $2 = the artifact to produce
cp "$1" "$2"
)cpp");
        auto value = [&] { return s::last_starting(app.mcpp({"run"}).out, "VALUE="); };
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "build with actions failed";
        expect(fatal(eq(value(), std::string{"VALUE=11"}))) << "generated source not linked";
        const auto pack = s::find_file(app.at("target"), "app.pack");
        expect(fatal(pack.has_value())) << "artifact action produced nothing";
        const auto outdir = pack->parent_path();
        expect(fatal(fs::exists(outdir / "lint.stamp"))) << "check action produced no stamp";
        app.write("data/value.txt", "23\n");
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "rebuild failed";
        expect(fatal(eq(value(), std::string{"VALUE=23"}))) << "action did not re-run when its input changed";
        const auto gen = s::find_file(app.at("target"), "gen.cpp");
        expect(fatal(gen.has_value())) << "no gen.cpp";
        const auto before = mtime(*gen);
        fs::last_write_time(app.at("src/main.cpp"), fs::file_time_type::clock::now());
        std::this_thread::sleep_for(std::chrono::seconds{1});  // coarser than any filesystem's mtime granularity here
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "rebuild failed";
        expect(fatal(eq(mtime(*s::find_file(app.at("target"), "gen.cpp")), before))) << "the action re-ran although its inputs were unchanged";
        {
            auto program = app.read("build.mcpp");
            for (std::size_t at; (at = program.find("\"/data/value.txt\"")) != std::string::npos;)
                program.replace(at, 17, "\"/data/other.txt\"");
            app.write("build.mcpp", program);
        }
        app.write("data/other.txt", "31\n");
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "rebuild after editing build.mcpp failed";
        expect(fatal(eq(value(), std::string{"VALUE=31"}))) << "an edited action command did not take effect";
        app.write("FAIL_THE_CHECK", "");
        fs::remove(outdir / "lint.stamp");
        expect(fatal(failed(app.mcpp({"build"})))) << "a failing check did not fail the build";
        app.remove("FAIL_THE_CHECK");
        s::write_executable(app.at("genpair.sh"), R"cpp(#!/usr/bin/env bash
# $1 = .cc to write, $2 = .h to write
printf '#include "%s"\nint paired() { return 13; }\n' "$(basename "$2")" > "$1"
printf 'int paired();\n' > "$2"
)cpp");
        app.write("build.mcpp", R"cpp(#include <cstdio>
#include <string>
import mcpp;
int main() {
    const std::string root = mcpp::manifest_dir();
    const std::string out  = mcpp::out_dir();
    mcpp::action a;
    a.id = "pair"; a.role = "source";
    a.arg((root + "/genpair.sh").c_str())
     .arg((out + "/p.cc").c_str())
     .arg((out + "/p.h").c_str())
     .output((out + "/p.cc").c_str())
     .output((out + "/p.h").c_str())   // companion: produced, NOT compiled
     .submit();
    mcpp::include_dir(out.c_str());
    // Keep the earlier generated source in the build so main.cpp still links.
    mcpp::action g;
    g.id = "generate"; g.role = "source";
    g.arg((root + "/gen.sh").c_str()).arg((root + "/data/other.txt").c_str())
     .arg((out + "/gen.cpp").c_str())
     .input((root + "/data/other.txt").c_str())
     .output((out + "/gen.cpp").c_str())
     .submit();
}
)cpp");
        app.remove("target");
        auto b2c = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(b2c))) << "a companion (non-source) action output broke the build";
        expect(fatal(s::lacks(s::text(b2c), "object path collision"))) << "the header was adopted as a translation unit";

        s::project edge{tmp / "edge", {{"mcpp.toml", "[package]\nname    = \"edge\"\nversion = \"0.1.0\"\n"},
                                       {"src/main.cpp", "int main() {}\n"},
                                       {"build.mcpp", R"cpp(#include <cstdio>
#include <string>
import mcpp;
int main() {
    const std::string root = mcpp::manifest_dir();
    mcpp::action a;
    a.id = "dollar"; a.role = "check";
    a.arg((root + "/dollar.sh").c_str()).arg("-Wl,-rpath,$ORIGIN")
     .arg("${mcpp.out_dir}/dollar.stamp")
     .output("${mcpp.out_dir}/dollar.stamp")
     .submit();
}
)cpp"}}};
        s::write_executable(edge.at("dollar.sh"), R"cpp(#!/usr/bin/env bash
# $1 must arrive containing a literal dollar sign
case "$1" in *'$'*) : > "$2";; *) echo "lost the dollar: [$1]" >&2; exit 1;; esac
)cpp");
        edge.remove("target");
        expect(fatal(e2e::succeeded(edge.mcpp({"build"})))) << "a literal $ in an action command did not survive";
        edge.write("build.mcpp", R"cpp(#include <cstdio>
import mcpp;
int main() {
    mcpp::action a;
    a.id = "bad-ref"; a.role = "artifact";
    a.arg("/bin/true").arg("${mcpp.target_file:no_such_target}")
     .input("${mcpp.target_file:no_such_target}")
     .output("${mcpp.out_dir}/x.out")
     .submit();
}
)cpp");
        edge.remove("target");
        auto b3c = edge.mcpp({"build"});
        expect(fatal(failed(b3c))) << "an unknown target reference was accepted";
        expect(fatal(e2e::contains(s::text(b3c), "no_such_target"))) << "error does not name the unknown target";

        s::project objrole{tmp / "objrole", {{"mcpp.toml", "[package]\nname    = \"objrole\"\nversion = \"0.1.0\"\n"},
                                             {"src/main.cpp", "int main() { return 0; }\n"}}};
        expect(fatal(e2e::succeeded(objrole.mcpp({"build"})))) << "probe build failed";
        const auto obj_ninja = ninja_file(objrole.at("target"));
        expect(fatal(obj_ninja.has_value())) << "no build.ninja";
        std::smatch cxx_line;
        const auto ninja_text = s::read_file(*obj_ninja);
        expect(fatal(std::regex_search(ninja_text, cxx_line, std::regex{R"((^|\n)cxx *= *([^\n]*))"}))) << "could not read the compiler out of build.ninja";
        const auto obj_cxx = cxx_line[2].str();
        objrole.write("src/main.cpp", R"cpp(#include <cstdio>
extern "C" int blob_value();
int main() { std::printf("BLOB=%d\n", blob_value()); return blob_value() == 7 ? 0 : 1; }
)cpp");
        objrole.write("blob.cpp", "extern \"C\" int blob_value() { return 7; }\n");
        s::write_executable(objrole.at("mkobj.sh"), std::format("#!/usr/bin/env bash\n# $1 = source, $2 = object to produce\n{} -c \"$1\" -o \"$2\"\n", obj_cxx));
        objrole.write("build.mcpp", R"cpp(#include <cstdio>
#include <string>
import mcpp;
int main() {
    const std::string root = mcpp::manifest_dir();
    const std::string out  = mcpp::out_dir();
    mcpp::action o;
    o.id = "blob"; o.role = "object";
    o.arg((root + "/mkobj.sh").c_str())
     .arg((root + "/blob.cpp").c_str())
     .arg((out + "/blob.o").c_str())
     .input((root + "/blob.cpp").c_str())
     .output((out + "/blob.o").c_str())
     .submit();
}
)cpp");
        expect(fatal(e2e::succeeded(objrole.mcpp({"build"})))) << "role=object build failed";
        auto o2 = objrole.mcpp({"run"});
        expect(fatal(e2e::succeeded(o2))) << "the object was not linked in";
        expect(fatal(e2e::contains(s::text(o2), "BLOB=7"))) << "wrong value from the linked object";
        std::this_thread::sleep_for(std::chrono::seconds{1});
        objrole.write("blob.cpp", "extern \"C\" int blob_value() { return 9; }\n");
        auto o3 = objrole.mcpp({"run"});
        expect(fatal(failed(o3))) << "expected the changed object to be relinked (main asserts ==7)";
        expect(fatal(e2e::contains(s::text(o3), "BLOB=9"))) << "editing the object's input did not reach the link";
        objrole.write("blob.cpp", "extern \"C\" int blob_value() { return 7; }\n");
        objrole.write("build.mcpp", R"cpp(#include <cstdio>
#include <string>
import mcpp;
int main() {
    const std::string root = mcpp::manifest_dir();
    const std::string out  = mcpp::out_dir();
    mcpp::action o;
    o.id = "blob"; o.role = "object";
    o.arg((root + "/mkobj.sh").c_str())
     .arg((root + "/blob.cpp").c_str())
     .arg((out + "/blob.o").c_str())
     .input((root + "/blob.cpp").c_str())
     .output((out + "/blob.o").c_str())
     .target("no_such_target")
     .submit();
}
)cpp");
        objrole.remove("target");
        auto o4 = objrole.mcpp({"build"});
        expect(fatal(failed(o4))) << "role=object accepted an unknown target";
        expect(fatal(e2e::contains(s::text(o4), "no_such_target"))) << "error does not name the unknown target";
        {
            auto program = objrole.read("build.mcpp");
            const std::string_view one = ".target(\"no_such_target\")";
            program.replace(program.find(one), one.size(), ".target(\"objrole\").target(\"no_such_target\")");
            objrole.write("build.mcpp", program);
        }
        objrole.remove("target");
        auto o4b = objrole.mcpp({"build"});
        expect(fatal(failed(o4b))) << "an unknown target was silently dropped because a sibling matched";
        expect(fatal(e2e::contains(s::text(o4b), "no_such_target"))) << "error does not name the unknown target";

        s::project objtest{tmp / "objtest", {{"mcpp.toml", "[package]\nname    = \"objtest\"\nversion = \"0.1.0\"\n"},
                                             {"src/engine.cppm", "export module objtest.engine;\nextern \"C\" int blob_value();\nexport int get(){ return blob_value(); }\n"},
                                             {"src/main.cpp", "import objtest.engine;\nint main(){ return get()==7?0:1; }\n"},
                                             {"tests/t_engine.cpp", "import objtest.engine;\nint main(){ return get()==7?0:1; }\n"},
                                             {"blob.cpp", "extern \"C\" int blob_value() { return 7; }\n"},
                                             {"build.mcpp", R"cpp(#include <cstdio>
#include <string>
import mcpp;
int main() {
    const std::string root = mcpp::manifest_dir();
    const std::string out  = mcpp::out_dir();
    mcpp::action o;
    o.id = "blob"; o.role = "object";
    o.arg((root + "/mkobj.sh").c_str())
     .arg((root + "/blob.cpp").c_str())
     .arg((out + "/blob.o").c_str())
     .input((root + "/blob.cpp").c_str())
     .output((out + "/blob.o").c_str())
     .submit();                      // no .target(): every image, tests included
}
)cpp"}}};
        s::write_executable(objtest.at("mkobj.sh"), objrole.read("mkobj.sh"));
        expect(fatal(e2e::succeeded(objtest.mcpp({"build"})))) << "objtest build failed";
        auto o6 = objtest.mcpp({"test"});
        expect(fatal(e2e::succeeded(o6))) << "the test binary did not receive the object (undefined symbol)";
        expect(fatal(e2e::contains(s::text(o6), "1 passed"))) << "the test did not run";

        s::project objnone{tmp / "objnone", {{"mcpp.toml", "[package]\nname    = \"objnone\"\nversion = \"0.1.0\"\n\n[features]\ngated = {}\n\n[targets.objnone]\nkind = \"lib\"\nrequired_features = [\"gated\"]\n"},
                                             {"src/objnone.cppm", "export module objnone;\nexport int f(){return 1;}\n"},
                                             {"blob.cpp", objtest.read("blob.cpp")},
                                             {"build.mcpp", objtest.read("build.mcpp")}}};
        s::write_executable(objnone.at("mkobj.sh"), objtest.read("mkobj.sh"));
        auto o7 = objnone.mcpp({"build"});
        expect(fatal(e2e::succeeded(o7))) << "objnone build failed";
        expect(fatal(e2e::contains(s::text(o7), "produces no target to put its outputs into"))) << "an object with no consumer must be reported";
        auto o7b = objnone.mcpp({"build", "--features", "gated"});
        expect(fatal(e2e::succeeded(o7b))) << "objnone build with the feature failed";
        expect(fatal(s::lacks(s::text(o7b), "produces no target to put its outputs into"))) << "the object was reported as having no consumer while its target existed";

        edge.write("build.mcpp", R"cpp(#include <cstdio>
int main() {
    std::printf("mcpp:protocol=1\n");
    // No `command`, no `outputs` — mcpp fixes the source set during prepare,
    // so an output whose NAME is unknown cannot be built.
    std::printf("mcpp:action={\"id\":\"broken\",\"role\":\"source\"}\n");
}
)cpp");
        edge.remove("target");
        auto b5 = edge.mcpp({"build"});
        expect(fatal(failed(b5))) << "a malformed action was accepted";
        expect(e2e::contains(s::text(b5), "malformed action")) << "no diagnostic for the malformed action";
    };
}};

}  // namespace
