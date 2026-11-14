// Tests built and not run, an archive of them run elsewhere, and the run mcpp keeps (formerly
// tests/e2e/745, 989, 1026 and 1050).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

std::string trim(std::string v) {
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
    return v;
}

// Every line of a file that is a JSON object.
std::vector<nlohmann::json> records(const fs::path& file) { return s::json_lines(s::read_file(file)); }

E2E archive{"archive", [] {
    "745 no run builds the tests and says so"_test[s::gcc] = [] {
        // `--no-run` builds the tests and reports that, distinguishably from a run that could not
        // run them. The runner is a name that is not a program: tests built, nothing run, on every
        // host, for the native target. The package's only sources are its tests.
        s::project p{{"mcpp.toml", R"([package]
name    = "norun"
version = "0.1.0"

# A name, not a path, and not a program: the runner lookup fails, which is the
# situation this test is about.
[target.HOST_TRIPLE]
runner = ["mcpp-no-such-runner-exists"]
)"},
                     {"tests/alpha.cpp", "int main() { return 0; }\n"},
                     {"tests/beta.cpp", "int main() { return 0; }\n"}};
        auto host = trim(p.mcpp({"--print-target"}).out);
        if (host.empty()) {
            // `--print-target` postdates some clients: the triple of a build directory.
            p.mcpp({"build"});
            for (const auto& e : fs::directory_iterator(p.at("target")))
                if (!e.path().filename().string().starts_with(".")) { host = e.path().filename().string(); break; }
        }
        expect(fatal(!host.empty())) << "could not determine the host triple";
        p.write("mcpp.toml", std::regex_replace(p.read("mcpp.toml"), std::regex("HOST_TRIPLE"), host));

        // A: it tried to run them and could not.
        auto a = p.mcpp({"test", "--target", host});
        expect(e2e::exited(a, 2)) << "a runner that does not exist";
        expect(e2e::contains(s::text(a), "not run"));
        // B: it was asked not to.
        auto b = p.mcpp({"test", "--target", host, "--no-run"});
        expect(e2e::exited(b, 0));
        expect(e2e::contains(s::text(b), "2 built, not run"));
        expect(s::lacks(s::text(b), "2 not run")) << "B reported the tests as not run as well as built";
        // C: --no-run does not make a broken test pass; it names it and still reports the others.
        p.write("tests/gamma.cpp", "int main() { this_function_does_not_exist(); }\n");
        auto c = p.mcpp({"test", "--target", host, "--no-run"});
        expect(c.started && c.status != 0) << "a test that does not compile was reported as built";
        expect(e2e::contains(s::text(c), "gamma"));
        expect(e2e::contains(s::text(c), "2 built, not run"));
        // D: --no-run with --no-runner is refused, and says why.
        auto d = p.mcpp({"test", "--target", host, "--no-run", "--no-runner"});
        expect(d.started && d.status != 0) << "--no-run with --no-runner was accepted";
        expect(e2e::contains(s::text(d), "cannot be combined"));
        // E: a workspace's total says it too.
        p.remove("tests/gamma.cpp");
        p.write("mcpp.toml", "[workspace]\nmembers = [\"members/one\", \"members/two\"]\n");
        for (auto m : {"one", "two"}) {
            p.write(std::format("members/{}/mcpp.toml", m),
                    std::format("[package]\nname    = \"{}\"\nversion = \"0.1.0\"\n\n[target.{}]\nrunner = [\"mcpp-no-such-runner-exists\"]\n", m, host));
            p.write(std::format("members/{}/tests/t.cpp", m), "int main() { return 0; }\n");
        }
        auto e = p.mcpp({"test", "--workspace", "--target", host, "--no-run"});
        expect(e2e::exited(e, 0));
        expect(e2e::contains(s::text(e), "2 built, not run")) << "the workspace total did not report the built tests";
    };

    "989 a native legs tests run from an archive"_test = [] {
        // A leg of the host's own system, archived, runs from the archive in a checkout with
        // nothing built (macOS once refused such a program: "invalid argument").
        const auto archive = s::case_dir() / "t.tar.gz";
        s::project app(s::case_dir() / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n"},
                                            {"src/main.cpp", "import std;\nint main() { std::println(\"app\"); }\n"},
                                            {"tests/smoke.cpp", "import std;\nint main() { std::vector<int> v{2, 1}; std::ranges::sort(v); return v.front() == 1 ? 0 : 1; }\n"}});
        // A
        expect(fatal(e2e::succeeded(app.mcpp({"test", "--no-run", "--archive", archive.string()}))));
        expect(fatal(fs::is_regular_file(archive) && fs::file_size(archive) > 0)) << "no archive written";
        // B: a checkout without target/.
        s::project co(s::case_dir() / "co", {});
        for (auto f : {"src", "tests", "mcpp.toml"})
            fs::copy(app.at(f), co.at(f), fs::copy_options::recursive);
        auto b = co.mcpp({"test", "--archive", archive.string()});
        expect(e2e::succeeded(b)) << "the host leg's archived test did not pass";
        expect(e2e::contains(s::text(b), "smoke ... ok"));
        // Nothing named smoke* outside the unpacked archive: the tests were not built again.
        std::vector<fs::path> rebuilt;
        if (fs::exists(co.at("target")))
            for (auto it = fs::recursive_directory_iterator(co.at("target")); it != fs::recursive_directory_iterator(); ++it) {
                if (it->path().filename() == ".test-archive") { it.disable_recursion_pending(); continue; }
                if (it->path().filename().string().starts_with("smoke")) rebuilt.push_back(it->path());
            }
        expect(rebuilt.empty()) << "the archive's tests were built again: " << (rebuilt.empty() ? "" : rebuilt.front().string());
    };

    "1086 the package's programs built elsewhere are named by the environment"_test = [] {
        // `--no-programs`. A `--no-run --archive`: the tests are archived, the program is neither
        // built nor carried, and the manifest names it for the run; B a run of the archive whose
        // environment does not name it is refused; C one that names it runs the tests with that
        // program; D a run here takes it from the environment as well, and refuses without it; E an
        // archive's run builds nothing, and is refused the option.
        const auto archive = s::case_dir() / "np.tar.gz";
        s::project app(s::case_dir() / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n"},
                                            {"src/main.cpp", "int main() { return 0; }\n"},
                                            {"tests/named.cpp", "import std;\nint main() {\n"
                                             "    const char* p = std::getenv(\"MCPP_BIN_APP\");\n"
                                             "    return p && std::filesystem::path(p).filename() == \"named-app\" ? 0 : 1;\n}\n"}});
        // A
        auto a = app.mcpp({"test", "--no-run", "--no-programs", "--archive", archive.string()});
        expect(fatal(e2e::succeeded(a))) << "A: the archive was not written\n" << s::text(a);
        std::vector<fs::path> built;
        for (auto it = fs::recursive_directory_iterator(app.at("target")); it != fs::recursive_directory_iterator(); ++it)
            if (it->is_regular_file() && it->path().stem() == "app" && it->path().parent_path().filename() == "bin")
                built.push_back(it->path());
        expect(fatal(built.empty())) << "A: the program was built: " << (built.empty() ? "" : built.front().string());
        const auto x = s::case_dir() / "x";
        s::tar_extract(archive, x);
        nlohmann::json manifest;
        for (auto it = fs::recursive_directory_iterator(x); it != fs::recursive_directory_iterator(); ++it)
            if (it->path().filename() == "mcpp-tests.json") manifest = s::json_file(it->path());
        expect(fatal(manifest.is_object() && manifest["legs"].size() == 1)) << "A: " << manifest.dump();
        const auto& leg = manifest["legs"][0];
        expect(fatal(leg["programs"].empty() && leg["named_programs"] == nlohmann::json{"MCPP_BIN_APP"}))
            << "A: the leg does not name the program for the run: " << leg.dump();
        // B
        auto b = app.mcpp({"test", "--archive", archive.string()}, {.env = {{"MCPP_BIN_APP", std::nullopt}}});
        expect(fatal(e2e::exited(b, 2))) << "B: a run with the program unnamed\n" << s::text(b);
        expect(e2e::contains(b.err, "MCPP_BIN_APP")) << "B: the refusal does not name the variable\n" << b.err;
        // C
        const auto named = s::case_dir() / "named-app";
        std::ofstream{named}.put('\n');
        auto c = app.mcpp({"test", "--archive", archive.string()}, {.env = {{"MCPP_BIN_APP", named.string()}}});
        expect(e2e::succeeded(c)) << "C: the test did not find the named program\n" << s::text(c);
        expect(e2e::contains(s::text(c), "named ... ok")) << "C\n" << s::text(c);
        // D
        auto d = app.mcpp({"test", "--no-programs"}, {.env = {{"MCPP_BIN_APP", named.string()}}});
        expect(e2e::succeeded(d)) << "D: a run here with the program named\n" << s::text(d);
        expect(e2e::contains(s::text(d), "named ... ok")) << "D\n" << s::text(d);
        auto d2 = app.mcpp({"test", "--no-programs"}, {.env = {{"MCPP_BIN_APP", std::nullopt}}});
        expect(e2e::exited(d2, 2) && e2e::contains(d2.err, "MCPP_BIN_APP")) << "D: a run here with the program unnamed\n" << s::text(d2);
        // E
        auto e = app.mcpp({"test", "--archive", archive.string(), "--no-programs"});
        expect(e2e::exited(e, 2)) << "E: --no-programs on an archive's run\n" << s::text(e);
    };

    "1026 an archived member test carries its members files"_test = [] {
        // A member's archived test carries the files its `[runtime] deploy` names, from the member's
        // own product directory, and finds them beside it in a checkout without target/.
        const auto archive = s::case_dir() / "a.tar.gz";
        s::project ws(s::case_dir() / "ws", {{"mcpp.toml", "[workspace]\nmembers = [\"app\", \"tool\"]\n"},
                                          {"app/mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n"},
                                          {"app/src/main.cpp", "int main() { return 0; }\n"},
                                          {"tool/mcpp.toml", R"([package]
name    = "tool"
version = "0.1.0"

[runtime]
deploy = [ { from = "notices/LICENSE", to = "licenses/tool" } ]
)"},
                                          {"tool/notices/LICENSE", "tool license\n"},
                                          {"tool/src/main.cpp", "int main() { return 0; }\n"},
                                          {"tool/tests/notice.cpp", R"(import std;
int main(int, char** argv) {
    auto here = std::filesystem::absolute(argv[0]).parent_path();
    std::ifstream in(here / "licenses" / "tool" / "LICENSE");
    std::string line;
    std::getline(in, line);
    return line == "tool license" ? 0 : 1;
}
)"}});
        // A
        expect(fatal(e2e::succeeded(ws.mcpp({"test", "-p", "tool", "--no-run", "--archive", archive.string()}))));
        const auto x = s::case_dir() / "x";
        s::tar_extract(archive, x);
        const auto manifests = s::glob(x, "*/mcpp-tests.json");
        expect(fatal(!manifests.empty())) << "no mcpp-tests.json in the archive";
        const auto d = s::json_file(manifests.front());
        nlohmann::json test;
        for (const auto& leg : d["legs"])
            for (const auto& t : leg["tests"])
                if (t["name"] == "notice") test = t;
        expect(fatal(!test.is_null())) << d.dump();
        std::string file;
        for (const auto& f : test["files"])
            if (f.get<std::string>().ends_with("licenses/tool/LICENSE")) { file = f; break; }
        expect(fatal(!file.empty())) << "the archive does not carry the member's file: " << test.dump();
        expect(eq(s::read_file(manifests.front().parent_path() / file), std::string("tool license\n")));
        // B
        s::project co(s::case_dir() / "co", {});
        for (auto f : {"mcpp.toml", "app", "tool"})
            fs::copy(ws.at(f), co.at(f), fs::copy_options::recursive);
        for (auto t : {"target", "app/target", "tool/target"}) co.remove(t);
        auto b = co.mcpp({"test", "-p", "tool", "--archive", archive.string()});
        expect(e2e::succeeded(b));
        expect(e2e::contains(s::text(b), "notice ... ok")) << "the archived test did not find its file";
    };

    "1050 a test run is kept in the mtest store"_test[s::gcc] = [] {
        // target/mtest/runs/<id>/ (run.json, programs.ndjson, cases.ndjson) and a line in
        // target/mtest/index.ndjson per run; `[test] store` names another directory or "none".
        auto p = s::project::scaffold("prog");
        expect(fatal(e2e::succeeded(p.mcpp({"test"}))));
        const auto store = p.at("target/mtest");
        expect(fatal(fs::is_regular_file(store / "index.ndjson"))) << "no index";
        const auto runs = records(store / "index.ndjson");
        expect(fatal(eq(runs.size(), 1u)));
        const auto& run = runs.front();
        expect(run["schema"] == "mtest.run/1" && run["status"] == "pass") << run.dump();
        expect(run["counts"].value("pass", 0) >= 1) << run.dump();
        const auto dir = store / "runs" / run["id"].get<std::string>();
        expect(s::json_file(dir / "run.json")["id"] == run["id"]);
        const auto programs = records(dir / "programs.ndjson");
        expect(!programs.empty());
        for (const auto& pr : programs)
            expect(pr["status"] == "pass" && pr["ms"] >= 0 && pr["started_ms"] >= 0 && pr["worker"] >= 0) << pr.dump();
        const auto cases = records(dir / "cases.ndjson");
        expect(!cases.empty());
        for (const auto& c : cases)
            expect(c["status"] == "pass" && !c["program"].is_null() && c["program"] != "") << c.dump();

        auto lines = [&] { return s::count_lines(s::read_file(store / "index.ndjson"), "{"); };
        expect(fatal(e2e::succeeded(p.mcpp({"test"}))));
        expect(eq(lines(), 2u)) << "a second run is not a second record";
        const auto archive = s::case_dir() / "tests.tar.gz";
        expect(fatal(e2e::succeeded(p.mcpp({"test", "--no-run", "--archive", archive.string()}))));
        auto a2 = p.mcpp({"test", "--archive", archive.string()});
        expect(fatal(e2e::succeeded(a2)));
        expect(s::has_line(s::text(a2), R"(test_smoke \.\.\. ok \(.*1 case\))")) << "an archived protocol program does not report its case";
        expect(eq(lines(), 3u)) << "the archive's run is not stored";
        p.write("mcpp.toml", p.read("mcpp.toml") + "\n[test]\nstore = \"none\"\n");
        expect(fatal(e2e::succeeded(p.mcpp({"test"}))));
        expect(eq(lines(), 3u)) << "store = none kept a run";
        p.write("mcpp.toml", std::regex_replace(p.read("mcpp.toml"), std::regex("store = \"none\""), "store = \"out/test-runs\""));
        expect(fatal(e2e::succeeded(p.mcpp({"test"}))));
        expect(p.exists("out/test-runs/index.ndjson")) << "store = <dir> did not write there";
    };
}};

}  // namespace
