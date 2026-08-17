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
    // Not EXPECT_TRUE: on an optional that only asserts "the key was present",
    // which stays green when the value parses as false.
    EXPECT_EQ(r->provides[0].isInterface, std::optional<bool>{true});
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

// ─── is-interface decides whether a source may be published ────────────────
//
// `export module M:api;` and `module M:impl;` differ only in the keyword, and
// `mcpp pack` publishes the interface closure's SOURCE. The compiler is the one
// participant here that actually parsed the declaration, so its answer — and
// its silence — both have to survive the trip.

constexpr const char* kImplementationPartition = R"({
"rules": [
{
"primary-output": "/tmp/secret.o",
"provides": [{"logical-name": "mathkit:secret", "is-interface": false}],
"requires": []
}
]
})";

constexpr const char* kNoIsInterfaceKey = R"({
"rules": [
{
"primary-output": "/tmp/quiet.o",
"provides": [{"logical-name": "mathkit:quiet"}],
"requires": []
}
]
})";

TEST(P1689Parse, ImplementationPartitionIsNotAnInterface) {
    auto r = parse_ddi(kImplementationPartition);
    ASSERT_TRUE(r) << r.error();
    ASSERT_EQ(r->provides.size(), 1u);
    EXPECT_EQ(r->provides[0].logicalName, "mathkit:secret");
    EXPECT_EQ(r->provides[0].isInterface, std::optional<bool>{false});
}

TEST(P1689Parse, AnAbsentIsInterfaceKeyStaysAbsent) {
    // P1689 makes the key optional, so absence has to reach the packer as
    // "nobody said" rather than as either answer. It used to arrive as `false`
    // by struct default — which is the same value as an explicit
    // implementation partition, i.e. the two became indistinguishable.
    auto r = parse_ddi(kNoIsInterfaceKey);
    ASSERT_TRUE(r) << r.error();
    ASSERT_EQ(r->provides.size(), 1u);
    EXPECT_EQ(r->provides[0].logicalName, "mathkit:quiet");
    EXPECT_FALSE(r->provides[0].isInterface.has_value());
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
