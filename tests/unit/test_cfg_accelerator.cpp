#include <gtest/gtest.h>

import std;
import mcpp.build.prepare_inputs;

namespace cfgpred = mcpp::build::cfgpred;

namespace {

// A context with the triple coordinates filled and the target side resolved,
// which is the state the second merge pass runs in.
cfgpred::Ctx resolved(std::vector<std::string> accelerators) {
    auto c = cfgpred::context_for("x86_64-linux-gnu");
    c.layersKnown   = true;
    c.accelerators  = std::move(accelerators);
    return c;
}

bool m(std::string_view predicate, const cfgpred::Ctx& c) {
    return cfgpred::matches(std::string(predicate), c);
}

} // namespace

// ─── Membership, not equality ──────────────────────────────────────────────
//
// `accelerator` is the first MULTI-VALUED layer: one build can enable several
// backends at once, which is what an inference framework shipping CUDA and
// ROCm in one artifact requires. The comparison is therefore membership.
//
// The alternative considered and rejected was to let `any(...)` mean
// membership while a bare key meant set equality. That would make a
// combinator change the meaning of its operand, and `all(a = "cuda",
// a = "rocm")` would then be unsatisfiable rather than "both are enabled".
// Membership everywhere keeps `any`/`all`/`not` pure boolean combinators.

TEST(CfgAccelerator, BareKeyIsMembership) {
    auto cuda = resolved({"cuda"});
    EXPECT_TRUE (m(R"(cfg(accelerator = "cuda"))", cuda));
    EXPECT_FALSE(m(R"(cfg(accelerator = "rocm"))", cuda));
}

TEST(CfgAccelerator, MultipleBackendsAreAllMembers) {
    auto both = resolved({"cuda", "rocm"});
    EXPECT_TRUE(m(R"(cfg(accelerator = "cuda"))", both));
    EXPECT_TRUE(m(R"(cfg(accelerator = "rocm"))", both));
}

TEST(CfgAccelerator, CombinatorsStayPure) {
    auto both = resolved({"cuda", "rocm"});
    auto cuda = resolved({"cuda"});
    EXPECT_TRUE (m(R"(cfg(any(accelerator = "cuda", accelerator = "rocm")))", cuda));
    EXPECT_TRUE (m(R"(cfg(all(accelerator = "cuda", accelerator = "rocm")))", both));
    EXPECT_FALSE(m(R"(cfg(all(accelerator = "cuda", accelerator = "rocm")))", cuda));
    EXPECT_TRUE (m(R"(cfg(not(accelerator = "rocm")))", cuda));
}

TEST(CfgAccelerator, ComposesWithTripleKeys) {
    auto cuda = resolved({"cuda"});
    EXPECT_TRUE (m(R"(cfg(all(linux, accelerator = "cuda")))", cuda));
    EXPECT_FALSE(m(R"(cfg(all(windows, accelerator = "cuda")))", cuda));
}

TEST(CfgAccelerator, UnresolvedTargetSideDoesNotMatch) {
    // The first pass runs before dependency resolution and cannot answer a
    // layer key. Returning false there is correct: the second pass owns it and
    // would otherwise contribute the same inputs twice.
    auto early = cfgpred::context_for("x86_64-linux-gnu");
    ASSERT_FALSE(early.layersKnown);
    EXPECT_FALSE(m(R"(cfg(accelerator = "cuda"))", early));
}

TEST(CfgAccelerator, IsALayerKeyNotATripleKey) {
    EXPECT_TRUE (cfgpred::uses_layer(R"(cfg(accelerator = "cuda"))"));
    EXPECT_FALSE(cfgpred::uses_layer(R"(cfg(arch = "x86_64"))"));
}

TEST(CfgAccelerator, MisspellingIsReportedNotSilentlyFalse) {
    // A predicate naming a token outside the vocabulary used to evaluate to
    // false in silence, which is indistinguishable from one that correctly did
    // not apply.
    auto unknown = cfgpred::unknown_tokens(R"(cfg(acclerator = "cuda"))");
    ASSERT_EQ(unknown.size(), 1u);
    EXPECT_EQ(unknown[0], "acclerator");
    EXPECT_TRUE(cfgpred::unknown_tokens(R"(cfg(accelerator = "cuda"))").empty());
}
