
import std;
import mcpplibs.mtest;
import mcpp.modgraph.p1689;

using namespace mcpplibs::mtest;

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

UTest simple_provider{"P1689Parse/SimpleProvider", [] {
    auto r = parse_ddi(kSimpleProvider);
    if (auto&& failed = expect(fatal(bool(r))); !failed) failed << r.error();
    expect(eq(r->primaryOutput, "/tmp/foo.o"));
    expect(fatal(eq(r->provides.size(), 1u)));
    expect(eq(r->provides[0].logicalName, "foo"));
    // Not EXPECT_TRUE: on an optional that only asserts "the key was present",
    // which stays green when the value parses as false.
    expect(eq(r->provides[0].isInterface, std::optional<bool>{true}));
    expect(fatal(eq(r->requires_.size(), 2u)));
    expect(eq(r->requires_[0], "std"));
    expect(eq(r->requires_[1], "foo:impl"));
}};

UTest pure_consumer{"P1689Parse/PureConsumer", [] {
    auto r = parse_ddi(kPureConsumer);
    if (auto&& failed = expect(fatal(bool(r))); !failed) failed << r.error();
    expect(r->provides.empty());
    expect(fatal(eq(r->requires_.size(), 2u)));
    expect(eq(r->requires_[0], "std"));
    expect(eq(r->requires_[1], "myapp.lib"));
}};

UTest empty_requires{"P1689Parse/EmptyRequires", [] {
    auto r = parse_ddi(kEmptyRequires);
    if (auto&& failed = expect(fatal(bool(r))); !failed) failed << r.error();
    expect(fatal(eq(r->provides.size(), 1u)));
    expect(eq(r->provides[0].logicalName, "lone"));
    expect(r->requires_.empty());
}};

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

UTest implementation_partition_is_not_an_interface{"P1689Parse/ImplementationPartitionIsNotAnInterface", [] {
    auto r = parse_ddi(kImplementationPartition);
    if (auto&& failed = expect(fatal(bool(r))); !failed) failed << r.error();
    expect(fatal(eq(r->provides.size(), 1u)));
    expect(eq(r->provides[0].logicalName, "mathkit:secret"));
    expect(eq(r->provides[0].isInterface, std::optional<bool>{false}));
}};

UTest an_absent_is_interface_key_stays_absent{"P1689Parse/AnAbsentIsInterfaceKeyStaysAbsent", [] {
    // P1689 makes the key optional, so absence has to reach the packer as
    // "nobody said" rather than as either answer. It used to arrive as `false`
    // by struct default — which is the same value as an explicit
    // implementation partition, i.e. the two became indistinguishable.
    auto r = parse_ddi(kNoIsInterfaceKey);
    if (auto&& failed = expect(fatal(bool(r))); !failed) failed << r.error();
    expect(fatal(eq(r->provides.size(), 1u)));
    expect(eq(r->provides[0].logicalName, "mathkit:quiet"));
    expect(!(r->provides[0].isInterface.has_value()));
}};

UTest rejects_non_object{"P1689Parse/RejectsNonObject", [] {
    auto r = parse_ddi("[]");
    expect(!r);
    expect(ne(r.error().find("top-level"), std::string::npos));
}};

UTest rejects_missing_rules{"P1689Parse/RejectsMissingRules", [] {
    auto r = parse_ddi(R"({"version": 0})");
    expect(!r);
    expect(ne(r.error().find("rules"), std::string::npos));
}};
