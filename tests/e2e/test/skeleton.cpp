// `mcpp new`'s test skeleton, and where a test that defines no `main` begins (formerly
// tests/e2e/1037).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// The lines that follow (up to `after`) a line containing `anchor` (`grep -A`).
std::string following(std::string_view text, std::string_view anchor, std::size_t after) {
    std::vector<std::string> lines = s::lines_with(text, "");
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (lines[i].contains(anchor))
            for (std::size_t j = i; j < lines.size() && j <= i + after; ++j) out += lines[j] + "\n";
    return out;
}

E2E skeleton{"skeleton", [] {

    "1037 a skeleton tests with mtest and a test needs no main"_test[tag{"requires:gcc"}] = [] {
        // `mcpp new` scaffolds a program or a library whose test is mtest's, and a test file with no
        // `main` begins at the entry a dependency offers.
        s::needs({"gcc"});
        s::project t(s::case_dir() / "t", {});
        auto in = [&](std::string_view sub, std::vector<std::string> args) { return t.mcpp_in(sub, std::move(args)); };

        // A
        auto a = t.mcpp({"new", "acme.my-lib", "--template", "lib"});
        expect(fatal(e2e::succeeded(a)));
        for (auto f : {"mcpp.toml", "src/my-lib.cppm", "src/hello.cppm", "src/hello.cpp", "tests/hello.cpp"})
            expect(fatal(t.exists(std::format("acme.my-lib/{}", f)))) << "no " << f;
        expect(s::has_line(t.read("acme.my-lib/src/my-lib.cppm"), "^export module acme.my_lib;"));
        expect(s::has_line(t.read("acme.my-lib/src/hello.cppm"), "^export module acme.my_lib:hello;"));
        expect(e2e::contains(t.read("acme.my-lib/src/hello.cpp"), "namespace acme::my_lib"));
        expect(s::lacks(t.read("acme.my-lib/tests/hello.cpp"), "int main")) << "the test defines main";
        expect(e2e::succeeded(in("acme.my-lib", {"build"}))) << "the library does not build";
        auto at = in("acme.my-lib", {"test"});
        expect(e2e::succeeded(at));
        expect(s::has_line(s::text(at), R"(hello \.\.\. ok \(.*1 case\))")) << "the test is not one protocol case";
        // B
        expect(fatal(e2e::succeeded(t.mcpp({"new", "prog"}))));
        auto bt = in("prog", {"test"});
        expect(e2e::succeeded(bt));
        expect(s::has_line(s::text(bt), R"(test_smoke \.\.\. ok \(.*1 case\))"));
        // C: a framework whose entry offers to begin tests.
        t.write("fw/mcpp.toml", R"([package]
name    = "fw"
version = "0.1.0"

[targets.main]
kind      = "entry"
main      = "src/entry.cpp"
test-main = true
)");
        t.write("fw/src/entry.cpp", "import std;\nextern \"C++\" int run_cases();\nint main() { std::println(\"fw main\"); return run_cases(); }\n");
        t.write("fw/tests/own.cpp", "extern \"C++\" int run_cases() { return 0; }\n");
        t.write("app/mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dev-dependencies]\nfw = { path = \"../fw\" }\n");
        t.write("app/tests/begun.cpp", "extern \"C++\" int run_cases() { return 0; }\n");
        t.write("app/tests/own.cpp", "import std;\nint main() { std::println(\"own main\"); return 0; }\n");
        auto c = in("app", {"test"});
        expect(e2e::succeeded(c)) << "the dependent's tests";
        expect(e2e::contains(s::text(c), "begun ... ok"));
        expect(e2e::contains(s::text(c), "own ... ok"));
        expect(e2e::succeeded(in("fw", {"test"}))) << "the framework's own test";
        const auto app_c = t.read("app/mcpp.toml");
        t.write("app/mcpp.toml", app_c + "\n[dependencies]\nfw = { path = \"../fw\" }\n");
        expect(e2e::succeeded(in("app", {"test"}))) << "a framework named in two tables offered twice";
        t.write("app/mcpp.toml", app_c);
        // D: a second framework offers too.
        fs::copy(t.at("fw"), t.at("fw2"), fs::copy_options::recursive);
        t.write("fw2/mcpp.toml", std::regex_replace(t.read("fw2/mcpp.toml"), std::regex("name    = \"fw\""), "name    = \"fw2\""));
        t.write("app/mcpp.toml", t.read("app/mcpp.toml") + "fw2 = { path = \"../fw2\" }\n");
        auto d = in("app", {"test"});
        expect(d.started && d.status != 0) << "two offers were taken";
        expect(e2e::contains(s::text(d), "fw/main"));
        expect(e2e::contains(s::text(d), "fw2/main"));
        t.remove("app/tests/own.cpp");  // under [test] main every test begins at the entry
        t.write("app/mcpp.toml", t.read("app/mcpp.toml") + "\n[test]\nmain = { from = \"fw2/main\" }\n");
        expect(e2e::succeeded(in("app", {"test"}))) << "[test] main did not choose";
        // E
        auto e = t.mcpp({"new", "3d", "--template", "lib"});
        expect(e.started && e.status != 0) << "a name beginning with a digit was taken";
        expect(e2e::contains(s::text(e), "does not begin with a letter"));
        // F
        t.write("fwm/mcpp.toml", R"([package]
name    = "fwm"
version = "0.1.0"

[targets.main]
kind      = "entry"
main      = "src/entry.cpp"
test-main = true
)");
        t.write("fwm/src/fwm.cppm", "export module fwm;\nexport int fwm_cases() { return 0; }\n");
        t.write("fwm/src/entry.cpp", "import std;\nimport fwm;\nint main() { std::println(\"began at fwm\"); return fwm_cases(); }\n");
        t.write("hdr/mcpp.toml", "[package]\nname    = \"hdr\"\nversion = \"0.1.0\"\n");
        t.write("hdr/src/hdr_main.cpp", "import std;\nint main() { std::println(\"began at hdr\"); return 0; }\n");
        t.write("hdr/src/hdr.cpp", "int hdr_marker() { return 0; }\n");
        t.write("mixed/mcpp.toml", "[package]\nname    = \"mixed\"\nversion = \"0.1.0\"\n\n[dev-dependencies]\nfwm = { path = \"../fwm\" }\nhdr = { path = \"../hdr\" }\n");
        t.write("mixed/tests/modular.cpp", "import fwm;\nint uses_fwm() { return fwm_cases(); }\n");
        t.write("mixed/tests/headers.cpp", "int uses_headers() { return 0; }\n");
        auto f = in("mixed", {"test"});
        expect(fatal(e2e::succeeded(f)));
        expect(e2e::contains(following(s::text(f), "modular ... ok", 2), "began at fwm"));
        expect(e2e::contains(following(s::text(f), "headers ... ok", 2), "began at hdr"));
        // G
        t.write("fwm/mcpp.toml", t.read("fwm/mcpp.toml") + "test-guide = \"GUIDE.md\"\n");
        t.write("fwm/GUIDE.md", "# Writing a test with fwm\n");
        auto g = in("mixed", {"test", "--guide"});
        expect(e2e::succeeded(g));
        expect(e2e::contains(s::text(g), "fwm 0.1.0"));
        expect(e2e::contains(s::text(g), "fwm/GUIDE.md"));
        auto gj = in("mixed", {"test", "--guide", "--message-format", "json"});
        expect(e2e::succeeded(gj));
        expect(e2e::matches(gj.out, R"("guide":"[^"]*fwm/GUIDE.md")"));
        t.write("mixed/tests/modular.cpp", "import fwm;\nint uses_fwm() { return fwm_cases(); }\nint broken = missing_name;\n");
        auto gf = in("mixed", {"test"});
        expect(gf.started && gf.status != 0) << "a test that does not compile passed";
        expect(e2e::contains(s::text(gf), "how these tests are written: mcpp test --guide"));
        // H
        t.write("fwh/mcpp.toml", R"([package]
name    = "fwh"
version = "0.1.0"

[targets.main]
kind      = "entry"
main      = "src/entry.cpp"
test-main = true

[dependencies]
hdr = { path = "../hdr" }
)");
        t.write("fwh/src/entry.cpp", "import std;\nint main() { std::println(\"began at fwh\"); return 0; }\n");
        t.write("fwh/src/fwh.cpp", "int fwh_marker() { return 0; }\n");
        t.write("deep/mcpp.toml", "[package]\nname    = \"deep\"\nversion = \"0.1.0\"\n\n[dev-dependencies]\nfwh = { path = \"../fwh\" }\n");
        t.write("deep/tests/one.cpp", "int uses_fwh() { return 0; }\n");
        auto h = in("deep", {"test"});
        expect(e2e::succeeded(h)) << "a main beneath the dev-dependency was linked";
        expect(e2e::contains(s::text(h), "began at fwh"));
        // I
        t.write("shlib/mcpp.toml", "[package]\nname    = \"shlib\"\nversion = \"0.1.0\"\n\n[targets.shlib]\nkind = \"shared\"\n");
        t.write("shlib/src/shlib.cpp", "extern \"C\" int shlib_answer() { return 42; }\n");
        t.write("shapp/mcpp.toml", "[package]\nname    = \"shapp\"\nversion = \"0.1.0\"\n\n[dev-dependencies]\nshlib = { path = \"../shlib\" }\n");
        t.write("shapp/tests/links.cpp", "extern \"C\" int shlib_answer();\nint main() { return shlib_answer() == 42 ? 0 : 1; }\n");
        expect(e2e::succeeded(in("shapp", {"test"}))) << "a shared dev-dependency is not linked";
        // J
        t.write("wsj/mcpp.toml", "[workspace]\nmembers = [\"one\", \"two\"]\n");
        t.write("wsj/one/mcpp.toml", "[package]\nname    = \"one\"\nversion = \"0.1.0\"\n\n[dev-dependencies]\nhdr = { path = \"../../hdr\" }\n");
        t.write("wsj/one/tests/plain.cpp", "int uses_headers() { return 0; }\n");
        t.write("wsj/two/mcpp.toml", "[package]\nname    = \"two\"\nversion = \"0.1.0\"\n\n[dev-dependencies]\nfw = { path = \"../../fw\" }\n");
        t.write("wsj/two/tests/begun.cpp", "extern \"C++\" int run_cases() { return 0; }\n");
        auto j = in("wsj", {"test", "--workspace"});
        expect(e2e::succeeded(j)) << "a member's test took another member's beginning";
        expect(e2e::contains(s::text(j), "began at hdr"));
        expect(e2e::contains(s::text(j), "fw main"));
        // K
        expect(fatal(e2e::succeeded(t.mcpp({"new", "kapp"}))));
        for (auto cmd : {"build", "test", "build", "test"})
            expect(fatal(e2e::succeeded(in("kapp", {cmd}))));
        const auto graphs = s::glob(t.at("kapp/target"), "*/*/build.ninja");
        expect(fatal(eq(graphs.size(), 1u))) << "build and test are several configurations";
        const auto dir = graphs.front().parent_path();
        const auto main_edges = std::ranges::count_if(s::ninja_log_outputs(dir), [](const std::string& o) {
            return std::regex_search(o, std::regex(R"(obj/main\.(o|obj)$)"));
        });
        expect(eq(main_edges, 1)) << "the program's source was compiled again";
        const auto cdb_text = s::read_file(dir / "compile_commands.json");
        expect(e2e::contains(cdb_text, "test_smoke")) << "the compile database has no test";
        for (const auto& entry : s::json(cdb_text)) {
            auto file = entry.value("file", "");
            std::ranges::replace(file, '\\', '/');
            if (!file.ends_with("kapp/src/main.cpp") || !entry.contains("arguments")) continue;
            for (const auto& arg : entry["arguments"])
                expect(!e2e::contains(arg.get<std::string>(), "__OPENKAL__") && !e2e::contains(arg.get<std::string>(), "openkal"))
                    << "the program's source sees the dev-dependency: " << arg.get<std::string>();
        }
        // L
        t.write("lib-tools/mcpp.toml", "[package]\nname    = \"lib-tools\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.c\"]\n");
        t.write("lib-tools/src/table.c", R"(int table_value(void) { return 7; }
#ifdef MAKE_TABLE
#include <stdio.h>
int main(void) { printf("%d\n", table_value()); return 0; }
#endif
)");
        t.write("lapp/mcpp.toml", "[package]\nname    = \"lapp\"\nversion = \"0.1.0\"\n\n[dev-dependencies]\nlib-tools = { path = \"../lib-tools\" }\n");
        t.write("lapp/tests/own.cpp", "extern \"C\" int table_value(void);\nint main() { return table_value() == 7 ? 0 : 1; }\n");
        auto l = in("lapp", {"test"});
        expect(e2e::succeeded(l)) << "a source whose main no command compiles was left out of the link";
        expect(e2e::contains(s::text(l), "own ... ok"));
    };
}};

}  // namespace
