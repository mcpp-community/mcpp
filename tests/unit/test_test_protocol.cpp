#include <gtest/gtest.h>

import std;
import mcpp.build.test_protocol;

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

TEST(TestProtocol, TheMarkerStatesTheVersion) {
    EXPECT_EQ(tp::marker_version(write_file("a", std::string("\x7f" "ELF....mtest-protocol=1\0rest", 30))), 1);
    EXPECT_EQ(tp::marker_version(write_file("b", "no marker here")), 0);
    // The version is followed by a byte that is not a digit; a marker with no
    // version is none.
    EXPECT_EQ(tp::marker_version(write_file("c", "mtest-protocol=x mtest-protocol=12;")), 12);
    EXPECT_EQ(tp::marker_version("/nonexistent/program"), 0);
}

TEST(TestProtocol, TheMarkerAcrossABlockBoundary) {
    std::string bytes((1 << 20) - 5, 'a');
    bytes += "mtest-protocol=1;";
    EXPECT_EQ(tp::marker_version(write_file("d", bytes)), 1);
}

TEST(TestProtocol, AReportGivesEachCaseAndItsFailures) {
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
    EXPECT_EQ(r.level, 3);
    EXPECT_EQ(r.framework, "f");
    EXPECT_TRUE(r.done);
    ASSERT_EQ(r.cases.size(), 2u);
    EXPECT_EQ(r.cases[0].status, "pass");
    EXPECT_DOUBLE_EQ(r.cases[0].ms, 1.5);
    ASSERT_EQ(r.cases[1].failures.size(), 1u);
    EXPECT_EQ(r.cases[1].failures[0].line, 9);
    EXPECT_EQ(r.cases[1].failures[0].actual, "2");
    EXPECT_EQ(r.missing, std::vector<std::string>{"c"});
    EXPECT_TRUE(r.failed());
    EXPECT_EQ(r.counts().at("fail"), 1);
}

TEST(TestProtocol, ACaseACrashLeftOpenFailed) {
    auto r = tp::read_report(write_file("crash.ndjson",
        "{\"e\":\"program\",\"protocol\":1,\"level\":1}\n"
        "{\"e\":\"begin\",\"id\":\"x\",\"t\":0}\n"), 139);
    EXPECT_FALSE(r.done);
    ASSERT_EQ(r.cases.size(), 1u);
    EXPECT_EQ(r.cases[0].status, "fail");
    EXPECT_NE(r.cases[0].reason.find("status 139"), std::string::npos);
}

TEST(TestProtocol, APatternMatchesIdentifiers) {
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
        EXPECT_EQ(tp::matches(row.id, row.pattern), row.matches) << row.id << " ~ " << row.pattern;
}

TEST(TestProtocol, TheVariablesOfARun) {
    auto env = tp::variables("run", "/r", std::filesystem::path("/s"), "/t", 7);
    std::map<std::string, std::string> m(env.begin(), env.end());
    EXPECT_EQ(m.at("MTEST_PROTOCOL"), "1");
    EXPECT_EQ(m.at("MTEST_MODE"), "run");
    EXPECT_EQ(m.at("MTEST_SELECT"), std::filesystem::path("/s").string());
    EXPECT_EQ(m.at("MTEST_SEED"), "7");
    auto list = tp::variables("list", "/r", std::nullopt, "/t", std::nullopt);
    EXPECT_TRUE(std::ranges::none_of(list, [](auto& kv) { return kv.first == "MTEST_SELECT" || kv.first == "MTEST_SEED"; }));
}

}  // namespace
