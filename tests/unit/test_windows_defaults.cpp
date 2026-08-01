#include <gtest/gtest.h>

import std;
import mcpp.platform;
import mcpp.toolchain.msvc;
import mcpp.toolchain.triple;
import mcpp.toolchain.registry;
import mcpp.build.prepare;

// ── has_usable_msvc() ───────────────────────────────────────────────────────
//
// The predicate that decides whether the MSVC ABI is a viable default on this
// machine. A bare Windows box has the UCRT runtime DLLs but neither the MSVC
// STL (Visual Studio's "Desktop development with C++" workload) nor the
// Windows SDK, so a default that targets the MSVC ABI there can never build.

TEST(WindowsDefaults, HasUsableMsvcIsFalseOffWindows) {
    if constexpr (mcpp::platform::is_windows) {
        GTEST_SKIP() << "windows behavior is covered by e2e 182";
    } else {
        EXPECT_FALSE(mcpp::toolchain::msvc::has_usable_msvc());
    }
}

// The predicate must never disagree with the two probes it is defined as.
// A drift here (e.g. someone "optimizing" it down to find_vs_install_path())
// silently reintroduces the half-installed-VS trap it exists to close.
TEST(WindowsDefaults, HasUsableMsvcAgreesWithItsParts) {
    const bool both = mcpp::toolchain::msvc::find_std_module_source().has_value()
                   && mcpp::toolchain::msvc::find_windows_sdk().has_value();
    EXPECT_EQ(mcpp::toolchain::msvc::has_usable_msvc(), both);
}

// ── First-run pins ──────────────────────────────────────────────────────────
//
// macOS and Windows shared one pin until 2026.8.2.1. They must not: Apple
// ships no GCC, while on Windows clang targets the MSVC ABI and therefore
// needs a Visual Studio that is not preinstalled.

TEST(WindowsDefaults, FirstRunPinsParse) {
    namespace pins = mcpp::toolchain::triple::pins;
    for (auto spec : { pins::kFirstRunMac, pins::kFirstRunWinMsvc,
                       pins::kFirstRunWinGnu, pins::kFirstRunLinuxX86_64,
                       pins::kFirstRunLinuxOther }) {
        auto parsed = mcpp::toolchain::parse_toolchain_spec(std::string(spec));
        ASSERT_TRUE(parsed.has_value()) << spec;
        EXPECT_FALSE(parsed->version.empty()) << spec;
    }
}

// The GNU fallback must land on a target mcpp is willing to build for, and
// the toolchain it names must equal that target's vocabulary pin — otherwise
// the same decision is derived in two places and they will drift.
TEST(WindowsDefaults, GnuFallbackTargetIsVerifiedAndPinAgrees) {
    namespace triple = mcpp::toolchain::triple;
    auto t = triple::parse(std::string(triple::pins::kFirstRunWinGnuTarget));
    ASSERT_TRUE(t.has_value());
    const auto* known = triple::find_known_target(*t);
    ASSERT_NE(known, nullptr) << triple::pins::kFirstRunWinGnuTarget;
    EXPECT_EQ(known->tier, "verified");
    EXPECT_EQ(known->pin, triple::pins::kFirstRunWinGnu);
}

// ── Intent classification ───────────────────────────────────────────────────
//
// This table decides who may be overruled. Getting it wrong fails in both
// directions: call an explicit choice "mcpp's own" and a project that needs
// the MSVC ABI silently gets a different one; call mcpp's own default
// "explicit" and every user carrying a stale persisted default stays stuck.
TEST(WindowsDefaults, OriginClassification) {
    using mcpp::build::TcOrigin;
    using mcpp::build::tc_origin_is_user_explicit;
    EXPECT_TRUE (tc_origin_is_user_explicit(TcOrigin::ManifestToolchain));
    EXPECT_TRUE (tc_origin_is_user_explicit(TcOrigin::TargetSection));
    EXPECT_FALSE(tc_origin_is_user_explicit(TcOrigin::GlobalDefault));
    EXPECT_FALSE(tc_origin_is_user_explicit(TcOrigin::TargetPin));
    EXPECT_FALSE(tc_origin_is_user_explicit(TcOrigin::FirstRun));
    EXPECT_FALSE(tc_origin_is_user_explicit(TcOrigin::None));
}

// The fallback target must be PE/GNU, not the host's MSVC-ABI triple — the
// whole point is to leave the ABI that needs Visual Studio behind.
TEST(WindowsDefaults, GnuFallbackTargetIsWindowsGnu) {
    namespace triple = mcpp::toolchain::triple;
    auto t = triple::parse(std::string(triple::pins::kFirstRunWinGnuTarget));
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->os, "windows");
    EXPECT_EQ(t->env, "gnu");
    EXPECT_TRUE(t->is_windows_gnu());
}
