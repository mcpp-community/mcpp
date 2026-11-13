// Test programs that speak mtest's test protocol: their cases, live case records, the spec's
// launch, capabilities, checks and output (formerly tests/e2e/1035, 1065 and 1070).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// The record of test `name` in a stream (the first line naming it).
json record(std::string_view stream, std::string_view name) {
    for (auto& r : s::json_lines(stream))
        if (r.contains("test") && r["test"] == name)
            return r;
    return json{};
}

std::vector<std::string> ids(const json& rec) {
    std::vector<std::string> out;
    if (rec.contains("cases"))
        for (const auto& c : rec["cases"]) out.push_back(c["id"]);
    return out;
}

std::string generic(const fs::path& p) { return p.generic_string(); }

E2E protocol{"protocol", [] {
    "1035 a test program reports its cases"_test[tag{"requires:gcc"}] = [] {
        // mcpp runs the runner's side of the mtest protocol, version 1; every other program is one
        // case judged by its exit status and is told none of the variables.
        s::needs({"gcc"});
        s::project ws{{"mcpp.toml", "[workspace]\nmembers = [\"fw\", \"app\", \"plain\"]\n"},
                      {"fw/mcpp.toml", R"([package]
name    = "fw"
version = "0.1.0"

[targets.main]
kind          = "entry"
main          = "src/main.cpp"
test-protocol = 1
)"},
                      {"fw/src/main.cpp", R"CPP(import std;
struct test_case { std::string id; int line; int (*body)(); };
std::vector<test_case> cases();
int main() {
    const char* mode = std::getenv("MTEST_MODE");
    const char* path = std::getenv("MTEST_REPORT");
    std::ofstream report;
    if (path) report.open(path, std::ios::app);
    auto event = [&](const std::string& body) { if (report) { report << "{" << body << "}\n"; report.flush(); } };
    event(R"("e":"program","protocol":1,"level":1,"framework":"fw","version":"0.1.0")");
    std::set<std::string> selected;
    bool selecting = false;
    if (const char* s = std::getenv("MTEST_SELECT")) {
        selecting = true;
        std::ifstream in(s);
        for (std::string line; std::getline(in, line);) if (!line.empty()) selected.insert(line);
    }
    int failed = 0;
    for (const auto& c : cases()) {
        if (mode && std::string(mode) == "list") {
            event(std::format(R"("e":"case","id":"{}","name":"{}","file":"t.cpp","line":{})", c.id, c.id, c.line));
            continue;
        }
        if (selecting && !selected.contains(c.id)) continue;
        event(std::format(R"("e":"begin","id":"{}","t":0)", c.id));
        const int r = c.body();
        if (r != 0) {
            ++failed;
            event(std::format(R"("e":"failure","id":"{}","file":"t.cpp","line":{},"message":"{} != 0")", c.id, c.line, r));
        }
        event(std::format(R"("e":"end","id":"{}","status":"{}","ms":1)", c.id, r ? "fail" : "pass"));
    }
    event(std::format(R"("e":"done","status":"{}","counts":{{}})", failed ? "fail" : "pass"));
    return failed ? 1 : 0;
}
)CPP"},
                      {"app/mcpp.toml", R"([package]
name    = "app"
version = "0.1.0"

[dev-dependencies]
fw = { path = "../fw" }

[test]
main = { from = "fw/main" }
)"},
                      {"app/src/app.cppm", "export module app;\nexport int answer() { return 42; }\n"},
                      {"app/tests/math.cpp", R"(import std;
import app;
struct test_case { std::string id; int line; int (*body)(); };
std::vector<test_case> cases() {
    return {
        {"math/adds", 4, [] { return answer() == 42 ? 0 : 1; }},
        {"math/fails", 5, [] { return 7; }},
        {"math/crashes", 6, [] { std::exit(3); return 0; }},
    };
}
)"},
                      {"plain/mcpp.toml", "[package]\nname    = \"plain\"\nversion = \"0.1.0\"\n"},
                      {"plain/tests/bare.cpp", R"(import std;
int main() {
    for (const char* v : {"MTEST_PROTOCOL", "MTEST_MODE", "MTEST_REPORT"})
        if (std::getenv(v)) { std::println("{} reached a program that implements no protocol", v); return 3; }
    return 0;
}
)"},
                      {"plain/tests/marked.cpp", R"CPP(import std;
constinit const char marker[] = "mtest-protocol=1";
int main() {
    [[maybe_unused]] volatile char keep = marker[0];
    const char* path = std::getenv("MTEST_REPORT");
    if (!path) return 4;
    std::ofstream report(path, std::ios::app);
    const std::string mode = std::getenv("MTEST_MODE") ? std::getenv("MTEST_MODE") : "run";
    report << R"({"e":"program","protocol":1,"level":1,"framework":"marked","version":"1"})" << "\n";
    if (mode == "list") report << R"({"e":"case","id":"only","name":"only","file":"marked.cpp","line":1})" << "\n";
    else report << R"({"e":"begin","id":"only","t":0})" << "\n" << R"({"e":"end","id":"only","status":"pass","ms":0})" << "\n";
    report << R"({"e":"done","status":"pass","counts":{}})" << "\n";
    return 0;
}
)CPP"}};
        // A
        auto a = ws.mcpp({"test", "-p", "app", "--message-format", "json"});
        expect(a.started && a.status != 0) << "a failing case passed the run";
        auto rec = record(a.out, "math");
        expect(fatal(!rec.is_null())) << "no record of the test\n" << s::text(a);
        expect(rec["protocol"] == 1) << rec.dump();
        std::vector<std::string> results;
        for (const auto& c : rec["cases"]) results.push_back(c["id"].get<std::string>() + ":" + c["status"].get<std::string>());
        expect(eq(results, std::vector<std::string>{"math/adds:pass", "math/fails:fail", "math/crashes:fail"})) << rec.dump();
        expect(rec["cases"][1]["failures"][0]["line"] == 5) << "the failure's place";
        expect(e2e::contains(rec["cases"][2].value("reason", ""), "status 3 before the case ended")) << "a crashed case's reason (P5.4)";
        auto ah = ws.mcpp({"test", "-p", "app"});
        expect(ah.started && ah.status != 0) << "the human run passed";
        expect(s::has_line(s::text(ah), R"(^        math/fails  t\.cpp:5$)")) << "the failures' index does not name the failed case at its place";
        expect(e2e::contains(s::text(ah), "cases: 1 passed, 2 failed, 0 skipped"));
        // B
        auto b = ws.mcpp({"test", "--case", "math/adds", "--message-format", "json"});
        expect(fatal(e2e::succeeded(b)));
        expect(eq(ids(record(b.out, "math")), std::vector<std::string>{"math/adds"}));
        expect(record(b.out, "bare").is_null()) << "a program with no protocol ran under --case";
        // C
        auto c = ws.mcpp({"test", "--cases"});
        expect(fatal(e2e::succeeded(c)));
        expect(e2e::contains(s::text(c), "math: 3 cases"));
        expect(e2e::contains(s::text(c), "math/fails  t.cpp:5"));
        expect(e2e::contains(s::text(c), "bare: one case"));
        // D, E: mcpp's own environment has the protocol's variables.
        auto d = ws.mcpp({"test", "-p", "plain", "--message-format", "json"},
                         {.env = {{"MTEST_PROTOCOL", "1"}, {"MTEST_MODE", "list"}, {"MTEST_REPORT", (temp_dir() / "outer.ndjson").string()}}});
        expect(fatal(e2e::succeeded(d)));
        expect(record(d.out, "bare")["status"] == "pass") << "the protocol's variables reached a program with none (P1.2)";
        expect(record(d.out, "marked")["protocol"] == 1) << "the marker";
        expect(eq(ids(record(d.out, "marked")), std::vector<std::string>{"only"}));
        // F
        auto f = ws.mcpp({"test", "-p", "app", "--no-run", "--message-format", "json"});
        expect(fatal(e2e::succeeded(f)));
        rec = record(f.out, "math");
        expect(rec["status"] == "built");
        expect(e2e::contains(rec.value("program", ""), "math"));
        bool test_src = false, lib_src = false;
        for (const auto& src : rec["sources"]) {
            const auto v = src.get<std::string>();
            test_src = test_src || v.ends_with("app/tests/math.cpp");
            lib_src = lib_src || v.ends_with("app/src/app.cppm");
        }
        expect(test_src && lib_src) << "the sources: " << rec["sources"].dump();
    };

    "1065 test records carry parts live cases and metrics"_test[tag{"requires:gcc"}] = [] {
        s::needs({"gcc"});
        auto p = s::project::scaffold("app");
        p.write("tests/unit/a.cpp", R"(import mcpplibs.mtest;
import mcpplibs.mtest.bench;
using namespace mcpplibs::mtest;
Bench timed{"timed", at_most{"spin", 10.0}, [] {
    bench::measure("spin", [] { int x = 0; for (int i = 0; i < 100; ++i) bench::keep(x += i); },
                   {.samples = 3, .warmup = 0});
}};
)");
        p.write("tests/unit/b.cpp", R"(import mcpplibs.mtest;
using namespace mcpplibs::mtest;
UTest plain{"plain", tag{"quick"}, [] { expect(eq(1 + 1, 2)); }};
)");
        p.write("mcpp.toml", p.read("mcpp.toml") + "\n[test]\nprograms = \"directory\"\n");
        // A: the listing names the parts.
        auto a = p.mcpp({"test", "--cases", "--message-format", "json"});
        expect(fatal(e2e::succeeded(a)));
        const auto unit = record(a.out, "unit");
        expect(fatal(unit.contains("parts"))) << a.out;
        std::vector<std::string> names;
        for (const auto& part : unit["parts"]) {
            names.push_back(part["name"]);
            const auto file = part["file"].get<std::string>();
            expect(file.ends_with("a.cpp") || file.ends_with("b.cpp")) << file;
        }
        std::ranges::sort(names);
        expect(eq(names, std::vector<std::string>{"unit/a", "unit/b"}));
        // B: case records as they happen, before their program's record.
        auto b = p.mcpp({"test", "--message-format", "json"});
        expect(fatal(e2e::succeeded(b)));
        const auto recs = s::json_lines(b.out);
        for (auto part : {"unit/a", "unit/b"}) {
            bool begin = false, end = false, reached = false;
            for (const auto& r : recs) {
                if (r.contains("test") && r["test"] == part) { reached = true; break; }
                if (r.contains("case") && r["case"]["test"] == part) {
                    begin = begin || r["case"]["event"] == "begin";
                    end = end || r["case"]["event"] == "end";
                }
            }
            expect(reached && begin && end) << part << ": no begin and end before its record";
        }
        json end;
        for (const auto& r : recs)
            if (r.contains("case") && r["case"]["event"] == "end" && r["case"]["id"] == "timed") { end = r["case"]; break; }
        expect(fatal(!end.is_null()));
        expect(end["status"] == "pass") << end.dump();
        expect(end["metrics"][0]["name"] == "spin" && end["metrics"][0]["unit"] == "s") << end.dump();
        expect(end["bounds"] == json::parse(R"([{"metric": "spin", "max": 10.0}])")) << end.dump();
        const auto ra = record(b.out, "unit/a");
        expect(ra["part_of"] == "unit") << ra.dump();
        json timed;
        for (const auto& c : ra["cases"]) if (c["id"] == "timed") timed = c;
        expect(timed["kind"] == "bench" && !timed["metrics"].empty() && !timed["bounds"].empty()) << timed.dump();
        json plain;
        for (const auto& c : record(b.out, "unit/b")["cases"]) if (c["id"] == "plain") plain = c;
        expect(plain.value("tags", json::array()) == json::parse(R"(["quick"])")) << plain.dump();
        // C: the kept run.
        const auto runs = s::json_lines(p.read("target/mtest/index.ndjson"));
        expect(fatal(!runs.empty()));
        json kept;
        for (const auto& c : s::json_lines(s::read_file(p.at("target/mtest/runs") / runs.back()["id"].get<std::string>() / "cases.ndjson")))
            if (c["id"] == "timed") kept = c;
        expect(kept["kind"] == "bench" && kept["started_ms"] >= 0) << kept.dump();
        expect(kept["metrics"][0]["name"] == "spin" && kept["bounds"][0]["metric"] == "spin") << kept.dump();
        // D
        auto d = p.mcpp({"self", "env", "--format", "json"});
        expect(fatal(e2e::succeeded(d)));
        for (auto f : {"test-parts", "test-case-stream", "test-metrics"})
            expect(e2e::contains(s::text(d), std::format("\"{}\"", f)));
    };

    "1070 the test spec launches frameworks checks and output"_test[tag{"requires:gcc"}] = [] {
        // The mcpp test spec's B5, B6, O3 and O8 (docs/specs/test.md).
        s::needs({"gcc"});
        s::project t(temp_dir() / "t", {{"fw/mcpp.toml", R"([package]
name    = "fw"
version = "0.1.0"

[targets.main]
kind              = "entry"
main              = "src/main.cpp"
test-protocol     = 1
test-main         = true
test-capabilities = ["check-details", "output"]
test-session      = ["fw-serve", "--stdio"]
)"},
                                       {"fw/src/main.cpp", R"CPP(import std;
extern bool cases(std::ofstream& report, bool list, const std::set<std::string>& selected);
int main() {
    const char* path = std::getenv("MTEST_REPORT");
    if (!path) return 4;
    std::ofstream report(path, std::ios::app);
    const std::string mode = std::getenv("MTEST_MODE") ? std::getenv("MTEST_MODE") : "run";
    std::set<std::string> selected;
    if (const char* s = std::getenv("MTEST_SELECT")) {
        std::ifstream in(s);
        for (std::string line; std::getline(in, line);) selected.insert(line);
    }
    report << R"({"e":"program","protocol":1,"level":1,"framework":"fw","version":"0.1.0"})" << "\n";
    const bool failed = cases(report, mode == "list", selected);
    report << (failed ? R"({"e":"done","status":"fail","counts":{}})" : R"({"e":"done","status":"pass","counts":{}})") << "\n";
    report.flush();
    return failed ? 1 : 0;
}
)CPP"},
                                       {"app/tests/t.cpp", R"CPP(import std;
bool cases(std::ofstream& r, bool list, const std::set<std::string>& selected) {
    auto on = [&](std::string_view id) { return selected.empty() || selected.contains(std::string(id)); };
    if (list) {
        r << R"({"e":"case","id":"stated","name":"stated","file":"t.cpp","line":3})" << "\n";
        r << R"({"e":"case","id":"plain","name":"plain","file":"t.cpp","line":4})" << "\n";
        return false;
    }
    if (on("stated")) {
        r << R"({"e":"begin","id":"stated","t":0})" << "\n";
        r << R"({"e":"output","id":"stated","stream":"out","text":"said by the case\n"})" << "\n";
        r << R"({"e":"failure","id":"stated","file":"t.cpp","line":3,"message":"a == b","expected":"2","actual":"3","source":"expect(eq(a, b));","fatal":true})" << "\n";
        r << R"({"e":"end","id":"stated","status":"fail","ms":0,"checks":{"held":4,"failed":1}})" << "\n";
    }
    if (on("plain")) {
        std::println("written by plain");
        r << R"({"e":"begin","id":"plain","t":0})" << "\n";
        r << R"({"e":"end","id":"plain","status":"pass","ms":0})" << "\n";
    }
    return on("stated");
}
)CPP"}});
        t.write("app/mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dev-dependencies]\nfw = {{ path = \"{}\" }}\n",
                                              generic(t.at("fw"))));
        auto app = [&](std::vector<std::string> args) { return t.mcpp_in("app", std::move(args)); };
        // B6
        auto g = app({"test", "--guide", "--message-format", "json"});
        expect(fatal(e2e::succeeded(g)));
        const auto guide = s::json_lines(g.out);
        expect(fatal(!guide.empty() && guide.front().contains("guide"))) << g.out;
        json fw;
        for (const auto& x : guide.front()["guide"]["frameworks"]) if (x["entry"] == "fw/main") fw = x;
        expect(fw["capabilities"] == json::parse(R"(["check-details", "output"])")) << fw.dump();
        expect(fw["session"] == json::parse(R"(["fw-serve", "--stdio"])")) << fw.dump();
        auto gh = app({"test", "--guide"});
        expect(e2e::succeeded(gh));
        expect(e2e::contains(s::text(gh), "implements  check-details, output"));
        expect(e2e::contains(s::text(gh), "session  fw-serve --stdio"));
        // B5
        auto nr = app({"test", "--no-run", "--message-format", "json", "--", "--flag"});
        expect(fatal(e2e::succeeded(nr)));
        auto rec = record(nr.out, "t");
        expect(rec["status"] == "built") << rec.dump();
        expect(fatal(rec.contains("launch"))) << rec.dump();
        expect(rec["launch"]["argv"].back() == "--flag") << "the launch omits the arguments after --";
        auto argv0 = rec["launch"]["argv"][0].get<std::string>();
        std::string argv0_slashed = argv0;
        std::ranges::replace(argv0_slashed, '\\', '/');
        expect(argv0 == rec["program"].get<std::string>() || argv0_slashed == rec["program"].get<std::string>()) << "the launch does not start the program";
        expect(!rec["launch"]["env"].contains("MTEST_PROTOCOL")) << "the launch carries the protocol's variables";
        expect(!rec["launch"].value("cwd", "").empty()) << "the launch names no directory";
        // O3, O8: both cases.
        auto all = app({"test", "--message-format", "json"});
        expect(all.started && all.status != 0) << "the failing case passed";
        rec = record(all.out, "t");
        expect(fatal(rec.contains("cases"))) << all.out;
        expect(rec["cases"][0]["failures"][0]["source"] == "expect(eq(a, b));") << "the failure's source";
        expect(rec["cases"][0]["failures"][0]["fatal"] == true) << "the failure's fatal";
        expect(rec["cases"][0]["checks"]["held"] == 4 && rec["cases"][0]["checks"]["failed"] == 1) << "the case's checks";
        // The script compared python's print of the string, which ends no line: the stated text.
        expect(rec["cases"][0].value("output", "") == "said by the case\n" || rec["cases"][0].value("output", "") == "said by the case")
            << "the case's stated output: " << rec["cases"][0].dump();
        expect(!rec["cases"][1].contains("output")) << "the output of a program of two cases was divided";
        bool kept = false;
        for (const auto& f : s::glob(t.at("app/target/mtest/runs"), "*/cases.ndjson")) {
            const auto c = s::read_file(f);
            kept = kept || c.contains(R"("checks":{"failed":1,"held":4})") || c.contains(R"("checks":{"held":4,"failed":1})");
        }
        expect(kept) << "the kept run has no checks";
        // O8: one case run alone.
        auto one = app({"test", "--case", "plain", "--message-format", "json"});
        expect(fatal(e2e::succeeded(one)));
        expect(e2e::contains(record(one.out, "t")["cases"][0].value("output", ""), "written by plain"));
    };
}};

}  // namespace
