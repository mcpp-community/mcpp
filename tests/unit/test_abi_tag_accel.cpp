#include <gtest/gtest.h>

import std;
import mcpp.pack.abi_tag;

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

TEST(AbiTagAccel, PublishedWithoutAccelAcceptsAnyRequest) {
    // A CPU-only library states nothing about accelerators, so it is usable
    // everywhere. This is the existing "a shorter tag IS the statement" rule
    // reaching one dimension further.
    EXPECT_TRUE(accepts(cxx(), with({cuda({"sm_86"})})));
}

TEST(AbiTagAccel, ARequestWithNoAcceleratorIsSatisfiedVacuously) {
    EXPECT_TRUE(accepts(with({cuda({"sm_90"})}), cxx()));
}

// ─── Membership ────────────────────────────────────────────────────────────

TEST(AbiTagAccel, ExactArchIsAccepted) {
    EXPECT_TRUE(accepts(with({cuda({"sm_80", "sm_90"})}), with({cuda({"sm_90"})})));
}

TEST(AbiTagAccel, AnArchOutsideTheSetIsRefused) {
    auto bad = tag_check(with({cuda({"sm_80", "sm_90"})}), with({cuda({"sm_86"})}));
    ASSERT_EQ(bad.size(), 1u);
    EXPECT_EQ(bad[0].dimension, "accel");
    EXPECT_NE(bad[0].need.find("sm_80"), std::string::npos);
    EXPECT_NE(bad[0].got.find("sm_86"),  std::string::npos);
}

TEST(AbiTagAccel, ADifferentBackendIsRefused) {
    AccelSet rocm{ .backend = "rocm", .version = "6.4", .archs = {"gfx942"} };
    auto bad = tag_check(with({rocm}), with({cuda({"sm_90"})}));
    ASSERT_EQ(bad.size(), 1u);
    EXPECT_NE(bad[0].need.find("rocm"), std::string::npos);
}

// ─── Family targets widen the published side ───────────────────────────────

TEST(AbiTagAccel, AFamilyTargetCoversItsFamily) {
    // sm_90f is compatible with the same major and an equal-or-higher minor.
    // This is why publishing a family target rather than one artifact per chip
    // is what keeps the variant matrix finite.
    EXPECT_TRUE (accepts(with({cuda({"sm_90f"})}), with({cuda({"sm_90"})})));
    EXPECT_TRUE (accepts(with({cuda({"sm_90f"})}), with({cuda({"sm_93"})})));
    EXPECT_FALSE(accepts(with({cuda({"sm_90f"})}), with({cuda({"sm_89"})})));
    EXPECT_FALSE(accepts(with({cuda({"sm_90f"})}), with({cuda({"sm_100"})})));
}

TEST(AbiTagAccel, AnArchitectureSpecificTargetCoversOnlyItself) {
    EXPECT_TRUE (accepts(with({cuda({"sm_90a"})}), with({cuda({"sm_90"})})));
    EXPECT_FALSE(accepts(with({cuda({"sm_90a"})}), with({cuda({"sm_93"})})));
}

// ─── The PTX floor is the second asymmetric dimension ──────────────────────

TEST(AbiTagAccel, EmbeddedPtxAcceptsAnythingAtOrAboveTheFloor) {
    // NVIDIA's own guidance for a distributed binary is cubins for the known
    // targets plus PTX for the newest, so that later hardware JITs. Recording
    // the floor is what lets tag_check say yes to a device the cubin set does
    // not name, instead of refusing an artifact that genuinely runs.
    EXPECT_TRUE (accepts(with({cuda({"sm_80"}, "80")}), with({cuda({"sm_86"})})));
    EXPECT_TRUE (accepts(with({cuda({"sm_80"}, "80")}), with({cuda({"sm_90"})})));
    EXPECT_FALSE(accepts(with({cuda({"sm_86"}, "86")}), with({cuda({"sm_80"})})));
}

TEST(AbiTagAccel, WithoutPtxThereIsNoForwardCompatibility) {
    // AMD has no PTX equivalent, so an empty floor must not silently widen the
    // set. Family coverage on the published side is how ROCm gets the same
    // effect, and that is a different mechanism.
    EXPECT_FALSE(accepts(with({cuda({"sm_80"})}), with({cuda({"sm_86"})})));
}

// ─── Toolkit version ───────────────────────────────────────────────────────

TEST(AbiTagAccel, TheToolkitMajorMustAgree) {
    AccelSet built{ .backend = "cuda", .version = "12.8", .archs = {"sm_90"} };
    AccelSet want12{ .backend = "cuda", .version = "12.0", .archs = {"sm_90"} };
    AccelSet want13{ .backend = "cuda", .version = "13.0", .archs = {"sm_90"} };
    // Minor version compatibility is real within a major release family.
    EXPECT_TRUE (accepts(with({built}), with({want12})));
    EXPECT_FALSE(accepts(with({built}), with({want13})));
}

// ─── Several backends in one artifact ──────────────────────────────────────

TEST(AbiTagAccel, OneArtifactCanCarrySeveralBackends) {
    AccelSet rocm{ .backend = "rocm", .version = "6.4", .archs = {"gfx942"} };
    auto fat = with({cuda({"sm_90f"}), rocm});
    EXPECT_TRUE(accepts(fat, with({cuda({"sm_90"})})));
    AccelSet wantRocm{ .backend = "rocm", .version = "6.4", .archs = {"gfx942"} };
    EXPECT_TRUE(accepts(fat, with({wantRocm})));
}

// ─── The other four dimensions still decide ────────────────────────────────

TEST(AbiTagAccel, AccelDoesNotMaskTheCxxDimensions) {
    auto published = with({cuda({"sm_90"})});
    auto current   = with({cuda({"sm_90"})});
    current.compiler = "gcc15";
    auto bad = tag_check(published, current);
    ASSERT_EQ(bad.size(), 1u);
    EXPECT_EQ(bad[0].dimension, "compiler");
}

// ─── The wire form round-trips ─────────────────────────────────────────────

TEST(AbiTagAccel, TheWireFormRoundTrips) {
    std::vector<mcpp::pack::AccelSet> sets{
        cuda({"sm_80", "sm_90f"}, "90"),
        mcpp::pack::AccelSet{ .backend = "rocm", .version = "6.4",
                              .archs = {"gfx942", "gfx10-3-generic"} },
    };
    auto text = mcpp::pack::accel_str(sets);
    auto back = mcpp::pack::parse_accel(text);
    ASSERT_EQ(back.size(), 2u);
    EXPECT_EQ(back[0].backend,  "cuda");
    EXPECT_EQ(back[0].version,  "12.8");
    EXPECT_EQ(back[0].archs,    (std::vector<std::string>{"sm_80", "sm_90f"}));
    EXPECT_EQ(back[0].ptxFloor, "90");
    EXPECT_EQ(back[1].backend,  "rocm");
    EXPECT_EQ(back[1].archs,    (std::vector<std::string>{"gfx942", "gfx10-3-generic"}));
    EXPECT_TRUE(back[1].ptxFloor.empty());
    EXPECT_EQ(mcpp::pack::accel_str(back), text);
}

TEST(AbiTagAccel, UnparseableTextMeansNoDeviceCode) {
    // The safe answer, and the same one a descriptor that never mentioned the
    // dimension gives: an artifact that states nothing constrains nothing.
    EXPECT_TRUE(mcpp::pack::parse_accel("").empty());
    EXPECT_TRUE(mcpp::pack::parse_accel("(none)").empty());
    EXPECT_TRUE(mcpp::pack::parse_accel("12.8+{sm_90}").empty());  // no backend
}
