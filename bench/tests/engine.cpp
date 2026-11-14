// THE ENGINE'S OWN COSTS, AS METRICS WITH A HISTORY (design 2026-11-10 §16).
//
// The harness (bench/README.md) compares build engines over hours; these cases
// time the mcpp under test on a small module fixture, so that CI keeps each
// cost in mtest's store and `mtest analyze trend` shows it beside its history.
// The mcpp is named by binary (MCPP), never by PATH (bench/README §4).
import std;
import mcpplibs.mtest;
import mcpplibs.mtest.bench;
import mcpplibs.mtest.e2e;

using namespace mcpplibs::mtest;

namespace {

std::string under_test() {
    const char* m = std::getenv("MCPP");
    return m ? m : "";
}

// The mcpp under test in `dir`, started directly (no shell): its status.
int mcpp(const std::filesystem::path& dir, std::vector<std::string> args) {
    args.insert(args.begin(), under_test());
    return e2e::run({.argv = std::move(args), .cwd = dir, .timeout = std::chrono::minutes{10}}).status;
}

void write(const std::filesystem::path& file, std::string_view text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

// A program over one module interface and `import std`: the smallest shape
// that exercises the module graph, the std module and a link.
std::filesystem::path fixture(const std::filesystem::path& root) {
    write(root / "mcpp.toml", "[package]\nname    = \"timed\"\nversion = \"0.1.0\"\n");
    write(root / "src/greet.cppm",
          "export module timed.greet;\nimport std;\nexport std::string greet() { return \"hi\"; }\n");
    write(root / "src/main.cpp",
          "import std;\nimport timed.greet;\nint main() { std::println(\"{}\", greet()); }\n");
    return root;
}

}  // namespace

// Each command is timed call by call (`bench::each`, mtest 0.8.1+): the median
// is the metric as before, and the p95, the maximum and the two slowest calls
// say whether a cost is steady or has outliers. A call is one command, a tenth
// of a second or more, so the count is the least: six calls, one of them warm-up.
Bench engine{"engine", skip_if{under_test().empty(), "MCPP names no mcpp under test"},
             timeout{std::chrono::minutes{20}}, [] {
    const auto dir = fixture(temp_dir());
    expect(fatal(eq(mcpp(dir, {"build"}), 0))) << "the first build of the fixture";
    const bench::each_options o{.least = 5, .time = std::chrono::nanoseconds{0}, .warmup = 1, .slowest = 2};
    // A call of a command that failed is not a cost of the command.
    "a build with nothing to do"_test = [&] {
        int failed = 0;
        bench::each("noop-build", [&](bench::lap&) { failed += mcpp(dir, {"build"}) != 0; }, o);
        expect(eq(failed, 0)) << "every `mcpp build` succeeded";
    };
    "the plan of a unit"_test = [&] {
        int failed = 0;
        bench::each("why-flags", [&](bench::lap&) { failed += mcpp(dir, {"why", "flags"}) != 0; }, o);
        expect(eq(failed, 0)) << "every `mcpp why flags` succeeded";
    };
    "a rebuild after one edit of an interface"_test = [&] {
        int failed = 0;
        bench::each("one-interface-edit", [&](bench::lap& l) {
            // The edit is not the rebuild's cost.
            l.pause();
            write(dir / "src/greet.cppm",
                  std::format("export module timed.greet;\nimport std;\n"
                              "export std::string greet() {{ return \"hi {}\"; }}\n", l.index()));
            l.resume();
            failed += mcpp(dir, {"build"}) != 0;
        }, o);
        expect(eq(failed, 0)) << "every rebuild succeeded";
    };
}};
