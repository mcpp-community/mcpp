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

TEST(CfgAccelerator, AnsweredBeforeResolutionUnlikeTheOtherLayers) {
    // `accelerator` is answerable in the FIRST merge pass, and the difference
    // from the other five layer keys is the schedule, not the subject. Its
    // value is an input to the build -- `--accel`, or `[build] accel` -- read
    // before the first package is resolved, while `c-abi` is an answer the
    // dependency graph produces.
    //
    // The consequence is what this states: a payload, or a dependency, can be
    // gated on the device it is for. Grouped with the resolved keys it could
    // not be, and a CPU-only build of a project with a device island
    // downloaded the whole vendor toolkit.
    auto early = cfgpred::context_for("x86_64-linux-gnu");
    ASSERT_FALSE(early.layersKnown);
    early.accelerators = {"cuda"};
    EXPECT_TRUE (m(R"(cfg(accelerator = "cuda"))", early));
    EXPECT_FALSE(m(R"(cfg(accelerator = "vulkan"))", early));

    // A resolved layer key in the same position still answers false, and must:
    // the second pass owns it and would otherwise contribute the same inputs
    // twice through `append()`.
    EXPECT_FALSE(m(R"(cfg(c-abi = "glibc"))", early));

    // No accel named: false for any backend, and `none` is how a section says
    // so without enumerating the backends it is not.
    auto nothing = cfgpred::context_for("x86_64-linux-gnu");
    EXPECT_FALSE(m(R"(cfg(accelerator = "cuda"))", nothing));
    EXPECT_TRUE (m(R"(cfg(accelerator = "none"))", nothing));
}

TEST(CfgAccelerator, IsInTheVocabularyButNotAResolvedLayer) {
    // Three separate questions, and only the middle one changed. It is a known
    // key (so a misspelling is still reported); it is not a triple key; and it
    // does NOT send its section to the pass that runs after resolution.
    EXPECT_TRUE (cfgpred::unknown_tokens(R"(cfg(accelerator = "cuda"))").empty());
    EXPECT_FALSE(cfgpred::uses_layer(R"(cfg(accelerator = "cuda"))"));
    EXPECT_FALSE(cfgpred::uses_layer(R"(cfg(arch = "x86_64"))"));
    EXPECT_TRUE (cfgpred::uses_layer(R"(cfg(c-abi = "musl"))"));
    // A predicate mixing the two belongs to the late pass, which can answer
    // both. Ownership is by membership, not by which leg matched.
    EXPECT_TRUE (cfgpred::uses_layer(
        R"(cfg(all(accelerator = "cuda", c-abi = "musl")))"));
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
