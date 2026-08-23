// Target-side resolution: which layer comes from where.
//
// These tests exist because the decision they cover used to be three
// independent derivations with three different criteria, and the bug was that
// they disagreed on a case none of them had been written for: a C program whose
// system comes from the dependency graph. So the assertions below are about the
// TABLE's totality — every combination the ecosystem can produce gets a row,
// including the ones that were unrepresentable before.
//
// The module under test is a pure function of plain data on purpose. The
// capability it replaces (`hosted-standard-library`) drove seven behaviours
// from inside a 7000-line translation unit and had zero test coverage, because
// asserting it required running a whole build. Nothing here runs a build.

#include <gtest/gtest.h>

import std;
import mcpp.targetside;

namespace ts = mcpp::targetside;

namespace {

// The traditional stack: a payload supplies every layer.
ts::Inputs payload_linux() {
    ts::Inputs in;
    in.llvmTriple          = "x86_64-unknown-linux-gnu";
    in.targetOs            = "linux";
    in.targetEnv           = "gnu";
    in.payloadSystemRef    = "xim-x-linux-headers@5.11.1";
    in.payloadLibcRef      = "xim-x-glibc@2.44";
    in.payloadCxxRef       = "xim-x-llvm@22.1.8";
    in.payloadCxxInterface = "libc++";
    return in;
}

ts::Provider provider(std::string name, std::string version,
                      std::string iface, bool stdModule = false) {
    ts::Provider p;
    p.name          = std::move(name);
    p.version       = std::move(version);
    p.interfaceName = std::move(iface);
    p.hasStdModule  = stdModule;
    return p;
}

} // namespace

// ── The capability grammar ───────────────────────────────────────────────────

TEST(TargetSideCapability, ThreeLayerNamesAreAccepted) {
    for (auto [entry, layer] : std::initializer_list<
             std::pair<std::string_view, ts::CapLayer>>{
             {"mcpp:kernel-abi", ts::CapLayer::KernelAbi},
             {"mcpp:c-abi",      ts::CapLayer::CAbi},
             {"mcpp:c++-abi",    ts::CapLayer::CxxAbi}}) {
        auto r = ts::parse_capability(entry);
        ASSERT_TRUE(r.has_value()) << entry;
        ASSERT_TRUE(r->has_value()) << entry;
        EXPECT_EQ((*r)->layer, layer) << entry;
        EXPECT_TRUE((*r)->interfaceName.empty()) << entry;
    }
}

TEST(TargetSideCapability, InterfaceNameIsCarriedByTheDeclaration) {
    auto r = ts::parse_capability("mcpp:kernel-abi=openkal");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->layer, ts::CapLayer::KernelAbi);
    EXPECT_EQ((*r)->interfaceName, "openkal");
}

// The engine must not reject names it does not own. `provides` serves the
// feature system too, and a closed set over the whole array would break the
// allocator selection that already ships.
TEST(TargetSideCapability, NamesOutsideTheReservedPrefixPassThrough) {
    for (std::string_view entry : {"freestanding-allocator",
                                   "hosted-standard-library",
                                   "anything-a-package-invents"}) {
        auto r = ts::parse_capability(entry);
        ASSERT_TRUE(r.has_value()) << entry;
        EXPECT_FALSE(r->has_value()) << entry;
    }
}

// And the point of having a prefix at all: inside it, a typo is an error rather
// than a silently disabled behaviour.
TEST(TargetSideCapability, UnknownNameInsideTheReservedPrefixIsAnError) {
    for (std::string_view entry : {"mcpp:kernel_abi",   // underscore, not hyphen
                                   "mcpp:cabi",
                                   "mcpp:c++abi",
                                   "mcpp:target-system"}) {
        auto r = ts::parse_capability(entry);
        EXPECT_FALSE(r.has_value()) << entry;
        if (!r.has_value())
            EXPECT_NE(r.error().find("mcpp:kernel-abi"), std::string::npos)
                << "the diagnostic must list the layers that do exist";
    }
}

// ── The resolution table ─────────────────────────────────────────────────────

TEST(TargetSideResolve, TraditionalStackTakesEveryLayerFromThePayload) {
    auto r = ts::resolve(payload_linux());
    EXPECT_EQ(r.kernelAbi.origin, ts::Origin::Payload);
    EXPECT_EQ(r.cAbi.origin,      ts::Origin::Payload);
    EXPECT_EQ(r.cxx.origin,       ts::Origin::Payload);
    EXPECT_EQ(r.kernelAbi.interfaceName, "linux");
    EXPECT_EQ(r.cAbi.interfaceName,      "gnu");
    EXPECT_EQ(r.cxx.interfaceName,       "libc++");
    EXPECT_FALSE(r.system_from_graph());
}

TEST(TargetSideResolve, OpenkalCxxTakesEveryLayerFromTheGraph) {
    auto in = payload_linux();
    in.llvmTriple = "arm64-apple-macos14.0";
    in.targetOs   = "macos";
    in.kernelAbi  = provider("openkal-macos", "0.3.1", "openkal");
    in.cAbi       = provider("openkal-musl", "0.3.1", "musl");
    in.cxxAbi     = provider("openkal-llvm-runtime", "0.1.0", "libc++", /*stdModule=*/true);

    auto r = ts::resolve(in);
    EXPECT_EQ(r.kernelAbi.origin, ts::Origin::Graph);
    EXPECT_EQ(r.cAbi.origin,      ts::Origin::Graph);
    EXPECT_EQ(r.cxx.origin,       ts::Origin::Graph);
    EXPECT_EQ(r.kernelAbi.impl, "openkal-macos@0.3.1");
    EXPECT_EQ(r.cAbi.impl,      "openkal-musl@0.3.1");
    EXPECT_FALSE(r.cxx.subset) << "a package that declares a std module supplies the whole library";
    EXPECT_TRUE(r.system_from_graph());
}

// THE CASE THAT WAS UNREPRESENTABLE, AND THE MEASURED DEFECT.
//
// A C program over openkal has a kernel ABI and a C library from the graph and
// no C++ runtime at all. Before this module the absence of the third layer was
// read as "the target side is not from the graph", the payload's own libc++
// stayed on the link line, and a macOS cross ended in
// `libc++.so: unhandled file type`.
TEST(TargetSideResolve, PureCOverOpenkalStillHasItsSystemFromTheGraph) {
    auto in = payload_linux();
    in.llvmTriple = "arm64-apple-macos14.0";
    in.targetOs   = "macos";
    in.kernelAbi  = provider("openkal-macos", "0.3.1", "openkal");
    in.cAbi       = provider("openkal-musl", "0.3.1", "musl");
    // no cxxAbi provider

    auto r = ts::resolve(in);
    EXPECT_EQ(r.cxx.origin, ts::Origin::None)
        << "a C program needs no C++ runtime, and that is not a failure to find one";
    EXPECT_TRUE(r.system_from_graph())
        << "the system still comes from the graph, which is the whole defect";
    EXPECT_NE(r.cxx.origin, ts::Origin::Payload)
        << "the payload's C++ runtime must not be selected over a graph C library";
}

// Only the platform implementation, and nothing above it.
TEST(TargetSideResolve, RawOpenkalHasOnlyAKernelAbi) {
    auto in = payload_linux();
    in.kernelAbi           = provider("openkal-linux", "0.5.1", "openkal");
    in.sysrootDeclaredEmpty = true;

    auto r = ts::resolve(in);
    EXPECT_EQ(r.kernelAbi.origin, ts::Origin::Graph);
    EXPECT_TRUE(r.cAbi.absent());
    EXPECT_TRUE(r.cxx.absent());
}

TEST(TargetSideResolve, BareMetalWithPicolibcHasNoKernelAbi) {
    ts::Inputs in;
    in.llvmTriple          = "riscv64-none-elf";
    in.freestandingTarget  = true;
    in.sysrootXpkg         = "xim:picolibc-riscv@1.8.12";

    auto r = ts::resolve(in);
    EXPECT_TRUE(r.kernelAbi.absent())
        << "a bare machine has no kernel, and saying so is the information";
    EXPECT_EQ(r.cAbi.origin,        ts::Origin::Xpkg);
    // The package name, not a prettier form of it. Stripping the `-riscv`
    // suffix would mean the engine knows how this ecosystem names its
    // packages, which is exactly the knowledge this design keeps out of it.
    EXPECT_EQ(r.cAbi.interfaceName, "picolibc-riscv");
    EXPECT_EQ(r.cAbi.impl,          "xim:picolibc-riscv@1.8.12");
    EXPECT_TRUE(r.cxx.absent());
    EXPECT_FALSE(r.system_from_graph());
}

// THE MIXED CASE. It already ships, and no single boolean can express it.
TEST(TargetSideResolve, PrebuiltCLibraryUnderAGraphSuppliedCxxSubset) {
    ts::Inputs in;
    in.llvmTriple         = "riscv64-none-elf";
    in.freestandingTarget = true;
    in.sysrootXpkg        = "xim:picolibc-riscv@1.8.12";
    in.cxxAbi             = provider("std-freestanding", "0.5.0", "freestanding subset");

    auto r = ts::resolve(in);
    EXPECT_EQ(r.cAbi.origin, ts::Origin::Xpkg)  << "prebuilt";
    EXPECT_EQ(r.cxx.origin,  ts::Origin::Graph) << "composed at build time";
    EXPECT_TRUE(r.cxx.subset) << "no std module declared, so the library is a subset";
}

TEST(TargetSideResolve, ZeroLibcTierHasNothingAtAll) {
    ts::Inputs in;
    in.llvmTriple           = "x86_64-none-elf";
    in.freestandingTarget   = true;
    in.sysrootDeclaredEmpty = true;

    auto r = ts::resolve(in);
    EXPECT_TRUE(r.kernelAbi.absent());
    EXPECT_TRUE(r.cAbi.absent());
    EXPECT_TRUE(r.cxx.absent());
}

// ── The layering rule ────────────────────────────────────────────────────────
//
// An implementation must have been configured for the layer beneath it. The
// resolver enforces it by construction; this asserts that the construction
// really does, rather than that a later check catches it.
TEST(TargetSideResolve, PayloadCxxIsNotSelectedOverANonPayloadCLibrary) {
    for (auto make : std::initializer_list<std::function<ts::Inputs()>>{
             [] { auto in = payload_linux(); in.sysrootXpkg = "xim:picolibc-riscv@1.8.12"; return in; },
             [] { auto in = payload_linux();
                  in.cAbi = provider("openkal-musl", "0.3.1", "musl"); return in; }}) {
        auto r = ts::resolve(make());
        EXPECT_NE(r.cxx.origin, ts::Origin::Payload);
        EXPECT_EQ(ts::check_layering(r), std::nullopt)
            << "the default path must not be able to construct the violation";
    }
}

TEST(TargetSideResolve, TheLayeringRuleIsStatedForTheOverridePath) {
    ts::TargetSide bad;
    bad.cAbi = { ts::Origin::Graph,   "musl",   "openkal-musl@0.3.1", false };
    bad.cxx  = { ts::Origin::Payload, "libc++", "xim-x-llvm@22.1.8",  false };

    auto why = ts::check_layering(bad);
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find("never configured for this one"), std::string::npos)
        << "the diagnostic must state the reason, not merely the combination";
}

// ── The report ───────────────────────────────────────────────────────────────

TEST(TargetSideReport, ShowsTheTranslatedTripleWhenItDiffers) {
    auto in = payload_linux();
    in.llvmTriple = "arm64-apple-macos14.0";
    auto text = ts::format_report(ts::resolve(in), "aarch64-macos");
    EXPECT_NE(text.find("aarch64-macos → arm64-apple-macos14.0"), std::string::npos)
        << "the translation is load bearing: the untranslated form emits a Mach-O "
           "whose MinVersion carries no platform";
}

TEST(TargetSideReport, AbsentLayersReadAsADashRatherThanBeingOmitted) {
    ts::Inputs in;
    in.llvmTriple           = "x86_64-none-elf";
    in.freestandingTarget   = true;
    in.sysrootDeclaredEmpty = true;
    auto text = ts::format_report(ts::resolve(in), "x86_64-none-elf");
    EXPECT_NE(text.find("kernel-abi  —"), std::string::npos);
    EXPECT_NE(text.find("c-abi       —"), std::string::npos);
    EXPECT_NE(text.find("c++         —"), std::string::npos);
}

TEST(TargetSideReport, NamesInterfaceAndImplementationSeparately) {
    auto in = payload_linux();
    in.kernelAbi = provider("openkal-opensbi", "0.1.0", "openkal");
    auto text = ts::format_report(ts::resolve(in), "riscv64-none-elf");
    EXPECT_NE(text.find("openkal"), std::string::npos);
    EXPECT_NE(text.find("openkal-opensbi@0.1.0"), std::string::npos)
        << "four packages answer to one interface name; collapsing them would "
           "hide why one source reaches four machines";
    EXPECT_NE(text.find("graph"), std::string::npos);
}
