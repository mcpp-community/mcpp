// The compile database a build writes: at the project's root when asked, its prebuilt-module
// paths absolute and unquoted, every source of the build and the tests in it, restored after a
// deletion, replacing foreign entries, and opt-in (formerly tests/e2e/47, 76, 77, 781, 782, 937).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

const s::options at_root{.env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};

E2E root{"root", [] {
    "47 cdb prebuilt module path abs"_test = [] {
        // Every -fprebuilt-module-path= of the database is absolute, unquoted, free of ninja
        // escapes, and names a pcm.cache or gcm.cache directory.
        auto p = s::project::scaffold("app");
        auto r = p.mcpp({"build"}, at_root);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(p.exists("compile_commands.json"))) << "no compile_commands.json generated";
        const auto text = p.read("compile_commands.json");
        expect(fatal(s::lacks(text, R"(\"-fprebuilt-module-path)") && s::lacks(text, "'-fprebuilt-module-path"))) << "a quoted -fprebuilt-module-path";
        for (const auto& e : s::json(text))
            for (const auto& a : e.value("arguments", nlohmann::json::array())) {
                if (!a.is_string() || !a.get<std::string>().starts_with("-fprebuilt-module-path=")) continue;
                auto v = a.get<std::string>().substr(std::string_view("-fprebuilt-module-path=").size());
                if (v.ends_with('\r')) v.pop_back();
                if (v.empty()) continue;
                expect(fatal(!v.contains("$:") && !v.contains("$ ") && !v.contains("$$"))) << "a ninja escape in " << v;
                expect(fatal(!v.starts_with('"') && !v.starts_with('\'') && !v.ends_with('"') && !v.ends_with('\''))) << "a quoted value " << v;
                expect(fatal(v.starts_with('/') || std::regex_search(v, std::regex{"^[A-Za-z]:"}))) << "not absolute: " << v;
                std::ranges::replace(v, '\\', '/');
                const auto base = v.substr(v.rfind('/') + 1);
                expect(fatal(base == "pcm.cache" || base == "gcm.cache")) << "basename is not pcm.cache/gcm.cache: '" << base << "'";
            }
    };

    "76 compile commands generated"_test = [] {
        // The root's database is a non-empty JSON array whose entries have a file, a directory
        // and a command or arguments, cover src/ and a second source glob, and spell no file with
        // mixed separators.
        auto p = s::project::scaffold("app");
        p.write("generated/modules/extra.cpp", "int mcpp_extra_anchor() { return 1; }\n");
        p.write("mcpp.toml", p.read("mcpp.toml") + "\n[build]\nsources = [\"src/**/*.cpp\", \"generated/modules/**/*.cpp\"]\n");
        auto r = p.mcpp({"build"}, at_root);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(p.exists("compile_commands.json"))) << "no compile_commands.json generated at project root";
        const auto text = p.read("compile_commands.json");
        expect(fatal(!text.empty() && text.front() == '[')) << "not a JSON array: " << text;
        for (std::string_view key : {R"("directory")", R"("file")"}) expect(fatal(e2e::contains(text, key))) << "missing " << key;
        expect(fatal(s::has_line(text, R"("command"|"arguments")"))) << "no command or arguments";
        expect(fatal(e2e::contains(text, "main.cpp"))) << "no entry for src/main.cpp";
        expect(fatal(e2e::contains(text, "extra.cpp"))) << "no entry for the second glob's source";
        const auto d = s::json(text);
        expect(fatal(d.is_array() && !d.empty())) << "CDB must be a non-empty JSON array";
        for (const auto& e : d) {
            expect(fatal(e.contains("file") && e.contains("directory"))) << "entry missing file/directory: " << e.dump();
            expect(fatal(e.contains("command") || e.contains("arguments"))) << "entry missing command/arguments: " << e.dump();
            const auto f = e["file"].get<std::string>();
            expect(fatal(!(f.contains('\\') && f.contains('/')))) << "mixed separators in file: " << f;
        }
    };

    "77 cdb preserves test entries"_test = [] {
        // A build after `test` keeps the test's entry; once the test is gone, it leaves.
        auto p = s::project::scaffold("app");
        auto r = p.mcpp({"test"}, at_root);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(e2e::contains(p.read("compile_commands.json"), "test_smoke.cpp"))) << "test entry missing after mcpp test";
        p.write("src/main.cpp", p.read("src/main.cpp") + "\n// touch to force rebuild\n");
        r = p.mcpp({"build"}, at_root);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(e2e::contains(p.read("compile_commands.json"), "test_smoke.cpp"))) << "a build dropped the test's entry";
        expect(fatal(e2e::contains(p.read("compile_commands.json"), "main.cpp"))) << "no entry for main.cpp";
        p.remove("tests/test_smoke.cpp");
        p.write("src/main.cpp", p.read("src/main.cpp") + "\n// touch again\n");
        r = p.mcpp({"build"}, at_root);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(s::lacks(p.read("compile_commands.json"), "test_smoke.cpp"))) << "a removed test's entry stayed";
    };
    "782 foreign root cdb entries are replaced"_test = [] {
        // A root database holding entries mcpp did not write is replaced by the configuration's,
        // with one warning naming their count; a file holding only mcpp's draws no warning.
        auto p = s::project::scaffold("app");
        auto r = p.mcpp({"build"}, at_root);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        p.write("compile_commands.json", R"([
  {"directory": "/somewhere", "file": "/somewhere/other.cpp",
   "arguments": ["cc", "-c", "/somewhere/other.cpp", "-o", "/somewhere/other.o"]},
  {"directory": "/somewhere", "file": "/somewhere/second.cpp",
   "arguments": ["cc", "-c", "/somewhere/second.cpp", "-o", "/somewhere/second.o"]}
]
)");
        fs::last_write_time(p.at("src/main.cpp"), fs::file_time_type::clock::now());
        r = p.mcpp({"build"}, at_root);
        expect(fatal(e2e::succeeded(r))) << "build failed: " << s::text(r);
        expect(fatal(s::count_lines(s::text(r), "held 2 entries mcpp did not write") == 1u)) << "expected exactly one warning naming 2 foreign entries: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "mcpp's configuration"))) << "the warning does not say the file now holds mcpp's configuration: " << s::text(r);
        const auto cdb = p.read("compile_commands.json");
        expect(fatal(s::lacks(cdb, "other.cpp") && s::lacks(cdb, "second.cpp"))) << "a foreign entry survived the replace";
        expect(fatal(e2e::contains(cdb, "main.cpp"))) << "the project's own entry is missing after the replace";
        r = p.mcpp({"build", "--no-cache"}, at_root);
        expect(fatal(e2e::succeeded(r))) << "second build failed: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "did not write"))) << "warned again on a file that already held only mcpp's configuration: " << s::text(r);
    };

    "937 the root compile database is opt in"_test = [] {
        // A: no root database by default, the configuration's always. B: [build]
        // compile_commands publishes it, on the fast path too. C: removing the key names the
        // left copy once and stops updating it. D: MCPP_COMPILE_COMMANDS=off overrides the key.
        const s::options unset{.env = {{"MCPP_COMPILE_COMMANDS", std::nullopt}}};
        auto p = s::project::scaffold("app");
        auto r = p.mcpp({"build"}, unset);
        expect(fatal(e2e::succeeded(r))) << "A: the build failed: " << s::text(r);
        expect(fatal(!p.exists("compile_commands.json"))) << "A: a root database was written by default";
        expect(fatal(!s::find_files(p.at("target"), R"(compile_commands\.json)").empty())) << "A: no configuration database";
        const auto key = "\n[build]\ncompile_commands = \"compile_commands.json\"\n";
        const auto base = p.read("mcpp.toml");
        p.write("mcpp.toml", base + key);
        r = p.mcpp({"build"}, unset);
        expect(fatal(e2e::succeeded(r))) << "B: the build failed: " << s::text(r);
        expect(fatal(p.exists("compile_commands.json"))) << "B: the key published nothing: " << s::text(r);
        p.remove("compile_commands.json");
        r = p.mcpp({"build"}, unset);
        expect(fatal(e2e::succeeded(r))) << "B: the fast build failed: " << s::text(r);
        expect(fatal(p.exists("compile_commands.json"))) << "B: the fast path did not publish the asked copy: " << s::text(r);
        const auto keep = p.read("compile_commands.json");
        p.write("mcpp.toml", base + "\n[build]\n");
        r = p.mcpp({"build"}, unset);
        expect(fatal(e2e::succeeded(r))) << "C: the build failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "is no longer updated"))) << "C: the left copy was not named: " << s::text(r);
        fs::last_write_time(p.at("src/main.cpp"), fs::file_time_type::clock::now());
        r = p.mcpp({"build"}, unset);
        expect(fatal(e2e::succeeded(r))) << "C: the second build failed: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "is no longer updated"))) << "C: named twice: " << s::text(r);
        expect(fatal(p.read("compile_commands.json") == keep)) << "C: the left copy was updated";
        p.remove("compile_commands.json");
        p.write("mcpp.toml", p.read("mcpp.toml") + key);
        fs::last_write_time(p.at("src/main.cpp"), fs::file_time_type::clock::now());
        r = p.mcpp({"build"}, {.env = {{"MCPP_COMPILE_COMMANDS", "off"}}});
        expect(fatal(e2e::succeeded(r))) << "D: the build failed: " << s::text(r);
        expect(fatal(!p.exists("compile_commands.json"))) << "D: MCPP_COMPILE_COMMANDS=off published a copy: " << s::text(r);
    };
}};


// The cases that hold the machine (P7.1): a suite of their own, which the runner runs with no
// other test beside it. Each states that a build is replayed, and a build is planned again when
// the runtime environment description changed: another program installing into the shared home
// beside it rewrote that description (measured on Linux CI, 2026-11-14).
E2E root_machine{"root-machine", resource{"machine"}, [] {
    "781 deleted root cdb is restored"_test = [] {
        // A root database removed by hand is restored by the next build, without a full plan,
        // equal to the configuration's own; a second removal likewise.
        auto p = s::project::scaffold("app");
        auto r = p.mcpp({"build"}, at_root);
        expect(fatal(e2e::succeeded(r))) << "the first build failed: " << s::text(r);
        expect(fatal(p.exists("compile_commands.json") && !p.read("compile_commands.json").empty())) << "no compile_commands.json after the first build";
        std::optional<fs::path> config;
        for (const auto& f : s::find_files(p.at("target"), R"(compile_commands\.json)"))
            if (!f.generic_string().contains("/.build-mcpp/")) { config = f; break; }
        expect(fatal(config.has_value())) << "no configuration database under target/";
        const auto before = s::read_file(*config);
        p.remove("compile_commands.json");
        // `touch .before-build2; sleep 1; find -newer`: the clock before a second's wait.
        const auto marker = fs::file_time_type::clock::now();
        std::this_thread::sleep_for(std::chrono::seconds{1});
        r = p.mcpp({"build"}, at_root);
        expect(fatal(e2e::succeeded(r))) << "the build after deleting the root database failed: " << s::text(r);
        expect(fatal(p.exists("compile_commands.json"))) << "compile_commands.json was not restored: " << s::text(r);
        if (s::on_linux())
            for (const auto& g : s::find_files(p.at("target"), R"(build\.ninja)"))
                expect(fatal(g.generic_string().contains("/.build-mcpp/") || fs::last_write_time(g) <= marker)) << "restoring the root database ran a full plan: " << s::text(r);
        expect(fatal(p.read("compile_commands.json") == before)) << "the restored root file differs from the configuration's database";
        p.remove("compile_commands.json");
        r = p.mcpp({"build"}, at_root);
        expect(fatal(e2e::succeeded(r))) << "the third build failed: " << s::text(r);
        expect(fatal(p.exists("compile_commands.json"))) << "the second delete was not restored";
    };
}};

}  // namespace
