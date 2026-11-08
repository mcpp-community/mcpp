
import std;
import mcpplibs.mtest;
import mcpp.pm.dependency_selector;

using namespace mcpplibs::mtest;

// SUBSYSTEM-LEVEL. A dependency selector is part of what a manifest MEANS, so
// it lives here rather than in a package of its own -- and it is testable with
// nothing else present, which is the check that the layering is real.

UTest a_dotted_selector_splits_into_its_segments{"SelectorGrammar/ADottedSelectorSplitsIntoItsSegments", [] {
    auto s = mcpp::pm::split_dependency_selector("mcpplibs.capi.lua");
    expect(fatal(eq(s.size(), 3u)));
    expect(eq(s[0], "mcpplibs"));
    expect(eq(s[1], "capi"));
    expect(eq(s[2], "lua"));
}};

UTest a_bare_name_is_one_segment_and_not_an_error{"SelectorGrammar/ABareNameIsOneSegmentAndNotAnError", [] {
    // The unqualified spelling is the common case and resolves through the
    // namespace ladder later. Treating it as malformed here would move that
    // decision into the grammar, where the ladder is not visible.
    auto s = mcpp::pm::split_dependency_selector("cmdline");
    expect(fatal(eq(s.size(), 1u)));
    expect(eq(s[0], "cmdline"));
}};

UTest joining_is_the_inverse_of_splitting{"SelectorGrammar/JoiningIsTheInverseOfSplitting", [] {
    const std::string original = "mcpplibs.capi.lua";
    auto s = mcpp::pm::split_dependency_selector(original);
    expect(eq(mcpp::pm::join_dependency_segments(s, 0, s.size()), original));
}};
