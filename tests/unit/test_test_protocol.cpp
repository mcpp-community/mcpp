
import std;
import mcpplibs.mtest;
import mcpp.build.test_protocol;

using namespace mcpplibs::mtest;

// The runner's side of the mtest test protocol, version 1 (docs/08 "Cases"):
// the marker a program's file carries (P2.3), the report it writes (P5), a
// crash's open cases (P5.4) and the patterns of `--case`.

namespace tp = mcpp::build::test_protocol;

namespace {

std::filesystem::path write_file(std::string_view name, std::string_view content) {
    auto dir = std::filesystem::temp_directory_path() / "mcpp-unit-test-protocol";
    std::filesystem::create_directories(dir);
    auto path = dir / name;
    std::ofstream(path, std::ios::binary) << content;
    return path;
}

UTest the_marker_states_the_version{"TestProtocol/TheMarkerStatesTheVersion", [] {
    expect(eq(tp::marker_version(write_file("a", std::string("\x7f" "ELF....mtest-protocol=1\0rest", 30))), 1));
    expect(eq(tp::marker_version(write_file("b", "no marker here")), 0));
    // The version is followed by a byte that is not a digit; a marker with no
    // version is none.
    expect(eq(tp::marker_version(write_file("c", "mtest-protocol=x mtest-protocol=12;")), 12));
    expect(eq(tp::marker_version("/nonexistent/program"), 0));
}};

UTest the_marker_across_a_block_boundary{"TestProtocol/TheMarkerAcrossABlockBoundary", [] {
    std::string bytes((1 << 20) - 5, 'a');
    bytes += "mtest-protocol=1;";
    expect(eq(tp::marker_version(write_file("d", bytes)), 1));
}};

UTest a_report_gives_each_case_and_its_failures{"TestProtocol/AReportGivesEachCaseAndItsFailures", [] {
    auto r = tp::read_report(write_file("r.ndjson",
        "{\"e\":\"program\",\"protocol\":1,\"level\":3,\"framework\":\"f\"}\n"
        "not json\n"
        "{\"e\":\"begin\",\"id\":\"a\",\"t\":0}\n"
        "{\"e\":\"end\",\"id\":\"a\",\"status\":\"pass\",\"ms\":1.5}\n"
        "{\"e\":\"begin\",\"id\":\"b\",\"t\":2}\n"
        "{\"e\":\"failure\",\"id\":\"b\",\"file\":\"t.cpp\",\"line\":9,\"message\":\"m\",\"expected\":\"1\",\"actual\":\"2\"}\n"
        "{\"e\":\"end\",\"id\":\"b\",\"status\":\"fail\",\"ms\":3}\n"
        "{\"e\":\"future\",\"id\":\"b\"}\n"
        "{\"e\":\"missing\",\"id\":\"c\"}\n"
        "{\"e\":\"done\",\"status\":\"fail\",\"counts\":{}}\n"), 1);
    expect(eq(r.level, 3));
    expect(eq(r.framework, "f"));
    expect(bool(r.done));
    expect(fatal(eq(r.cases.size(), 2u)));
    expect(eq(r.cases[0].status, "pass"));
    expect(near(r.cases[0].ms, 1.5, 1e-9));
    expect(fatal(eq(r.cases[1].failures.size(), 1u)));
    expect(eq(r.cases[1].failures[0].line, 9));
    expect(eq(r.cases[1].failures[0].actual, "2"));
    expect(eq(r.missing, std::vector<std::string>{"c"}));
    expect(bool(r.failed()));
    expect(eq(r.counts().at("fail"), 1));
}};

UTest a_case_a_crash_left_open_failed{"TestProtocol/ACaseACrashLeftOpenFailed", [] {
    auto r = tp::read_report(write_file("crash.ndjson",
        "{\"e\":\"program\",\"protocol\":1,\"level\":1}\n"
        "{\"e\":\"begin\",\"id\":\"x\",\"t\":0}\n"), 139);
    expect(!r.done);
    expect(fatal(eq(r.cases.size(), 1u)));
    expect(eq(r.cases[0].status, "fail"));
    expect(ne(r.cases[0].reason.find("status 139"), std::string::npos));
}};

UTest a_pattern_matches_identifiers{"TestProtocol/APatternMatchesIdentifiers", [] {
    struct Row { std::string_view id, pattern; bool matches; };
    constexpr Row rows[] = {
        {"fs/read", "fs/read", true},
        {"fs/read", "fs", true},           // a prefix of whole names
        {"fsx/read", "fs", false},
        {"fs/read", "fs/*", true},
        {"fs/read", "*read", true},
        {"fs/read", "fs/rea?", true},
        {"fs/read", "fs/?", false},
        {"fs/read", "net/*", false},
        {"a b/c", "a b/*", true},
    };
    for (const auto& row : rows)
        expect(eq(tp::matches(row.id, row.pattern), row.matches)) << row.id << " ~ " << row.pattern;
}};

UTest a_selection_reaches_the_cases_a_listed_case_declares{"TestProtocol/ASelectionReachesTheCasesAListedCaseDeclares", [] {
    // P3.4: `fields` is listed; the cases its body declares are reported when it runs.
    const std::vector<tp::Case> listed = {{"fields"}, {"empty"}};
    using V = std::vector<std::string>;
    expect(eq(tp::selection(listed, {"fields"}), V{"fields"}));
    expect(eq(tp::selection(listed, {"fields/one"}), V{"fields/one"}));      // passed as it is
    expect(eq(tp::selection(listed, {"fields/o*"}), V{"fields"}));           // the listed case, all of it
    expect(eq(tp::selection(listed, {"e*", "fields/one", "fields"}), (V{"empty", "fields/one", "fields"})));
    expect(eq(tp::selection(listed, {"other/one"}), V{}));
}};

UTest the_variables_of_a_run{"TestProtocol/TheVariablesOfARun", [] {
    auto env = tp::variables("run", "/r", std::filesystem::path("/s"), "/t", 7);
    std::map<std::string, std::string> m(env.begin(), env.end());
    expect(eq(m.at("MTEST_PROTOCOL"), "1"));
    expect(eq(m.at("MTEST_MODE"), "run"));
    expect(eq(m.at("MTEST_SELECT"), std::filesystem::path("/s").string()));
    expect(eq(m.at("MTEST_SEED"), "7"));
    auto list = tp::variables("list", "/r", std::nullopt, "/t", std::nullopt);
    expect(std::ranges::none_of(list, [](auto& kv) { return kv.first == "MTEST_SELECT" || kv.first == "MTEST_SEED"; }));
}};

}  // namespace
