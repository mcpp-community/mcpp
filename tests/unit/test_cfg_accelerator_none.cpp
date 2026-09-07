#include <gtest/gtest.h>

import std;
import mcpp.build.prepare_inputs;

namespace cfgpred = mcpp::build::cfgpred;

namespace {

cfgpred::Ctx with(std::vector<std::string> accelerators) {
    auto c = cfgpred::context_for("x86_64-linux-gnu");
    c.layersKnown  = true;
    c.accelerators = std::move(accelerators);
    return c;
}

bool m(std::string_view predicate, const cfgpred::Ctx& c) {
    return cfgpred::matches(std::string(predicate), c);
}

} // namespace

// `accelerator = "none"` is the empty set, and the reason it has to exist is
// that `accelerator`'s vocabulary is OPEN: docs/20 states a fifth backend is a
// package rather than an engine change. A fallback written by enumeration
// therefore changes meaning the day a fifth backend exists, silently.

TEST(CfgAcceleratorNone, NoneIsTheEmptySet) {
    EXPECT_TRUE (m(R"(cfg(accelerator = "none"))", with({})));
    EXPECT_FALSE(m(R"(cfg(accelerator = "none"))", with({"cuda"})));
    EXPECT_FALSE(m(R"(cfg(accelerator = "none"))", with({"cuda", "vulkan"})));

    // The negation is the other half a seam needs: "some device backend".
    EXPECT_FALSE(m(R"(cfg(not(accelerator = "none")))", with({})));
    EXPECT_TRUE (m(R"(cfg(not(accelerator = "none")))", with({"vulkan"})));
}

TEST(CfgAcceleratorNone, NoneDoesNotDisturbMembership) {
    // `none` must not become a member of the set it describes the emptiness
    // of, or `not(accelerator = "cuda")` would start answering for it.
    EXPECT_TRUE (m(R"(cfg(accelerator = "cuda"))",   with({"cuda"})));
    EXPECT_FALSE(m(R"(cfg(accelerator = "vulkan"))", with({"cuda"})));
    // A build that named a backend literally spelled "none" is not a case this
    // engine has to serve; what matters is that the empty set stays the only
    // thing `none` reports, which the first test pins.
}

// THE REASON THIS IS NOT A COSMETIC CHANGE.
//
// The two spellings agree today and part company on the day a backend is
// added. That is the whole point, so the test simulates the addition rather
// than describing it: `enumerated` is what a project wrote against the
// two-backend world, `none` is what it should have written.
TEST(CfgAcceleratorNone, EnumerationRotsAndNoneDoesNot) {
    constexpr auto enumerated =
        R"(cfg(not(any(accelerator = "cuda", accelerator = "vulkan"))))";
    constexpr auto stable = R"(cfg(accelerator = "none"))";

    // The world the fallback was written in: the two agree.
    EXPECT_EQ(m(enumerated, with({})),       m(stable, with({})));
    EXPECT_EQ(m(enumerated, with({"cuda"})), m(stable, with({"cuda"})));

    // A fifth backend arrives. The enumeration now claims "no accelerator" for
    // a build that named one -- the CPU fallback would compile beside the
    // device implementation -- while `none` is unchanged.
    auto fifth = with({"ascend"});
    EXPECT_TRUE (m(enumerated, fifth)) << "the enumeration is expected to rot";
    EXPECT_FALSE(m(stable,     fifth)) << "`none` must not rot";
}
