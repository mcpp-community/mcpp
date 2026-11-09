
import std;
import mcpplibs.mtest;
import mcpp.build.version_floor;

using namespace mcpplibs::mtest;

using mcpp::build::parse_version_floor;
using mcpp::build::parse_version_fact;
using mcpp::build::version_at_least;

// ── The two spellings ──────────────────────────────────────────────────────

UTest reads_a_floor{"VersionFloor/ReadsAFloor", [] {
    auto f = parse_version_floor("cuda.driver >= 12.0");
    expect(fatal(bool(f.valid())));
    expect(eq(f.name, "cuda.driver"));
    expect(eq(f.version, "12.0"));
}};

UTest reads_a_fact{"VersionFloor/ReadsAFact", [] {
    auto f = parse_version_fact("cuda.driver=12.4");
    expect(fatal(bool(f.valid())));
    expect(eq(f.name, "cuda.driver"));
    expect(eq(f.version, "12.4"));
}};

UTest a_floor_is_not_read_as_a_fact{"VersionFloor/AFloorIsNotReadAsAFact", [] {
    // Both spellings live in string lists, and `>=` contains `=`. Reading one
    // as the other would turn "needs at least 12.0" into "this machine has
    // 12.0" — a requirement silently becoming its own satisfaction.
    expect(!(parse_version_fact("cuda.driver >= 12.0").valid()));
}};

UTest anything_else_is_not_a_floor{"VersionFloor/AnythingElseIsNotAFloor", [] {
    expect(!(parse_version_floor("cuda.driver").valid()));
    expect(!(parse_version_floor("cuda.driver >= ").valid()));
    expect(!(parse_version_floor(" >= 12.0").valid()));
    // Not a version: refused rather than read as `12`.
    expect(!(parse_version_floor("cuda.driver >= 12.").valid()));
    expect(!(parse_version_floor("cuda.driver >= twelve").valid()));
}};

// ── Comparison ─────────────────────────────────────────────────────────────

UTest compares_componentwise{"VersionFloor/ComparesComponentwise", [] {
    expect(eq(version_at_least("12.4",  "12.0"), std::optional{true}));
    expect(eq(version_at_least("12.0",  "12.4"), std::optional{false}));
    expect(eq(version_at_least("13.0",  "12.9"), std::optional{true}));
    // Not lexicographic: 12.10 is above 12.9.
    expect(eq(version_at_least("12.10", "12.9"), std::optional{true}));
}};

UTest a_missing_component_is_zero{"VersionFloor/AMissingComponentIsZero", [] {
    expect(eq(version_at_least("12",   "12.0"), std::optional{true}));
    expect(eq(version_at_least("12.0", "12"), std::optional{true}));
    expect(eq(version_at_least("12",   "12.1"), std::optional{false}));
}};

UTest either_side_unreadable_is_no_claim{"VersionFloor/EitherSideUnreadableIsNoClaim", [] {
    // The whole reason this mechanism exists is that a wrong answer is worse
    // than no answer. A version that cannot be read yields neither.
    expect(!(version_at_least("unknown", "12.0").has_value()));
    expect(!(version_at_least("12.0", "").has_value()));
}};

UTest no_vendor_vocabulary_is_required{"VersionFloor/NoVendorVocabularyIsRequired", [] {
    // The name is data. A backend this engine has never heard of compares the
    // same way, which is what keeps the comparison out of the vendors' reach.
    auto f = parse_version_floor("some.future.thing >= 4.2.1");
    expect(fatal(bool(f.valid())));
    expect(eq(f.name, "some.future.thing"));
    expect(eq(version_at_least("4.3", f.version), std::optional{true}));
    expect(eq(version_at_least("4.2", f.version), std::optional{false}));
}};
