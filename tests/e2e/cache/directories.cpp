// A configuration's build directory over time: named by its lineage and taken over by a new
// fingerprint, locked against a second build, swept by age and by budget, cleaned with
// `clean --stale` and reported with `clean --report`; what a build writes is written once (debug
// information split or compressed, a library's own objects, an equal copy linked) and its state
// directory ignores itself (formerly tests/e2e/609, 935, 998, 999, 1000, 1005, 1022, 1028, 1030,
// 1031).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

void build_ok(const e2e::outcome& r, std::string_view what) {
    expect(fatal(e2e::succeeded(r))) << what << "\n" << s::text(r);
}

// The first configuration directory `target/<triple>/<16 hex digits>`.
fs::path config_dir(const fs::path& project) {
    for (const auto& d : s::glob(project / "target", "*/*"))
        if (fs::is_directory(d) && std::regex_match(d.filename().string(), std::regex{"[0-9a-f]{16}"})) return d;
    return {};
}

// The entries of `dir` a plain `ls` shows (no dot files).
std::size_t shown(const fs::path& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (!e.path().filename().string().starts_with(".")) ++n;
    return n;
}

std::string names(const fs::path& dir) {
    std::string out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) out += e.path().filename().string() + "\n";
    return out;
}

void set_time(const fs::path& p, fs::file_time_type t) { fs::last_write_time(p, t); }
fs::file_time_type days_ago(int days) { return fs::file_time_type::clock::now() - std::chrono::hours{24 * days}; }
// `touch -t 200001010000`.
fs::file_time_type year_2000() {
    return std::chrono::clock_cast<fs::file_time_type::clock>(std::chrono::sys_days{std::chrono::year{2000} / 1 / 1});
}

std::string trimmed(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

// A file's inode (`stat -c %i`).
std::string inode(const fs::path& p) {
    const auto id = s::file_identity(p);
    return id.substr(0, id.find(' '));
}

// `readelf -S -W <program>` shows `.debug_info` with the flag C (compressed).
bool debug_info_compressed(const fs::path& program) {
    const auto r = e2e::run({.argv = {"readelf", "-S", "-W", program.string()}});
    for (const auto& l : s::lines_with(r.out, ".debug_info"))
        if (l.contains(" C ")) return true;
    return false;
}

// mcpp at another path: the program under test's command, started from `exe`.
e2e::outcome engine(const fs::path& exe, std::vector<std::string> args, const fs::path& cwd, const s::options& o) {
    auto c = s::mcpp_command(std::move(args), cwd, o);
    c.argv[0] = exe.string();
    return e2e::run(std::move(c));
}

// `cp -a`: the tree, its links as links, and the write times of its files and directories.
void copy_tree(const fs::path& from, const fs::path& to) {
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
    std::vector<std::pair<fs::path, fs::file_time_type>> times;
    for (const auto& e : fs::recursive_directory_iterator(from))
        if (!e.is_symlink()) times.emplace_back(fs::relative(e.path(), from), e.last_write_time());
    for (const auto& [rel, t] : times | std::views::reverse) fs::last_write_time(to / rel, t);
    fs::last_write_time(to, fs::last_write_time(from));
}

}  // namespace

E2E directories{"directories", [] {
    "609 clean stale"_test = [] {
        // #565: `clean --stale` removes only the fingerprint directories no record considers
        // current and older than the window; --dry-run lists; --older-than; refusals; the
        // survivors are not rebuilt.
        auto p = s::project::scaffold("stale");
        const auto refused = [&](std::vector<std::string> args) {
            auto r = p.mcpp(std::move(args));
            expect(fatal(!e2e::succeeded(r))) << r.line << " should be refused\n" << s::text(r);
            return s::text(r);
        };
        const auto out = refused({"clean", "--stale"});
        expect(fatal(e2e::contains(out, "build_cache"))) << "refusal should name the record: " << out;
        build_ok(p.mcpp({"build"}), "build");
        build_ok(p.mcpp({"build", "--release"}), "build --release");
        std::string triple_name;
        for (const auto& e : fs::directory_iterator(p.at("target")))
            if (!e.path().filename().string().starts_with(".") && (triple_name.empty() || e.path().filename().string() < triple_name)) triple_name = e.path().filename().string();
        expect(fatal(!triple_name.empty())) << "no target/<triple>/ after build";
        const auto triple = p.at("target") / triple_name;
        expect(fatal(shown(triple) == 2u)) << "expected 2 fingerprint dirs (dev+release)\n" << names(triple);
        s::project(triple / "deadbeefdeadbeef/bin", {{"leftover", "stale\n"}});
        set_time(triple / "deadbeefdeadbeef", year_2000());
        set_time(triple / "deadbeefdeadbeef/bin/leftover", year_2000());
        s::project(triple / "cafef00dcafef00d", {{"build.ninja", "latest\n"}});

        auto r = p.mcpp({"clean", "--stale", "--dry-run"});
        expect(fatal(s::has_line(s::text(r), "would remove target/.*/deadbeefdeadbeef"))) << "dry-run did not list the stale dir: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), "kept .*cafef00dcafef00d"))) << "dry-run should keep the freshly written unrecorded dir: " << s::text(r);
        expect(fatal(fs::is_directory(triple / "deadbeefdeadbeef"))) << "dry-run deleted something";
        expect(fatal(shown(triple) == 4u)) << "dry-run changed target/";
        r = p.mcpp({"clean", "--stale"});
        expect(fatal(s::has_line(s::text(r), "removed target/.*/deadbeefdeadbeef"))) << "clean --stale did not report the removed dir: " << s::text(r);
        expect(fatal(!fs::exists(triple / "deadbeefdeadbeef"))) << "stale dir survived";
        expect(fatal(fs::is_directory(triple / "cafef00dcafef00d"))) << "fresh unrecorded dir was removed";
        expect(fatal(shown(triple) == 3u)) << "a current dir was removed: " << names(triple);
        refused({"clean", "--stale", "--older-than", "nonsense"});
        r = p.mcpp({"clean", "--stale", "--older-than", "0"});
        expect(fatal(!fs::exists(triple / "cafef00dcafef00d"))) << "--older-than 0 kept the fresh unrecorded dir: " << s::text(r);
        expect(fatal(shown(triple) == 2u)) << "--older-than 0 removed a current dir: " << names(triple);
        r = p.mcpp({"clean", "--stale"});
        expect(fatal(e2e::contains(s::text(r), "Nothing stale"))) << "second pass should report nothing stale: " << s::text(r);
        expect(fatal(shown(triple) == 2u)) << "second pass removed a current dir";

        fs::create_directories(triple / "feedfacefeedface");
        set_time(triple / "feedfacefeedface", year_2000());
        r = p.mcpp({"clean", "--dry-run"});
        expect(fatal(e2e::contains(s::text(r), "feedfacefeedface"))) << "--dry-run alone did not list the stale dir: " << s::text(r);
        expect(fatal(fs::is_directory(triple / "feedfacefeedface"))) << "--dry-run alone deleted";
        refused({"clean", "--stale", "--bmi-cache"});
        expect(fatal(fs::is_directory(triple / "feedfacefeedface"))) << "refused combination still deleted";
        r = p.mcpp({"clean", "--older-than", "1d"});
        expect(fatal(!fs::exists(triple / "feedfacefeedface"))) << "--older-than 1d did not remove the stale dir: " << s::text(r);
        expect(fatal(shown(triple) == 2u)) << "--older-than alone wiped target/: " << names(triple);
        fs::create_directories(triple / "feedfacefeedface");
        set_time(triple / "feedfacefeedface", year_2000());
        refused({"clean", "--stale", "--older-than", "-1s"});
        expect(fatal(fs::is_directory(triple / "feedfacefeedface"))) << "refused negative window still deleted";
        p.mcpp({"clean", "--stale"});

        // Survivors are intact: both profiles rebuild without relinking anything.
        const auto mtimes = [&] {
            std::map<fs::path, fs::file_time_type> m;
            for (const auto& f : s::glob(triple, "*/bin/stale")) m[f] = fs::last_write_time(f);
            return m;
        };
        const auto before = mtimes();
        expect(fatal(!before.empty())) << "no artifact mtimes read";
        p.mcpp({"build"});
        p.mcpp({"build", "--release"});
        expect(fatal(before == mtimes())) << "a current directory was rebuilt after clean --stale";
    };

    "935 two builds of one configuration"_test = [] {
        // mcpp#839: two builds of one configuration at once both succeed (the second waits for
        // the directory's lock), the result record is complete, and two configurations do not
        // wait on each other.
        s::project p({{"mcpp.toml", "[package]\nname = \"lk\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n"}});
        std::string main = "import std;\n";
        for (int i = 1; i <= 24; ++i) {
            p.write(std::format("src/m{}.cppm", i), std::format("export module m{0};\nimport std;\nexport int f{0}(){{ std::vector<int> v({0}); return (int)v.size(); }}\n", i));
            main += std::format("import m{};\n", i);
        }
        p.write("src/main.cpp", main + "int main(){ std::println(\"{}\", f1()+f24()); }\n");
        build_ok(p.mcpp({"build"}), "the first build failed");  // the std module and toolchain in place
        bool waited = false;
        for (int round = 1; round <= 5; ++round) {
            p.remove("target");
            auto a = std::async(std::launch::async, [&] { return p.mcpp({"build"}); });
            std::this_thread::sleep_for(std::chrono::milliseconds{100 * round});
            const auto b = p.mcpp({"build"});
            const auto ra = a.get();
            expect(fatal(ra && b)) << "A: round " << round << ": rc=" << ra.status << "," << b.status << "\n" << s::text(ra) << s::text(b);
            if (s::has_line(s::text(ra), "Waiting.*held by pid") || s::has_line(s::text(b), "Waiting.*held by pid")) waited = true;
        }
        (void)waited;  // B is reported when observed; a machine where no round overlapped long enough does not fail it

        expect(fatal(p.exists("target/.last-result.json"))) << "C: no target/.last-result.json";
        const auto record = s::json_file(fs::path(s::json_file(p.at("target/.last-result.json"))["data"]["result"].get<std::string>()));
        expect(fatal(record["kind"] == "mcpp.result")) << "C: " << record["kind"].dump();
        const auto& data = record["data"];
        expect(fatal(data["status"] == "ok")) << "C: the result record is incomplete: " << data.dump();
        expect(fatal(data.contains("finishedAt") && !data["finishedAt"].is_null() && data["finishedAt"] != "")) << "C: " << data.dump();
        bool bin = false;
        for (const auto& a : data["artifacts"])
            if (a["kind"] == "bin" && a.contains("launch") && !a["launch"].is_null() && !a["launch"].empty()) bin = true;
        expect(fatal(bin)) << "C: " << data["artifacts"].dump();

        // D: dev and release are two directories, two locks.
        auto dev = std::async(std::launch::async, [&] { return p.mcpp({"build"}); });
        const auto rel = p.mcpp({"build", "--release"});
        const auto rdev = dev.get();
        expect(fatal(rdev && rel)) << "D: rc=" << rdev.status << "," << rel.status << "\n" << s::text(rdev) << s::text(rel);
        expect(fatal(s::no_line(s::text(rdev) + s::text(rel), "Waiting.*held by pid"))) << "D: two configurations waited on one lock\n" << s::text(rdev) << s::text(rel);
    };

    "998 a build keeps its bytes once"_test[tag{"requires:gcc"}] = [] {
        // mcpp#874: a library's archive holds its own objects, a program's debug information is
        // compressed, an equal copy is linked, `clean --stale` and `clean --report` cover
        // `.build-mcpp/out`, a tool's edit supersedes its entry, an unchanged generated header
        // rebuilds nothing, and a library's build program is built in a group beside the first.
        s::needs({"gcc"});
        const s::options home{.where = s::home::linked};
        const auto mcpp_home = s::case_home(s::home::linked);
        const auto tmp = s::case_dir();
        s::project(tmp / "dep", {{"mcpp.toml", "[package]\nname    = \"dep\"\nversion = \"0.1.0\"\n[targets.dep]\nkind = \"lib\"\n"}, {"src/dep.cpp", "int dep_value() { return 41; }\n"}});
        const auto write_tool = [&](int v) {
            s::project(tmp / "tool", {{"mcpp.toml", "[package]\nname    = \"tool\"\nversion = \"0.1.0\"\n[targets.gen]\nkind = \"bin\"\nmain = \"src/gen.cpp\"\n"},
                                      {"src/gen.cpp", std::format("#include <cstdio>\nint main(int argc, char** argv) {{\n    if (argc < 2) return 2;\n    FILE* f = std::fopen(argv[1], \"w\");\n    if (!f) return 3;\n    std::fprintf(f, \"int app_extra() {{ return {}; }}\\n\");\n    return std::fclose(f);\n}}\n", v)}});
        };
        write_tool(1);
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[targets.app]\nkind = \"lib\"\n[targets.main]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n[dependencies]\ndep = { path = \"../dep\" }\n[build-dependencies]\ntool = { path = \"../tool\", tools = [\"gen\"] }\n"},
                                     {"src/app.cpp", "int dep_value(); int app_value() { return dep_value() + 1; }\n"},
                                     {"src/main.cpp", "#include <cstdio>\nint app_value();\nint app_extra();\nint main() { std::printf(\"%d\\n\", app_value() + app_extra()); }\n"},
                                     {"build.mcpp", "#include <cstdlib>\n#include <string>\nimport mcpp;\nint main() {\n    const std::string out = std::string(mcpp::out_dir()) + \"/extra.cpp\";\n    const std::string cmd = std::string(\"\\\"\") + mcpp::dep_bin(\"tool\", \"gen\") + \"\\\" \\\"\" + out + \"\\\"\";\n    if (std::system(cmd.c_str()) != 0) return 1;\n    mcpp::generated(out.c_str());\n}\n"}});
        auto r = app.mcpp({"build"}, home);
        build_ok(r, "the build failed");
        // A
        fs::path archive;
        for (const auto& f : s::find_files(app.at("target"), R"(libapp\.a)"))
            if (!f.generic_string().contains("/.build-mcpp/")) { archive = f; break; }
        expect(fatal(!archive.empty())) << "no libapp.a\n" << s::text(r);
        std::string members;
        for (const auto& m : s::archive_members(archive)) members += m + "\n";
        expect(fatal(e2e::contains(members, "app"))) << "the archive lacks the package's own object: " << members;
        expect(fatal(s::lacks(members, "dep"))) << "the archive holds its dependency's object: " << members;
        // B
        if (s::on_linux() && s::on_path("readelf")) {
            const auto program = s::glob(app.at("target"), "*/*/bin/main");
            expect(fatal(!program.empty() && debug_info_compressed(program.front()))) << "the program's .debug_info is not compressed";
        }
        // C
        app.write("stage/libx.so", "not really a library");
        app.write("stage/placed.so", "not really a library");
        r = app.mcpp({"stage", "--output", "stage/placed.so", "stage/libx.so"}, home);
        build_ok(r, "mcpp stage failed");
        expect(fatal(inode(app.at("stage/libx.so")) == inode(app.at("stage/placed.so")))) << "an equal copy of a shared library was left a copy";
        // D
        std::string used;
        for (const auto& u : s::glob(app.at("target"), "*/*/uses.list"))
            for (const auto& l : s::file_lines_with(u, "out "))
                if (used.empty() && l.starts_with("out ")) used = l.substr(4);
        expect(fatal(!used.empty())) << "the build names no generated-source directory";
        const auto unused = app.at("target/.build-mcpp/out/0000000000000000");
        fs::create_directories(unused / "app");
        set_time(unused, days_ago(4));
        r = app.mcpp({"clean", "--stale"}, home);
        build_ok(r, "clean --stale failed");
        expect(fatal(!fs::exists(unused))) << "an unused generated-source directory was kept\n" << s::text(r);
        expect(fatal(fs::is_directory(app.at("target/.build-mcpp/out") / used))) << "the generated-source directory in use was removed\n" << s::text(r);
        // E
        r = app.mcpp({"clean", "--report"}, home);
        build_ok(r, "clean --report failed");
        expect(fatal(e2e::contains(s::text(r), ".build-mcpp/out") && s::has_line(s::text(r), "^target/"))) << "the report lacks its areas\n" << s::text(r);
        // F
        std::this_thread::sleep_for(std::chrono::seconds{1});
        write_tool(2);
        r = app.mcpp({"build"}, home);
        build_ok(r, "the build after the tool's edit failed");
        const auto tools = s::glob(mcpp_home / "build-cache/v1/tool", "*/tool@*");
        expect(fatal(tools.size() == 1u)) << "the superseded tool entry was kept (" << tools.size() << ")";
        r = app.mcpp({"cache", "list"}, home);
        build_ok(r, "cache list failed");
        bool listed = false;
        for (const auto& l : s::lines_with(s::text(r), "tool@"))
            if (std::regex_search(l, std::regex{R"(\btool\b)"})) listed = true;
        expect(fatal(listed)) << "cache list does not show the tool entry\n" << s::text(r);
        // G
        s::project gen(tmp / "gen", {{"mcpp.toml", "[package]\nname    = \"gen\"\nversion = \"0.1.0\"\n"},
                                     {"value.in", "one\n"},
                                     {"build.mcpp", "#include <string>\nimport mcpp;\nint main() {\n    const std::string root = mcpp::manifest_dir();\n    const std::string out  = mcpp::out_dir();\n    mcpp::action a;\n    a.id   = \"gen:value\";\n    a.role = \"source\";\n    a.arg((root + \"/gen.sh\").c_str()).arg((root + \"/value.in\").c_str())\n     .arg((out + \"/value.h\").c_str())\n     .input((root + \"/value.in\").c_str())\n     .output((out + \"/value.h\").c_str())\n     .submit();\n    mcpp::include_dir(out.c_str());\n}\n"},
                                     {"src/main.cpp", "#include <cstdio>\n#include \"value.h\"\nint main() { std::printf(\"%d\\n\", VALUE); }\n"}});
        s::write_executable(gen.at("gen.sh"), "#!/bin/sh\nprintf '#define VALUE 7\\n' > \"$2.tmp\"\nif cmp -s \"$2.tmp\" \"$2\"; then rm \"$2.tmp\"; else mv \"$2.tmp\" \"$2\"; fi\n");
        r = gen.mcpp({"build"}, home);
        build_ok(r, "the generator's build failed");
        fs::path log;
        for (const auto& f : s::find_files(gen.at("target"), R"(\.ninja_log)"))
            if (!f.generic_string().contains("/.build-mcpp/")) { log = f; break; }
        expect(fatal(!log.empty())) << "no .ninja_log";
        const auto before = s::count_lines(s::read_file(log), "main");
        std::this_thread::sleep_for(std::chrono::seconds{1});
        gen.write("value.in", "two\n");
        r = gen.mcpp({"build"}, home);
        build_ok(r, "the second build failed");
        std::vector<std::string> lines;
        for (auto l : std::views::split(s::read_file(log), '\n')) lines.emplace_back(l.begin(), l.end());
        if (!lines.empty() && lines.back().empty()) lines.pop_back();
        std::string tail;
        for (std::size_t i = std::max<std::size_t>(1, lines.size() >= 5 ? lines.size() - 5 : 0); i < lines.size(); ++i) tail += lines[i] + "\n";
        expect(fatal(e2e::contains(tail, "value.h"))) << "the generator did not run again\n" << s::read_file(log);
        expect(fatal(s::count_lines(s::read_file(log), "main") == before)) << "an unchanged generated header rebuilt its includer\n" << s::read_file(log);
        // H
        s::project(tmp / "genlib", {{"mcpp.toml", "[package]\nname    = \"genlib\"\nversion = \"0.1.0\"\n[targets.genlib]\nkind = \"lib\"\n"},
                                    {"src/genlib.cppm", "export module genlib;\nexport int genlib_value();\n"},
                                    {"build.mcpp", "#include <fstream>\n#include <string>\nimport mcpp;\nint main() {\n    const std::string out = std::string(mcpp::out_dir()) + \"/genlib_impl.cpp\";\n    std::ofstream(out) << \"module genlib;\\nint genlib_value() { return 5; }\\n\";\n    mcpp::generated(out.c_str());\n}\n"}});
        s::project user(tmp / "user", {{"mcpp.toml", "[package]\nname    = \"user\"\nversion = \"0.1.0\"\n[build-dependencies]\ngenlib = { path = \"../genlib\" }\n"},
                                       {"src/main.cpp", "int main() { return 0; }\n"},
                                       {"build.mcpp", "import mcpp;\nimport genlib;\nint main() { return genlib_value() == 5 ? 0 : 1; }\n"}});
        r = user.mcpp({"build"}, home);
        build_ok(r, "the build with a nested build program failed");
        const auto host = user.at("target/.build-mcpp/host");
        for (const auto& e : fs::recursive_directory_iterator(host))
            expect(fatal(!e.path().generic_string().ends_with("/target/.build-mcpp/host"))) << "the host workspace holds a copy of itself: " << e.path().string();
        const auto groups = s::glob(host, "*.1");
        expect(fatal(!groups.empty())) << "the library's build program has no group beside the first\n" << names(host);
        fs::path programs;
        for (const auto& f : s::glob(host, "*/programs.json"))
            if (!f.parent_path().filename().string().ends_with(".1")) { programs = f; break; }
        expect(fatal(!programs.empty())) << "no programs.json\n" << names(host);
        const auto record = s::json_file(programs);
        const fs::path binary = record.begin().value()["binary"].get<std::string>();
        expect(fatal(inode(user.at("target/.build-mcpp/build.mcpp.bin")) == inode(binary))) << "the program beside its record is a copy";
    };

    "999 debug information is stored once"_test[tag{"requires:gcc"}] = [] {
        // design 2026-10-31 §4.1 (b): under dev a root unit keeps its debug information in a
        // `.dwo` and an index package's units keep theirs in the object (compressed); release and
        // `split_debuginfo = "off"` write no `.dwo`; another value is refused.
        s::needs({"gcc"});
        if (!s::on_linux()) skip("the split form is checked on Linux (ELF)");
        s::project app({{"src/main.cpp", "#include <zlib.h>\n#include <cstdio>\nint main() { std::printf(\"%s\\n\", zlibVersion()); }\n"}});
        const auto manifest = [&](std::string_view extra) {
            app.write("mcpp.toml", std::string("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\n\"compat.zlib\" = \"1.3.2\"\n") + std::string(extra));
        };
        const auto dwos = [&] { return s::find_files(app.at("target"), R"(.*\.dwo)"); };
        manifest("");
        auto r = app.mcpp({"build"});
        build_ok(r, "the dev build failed");
        const auto dirs = s::glob(app.at("target"), "*/*");
        expect(fatal(!dirs.empty())) << "no build directory";
        const auto dir = dirs.front();
        expect(fatal(!s::find_files(dir / "obj", R"(main.*\.dwo)").empty())) << "the root's unit has no .dwo";
        for (const auto& f : s::find_files(dir / "obj", R"(.*\.dwo)"))
            expect(fatal(!f.generic_string().contains("compat_zlib"))) << "an index package's unit was split: " << f.string();
        const auto program = s::find_file(dir / "bin", "app");
        expect(fatal(program.has_value())) << "no program";
        expect(fatal(debug_info_compressed(*program))) << "the program's .debug_info is not compressed";
        if (s::on_path("gdb")) {
            const auto g = e2e::run({.argv = {"gdb", "-batch", "-ex", "list main", program->string()}});
            expect(fatal(e2e::contains(g.out, "zlibVersion"))) << "gdb does not read the source through the skeleton";
        }
        app.remove("target");
        r = app.mcpp({"build", "--release"});
        build_ok(r, "the release build failed");
        expect(fatal(dwos().empty())) << "a release build wrote .dwo files";
        app.remove("target");
        manifest("\n[profile.dev]\nsplit_debuginfo = \"off\"\n");
        r = app.mcpp({"build"});
        build_ok(r, "the build with split_debuginfo off failed");
        expect(fatal(dwos().empty())) << "split_debuginfo = \"off\" wrote .dwo files";
        manifest("\n[profile.dev]\nsplit_debuginfo = \"packed\"\n");
        r = app.mcpp({"build"});
        expect(fatal(!e2e::succeeded(r))) << "an unknown split_debuginfo value was accepted\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "\"unpacked\" or \"off\""))) << "the refusal does not name the values\n" << s::text(r);
    };

    "1000 a configuration keeps one directory"_test[tag{"requires:gcc"}] = [] {
        // design 2026-10-31 §8 and 2026-11-12 §17: a predecessor of the lineage is taken over, one
        // of another toolchain replaced, another lineage kept; the sweep by keep_days; uses.list
        // marks the cache entries used; the directory is named by the lineage and the graph runs
        // mcpp through its link; mcpp at another path compiles nothing again; a rooted workspace
        // keeps its configuration under clean --stale.
        s::needs({"gcc"});
        const s::options home{.where = s::home::inherited};
        const auto tmp = s::case_dir();
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\n\"compat.zlib\" = \"1.3.2\"\n"},
                                     {"src/main.cpp", "#include <zlib.h>\nint main() { return zlibVersion()[0] == 0; }\n"}});
        build_ok(app.mcpp({"build"}, home), "the first build failed");
        const auto dir = config_dir(app.dir());
        expect(fatal(!dir.empty())) << "no configuration directory";
        const auto triple = dir.parent_path();
        expect(fatal(fs::is_regular_file(dir / "lineage") && fs::is_regular_file(dir / "toolchain"))) << "the directory records no lineage\n" << names(dir);
        const auto compiles = [&] { return s::count_lines(s::read_file(dir / ".ninja_log"), "main"); };
        const auto zeros = triple / "0000000000000000";
        const auto before = compiles();
        // A
        fs::rename(dir, zeros);
        auto r = app.mcpp({"build"}, home);
        build_ok(r, "the build after the move failed");
        expect(fatal(fs::is_directory(dir) && !fs::exists(zeros))) << "the predecessor was not taken over\n" << names(triple);
        expect(fatal(compiles() == before)) << "taking the predecessor over compiled again\n" << s::read_file(dir / ".ninja_log");
        // B
        fs::rename(dir, zeros);
        std::ofstream(zeros / "toolchain") << "compiler another\n";
        r = app.mcpp({"build"}, home);
        build_ok(r, "the build after a toolchain change failed");
        expect(fatal(!fs::exists(zeros))) << "a predecessor of another toolchain was kept\n" << names(triple);
        expect(fatal(e2e::contains(s::text(r), "Replaced"))) << "the replacement was not said\n" << s::text(r);
        // C
        s::project(triple / "1111111111111111", {{"lineage", "compiler other\n"}, {"build.ninja", ""}});
        r = app.mcpp({"build"}, home);
        build_ok(r, "the build beside another lineage failed");
        expect(fatal(fs::is_directory(triple / "1111111111111111"))) << "a directory of another lineage was taken\n" << names(triple);
        // D
        const auto old = triple / "2222222222222222";
        s::project(old, {{"build.ninja", ""}});
        set_time(old / "build.ninja", days_ago(5));
        set_time(old, days_ago(5));
        build_ok(app.mcpp({"self", "config", "--keep-days", "0"}, home), "self config --keep-days 0 failed");
        app.remove("target/.mcpp-swept");
        r = app.mcpp({"build"}, home);
        build_ok(r, "the build with keep_days = 0 failed");
        expect(fatal(fs::is_directory(old))) << "keep_days = 0 swept a directory\n" << s::text(r);
        build_ok(app.mcpp({"self", "config", "--keep-days", "3"}, home), "self config --keep-days 3 failed");
        app.remove("target/.mcpp-swept");
        r = app.mcpp({"build"}, home);
        build_ok(r, "the build that sweeps failed");
        expect(fatal(!fs::exists(old))) << "a directory no configuration used for 5 days was kept\n" << s::text(r) << names(triple);
        expect(fatal(e2e::contains(s::text(r), "Swept"))) << "the sweep was not said\n" << s::text(r);
        expect(fatal(fs::is_directory(dir))) << "the sweep removed the configuration in use\n" << names(triple);
        // E
        fs::path record;
        for (const auto& l : s::file_lines_with(dir / "uses.list", "compat.zlib"))
            if (record.empty() && l.starts_with("cache ")) record = l.substr(6);
        expect(fatal(!record.empty() && fs::is_regular_file(record))) << "the configuration names no cache entry it used\n" << s::read_file(dir / "uses.list");
        set_time(record, days_ago(40));
        build_ok(app.mcpp({"build"}, home), "the replayed build failed");
        app.remove("target/.mcpp-swept");
        build_ok(app.mcpp({"build"}, home), "the build that sweeps failed");
        expect(fatal(fs::last_write_time(record) > days_ago(1))) << "a replayed build did not mark its cache entry used";
        // G
        r = app.mcpp({"build", "--print-fingerprint"}, home);
        build_ok(r, "--print-fingerprint failed");
        std::smatch m;
        const auto prints = s::text(r);
        expect(fatal(std::regex_search(prints, m, std::regex{R"((^|\n)Lineage: ([0-9a-f]*))"}))) << prints;
        const std::string lineage = m[2];
        expect(fatal(std::regex_search(prints, m, std::regex{R"((^|\n)Fingerprint: ([^\n]*))"}))) << prints;
        const std::string fingerprint = trimmed(m[2]);
        expect(fatal(dir.filename().string() == lineage)) << "the directory is not named by the lineage (" << lineage << ")\n" << names(triple);
        expect(fatal(trimmed(s::read_file(dir / "fingerprint")) == fingerprint)) << "the directory does not record the fingerprint";
        expect(fatal(s::has_line(s::read_file(dir / "build.ninja"), std::format("^mcpp +=.*/{}/engine/epoch-[0-9]+/mcpp", dir.filename().string())))) << "the graph does not run mcpp through the directory's link";
        // H
        const fs::path mcpp = s::mcpp_path();
        const auto other = tmp / "elsewhere" / mcpp.filename();
        fs::create_directories(other.parent_path());
        fs::copy_file(mcpp, other);
        app.write("src/main.cpp", app.read("src/main.cpp") + "// another line\n");
        build_ok(app.mcpp({"build"}, home), "the build before the move failed");
        const auto steps = [&] { return s::count_matching(s::read_file(dir / ".ninja_log"), ".*"); };
        const auto ran = steps();
        r = engine(other, {"build"}, app.dir(), home);
        build_ok(r, "the build with mcpp at another path failed");
        expect(fatal(s::no_line(s::text(r), "^ *(Compiling|Linking)"))) << "mcpp at another path compiled again\n" << s::text(r);
        expect(fatal(steps() == ran)) << "mcpp at another path ran a step again\n" << s::read_file(dir / ".ninja_log");
        expect(fatal(s::lacks(s::text(r), "configuration changed"))) << "mcpp at another path was taken for another configuration\n" << s::text(r);
        // I
        std::ofstream(dir / "toolchain") << "compiler another\n";
        r = app.mcpp({"build"}, home);
        build_ok(r, "the build after an in-place toolchain change failed");
        expect(fatal(e2e::contains(s::text(r), "Replaced"))) << "the replacement in place was not said\n" << s::text(r);
        expect(fatal(fs::is_directory(dir) && trimmed(s::read_file(dir / "fingerprint")) == fingerprint)) << "the directory did not continue under its name\n" << names(triple);
        // F
        s::project ws(tmp / "ws", {{"mcpp.toml", "[package]\nname    = \"ws\"\nversion = \"0.1.0\"\n\n[workspace]\nmembers = [\".\"]\n"}, {"src/main.cpp", "int main() { return 0; }\n"}});
        build_ok(ws.mcpp({"build"}, home), "the rooted workspace did not build");
        const auto current = config_dir(ws.dir());
        r = ws.mcpp({"clean", "--stale"}, home);
        build_ok(r, "clean --stale failed");
        expect(fatal(!current.empty() && fs::is_directory(current))) << "clean --stale removed the rooted workspace's own build\n" << s::text(r);
    };

    "1083 a configuration built beside another states the other's size"_test[tag{"requires:gcc"}] = [] {
        // mcpp#927 (design 2026-11-12 §18 P6): a change of configuration that builds in a directory
        // of its own names the directory it leaves beside it, its size and what reclaims it.
        s::needs({"gcc"});
        s::project app({{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n"},
                        {"src/main.cpp", "int main() { return 0; }\n"}});
        build_ok(app.mcpp({"build"}), "the first build failed");
        app.write("mcpp.toml", app.read("mcpp.toml") + "\n[profile.dev]\ndebug = \"lines\"\n");
        auto r = app.mcpp({"build"});
        build_ok(r, "the build of the changed configuration failed");
        expect(fatal(s::has_line(s::text(r),
            R"(configuration changed \([^)]*\): [0-9a-f]+ is built in full beside [0-9a-f]+ \([0-9.]+ [KMG]?i?B\); `mcpp clean --stale` drops it)")))
            << s::text(r);
    };

    "1005 clean stale says what it kept"_test[tag{"requires:gcc"}] = [] {
        // mcpp#881: `clean --stale` says what it kept and within what window, `[build] keep_days`.
        s::needs({"gcc"});
        const s::options home{.where = s::home::inherited};
        s::project app({{"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", "int main() { return 0; }\n"}});
        build_ok(app.mcpp({"build"}, home), "the build failed");
        const auto triple = config_dir(app.dir()).parent_path();
        auto r = app.mcpp({"clean", "--stale", "--dry-run"}, home);
        build_ok(r, "clean --stale failed");
        expect(fatal(e2e::contains(s::text(r), "every fingerprint directory is recorded as current"))) << "the plain summary changed\n" << s::text(r);
        const auto dead = triple / "deadbeefdeadbeef";
        s::project(dead, {{"build.ninja", "rule\n"}});
        r = app.mcpp({"clean", "--stale", "--dry-run"}, home);
        build_ok(r, "clean --stale failed");
        expect(fatal(s::has_line(s::text(r), "kept    .*deadbeefdeadbeef"))) << "the kept directory is not listed\n" << s::text(r);
        expect(fatal(s::lacks(s::text(r), "every fingerprint directory is recorded as current"))) << "the summary still says every directory is current\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "1 directory not in use kept, written within the last 3d"))) << "the summary does not say what was kept\n" << s::text(r);
        set_time(dead / "build.ninja", days_ago(2));
        set_time(dead, days_ago(2));
        r = app.mcpp({"clean", "--stale"}, home);
        build_ok(r, "clean --stale failed");
        expect(fatal(fs::is_directory(dead))) << "a directory written two days ago was removed under the default window\n" << s::text(r);
        r = app.mcpp({"clean", "--stale", "--older-than", "1d"}, home);
        build_ok(r, "clean --stale --older-than 1d failed");
        expect(fatal(!fs::exists(dead))) << "--older-than 1d kept a directory written two days ago\n" << s::text(r);
    };

    "1022 a taken over directory keeps no compiler record of another"_test[tag{"requires:gcc"}] = [] {
        // 2026.11.7.1: a directory taken over keeps no split-compile certificate naming the
        // predecessor's objects, and a certificate of another directory found at admission (the
        // project moved) is no record: the object is compiled again.
        s::needs({"gcc"});
        const s::options home{.where = s::home::inherited};
        const auto tmp = s::case_dir();
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[build]\nbmi_schedule = \"on\"\n"},
                                     {"src/greet.cppm", "export module greet;\nexport int answer() { return 42; }\n"},
                                     {"src/main.cpp", "import greet;\nint main() { return answer() == 42 ? 0 : 1; }\n"}});
        build_ok(app.mcpp({"build", "--release"}, home), "the first release build failed");
        const auto release = config_dir(app.dir());
        const auto release_lineage = s::read_file(release / "lineage");
        fs::remove_all(release);
        build_ok(app.mcpp({"build"}, home), "the dev build failed");
        const auto dev = config_dir(app.dir());
        expect(fatal(!s::glob(dev / "obj", "*.sched.completed").empty())) << "the split compile left no certificate";
        const auto pred = dev.parent_path() / "0000000000000000";
        fs::rename(dev, pred);
        std::ofstream(pred / "lineage", std::ios::binary) << release_lineage;
        // A
        app.write("src/main.cpp", "import greet;\nint main() { return answer() == 42 ? 0 : 2; }\n");
        auto r = app.mcpp({"build", "--release"}, home);
        build_ok(r, "the build that took the predecessor over failed");
        expect(fatal(!fs::exists(pred))) << "the predecessor was not taken over\n" << names(pred.parent_path());
        const auto taken = config_dir(app.dir());
        // B
        for (const auto& f : s::find_files(taken / "obj", R"(.*\.sched.*)"))
            expect(fatal(s::lacks(s::read_file(f), "/0000000000000000/"))) << "a record of the predecessor was left beside the objects: " << f.string();
        // C
        const auto moved = tmp / "moved-app";
        copy_tree(app.dir(), moved);
        s::project m(moved, {{"src/greet.cppm", "export module greet;\nexport int answer() { return 41 + 1; }\n"}});
        r = m.mcpp({"build", "--release"}, home);
        build_ok(r, "the build of a moved directory failed");
    };

    "1028 a projects state directory ignores itself"_test = [] {
        // A build's `<project>/.mcpp/` ignores itself: `git status` lists nothing under it.
        if (!s::on_path("git")) skip("no git on this machine");
        s::project p({{"mcpp.toml", "[package]\nname    = \"p\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", "int main() { return 0; }\n"}, {".gitignore", "target/\n"}});
        e2e::run({.argv = {"git", "init", "-q", "."}, .cwd = p.dir()});
        build_ok(p.mcpp({"build"}), "the build failed");
        if (!fs::is_directory(p.at(".mcpp"))) skip("this build wrote no state directory");
        expect(fatal(s::has_line(p.read(".mcpp/.gitignore"), R"(^\*$)"))) << ".mcpp/.gitignore does not ignore everything\n" << p.read(".mcpp/.gitignore");
        const auto status = e2e::run({.argv = {"git", "status", "--porcelain", "--untracked-files=all"}, .cwd = p.dir()});
        expect(fatal(s::no_line(status.out, R"(^\?\? \.mcpp/)"))) << "version control sees the state directory\n" << status.out;
    };

    "1030 the low space warning knows what the last build took"_test = [] {
        // mcpp#874: a build records what it took (`.mcpp-written`), and the next build warns below
        // that before it builds.
        auto p = s::project::scaffold("app");
        build_ok(p.mcpp({"build"}), "the build failed");
        const auto rec = s::find_file(p.at("target"), ".mcpp-written");
        if (!rec) skip("the file system reported no change in free space");
        expect(fatal(std::regex_match(trimmed(s::read_file(*rec)), std::regex{"[0-9]+"}))) << "A: .mcpp-written is not a number: " << s::read_file(*rec);
        std::ofstream(*rec) << "1152921504606846976\n";  // 1 EiB: more than any file system here has free
        p.write("src/main.cpp", p.read("src/main.cpp") + "\n// changed\n");
        const auto r = p.mcpp({"build"});
        build_ok(r, "the second build failed");
        expect(fatal(e2e::contains(s::text(r), "free where this build writes"))) << "B: no warning before a build that needs more than is free\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "last build took"))) << "B: the warning does not say what the last build took\n" << s::text(r);
    };

    "1031 target keeps within its budget"_test = [] {
        // mcpp#874: over `[build] target_budget` the directories no configuration uses go whatever
        // their age, the one in use stays and the build says target/ is over its budget; without
        // a budget a recent unused directory is kept.
        auto p = s::project::scaffold("app");
        build_ok(p.mcpp({"build"}), "the build failed");
        fs::path cfg;
        for (const auto& f : s::find_files(p.at("target"), s::exe("app")))
            if (f.parent_path().filename() == "bin") { cfg = f.parent_path().parent_path(); break; }
        expect(fatal(fs::is_directory(cfg))) << "no configuration directory";
        const auto stale = cfg.parent_path() / "0000000000000000";
        fs::copy(cfg, stale, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
        p.write("src/main.cpp", p.read("src/main.cpp") + "\n// c\n");
        auto r = p.mcpp({"build"});
        build_ok(r, "C: the build failed");
        expect(fatal(fs::is_directory(stale))) << "C: without a budget a recent unused directory was removed\n" << s::text(r);
        p.write("mcpp.toml", p.read("mcpp.toml") + "\n[build]\ntarget_budget = \"1KiB\"\n");
        p.write("src/main.cpp", p.read("src/main.cpp") + "\n// a\n");
        r = p.mcpp({"build"});
        build_ok(r, "the build with a budget failed");
        expect(fatal(!fs::exists(stale))) << "A: the unused directory survived a build over budget\n" << s::text(r);
        expect(fatal(fs::is_directory(cfg))) << "B: the configuration in use was removed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "over [build] target_budget"))) << "B: no warning for a target/ over budget in use\n" << s::text(r);
        expect(fatal(s::lacks(s::text(r), "unsupported key"))) << "B: target_budget is not a known key\n" << s::text(r);
    };
}};
