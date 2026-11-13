// Generated files: a package's and a manifest's [generated_files], a build program's generated
// sources and headers (formerly tests/e2e/51, 107, 124, 142, 314).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

// A file's modification time in whole seconds (`stat -c %Y`).
long long mtime(const fs::path& p) {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::file_clock::to_sys(fs::last_write_time(p)).time_since_epoch()).count();
}

// The build.ninja of the build, not of a build program's.
std::optional<fs::path> ninja_file(const fs::path& target) {
    for (const auto& f : s::find_files(target, R"(build\.ninja)"))
        if (f.generic_string().find("/.build-mcpp/") == std::string::npos) return f;
    return std::nullopt;
}

E2E generated{"generated", [] {
    "51 package generated files"_test[tag{"requires:gcc"}][tag{"requires:fresh-sandbox"}] = [] {
        // A package descriptor's generated_files are materialized in the package before it is
        // scanned and compiled.
        s::needs({"gcc", "fresh-sandbox"});
        const auto tmp = temp_dir();
        s::project{tmp / "local-index", {{"pkgs/t/tinycfg.lua", R"cpp(package = {
    spec = "1",
    name = "tinycfg",
    description = "Generated config header package",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/tinycfg-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/*.c" },
        include_dirs = { "mcpp_generated/include" },
        generated_files = {
            ["mcpp_generated/include/generated_config.h"] = "#pragma once\n#define TINYCFG_VALUE 42\n",
        },
        targets = { ["tinycfg"] = { kind = "lib" } },
        deps = {},
    },
}
)cpp"}}};
        s::project app{tmp / "project/app", {{".mcpp/.xlings/data/xpkgs/local-dev.tinycfg/1.0.0/src/tinycfg.c", "#include \"generated_config.h\"\nint tinycfg_value(void) {\n    return TINYCFG_VALUE;\n}\n"},
                                             {"src/main.cpp", "extern \"C\" int tinycfg_value(void);\nint main() {\n    return tinycfg_value() == 42 ? 0 : 1;\n}\n"}}};
        app.write("mcpp.toml", std::format("[package]\nname = \"app\"\nversion = \"0.1.0\"\n[indices]\nlocal-dev = {{ path = \"{}\" }}\n[dependencies]\n\"local-dev.tinycfg\" = \"1.0.0\"\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", (tmp / "local-index").generic_string()));
        const s::options inherited{.where = s::home::inherited};
        expect(fatal(e2e::succeeded(app.mcpp({"build"}, inherited)))) << "build failed";
        const auto made = ".mcpp/.xlings/data/xpkgs/local-dev.tinycfg/1.0.0/mcpp_generated/include/generated_config.h";
        expect(fatal(app.exists(made))) << "generated file was not materialized";
        expect(e2e::contains(app.read(made), "TINYCFG_VALUE 42")) << "generated file content mismatch";
        expect(e2e::succeeded(app.mcpp({"run"}, inherited))) << "run failed";
    };

    "107 generated files toml"_test[tag{"requires:gcc"}] = [] {
        // [generated_files] entries materialize in the project root before the scan, enter the
        // fingerprint (a content change rebuilds), and a path escaping the root is an error.
        s::needs({"gcc"});
        auto p = s::project::scaffold("genf");
        p.write("mcpp.toml", "[package]\nname    = \"genf\"\nversion = \"0.1.0\"\n[generated_files]\n\"src/gen/answer.cpp\" = \"\"\"\nextern \"C\" int gen_answer() { return 41; }\n\"\"\"\n");
        p.write("src/main.cpp", "import std;\nextern \"C\" int gen_answer();\nint main() {\n    std::println(\"gen = {}\", gen_answer());\n    return 0;\n}\n");
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "build failed";
        expect(fatal(p.exists("src/gen/answer.cpp"))) << "generated file not materialized";
        expect(eq(s::last_line(p.mcpp({"run"}).out), std::string{"gen = 41"})) << "unexpected output";
        auto manifest = p.read("mcpp.toml");
        manifest.replace(manifest.find("return 41"), 9, "return 42");
        p.write("mcpp.toml", manifest);
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "rebuild failed";
        expect(eq(s::last_line(p.mcpp({"run"}).out), std::string{"gen = 42"})) << "stale after content change";
        p.write("mcpp.toml", "[package]\nname    = \"genf\"\nversion = \"0.1.0\"\n[generated_files]\n\"../evil.cpp\" = \"int x;\"\n");
        auto escape = p.mcpp({"build"});
        expect(fatal(failed(escape))) << "escaping path was accepted";
        expect(s::contains_nocase(s::text(escape), "generated_files")) << "wrong error";
    };

    "124 gen source out of root object path"_test[tag{"requires:elf"}][tag{"requires:gcc"}] = [] {
        // A dependency's build program emits a source into OUT_DIR, outside the dependency's
        // root; its object path stays under obj/ (no `..`, not absolute), beside the consumer's
        // own gen.cpp.
        s::needs({"elf", "gcc"});
        const auto tmp = temp_dir();
        s::project{tmp / "gdep", {{"build.mcpp", R"cpp(#include <cstdio>
#include <string>
import mcpp;
int main() {
    std::string p = std::string(mcpp::out_dir()) + "/gen.cpp";
    std::FILE* f = std::fopen(p.c_str(), "w");
    std::fputs("int gen_val() { return 7; }\n", f);
    std::fclose(f);
    mcpp::generated("gen.cpp");
    return 0;
}
)cpp"},
                                  {"src/lib.cpp", "extern int gen_val();\nint gdep_use() { return gen_val(); }\n"},
                                  {"mcpp.toml", "[package]\nname    = \"gdep\"\nversion = \"0.1.0\"\n[modules]\nsources = [\"src/**/*.cpp\"]\n[targets.gdep]\nkind = \"lib\"\n"}}};
        s::project consumer{tmp / "consumer", {{"src/gen.cpp", "int consumer_gen() { return 100; }\n"},
                                               {"src/main.cpp", "import std;\nextern int gdep_use();\nextern int consumer_gen();\nint main() {\n    std::println(\"sum={}\", gdep_use() + consumer_gen());\n    return (gdep_use() == 7 && consumer_gen() == 100) ? 0 : 1;\n}\n"},
                                               {"mcpp.toml", "[package]\nname    = \"consumer\"\nversion = \"0.1.0\"\n[modules]\nsources = [\"src/**/*.cpp\"]\n[dependencies]\ngdep = { path = \"../gdep\" }\n[targets.consumer]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}}};
        expect(fatal(e2e::succeeded(consumer.mcpp({"build"})))) << "build failed (expected: out-of-root generated source must map under obj/)";
        const auto nj = ninja_file(consumer.at("target"));
        expect(fatal(nj.has_value())) << "no build.ninja generated";
        const auto ninja = s::read_file(*nj);
        std::string bad;
        const std::regex object{R"(build ([^ ]+\.o) : cxx_object)"};
        const std::regex escapes{R"((^|/)\.\.(/|$)|^/)"};
        for (std::sregex_iterator it(ninja.begin(), ninja.end(), object), end; it != end; ++it)
            if (const auto o = (*it)[1].str(); std::regex_search(o, escapes)) bad += o + "\n";
        expect(bad.empty()) << "object path escapes obj/:\n" << bad;
        expect(eq(s::last_line(consumer.mcpp({"run"}).out), std::string{"sum=107"})) << "unexpected output";
    };

    "142 generated files incremental"_test[tag{"requires:gcc"}] = [] {
        // An unchanged [generated_files] entry is not rewritten when an unrelated source change
        // re-runs the materialization, so its includers are not recompiled; a changed one is
        // rewritten and its includers recompiled.
        s::needs({"gcc"});
        auto p = s::project::scaffold("genincr");
        p.write("mcpp.toml", "[package]\nname    = \"genincr\"\nversion = \"0.1.0\"\n[generated_files]\n\"src/gen/config.h\" = \"\"\"\n#define GEN_ANSWER 41\n\"\"\"\n");
        p.write("src/main.cpp", "#include \"gen/config.h\"\n#include <cstdio>\nextern int other();\nint main() {\n    std::printf(\"gen = %d\\n\", GEN_ANSWER + other());\n    return 0;\n}\n");
        p.write("src/other.cpp", "int other() { return 0; }\n");
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "build failed";
        expect(fatal(p.exists("src/gen/config.h"))) << "generated file not materialized";
        const auto mains = s::find_files(p.at("target"), R"(main.*\.o)");
        const auto others = s::find_files(p.at("target"), R"(other.*\.o)");
        expect(fatal(!mains.empty() && !others.empty())) << "object files not found under target/";
        const auto gen_before = mtime(p.at("src/gen/config.h"));
        const auto main_before = mtime(mains.front());
        std::this_thread::sleep_for(std::chrono::seconds{1});  // a rewrite observable at 1 s granularity
        fs::last_write_time(p.at("src/other.cpp"), fs::file_time_type::clock::now());
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "incremental rebuild failed";
        expect(fatal(mtime(others.front()) > gen_before)) << "other.o was not recompiled — test premise broken";
        const auto gen_after = mtime(p.at("src/gen/config.h"));
        expect(eq(gen_after, gen_before)) << "generated file rewritten on incremental rebuild";
        expect(eq(mtime(mains.front()), main_before)) << "main.o recompiled on incremental rebuild";
        auto manifest = p.read("mcpp.toml");
        manifest.replace(manifest.find("GEN_ANSWER 41"), 13, "GEN_ANSWER 42");
        p.write("mcpp.toml", manifest);
        std::this_thread::sleep_for(std::chrono::seconds{1});
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "rebuild after content change failed";
        const auto gen_changed = mtime(p.at("src/gen/config.h"));
        expect(gen_after != gen_changed) << "generated file not rewritten after content change";
        expect(e2e::contains(p.read("src/gen/config.h"), "GEN_ANSWER 42")) << "generated file has stale content";
        long long main_changed = 0;
        for (const auto& f : s::find_files(p.at("target"), R"(main.*\.o)")) main_changed = std::max(main_changed, mtime(f));
        expect(main_changed >= gen_changed) << "main.o not recompiled after content change";
        expect(eq(s::last_line(p.mcpp({"run"}).out), std::string{"gen = 42"})) << "stale binary after content change";
    };

    "314 dependency action generated header"_test[tag{"requires:gcc"}] = [] {
        // A package that generates its own header compiles after the generator (mcpp#534): the
        // header is the generator's output, and every compile edge of the package waits for
        // the package's action phony, of which there is one.
        s::needs({"gcc"});
        const auto tmp = temp_dir();
        s::project proto{tmp / "proto", {{"mcpp.toml", "[package]\nname    = \"proto\"\nversion = \"0.1.0\"\n[targets.proto]\nkind = \"lib\"\n"},
                                         {"src/proto.cppm", "module;\n#include \"proto_generated.h\"\nexport module proto;\nexport int proto_answer() { return PROTO_ANSWER; }\n"},
                                         {"build.mcpp", R"cpp(#include <string>
import mcpp;
int main() {
    const std::string root = mcpp::manifest_dir();
    const std::string out  = mcpp::out_dir();
    mcpp::action a;
    a.id   = "proto:header";
    a.role = "source";
    a.arg((root + "/gen.sh").c_str())
     .arg((out + "/proto_generated.h").c_str())
     .output((out + "/proto_generated.h").c_str())   // HEADER, and the only output
     .submit();
    mcpp::include_dir(out.c_str());
}
)cpp"}}};
        s::write_executable(proto.at("gen.sh"), "#!/usr/bin/env bash\nsleep 2\nprintf '#define PROTO_ANSWER 42\\n' > \"$1\"\n");
        s::project app{tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[dependencies]\nproto = { path = \"../proto\" }\n"},
                                     {"src/main.cpp", "#include <cstdio>\nimport proto;\nint main() { std::printf(\"ANSWER=%d\\n\", proto_answer()); }\n"}}};
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "a dependency whose action generates its only header did not build";
        expect(eq(s::last_line(app.mcpp({"run"}).out), std::string{"ANSWER=42"})) << "expected ANSWER=42 from the generated header";
        const auto hdr = s::find_file(tmp, "proto_generated.h");
        expect(fatal(hdr.has_value())) << "no proto_generated.h anywhere";
        expect(fatal(fs::file_size(*hdr) > 0)) << hdr->string() << " is empty — the placeholder, not the generator's output";
        expect(e2e::contains(s::read_file(*hdr), "PROTO_ANSWER")) << hdr->string() << " is not what gen.sh writes";
        const auto nj = ninja_file(app.at("target"));
        expect(fatal(nj.has_value())) << "no build.ninja";
        const auto ninja = s::read_file(*nj);
        std::smatch m;
        const bool any = std::regex_search(ninja, m, std::regex{R"((^|\n)build (mcpp-actions-[^ ]*proto[^ ]*) : phony)"});
        expect(fatal(any)) << "no action phony was emitted for the dependency";
        const auto phony = m[2].str();
        const auto quoted = std::regex_replace(phony, std::regex{R"([.^$|()\[\]{}*+?\\])"}, R"(\$&)");
        expect(eq(s::count_matching(ninja, std::format("^build {} : phony", quoted)), std::size_t{1})) << "expected exactly one '" << phony << "' edge";
        const std::string rules = "cxx_object|c_object|cxx_module|cxx_module_obj|cxx_module_bmi|cxx_scan|asm_object";
        const std::string src = R"(proto/src/proto\.cppm)";
        const auto edges = s::count_matching(ninja, std::format("^build .*: *({}) .*{}", rules, src));
        const auto ordered = s::count_matching(ninja, std::format(R"(^build .*: *({}) .*{}.*\|\| *{})", rules, src, quoted));
        expect(fatal(edges > 0)) << "no compile edge for the dependency was found at all — the assertion below would have passed by describing nothing";
        expect(eq(ordered, edges)) << ordered << " of " << edges << " dependency compile edges wait for the action";
    };
}};

}  // namespace
