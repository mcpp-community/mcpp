// Where actions write and what they read: placements staged by one process, an output's one
// writer, the out_dir of each configuration, the build's contract (formerly tests/e2e/827, 896,
// 897, 982).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

std::optional<fs::path> ninja_file(const fs::path& target) {
    for (const auto& f : s::find_files(target, R"(build\.ninja)"))
        if (f.generic_string().find("/.build-mcpp/") == std::string::npos) return f;
    return std::nullopt;
}

// The first file under `root` whose generic path ends with `suffix` (`find -path '*<suffix>'`).
std::optional<fs::path> ending(const fs::path& root, std::string_view name, std::string_view suffix) {
    for (const auto& f : s::find_files(root, name))
        if (f.generic_string().ends_with(suffix)) return f;
    return std::nullopt;
}

E2E outputs{"outputs", [] {
    "827 many placements run as one process"_test[s::unix_shell] = [] {
        // The files a program's runtime needs beside it are placed by one `stage_list` edge, not
        // one per file; the edge does not run when no placement changed; a changed source
        // arrives and an unchanged destination is not rewritten; one entry keeps its own edge.
        s::project p{s::files{{"src/main.cpp", "int main() { return 0; }\n"},
                      {"mcpp.toml", "[package]\nname = \"place827\"\nversion = \"0.1.0\"\n"}}};
        for (int i = 1; i <= 50; ++i) p.write(std::format("data/f{}.txt", i), std::format("{}\n", i));
        auto program = [&](int n) {
            p.write("build.mcpp", std::format(R"cpp(import std;
import mcpp.core;
int main() {{
    for (int i = 1; i <= {}; ++i) {{
        const std::string f = "data/f" + std::to_string(i) + ".txt";
        mcpp::deploy(f.c_str(), "data");
    }}
    return 0;
}}
)cpp", n));
        };
        auto touch_main = [&] { fs::last_write_time(p.at("src/main.cpp"), fs::file_time_type::clock::now()); };
        program(50);
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "S1: the build failed";
        auto nj = ninja_file(p.at("target"));
        expect(fatal(nj.has_value())) << "S1: no build.ninja";
        expect(eq(s::count_lines(s::read_file(*nj), ": stage_list "), std::size_t{1})) << "S1: not exactly one stage_list edge";
        expect(s::no_line(s::read_file(*nj), ": stage_file .*data")) << "S1: a per-file placement edge remains";
        const auto out = nj->parent_path();
        std::size_t placed = 0;
        for (const auto& f : s::find_files(out, R"(f[0-9]+\.txt)"))
            placed += f.generic_string().find("data/f") != std::string::npos ? 1 : 0;
        const auto in_bin = fs::is_directory(out / "bin/data") ? std::ranges::distance(fs::directory_iterator(out / "bin/data"), fs::directory_iterator{}) : 0;
        expect(in_bin == 50 || placed >= 50) << "S1: not every file was placed";
        const auto dest1 = ending(out, R"(f1\.txt)", "/data/f1.txt");
        const auto dest2 = ending(out, R"(f2\.txt)", "/data/f2.txt");
        expect(fatal(dest1 && dest2)) << "S1: the placed files are not found under " << out.string();
        auto ran = [&] { return s::count_matching(s::read_file(out / ".ninja_log"), "placements|data/f"); };
        const auto before = ran();
        touch_main();
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "S2: the build failed";
        expect(eq(ran(), before)) << "S2: a build with no placement change ran the placement edge";
        const auto ref = fs::last_write_time(*dest2);
        std::this_thread::sleep_for(std::chrono::milliseconds{1100});
        p.write("data/f1.txt", "changed\n");
        touch_main();
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "S3: the build failed";
        expect(s::has_line(s::read_file(*dest1), "^changed$")) << "S3: the changed source did not arrive";
        expect(fs::last_write_time(*dest2) <= ref) << "S3: an unchanged destination was rewritten";
        program(1);
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "S4: the build failed";
        nj = ninja_file(p.at("target"));
        expect(fatal(nj.has_value())) << "S4: no build.ninja";
        expect(s::lacks(s::read_file(*nj), ": stage_list ")) << "S4: one entry produced a list edge";
        expect(s::has_line(s::read_file(*nj), R"(: stage_file .*f1.txt)")) << "S4: the single entry lost its per-file edge";
    };

    "896 an action output has one writer"_test[s::gcc][s::unix_shell] = [] {
        // An action's output is written by the action alone: prepare writes no placeholder, a
        // removed output is generated again, and an existing (empty) output is not rewritten.
        const s::options inherited{.where = s::home::inherited};
        s::project ws{s::files{{"mcpp.toml", "[workspace]\nmembers = [\"app\"]\n"},
                       {"app/mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.0.1\"\n[build]\nsources = []\n[targets.app]\nkind = \"bin\"\nmain = \"main.cpp\"\n"},
                       {"app/main.cpp", "extern const char* resource();\nint main() { return resource()[0] == 'R' ? 0 : 1; }\n"},
                       {"app/gen.sh", "#!/bin/sh\nprintf 'const char* resource() { return \"RESOURCE\"; }\\n' > \"$1\"\nprintf \"\" > \"$2\"\n"},
                       {"app/build.mcpp", R"cpp(import std;
import mcpp;
int main() {
    const auto root = std::filesystem::path(mcpp::manifest_dir());
    const auto out = std::filesystem::path(mcpp::out_dir());
    const auto gen = (root / "gen.sh").generic_string();
    const auto cpp = (out / "generated.cpp").generic_string();
    const auto empty = (out / "empty.cpp").generic_string();
    mcpp::action a;
    a.id = "generate:resource";
    a.role = mcpp::roles::source;
    a.arg("sh").arg(gen.c_str()).arg(cpp.c_str()).arg(empty.c_str())
     .input(gen.c_str()).output(cpp.c_str()).output(empty.c_str()).submit();
    return 0;
}
)cpp"}}};
        expect(fatal(e2e::succeeded(ws.mcpp({"build", "-p", "app", "--configure-only"}, inherited)))) << "configure-only";
        expect(fatal(!s::find_file(ws.dir(), "generated.cpp").has_value())) << "prepare wrote the action's output";
        expect(fatal(e2e::succeeded(ws.mcpp({"build", "-p", "app"}, inherited)))) << "first build";
        const auto gen = ending(ws.dir(), R"(generated\.cpp)", "/generated.cpp");
        expect(fatal(gen && gen->generic_string().find("/out/") != std::string::npos && fs::file_size(*gen) > 0)) << "the action did not write its output";
        fs::remove(*gen);
        expect(fatal(e2e::succeeded(ws.mcpp({"build", "-p", "app"}, inherited)))) << "the build after the generated file was removed failed (#778)";
        expect(fatal(fs::exists(*gen) && fs::file_size(*gen) > 0)) << "the generated file was not generated again";
        std::optional<fs::path> empty;
        for (const auto& f : s::find_files(ws.dir(), R"(empty\.cpp)"))
            if (!empty && f.generic_string().find("/out/") != std::string::npos) empty = f;
        expect(fatal(empty && fs::file_size(*empty) == 0)) << "the action's empty output is missing or not empty";
        const auto before = fs::last_write_time(*empty);
        std::this_thread::sleep_for(std::chrono::seconds{1});
        expect(fatal(e2e::succeeded(ws.mcpp({"build", "-p", "app"}, inherited)))) << "the third build";
        expect(fs::last_write_time(*empty) == before) << "an existing output was rewritten by prepare";
    };

    "897 each configuration has its own generated output"_test[s::gcc][s::unix_shell] = [] {
        // `mcpp::out_dir()` is per configuration and resolution.json records it: after dev,
        // release, dev, each configuration's binary prints what its own out_dir was generated
        // for, and dev and release do not share one.
        const s::options inherited{.where = s::home::inherited};
        s::project p{s::files{{"mcpp.toml", "[package]\nname = \"gp\"\nversion = \"0.1.0\"\n[build]\nsources = [\"src/main.cpp\"]\n[targets.gp]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                      {"src/main.cpp", "#include <cstdio>\nconst char* gen_profile();\nint main() { std::printf(\"GEN=%s\\n\", gen_profile()); }\n"},
                      {"gen.sh", "#!/bin/sh\nprintf 'const char* gen_profile() { return \"%s\"; }\\n' \"$1\" > \"$2\"\n"},
                      {"build.mcpp", R"cpp(import std;
import mcpp;
int main() {
    const auto root = std::filesystem::path(mcpp::manifest_dir());
    const auto out = (std::filesystem::path(mcpp::out_dir()) / "gen.cpp").generic_string();
    const auto gen = (root / "gen.sh").generic_string();
    mcpp::action a;
    a.id = "gen";
    a.role = mcpp::roles::source;
    a.arg("sh").arg(gen.c_str()).arg(mcpp::profile()).arg(out.c_str())
     .input(gen.c_str()).output(out.c_str()).submit();
    return 0;
}
)cpp"}}};
        expect(fatal(e2e::succeeded(p.mcpp({"build"}, inherited)))) << "dev";
        expect(fatal(e2e::succeeded(p.mcpp({"build", "--release"}, inherited)))) << "release";
        expect(fatal(e2e::succeeded(p.mcpp({"build"}, inherited)))) << "dev";
        std::map<std::string, std::string> seen;  // profile -> out_dir
        for (const auto& r : s::find_files(p.at("target"), R"(resolution\.json)")) {
            if (r.generic_string().find("/.build-mcpp/") != std::string::npos) continue;
            std::string out;
            for (const auto& pkg : s::json_file(r)["graph"]["packages"])
                if (pkg.value("root", false)) { out = pkg.value("outDir", ""); break; }
            expect(fatal(!out.empty())) << "resolution.json records no outDir: " << r.string();
            const auto gen = fs::path(out) / "gen.cpp";
            expect(fatal(fs::exists(gen) && fs::file_size(gen) > 0)) << "outDir " << out << " does not hold the generated file";
            std::smatch m;
            const auto text = s::read_file(gen);
            const std::string profile = std::regex_search(text, m, std::regex{R"re("([a-z]*)")re"}) ? m[1].str() : "";
            auto got = s::run(r.parent_path() / "bin/gp").out;
            std::erase(got, '\r');
            expect(eq(s::last_line(got), "GEN=" + profile)) << "the binary in " << r.parent_path().string() << " and its outDir disagree";
            seen[profile] = out;
        }
        expect(fatal(seen.contains("dev") && seen.contains("release"))) << "expected one dev and one release configuration";
        expect(seen["dev"] != seen["release"]) << "dev and release share one out_dir: " << seen["dev"];
    };

    "982 an action reads the builds contract"_test[s::elf] = [] {
        // `${mcpp.contract}` names the configuration's contract beside its compile database:
        // each language's compiler and the words every unit gets (no dialect or module flags),
        // the link driver, its words and the archiver, without a package's own link inputs —
        // which still reach the consumer.
        s::project p{s::files{{"mcpp.toml", "[package]\nname    = \"contract\"\nversion = \"0.1.0\"\n"},
                      {"src/main.cpp", "int main() { return 0; }\n"},
                      {"build.mcpp", R"cpp(#include <string>
import mcpp;
int main() {
    mcpp::link_flag("-Wl,--defsym,mcpp_contract_consumer=1");
    const std::string out = std::string(mcpp::out_dir()) + "/seen-contract.json";
    mcpp::action a;
    a.id   = "copy-contract";
    a.role = mcpp::roles::check;
    a.arg("cp").arg("${mcpp.contract}").arg(out.c_str())
     .output(out.c_str())
     .submit();
}
)cpp"}}};
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "build failed";
        const auto seen = s::find_file(p.at("target"), "seen-contract.json");
        expect(fatal(seen.has_value())) << "the action did not run";
        const auto d = s::json_file(*seen);
        expect(eq(d["schema"].get<int>(), 1)) << d.dump();
        expect(!d["target"].get<std::string>().empty()) << d.dump();
        for (std::string_view lang : {"c", "cxx", "asm"}) {
            const auto& t = d["compile"][std::string(lang)];
            expect(fs::path(t["program"].get<std::string>()).is_absolute() && t["args"].is_array()) << lang << ": " << t.dump();
        }
        bool optimised = false, dialect = false;
        for (const auto& a : d["compile"]["cxx"]["args"]) {
            const auto w = a.get<std::string>();
            optimised = optimised || w.starts_with("-O");
            dialect = dialect || w.starts_with("-std=") || w == "-fmodules" || w.starts_with("-fmodule-");
        }
        expect(optimised) << d["compile"]["cxx"]["args"].dump();
        expect(!dialect) << d["compile"]["cxx"]["args"].dump();
        expect(fs::path(d["link"]["program"].get<std::string>()).is_absolute() && d["link"]["args"].is_array()) << d["link"].dump();
        expect(d["link"].contains("archiver")) << d["link"].dump();
        bool leaked = false;
        for (const auto& a : d["link"]["args"]) leaked = leaked || a.get<std::string>().find("mcpp_contract_consumer") != std::string::npos;
        expect(!leaked) << d["link"].dump();
        std::optional<fs::path> db;
        for (const auto& f : s::find_files(p.at("target"), R"(compile_commands\.json)"))
            if (!db && f.generic_string().find("/.build-mcpp/") == std::string::npos) db = f;
        expect(fatal(db.has_value())) << "no compile database";
        expect(fs::exists(db->parent_path() / "mcpp-contract.json")) << "no contract beside the compile database";
        expect(e2e::contains(s::read_file(db->parent_path() / "build.ninja"), "mcpp_contract_consumer=1")) << "excluding package link inputs from the contract removed them from the consumer";
    };
}};

}  // namespace
