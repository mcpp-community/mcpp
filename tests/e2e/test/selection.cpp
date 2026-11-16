// Which tests `mcpp test` finds and runs: names from paths, a pattern and several, the listing,
// `[test] discover` (formerly tests/e2e/152, 154, 159 and 673; 1084), the tests of a target
// (`[target.<sel>.test]`, 1098) and the features they are built with (`[test] features`, 1099, 1100).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;

namespace {

std::string manifest(std::string_view name) {
    return std::format("[package]\nname = \"{}\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n", name);
}

namespace fs = std::filesystem;

// The most recently written <name> under `target`, outside the build programs' trees.
std::optional<fs::path> newest_graph(const fs::path& target, std::string_view name) {
    std::optional<fs::path> best;
    for (const auto& f : s::find_files(target, std::regex_replace(std::string(name), std::regex{R"(\.)"}, R"(\.)")))
        if (f.generic_string().find("/.build-mcpp/") == std::string::npos
            && (!best || fs::last_write_time(f) > fs::last_write_time(*best)))
            best = f;
    return best;
}

// A library whose module `<name>.extra` is the source of its feature `extra`.
std::string featured(std::string_view name, std::string_view test) {
    return std::format(R"([package]
name     = "{0}"
version  = "0.1.0"
standard = "c++23"

[targets.{0}]
kind = "lib"

[features]
extra = {{ sources = ["src/extra/*.cppm"] }}
{1})", name, test);
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

    "1084 several patterns select every test one of them names"_test = [] {
        // `mcpp test <pattern> <pattern>`: a test one of them names runs, one neither names does
        // not; a pattern after the first is never dropped; `--print-launch` names one test and
        // refuses two patterns; the refusal for no match names every pattern.
        s::project p{{"mcpp.toml", manifest("patterns")},
                     {"tests/00-a/0.cpp", "int main() { return 0; }\n"},
                     {"tests/01-b/0.cpp", "int main() { return 0; }\n"},
                     {"tests/02-c/0.cpp", "int main() { return 0; }\n"}};
        auto r = p.mcpp({"test", "00-a", "02-c"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto out = s::text(r);
        expect(e2e::contains(out, "00-a/0 ... ok"));
        expect(e2e::contains(out, "02-c/0 ... ok")) << "the second pattern was dropped: " << out;
        expect(s::lacks(out, "01-b/0")) << "a test neither pattern names ran: " << out;
        expect(e2e::contains(out, "2 passed"));
        auto launch = p.mcpp({"test", "00-a", "02-c", "--print-launch"});
        expect(e2e::exited(launch, 2)) << s::text(launch);
        expect(e2e::contains(s::text(launch), "names several PATTERNs")) << s::text(launch);
        auto none = p.mcpp({"test", "nope-1", "nope-2"});
        expect(e2e::exited(none, 2));
        expect(e2e::contains(s::text(none), "no tests match 'nope-1 nope-2'")) << s::text(none);
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

    "1098 the tests of a target are selected by its predicate"_test = [] {
        // mcpp#942: `[target.<sel>.test] discover` adds to `[test] discover` for the targets it
        // selects. A: a `!` glob removes tests on one target and leaves them on this machine;
        // B: a glob adds tests on the targets it selects only; C: a predicate on a target-side
        // layer is refused, as the tests are discovered before it has a value; D: a key other
        // than `discover` is a warning.
        constexpr std::string_view web = "wasm32-emscripten";
        s::project p{{"mcpp.toml", manifest("bytarget") + "\n[target.'cfg(os = \"emscripten\")'.test]\n"
                                                         "discover = [\"!tests/native/**\"]\n"},
                     {"tests/core.cpp", "int main() { return 0; }\n"},
                     {"tests/native/window.cpp", "int main() { return 0; }\n"}};
        // A
        auto r = p.mcpp({"test", "--list"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(e2e::contains(s::text(r), "core")) << s::text(r);
        expect(e2e::contains(s::text(r), "native/window")) << "A: this machine lost a test: " << s::text(r);
        r = p.mcpp({"test", "--list", "--target", std::string(web)});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(e2e::contains(s::text(r), "core")) << s::text(r);
        expect(s::lacks(s::text(r), "native/window")) << "A: the excluded test is listed for the Web: " << s::text(r);
        r = p.mcpp({"test"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(e2e::contains(s::text(r), "2 passed")) << s::text(r);
        // B
        p.write("mcpp.toml", manifest("bytarget") + "\n[test]\ndiscover = [\"tests/*.cpp\"]\n"
                                                    "\n[target.'cfg(not(os = \"emscripten\"))'.test]\n"
                                                    "discover = [\"tests/native/*.cpp\"]\n");
        r = p.mcpp({"test", "--list"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(e2e::contains(s::text(r), "window")) << "B: the added test is not listed here: " << s::text(r);
        r = p.mcpp({"test", "--list", "--target", std::string(web)});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(s::lacks(s::text(r), "window")) << "B: a test added for native targets is listed for the Web: " << s::text(r);
        // C
        p.write("mcpp.toml", manifest("bytarget") + "\n[target.'cfg(kernel-abi = \"openkal\")'.test]\n"
                                                    "discover = [\"!tests/native/**\"]\n");
        r = p.mcpp({"test", "--list"});
        expect(fatal(!e2e::succeeded(r))) << "C: a layer predicate selected tests: " << s::text(r);
        expect(e2e::contains(s::text(r), "names a target-side layer")) << s::text(r);
        // D
        p.write("mcpp.toml", manifest("bytarget") + "\n[target.'cfg(os = \"emscripten\")'.test]\n"
                                                    "programs = \"directory\"\n");
        r = p.mcpp({"test"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(e2e::contains(s::text(r), "contributes test programs only: discover")) << s::text(r);
    };

    "1099 a package's tests are built with the features [test] features names"_test = [] {
        // mcpp#941: the tests are a consumer of their package, and `[test] features` is that
        // consumer's request. A: a test of a module behind a non-default feature fails without
        // it; B: with it the test builds and passes, and an unchanged second run is the recorded
        // one; C: `mcpp build` builds the package's defaults; D: a feature the package does not
        // declare is refused.
        s::project p{{"mcpp.toml", featured("tf", "")},
                     {"src/tf.cppm", "export module tf;\nexport int tf_v() { return 1; }\n"},
                     {"src/extra/extra.cppm", "export module tf.extra;\nexport int extra_v() { return 7; }\n"},
                     {"tests/extra.cpp", "import tf.extra;\nint main() { return extra_v() == 7 ? 0 : 1; }\n"}};
        // A
        auto r = p.mcpp({"test"});
        expect(fatal(!e2e::succeeded(r))) << "A: a module behind an inactive feature was built: " << s::text(r);
        // B
        p.write("mcpp.toml", featured("tf", "\n[test]\nfeatures = [\"extra\"]\n"));
        r = p.mcpp({"test"});
        expect(fatal(e2e::succeeded(r))) << "B: " << s::text(r);
        expect(e2e::contains(s::text(r), "1 passed")) << s::text(r);
        auto nj = newest_graph(p.at("target"), "test.ninja");
        expect(fatal(nj && e2e::contains(s::read_file(*nj), "extra.cppm"))) << "B: the feature's source is not in the test graph";
        r = p.mcpp({"test", "--verbose"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(s::lacks(s::text(r), "no recorded test graph matches")) << "B: the unchanged run was planned again: " << s::text(r);
        // C
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "C: " << s::text(r);
        nj = newest_graph(p.at("target"), "build.ninja");
        expect(fatal(nj.has_value())) << "C: no build graph";
        expect(s::lacks(s::read_file(*nj), "extra.cppm")) << "C: `mcpp build` turned the tests' feature on";
        // D
        p.write("mcpp.toml", featured("tf", "\n[test]\nfeatures = [\"nope\"]\n"));
        r = p.mcpp({"test"});
        expect(fatal(!e2e::succeeded(r))) << "D: an undeclared feature was accepted: " << s::text(r);
        expect(e2e::contains(s::text(r), "[test] features names 'nope', which [features] does not declare")) << s::text(r);
    };

    "1100 a member's test features are its own"_test = [] {
        // mcpp#941 in a workspace: each member's `[test] features` are requested on its own edge.
        // Member a's tests turn `extra` on; member b declares a feature of the same name whose
        // source does not compile, and `mcpp test --workspace` builds b without it.
        s::project ws{{"mcpp.toml", "[workspace]\nmembers = [\"a\", \"b\"]\n"},
                      {"a/mcpp.toml", featured("a", "\n[test]\nfeatures = [\"extra\"]\n")},
                      {"a/src/a.cppm", "export module a;\nexport int a_v() { return 1; }\n"},
                      {"a/src/extra/extra.cppm", "export module a.extra;\nexport int extra_v() { return 7; }\n"},
                      {"a/tests/extra.cpp", "import a.extra;\nint main() { return extra_v() == 7 ? 0 : 1; }\n"},
                      {"b/mcpp.toml", featured("b", "")},
                      {"b/src/b.cppm", "export module b;\nexport int b_v() { return 2; }\n"},
                      {"b/src/extra/extra.cppm", "export module b.extra;\nstatic_assert(false, \"b's extra was requested\");\n"},
                      {"b/tests/plain.cpp", "import b;\nint main() { return b_v() == 2 ? 0 : 1; }\n"}};
        auto r = ws.mcpp({"test", "--workspace"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(s::lacks(s::text(r), "b's extra was requested")) << "another member's feature of the same name was requested";
    };
}};

}  // namespace
