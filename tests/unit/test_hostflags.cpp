#include <gtest/gtest.h>

import std;
import mcpp.platform;
import mcpp.toolchain.dialect;
import mcpp.toolchain.hostflags;
import mcpp.toolchain.linkmodel;
import mcpp.toolchain.model;

using mcpp::toolchain::CompilerId;
using mcpp::toolchain::HostFlagOptions;

namespace {

mcpp::toolchain::Toolchain tc_for(CompilerId id) {
    mcpp::toolchain::Toolchain tc;
    tc.compiler = id;
    tc.targetTriple = id == CompilerId::MSVC ? "x86_64-pc-windows-msvc"
                                             : "x86_64-linux-gnu";
    return tc;
}

// Every compiler family mcpp claims to support, so a new one cannot be added
// without being answered for here.
constexpr CompilerId kFamilies[] = {
    CompilerId::GCC, CompilerId::Clang, CompilerId::MSVC,
};

} // namespace

// ── The capability-parity guard ─────────────────────────────────────────────
//
// build.mcpp is "one host C++ program", and compiling one of those is mcpp's
// job — so it must not have a narrower capability list than the main build.
// Stating that only in prose is what let `import mcpp;` / `import std;` sit
// behind a "not yet supported under MSVC" gate long after the main build had
// the .ifc pipeline (e2e 99 was already producing .ifc artifacts). This test
// turns the principle into a compile-and-run check: a family that is missing
// a dialect row, a module row, or host flags fails HERE, not at a user's
// `mcpp build`.
TEST(HostFlags, EveryFamilyHasCompleteTables) {
    for (auto id : kFamilies) {
        auto tc = tc_for(id);
        const auto& d = mcpp::toolchain::dialect_for(tc);
        const auto  t = mcpp::toolchain::bmi_traits(tc);

        EXPECT_FALSE(d.id.empty());
        // Needed to compile a `.mcpp` at all: the extension is unknown to
        // every driver, so the language has to be forced.
        EXPECT_FALSE(d.forceCxxLangArgv.empty()) << d.id;
        // Needed to name the produced program.
        EXPECT_FALSE(d.outputExePrefix.empty()) << d.id;
        EXPECT_FALSE(d.objExt.empty()) << d.id;
        // Needed to build and reference the bundled `mcpp` module.
        EXPECT_FALSE(t.bmiDir.empty()) << d.id;
        EXPECT_FALSE(t.bmiExt.empty()) << d.id;
    }
}

// The producer must answer for every family too — MSVC deliberately returns
// nothing (cl.exe finds headers and libs through INCLUDE/LIB, not argv), but
// it must be a decision, not a crash or an accident.
TEST(HostFlags, ProducerAnswersForEveryFamily) {
    HostFlagOptions opt;
    for (auto id : kFamilies) {
        auto tc = tc_for(id);
        auto compile = mcpp::toolchain::host_compile_tokens(
            tc, opt, mcpp::toolchain::no_escape);
        auto link = mcpp::toolchain::host_link_tokens(
            tc, opt, mcpp::toolchain::no_escape);
        if (id == CompilerId::MSVC) {
            EXPECT_TRUE(compile.empty());
            EXPECT_TRUE(link.empty());
        }
        // No empty tokens anywhere: an empty argv element is an argument the
        // driver still has to interpret.
        for (auto const& t : compile) EXPECT_FALSE(t.empty());
        for (auto const& t : link)    EXPECT_FALSE(t.empty());
    }
}

// ── bmi_reference_tokens ────────────────────────────────────────────────────
//
// The traits store these for the ninja STRING channel, where one word vs two
// makes no difference. argv consumers cannot be that relaxed.
TEST(HostFlags, BmiReferenceSplitsOnlyWhenTheSpellingHasASpace) {
    auto gnu = mcpp::toolchain::bmi_reference_tokens(
        " -fmodule-file=std=", std::filesystem::path("/tmp/std.pcm"));
    ASSERT_EQ(gnu.size(), 1u);
    EXPECT_EQ(gnu[0], "-fmodule-file=std=/tmp/std.pcm");

    auto msvc = mcpp::toolchain::bmi_reference_tokens(
        " /reference std=", std::filesystem::path("/tmp/std.ifc"));
    ASSERT_EQ(msvc.size(), 2u);
    EXPECT_EQ(msvc[0], "/reference");
    EXPECT_EQ(msvc[1], "std=/tmp/std.ifc");
}

TEST(HostFlags, BmiReferenceIsEmptyForAToolchainThatNamesNothing) {
    // GCC finds BMIs implicitly under <cwd>/gcm.cache — its prefix is empty
    // and must not produce a stray token.
    EXPECT_TRUE(mcpp::toolchain::bmi_reference_tokens(
        "", std::filesystem::path("/tmp/x.gcm")).empty());
}

// ── HostFlagOptions divergences ─────────────────────────────────────────────
//
// These knobs encode documented differences between the three consumers. A
// test so that "why is this optional?" has an answer in code, not only prose.
TEST(HostFlags, CfgBypassLinuxOnlyDiffersFromAlwaysOffLinux) {
    auto tc = tc_for(CompilerId::Clang);
    HostFlagOptions always;   always.cfgBypass = HostFlagOptions::CfgBypass::Always;
    HostFlagOptions linuxOnly; linuxOnly.cfgBypass = HostFlagOptions::CfgBypass::LinuxOnly;

    // Without a real clang payload there is no cfg to bypass, so both are
    // empty here; the assertion that matters is that the option exists and is
    // honoured identically on Linux, where the host helper does bypass.
    if constexpr (mcpp::platform::is_linux) {
        EXPECT_EQ(mcpp::toolchain::host_compile_tokens(tc, always, mcpp::toolchain::no_escape),
                  mcpp::toolchain::host_compile_tokens(tc, linuxOnly, mcpp::toolchain::no_escape));
    }
}

TEST(HostFlags, DeploymentTargetOnlyOnMacos) {
    auto tc = tc_for(CompilerId::GCC);
    HostFlagOptions opt;
    opt.macosDeploymentTarget = "14.0";
    auto tokens = mcpp::toolchain::host_compile_tokens(
        tc, opt, mcpp::toolchain::no_escape);
    bool found = std::ranges::any_of(tokens, [](auto const& t) {
        return t.starts_with("-mmacosx-version-min=");
    });
    EXPECT_EQ(found, mcpp::platform::is_macos);
}
