// `build --configure-only`: the database of a project that does not compile, its tests with their
// dev-dependencies and flags, no build products, a later build that does not replay it, a
// workspace's database, and a publication that fails (formerly tests/e2e/211).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

const s::options at_root{.env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

std::string normal(std::string p) {
    std::ranges::replace(p, '\\', '/');
    while (!p.empty() && p.back() == '/') p.pop_back();
    return p;
}

std::string folded(std::string p) {
    for (auto& c : p) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return p;
}

E2E configure{"configure", [] {
    "211 configure only cdb"_test = [] {
        const auto tmp = s::case_dir();
        s::project(tmp / "devkit", {{"mcpp.toml", "[package]\nname = \"devkit\"\nversion = \"0.1.0\"\n\n[build]\ninclude_dirs = [\"include\"]\n"},
                                    {"include/devkit.hpp", "#define DEVKIT_MARKER 1\n"}});
        s::project app(tmp / "app", {
            {"mcpp.toml", "[package]\nname = \"configure-only\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n\n"
                          "[build]\nflags = [{ glob = \"tests/**/*.cpp\", cxxflags = [\"-DMCPP_CONFIGURE_ONLY_TEST_FLAG=1\"] }]\n\n"
                          "[dev-dependencies]\ndevkit = { path = \"../devkit\" }\n"},
            {"src/main.cpp", "int main( { return 0; }\n"},
            {"tests/test_smoke.cpp", "#include <devkit.hpp>\nint test_entry() { return 0; }\n"},
        });
        // A source that does not compile is described, with its test, the test's include
        // directory from a dev-dependency and its [build].flags, and nothing is built.
        auto r = app.mcpp({"build", "--configure-only"}, at_root);
        expect(fatal(e2e::succeeded(r))) << "configure-only failed unexpectedly: " << s::text(r);
        expect(fatal(app.exists("compile_commands.json"))) << "configure-only did not publish compile_commands.json: " << s::text(r);
        const auto text = app.read("compile_commands.json");
        expect(fatal(s::has_line(text, R"(src[\\/][\\/]*main\.cpp)"))) << "CDB is missing the ordinary source TU";
        expect(fatal(s::has_line(text, R"(tests[\\/][\\/]*test_smoke\.cpp)"))) << "CDB is missing the test TU";
        nlohmann::json test, main;
        for (const auto& e : s::json(text)) {
            const auto f = normal(e["file"].get<std::string>());
            if (test.is_null() && f.ends_with("/tests/test_smoke.cpp")) test = e;
            if (main.is_null() && f.ends_with("/src/main.cpp")) main = e;
        }
        expect(fatal(!test.is_null() && !main.is_null())) << "no test or main entry";
        const auto project = normal(test["file"].get<std::string>()).substr(0, normal(test["file"].get<std::string>()).size() - std::string_view("/tests/test_smoke.cpp").size());
        // By the fixture directory's own name: Windows may spell the temporary directory in
        // its short form in one field and its long form in another.
        const auto parent = fs::path(project).parent_path().filename().string();
        const auto tail = folded("/" + parent + "/devkit/include");
        bool include = false, flag = false, main_flag = false;
        for (const auto& a : test["arguments"]) {
            const auto w = a.get<std::string>();
            include = include || (w.size() > 2 && folded(w.substr(0, 2)) == "-i" && folded(normal(w.substr(2))).ends_with(tail));
            flag = flag || w.contains("MCPP_CONFIGURE_ONLY_TEST_FLAG=1");
        }
        for (const auto& a : main["arguments"]) main_flag = main_flag || a.get<std::string>().contains("MCPP_CONFIGURE_ONLY_TEST_FLAG=1");
        expect(fatal(include)) << "the test lacks " << tail << ": " << test["arguments"].dump();
        expect(fatal(flag)) << "the test lacks its [build].flags: " << test["arguments"].dump();
        expect(fatal(!main_flag)) << "the test's flag reached main: " << main["arguments"].dump();
        expect(fatal(s::find_files(app.at("target"), R"(.*\.(o|obj))").empty())) << "configure-only produced an object file";
        expect(fatal(!fs::is_directory(app.at("target/bin")))) << "configure-only produced target/bin";
        expect(fatal(!app.exists("target/.build_cache"))) << "configure-only wrote target/.build_cache";

        // A build after --configure-only does not replay the configure graph.
        s::project fast(tmp / "fastpath", {{"mcpp.toml", "[package]\nname = \"fastpath\"\nversion = \"0.1.0\"\n"},
                                           {"src/main.cpp", "int main() { return 0; }\n"},
                                           {"tests/smoke.cpp", "int main() { return 0; }\n"}});
        r = fast.mcpp({"build"}, at_root);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        std::optional<fs::path> target_bin;
        for (const auto& f : s::find_files(fast.at("target"), "fastpath(\\.exe)?"))
            if (f.parent_path().generic_string().contains("/bin")) target_bin = f;
        expect(fatal(target_bin.has_value())) << "baseline build produced no binary";
        const auto test_bin = target_bin->parent_path() / (std::string("smoke") + (target_bin->extension() == ".exe" ? ".exe" : ""));
        r = fast.mcpp({"build", "--configure-only"}, at_root);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(!fs::exists(test_bin))) << "configure-only linked a test binary";
        r = fast.mcpp({"build"}, at_root);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(!fs::exists(test_bin))) << "build after configure-only replayed the configure graph and linked the tests: " << s::text(r);
        expect(fatal(fs::is_regular_file(*target_bin))) << "build after configure-only lost the package target";

        // A workspace's database at its root, every member's sources and tests; -p a alone.
        s::project ws(tmp / "ws", {{"mcpp.toml", "[workspace]\nmembers = [\"a\", \"b\"]\n"}});
        for (std::string m : {"a", "b"}) {
            ws.write(m + "/mcpp.toml", std::format("[package]\nname = \"{}\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n", m));
            ws.write(m + "/src/main.cpp", "int main() { return 0; }\n");
            ws.write(m + "/tests/main.cpp", "int test_entry() { return 0; }\n");
        }
        r = ws.mcpp({"build", "--configure-only"}, at_root);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(ws.exists("compile_commands.json") && !ws.read("compile_commands.json").empty())) << "missing compile_commands.json";
        for (std::string m : {"a", "b"}) {
            expect(fatal(!ws.exists(m + "/compile_commands.json"))) << m << "/compile_commands.json written beside the workspace's database";
            expect(fatal(s::has_line(ws.read("compile_commands.json"), m + R"([\\/][\\/]*src[\\/][\\/]*main\.cpp)"))) << m << "'s source is not described";
            expect(fatal(s::has_line(ws.read("compile_commands.json"), m + R"([\\/][\\/]*tests[\\/][\\/]*main\.cpp)"))) << m << "'s test is not described";
        }
        fs::remove_all(ws.at("target"));
        ws.remove("compile_commands.json");
        r = ws.mcpp({"build", "--configure-only", "-p", "a"}, at_root);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(s::has_line(ws.read("compile_commands.json"), R"(a[\\/][\\/]*src[\\/][\\/]*main\.cpp)"))) << "-p a did not configure a";
        expect(fatal(s::no_line(ws.read("compile_commands.json"), R"(b[\\/][\\/]*src[\\/][\\/]*main\.cpp)"))) << "-p a configured member b";

        // A destination that cannot be written: a build warns and keeps it; --configure-only,
        // whose product it is, fails and keeps it.
        s::project policy(tmp / "publish-policy", {{"mcpp.toml", "[package]\nname = \"publish-policy\"\nversion = \"0.1.0\"\nstandard = \"c++20\"\n"},
                                                    {"src/main.cpp", "int main() { return 0; }\n"},
                                                    {"compile_commands.json/last-known-good", "keep\n"}});
        r = policy.mcpp({"build"}, at_root);
        expect(fatal(e2e::succeeded(r))) << "normal build failed on optional CDB publication: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "compile_commands.json was not updated"))) << "normal build did not warn about CDB publication: " << s::text(r);
        expect(fatal(policy.exists("compile_commands.json/last-known-good"))) << "normal build removed the prior CDB destination";
        r = policy.mcpp({"build", "--configure-only"}, at_root);
        expect(fatal(failed(r))) << "configure-only accepted failed CDB publication: " << s::text(r);
        expect(fatal(policy.exists("compile_commands.json/last-known-good"))) << "configure-only removed the prior CDB destination";
    };
}};

}  // namespace
