// `mcpp test <name> --print-launch`: one test, built and described as `mcpp test` would start it
// (formerly tests/e2e/993).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr std::string_view math_cpp = R"(import std;
int main(int argc, char** argv) {
    std::ofstream("started-math") << argc;
    std::println("math ran with {} argument(s), last [{}]", argc - 1, argc > 1 ? argv[argc - 1] : "");
    return 0;
}
)";
constexpr std::string_view math2_cpp = "import std;\nint main() { std::ofstream(\"started-math2\") << 1; return 0; }\n";

bool same_path(const std::string& a, const std::string& b) {
    return fs::path(a).lexically_normal() == fs::path(b).lexically_normal();
}

// Runs what an envelope's `data` describes: its argv, in its cwd, with its env.
e2e::outcome run_described(const json& data) {
    e2e::command c;
    for (const auto& a : data["argv"]) c.argv.push_back(a);
    c.cwd = data["cwd"].get<std::string>();
    if (data.contains("env") && data["env"].is_object())
        for (auto it = data["env"].begin(); it != data["env"].end(); ++it)
            c.env.emplace_back(it.key(), it.value().get<std::string>());
    return e2e::run(std::move(c));
}

s::project app_with_tests() {
    auto p = s::project::scaffold("app");
    p.write("tests/unit/math.cpp", math_cpp);
    p.write("tests/unit/math2.cpp", math2_cpp);
    return p;
}

E2E launch{"launch", [] {
    "993 a test describes its launch"_test = [] {
        auto p = app_with_tests();
        // A: the envelope names the test, its program, env and cwd, ends its argv with the arguments
        // after `--`, and the test is not started.
        auto a = p.mcpp({"test", "unit/math", "--print-launch", "--", "--flag", "x"});
        expect(fatal(e2e::succeeded(a)));
        expect(!p.exists("started-math") && !p.exists("target/started-math")) << "the test was started";
        const auto d = s::json(a.out);
        expect(fatal(d.contains("kind") && d["kind"] == "mcpp.launch")) << a.out;
        const auto& data = d["data"];
        expect(data["test"] == "unit/math") << data.dump();
        expect(fs::is_regular_file(data["program"].get<std::string>())) << data.dump();
        const auto& argv = data["argv"];
        expect(std::ranges::any_of(argv, [&](const json& x) { return same_path(x.get<std::string>(), data["program"].get<std::string>()); })) << data.dump();
        expect(argv.size() >= 2 && argv[argv.size() - 2] == "--flag" && argv.back() == "x") << argv.dump();
        expect(data.contains("env") && data.contains("cwd"));
        const auto last = s::json_file(p.at("target/.last-result.json"))["data"];
        const auto recorded = s::json_file(last["result"].get<std::string>())["data"]["configuration"];
        for (auto key : {"triple", "profile", "platform"})
            expect(data["configuration"][key] == recorded[key]) << key << ": " << data.dump();
        expect(data["configuration"]["runners"].is_array()) << data.dump();
        // B: what the envelope describes runs.
        auto b = run_described(data);
        expect(e2e::succeeded(b)) << "what the envelope describes did not run";
        expect(e2e::contains(b.out, "math ran with 2 argument(s), last [x]"));
        // C: `unit/math` is contained in `unit/math2` too; the exact name is chosen.
        expect(e2e::contains(a.out, R"("unit/math")"));
        // D: the refusals, exit 2.
        auto d1 = p.mcpp({"test", "--print-launch"});
        expect(e2e::exited(d1, 2));
        expect(e2e::contains(d1.err, "names no test"));
        auto d2 = p.mcpp({"test", "unit/", "--print-launch"});
        expect(e2e::exited(d2, 2));
        expect(e2e::contains(d2.err, "describes one test"));
        auto d3 = p.mcpp({"test", "unit/math", "--print-launch", "--message-format", "json"});
        expect(e2e::exited(d3, 2));
        expect(e2e::contains(d3.err, "message-format json"));
        auto d4 = p.mcpp({"test", "unit/math", "--print-launch", "--no-run"});
        expect(e2e::exited(d4, 2));
        expect(e2e::contains(d4.err, "--no-run"));
        // E
        auto e = p.mcpp({"--protocol-version"});
        expect(e2e::succeeded(e));
        expect(e2e::contains(s::text(e), R"("test-launch")"));
    };

    "993 a test describes its launch (a named runner)"_test[s::python3] = [] {
        // A named runner is the same transformation for a test and a run. The runner is a Python
        // program, a real host runner whose generated argv can be executed.
        auto p = app_with_tests();
        const auto python = s::on_path("python3")->generic_string();
        p.write("runner.py",
                "import json,subprocess,sys\nwith open(\"named-invocations.txt\",\"a\") as f: f.write(json.dumps(sys.argv[1:])+\"\\n\")\n"
                "print(\"NAMED RUNNER\", flush=True)\nsys.exit(subprocess.call(sys.argv[1:]))\n");
        const auto os_name = s::on_linux() ? "linux" : s::on_macos() ? "macos" : "windows";
        p.write("mcpp.toml", p.read("mcpp.toml") +
                                 std::format("\n[target.'cfg(os = \"{}\")'.runners]\ndebuggee = {}\n", os_name,
                                             json::array({python, p.at("runner.py").generic_string(), "{}"}).dump()));
        auto named = p.mcpp({"test", "unit/math", "--runner", "debuggee", "--print-launch", "--", "with spaces"});
        expect(fatal(e2e::succeeded(named))) << "named test launch";
        const auto d = s::json(named.out)["data"];
        expect(d["via"] == "runner") << d.dump();
        expect(d["configuration"]["runners"] == json::array({"debuggee"})) << d.dump();
        expect(d["argv"].back() == "with spaces") << d.dump();
        expect(d["argv"][1].get<std::string>().ends_with("runner.py")) << d.dump();
        auto run = run_described(d);
        expect(e2e::succeeded(run)) << "named launch execution";
        expect(e2e::contains(run.out, "NAMED RUNNER")) << "the named runner did not run";
        expect(e2e::contains(run.out, "last [with spaces]")) << "the named runner changed the arguments";
        p.remove("named-invocations.txt");

        auto invocations = [&] {
            std::vector<json> rows;
            for (const auto& l : s::lines_with(p.read("named-invocations.txt"), "["))
                rows.push_back(json::parse(l));
            return rows;
        };
        auto one_call = [&](std::string_view last) {
            const auto rows = invocations();
            return rows.size() == 1 && rows[0][0].get<std::string>().contains("math2") && rows[0].back().is_string() && rows[0].back().get<std::string>() == last;
        };
        auto direct = p.mcpp({"test", "unit/math2", "--runner", "debuggee", "--", "direct"});
        expect(e2e::succeeded(direct)) << "named runner direct test";
        expect(one_call("direct")) << "a direct test skipped the named runner: " << p.read("named-invocations.txt");
        expect(fatal(e2e::succeeded(p.mcpp({"test", "unit/math2", "--no-run", "--archive", "named-tests.tar.gz"}))));
        p.remove("named-invocations.txt");
        auto archived = p.mcpp({"test", "--archive", "named-tests.tar.gz", "--runner", "debuggee", "--", "archived"});
        expect(e2e::succeeded(archived)) << "named archive runner";
        expect(one_call("archived")) << "the archive skipped the named runner: " << p.read("named-invocations.txt");

        auto missing = p.mcpp({"test", "unit/math", "--runner", "absent", "--print-launch"});
        expect(e2e::exited(missing, 2));
        expect(e2e::contains(missing.err, "no runner named 'absent'"));
        auto conflict = p.mcpp({"test", "unit/math", "--runner", "debuggee", "--no-runner"});
        expect(e2e::exited(conflict, 2));
        expect(e2e::contains(conflict.err, "--runner and --no-runner"));
    };
}};

}  // namespace
