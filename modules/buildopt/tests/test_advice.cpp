// What a build says of a module that is one file: where the rebuild started,
// how many units followed, and when nothing is said.

import std;
import mcpplibs.mtest;
import mcpp.buildopt.advice;
import mcpp.buildopt.registry;

using namespace mcpplibs::mtest;
namespace advice = mcpp::buildopt::advice;

namespace {
// `core` imported by five modules, each imported by `main`.
std::vector<advice::Unit> graph(bool withImplementation = false) {
    std::vector<advice::Unit> u{{"core.cppm", "core", {}, false}};
    std::vector<std::string> mids;
    for (int i = 0; i < 5; ++i) {
        const auto m = std::format("m{}", i);
        mids.push_back(m);
        u.push_back({m + ".cppm", m, {"core"}, false});
    }
    u.push_back({"main.cpp", "", mids, false});
    if (withImplementation) u.push_back({"core.cpp", "", {"core"}, true});
    return u;
}
} // namespace

UTest where_the_rebuild_started{"Advice/TheModuleTheRebuildStartedAtIsNamedWithItsImporters", [] {
    const auto found = advice::single_file_cascades(graph(), {"core.pcm", "m0.pcm", "m1.pcm"});
    expect(fatal(found.size() == 1u));
    expect(eq(found[0].module, std::string("core")));
    expect(eq(found[0].source, std::string("core.cppm")));
    expect(eq(found[0].importers, std::size_t{6}));   // five modules and main
    const auto line = advice::line(found[0]);
    expect(line.find("core.cppm is the whole of module core, which 6 units import") != std::string::npos) << line;
    expect(line.find("module :private;") != std::string::npos);
}};

UTest nothing_is_said{"Advice/NothingIsSaidOfAModuleWithAnImplementationUnitOrFewImporters", [] {
    // An implementation unit: the module is not one file. It is no importer either.
    expect(advice::single_file_cascades(graph(true), {"core.gcm"}).empty());
    // Few importers.
    expect(advice::single_file_cascades(graph(), {"m0.pcm"}).empty());
    // Nothing replaced.
    expect(advice::single_file_cascades(graph(), {}).empty());
    // A partition is not where a one-file module's advice applies.
    std::vector<advice::Unit> parts = graph();
    parts.push_back({"core-p.cppm", "core:p", {}, false});
    expect(advice::single_file_cascades(parts, {"core.pcm"}).empty());
}};

UTest said_once{"Advice/AnAdviceIsSaidOnceABuildDirectory", [] {
    const auto d = std::filesystem::temp_directory_path() / std::format("mcpp-advice-{}", std::random_device{}());
    std::filesystem::create_directories(d);
    expect(advice::first_time(d, "core"));
    expect(!advice::first_time(d, "core"));
    expect(advice::first_time(d, "other"));
    std::filesystem::remove_all(d);
}};

// WHAT A FAILED BUILD SAYS (SPEC-019 §7): the step that failed decides, and a
// failure the entries did not touch says nothing.
namespace {
bool says(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

advice::Applied applied() {
    advice::Applied a;
    a.fragments.push_back({"/p/src/c.cppm", {"/p/target/.buildopt/private-fragment/p/src/c.cppm",
                                              "/p/target/.buildopt/private-fragment/p/src/c.private.cpp"}});
    a.importers.push_back({"/p/src/b.cppm", {"c", "std"}});
    a.importers.push_back({"/p/src/main.cpp", {"b"}});
    a.fragmentCheck = "as-written";
    return a;
}
} // namespace

UTest tip_for_a_fragment{"Advice/ASourceBuiltAsTwoUnitsThatFailedNamesTheCheck", [] {
    // The step names the unit relative to its package root, or whole.
    for (auto const& source : {std::string("target/.buildopt/private-fragment/p/src/c.private.cpp"),
                               std::string("/p/target/.buildopt/private-fragment/p/src/c.cppm")}) {
        const auto tips = advice::suspects(applied(), {}, {{source}, false});
        expect(fatal(tips.size() == 1u)) << source;
        expect(eq(tips[0].domain, std::string("buildopt/private-fragment")));
        expect(says(tips[0].text, "/p/src/c.cppm was built as two units (private-fragment)")) << tips[0].text;
        expect(says(tips[0].text, "MCPP_PRIVATE_FRAGMENT=as-written mcpp build")) << tips[0].text;
        expect(says(tips[0].text, "[build] private_fragment = \"as-written\" keeps it")) << tips[0].text;
        expect(says(tips[0].text, "MCPP_BUILDOPT=off")) << tips[0].text;
    }
    // GCC cannot build the fragment as written: its check is the old rule.
    auto gcc = applied();
    gcc.fragmentCheck = "conservative";
    const auto tips = advice::suspects(gcc, {}, {{"/p/target/.buildopt/private-fragment/p/src/c.cppm"}, false});
    expect(fatal(tips.size() == 1u));
    expect(says(tips[0].text, "MCPP_PRIVATE_FRAGMENT=conservative mcpp build")) << tips[0].text;
    // A source the sidecar does not know (a sidecar from another run) gives the register's own value.
    advice::Applied bare;
    bare.fragments = applied().fragments;
    const auto fallback = advice::suspects(bare, {}, {{"/p/target/.buildopt/private-fragment/p/src/c.cppm"}, false});
    expect(fatal(fallback.size() == 1u));
    expect(says(fallback[0].text, "MCPP_PRIVATE_FRAGMENT=as-written")) << fallback[0].text;
}};

UTest tip_for_a_kept_bmi{"Advice/AUnitThatImportsAKeptBmiAndFailedNamesTheCheck", [] {
    const auto tips = advice::suspects(applied(), {"c.gcm"}, {{"src/b.cppm"}, false});
    expect(fatal(tips.size() == 1u));
    expect(eq(tips[0].domain, std::string("buildopt/bmi-cutoff")));
    expect(says(tips[0].text, "the BMI of module c was kept")) << tips[0].text;
    expect(says(tips[0].text, "MCPP_BMI_CUTOFF=exact mcpp build")) << tips[0].text;
    expect(says(tips[0].text, "[profile.<name>] bmi_cutoff = \"exact\" keeps it")) << tips[0].text;
    // A kept BMI under a directory, and a partition's name.
    auto a = applied();
    a.importers.push_back({"/p/src/d.cppm", {"c:part"}});
    expect(advice::suspects(a, {"pkg/c-part.gcm"}, {{"/p/src/d.cppm"}, false}).size() == 1u);
}};

UTest no_tip_for_other_failures{"Advice/AFailureTheEntriesDidNotTouchSaysNothing", [] {
    // R4: an unrelated file's compile error.
    expect(advice::suspects(applied(), {"c.gcm"}, {{"/p/src/unrelated.cpp"}, false}).empty());
    // A unit that imports no kept BMI (main imports b, whose BMI was replaced).
    expect(advice::suspects(applied(), {"c.gcm"}, {{"/p/src/main.cpp"}, false}).empty());
    // A kept BMI of a module the failed unit does not import.
    expect(advice::suspects(applied(), {"other.gcm"}, {{"/p/src/b.cppm"}, false}).empty());
    // Nothing was applied.
    expect(advice::suspects({}, {}, {{"/p/src/b.cppm"}, false}).empty());
    // A step that names no source.
    expect(advice::suspects(applied(), {"c.gcm"}, {{}, false}).empty());
    // A source whose name is only the tail of another's, not a path tail.
    expect(advice::suspects(applied(), {}, {{"rc.cppm"}, false}).empty());
}};

UTest tip_for_a_link{"Advice/ALinkThatFailedWithEitherEntryInTheGraphSaysSoWeakly", [] {
    const auto tips = advice::suspects(applied(), {"c.gcm"}, {{}, true});
    expect(fatal(tips.size() == 2u));
    expect(says(tips[0].text, "the link failed")) << tips[0].text;
    expect(says(tips[0].text, "cannot say whether that is the cause")) << tips[0].text;
    expect(says(tips[1].text, "MCPP_BMI_CUTOFF=exact")) << tips[1].text;
    // Neither in the graph, or a compile that failed: nothing.
    expect(advice::suspects({}, {}, {{}, true}).empty());
    // A compile failure that a linker's words follow is the compile's.
    expect(advice::suspects(applied(), {}, {{"/p/src/unrelated.cpp"}, false}).empty());
}};

UTest register_check{"Advice/EveryDefaultOnEntryWithAKeyHasAValueThatChecksIt", [] {
    namespace registry = mcpp::buildopt::registry;
    auto values_of = [](std::string_view values) {
        std::vector<std::string> out;
        for (std::size_t at = 0; at < values.size();) {
            auto end = values.find('|', at);
            std::string v(values.substr(at, end == std::string_view::npos ? end : end - at));
            while (!v.empty() && v.front() == ' ') v.erase(v.begin());
            while (!v.empty() && v.back() == ' ') v.pop_back();
            out.push_back(v);
            if (end == std::string_view::npos) break;
            at = end + 1;
        }
        return out;
    };
    for (auto const& e : registry::kEntries) {
        if (e.key.empty() || !e.byDefault) {
            expect(e.check.empty() && e.checkGcc.empty()) << e.id;
            continue;
        }
        const auto values = values_of(e.values);
        expect(!e.check.empty()) << e.id;
        expect(std::ranges::contains(values, std::string(e.check))) << e.id;
        // The check is never the default: it must turn the entry off.
        expect(values.front() != std::string(e.check)) << e.id;
        if (!e.checkGcc.empty()) {
            expect(std::ranges::contains(values, std::string(e.checkGcc))) << e.id;
            expect(values.front() != std::string(e.checkGcc)) << e.id;
        }
    }
    expect(registry::find(registry::kPrivateFragment)->checkGcc == "conservative");
}};

UTest failed_steps{"Advice/FailedOutputsAreReadFromNinjasOutput", [] {
    const std::string out =
        "[3/9] c++ x\nFAILED: obj/a.o obj/a.o.d\nc++ -c a.cpp\nerror: x\nFAILED: [code=1] pcm/b.gcm\n"
        "  FAILED: link/app\nninja: build stopped\n";
    const auto steps = advice::failed_outputs(out);
    expect(fatal(steps.size() == 3u));
    expect(steps[0].size() == 2u && steps[0][0] == "obj/a.o");
    expect(eq(steps[1][0], std::string("pcm/b.gcm")));
    expect(eq(steps[2][0], std::string("link/app")));
    expect(advice::failed_outputs("nothing failed\n").empty());
    expect(advice::linker_failed("ld: x.o: undefined reference to `f()'\n"));
    expect(advice::linker_failed("ld.lld: error: undefined symbol: f()\n"));
    expect(!advice::linker_failed("error: expected ';'\n"));
}};

UTest applied_roundtrip{"Advice/TheAppliedTableIsWrittenBesideTheGraphAndReadBack", [] {
    const auto d = std::filesystem::temp_directory_path() / std::format("mcpp-applied-{}", std::random_device{}());
    std::filesystem::create_directories(d);
    expect(advice::read_applied(d).fragments.empty());
    advice::write_applied(d, applied());
    const auto back = advice::read_applied(d);
    expect(fatal(back.fragments.size() == 1u && back.importers.size() == 2u));
    expect(eq(back.fragmentCheck, std::string("as-written")));
    expect(eq(back.fragments[0].written, std::string("/p/src/c.cppm")));
    expect(back.fragments[0].units.size() == 2u);
    expect(back.importers[0].imports.size() == 2u);
    // Written again with the same content: the file is left alone.
    const auto before = std::filesystem::last_write_time(d / advice::kAppliedFile);
    advice::write_applied(d, applied());
    expect(std::filesystem::last_write_time(d / advice::kAppliedFile) == before);
    // A graph without a fragment states no check.
    advice::write_applied(d, {});
    expect(advice::read_applied(d).fragmentCheck.empty());
    std::filesystem::remove_all(d);
}};
