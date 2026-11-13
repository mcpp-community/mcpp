// The granularity of a test program (design 2026-11-12 §18 P1, mcpp#927): `"package"` makes a
// package's discovered files one program, linked once and run one process per file, each part
// named by its file's path below the program's directory; at most one file defines `main`; and
// `"directory"` says which files it leaves programs of their own.
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
using nlohmann::json;

namespace {

json record(std::string_view stream, std::string_view name) {
    for (const auto& r : s::json_lines(stream))
        if (r.contains("test") && r["test"] == name) return r;
    return {};
}

// A file of one case; the files of one program are one image, so each names its case apart.
std::string unit(std::string_view id) {
    return std::format("import mcpplibs.mtest;\nusing namespace mcpplibs::mtest;\n"
                       "UTest case_{}{{\"{}\", [] {{ expect(eq(1 + 1, 2)); }}}};\n", id, id);
}

E2E granularity{"granularity", [] {
    "1081 a package's tests are one program"_test[tag{"requires:gcc"}] = [] {
        s::needs({"gcc"});
        auto p = s::project::scaffold("app");
        p.write("tests/b.cpp", unit("top"));
        p.write("tests/unit/b.cpp", unit("nested"));
        p.write("mcpp.toml", p.read("mcpp.toml") + "\n[test]\nprograms = \"package\"\n");
        // A: one program named by the fixed directory; a part per file, by its path below it.
        auto a = p.mcpp({"test", "--cases", "--message-format", "json"});
        expect(fatal(e2e::succeeded(a))) << s::text(a);
        const auto program = record(a.out, "tests");
        expect(fatal(program.contains("parts"))) << a.out;
        std::vector<std::string> names;
        for (const auto& part : program["parts"]) names.push_back(part["name"]);
        std::ranges::sort(names);
        expect(eq(names, std::vector<std::string>{"tests/b", "tests/test_smoke", "tests/unit/b"}))
            << "two files of one name in two directories are two parts";
        // B: each part is a process of its own, and a pattern names one.
        auto b = p.mcpp({"test", "--message-format", "json"});
        expect(fatal(e2e::succeeded(b))) << s::text(b);
        for (auto part : {"tests/b", "tests/unit/b"})
            expect(record(b.out, part).value("part_of", "") == "tests") << part << " ran as no part\n" << b.out;
        auto c = p.mcpp({"test", "unit/b"});
        expect(fatal(e2e::succeeded(c))) << s::text(c);
        expect(e2e::contains(c.out, "tests/unit/b ... ok"));
        expect(s::lacks(c.out, "tests/b ... ok")) << "the pattern ran another part";
        // C: two files that define main are refused, naming both.
        p.write("tests/m1.cpp", "int main() { return 0; }\n");
        p.write("tests/unit/m2.cpp", "int main() { return 0; }\n");
        auto d = p.mcpp({"test"});
        expect(fatal(!e2e::succeeded(d))) << "two beginnings were accepted";
        expect(e2e::contains(s::text(d), "m1.cpp") && e2e::contains(s::text(d), "m2.cpp")) << s::text(d);
    };

    "1082 programs by directory says what it leaves"_test[tag{"requires:gcc"}] = [] {
        s::needs({"gcc"});
        auto p = s::project::scaffold("app");
        p.write("tests/unit/a.cpp", unit("a"));
        p.write("mcpp.toml", p.read("mcpp.toml") + "\n[test]\nprograms = \"directory\"\n");
        // A: the file left in tests/ is noted, with what makes the suite one program.
        auto a = p.mcpp({"test"});
        expect(fatal(e2e::succeeded(a))) << s::text(a);
        expect(e2e::contains(s::text(a), "1 file directly in tests/ stays a program of its own"))
            << s::text(a);
        expect(e2e::contains(s::text(a), "\"package\" makes the suite one program"));
        // B: a setting that forms no group is a degradation: it runs, and --strict refuses it.
        p.remove("tests/unit/a.cpp");
        auto b = p.mcpp({"test"});
        expect(fatal(e2e::succeeded(b))) << s::text(b);
        expect(e2e::contains(s::text(b), "changes no program")) << s::text(b);
        auto c = p.mcpp({"test", "--strict"});
        expect(fatal(!e2e::succeeded(c))) << "--strict accepted a setting that changes no program";
    };
}};

}  // namespace
