
import std;
import mcpplibs.mtest;
import mcpp.build.prepare_inputs;

using namespace mcpplibs::mtest;

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

UTest bare_key_is_membership{"CfgAccelerator/BareKeyIsMembership", [] {
    auto cuda = resolved({"cuda"});
    expect(bool(m(R"(cfg(accelerator = "cuda"))", cuda)));
    expect(!(m(R"(cfg(accelerator = "rocm"))", cuda)));
}};

UTest multiple_backends_are_all_members{"CfgAccelerator/MultipleBackendsAreAllMembers", [] {
    auto both = resolved({"cuda", "rocm"});
    expect(bool(m(R"(cfg(accelerator = "cuda"))", both)));
    expect(bool(m(R"(cfg(accelerator = "rocm"))", both)));
}};

UTest combinators_stay_pure{"CfgAccelerator/CombinatorsStayPure", [] {
    auto both = resolved({"cuda", "rocm"});
    auto cuda = resolved({"cuda"});
    expect(bool(m(R"(cfg(any(accelerator = "cuda", accelerator = "rocm")))", cuda)));
    expect(bool(m(R"(cfg(all(accelerator = "cuda", accelerator = "rocm")))", both)));
    expect(!(m(R"(cfg(all(accelerator = "cuda", accelerator = "rocm")))", cuda)));
    expect(bool(m(R"(cfg(not(accelerator = "rocm")))", cuda)));
}};

UTest composes_with_triple_keys{"CfgAccelerator/ComposesWithTripleKeys", [] {
    auto cuda = resolved({"cuda"});
    expect(bool(m(R"(cfg(all(linux, accelerator = "cuda")))", cuda)));
    expect(!(m(R"(cfg(all(windows, accelerator = "cuda")))", cuda)));
}};

UTest answered_before_resolution_unlike_the_other_layers{"CfgAccelerator/AnsweredBeforeResolutionUnlikeTheOtherLayers", [] {
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
    expect(fatal(!early.layersKnown));
    early.accelerators = {"cuda"};
    expect(bool(m(R"(cfg(accelerator = "cuda"))", early)));
    expect(!(m(R"(cfg(accelerator = "vulkan"))", early)));

    // A resolved layer key in the same position still answers false, and must:
    // the second pass owns it and would otherwise contribute the same inputs
    // twice through `append()`.
    expect(!(m(R"(cfg(c-abi = "glibc"))", early)));

    // No accel named: false for any backend, and `none` is how a section says
    // so without enumerating the backends it is not.
    auto nothing = cfgpred::context_for("x86_64-linux-gnu");
    expect(!(m(R"(cfg(accelerator = "cuda"))", nothing)));
    expect(bool(m(R"(cfg(accelerator = "none"))", nothing)));
}};

UTest is_in_the_vocabulary_but_not_a_resolved_layer{"CfgAccelerator/IsInTheVocabularyButNotAResolvedLayer", [] {
    // Three separate questions, and only the middle one changed. It is a known
    // key (so a misspelling is still reported); it is not a triple key; and it
    // does NOT send its section to the pass that runs after resolution.
    expect(cfgpred::unknown_tokens(R"(cfg(accelerator = "cuda"))").empty());
    expect(!(cfgpred::uses_layer(R"(cfg(accelerator = "cuda"))")));
    expect(!(cfgpred::uses_layer(R"(cfg(arch = "x86_64"))")));
    expect(bool(cfgpred::uses_layer(R"(cfg(c-abi = "musl"))")));
    // A predicate mixing the two belongs to the late pass, which can answer
    // both. Ownership is by membership, not by which leg matched.
    expect(bool(cfgpred::uses_layer(
        R"(cfg(all(accelerator = "cuda", c-abi = "musl")))")));
}};

UTest misspelling_is_reported_not_silently_false{"CfgAccelerator/MisspellingIsReportedNotSilentlyFalse", [] {
    // A predicate naming a token outside the vocabulary used to evaluate to
    // false in silence, which is indistinguishable from one that correctly did
    // not apply.
    auto unknown = cfgpred::unknown_tokens(R"(cfg(acclerator = "cuda"))");
    expect(fatal(eq(unknown.size(), 1u)));
    expect(eq(unknown[0], "acclerator"));
    expect(cfgpred::unknown_tokens(R"(cfg(accelerator = "cuda"))").empty());
}};
