#include <gtest/gtest.h>

import std;
import mcpp.modgraph.p1689;

using namespace mcpp::modgraph::p1689;

namespace {

constexpr const char* kSimpleProvider = R"({
"rules": [
{
"primary-output": "/tmp/foo.o",
"provides": [
{
"logical-name": "foo",
"is-interface": true
}
],
"requires": [
{
"logical-name": "std"
}
,
{
"logical-name": "foo:impl"
}
]
}
],
"version": 0,
"revision": 0
}
)";

constexpr const char* kPureConsumer = R"({
"rules": [
{
"primary-output": "/tmp/main.o",
"provides": [],
"requires": [
{ "logical-name": "std" },
{ "logical-name": "myapp.lib" }
]
}
]
})";

constexpr const char* kEmptyRequires = R"({
"rules": [
{
"primary-output": "/tmp/lone.o",
"provides": [{"logical-name": "lone", "is-interface": true}],
"requires": []
}
]
})";

} // namespace

TEST(P1689Parse, SimpleProvider) {
    auto r = parse_ddi(kSimpleProvider);
    ASSERT_TRUE(r) << r.error();
    EXPECT_EQ(r->primaryOutput, "/tmp/foo.o");
    ASSERT_EQ(r->provides.size(), 1u);
    EXPECT_EQ(r->provides[0].logicalName, "foo");
    EXPECT_TRUE(r->provides[0].isInterface);
    ASSERT_EQ(r->requires_.size(), 2u);
    EXPECT_EQ(r->requires_[0], "std");
    EXPECT_EQ(r->requires_[1], "foo:impl");
}

TEST(P1689Parse, PureConsumer) {
    auto r = parse_ddi(kPureConsumer);
    ASSERT_TRUE(r) << r.error();
    EXPECT_TRUE(r->provides.empty());
    ASSERT_EQ(r->requires_.size(), 2u);
    EXPECT_EQ(r->requires_[0], "std");
    EXPECT_EQ(r->requires_[1], "myapp.lib");
}

TEST(P1689Parse, EmptyRequires) {
    auto r = parse_ddi(kEmptyRequires);
    ASSERT_TRUE(r) << r.error();
    ASSERT_EQ(r->provides.size(), 1u);
    EXPECT_EQ(r->provides[0].logicalName, "lone");
    EXPECT_TRUE(r->requires_.empty());
}

TEST(P1689Parse, RejectsNonObject) {
    auto r = parse_ddi("[]");
    EXPECT_FALSE(r);
    EXPECT_NE(r.error().find("top-level"), std::string::npos);
}

TEST(P1689Parse, RejectsMissingRules) {
    auto r = parse_ddi(R"({"version": 0})");
    EXPECT_FALSE(r);
    EXPECT_NE(r.error().find("rules"), std::string::npos);
}
