// `mcpp test` over a workspace's members: selection, one plan per configuration group, the report
// of a shared build, a failing member, a member's programs (formerly tests/e2e/90, 854, 855, 856
// and 1034).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// The directories holding a graph (build.ninja, test.ninja or both), outside the build programs'.
std::vector<fs::path> graph_dirs(const s::project& p) {
    std::set<fs::path> dirs;
    for (const auto& f : s::find_files(p.at("target"), "(build|test)\\.ninja"))
        if (!f.generic_string().contains("/.build-mcpp/"))
            dirs.insert(f.parent_path());
    return {dirs.begin(), dirs.end()};
}

// The value of `"key":<digits>` in a record line.
std::optional<long> number(std::string_view line, std::string_view key) {
    std::match_results<std::string_view::const_iterator> m;
    const std::regex re{std::format("\"{}\":([0-9]+)", key)};
    if (!std::regex_search(line.begin(), line.end(), m, re))
        return std::nullopt;
    return std::stol(m[1].str());
}

// A workspace of libraries, each with one test `test_<m>` importing its module `sel<tag>_<m>`.
s::project libraries(const fs::path& dir, std::string_view tag, std::initializer_list<std::string_view> members) {
    std::string list;
    for (auto m : members) list += std::format("{}\"{}\"", list.empty() ? "" : ", ", m);
    s::project ws(dir, {});
    ws.write("mcpp.toml", std::format("[workspace]\nmembers = [{}]\n", list));
    for (auto m : members) {
        ws.write(std::format("{}/mcpp.toml", m),
                 std::format("[package]\nname    = \"{}\"\nversion = \"0.1.0\"\n\n[targets.{}]\nkind = \"lib\"\n", m, m));
        ws.write(std::format("{}/src/{}.cppm", m, m),
                 std::format("export module sel{}_{};\nexport int {}_v() {{ return 1; }}\n", tag, m, m));
        ws.write(std::format("{}/tests/test_{}.cpp", m, m),
                 std::format("import sel{}_{};\nint main() {{ return {}_v() == 1 ? 0 : 1; }}\n", tag, m, m));
    }
    return ws;
}

E2E workspace{"workspace", [] {
    "90 workspace test"_test = [] {
        // `-p <member>`, `--workspace` and a bare `mcpp test` at a virtual root; discovery is scoped
        // to a member, so two members may each have tests/main.cpp. (The script's `requires: gcc`
        // stood on line 8, which run_all.sh does not read: it ran on every host, and so does this.)
        s::project ws{{"mcpp.toml", "[workspace]\nmembers = [\"liba\", \"libb\"]\n"},
                      {"liba/tests/main.cpp", "int main() { return (1 + 1 == 2) ? 0 : 1; }\n"},
                      {"libb/tests/main.cpp", "int main() { return (2 * 2 == 4) ? 0 : 1; }\n"}};
        for (auto l : {"liba", "libb"}) {
            ws.write(std::format("{}/mcpp.toml", l),
                     std::format("[package]\nname    = \"{}\"\nversion = \"0.1.0\"\n\n[targets.{}]\nkind = \"lib\"\n", l, l));
            ws.write(std::format("{}/src/{}.cppm", l, l), std::format("export module {};\n", l));
        }
        // 1. -p selects one member.
        auto r1 = ws.mcpp({"test", "-p", "liba"});
        expect(fatal(e2e::succeeded(r1)));
        expect(e2e::contains(s::text(r1), "1 passed"));
        expect(!s::contains_nocase(s::text(r1), "testing member 'libb'")) << "-p liba also ran libb";
        // 2. --workspace runs both, with no duplicate-stem error.
        auto r2 = ws.mcpp({"test", "--workspace"});
        expect(fatal(e2e::succeeded(r2)));
        expect(!s::contains_nocase(s::text(r2), "duplicate test name")) << "the duplicate-stem bug";
        expect(s::contains_nocase(s::text(r2), "testing member 'liba'"));
        expect(s::contains_nocase(s::text(r2), "testing member 'libb'"));
        // 3. A bare `mcpp test` at the virtual root fans out the same way.
        auto r3 = ws.mcpp({"test"});
        expect(fatal(e2e::succeeded(r3)));
        expect(!s::contains_nocase(s::text(r3), "duplicate test name"));
        expect(s::contains_nocase(s::text(r3), "testing member 'libb'"));
        // 4. A failing member fails the run, and the other still runs; the summary carries the
        // counts and, on a line of its own, the names.
        ws.write("libb/tests/main.cpp", "int main(){return 1;}\n");
        auto r4 = ws.mcpp({"test", "--workspace"});
        expect(r4.started && r4.status != 0) << "the failing member did not fail the run";
        expect(s::contains_nocase(s::text(r4), "workspace test: 1/2 member(s) failed"));
        expect(s::contains_nocase(s::text(r4), "failed members: libb"));
        expect(s::contains_nocase(s::text(r4), "testing member 'liba'"));
    };

    "854 a test over several members plans and builds once"_test[s::unix_shell] = [] {
        // `test --workspace` plans once per configuration group: a member two members use runs its
        // build program once, with the union of the features asked of it.
        s::project ws{{"mcpp.toml", "[workspace]\nmembers = [\"core\", \"a\", \"b\"]\n"},
                      {"core/mcpp.toml", R"([package]
name    = "core"
version = "0.1.0"

[features]
extra = []

[targets.core]
kind = "lib"
)"},
                      {"core/src/core.cppm", R"(export module sel854_core;
export int core_v() { return 1; }
export int core_extra() {
#ifdef MCPP_FEATURE_EXTRA
    return 1;
#else
    return 0;
#endif
}
)"},
                      {"core/build.mcpp", R"(import std;
import mcpp;
int main() {
    std::ofstream(std::string(mcpp::out_dir()) + "/runs.log", std::ios::app) << "ran\n";
    return 0;
}
)"},
                      {"a/mcpp.toml", R"([package]
name    = "a"
version = "0.1.0"

[dependencies]
core = { path = "../core", features = ["extra"] }

[targets.a]
kind = "lib"
)"},
                      {"b/mcpp.toml", R"([package]
name    = "b"
version = "0.1.0"

[dependencies]
core = { path = "../core" }

[targets.b]
kind = "lib"
)"}};
        for (auto m : {"a", "b"}) {
            ws.write(std::format("{}/src/{}.cppm", m, m),
                     std::format("export module sel854_{};\nimport sel854_core;\nexport int {}_v() {{ return core_v(); }}\n", m, m));
            // Both members name their test `main`: discovery is scoped to each member.
            ws.write(std::format("{}/tests/main.cpp", m),
                     std::format("import sel854_{};\nint main() {{ return {}_v() == 1 ? 0 : 1; }}\n", m, m));
        }
        auto runs = [&] {
            std::size_t n = 0;
            for (const auto& f : s::find_files(ws.dir(), "runs\\.log")) n += s::count_lines(s::read_file(f), "ran");
            return n;
        };
        auto core_edges = [&](const fs::path& dir) {
            return std::ranges::count_if(s::ninja_log_outputs(dir), [](const std::string& o) {
                return std::regex_search(o, std::regex(R"((^|/)obj/core/src/core\.m\.o$)"));
            });
        };
        auto reset = [&] { for (auto d : {"target", "core/target", "a/target", "b/target"}) ws.remove(d); };

        // F
        reset();
        auto f = ws.mcpp({"test", "--workspace"});
        expect(fatal(e2e::succeeded(f)));
        const auto fl = s::text(f);
        expect(eq(runs(), 1u)) << "core's build program ran more than once";
        expect(eq(s::count_lines(fl, "Workspace building"), 1u)) << "the members were planned more than once";
        expect(e2e::contains(fl, "Workspace building 2 members: a, b"));
        expect(e2e::contains(fl, "member 'a' (2/3) ok — 1 passed"));
        expect(e2e::contains(fl, "member 'b' (3/3) ok — 1 passed"));
        expect(e2e::contains(fl, "workspace result ok. 3 member(s); 2 passed"));
        auto dirs = graph_dirs(ws);
        expect(fatal(eq(dirs.size(), 1u))) << "the members were built in more than one directory";
        expect(eq(core_edges(dirs.front()), 1)) << "core was compiled more than once";

        // G
        reset();
        expect(fatal(e2e::succeeded(ws.mcpp({"build", "--workspace"}))));
        dirs = graph_dirs(ws);
        expect(fatal(!dirs.empty()));
        const auto dir = dirs.front();
        const auto before = core_edges(dir);
        expect(before == 1 && runs() == 1) << "the build did not compile and run core once";
        auto g = ws.mcpp({"test", "--workspace"});
        expect(fatal(e2e::succeeded(g)));
        expect(eq(graph_dirs(ws).front(), dir)) << "the test build used another directory";
        expect(eq(core_edges(dir), before)) << "test --workspace compiled core again";
        expect(eq(runs(), 1u)) << "test --workspace ran core's build program again";
        expect(e2e::contains(s::text(g), "workspace result ok. 3 member(s); 2 passed"));
    };

    "855 a shared test build is reported once per group"_test[s::unix_shell] = [] {
        // A build several members' tests share is reported once, by addition: a group_build
        // record, a build_group on each member's summary, the members' build_ms its group's.
        auto ws = libraries(s::case_dir() / "ws", "855", {"a", "b", "c"});

        // H: the records, from stdout alone.
        auto h = ws.mcpp({"test", "-p", "a", "-p", "b", "--message-format", "json"});
        expect(fatal(e2e::succeeded(h)));
        std::vector<std::string> lines;
        for (const auto& l : s::lines_with(h.out, ""))
            if (!l.empty()) lines.push_back(l);
        expect(fatal(!lines.empty()));
        for (const auto& l : lines)
            expect(l.starts_with("{") && l.ends_with("}")) << "a line of the stream is not one JSON object: " << l;
        expect(eq(s::count_lines(h.out, R"("group_build")"), 1u));
        const auto& first = lines.front();
        expect(first.starts_with(R"({"group_build":)")) << "group_build is not before the first test record";
        expect(e2e::contains(first, R"("group":0)"));
        expect(e2e::contains(first, R"("members":["a","b"])"));
        const auto gms = number(first, "build_ms");
        expect(fatal(gms.has_value())) << "group_build has no build_ms";
        for (auto m : {"a", "b"}) {
            std::string summary;
            for (const auto& l : lines)
                if (l.contains(R"("summary")") && l.contains(std::format(R"("member":"{}")", m))) { summary = l; break; }
            expect(fatal(!summary.empty())) << "no summary for " << m;
            expect(e2e::contains(summary, R"("build_group":0)"));
            expect(number(summary, "build_ms") == gms) << m << "'s build_ms is not its group's";
            expect(e2e::contains(summary, R"("elapsed_ms":)"));
            expect(e2e::contains(summary, R"("run_ms":)"));
            expect(e2e::contains(h.out, std::format(R"("member":"{}","test":"test_{}","status":"pass")", m, m)));
            std::string rec;
            for (const auto& l : lines)
                if (l.contains(std::format(R"("member":"{}","test":)", m))) { rec = l; break; }
            expect(number(rec, "duration_ms").has_value()) << "no duration_ms for " << m << "'s test";
            expect(number(rec, "build_ms").has_value()) << "no build_ms for " << m << "'s test";
        }
        const auto& last = lines.back();
        expect(last.starts_with(R"({"workspace_summary":)")) << "the stream does not end with workspace_summary";
        expect(e2e::contains(last, R"("members":2,"passed":2,"failed":0)"));

        // J: a member no -p names is in no record, and is not built.
        expect(s::lacks(h.out, R"("member":"c")"));
        bool built_c = false;
        for (const auto& f : s::find_files(ws.at("target"), ".*"))
            if (f.generic_string().contains("/obj/c")) built_c = true;
        expect(!built_c) << "c was built";

        // I: the human report states the group once, and each member's run.
        auto i = ws.mcpp({"test", "-p", "a", "-p", "b"});
        expect(fatal(e2e::succeeded(i)));
        const auto il = s::text(i);
        std::size_t group_lines = 0;
        for (const auto& l : s::lines_with(il, "built members"))
            if (std::regex_search(l, std::regex(R"(Workspace +built members a, b in [0-9]+\.[0-9]+s)"))) ++group_lines;
        expect(eq(group_lines, 1u)) << "the report does not have one line for the group";
        for (auto m : {"a", "b"})
            expect(s::has_line(il, std::format(R"(member '{}' \([12]/2\) ok — 1 passed, run [0-9]+\.[0-9]+s)", m)));
    };

    "856 a test over several members continues past a failing one"_test[s::unix_shell] = [] {
        // `mcpp test -p a -p b` tests both (mcpp#750), and a member that fails -- its test, its
        // package's build or its plan -- fails alone.
        auto obj_c = [](const s::project& ws) {
            for (const auto& f : s::find_files(ws.at("target"), ".*"))
                if (f.generic_string().contains("/obj/c")) return true;
            return false;
        };
        // I
        {
            auto ws = libraries(s::case_dir() / "w1", "856", {"a", "b", "c"});
            auto i1 = ws.mcpp({"test", "-p", "a", "-p", "b"});
            expect(fatal(e2e::succeeded(i1)));
            const auto l = s::text(i1);
            expect(e2e::contains(l, "test_a ... ok"));
            expect(e2e::contains(l, "test_b ... ok")) << "the repeated -p kept one value";
            expect(s::lacks(l, "test_c")) << "c's test ran although no -p named it";
            expect(e2e::contains(l, "workspace result ok. 2 member(s); 2 passed; 0 failed"));
            auto i2 = ws.mcpp({"test", "-p", "b", "-p", "a"});
            expect(e2e::succeeded(i2));
            expect(e2e::contains(s::text(i2), "workspace result ok. 2 member(s); 2 passed; 0 failed"));
            auto i3 = ws.mcpp({"test", "-p", "a", "-p", "b", "--message-format", "json"});
            expect(e2e::succeeded(i3));
            expect(e2e::contains(i3.out, R"("workspace_summary":{"members":2,"passed":2,"failed":0)"));
            expect(!obj_c(ws)) << "c was built";
        }
        // K
        {
            auto ws = libraries(s::case_dir() / "w2", "856", {"a", "b", "c"});
            ws.write("c/tests/test_c.cpp", "int main() { return 1; }\n");
            auto k = ws.mcpp({"test", "--workspace"});
            expect(e2e::exited(k, 1));
            const auto l = s::text(k);
            expect(e2e::contains(l, "member 'a' (1/3) ok"));
            expect(e2e::contains(l, "member 'b' (2/3) ok"));
            expect(e2e::contains(l, "member 'c' (3/3) FAILED"));
            expect(e2e::contains(l, "failed members: c"));
            expect(eq(s::count_lines(l, "Workspace building"), 1u)) << "the members were planned more than once";
        }
        // L
        {
            auto ws = libraries(s::case_dir() / "w3", "856", {"a", "b", "c"});
            ws.write("c/src/c.cppm", "export module sel856_c;\nexport int c_v() { return \"not an int\"; }\n");
            auto r = ws.mcpp({"test", "--workspace"});
            expect(r.started && r.status != 0) << "a member whose package does not compile exited 0";
            const auto l = s::text(r);
            expect(e2e::contains(l, "test_a ... ok"));
            expect(e2e::contains(l, "test_b ... ok"));
            expect(e2e::contains(l, "member 'c' (3/3) FAILED"));
            expect(e2e::contains(l, "member 'c': its package did not build"));
            expect(e2e::contains(l, "failed members: c"));
            expect(s::lacks(l, "test_c ... ok"));
        }
        // M
        {
            auto ws = libraries(s::case_dir() / "w4", "856", {"a", "b", "d", "c"});
            ws.write("d/mcpp.toml", R"([package]
name    = "d"
version = "0.1.0"

[dependencies]
absent = { path = "../no-such-directory" }

[targets.d]
kind = "lib"
)");
            auto r = ws.mcpp({"test", "--workspace"});
            expect(r.started && r.status != 0) << "a member that cannot be planned exited 0";
            const auto l = s::text(r);
            for (auto m : {"a", "b", "c"})
                expect(e2e::contains(l, std::format("test_{} ... ok", m)));
            expect(e2e::contains(l, "member 'd' (3/4) FAILED"));
            expect(e2e::contains(l, "failed members: d"));
            expect(e2e::contains(l, "workspace test: 1/4 member(s) failed; 3 passed"));
        }
        // N (Linux: LD_LIBRARY_PATH is the runtime search variable)
        if (s::on_linux()) {
            s::project ws(s::case_dir() / "w5", {{"mcpp.toml", "[workspace]\nmembers = [\"x\", \"y\"]\n"}});
            for (auto [m, other] : {std::pair{"x", "y"}, std::pair{"y", "x"}}) {
                ws.write(std::format("{}/rt856{}/keep", m, m), "");
                ws.write(std::format("{}/mcpp.toml", m),
                         std::format("[package]\nname    = \"{}\"\nversion = \"0.1.0\"\n\n[runtime]\nlibrary_dirs = "
                                     "[\"rt856{}\"]\n\n[targets.{}]\nkind = \"lib\"\n",
                                     m, m, m));
                ws.write(std::format("{}/src/{}.cppm", m, m),
                         std::format("export module sel856_{};\nexport int {}_v() {{ return 1; }}\n", m, m));
                ws.write(std::format("{}/tests/env.cpp", m), std::format(R"(#include <cstdlib>
#include <cstring>
int main() {{
    const char* p = std::getenv("LD_LIBRARY_PATH");
    if (!p) return 2;
    if (!std::strstr(p, "rt856{}")) return 3;    // its own runtime directory
    if (std::strstr(p, "rt856{}")) return 4; // another member's
    return 0;
}}
)",
                                                                         m, other));
            }
            auto n = ws.mcpp({"test", "--workspace"});
            expect(e2e::succeeded(n)) << "a member's test saw the wrong runtime directories (3: its own missing, 4: another's present)";
            expect(eq(s::count_lines(s::text(n), "Workspace building"), 1u));
            expect(e2e::contains(s::text(n), "workspace result ok. 2 member(s); 2 passed"));
        }
    };

    "1034 a workspace members test finds its programs"_test[s::gcc] = [] {
        // A member's test finds the member's programs (MCPP_BIN_<NAME>) under --workspace as
        // under -p; each finds its own program of one shared name.
        s::project ws{{"mcpp.toml", "[workspace]\nmembers = [\"a\", \"b\"]\n"}};
        for (auto m : {"a", "b"}) {
            ws.write(std::format("{}/mcpp.toml", m),
                     std::format("[package]\nname    = \"{}\"\nversion = \"0.1.0\"\n\n[targets.tool]\nkind = \"bin\"\nmain = \"src/tool.cpp\"\n", m));
            ws.write(std::format("{}/src/tool.cpp", m), std::format("import std;\nint main() {{ std::println(\"{}\"); return 0; }}\n", m));
            ws.write(std::format("{}/tests/finds.cpp", m), std::format(R"(import std;
int main() {{
    const char* tool = std::getenv("MCPP_BIN_TOOL");
    if (!tool || !std::filesystem::is_regular_file(tool)) {{ std::println("no MCPP_BIN_TOOL"); return 1; }}
    const std::string path = tool;
    return path.find("{}") != std::string::npos || std::filesystem::path(path).parent_path().string().find("{}") != std::string::npos ? 0 : 2;
}}
)",
                                                                      m, m));
        }
        auto w = ws.mcpp({"test", "--workspace"});
        expect(e2e::succeeded(w)) << "under --workspace a member's test did not find its program";
        expect(e2e::succeeded(ws.mcpp({"test", "-p", "a"})));
        expect(e2e::contains(s::text(w), "finds ... ok"));
    };
}};

}  // namespace
