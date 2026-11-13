// Which tests `mcpp test` finds and runs: names from paths, a pattern, the listing, `[test]
// discover` (formerly tests/e2e/152, 154, 159 and 673).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;

namespace {

std::string manifest(std::string_view name) {
    return std::format("[package]\nname = \"{}\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n", name);
}

E2E selection{"selection", [] {
    "152 test subdir names"_test = [] {
        // The same stem in two directories of tests/ coexists; a test is named by its path.
        s::project p{{"mcpp.toml", manifest("subdirs")},
                     {"tests/00-a/0.cpp", "int main() { return 0; }\n"},
                     {"tests/01-b/0.cpp", "int main() { return 0; }\n"}};
        auto r = p.mcpp({"test"});
        expect(fatal(e2e::succeeded(r)));
        const auto out = s::text(r);
        expect(e2e::contains(out, "00-a/0 ... ok"));
        expect(e2e::contains(out, "01-b/0 ... ok"));
        expect(e2e::contains(out, "2 passed"));
    };

    "154 test filter"_test = [] {
        // `mcpp test <pattern>`: a substring of the path-based names.
        s::project p{{"mcpp.toml", manifest("filter")},
                     {"tests/00-a/0.cpp", "int main() { return 0; }\n"},
                     {"tests/01-b/0.cpp", "int main() { return 0; }\n"}};
        auto r = p.mcpp({"test", "00-a"});
        expect(fatal(e2e::succeeded(r)));
        const auto out = s::text(r);
        expect(e2e::contains(out, "00-a/0 ... ok"));
        expect(s::lacks(out, "01-b/0")) << "the filter let another test through";
        expect(e2e::contains(out, "1 passed"));
        auto none = p.mcpp({"test", "does-not-exist"});
        expect(e2e::exited(none, 2));
        expect(e2e::contains(s::text(none), "no tests match"));
    };

    "159 test list"_test = [] {
        // `mcpp test --list` names the (selected) tests and builds nothing; a file that does not
        // parse is listed too.
        s::project p{{"mcpp.toml", manifest("listing")},
                     {"tests/00-a/0.cpp", "int main() { return 0; }\n"},
                     {"tests/01-b/0.cpp", "this does not even parse\n"}};
        auto r = p.mcpp({"test", "--list"});
        expect(fatal(e2e::succeeded(r)));
        const auto out = s::text(r);
        expect(e2e::contains(out, "00-a/0"));
        expect(e2e::contains(out, "01-b/0"));
        expect(!p.exists("target")) << "--list built (target/ was created)";
        auto j = p.mcpp({"test", "--list", "00-a", "--message-format", "json"});
        expect(s::has_line(j.out, R"x("test":"00-a/0","main":".*tests/00-a/0\.cpp")x"));
        // The script's `grep -qv` held for any line without the record; what it meant is that the
        // filter keeps 01-b/0 out of the listing.
        expect(s::lacks(j.out, R"("test":"01-b/0")")) << "the filter leaked into the list";
        expect(e2e::contains(j.out, R"("summary":{"total":1})"));
    };

    "673 test discover names where tests are"_test = [] {
        // `[test] discover` takes globs in the vocabulary of `[build] sources`; a test is named by
        // its path below the fixed directory of the glob that found it.
        auto p = s::project::scaffold("disc");
        for (const auto& f : s::find_files(p.at("tests"), R"(.*\.cpp)"))
            std::filesystem::remove(f);
        p.write("tests/a_ok.cpp", "int main() { return 0; }\n");
        p.write("tests/unit/b_ok.cpp", "int main() { return 0; }\n");
        p.write("tests/fails.cpp", "int main() { return 1; }\n");
        p.write("checks/c_ok.cpp", "int main() { return 0; }\n");
        p.write("checks/deep/skipped.cpp", "int main() { return 1; }\n");
        const auto base = p.read("mcpp.toml");

        // 1. Without the key, the names are those of every earlier release.
        auto l1 = p.mcpp({"test", "--list"});
        expect(fatal(e2e::succeeded(l1)));
        expect(s::has_line(s::text(l1), "^a_ok$")) << "tests/a_ok.cpp is not named 'a_ok'";
        expect(s::has_line(s::text(l1), "^unit/b_ok$")) << "tests/unit/b_ok.cpp is not named 'unit/b_ok'";
        expect(s::no_line(s::text(l1), "^c_ok$")) << "the default discovered a file outside tests/";

        // 2 and 3. Elsewhere, with an exclusion.
        p.write("mcpp.toml", base + "\n[test]\ndiscover = [\"checks/**/*.cpp\", \"!checks/deep/**\"]\n");
        auto l2 = p.mcpp({"test", "--list"});
        expect(fatal(e2e::succeeded(l2)));
        expect(s::has_line(s::text(l2), "^c_ok$"));
        expect(s::no_line(s::text(l2), "fails|a_ok")) << "discover still discovered tests/";
        expect(s::lacks(s::text(l2), "skipped")) << "the ! glob did not exclude checks/deep/skipped.cpp";
        auto t2 = p.mcpp({"test"});
        expect(e2e::succeeded(t2)) << "a file outside the set ran?";
        expect(e2e::contains(s::text(t2), "c_ok ... ok"));
        expect(s::lacks(s::text(t2), "fails")) << "tests/fails.cpp ran";

        // 4. An empty list discovers nothing, and says where it looked.
        p.write("mcpp.toml", base + "\n[test]\ndiscover = []\n");
        auto t4 = p.mcpp({"test"});
        expect(e2e::succeeded(t4));
        expect(e2e::contains(s::text(t4), "no tests found ([test] discover = [])"));

        // 5. Two files that map to one name are refused, naming both.
        p.write("mcpp.toml", base + "\n[test]\ndiscover = [\"tests/*.cpp\", \"checks/*.cpp\"]\n");
        p.write("checks/a_ok.cpp", "int main() { return 0; }\n");
        auto t5 = p.mcpp({"test", "--list"});
        expect(t5.started && t5.status != 0) << "two files mapping to 'a_ok' were accepted";
        expect(e2e::contains(s::text(t5), "duplicate test name 'a_ok'"));
        expect(e2e::contains(s::text(t5), "tests/a_ok.cpp"));
        expect(e2e::contains(s::text(t5), "checks/a_ok.cpp"));
        p.remove("checks/a_ok.cpp");

        // 6. A value that is not an array of strings is refused, naming the key (`mcpp test`: the
        // listing is best effort over a manifest that does not load).
        p.write("mcpp.toml", base + "\n[test]\ndiscover = \"tests/**/*.cpp\"\n");
        auto t6 = p.mcpp({"test"});
        expect(t6.started && t6.status != 0) << "a string discover was accepted";
        expect(e2e::contains(s::text(t6), "[test] discover must be an array"));
    };
}};

}  // namespace
