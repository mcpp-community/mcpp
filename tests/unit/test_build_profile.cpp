// The profile axis: profile selection, and the invariant that the resolved
// profile knobs participate in the fingerprint.
//
// Both halves were broken together. `[profile.<name>]` lands its knobs in
// buildConfig.optLevel/debug/lto/strip and flags.cppm turns them into
// -O<n>/-g/-flto, but canonical_compile_flags serialized only cflags/cxxflags/
// ldflags — so `--dev`, `--release` and `--profile dist` produced ONE
// fingerprint, hence one target/<triple>/<fp>/ directory and one global cache
// entry. Meanwhile `.build_cache` keyed its fast-path entries by target triple
// alone and the fast path only refused to run for an EXPLICIT profile flag, so
// `mcpp build --release` followed by a bare `mcpp build` reported success in
// 0.00s and left the release artifacts in place.

#include <gtest/gtest.h>
#include <cstdlib>

import std;
import mcpp.build.prepare;
import mcpp.manifest;

namespace {

mcpp::manifest::Manifest base() {
    mcpp::manifest::Manifest m;
    m.package.name     = "demo";
    m.package.version  = "0.1.0";
    m.package.standard = "c++23";
    m.buildConfig.optLevel = "2";
    return m;
}

// Save/restore MACOSX_DEPLOYMENT_TARGET, so a value this test sets (or one
// already in the environment) cannot leak into a sibling test in this binary.
class ScopedDeploymentTargetEnv {
public:
    explicit ScopedDeploymentTargetEnv(const char* value) {
        if (const char* old = std::getenv(kName); old) { had_ = true; old_ = old; }
        apply(value);
    }
    ~ScopedDeploymentTargetEnv() { apply(had_ ? old_.c_str() : nullptr); }
    ScopedDeploymentTargetEnv(const ScopedDeploymentTargetEnv&) = delete;
    ScopedDeploymentTargetEnv& operator=(const ScopedDeploymentTargetEnv&) = delete;
private:
    static constexpr const char* kName = "MACOSX_DEPLOYMENT_TARGET";
    void apply(const char* v) {
#if defined(_WIN32)
        ::_putenv_s(kName, v ? v : "");
#else
        if (v) ::setenv(kName, v, 1); else ::unsetenv(kName);
#endif
    }
    bool had_ = false;
    std::string old_;
};

} // namespace

// ── resolve_profile_name: the ONE rule, shared with the fast paths ───────────

TEST(BuildProfile, OverrideBeatsManifestDefault) {
    auto m = base();
    m.buildConfig.defaultProfile = "release";
    EXPECT_EQ(mcpp::build::resolve_profile_name(m, "dev"), "dev");
}

TEST(BuildProfile, ManifestDefaultBeatsGlobalDefault) {
    auto m = base();
    m.buildConfig.defaultProfile = "release";
    EXPECT_EQ(mcpp::build::resolve_profile_name(m, ""), "release");
}

TEST(BuildProfile, GlobalDefaultIsDev) {
    EXPECT_EQ(mcpp::build::resolve_profile_name(base(), ""), "dev");
}

TEST(BuildProfile, CustomProfileNamePassesThrough) {
    EXPECT_EQ(mcpp::build::resolve_profile_name(base(), "contracts"), "contracts");
}

// ── the fingerprint invariant ────────────────────────────────────────────────

TEST(BuildProfile, OptLevelIsInTheCanonicalFlags) {
    auto a = base();
    auto b = base(); b.buildConfig.optLevel = "0";
    EXPECT_NE(mcpp::build::canonical_compile_flags(a),
              mcpp::build::canonical_compile_flags(b));
}

TEST(BuildProfile, DebugLtoStripAreInTheCanonicalFlags) {
    auto ref = mcpp::build::canonical_compile_flags(base());
    { auto m = base(); m.buildConfig.debug = true;
      EXPECT_NE(mcpp::build::canonical_compile_flags(m), ref); }
    { auto m = base(); m.buildConfig.lto = true;
      EXPECT_NE(mcpp::build::canonical_compile_flags(m), ref); }
    { auto m = base(); m.buildConfig.strip = true;
      EXPECT_NE(mcpp::build::canonical_compile_flags(m), ref); }
}

// The exact shape the three built-in profiles resolve to (prepare_build's
// profile block). Asserting on the flag string rather than on a fingerprint hex
// keeps the failure legible when someone adds a knob and forgets to serialize it.
TEST(BuildProfile, BuiltInProfilesProduceDistinctCanonicalFlags) {
    auto dev = base();
    dev.buildConfig.optLevel = "0";
    dev.buildConfig.debug    = true;

    auto release = base();
    release.buildConfig.optLevel = "2";

    auto dist = base();
    dist.buildConfig.optLevel = "3";
    dist.buildConfig.strip    = true;

    auto fdev     = mcpp::build::canonical_compile_flags(dev);
    auto frelease = mcpp::build::canonical_compile_flags(release);
    auto fdist    = mcpp::build::canonical_compile_flags(dist);

    EXPECT_NE(fdev, frelease);
    EXPECT_NE(frelease, fdist);
    EXPECT_NE(fdev, fdist);
}

TEST(BuildProfile, CanonicalFlagsStillCoverTheNonProfileKnobs) {
    auto ref = mcpp::build::canonical_compile_flags(base());
    { auto m = base(); m.package.standard = "c++26";
      EXPECT_NE(mcpp::build::canonical_compile_flags(m), ref); }
    { auto m = base(); m.buildConfig.cxxflags = {"-DFOO"};
      EXPECT_NE(mcpp::build::canonical_compile_flags(m), ref); }
    { auto m = base(); m.buildConfig.cflags = {"-DBAR"};
      EXPECT_NE(mcpp::build::canonical_compile_flags(m), ref); }
    { auto m = base(); m.buildConfig.ldflags = {"-lm"};
      EXPECT_NE(mcpp::build::canonical_compile_flags(m), ref); }
    { auto m = base(); m.buildConfig.dialectCxxflags = {"-freflection"};
      EXPECT_NE(mcpp::build::canonical_compile_flags(m), ref); }
    { auto m = base(); m.buildConfig.cStandard = "c17";
      EXPECT_NE(mcpp::build::canonical_compile_flags(m), ref); }
}

// ── macos_deployment_target: TARGET-keyed, not HOST-keyed (mcpp#685) ────────
//
// `canonical_compile_flags` used to fold this value in only
// `if constexpr (mcpp::platform::is_macos)` -- the platform mcpp ITSELF was
// built for -- so `mcpp build --target aarch64-macos` on a Linux host kept
// the exact same fingerprint (hence the exact same `target/<triple>/<fp>/`
// directory) no matter what `[build] macos_deployment_target` was edited to,
// and a std.pcm built for one deployment target could be replayed for
// another. `targetIsMacos` is now an explicit parameter, answered by the
// caller from the TARGET triple, so these assertions hold on whatever host
// runs this suite -- reverting to the old `if constexpr` would fail
// `TargetIsMacosFoldsDeploymentTargetIntoTheFingerprint` on any non-Apple CI
// runner.

TEST(BuildProfile, NonMacosTargetIgnoresDeploymentTargetInTheFingerprint) {
    ScopedDeploymentTargetEnv noEnv(nullptr);
    auto ref = base();
    auto changed = base(); changed.buildConfig.macosDeploymentTarget = "11.0";
    // `targetIsMacos` defaults to false: a caller with no target in hand (or
    // one building for something other than macOS) must not see the
    // fingerprint move just because the manifest names a macOS floor it will
    // never apply.
    EXPECT_EQ(mcpp::build::canonical_compile_flags(ref),
              mcpp::build::canonical_compile_flags(changed));
    EXPECT_EQ(mcpp::build::canonical_compile_flags(ref, /*targetIsMacos=*/false),
              mcpp::build::canonical_compile_flags(changed, /*targetIsMacos=*/false));
}

TEST(BuildProfile, TargetIsMacosFoldsDeploymentTargetIntoTheFingerprint) {
    ScopedDeploymentTargetEnv noEnv(nullptr);
    auto ref = base();
    auto changed = base(); changed.buildConfig.macosDeploymentTarget = "11.0";
    EXPECT_NE(mcpp::build::canonical_compile_flags(ref, /*targetIsMacos=*/true),
              mcpp::build::canonical_compile_flags(changed, /*targetIsMacos=*/true));

    // AND THE ENV OVERRIDE, folded the same way `deployment_target` resolves
    // it: with MACOSX_DEPLOYMENT_TARGET set, the manifest value stops
    // mattering to the fingerprint too, because it stops mattering to what
    // actually gets compiled.
    ScopedDeploymentTargetEnv env("12.3");
    auto manifestA = base(); manifestA.buildConfig.macosDeploymentTarget = "11.0";
    auto manifestB = base(); manifestB.buildConfig.macosDeploymentTarget = "13.0";
    EXPECT_EQ(mcpp::build::canonical_compile_flags(manifestA, /*targetIsMacos=*/true),
              mcpp::build::canonical_compile_flags(manifestB, /*targetIsMacos=*/true));
}

// ── cache-mode parsing ──────────────────────────────────────────────────────

TEST(BuildProfile, CacheModeParsesTheThreeModes) {
    using mcpp::build::CacheMode;
    EXPECT_EQ(mcpp::build::parse_cache_mode("global"), CacheMode::Global);
    EXPECT_EQ(mcpp::build::parse_cache_mode("local"),  CacheMode::Local);
    EXPECT_EQ(mcpp::build::parse_cache_mode("off"),    CacheMode::Off);
    // `none` accepted as a synonym for off; anything else is refused rather
    // than silently meaning "global", which would be the worst default for a
    // typo to land on.
    EXPECT_EQ(mcpp::build::parse_cache_mode("none"),   CacheMode::Off);
    EXPECT_FALSE(mcpp::build::parse_cache_mode("on").has_value());
    EXPECT_FALSE(mcpp::build::parse_cache_mode("").has_value());
    EXPECT_FALSE(mcpp::build::parse_cache_mode("GLOBAL").has_value());
}

TEST(BuildProfile, CacheModeNameRoundTrips) {
    using mcpp::build::CacheMode;
    for (auto m : {CacheMode::Global, CacheMode::Local, CacheMode::Off})
        EXPECT_EQ(mcpp::build::parse_cache_mode(mcpp::build::cache_mode_name(m)), m);
}

// ── cache-mode resolution ───────────────────────────────────────────────────
// Same shape as resolve_profile_name and for the same reason: the fast paths
// skip prepare_build, so they must settle the mode from one shared pure rule.
// Without that, a graph generated under `global` (which contains stage_file
// edges reading the cache) got replayed for a request that asked for `local` —
// and ruling the cache out is `local`'s entire purpose.

TEST(BuildProfile, CacheModeOverrideBeatsManifest) {
    auto m = base();
    m.buildConfig.cacheMode = "local";
    EXPECT_EQ(mcpp::build::resolve_cache_mode(m, "global"),
              mcpp::build::CacheMode::Global);
}

TEST(BuildProfile, CacheModeManifestBeatsDefault) {
    auto m = base();
    m.buildConfig.cacheMode = "off";
    EXPECT_EQ(mcpp::build::resolve_cache_mode(m, ""), mcpp::build::CacheMode::Off);
}

TEST(BuildProfile, CacheModeDefaultsToGlobal) {
    EXPECT_EQ(mcpp::build::resolve_cache_mode(base(), ""),
              mcpp::build::CacheMode::Global);
}

// An unparseable value must not silently mean "global" at THIS level either: it
// falls through to the next source, and prepare_build reports it separately.
TEST(BuildProfile, UnknownManifestCacheModeFallsThroughToDefault) {
    auto m = base();
    m.buildConfig.cacheMode = "bogus";
    EXPECT_EQ(mcpp::build::resolve_cache_mode(m, ""),
              mcpp::build::CacheMode::Global);
    // ...and an unparseable override does not shadow a valid manifest value.
    m.buildConfig.cacheMode = "local";
    EXPECT_EQ(mcpp::build::resolve_cache_mode(m, "bogus"),
              mcpp::build::CacheMode::Local);
}

// ── #649 E9: the override the command line states, one rule for every verb ───
//
// `build` preferred `--profile` over the shorthands and `run` preferred the
// shorthands, so one command line built two profiles. Every verb now reads this.
TEST(BuildProfile, ProfileOptionBeatsTheShorthands) {
    EXPECT_EQ(mcpp::build::profile_override_from_flags("dev", true, false), "dev");
    EXPECT_EQ(mcpp::build::profile_override_from_flags("release", false, true), "release");
}

TEST(BuildProfile, ShorthandsNameTheirProfile) {
    EXPECT_EQ(mcpp::build::profile_override_from_flags("", true, false), "release");
    EXPECT_EQ(mcpp::build::profile_override_from_flags("", false, true), "dev");
    EXPECT_EQ(mcpp::build::profile_override_from_flags("", false, false), "");
}
