// The compile database under the llvm toolchain (design 2026-09-26 §3.2-§3.5): one database per
// configuration, replayed from its own directory, a module interface's language stated before
// `-c`, and the std unit with its emit agreement, its `provides` and the toolchain's build-id
// (formerly tests/e2e/783, 784, 785, 786). The hermetic job of ci-linux-e2e.yml runs this suite
// (`--case toolchains`): no shard has llvm.
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

const s::options at_root{.env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};

// The gcc row these cases name: the home's gcc@16.1.0 toolchain, which mcpp installs as it installs
// any toolchain a command names; no g++ on PATH is run.
constexpr std::string_view gcc_row = "gcc@16.1.0";

std::string llvm_row() { return "llvm@" + s::llvm_version(); }

// The first word of the entries for the test and for main.cpp (783's driver_of).
std::vector<std::string> drivers(const s::project& p) {
    std::vector<std::string> out;
    for (const auto& e : s::json(p.read("compile_commands.json"))) {
        const auto file = e["file"].get<std::string>();
        if (file.contains("test_smoke") || file.contains("main.cpp"))
            out.push_back(e["arguments"][0].get<std::string>());
    }
    return out;
}

bool any_clang(const std::vector<std::string>& words) {
    return std::ranges::any_of(words, [](const std::string& w) { return w.contains("clang"); });
}

// The entry's `arguments` run in its `directory`, its `output` removed first: whether it exited 0
// and wrote that output again (a relative output is read from the project, as the script read it).
bool replays(const s::project& p, const nlohmann::json& e, std::string& why) {
    const auto obj = p.dir() / e["output"].get<std::string>();
    std::error_code ec;
    fs::remove(obj, ec);
    const auto r = e2e::run({.argv = e["arguments"].get<std::vector<std::string>>(),
                             .cwd = e["directory"].get<std::string>(),
                             .timeout = std::chrono::minutes{5}});
    if (r && fs::exists(obj)) return true;
    why = std::format("{} from directory={} (exit {}): {}", e["file"].get<std::string>(),
                      e["directory"].get<std::string>(), r.status, r.err);
    return false;
}

const nlohmann::json& entry_ending(const nlohmann::json& entries, std::string_view suffix) {
    for (const auto& e : entries)
        if (e["file"].get<std::string>().ends_with(suffix)) return e;
    fail(std::format("no entry whose file ends with {}", suffix));
}

E2E toolchains{"toolchains", [] {
    "783 cdb switches whole with the configuration"_test[tag{"requires:llvm"}] = [] {
        // `mcpp test` then `mcpp build` in one configuration keep the test entry; switching the
        // toolchain switches the whole root file; switching back restores that configuration's
        // entries, test units included.
        s::needs({"llvm"});
        auto p = s::project::scaffold("app");
        auto r = p.mcpp({"test", "--toolchain", llvm_row()}, at_root);
        expect(fatal(e2e::succeeded(r))) << "mcpp test (llvm) failed: " << s::text(r);
        expect(fatal(e2e::contains(p.read("compile_commands.json"), "test_smoke"))) << "after 'mcpp test' (llvm), no entry for tests/test_smoke.cpp:\n" << p.read("compile_commands.json");
        expect(fatal(any_clang(drivers(p)))) << "the llvm test entry does not name a clang driver";

        r = p.mcpp({"build", "--toolchain", llvm_row()}, at_root);
        expect(fatal(e2e::succeeded(r))) << "mcpp build (llvm) failed: " << s::text(r);
        expect(fatal(e2e::contains(p.read("compile_commands.json"), "test_smoke"))) << "'mcpp build' in the same (llvm) configuration lost the test entry:\n" << p.read("compile_commands.json");

        r = p.mcpp({"build", "--toolchain", std::string(gcc_row)}, at_root);
        expect(fatal(e2e::succeeded(r))) << "mcpp build (gcc) failed: " << s::text(r);
        expect(fatal(s::lacks(p.read("compile_commands.json"), "test_smoke"))) << "llvm's test entries leaked into the gcc configuration";
        expect(fatal(!any_clang(drivers(p)))) << "a clang driver survives under the gcc configuration";
        const auto main_driver = entry_ending(s::json(p.read("compile_commands.json")), "main.cpp")["arguments"][0].get<std::string>();
        expect(fatal(main_driver.contains("g++") || main_driver.contains("gcc"))) << "main.cpp's driver under gcc: " << main_driver;

        r = p.mcpp({"build", "--toolchain", llvm_row()}, at_root);
        expect(fatal(e2e::succeeded(r))) << "mcpp build (llvm again) failed: " << s::text(r);
        expect(fatal(e2e::contains(p.read("compile_commands.json"), "test_smoke"))) << "llvm's test entries did not return when switching back:\n" << p.read("compile_commands.json");
        expect(fatal(any_clang(drivers(p)))) << "the root is not llvm's configuration after switching back";
    };

    "784 cdb replays from its directory"_test[tag{"requires:llvm"}] = [] {
        // `directory` is the output directory the compiler runs in, for every unit of the project
        // and both rows: each entry replays from it, and the project root gains no BMI cache.
        s::needs({"llvm"});
        s::project p({{"mcpp.toml", "[package]\nname = \"replaytest\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n"},
                      {"src/greet.cppm", "export module replaytest.greet;\nexport int answer() { return 42; }\n"},
                      {"src/main.cpp", "import replaytest.greet;\nint main() { return answer() == 42 ? 0 : 1; }\n"}});
        const auto root = fs::weakly_canonical(p.dir());
        for (const auto& tc : {std::string(gcc_row), llvm_row()}) {
            auto r = p.mcpp({"build", "--toolchain", tc, "--no-cache"}, at_root);
            expect(fatal(e2e::succeeded(r))) << "build with " << tc << " failed: " << s::text(r);
            for (const auto& e : s::json(p.read("compile_commands.json"))) {
                // The project's own units only: the standard-library units share one cache across
                // the machine, and their replay is 786's criterion.
                const auto file = fs::weakly_canonical(p.dir() / e["file"].get<std::string>());
                const auto rel = file.lexically_relative(root);
                if (rel.empty() || *rel.begin() == "..") continue;
                std::string why;
                expect(fatal(replays(p, e, why))) << "[" << tc << "] replay failed: " << why;
            }
            expect(fatal(!fs::is_directory(p.at("gcm.cache")) && !fs::is_directory(p.at("pcm.cache")))) << "a BMI cache directory leaked into the project root under " << tc;
        }
    };

    "785 cdb interface flag module extensions"_test[tag{"requires:llvm"}] = [] {
        // An interface of a `.ixx` extension carries `-x c++-module` immediately before `-c` and
        // replays into its object; the non-module unit carries no language flag.
        s::needs({"llvm"});
        s::project p({{"mcpp.toml", "[package]\nname = \"ixxtest\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n\n[build]\nmodule_extensions = [\".ixx\"]\n"},
                      {"src/greet.ixx", "export module ixxtest.greet;\nexport int answer() { return 42; }\n"},
                      {"src/main.cpp", "import ixxtest.greet;\nint main() { return answer() == 42 ? 0 : 1; }\n"}});
        auto r = p.mcpp({"build", "--toolchain", llvm_row()}, at_root);
        expect(fatal(e2e::succeeded(r))) << "build failed: " << s::text(r);
        const auto entries = s::json(p.read("compile_commands.json"));
        const auto& iface = entry_ending(entries, "greet.ixx");
        const auto args = iface["arguments"].get<std::vector<std::string>>();
        const auto c = std::ranges::find(args, "-c");
        expect(fatal(c != args.end())) << "no -c: " << iface["arguments"].dump();
        const auto c_at = static_cast<std::size_t>(c - args.begin());
        expect(fatal(c_at >= 2 && args[c_at - 2] == "-x" && args[c_at - 1] == "c++-module")) << "-x c++-module is not immediately before -c: " << iface["arguments"].dump();
        std::string why;
        expect(fatal(replays(p, iface, why))) << "the interface entry did not replay into its object: " << why;
        const auto main_args = entry_ending(entries, "main.cpp")["arguments"].get<std::vector<std::string>>();
        expect(fatal(!std::ranges::contains(main_args, std::string("-x")))) << "the non-module unit carries a language flag: " << entry_ending(entries, "main.cpp")["arguments"].dump();
    };

    "786 std unit in the database and build id"_test[tag{"requires:llvm"}] = [] {
        // The std unit's directory is the shared std cache; `emit --spec compile-commands` renders
        // the same std entry as the build's database; provides['std'] names the std-cache BMI;
        // the toolchain's build-id is present and stable across two runs.
        s::needs({"llvm"});
        s::project p({{"mcpp.toml", "[package]\nname = \"stdunit\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n"},
                      {"src/main.cpp", "import std;\nint main() { std::println(\"hi\"); return 0; }\n"}});
        auto r = p.mcpp({"build", "--toolchain", llvm_row()}, at_root);
        expect(fatal(e2e::succeeded(r))) << "build failed: " << s::text(r);
        const auto built = s::json(p.read("compile_commands.json"));
        std::vector<nlohmann::json> std_units;
        for (const auto& e : built)
            if (fs::path(e["file"].get<std::string>()).filename() == "std.cppm") std_units.push_back(e);
        expect(fatal(std_units.size() == 1u)) << "std.cppm entries: " << std_units.size() << " in " << built.dump();
        const auto dir = std_units[0]["directory"].get<std::string>();
        expect(fatal(!dir.starts_with((p.dir() / "target").string()))) << "the std unit's directory is the project's: " << dir;
        expect(fatal(fs::is_directory(dir))) << "the std unit's directory is not a directory: " << dir;

        r = p.mcpp({"emit", "build-database", "--toolchain", llvm_row(), "--spec", "compile-commands"}, at_root);
        expect(fatal(e2e::succeeded(r))) << "emit failed: " << r.err;
        const auto emitted = s::json(r.out);
        const auto& b = entry_ending(built, "std.cppm");
        const auto std_file = b["file"].get<std::string>();
        const nlohmann::json* e = nullptr;
        for (const auto& x : emitted)
            if (x["file"] == std_file) e = &x;
        expect(fatal(e != nullptr)) << "emit renders no entry for " << std_file;
        for (std::string_view key : {"arguments", "directory", "output"})
            expect(fatal(b[key] == (*e)[key])) << key << ": " << b[key].dump() << " vs " << (*e)[key].dump();

        std::array<nlohmann::json, 2> d;
        for (std::size_t i = 0; i < d.size(); ++i) {
            r = p.mcpp({"emit", "build-database", "--toolchain", llvm_row(), "--format", "json"}, at_root);
            expect(fatal(e2e::succeeded(r))) << "emit (s1, run " << i + 1 << ") failed: " << r.err;
            d[i] = s::json(r.out)["data"]["database"];
        }
        const auto std_provides = [](const nlohmann::json& db) {
            for (const auto& set : db["sets"]) {
                if (set["name"] != "mcpp:std") continue;
                for (const auto& u : set["translation-units"])
                    if (u["provides"].contains("std")) return u["provides"]["std"].get<std::string>();
            }
            fail("no mcpp:std unit providing 'std'");
        };
        const auto path1 = std_provides(d[0]);
        expect(fatal(!path1.empty())) << "provides['std'] is empty; D5b names the BMI path";
        expect(fatal(path1.ends_with(".pcm") || path1.ends_with(".gcm") || path1.ends_with(".ifc"))) << "provides['std']: " << path1;
        expect(fatal(path1 == std_provides(d[1]))) << path1 << " vs " << std_provides(d[1]);
        const auto build_id = [](const nlohmann::json& db) {
            const auto& tc = db["ide"]["toolchains"].begin().value();
            return tc.contains("build-id") ? tc["build-id"] : nlohmann::json{};
        };
        const auto bid1 = build_id(d[0]);
        expect(fatal(bid1.is_string() && !bid1.get<std::string>().empty())) << "build-id is absent";
        expect(fatal(bid1 == build_id(d[1]))) << bid1.dump() << " vs " << build_id(d[1]).dump();
    };
}};

}  // namespace
