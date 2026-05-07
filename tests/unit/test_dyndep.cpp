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
