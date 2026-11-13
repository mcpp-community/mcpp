// The ABI a row's selector states and the forms a row chooses: `threads` is one switch for the
// graph, `exceptions` is the second ABI member, and a row states a library form
// (formerly tests/e2e/647, 653, 678).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// `ls -t $(find <root> -name build.ninja -not -path '*/.build-mcpp/*') | head -1`.
std::optional<fs::path> newest_ninja(const fs::path& root) {
    std::optional<fs::path> best;
    fs::file_time_type when{};
    for (const auto& f : s::find_files(root, R"(build\.ninja)")) {
        if (f.generic_string().contains("/.build-mcpp/")) continue;
        const auto t = fs::last_write_time(f);
        if (!best || t > when) { best = f; when = t; }
    }
    return best;
}

// `find <root> -name build.ninja -not -path '*/.build-mcpp/*' | head -1`.
std::optional<fs::path> first_ninja(const fs::path& root) {
    for (const auto& f : s::find_files(root, R"(build\.ninja)"))
        if (!f.generic_string().contains("/.build-mcpp/")) return f;
    return std::nullopt;
}

std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> out;
    for (auto part : std::views::split(text, '\n')) out.emplace_back(part.begin(), part.end());
    return out;
}

std::string emscripten_abi_table(std::string_view member) {
    return std::format("[target.'cfg(os = \"emscripten\")'.abi]\n{} = true\n", member);
}

E2E abi{"abi", [] {
    "647 abi threads is one switch for the graph"_test[s::elf] = [] {
        // `[target.<selector>.abi] threads = true` in the root is one switch: -pthread reaches a
        // dependency's compile command and the fingerprint moves; a table scoped to another
        // system is inert; a requirement without the switch is refused, naming the package or the
        // feature; a dependency's own table is reported and ignored.
        const s::options o{.env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};
        const auto tmp = s::case_dir();
        const std::string abi_table = "[target.'cfg(os = \"linux\")'.abi]\nthreads = true\n";
        s::project dep(tmp / "mtdep", {{"src/mtdep.cppm", "export module mtdep;\nexport int mt_value() { return 5; }\n"}});
        auto write_dep = [&](std::string_view package_lines, std::string_view tables) {
            dep.write("mcpp.toml", std::format("[package]\nname    = \"mtdep\"\nversion = \"0.1.0\"\n{}\n\n[targets.mtdep]\nkind = \"lib\"\n\n[features]\nmt = {{ requires_abi = {{ threads = true }} }}\n\n{}\n", package_lines, tables));
        };
        s::project app(tmp / "app", {{"src/main.cpp", "import mtdep;\nint main() { return mt_value() == 5 ? 0 : 1; }\n"}});
        auto write_app = [&](std::string_view edge, std::string_view tables) {
            app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nmtdep = {}\n\n{}\n", edge, tables));
        };
        // The compile command of mtdep.cppm carries -pthread.
        auto pthread_in_dep_command = [&] {
            fs::path cdb;
            if (auto n = newest_ninja(app.at("target"))) cdb = n->parent_path() / "compile_commands.json";
            if (cdb.empty() || !fs::exists(cdb)) cdb = s::newest_file(app.dir(), "compile_commands.json").value_or(fs::path());
            if (cdb.empty()) return false;
            const auto doc = s::json_file(cdb);
            if (!doc.is_array()) return false;
            for (const auto& e : doc) {
                const auto text = e.dump();
                if (text.contains("mtdep.cppm") && text.contains("-pthread")) return true;
            }
            return false;
        };
        auto fingerprint_dir = [&] { return newest_ninja(app.at("target")).value_or(fs::path()).parent_path(); };
        write_dep("", "");
        write_app("{ path = \"../mtdep\" }", "");
        auto r = app.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "FAIL: the plain build failed: " << s::text(r);
        const auto plain_dir = fingerprint_dir();
        expect(fatal(!pthread_in_dep_command())) << "FAIL: control: -pthread is already on the dependency's compile command without the switch: " << s::text(r);
        write_app("{ path = \"../mtdep\" }", emscripten_abi_table("threads"));
        r = app.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "FAIL: a table scoped to emscripten failed a host build: " << s::text(r);
        expect(fatal(fingerprint_dir() == plain_dir)) << "FAIL: a table scoped to emscripten moved the host build to another fingerprint directory: " << s::text(r);
        expect(fatal(!pthread_in_dep_command())) << "FAIL: a table scoped to emscripten put -pthread on a host build: " << s::text(r);
        write_dep("requires_abi = { threads = true }", "");
        r = app.mcpp({"build"}, o);
        expect(fatal(!r)) << "FAIL: a dependency requiring threads was built without them: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "`mtdep` requires the artefact's ABI to have threads (the package)"))) << "FAIL: the refusal does not name the package requirement: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "threads = true"))) << "FAIL: the refusal does not show the table that states it: " << s::text(r);
        write_dep("", "");
        write_app("{ path = \"../mtdep\", features = [\"mt\"] }", "");
        r = app.mcpp({"build"}, o);
        expect(fatal(!r)) << "FAIL: a feature requiring threads was built without them: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "`mtdep` requires the artefact's ABI to have threads (feature `mt`)"))) << "FAIL: the refusal does not name the feature: " << s::text(r);
        write_app("{ path = \"../mtdep\", features = [\"mt\"] }", abi_table);
        r = app.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "FAIL: the build with threads failed: " << s::text(r);
        expect(fatal(pthread_in_dep_command())) << "FAIL: -pthread did not reach the dependency's compile command: " << s::text(r);
        expect(fatal(fingerprint_dir() != plain_dir)) << "FAIL: the build with threads reused the fingerprint directory of the build without";
        r = app.mcpp({"run"}, o);
        expect(fatal(e2e::succeeded(r))) << "FAIL: the program did not run: " << s::text(r);
        app.remove("target");
        write_dep("", abi_table);
        write_app("{ path = \"../mtdep\" }", "");
        r = app.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "FAIL: a dependency's abi table failed the build: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "`mtdep` declares [target.<selector>.abi], which only the root manifest decides"))) << "FAIL: a dependency's abi table was not reported: " << s::text(r);
        expect(fatal(!pthread_in_dep_command())) << "FAIL: a dependency's abi table changed the dependency's compile command";
    };

    "653 exceptions is the second abi member"_test[s::elf] = [] {
        // `[target.<selector>.abi] exceptions = true`: a Linux build is byte-identical with an
        // emscripten-only table; on the Web row the program builds without it and aborts at the
        // throw, and with it prints the marker, the flag reaching the dependency and the std
        // module and the fingerprint moving; a dependency requiring it is refused without it.
        bool have_emsdk = false;
        for (const auto& root : {s::shared_home() / "registry/data/xpkgs", fs::path(s::env("HOME").value_or("")) / ".xlings/data/xpkgs"})
            for (const auto& em : s::glob(root, "xim-x-emsdk/*/emscripten/em++"))
                if ((fs::status(em).permissions() & fs::perms::owner_exec) != fs::perms::none) have_emsdk = true;
        if (!have_emsdk) skip("653 -- no emsdk payload installed (looked under xim-x-emsdk/*/emscripten)");
        const auto tmp = s::case_dir();
        // 1. A Linux build of a manifest carrying the table is byte-identical to one without.
        s::project lx(tmp / "lx", {{"mcpp.toml", "[package]\nname    = \"lx\"\nversion = \"0.1.0\"\n\n[targets.lx]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                   {"src/main.cpp", "import std;\nint main() { std::println(\"ok\"); return 0; }\n"}});
        auto r = lx.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: the plain Linux build failed: " << s::text(r);
        const auto plain_ninja = s::read_file(first_ninja(lx.at("target")).value_or(fs::path()));
        lx.remove("target");
        lx.write("mcpp.toml", lx.read("mcpp.toml") + "\n" + emscripten_abi_table("exceptions"));
        r = lx.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: the Linux build with the table failed: " << s::text(r);
        const auto with_ninja = s::read_file(first_ninja(lx.at("target")).value_or(fs::path()));
        expect(fatal(!plain_ninja.empty() && plain_ninja == with_ninja)) << "FAIL: a Linux build.ninja differs with the emscripten-only abi table present";
        // 2. Without exceptions the build and link succeed, and the run fails at the throw.
        s::project(tmp / "excdep", {{"mcpp.toml", "[package]\nname    = \"excdep\"\nversion = \"0.1.0\"\n\n[targets.excdep]\nkind = \"lib\"\n"},
                                    {"src/excdep.cppm", "export module excdep;\nimport std;\nexport void throwing() { throw std::runtime_error(\"boom\"); }\n"}});
        s::project app(tmp / "app", {{"src/main.cpp", R"(import excdep;
import std;
int main() {
    try {
        throwing();
    } catch (std::exception const& e) {
        std::println("caught: {}", e.what());
        return 0;
    }
    return 1;
}
)"}});
        auto write_app = [&](std::string_view tables) {
            app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nexcdep = {{ path = \"../excdep\" }}\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n[target.wasm32-emscripten]\nrunner = [\"node\"]\n\n{}\n", tables));
        };
        const std::vector<std::string> web{"--target", "wasm32-emscripten"};
        auto with = [&](std::string_view command) {
            std::vector<std::string> args{std::string(command)};
            args.insert(args.end(), web.begin(), web.end());
            return args;
        };
        write_app("");
        r = app.mcpp(with("build"));
        expect(fatal(e2e::succeeded(r))) << "FAIL: the build without the abi table failed (expected: it succeeds; only the run fails): " << s::text(r);
        const auto plain_dir = newest_ninja(app.at("target/wasm32-emscripten")).value_or(fs::path()).parent_path();
        r = app.mcpp(with("run"));
        expect(fatal(!r)) << "FAIL: mcpp run succeeded without -fexceptions -- the throw should have aborted: " << s::text(r);
        expect(fatal(s::no_line(s::text(r), "^caught: boom$"))) << "FAIL: the marker printed without -fexceptions";
        expect(fatal(e2e::contains(s::text(r), "exception catching is not enabled"))) << "FAIL: the run did not fail with the expected Emscripten abort: " << s::text(r);
        // 3. With exceptions the marker prints, the flags reach both compiles, and the
        //    fingerprint moves.
        write_app(emscripten_abi_table("exceptions"));
        r = app.mcpp(with("build"));
        expect(fatal(e2e::succeeded(r))) << "FAIL: the build with the abi table failed: " << s::text(r);
        const auto exc_dir = newest_ninja(app.at("target/wasm32-emscripten")).value_or(fs::path()).parent_path();
        expect(fatal(exc_dir != plain_dir)) << "FAIL: the build with exceptions reused the fingerprint directory of the build without";
        r = app.mcpp(with("run"));
        expect(fatal(e2e::succeeded(r))) << "FAIL: the program did not run with the abi table present: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), "^caught: boom$"))) << "FAIL: mcpp run did not print the marker";
        const auto ninja = s::read_file(exc_dir / "build.ninja");
        bool cxx_has = false;
        for (const auto& l : lines_of(ninja))
            if (l.starts_with("cxxflags") && l.contains("-fexceptions")) cxx_has = true;
        expect(fatal(cxx_has)) << "FAIL: -fexceptions is not in build.ninja's cxxflags (the dependency's compile line)";
        std::string stdcache_pcm;
        static const std::regex pcm{R"(/[^ ]*pcm\.cache/std\.pcm)"};
        for (auto it = std::sregex_iterator(ninja.begin(), ninja.end(), pcm); it != std::sregex_iterator(); ++it)
            if (it->str().contains("build-cache")) { stdcache_pcm = it->str(); break; }
        expect(fatal(!stdcache_pcm.empty())) << "FAIL: build.ninja does not name a cached std.pcm to stage";
        const auto stdmodule_json = fs::path(stdcache_pcm).parent_path().parent_path() / "std-module.json";
        expect(fatal(fs::is_regular_file(stdmodule_json))) << "FAIL: no std-module.json beside the cached std.pcm build.ninja names: " << stdmodule_json.string();
        expect(fatal(e2e::contains(s::read_file(stdmodule_json), "-fexceptions"))) << "FAIL: -fexceptions is not recorded on the std module's cached prebuild command";
        // 4. A dependency requiring exceptions is refused on the Web build without the table.
        s::project(tmp / "excreq", {{"mcpp.toml", "[package]\nname         = \"excreq\"\nversion      = \"0.1.0\"\nrequires_abi = { exceptions = true }\n\n[targets.excreq]\nkind = \"lib\"\n"},
                                    {"src/excreq.cppm", "export module excreq;\nexport int v() { return 1; }\n"}});
        s::project app3(tmp / "app3", {{"mcpp.toml", "[package]\nname    = \"app3\"\nversion = \"0.1.0\"\n\n[dependencies]\nexcreq = { path = \"../excreq\" }\n\n[targets.app3]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n[target.wasm32-emscripten]\nrunner = [\"node\"]\n"},
                                       {"src/main.cpp", "import excreq;\nint main() { return v() == 1 ? 0 : 1; }\n"}});
        r = app3.mcpp(with("build"));
        expect(fatal(!r)) << "FAIL: a dependency requiring exceptions was built for the Web without the root's table: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "`excreq` requires the artefact's ABI to have exceptions (the package)"))) << "FAIL: the refusal does not name 'exceptions': " << s::text(r);
    };

    "678 a row states a library form"_test[s::elf] = [] {
        // `[target.<selector>.targets.<name>] kind` is the per-row form of a library target: a
        // matching row builds it shared and records why, a non-matching one leaves it static, an
        // edge's `linkage = "static"` degrades it (refused by --strict), and a row naming no
        // target, a program or a non-library kind is refused.
        const auto tmp = s::case_dir();
        auto reason_of = [](const fs::path& resolution, std::string_view canonical) -> std::string {
            const auto doc = s::json_file(resolution);
            std::string out;
            for (const auto& p : doc["graph"]["packages"])
                if (p["package"]["canonical"].get<std::string>() == canonical) {
                    const auto link = p.value("link", nlohmann::json::object());
                    out += std::format("{} {}\n", link.value("form", std::string()), link.value("reason", std::string()));
                }
            if (out.ends_with('\n')) out.pop_back();
            return out;
        };
        auto make_fw = [&](std::string_view dir, std::string_view os, std::string_view extra) {
            s::project(tmp / dir, {{"mcpp.toml", std::format("[package]\nnamespace = \"demo\"\nname      = \"fw\"\nversion   = \"0.1.0\"\n\n[targets.fw]\nkind = \"lib\"\n\n[target.'cfg(os = \"{}\")'.targets.fw]\nkind = \"shared\"\n{}\n", os, extra)},
                                   {"src/fw.cppm", "export module fw;\nexport int fw_anchor() { return 41; }\n"}});
        };
        auto make_app = [&](std::string_view dir, std::string_view fw, std::string_view edge) {
            return s::project(tmp / dir, {{"mcpp.toml", std::format("[package]\nname = \"{}\"\nversion = \"0.1.0\"\n[dependencies]\ndemo.fw = {{ path = \"../{}\"{} }}\n", dir, fw, edge)},
                                          {"src/main.cpp", "import fw;\nint main() { return fw_anchor() == 41 ? 0 : 1; }\n"}});
        };
        make_fw("fwlinux", "linux", "");
        make_fw("fwwin", "windows", "");
        auto a = make_app("a", "fwlinux", "");
        auto r = a.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: A: build failed: " << s::text(r);
        expect(fatal(s::find_file(a.at("target"), "libfw.so").has_value())) << "FAIL: A: no libfw.so: " << s::text(r);
        auto rec = reason_of(s::find_file(a.at("target"), "resolution.json").value_or(fs::path()), "demo.fw@0.1.0");
        expect(fatal(rec == "shared row-kind")) << "FAIL: A: link record is '" << rec << "'";
        auto b = make_app("b", "fwwin", "");
        r = b.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: B: build failed: " << s::text(r);
        expect(fatal(!s::find_file(b.at("target"), "libfw.so").has_value())) << "FAIL: B: a non-matching row built libfw.so";
        rec = reason_of(s::find_file(b.at("target"), "resolution.json").value_or(fs::path()), "demo.fw@0.1.0");
        expect(fatal(rec == "static default")) << "FAIL: B: link record is '" << rec << "'";
        auto c = make_app("c", "fwlinux", ", linkage = \"static\"");
        r = c.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: C: build failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "its manifest states [target.'cfg(os = \"linux\")'.targets.fw] kind = \"shared\""))) << "FAIL: C: the degradation does not name the row's statement: " << s::text(r);
        c.remove("target");
        r = c.mcpp({"build", "--strict"});
        expect(fatal(!r)) << "FAIL: C: --strict accepted the degradation: " << s::text(r);
        make_fw("nosuch", "linux", "[target.'cfg(os = \"linux\")'.targets.nope]\nkind = \"shared\"");
        auto d1 = make_app("d1", "nosuch", "");
        r = d1.mcpp({"build"});
        expect(fatal(!r)) << "FAIL: D: a row naming no target was accepted";
        expect(fatal(e2e::contains(s::text(r), "names no target of this package"))) << "FAIL: D: refusal wording: " << s::text(r);
        s::project prog(tmp / "prog", {{"mcpp.toml", "[package]\nname    = \"prog\"\nversion = \"0.1.0\"\n\n[targets.prog]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n[target.'cfg(os = \"linux\")'.targets.prog]\nkind = \"shared\"\n"},
                                       {"src/main.cpp", "int main() { return 0; }\n"}});
        r = prog.mcpp({"build"});
        expect(fatal(!r)) << "FAIL: D: a row made a program shared";
        expect(fatal(e2e::contains(s::text(r), "is a program target"))) << "FAIL: D: program refusal wording: " << s::text(r);
        prog.write("mcpp.toml", "[package]\nname    = \"prog\"\nversion = \"0.1.0\"\n\n[targets.lib]\nkind = \"lib\"\n\n[target.'cfg(os = \"linux\")'.targets.lib]\nkind = \"bin\"\n");
        r = prog.mcpp({"build"});
        expect(fatal(!r)) << "FAIL: D: a row kind 'bin' was accepted";
        expect(fatal(e2e::contains(s::text(r), "a row chooses between the library forms only"))) << "FAIL: D: kind refusal wording: " << s::text(r);
    };
}};

}  // namespace
