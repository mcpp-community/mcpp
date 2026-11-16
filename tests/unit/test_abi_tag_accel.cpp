
import std;
import mcpplibs.mtest;
import mcpp.pack.abi_tag;

using namespace mcpplibs::mtest;

using mcpp::pack::AbiTag;
using mcpp::pack::AccelSet;
using mcpp::pack::tag_check;

namespace {

AbiTag cxx() {
    AbiTag t;
    t.triple = "x86_64-linux-gnu"; t.compiler = "gcc16";
    t.stdlib = "libstdcxx16";      t.standard = "c++23";
    return t;
}

AbiTag with(std::vector<AccelSet> a) { auto t = cxx(); t.accel = std::move(a); return t; }

AccelSet cuda(std::vector<std::string> archs, std::string ptx = {}) {
    return AccelSet{ .backend = "cuda", .version = "12.8",
                     .archs = std::move(archs), .ptxFloor = std::move(ptx) };
}

bool accepts(const AbiTag& published, const AbiTag& current) {
    return tag_check(published, current).empty();
}

} // namespace

// ─── The published side constrains; an absent dimension does not ───────────

UTest published_without_accel_accepts_any_request{"AbiTagAccel/PublishedWithoutAccelAcceptsAnyRequest", [] {
    // A CPU-only library states nothing about accelerators, so it is usable
    // everywhere. This is the existing "a shorter tag IS the statement" rule
    // reaching one dimension further.
    expect(bool(accepts(cxx(), with({cuda({"sm_86"})}))));
}};

UTest a_request_with_no_accelerator_is_satisfied_vacuously{"AbiTagAccel/ARequestWithNoAcceleratorIsSatisfiedVacuously", [] {
    expect(bool(accepts(with({cuda({"sm_90"})}), cxx())));
}};

// ─── Membership ────────────────────────────────────────────────────────────

UTest exact_arch_is_accepted{"AbiTagAccel/ExactArchIsAccepted", [] {
    expect(bool(accepts(with({cuda({"sm_80", "sm_90"})}), with({cuda({"sm_90"})}))));
}};

UTest an_arch_outside_the_set_is_refused{"AbiTagAccel/AnArchOutsideTheSetIsRefused", [] {
    auto bad = tag_check(with({cuda({"sm_80", "sm_90"})}), with({cuda({"sm_86"})}));
    expect(fatal(eq(bad.size(), 1u)));
    expect(eq(bad[0].dimension, "accel"));
    expect(ne(bad[0].need.find("sm_80"), std::string::npos));
    expect(ne(bad[0].got.find("sm_86"), std::string::npos));
}};

UTest a_different_backend_is_refused{"AbiTagAccel/ADifferentBackendIsRefused", [] {
    AccelSet rocm{ .backend = "rocm", .version = "6.4", .archs = {"gfx942"} };
    auto bad = tag_check(with({rocm}), with({cuda({"sm_90"})}));
    expect(fatal(eq(bad.size(), 1u)));
    expect(ne(bad[0].need.find("rocm"), std::string::npos));
}};

// ─── Family targets widen the published side ───────────────────────────────

UTest a_family_target_covers_its_family{"AbiTagAccel/AFamilyTargetCoversItsFamily", [] {
    // sm_90f is compatible with the same major and an equal-or-higher minor.
    // This is why publishing a family target rather than one artifact per chip
    // is what keeps the variant matrix finite.
    expect(bool(accepts(with({cuda({"sm_90f"})}), with({cuda({"sm_90"})}))));
    expect(bool(accepts(with({cuda({"sm_90f"})}), with({cuda({"sm_93"})}))));
    expect(!(accepts(with({cuda({"sm_90f"})}), with({cuda({"sm_89"})}))));
    expect(!(accepts(with({cuda({"sm_90f"})}), with({cuda({"sm_100"})}))));
}};

UTest an_architecture_specific_target_covers_only_itself{"AbiTagAccel/AnArchitectureSpecificTargetCoversOnlyItself", [] {
    expect(bool(accepts(with({cuda({"sm_90a"})}), with({cuda({"sm_90"})}))));
    expect(!(accepts(with({cuda({"sm_90a"})}), with({cuda({"sm_93"})}))));
}};

// ─── The PTX floor is the second asymmetric dimension ──────────────────────

UTest embedded_ptx_accepts_anything_at_or_above_the_floor{"AbiTagAccel/EmbeddedPtxAcceptsAnythingAtOrAboveTheFloor", [] {
    // NVIDIA's own guidance for a distributed binary is cubins for the known
    // targets plus PTX for the newest, so that later hardware JITs. Recording
    // the floor is what lets tag_check say yes to a device the cubin set does
    // not name, instead of refusing an artifact that genuinely runs.
    expect(bool(accepts(with({cuda({"sm_80"}, "80")}), with({cuda({"sm_86"})}))));
    expect(bool(accepts(with({cuda({"sm_80"}, "80")}), with({cuda({"sm_90"})}))));
    expect(!(accepts(with({cuda({"sm_86"}, "86")}), with({cuda({"sm_80"})}))));
}};

UTest without_ptx_there_is_no_forward_compatibility{"AbiTagAccel/WithoutPtxThereIsNoForwardCompatibility", [] {
    // AMD has no PTX equivalent, so an empty floor must not silently widen the
    // set. Family coverage on the published side is how ROCm gets the same
    // effect, and that is a different mechanism.
    expect(!(accepts(with({cuda({"sm_80"})}), with({cuda({"sm_86"})}))));
}};

// ─── Toolkit version ───────────────────────────────────────────────────────

UTest the_toolkit_major_must_agree{"AbiTagAccel/TheToolkitMajorMustAgree", [] {
    AccelSet built{ .backend = "cuda", .version = "12.8", .archs = {"sm_90"} };
    AccelSet want12{ .backend = "cuda", .version = "12.0", .archs = {"sm_90"} };
    AccelSet want13{ .backend = "cuda", .version = "13.0", .archs = {"sm_90"} };
    // Minor version compatibility is real within a major release family.
    expect(bool(accepts(with({built}), with({want12}))));
    expect(!(accepts(with({built}), with({want13}))));
}};

// ─── Several backends in one artifact ──────────────────────────────────────

UTest one_artifact_can_carry_several_backends{"AbiTagAccel/OneArtifactCanCarrySeveralBackends", [] {
    AccelSet rocm{ .backend = "rocm", .version = "6.4", .archs = {"gfx942"} };
    auto fat = with({cuda({"sm_90f"}), rocm});
    expect(bool(accepts(fat, with({cuda({"sm_90"})}))));
    AccelSet wantRocm{ .backend = "rocm", .version = "6.4", .archs = {"gfx942"} };
    expect(bool(accepts(fat, with({wantRocm}))));
}};

// ─── The other four dimensions still decide ────────────────────────────────

UTest accel_does_not_mask_the_cxx_dimensions{"AbiTagAccel/AccelDoesNotMaskTheCxxDimensions", [] {
    auto published = with({cuda({"sm_90"})});
    auto current   = with({cuda({"sm_90"})});
    current.compiler = "gcc15";
    auto bad = tag_check(published, current);
    expect(fatal(eq(bad.size(), 1u)));
    expect(eq(bad[0].dimension, "compiler"));
}};

// ─── The wire form round-trips ─────────────────────────────────────────────

UTest the_wire_form_round_trips{"AbiTagAccel/TheWireFormRoundTrips", [] {
    std::vector<mcpp::pack::AccelSet> sets{
        cuda({"sm_80", "sm_90f"}, "90"),
        mcpp::pack::AccelSet{ .backend = "rocm", .version = "6.4",
                              .archs = {"gfx942", "gfx10-3-generic"} },
    };
    auto text = mcpp::pack::accel_str(sets);
    auto back = mcpp::pack::parse_accel(text);
    expect(fatal(eq(back.size(), 2u)));
    expect(eq(back[0].backend, "cuda"));
    expect(eq(back[0].version, "12.8"));
    expect(eq(back[0].archs, (std::vector<std::string>{"sm_80", "sm_90f"})));
    expect(eq(back[0].ptxFloor, "90"));
    expect(eq(back[1].backend, "rocm"));
    expect(eq(back[1].archs, (std::vector<std::string>{"gfx942", "gfx10-3-generic"})));
    expect(back[1].ptxFloor.empty());
    expect(eq(mcpp::pack::accel_str(back), text));
}};

UTest unparseable_text_means_no_device_code{"AbiTagAccel/UnparseableTextMeansNoDeviceCode", [] {
    // The safe answer, and the same one a descriptor that never mentioned the
    // dimension gives: an artifact that states nothing constrains nothing.
    expect(mcpp::pack::parse_accel("").empty());
    expect(mcpp::pack::parse_accel("(none)").empty());
    expect(mcpp::pack::parse_accel("12.8+{sm_90}").empty());  // no backend
}};

// ── The grammar is open: backends mcpp has never heard of ──────────────────
//
// The `accel` dimension exists to carry "which device was this built for", and
// which devices exist is not mcpp's question. These assert that a spelling from
// a backend the engine knows nothing about round-trips and compares -- and,
// just as importantly, that an unknown arch shape falls back to equality rather
// than to some numeric guess.

UTest vulkan_spirv_round_trips{"AccelOpenGrammar/VulkanSpirvRoundTrips", [] {
    auto a = mcpp::pack::parse_accel("vulkan1.3+{spirv1.6}");
    expect(fatal(eq(a.size(), 1u)));
    expect(eq(a[0].backend, "vulkan"));
    expect(eq(a[0].version, "1.3"));
    expect(fatal(eq(a[0].archs.size(), 1u)));
    expect(eq(a[0].archs[0], "spirv1.6"));
    expect(eq(mcpp::pack::accel_str(a), "vulkan1.3+{spirv1.6}"));
}};

UTest sycl_target_round_trips{"AccelOpenGrammar/SyclTargetRoundTrips", [] {
    auto a = mcpp::pack::parse_accel("sycl2020+{spir64,nvptx64-sm_89}");
    expect(fatal(eq(a.size(), 1u)));
    expect(eq(a[0].backend, "sycl"));
    expect(fatal(eq(a[0].archs.size(), 2u)));
    expect(eq(a[0].archs[0], "spir64"));
    expect(eq(a[0].archs[1], "nvptx64-sm_89"));
}};

UTest an_unknown_arch_shape_compares_by_equality{"AccelOpenGrammar/AnUnknownArchShapeComparesByEquality", [] {
    // `gfx942` carries no leading number, so there is no ordering to read out
    // of it. Equality is the only question available and it is the one asked --
    // rather than reading `942` out of the middle and inventing a level.
    auto pub  = mcpp::pack::parse_accel("hip6.4+{gfx942}");
    auto same = mcpp::pack::parse_accel("hip6.4+{gfx942}");
    auto diff = mcpp::pack::parse_accel("hip6.4+{gfx1100}");
    expect(bool(mcpp::pack::accel_accepts(pub, same)));
    expect(!mcpp::pack::accel_accepts(pub, diff));
}};

UTest floor_has_a_backend_neutral_spelling{"AccelOpenGrammar/FloorHasABackendNeutralSpelling", [] {
    // `ptx>=` is CUDA's word and the one already published. `floor>=` says the
    // same thing without borrowing it, so a backend whose portable form is
    // SPIR-V does not have to write `ptx`.
    auto neutral = mcpp::pack::parse_accel("vulkan1.3+{spirv1.6} floor>=1.4");
    expect(fatal(eq(neutral.size(), 1u)));
    expect(eq(neutral[0].ptxFloor, "1.4"));

    auto cuda = mcpp::pack::parse_accel("cuda12.9+{sm_80} ptx>=80");
    expect(fatal(eq(cuda.size(), 1u)));
    expect(eq(cuda[0].ptxFloor, "80"));
}};

UTest the_existing_ptx_spelling_still_parses{"AccelOpenGrammar/TheExistingPtxSpellingStillParses", [] {
    // The negative control for the change above: adding a second spelling must
    // not have moved the first. Descriptors published before it exist.
    auto a = mcpp::pack::parse_accel("cuda12.8+{sm_80,sm_90f} ptx>=90");
    expect(fatal(eq(a.size(), 1u)));
    expect(eq(a[0].ptxFloor, "90"));
    expect(eq(mcpp::pack::accel_str(a), "cuda12.8+{sm_80,sm_90f} ptx>=90"));
}};

UTest rejects_incomplete_sets_and_unconsumed_input{"AccelRequest/RejectsIncompleteSetsAndUnconsumedInput", [] {
    for (auto input : {"sycl+{sm_89", "sycl+{sm_89}garbage", "sycl+sm_89",
                       "sycl+{sm_89,,sm_90}", "sycl+{,sm_89}", "sycl+{sm_89,}",
                       "sycl+{{sm_89}}", "sycl,", ",sycl", "sycl ptx>=", "12.8+{sm_89}"}) {
        const auto parsed = mcpp::pack::parse_accel_request(input);
        expect(fatal(!parsed)) << input;
        expect(ne(parsed.error().find("byte"), std::string::npos)) << input;
    }
}};

UTest duplicate_implementations_cannot_change_coverage_by_their_order{"AccelRequest/DuplicateImplementationsCannotChangeCoverageByTheirOrder", [] {
    for (auto input : {"sycl+{sm_89},sycl+{sm_90}", "sycl+{sm_90},sycl+{sm_89}"}) {
        const auto parsed = mcpp::pack::parse_accel_request(input);
        expect(fatal(!parsed));
        expect(ne(parsed.error().find("appears more than once"), std::string::npos));
        expect(ne(parsed.error().find("sycl"), std::string::npos));
    }
}};

UTest equivalent_sets_have_one_canonical_spelling{"AccelRequest/EquivalentSetsHaveOneCanonicalSpelling", [] {
    const auto a = mcpp::pack::parse_accel_request(
        "sycl+{spir64,nvptx64-sm_90,nvptx64-sm_89,spir64}, cuda12.9+{sm_90,sm_89} ptx>=90");
    const auto b = mcpp::pack::parse_accel_request(
        "cuda12.9+{sm_89,sm_90} floor>=90, sycl+{nvptx64-sm_89,nvptx64-sm_90,spir64}");
    expect(fatal(bool(a)));
    expect(fatal(bool(b)));
    expect(eq(mcpp::pack::accel_str(*a), mcpp::pack::accel_str(*b)));
    expect(eq(mcpp::pack::accel_str(*a), "cuda12.9+{sm_89,sm_90} ptx>=90, sycl+{nvptx64-sm_89,nvptx64-sm_90,spir64}"));
}};

UTest retains_empty_and_unconstrained_forms_and_open_target_vocabulary{"AccelRequest/RetainsEmptyAndUnconstrainedFormsAndOpenTargetVocabulary", [] {
    expect(mcpp::pack::parse_accel_request("")->empty());
    expect(mcpp::pack::parse_accel_request(" (none) ")->empty());
    const auto bare = mcpp::pack::parse_accel_request("sycl");
    expect(fatal(bool(bare)));
    expect(fatal(eq(bare->size(), 1u)));
    expect(bare->front().archs.empty());
    const auto explicitEmpty = mcpp::pack::parse_accel_request("sycl+{}");
    expect(fatal(bool(explicitEmpty)));
    expect(eq(mcpp::pack::accel_str(*bare), mcpp::pack::accel_str(*explicitEmpty)));
    const auto opaque = mcpp::pack::parse_accel_request("newmodel3.1+{future-device-v2,domain:opaque}");
    expect(fatal(bool(opaque)));
    expect(fatal(eq(opaque->front().archs.size(), 2u)));
}};
