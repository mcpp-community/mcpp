#include <gtest/gtest.h>

import std;
import mcpp.dyndep;

using namespace mcpp::dyndep;

TEST(Dyndep, BmiBasenameSanitizes) {
    EXPECT_EQ(bmi_basename("foo"),         "foo.gcm");
    EXPECT_EQ(bmi_basename("foo.bar"),     "foo.bar.gcm");
    EXPECT_EQ(bmi_basename("foo:impl"),    "foo-impl.gcm");
    EXPECT_EQ(bmi_basename("a.b:c"),       "a.b-c.gcm");
}

TEST(Dyndep, ParseDdiPickProvidesRequires) {
    constexpr const char* body = R"({
"rules": [
{
"primary-output": "obj/foo.m.o",
"provides": [{"logical-name": "foo", "is-interface": true}],
"requires": [{"logical-name": "std"}, {"logical-name": "foo:impl"}]
}
]
})";
    auto u = parse_ddi(body);
    ASSERT_TRUE(u) << u.error();
    EXPECT_EQ(u->primaryOutput.string(), "obj/foo.m.o");
    ASSERT_EQ(u->provides.size(), 1u);
    EXPECT_EQ(u->provides[0], "foo");
    ASSERT_EQ(u->requires_.size(), 2u);
    EXPECT_EQ(u->requires_[0], "std");
    EXPECT_EQ(u->requires_[1], "foo:impl");
}

TEST(Dyndep, EmitDyndepBasic) {
    std::vector<UnitInfo> units = {
        { "obj/lib-greet.m.o", {"myapp.lib:greet"}, {"std"} },
        { "obj/lib.m.o",        {"myapp.lib"},        {"myapp.lib:greet", "std"} },
        { "obj/main.o",         {},                    {"myapp.lib", "std"} },
    };
    auto body = emit_dyndep(units, /*stdImports=*/{});

    // Header
    EXPECT_NE(body.find("ninja_dyndep_version = 1"), std::string::npos);
    // Entry per unit, with implicit inputs only (no implicit outputs — those
    // are declared statically in build.ninja).
    EXPECT_NE(body.find(
        "build obj/lib-greet.m.o: dyndep | gcm.cache/std.gcm"), std::string::npos);
    EXPECT_NE(body.find(
        "build obj/lib.m.o: dyndep | gcm.cache/myapp.lib-greet.gcm gcm.cache/std.gcm"),
        std::string::npos);
    EXPECT_NE(body.find(
        "build obj/main.o: dyndep | gcm.cache/myapp.lib.gcm gcm.cache/std.gcm"),
        std::string::npos);
    // Each entry must end with restat = 1.
    int restat_count = 0;
    for (std::size_t pos = 0; (pos = body.find("restat = 1", pos)) != std::string::npos; ++pos)
        ++restat_count;
    EXPECT_EQ(restat_count, 3);
}

TEST(Dyndep, EmitDyndepNoRequires) {
    std::vector<UnitInfo> units = {
        { "obj/leaf.m.o", {"leaf"}, {} },
    };
    auto body = emit_dyndep(units, {});
    // No '|' implicit-deps section when requires is empty.
    EXPECT_NE(body.find("build obj/leaf.m.o: dyndep\n"), std::string::npos);
}

TEST(Dyndep, EmitDyndepSelfProvideFiltered) {
    // Defensive: even if a unit lists its own provides in requires
    // (shouldn't happen but be robust), we don't emit a self-loop.
    std::vector<UnitInfo> units = {
        { "obj/foo.m.o", {"foo"}, {"foo", "std"} },
    };
    auto body = emit_dyndep(units, {});
    EXPECT_NE(body.find(
        "build obj/foo.m.o: dyndep | gcm.cache/std.gcm\n"), std::string::npos);
    EXPECT_EQ(body.find("gcm.cache/foo.gcm"), std::string::npos);
}

// Two-phase (clang): one source, two edges — a BMI edge and an object edge —
// and BOTH parse the same imports, so both need the same implicit inputs.
// P1689 only knows about the object (`primary-output` comes from the scanned
// command's `-o`), and an edge with no record is not a warning: ninja refuses
// the whole graph with "'…pcm' not mentioned in its dyndep file", naming the
// edge rather than the missing record.
TEST(Dyndep, SplitModuleEmitsARecordForBothEdges) {
    std::vector<UnitInfo> units = {
        { "obj/lib.m.o", {"myapp.lib"}, {"std"} },
    };
    DyndepOptions opts;
    opts.bmiDir = "pcm.cache";
    opts.bmiExt = ".pcm";
    opts.splitModuleEdges = true;
    auto body = emit_dyndep(units, {}, opts);

    EXPECT_NE(body.find("build pcm.cache/myapp.lib.pcm: dyndep | pcm.cache/std.pcm\n"),
              std::string::npos) << body;
    EXPECT_NE(body.find("build obj/lib.m.o: dyndep | pcm.cache/std.pcm\n"),
              std::string::npos) << body;
}

// A unit that provides nothing is NOT split — there is only one edge, so a
// second record would name something nobody declared. Same failure text as the
// missing-record case, from the opposite mistake.
TEST(Dyndep, SplitModuleLeavesNonProvidingUnitsAlone) {
    std::vector<UnitInfo> units = {
        { "obj/main.o", {}, {"myapp.lib"} },
    };
    DyndepOptions opts;
    opts.splitModuleEdges = true;
    auto body = emit_dyndep(units, {}, opts);

    // Count the records, not the lines: one record is `build …` plus its
    // `restat = 1`, so a line count says nothing about how many there are.
    std::size_t records = 0;
    for (auto pos = body.find("build "); pos != std::string::npos;
         pos = body.find("build ", pos + 1))
        ++records;
    EXPECT_EQ(records, 1u) << body;
    EXPECT_NE(body.find("build obj/main.o: dyndep | gcm.cache/myapp.lib.gcm\n"),
              std::string::npos) << body;
}

// The single-unit path is what ninja actually runs (one .ddi → one .dd), and it
// used to be a separate hand-written copy of the loop above. Pinned together so
// a change to one shape cannot silently leave the other behind.
TEST(Dyndep, SplitModuleSingleFileMatchesTheBatchShape) {
    auto tmp = std::filesystem::temp_directory_path()
             / std::format("mcpp_dyndep_split_{}", std::random_device{}());
    std::filesystem::create_directories(tmp);
    auto p = tmp / "lib.ddi";
    std::ofstream(p) << R"({
"rules":[{"primary-output":"obj/lib.m.o","provides":[{"logical-name":"myapp.lib","is-interface":true}],"requires":[{"logical-name":"std"}]}]
})";
    DyndepOptions opts;
    opts.bmiDir = "pcm.cache";
    opts.bmiExt = ".pcm";
    opts.splitModuleEdges = true;

    auto single = emit_dyndep_single(p, opts);
    ASSERT_TRUE(single) << single.error();

    std::vector<UnitInfo> units = { { "obj/lib.m.o", {"myapp.lib"}, {"std"} } };
    EXPECT_EQ(*single, emit_dyndep(units, {}, opts));

    std::filesystem::remove_all(tmp);
}

TEST(Dyndep, EmitDyndepFromFiles) {
    auto tmp = std::filesystem::temp_directory_path()
             / std::format("mcpp_dyndep_test_{}", std::random_device{}());
    std::filesystem::create_directories(tmp);
    auto cleanup = std::unique_ptr<int, void(*)(int*)>(new int(0),
        [](int*){ /* noop */ });

    auto a = tmp / "a.ddi";
    std::ofstream(a) << R"({
"rules":[{"primary-output":"obj/a.m.o","provides":[{"logical-name":"a","is-interface":true}],"requires":[]}]
})";
    auto b = tmp / "b.ddi";
    std::ofstream(b) << R"({
"rules":[{"primary-output":"obj/b.m.o","provides":[{"logical-name":"b","is-interface":true}],"requires":[{"logical-name":"a"}]}]
})";

    auto body = emit_dyndep_from_files({a, b}, {});
    ASSERT_TRUE(body) << body.error();
    EXPECT_NE(body->find("build obj/a.m.o: dyndep\n"), std::string::npos);
    EXPECT_NE(body->find(
        "build obj/b.m.o: dyndep | gcm.cache/a.gcm\n"), std::string::npos);

    std::filesystem::remove_all(tmp);
}

// ── plan-vs-ddi reconciliation (scan_overrides auditor) ──

TEST(VerifyUnitExpectations, MatchPasses) {
    mcpp::dyndep::UnitInfo u;
    u.primaryOutput = "obj/fmt.o";
    u.provides = {"fmt"};
    u.requires_ = {"std"};
    auto err = mcpp::dyndep::verify_unit_expectations(u, "fmt", {"std"});
    EXPECT_FALSE(err.has_value()) << *err;
}

TEST(VerifyUnitExpectations, DivergenceReportsBothSides) {
    mcpp::dyndep::UnitInfo u;
    u.primaryOutput = "obj/fmt.o";
    u.provides = {"fmt"};
    u.requires_ = {"std"};
    auto err = mcpp::dyndep::verify_unit_expectations(u, "fmt", {});
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("divergence"), std::string::npos);
    EXPECT_NE(err->find("planned"), std::string::npos);
    EXPECT_NE(err->find("std"), std::string::npos);
}

TEST(VerifyUnitExpectations, ExpectNoneMatchesEmptyUnit) {
    mcpp::dyndep::UnitInfo u;
    u.primaryOutput = "obj/plain.o";
    auto err = mcpp::dyndep::verify_unit_expectations(u, std::nullopt, {});
    EXPECT_FALSE(err.has_value());
}
