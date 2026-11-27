// A package as a shared library: a dependency's library linked and loaded rather than flattened,
// through another shared or a static package, under its ABI name; the consumer's link axis and a
// package's default link form; a static package under a shared library; what a library exports;
// the search paths a link carries; a library placed beside each program of a workspace; and the
// closure each shared library and shipped program links with (formerly tests/e2e/55, 56, 57, 64,
// 306, 621, 670, 685, 692, 702, 835, 838, 884); a dlopen requirement is judged from the object
// that loads it (993).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

// `find <root> -path '*<suffix>' -type f | head -1`, the paths sorted.
std::optional<fs::path> ending(const fs::path& root, std::string_view suffix) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return std::nullopt;
    std::vector<fs::path> found;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && it->path().generic_string().ends_with(suffix)) found.push_back(it->path());
    if (found.empty()) return std::nullopt;
    std::ranges::sort(found);
    return found.front();
}

// `find <root> -type f -name <name> -path '*<part>*' | head -1`.
std::optional<fs::path> named_within(const fs::path& root, std::string_view name, std::string_view part) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return std::nullopt;
    std::vector<fs::path> found;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && it->path().filename() == name && it->path().generic_string().contains(part))
            found.push_back(it->path());
    if (found.empty()) return std::nullopt;
    std::ranges::sort(found);
    return found.front();
}

// `ls -d <root>/*/*/`: the directories two levels below `root`.
std::vector<fs::path> build_dirs(const fs::path& root) {
    std::vector<fs::path> out;
    for (auto& d : s::glob(root, "*/*"))
        if (fs::is_directory(d)) out.push_back(d);
    return out;
}

// The entries of a directory whose name starts with `prefix` (`find <dir> -maxdepth 1 -name '<prefix>*'`).
std::optional<fs::path> entry_starting(const fs::path& dir, std::string_view prefix) {
    std::error_code ec;
    std::vector<fs::path> found;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().filename().string().starts_with(prefix)) found.push_back(e.path());
    if (found.empty()) return std::nullopt;
    std::ranges::sort(found);
    return found.front();
}

bool needs_library(const fs::path& file, std::string_view part) {
    for (const auto& n : s::read_header(file).needed)
        if (n.contains(part)) return true;
    return false;
}

std::vector<std::string> split(std::string_view text, char sep) {
    std::vector<std::string> out;
    for (auto part : std::views::split(text, sep)) out.emplace_back(part.begin(), part.end());
    return out;
}

// A program's output as `$(prog 2>&1)` holds it: both streams, the trailing newlines removed.
std::string captured(const e2e::outcome& r) {
    auto t = s::text(r);
    while (t.ends_with('\n') || t.ends_with('\r')) t.pop_back();
    return t;
}

E2E packages{"packages", s::home_fixture, [] {
    "55 dependency shared artifact"_test[s::elf] = [] {
        // A dependency's shared library is built as its own artifact and linked by the consumer,
        // not flattened into it as objects.
        const auto tmp = s::case_dir();
        s::project dep(tmp / "depShared", {{"src/dep.c", "int dep_shared_answer(void) {\n    return 42;\n}\n"},
                                           {"mcpp.toml", "[package]\nname    = \"depShared\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.c\"]\n\n[targets.depShared]\nkind = \"shared\"\n"}});
        s::project app(tmp / "app", {{"src/main.cpp", "extern \"C\" int dep_shared_answer(void);\n\nint main() {\n    return dep_shared_answer() == 42 ? 0 : 1;\n}\n"},
                                     {"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.cpp\"]\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n[dependencies.depShared]\npath = \"../depShared\"\n"}});
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed: " << s::text(r);
        expect(fatal(s::find_file(app.at("target"), "libdepShared.so").has_value())) << "dependency shared library artifact was not produced: " << s::text(r);
        const auto bin = ending(app.at("target"), "/bin/app");
        expect(fatal(bin.has_value())) << "app binary was not produced: " << s::text(r);
        expect(fatal(std::ranges::contains(s::read_header(*bin).needed, "libdepShared.so"))) << "app binary does not link against libdepShared.so";
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "run failed: " << s::text(r);
    };

    "56 transitive shared artifact"_test[s::elf] = [] {
        // A shared dependency over another shared package links that package itself, so the
        // intermediate library records the NEEDED edge.
        const auto tmp = s::case_dir();
        s::project base(tmp / "depBase", {{"src/base.c", "int dep_base_answer(void) {\n    return 41;\n}\n"},
                                          {"mcpp.toml", "[package]\nname    = \"depBase\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.c\"]\n\n[targets.depBase]\nkind = \"shared\"\n"}});
        s::project mid(tmp / "depMid", {{"src/mid.c", "extern int dep_base_answer(void);\n\nint dep_mid_answer(void) {\n    return dep_base_answer() + 1;\n}\n"},
                                        {"mcpp.toml", "[package]\nname    = \"depMid\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.c\"]\n\n[targets.depMid]\nkind = \"shared\"\n\n[dependencies.depBase]\npath = \"../depBase\"\n"}});
        s::project app(tmp / "app", {{"src/main.cpp", "extern \"C\" int dep_mid_answer(void);\n\nint main() {\n    return dep_mid_answer() == 42 ? 0 : 1;\n}\n"},
                                     {"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.cpp\"]\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n[dependencies.depMid]\npath = \"../depMid\"\n"}});
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed: " << s::text(r);
        const auto base_so = s::find_file(app.at("target"), "libdepBase.so");
        const auto mid_so = s::find_file(app.at("target"), "libdepMid.so");
        const auto bin = ending(app.at("target"), "/bin/app");
        expect(fatal(base_so && mid_so && bin)) << "expected shared artifacts were not produced: " << s::text(r);
        expect(fatal(std::ranges::contains(s::read_header(*mid_so).needed, "libdepBase.so"))) << "libdepMid.so does not link against libdepBase.so";
        expect(fatal(std::ranges::contains(s::read_header(*bin).needed, "libdepMid.so"))) << "app binary does not link against libdepMid.so";
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "run failed: " << s::text(r);
    };

    "57 static dep shared artifact"_test[s::elf] = [] {
        // A static package that references a shared one: its objects are linked into the
        // consumer, so the consumer's link names the shared library too.
        const auto tmp = s::case_dir();
        s::project shared(tmp / "depShared", {{"src/dep.c", "int dep_shared_answer(void) {\n    return 41;\n}\n"},
                                              {"mcpp.toml", "[package]\nname    = \"depShared\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.c\"]\n\n[targets.depShared]\nkind = \"shared\"\n"}});
        s::project stat(tmp / "depStatic", {{"src/static.c", "extern int dep_shared_answer(void);\n\nint dep_static_answer(void) {\n    return dep_shared_answer() + 1;\n}\n"},
                                            {"mcpp.toml", "[package]\nname    = \"depStatic\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.c\"]\n\n[targets.depStatic]\nkind = \"lib\"\n\n[dependencies.depShared]\npath = \"../depShared\"\n"}});
        s::project app(tmp / "app", {{"src/main.cpp", "extern \"C\" int dep_static_answer(void);\n\nint main() {\n    return dep_static_answer() == 42 ? 0 : 1;\n}\n"},
                                     {"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.cpp\"]\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n[dependencies.depStatic]\npath = \"../depStatic\"\n"}});
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed: " << s::text(r);
        const auto so = s::find_file(app.at("target"), "libdepShared.so");
        const auto bin = ending(app.at("target"), "/bin/app");
        expect(fatal(so && bin)) << "expected shared artifact or app binary was not produced: " << s::text(r);
        expect(fatal(std::ranges::contains(s::read_header(*bin).needed, "libdepShared.so"))) << "app binary does not link against static dependency's shared dependency";
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "run failed: " << s::text(r);
    };

    "64 shared soname runtime alias"_test[s::elf] = [] {
        // A library's SONAME is given an alias that `run` and `test` find, a test's own link
        // making it as well as the default target.
        const auto tmp = s::case_dir();
        s::project dep(tmp / "depShared", {{"src/dep.c", "int dep_shared_answer(void) {\n    return 42;\n}\n"},
                                           {"mcpp.toml", "[package]\nname    = \"depShared\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.c\"]\n\n[targets.depShared]\nkind = \"shared\"\nsoname = \"libdepShared.so.1\"\n"}});
        s::project app(tmp / "app", {{"src/main.cpp", R"(#include <dlfcn.h>

using answer_fn = int (*)();

int main() {
    void* handle = dlopen("libdepShared.so.1", RTLD_NOW);
    if (!handle) {
        return 10;
    }
    auto answer = reinterpret_cast<answer_fn>(dlsym(handle, "dep_shared_answer"));
    if (!answer) {
        dlclose(handle);
        return 11;
    }
    int result = answer();
    dlclose(handle);
    return result == 42 ? 0 : 12;
}
)"},
                                     {"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.cpp\"]\nldflags = [\"-ldl\"]\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n[dependencies.depShared]\npath = \"../depShared\"\n"}});
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed: " << s::text(r);
        const auto so = s::find_file(app.at("target"), "libdepShared.so");
        const auto alias = s::find_file(app.at("target"), "libdepShared.so.1");
        expect(fatal(so && alias)) << "expected shared library and ABI soname alias were not produced: " << s::text(r);
        expect(fatal(s::read_header(*so).soname == "libdepShared.so.1")) << "shared library missing requested SONAME";
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "run failed: " << s::text(r);
        // `mcpp test` names its goals explicitly; from a clean target only the test path can
        // make the alias.
        app.write("tests/linked_test.cpp", "extern \"C\" int dep_shared_answer();\n\nint main() {\n    return dep_shared_answer() == 42 ? 0 : 1;\n}\n");
        app.remove("target");
        r = app.mcpp({"test"});
        expect(fatal(e2e::succeeded(r))) << "mcpp test failed — the SONAME alias is a prerequisite of anything that links the library, not a by-product of the default target: " << s::text(r);
        expect(fatal(s::find_file(app.at("target"), "libdepShared.so.1").has_value())) << "mcpp test did not produce the ABI soname alias: " << s::text(r);
    };

    "306 dependency linkage axis"_test[s::elf] = [] {
        // `[build] dependency_linkage`: the default changes nothing, `shared` gives a library the
        // program loads, the two configurations take different output directories, and the
        // packed bundle starts.
        const auto tmp = s::case_dir();
        s::project core(tmp / "core", {{"mcpp.toml", "[package]\nname    = \"core\"\nversion = \"0.1.0\"\n[build]\nc_standard = \"c11\"\nsources    = [\"src/*.c\"]\n[targets.core]\nkind = \"lib\"\n"},
                                       {"src/core.c", "int core_answer(void) { return 42; }\n"}});
        const std::string manifest = "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[build]\nc_standard = \"c11\"\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.c\"\n[dependencies]\ncore = { path = \"../core\" }\n";
        s::project app(tmp / "app", {{"mcpp.toml", manifest},
                                     {"src/main.c", "#include <stdio.h>\nint core_answer(void);\nint main(void) { printf(\"%d\\n\", core_answer()); return 0; }\n"}});
        // 1
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        auto dirs = build_dirs(app.at("target"));
        expect(fatal(!dirs.empty())) << "FAIL: could not locate the fingerprint directory";
        const auto static_fp = dirs.front().filename();
        const auto triples = s::glob(app.at("target"), "*");
        expect(fatal(triples.empty() || static_fp != triples.front().filename())) << "FAIL: could not locate the fingerprint directory";
        expect(fatal(dirs.size() == 1u)) << "FAIL: expected exactly one build dir, got " << dirs.size();
        const auto bin = s::find_file(app.at("target"), "app");
        expect(fatal(bin.has_value())) << "no executable: " << s::text(r);
        auto run = s::run(*bin);
        expect(fatal(s::has_line(run.out, "^42$"))) << "static build does not run";
        expect(fatal(s::find_files(app.at("target"), R"(libcore\.so)").empty())) << "FAIL: the default produced a shared library";
        // 2
        auto shared = manifest;
        shared.replace(shared.find("c_standard = \"c11\"\n"), 19, "c_standard = \"c11\"\ndependency_linkage = \"shared\"\n");
        app.write("mcpp.toml", shared);
        expect(fatal(e2e::contains(app.read("mcpp.toml"), "dependency_linkage = \"shared\""))) << "sed did not apply";
        r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto so = s::find_file(app.at("target"), "libcore.so");
        expect(fatal(so.has_value())) << "FAIL: no shared library was built: " << s::text(r);
        const auto so_header = s::read_header(*so);
        // `file`'s "ELF ... shared object" is an ELF ET_DYN.
        expect(fatal(so_header.kind == s::format::elf && so_header.elf_type == 3)) << "FAIL: not an ELF .so";
        std::optional<fs::path> sbin;
        for (const auto& f : s::find_files(app.at("target"), "app"))
            if (fs::last_write_time(f) > fs::last_write_time(*so)) { sbin = f; break; }
        if (!sbin) sbin = so->parent_path() / "app";
        expect(fatal(fs::exists(*sbin) && (fs::status(*sbin).permissions() & fs::perms::owner_exec) != fs::perms::none)) << "FAIL: no executable beside the .so";
        bool imported = false;
        for (const auto& sym : s::elf_symbol_table(*sbin, ".dynsym"))
            if (sym.name == "core_answer" && !sym.defined) imported = true;
        expect(fatal(imported)) << "FAIL: core_answer is not an imported symbol in the executable";
        expect(fatal(needs_library(*sbin, "libcore.so"))) << "FAIL: the executable does not declare libcore.so as NEEDED";
        run = s::run(*sbin);
        expect(fatal(s::has_line(run.out, "^42$"))) << "FAIL: the shared build does not run";
        // 3
        dirs = build_dirs(app.at("target"));
        expect(fatal(dirs.size() == 2u)) << "FAIL: expected two build directories (static + shared), got " << dirs.size();
        fs::path shared_fp;
        for (const auto& d : dirs)
            if (d.filename() != static_fp) shared_fp = d.filename();
        expect(fatal(!shared_fp.empty())) << "FAIL: both configurations landed in one output directory (" << static_fp.string() << ")";
        // 4
        r = app.mcpp({"pack"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: pack: " << s::text(r);
        const auto archives = s::find_files(app.at("target/dist"), R"(.*\.tar\.gz)");
        expect(fatal(!archives.empty())) << "FAIL: no archive: " << s::text(r);
        const auto extract = tmp / "extract";
        fs::create_directories(extract);
        s::tar_extract(archives.front(), extract);
        expect(fatal(s::find_file(extract, "libcore.so").has_value())) << "FAIL: the shared dependency was not bundled";
        std::optional<fs::path> packed;
        for (const auto& f : s::find_files(extract, "app"))
            if ((fs::status(f).permissions() & fs::perms::owner_exec) != fs::perms::none) { packed = f; break; }
        expect(fatal(packed.has_value())) << "FAIL: no executable in the bundle";
        run = s::run(fs::absolute(*packed));
        expect(fatal(e2e::succeeded(run))) << "FAIL: the extracted bundle does not start: " << s::text(run);
        expect(fatal(captured(run) == "42")) << "FAIL: bundle printed '" << captured(run) << "'";
    };

    "621 exports narrows the symbol surface"_test[s::elf][s::gcc] = [] {
        // A shared library publishes what `exports` names (a file or an inline list), and
        // everything otherwise.
        s::project p({{"src/lib.cpp", "extern \"C\" int mcpp_e2e_621_public(int x)   { return x + 1; }\nextern \"C\" int mcpp_e2e_621_internal(int x) { return x + 2; }\n"},
                      {"abi/lib.exports", "# One symbol pattern per line; `#` starts a comment.\nmcpp_e2e_621_public\n"}});
        auto emit = [&](std::string_view extra) {
            p.write("mcpp.toml", std::format("[package]\nname    = \"expsurface\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.cpp\"]\n\n"
                                             "[targets.expsurface]\nkind   = \"shared\"\nsoname = \"libexpsurface.so.1\"\n{}\n", extra));
        };
        // `nm -D --defined-only`, the names starting mcpp_e2e_621, sorted.
        auto dynsyms = [&] {
            const auto libs = s::find_files(p.at("target"), R"(libexpsurface.*\.so.*)");
            expect(fatal(!libs.empty())) << "FAIL: no shared library was produced";
            std::vector<std::string> names;
            for (const auto& sym : s::elf_symbol_table(libs.front(), ".dynsym"))
                if (sym.defined && sym.name.starts_with("mcpp_e2e_621")) names.push_back(sym.name);
            std::ranges::sort(names);
            return names;
        };
        auto has = [](const std::vector<std::string>& v, std::string_view n) { return std::ranges::contains(v, n); };
        emit("");
        p.remove("target");
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "FAIL: the default build failed";
        const auto before = dynsyms();
        expect(fatal(has(before, "mcpp_e2e_621_public"))) << "FAIL: the default build did not publish the public symbol";
        expect(fatal(has(before, "mcpp_e2e_621_internal"))) << "FAIL: the default is supposed to publish everything and did not.";
        emit("exports = \"abi/lib.exports\"");
        p.remove("target");
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "FAIL: the build with exports failed";
        const auto after = dynsyms();
        expect(fatal(has(after, "mcpp_e2e_621_public"))) << "FAIL: the declared symbol is not published";
        expect(fatal(!has(after, "mcpp_e2e_621_internal"))) << "FAIL: an undeclared symbol is still published";
        expect(fatal(before != after)) << "FAIL: the two legs read identically";
        emit("exports = [\"mcpp_e2e_621_public\"]");
        p.remove("target");
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "FAIL: the inline form failed to build";
        expect(fatal(dynsyms() == after)) << "FAIL: the inline list and the file disagree";
    };

    "670 an rpath loader token is left as written"_test[s::elf][s::gcc] = [] {
        // A search path beginning with a loader token (`@executable_path`, `@loader_path`)
        // reaches the binary as written, from the root and from a dependency; an ordinary
        // relative one is anchored to its package.
        const auto tmp = s::case_dir();
        s::project dep(tmp / "dep", {{"mcpp.toml", "[package]\nnamespace = \"demo\"\nname      = \"dep\"\nversion   = \"0.1.0\"\n[targets.dep]\nkind = \"lib\"\n[build]\nldflags = [\"-Wl,-rpath,@loader_path/../lib\"]\n"},
                                     {"src/dep.cppm", "export module dep;\nexport int dep_anchor() { return 3; }\n"}});
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[toolchain]\nlinux = \"gcc@16.1.0\"\n[dependencies]\ndemo.dep = { path = \"../dep\" }\n[build]\nldflags = [\"-Wl,-rpath,@executable_path/../Frameworks\", \"-Wl,-rpath,vendor/lib\"]\n"},
                                     {"src/main.cpp", "import dep;\nint main() { return dep_anchor() == 3 ? 0 : 1; }\n"}});
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "mcpp build failed: " << s::text(r);
        const auto bin = ending(app.at("target"), "/bin/app");
        expect(fatal(bin.has_value())) << "no bin/app: " << s::text(r);
        const auto rpath = s::read_header(*bin).runpath;
        expect(fatal(!rpath.empty())) << "the program carries no RPATH";
        const auto entries = split(rpath, ':');
        expect(fatal(std::ranges::contains(entries, "@executable_path/../Frameworks"))) << "the root's @executable_path rpath is not literal: " << rpath;
        expect(fatal(std::ranges::contains(entries, "@loader_path/../lib"))) << "the dependency's @loader_path rpath is not literal: " << rpath;
        for (const auto& e : entries)
            expect(fatal(!e.contains("/@executable_path") && !e.contains("/@loader_path"))) << "a loader token was anchored to a package directory: " << rpath;
        const bool anchored = std::ranges::contains(entries, (fs::canonical(app.dir()) / "vendor/lib").generic_string()) ||
                              std::ranges::contains(entries, (app.dir() / "vendor/lib").generic_string());
        expect(fatal(anchored)) << "an ordinary relative rpath is no longer anchored to the package: " << rpath;
    };

    "685 a nested test loads a graph shared library"_test[s::elf] = [] {
        // A test from `tests/sub/` searches `$ORIGIN/..` for a graph-built library; the program
        // in `bin/` carries no relative entry.
        const auto tmp = s::case_dir();
        const std::string main_cpp = "import fw;\nint main() { return fw_anchor() == 41 ? 0 : 1; }\n";
        s::project fw(tmp / "fw", {{"mcpp.toml", "[package]\nnamespace = \"demo\"\nname      = \"fw\"\nversion   = \"0.1.0\"\n[targets.fw]\nkind = \"shared\"\n"},
                                   {"src/fw.cppm", "export module fw;\nexport int fw_anchor() { return 41; }\n"}});
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[dependencies]\ndemo.fw = { path = \"../fw\" }\n"},
                                     {"src/main.cpp", main_cpp}, {"tests/sub/deep.cpp", main_cpp}, {"tests/top.cpp", main_cpp}});
        // 1
        auto r = app.mcpp({"test"});
        expect(fatal(e2e::succeeded(r))) << "1: mcpp test failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "sub/deep ... ok"))) << "1: the nested test did not pass: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "top ... ok"))) << "1: the top-level test did not pass: " << s::text(r);
        // 2
        const auto deep = named_within(app.at("target"), "deep", "/bin/sub/");
        expect(fatal(deep.has_value())) << "2: no bin/sub/deep was built";
        expect(fatal(e2e::contains(s::read_header(*deep).runpath, "$ORIGIN/.."))) << "2: the nested test does not search $ORIGIN/..";
        // 3
        r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "3: mcpp build failed: " << s::text(r);
        const auto prog = named_within(app.at("target"), "app", "/bin/");
        expect(fatal(prog.has_value())) << "3: no bin/app was built";
        expect(fatal(s::lacks(s::read_header(*prog).runpath, "$ORIGIN/"))) << "3: the program beside the library carries a relative search entry";
    };

    "692 a package states its default link form"_test[s::elf] = [] {
        // `linkage` beside `kind` states a library's default link form: a silent consumer gets
        // it, a consumer's statement outranks it, a row may state it, and the contradictions
        // are refused.
        const auto tmp = s::case_dir();
        auto make_fw = [&](std::string_view dir, std::string_view tail) {
            s::project fw(tmp / dir, {});
            fw.write("mcpp.toml", std::format("[package]\nnamespace = \"demo\"\nname      = \"fw\"\nversion   = \"0.1.0\"\n{}\n", tail));
            fw.write("src/fw.cppm", "export module fw;\nexport int fw_anchor() { return 41; }\n");
        };
        auto make_app = [&](std::string_view dir, std::string_view fwdir, std::string_view edge = "", std::string_view tail = "") {
            s::project a(tmp / dir, {});
            a.write("mcpp.toml", std::format("[package]\nname = \"{}\"\nversion = \"0.1.0\"\n[dependencies]\ndemo.fw = {{ path = \"../{}\"{} }}\n{}\n", dir, fwdir, edge, tail));
            a.write("src/main.cpp", "import fw;\nint main() { return fw_anchor() == 41 ? 0 : 1; }\n");
        };
        // `rm -rf target && mcpp build <flags> > build.log`
        auto build = [&](std::string_view dir, std::vector<std::string> flags = {}) {
            s::project a(tmp / dir, {});
            a.remove("target");
            flags.insert(flags.begin(), "build");
            auto r = a.mcpp(flags);
            a.write("build.log", s::text(r));
            return r;
        };
        auto log = [&](std::string_view dir) { return s::read_file(tmp / dir / "build.log"); };
        // `demo.fw`'s library is named by its package (mcpp#975): `libdemo_fw.so`.
        auto libfw = [&](std::string_view dir) { return !s::find_files(tmp / dir / "target", R"(libdemo_fw\.so)").empty(); };
        auto record_of = [&](std::string_view dir) {
            std::string out;
            for (const auto& res : s::find_files(tmp / dir / "target", R"(resolution\.json)")) {
                if (res.generic_string().contains("/.build-mcpp/")) continue;
                const auto doc = s::json_file(res);
                for (const auto& p : doc["graph"]["packages"])
                    if (p["package"]["canonical"] == "demo.fw@0.1.0") {
                        const auto link = p.value("link", nlohmann::json::object());
                        out = link.value("form", std::string()) + " " + link.value("reason", std::string());
                    }
                break;
            }
            return out;
        };
        make_fw("fwdefault", "[targets.fw]\nkind    = \"lib\"\nlinkage = \"shared\"");
        // A
        make_app("a", "fwdefault");
        expect(fatal(e2e::succeeded(build("a", {"--strict"})))) << "A: build --strict failed: " << log("a");
        expect(fatal(libfw("a"))) << "A: no libdemo_fw.so for a silent consumer: " << log("a");
        const auto bin_a = named_within(tmp / "a/target", "a", "/bin/");
        expect(fatal(bin_a.has_value())) << "A: no program";
        expect(fatal(needs_library(*bin_a, "libdemo_fw.so"))) << "A: the program does not NEED libdemo_fw.so";
        auto r = record_of("a");
        expect(fatal(r == "shared package-default")) << "A: link record is '" << r << "'";
        expect(fatal(e2e::succeeded(s::run(*bin_a)))) << "A: the program did not run";
        // B
        make_app("b", "fwdefault", ", linkage = \"static\"");
        expect(fatal(e2e::succeeded(build("b", {"--strict"})))) << "B: build --strict failed: " << log("b");
        expect(fatal(!libfw("b"))) << "B: an explicit static request still built libfw.so: " << log("b");
        expect(fatal(e2e::contains(log("b"), "the consumer's `linkage = \"static\"` on this dependency overrides the package's default, [targets.fw] linkage = \"shared\"")))
            << "B: the information line does not name both statements: " << log("b");
        expect(fatal(s::no_line(log("b"), "degraded|warning: .*demo.fw"))) << "B: the override was reported as a degradation";
        r = record_of("b");
        expect(fatal(r == "static requested")) << "B: link record is '" << r << "'";
        // C
        make_app("c", "fwdefault", "", "[build]\ndependency_linkage = \"static\"");
        expect(fatal(e2e::succeeded(build("c")))) << "C: build failed: " << log("c");
        expect(fatal(!libfw("c"))) << "C: dependency_linkage = static did not outrank the default: " << log("c");
        r = record_of("c");
        expect(fatal(r == "static requested")) << "C: link record is '" << r << "'";
        // D
        make_fw("fwrowlinux", "[targets.fw]\nkind = \"lib\"\n[target.'cfg(os = \"linux\")'.targets.fw]\nlinkage = \"shared\"");
        make_fw("fwrowwin", "[targets.fw]\nkind = \"lib\"\n[target.'cfg(os = \"windows\")'.targets.fw]\nlinkage = \"shared\"");
        make_app("d1", "fwrowlinux");
        expect(fatal(e2e::succeeded(build("d1")))) << "D: build failed (matching row): " << log("d1");
        expect(fatal(libfw("d1"))) << "D: a matching row did not give the shared default: " << log("d1");
        r = record_of("d1");
        expect(fatal(r == "shared package-default")) << "D: matching row record is '" << r << "'";
        make_app("d2", "fwrowwin");
        expect(fatal(e2e::succeeded(build("d2")))) << "D: build failed (non-matching row): " << log("d2");
        expect(fatal(!libfw("d2"))) << "D: a non-matching row built libfw.so: " << log("d2");
        r = record_of("d2");
        expect(fatal(r == "static default")) << "D: non-matching row record is '" << r << "'";
        // E
        make_fw("fwreplace", "[targets.fw]\nkind = \"shared\"\n[target.'cfg(os = \"linux\")'.targets.fw]\nlinkage = \"static\"");
        make_app("e", "fwreplace");
        expect(fatal(e2e::succeeded(build("e", {"--strict"})))) << "E: build --strict failed: " << log("e");
        expect(fatal(!libfw("e"))) << "E: the row default did not replace kind = shared: " << log("e");
        r = record_of("e");
        expect(fatal(r == "static package-default")) << "E: link record is '" << r << "'";
        // F
        auto refused = [&](std::string leg, std::string_view tail, std::string_view text) {
            make_fw("fw" + leg, tail);
            make_app(leg, "fw" + leg);
            expect(fatal(!build(leg))) << "F(" << leg << "): the manifest was accepted: " << log(leg);
            expect(fatal(e2e::contains(log(leg), text))) << "F(" << leg << "): refusal wording: " << log(leg);
        };
        refused("f1", "[targets.fw]\nkind    = \"shared\"\nlinkage = \"static\"", "states both `kind = \"shared\"` and `linkage = \"static\"`");
        refused("f2", "[targets.fw]\nkind = \"lib\"\n[target.'cfg(os = \"linux\")'.targets.fw]\nkind    = \"shared\"\nlinkage = \"shared\"", "states both `kind` and `linkage`");
        refused("f3", "[targets.fw]\nkind    = \"lib\"\nlinkage = \"dynamic\"", "the default form of a library is `static` or `shared`");
        s::project prog(tmp / "prog", {{"mcpp.toml", "[package]\nname    = \"prog\"\nversion = \"0.1.0\"\n[targets.prog]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n[target.'cfg(os = \"linux\")'.targets.prog]\nlinkage = \"shared\"\n"},
                                       {"src/main.cpp", "int main() { return 0; }\n"}});
        expect(fatal(!build("prog"))) << "F: a row gave a program target a default link form: " << log("prog");
        expect(fatal(e2e::contains(log("prog"), "is a program target, which has no link form"))) << "F: program refusal wording: " << log("prog");
    };

    "702 a static package under a shared library is linked into it"_test[s::unix_shell][s::python3] = [] {
        // A static package reachable only through a shared library is linked into it (a host
        // that never linked it loads the library); reached by the program too, ELF builds and
        // reports it and Mach-O refuses it; `linkage = "shared"` on it is the remedy.
        if (s::on_windows()) skip("the PE legs of 702 are not wired on this shard");
        const auto python = s::on_path("python3");
        expect(fatal(python.has_value())) << "no python to load the library from a foreign host";
        const bool macho = s::on_macos();
        const std::string so = macho ? "dylib" : "so";
        const auto tmp = s::case_dir();
        s::project x(tmp / "x", {{"mcpp.toml", "[package]\nname    = \"x\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.c\"]\n\n[targets.x]\nkind = \"lib\"\n"},
                                 {"src/x.c", "int x_answer(void) { return 41; }\n"}});
        s::project fw(tmp / "fw", {{"mcpp.toml", "[package]\nname    = \"fw\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.c\"]\n\n[targets.fw]\nkind = \"shared\"\n\n[dependencies]\nx = { path = \"../x\" }\n"},
                                   {"src/fw.c", "extern int x_answer(void);\n__attribute__((visibility(\"default\"))) int fw_answer(void) { return x_answer() + 1; }\n"}});
        s::project app(tmp / "app", {});
        auto write_app = [&](std::string_view deps, std::string_view body) {
            app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nfw = {{ path = \"../fw\" }}\n{}\n", deps));
            app.write("src/main.cpp", std::string(body) + "\n");
        };
        auto first_dir = [&] {
            const auto dirs = build_dirs(app.at("target"));
            expect(fatal(!dirs.empty())) << "no build directory";
            return dirs.front();
        };
        // A
        write_app("", "extern \"C\" int fw_answer(void);\nint main() { return fw_answer() == 42 ? 0 : 1; }");
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "a shared library over a static package did not build: " << s::text(r);
        auto dir = first_dir();
        expect(fatal(e2e::succeeded(s::run(dir / "bin/app")))) << "the program over libfw exited non-zero";
        const auto lib = dir / "bin" / ("libfw." + so);
        expect(fatal(fs::is_regular_file(lib))) << "no libfw." << so << ": " << s::text(r);
        auto loaded = s::run(*python, {"-I", "-c", "import ctypes, os, sys\nlib = ctypes.CDLL(sys.argv[1], mode=os.RTLD_NOW)\nprint(lib.fw_answer())\n", lib.string()});
        expect(fatal(e2e::succeeded(loaded))) << "a host that never linked x cannot load libfw: " << s::text(loaded);
        expect(fatal(captured(loaded) == "42")) << "expected 42 from libfw in a foreign host, got: " << captured(loaded);
        // B
        const auto both = "extern \"C\" int fw_answer(void);\nextern \"C\" int x_answer(void);\nint main() { return fw_answer() == 42 && x_answer() == 41 ? 0 : 1; }";
        write_app("x = { path = \"../x\" }", both);
        app.remove("target");
        r = app.mcpp({"build"});
        const auto b = s::text(r);
        if (macho) {
            expect(fatal(!r)) << "a static package in two Mach-O images was not refused: " << b;
            expect(fatal(e2e::contains(b, "a static package is linked into more than one image"))) << "the refusal does not say what it refuses: " << b;
            expect(fatal(e2e::contains(b, "'mcpplibs.x' is reached by the program, 'fw'"))) << "the refusal does not name the package and its images: " << b;
            expect(fatal(e2e::contains(b, "linkage = \"shared\""))) << "the refusal does not name the remedy: " << b;
        } else {
            expect(fatal(e2e::succeeded(r))) << "the two-image graph no longer builds on ELF: " << b;
            expect(fatal(e2e::contains(b, "a static package is reachable from more than one image"))) << "the ELF build does not report the two-image package: " << b;
            expect(fatal(e2e::contains(b, "'mcpplibs.x' is reached by the program, 'fw'"))) << "the report does not name the package and its images: " << b;
            dir = first_dir();
            expect(fatal(e2e::succeeded(s::run(dir / "bin/app")))) << "the two-image program exited non-zero";
            app.remove("target");
            r = app.mcpp({"build", "--strict"});
            expect(fatal(!r)) << "--strict accepted a static package in two images: " << s::text(r);
        }
        // C
        write_app("x = { path = \"../x\", linkage = \"shared\" }", both);
        app.remove("target");
        r = app.mcpp({"build", "--strict"});
        expect(fatal(e2e::succeeded(r))) << "linkage = \"shared\" on x did not build under --strict: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "more than one image"))) << "the shared form still reports the two-image package: " << s::text(r);
        dir = first_dir();
        expect(fatal(fs::is_regular_file(dir / "bin" / ("libx." + so)))) << "no libx." << so << " under linkage = \"shared\": " << s::text(r);
        expect(fatal(e2e::succeeded(s::run(dir / "bin/app")))) << "the program over the shared x exited non-zero";
    };

    "835 a shared library is placed by a link"_test[s::elf] = [] {
        // A member's product directory holds the graph-built libraries its program loads (and
        // those they need); two product directories share one file where links are possible.
        s::project ws({{"mcpp.toml", "[workspace]\nmembers = [\"base\", \"shlib\", \"p1\", \"p2\"]\n"},
                       {"base/mcpp.toml", "[package]\nname = \"base\"\nversion = \"0.1.0\"\n\n[targets.base]\nkind = \"shared\"\n"},
                       {"base/src/base.cppm", "export module basis;\nexport int base_v() { return 6; }\n"},
                       {"shlib/mcpp.toml", "[package]\nname = \"shlib\"\nversion = \"0.1.0\"\n\n[dependencies]\nbase = { path = \"../base\" }\n\n[targets.shlib]\nkind = \"shared\"\n"},
                       {"shlib/src/shlib.cppm", "export module shlib;\nimport basis;\nexport int shared_v() { return base_v() + 1; }\n"}});
        for (std::string p : {"p1", "p2"}) {
            ws.write(p + "/mcpp.toml", std::format("[package]\nname = \"{0}\"\nversion = \"0.1.0\"\n\n[dependencies]\nshlib = {{ path = \"../shlib\" }}\n\n[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", p));
            ws.write(p + "/src/main.cpp", "import shlib;\nint main() { return shared_v() == 7 ? 0 : 1; }\n");
        }
        auto r = ws.mcpp({"build", "--workspace"});
        expect(fatal(e2e::succeeded(r))) << "the workspace build failed: " << s::text(r);
        const auto p1 = ending(ws.at("target"), "/bin/p1/p1");
        expect(fatal(p1.has_value())) << "no product directory for p1: " << s::text(r);
        const auto dir = p1->parent_path().parent_path();
        // L1
        for (std::string p : {"p1", "p2"}) {
            expect(fatal(entry_starting(dir / p, "libshlib.so").has_value())) << "L1: libshlib is not placed in bin/" << p << "/";
            expect(fatal(e2e::succeeded(s::run(dir / p / p)))) << "L1: bin/" << p << "/" << p << " did not run from its product directory";
        }
        // L2
        const auto a = dir / "p1/libshlib.so", b = dir / "p2/libshlib.so";
        expect(fatal(fs::exists(a) && fs::exists(b))) << "L2: libshlib.so missing from a product directory";
        if (!fs::equivalent(a, b))
            expect(fatal(s::read_file(a) == s::read_file(b))) << "L2: the two placements differ";
        // L3
        for (std::string p : {"p1", "p2"})
            expect(fatal(entry_starting(dir / p, "libbase.so").has_value())) << "L3: libbase, which libshlib needs, is not placed in bin/" << p << "/";
    };

    "838 a program shipped through artifacts links its own closure"_test[s::elf] = [] {
        // A program a member ships through `artifacts` links with its own package's closure
        // (the library its build program states), runs in both product directories, and its
        // build program's `${mcpp.bin_dir}` is its own product directory.
        const auto tmp = s::case_dir();
        s::project vend(tmp / "vend", {{"mcpp.toml", "[package]\nname = \"vend\"\nversion = \"0.1.0\"\n\n[targets.vend]\nkind = \"lib\"\n"},
                                       {"src/vend.cpp", "extern \"C\" int vend_v() { return 7; }\n"}});
        auto r = vend.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the archive project did not build: " << s::text(r);
        const auto archive = s::find_file(vend.at("target"), "libvend.a");
        expect(fatal(archive.has_value())) << "no libvend.a: " << s::text(r);
        s::project ws(tmp / "ws", {{"mcpp.toml", "[workspace]\nmembers = [\"gui\", \"helper\"]\n"},
                                   {"helper/mcpp.toml", "[package]\nname = \"helper\"\nversion = \"0.1.0\"\n\n[targets.helper]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                   {"helper/build.mcpp", R"(import mcpp;
int main() {
    mcpp::link_search("vendor/lib");
    mcpp::link_lib("vend");
    mcpp::action a;
    a.id = "copy-helper";
    a.role = mcpp::roles::artifact;
    a.arg("${mcpp.self}").arg("stage").arg("--verify").arg("content")
     .arg("--output").arg("${mcpp.bin_dir}/helper.copy")
     .arg("${mcpp.target_file:helper}")
     .input("${mcpp.target_file:helper}")
     .output("${mcpp.bin_dir}/helper.copy")
     .submit();
    return 0;
}
)"},
                                   {"helper/src/main.cpp", "extern \"C\" int vend_v();\nint main() { return vend_v() == 7 ? 0 : 1; }\n"},
                                   {"gui/mcpp.toml", "[package]\nname = \"gui\"\nversion = \"0.1.0\"\n\n[dependencies.helper]\npath = \"../helper\"\nartifacts = [\"helper\"]\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                   {"gui/src/main.cpp", "int main() { return 0; }\n"}});
        fs::create_directories(ws.at("helper/vendor/lib"));
        fs::copy_file(*archive, ws.at("helper/vendor/lib/libvend.a"));
        // A1
        r = ws.mcpp({"build", "--workspace"});
        expect(fatal(e2e::succeeded(r))) << "A1: the workspace build failed: " << s::text(r);
        // A2, A3
        const auto shipped = ending(ws.at("target"), "/bin/gui/helper");
        expect(fatal(shipped.has_value())) << "A2: bin/gui/helper is missing: " << s::text(r);
        expect(fatal(e2e::succeeded(s::run(*shipped)))) << "A2: bin/gui/helper did not run";
        const auto own = ending(ws.at("target"), "/bin/helper/helper");
        expect(fatal(own.has_value())) << "A3: bin/helper/helper is missing: " << s::text(r);
        expect(fatal(e2e::succeeded(s::run(*own)))) << "A3: bin/helper/helper did not run";
        // A4
        expect(fatal(fs::is_regular_file(own->parent_path() / "helper.copy"))) << "A4: ${mcpp.bin_dir} is not the helper's product directory: " << s::text(r);
    };

    "993 a dlopen requirement is judged from the object that loads it"_test[s::elf] = [] {
        // A library a package requires and loads with dlopen is looked up from that package's own
        // shared object (design 2026-11-21, offline and toolchain defaults §3.10): an object with
        // DT_RUNPATH does not see the executable's DT_RPATH, so a library only the executable's
        // run path names is reported unreachable (dlopen-unreachable); the same object with
        // DT_RPATH inherits the executable's and reaches it.
        const auto tmp = s::case_dir();
        const auto host_cc = s::host_cc();
        if (!host_cc) skip("no host C compiler links a shared object");
        auto shared = [&](const fs::path& dir, std::string_view source, std::vector<std::string> args) {
            fs::create_directories(dir / "lib");
            s::project(dir, {{"c/src.c", std::string(source)}});
            args.insert(args.begin(), {"-shared", "-fPIC", "c/src.c"});
            auto r = s::run(*host_cc, args, dir);
            expect(fatal(e2e::succeeded(r))) << "the C compiler did not link: " << s::text(r);
        };
        auto package = [](std::string_view name, std::string_view runtime) {
            return std::format("[package]\nname = \"{0}\"\nversion = \"0.1.0\"\n\n[runtime]\n{1}\n\n[targets.{0}]\nkind = \"lib\"\n", name, runtime);
        };
        shared(tmp / "pb", "int pb_fn(void) { return 2; }\n", {"-Wl,-soname,libpb.so.1", "-o", "lib/libpb.so.1"});
        s::project(tmp / "pb", {{"mcpp.toml", package("pb", "runtime_search_dirs = [\"lib\"]")},
                                {"src/pb.cppm", "export module t993_pb;\nexport int pb_anchor() { return 0; }\n"}});
        const std::string paRuntime = "libraries = [\"pa\"]\nlink_library_dirs = [\"lib\"]\nruntime_search_dirs = [\"lib\"]\n"
                                      "requirements = [{ kind = \"soname\", value = \"libpb.so.1\", phase = \"run\", required = true }]";
        auto link_pa = [&](std::string_view tags) {
            shared(tmp / "pa", "int pa_fn(void) { return 1; }\n",
                   {"-Wl,-soname,libpa.so", std::format("-Wl,{},-rpath,/t993-nowhere", tags), "-o", "lib/libpa.so"});
        };
        link_pa("--enable-new-dtags");
        s::project(tmp / "pa", {{"mcpp.toml", package("pa", paRuntime)},
                                {"src/pa.cppm", "export module t993_pa;\nexport int pa_anchor() { return 0; }\n"}});
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\npa = { path = \"../pa\" }\npb = { path = \"../pb\" }\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                     {"src/main.cpp", "extern \"C\" int pa_fn(void);\nint main() { return pa_fn() == 1 ? 0 : 1; }\n"}});
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the build failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "requires libpb.so.1 at run time and loads it with dlopen from")
                     && e2e::contains(s::text(r), "libpa.so"))) << "a library the loading object cannot see was not reported: " << s::text(r);
        const auto doctor = app.mcpp({"self", "doctor"});
        expect(fatal(e2e::contains(s::text(doctor), "dlopen-unreachable: ") && e2e::contains(s::text(doctor), "requires libpb.so.1")))
            << "self doctor and the post-link validation disagree: " << s::text(doctor);
        link_pa("--disable-new-dtags");
        app.write("src/main.cpp", app.read("src/main.cpp") + "\n");
        r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(s::lacks(s::text(r), "loads it with dlopen from"))) << "an object with DT_RPATH inherits the executable's and reaches it: " << s::text(r);
    };
    "884 a shared library links with its own closure"_test[s::elf] = [] {
        // A shared library that is not the root's links with the graph's flags and those of the
        // packages its owner reaches, and no other package's; a search path only the root names
        // no longer reaches it, and the build says so.
        const auto tmp = s::case_dir();
        const auto home = s::another_home(s::home::fresh);
        fs::create_directories(home);
        s::project(home, {{"config.toml", std::format("[xlings]\nhome = \"{}\"\n", (s::shared_home() / "registry").generic_string())}});
        const s::options o{.env = {{"MCPP_HOME", home.string()}}};
        auto lib = [](const fs::path& root, std::string_view name) { return named_within(root / "target", name, "/bin/"); };
        auto runpath = [](const std::optional<fs::path>& f) { return f ? s::read_header(*f).runpath : std::string(); };
        // A
        s::project ws(tmp / "ws", {{"mcpp.toml", "[workspace]\nmembers = [\"player\", \"other\"]\n"},
                                   {"player/mcpp.toml", "[package]\nname = \"player\"\nversion = \"0.1.0\"\n\n[targets.player_dll]\nkind = \"shared\"\n\n[targets.player]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                   {"player/build.mcpp", "import mcpp;\nint main() {\n    mcpp::link_lib(\"m\");\n    mcpp::link_flag(\"-Wl,-rpath,/t884-player-program\");\n    return 0;\n}\n"},
                                   {"player/src/player.cppm", "export module t884_player;\nexport int answer() { return 42; }\n"},
                                   {"player/src/main.cpp", "import t884_player;\nint main() { return answer() == 42 ? 0 : 1; }\n"},
                                   {"other/mcpp.toml", "[package]\nname = \"other\"\nversion = \"0.1.0\"\n\n[build]\nldflags = [\"-Wl,-rpath,/t884-other\"]\n\n[targets.other]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                   {"other/src/main.cpp", "int main() { return 0; }\n"}});
        for (const std::vector<std::string>& selection : {std::vector<std::string>{}, std::vector<std::string>{"-p", "player"}}) {
            const std::string which = selection.empty() ? "workspace" : "-p player";
            ws.remove("target");
            auto args = selection;
            args.insert(args.begin(), "build");
            auto r = ws.mcpp(args, o);
            expect(fatal(e2e::succeeded(r))) << "A: build of " << which << " failed: " << s::text(r);
            const auto so = lib(ws.dir(), "libplayer_dll.so");
            expect(fatal(so.has_value())) << "A: no libplayer_dll.so: " << s::text(r);
            const auto dyn = runpath(so);
            expect(fatal(e2e::contains(dyn, "/t884-player-program"))) << "A (" << which << "): the member's build.mcpp flag did not reach its shared library: " << dyn;
            expect(fatal(s::lacks(dyn, "/t884-other"))) << "A (" << which << "): another member's flag reached the shared library: " << dyn;
        }
        // B and C
        s::project dep(tmp / "b/dep", {{"mcpp.toml", "[package]\nname = \"dep\"\nversion = \"0.1.0\"\n\n[build]\nldflags = [\"-Wl,-rpath,/t884-dep\"]\n\n[targets.dep]\nkind = \"shared\"\n"},
                                       {"src/dep.cppm", "export module t884_dep;\nexport int dv() { return 1; }\n"}});
        s::project sib(tmp / "b/sib", {{"mcpp.toml", "[package]\nname = \"sib\"\nversion = \"0.1.0\"\n\n[build]\nldflags = [\"-Wl,-rpath,/t884-sib\"]\n\n[targets.sib]\nkind = \"lib\"\n"},
                                       {"src/sib.cppm", "export module t884_sib;\nexport int sv() { return 2; }\n"}});
        s::project app(tmp / "b/app", {{"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[build]\nldflags = [\"-Wl,-rpath,/t884-root\"]\n\n[profile.release]\nldflags = [\"-Wl,-rpath,/t884-profile\"]\n\n"
                                                     "[dependencies]\ndep = { path = \"../dep\" }\nsib = { path = \"../sib\" }\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                       {"src/main.cpp", "import t884_dep;\nimport t884_sib;\nint main() { return dv() + sv() == 3 ? 0 : 1; }\n"}});
        fs::create_directories(tmp / "b/ext");
        auto r = app.mcpp({"build", "--release"}, o);
        expect(fatal(e2e::succeeded(r))) << "B: build failed: " << s::text(r);
        const auto bdyn = runpath(lib(app.dir(), "libdep.so"));
        expect(fatal(e2e::contains(bdyn, "/t884-dep"))) << "B: the dependency's own flag did not reach its shared library: " << bdyn;
        expect(fatal(e2e::contains(bdyn, "/t884-profile"))) << "B: the profile's flag did not reach the dependency's shared library: " << bdyn;
        for (std::string_view foreign : {"/t884-root", "/t884-sib"})
            expect(fatal(s::lacks(bdyn, foreign))) << "B: " << foreign << " reached the dependency's shared library: " << bdyn;
        const auto appdyn = runpath(lib(app.dir(), "app"));
        expect(fatal(e2e::contains(appdyn, "/t884-root"))) << "B: the root's own flag did not reach the root's program: " << appdyn;
        // C: the root names a search path the dependency's library needs.
        const auto ext = (tmp / "b/ext").generic_string();
        s::project(tmp / "b/ext", {{"ext.c", "int ext_fn(void) { return 1; }\n"}});
        const auto host_cc = s::host_cc();
        expect(fatal(host_cc.has_value())) << "C: no host C compiler links a shared object";
        auto cc = s::run(*host_cc, {"-shared", "-fPIC", "ext.c", "-o", "libt884ext.so"}, tmp / "b/ext");
        expect(fatal(e2e::succeeded(cc))) << "the C compiler did not make libt884ext.so: " << s::text(cc);
        auto dep_manifest = dep.read("mcpp.toml");
        auto replace = [](std::string& text, std::string_view from, std::string_view to) {
            const auto at = text.find(from);
            if (at != std::string::npos) text.replace(at, from.size(), to);
        };
        replace(dep_manifest, "ldflags = [\"-Wl,-rpath,/t884-dep\"]", "ldflags = [\"-Wl,-rpath,/t884-dep\", \"-lt884ext\"]");
        dep.write("mcpp.toml", dep_manifest);
        auto app_manifest = app.read("mcpp.toml");
        replace(app_manifest, "ldflags = [\"-Wl,-rpath,/t884-root\"]", std::format("ldflags = [\"-Wl,-rpath,/t884-root\", \"-L{}\"]", ext));
        app.write("mcpp.toml", app_manifest);
        app.remove("target");
        r = app.mcpp({"build"}, o);
        expect(fatal(!r)) << "C: the dependency's library linked with a search path only the root names: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "no longer reach the shared"))) << "C: the change of rule was not stated: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "-L" + ext))) << "C: the statement does not name the root's search path: " << s::text(r);
        // The dependency stating its own search path is the migration.
        dep_manifest = dep.read("mcpp.toml");
        replace(dep_manifest, "\"-lt884ext\"]", std::format("\"-L{0}\", \"-Wl,-rpath,{0}\", \"-lt884ext\"]", ext));
        dep.write("mcpp.toml", dep_manifest);
        r = app.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "C: the dependency's own search path did not fix the link: " << s::text(r);
    };

    "1167 a dependency's shared library is named by its package"_test[s::elf] = [] {
        // mcpp#975 (SPEC-004 §11 item 8, docs/04): two packages whose targets share a name, linked
        // shared, make two libraries named by their packages, and the program loads both; two
        // libraries stated with one soname are refused at planning, naming both packages.
        const auto tmp = s::case_dir();
        auto library = [&](std::string_view ns, int value, std::string_view extra) {
            s::project(tmp / ns, {{"mcpp.toml", std::format("[package]\nnamespace = \"{0}\"\nname      = \"util\"\nversion   = \"0.1.0\"\n\n[build]\nsources = [\"src/*.cpp\"]\n\n[targets.util]\nkind = \"shared\"\n{1}", ns, extra)},
                                  {"src/util.cpp", std::format("extern \"C\" int {}_value() {{ return {}; }}\n", ns, value)}});
        };
        library("acme", 40, "");
        library("beta", 2, "");
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.cpp\"]\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n[dependencies]\nacme.util = { path = \"../acme\" }\nbeta.util = { path = \"../beta\" }\n"},
                                     {"src/main.cpp", "extern \"C\" int acme_value();\nextern \"C\" int beta_value();\nint main() { return acme_value() + beta_value() == 42 ? 0 : 1; }\n"}});
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the build failed\n" << s::text(r);
        expect(s::find_file(app.at("target"), "libacme_util.so").has_value()) << "no libacme_util.so";
        expect(s::find_file(app.at("target"), "libbeta_util.so").has_value()) << "no libbeta_util.so";
        expect(!s::find_file(app.at("target"), "libutil.so").has_value()) << "a dependency's library took its target's name";
        r = app.mcpp({"run"});
        expect(e2e::succeeded(r)) << "the program did not load both libraries\n" << s::text(r);
        library("acme", 40, "soname = \"libsame.so\"\n");
        library("beta", 2, "soname = \"libsame.so\"\n");
        r = app.mcpp({"build"});
        expect(fatal(!e2e::succeeded(r))) << "two libraries loaded by one name were accepted";
        expect(e2e::contains(s::text(r), "both make a shared library loaded as 'libsame.so'")) << s::text(r);
    };

    "1168 a library linked from one file and loaded from another is refused"_test[s::elf][s::gcc] = [] {
        // mcpp#537 (docs/04 §2.11): the link reads libfoo.so from one directory, and the image's
        // run path finds another libfoo.so; the build is refused naming both files. With one
        // directory for both, the build runs.
        const auto gcc = s::host_cc();
        expect(fatal(gcc.has_value())) << "no host C compiler links a shared object";
        s::project app({{"a/foo.c", "int foo_answer(void) { return 1; }\n"},
                        {"b/foo.c", "int foo_answer(void) { return 2; }\n"},
                        {"src/main.cpp", "extern \"C\" int foo_answer();\nint main() { return foo_answer() > 0 ? 0 : 1; }\n"},
                        {"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}});
        for (auto dir : {"a", "b"})
            expect(fatal(e2e::succeeded(s::run(*gcc, {"-shared", "-fPIC", "-Wl,-soname,libfoo.so", app.at(std::format("{}/foo.c", dir)).string(),
                                                      "-o", app.at(std::format("{}/libfoo.so", dir)).string()}))));
        auto program = [&](std::string_view linked, std::string_view loaded) {
            app.write("build.mcpp", std::format("import mcpp;\n#include <string>\nint main() {{\n    const std::string root = mcpp::manifest_dir();\n    mcpp::link_lib(\"foo\");\n"
                                                "    mcpp::link_search((root + \"/{}\").c_str());\n    mcpp::runtime_search_dir((root + \"/{}\").c_str());\n    return 0;\n}}\n", linked, loaded));
        };
        program("a", "b");
        auto r = app.mcpp({"build"});
        expect(fatal(!e2e::succeeded(r))) << "a library linked from a/ and loaded from b/ was accepted\n" << s::text(r);
        expect(e2e::contains(s::text(r), "libfoo.so is linked from one file and loaded from another")) << s::text(r);
        expect(e2e::contains(s::text(r), "/a/libfoo.so") && e2e::contains(s::text(r), "/b/libfoo.so")) << "the refusal does not name both files\n" << s::text(r);
        program("a", "a");
        r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the build that links and loads one file failed\n" << s::text(r);
        expect(e2e::succeeded(app.mcpp({"run"}))) << "the program did not run";
    };

    "1169 a shared image over the static TLS dlopen serves is reported"_test[s::elf][s::gcc] = [] {
        // mcpp#907 (SPEC-016 §2.1 `dlopen-static-tls`, docs/04): a shared library whose objects use
        // the initial-exec model for 4096 bytes is reported against the row's bound; with an exact
        // `exports` name, which says a host loads it by name, it is refused.
        s::project p({{"src/lib.cpp", "thread_local char big[4096];\nextern \"C\" int plugin_entry() { big[0] = 1; return big[0]; }\n"}});
        auto manifest = [&](std::string_view extra) {
            p.write("mcpp.toml", std::format("[package]\nname    = \"tlsimg\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.cpp\"]\ncxxflags = [\"-ftls-model=initial-exec\"]\n\n"
                                             "[targets.tlsimg]\nkind = \"shared\"\n{}", extra));
        };
        manifest("");
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "a library a program loads at start-up was refused\n" << s::text(r);
        expect(e2e::contains(s::text(r), "bytes of thread-local storage (DF_STATIC_TLS)")) << "the image was not reported\n" << s::text(r);
        manifest("exports = [\"plugin_entry\"]\n");
        r = p.mcpp({"build"});
        expect(fatal(!e2e::succeeded(r))) << "an image a host loads by name was not refused\n" << s::text(r);
        expect(e2e::contains(s::text(r), "its `exports` name the symbols a host loads it to call")) << s::text(r);
        expect(e2e::contains(s::text(r), "-ftls-model=global-dynamic")) << "the refusal gives no remedy\n" << s::text(r);
    };

    "1170 an exact export the image does not define is reported"_test[s::elf][s::gcc] = [] {
        // mcpp#982 (SPEC-004 §11 item 7): an `exports` entry without a wildcard names a symbol a
        // host looks up; one the image does not define is reported as `unmatched-exact-export`,
        // and neither a defined name nor a pattern that matches nothing is.
        s::project p({{"src/lib.cpp", "extern \"C\" int defined_entry() { return 1; }\n"},
                      {"mcpp.toml", "[package]\nname    = \"exactexp\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.cpp\"]\n\n"
                                    "[targets.exactexp]\nkind    = \"shared\"\nexports = [\"defined_entry\", \"missing_entry\", \"nomatch_*\"]\n"}});
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(e2e::contains(s::text(r), "[targets.exactexp] exports \"missing_entry\" names a symbol the image does not define")) << s::text(r);
        expect(e2e::contains(s::text(r), "unmatched-exact-export") || e2e::contains(s::text(r), "2027.3")) << "the report names no deprecation\n" << s::text(r);
        expect(s::lacks(s::text(r), "\"defined_entry\" names a symbol")) << "a defined name was reported";
        expect(s::lacks(s::text(r), "nomatch_")) << "a pattern was reported";
    };
}};

}  // namespace
