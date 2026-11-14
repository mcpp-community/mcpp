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

#include <cstdlib>

import std;
import mcpplibs.mtest;
import mcpp.invocation;
import mcpp.build.prepare;
import mcpp.manifest;
import mcpplibs.os.platform;

using namespace mcpplibs::mtest;

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
    explicit ScopedDeploymentTargetEnv(const char* value)
        : inner_("MACOSX_DEPLOYMENT_TARGET", value ? std::optional<std::string>(value) : std::nullopt) {}
private:
    mcpp::invocation::ScopedEnv inner_;   // stated for this invocation, as mcpp states its own
};

} // namespace

// ── resolve_profile_name: the ONE rule, shared with the fast paths ───────────

UTest override_beats_manifest_default{"BuildProfile/OverrideBeatsManifestDefault", [] {
    auto m = base();
    m.buildConfig.defaultProfile = "release";
    expect(eq(mcpp::build::resolve_profile_name(m, "dev"), "dev"));
}};

UTest manifest_default_beats_global_default{"BuildProfile/ManifestDefaultBeatsGlobalDefault", [] {
    auto m = base();
    m.buildConfig.defaultProfile = "release";
    expect(eq(mcpp::build::resolve_profile_name(m, ""), "release"));
}};

UTest global_default_is_dev{"BuildProfile/GlobalDefaultIsDev", [] {
    expect(eq(mcpp::build::resolve_profile_name(base(), ""), "dev"));
}};

UTest custom_profile_name_passes_through{"BuildProfile/CustomProfileNamePassesThrough", [] {
    expect(eq(mcpp::build::resolve_profile_name(base(), "contracts"), "contracts"));
}};

// ── the fingerprint invariant ────────────────────────────────────────────────

UTest opt_level_is_in_the_canonical_flags{"BuildProfile/OptLevelIsInTheCanonicalFlags", [] {
    auto a = base();
    auto b = base(); b.buildConfig.optLevel = "0";
    expect(ne(mcpp::build::canonical_configuration_flags(a), mcpp::build::canonical_configuration_flags(b)));
}};

UTest debug_lto_strip_are_in_the_canonical_flags{"BuildProfile/DebugLtoStripAreInTheCanonicalFlags", [] {
    auto ref = mcpp::build::canonical_configuration_flags(base());
    { auto m = base(); m.buildConfig.debug = true;
      expect(ne(mcpp::build::canonical_configuration_flags(m), ref)); }
    { auto m = base(); m.buildConfig.lto = true;
      expect(ne(mcpp::build::canonical_configuration_flags(m), ref)); }
    { auto m = base(); m.buildConfig.strip = true;
      expect(ne(mcpp::build::canonical_configuration_flags(m), ref)); }
}};

// The exact shape the three built-in profiles resolve to (prepare_build's
// profile block). Asserting on the flag string rather than on a fingerprint hex
// keeps the failure legible when someone adds a knob and forgets to serialize it.
UTest built_in_profiles_produce_distinct_canonical_flags{"BuildProfile/BuiltInProfilesProduceDistinctCanonicalFlags", [] {
    auto dev = base();
    dev.buildConfig.optLevel = "0";
    dev.buildConfig.debug    = true;

    auto release = base();
    release.buildConfig.optLevel = "2";

    auto dist = base();
    dist.buildConfig.optLevel = "3";
    dist.buildConfig.strip    = true;

    auto fdev     = mcpp::build::canonical_configuration_flags(dev);
    auto frelease = mcpp::build::canonical_configuration_flags(release);
    auto fdist    = mcpp::build::canonical_configuration_flags(dist);

    expect(ne(fdev, frelease));
    expect(ne(frelease, fdist));
    expect(ne(fdev, fdist));
}};

UTest canonical_flags_still_cover_the_non_profile_knobs{"BuildProfile/CanonicalFlagsStillCoverTheNonProfileKnobs", [] {
    auto ref = mcpp::build::canonical_configuration_flags(base());
    { auto m = base(); m.package.standard = "c++26";
      expect(ne(mcpp::build::canonical_configuration_flags(m), ref)); }
    { auto m = base(); m.buildConfig.dialectCxxflags = {"-freflection"};
      expect(ne(mcpp::build::canonical_configuration_flags(m), ref)); }
    { auto m = base(); m.buildConfig.dependencyLinkage = "shared";
      expect(ne(mcpp::build::canonical_configuration_flags(m), ref)); }
}};

// Workspace design 2026-09-29 §3: a package's own flags are attributes of its
// node, not inputs of the configuration, so editing them keeps the build
// directory and ninja rebuilds the edges whose commands changed.
UTest package_flags_are_not_in_the_configuration{"BuildProfile/PackageFlagsAreNotInTheConfiguration", [] {
    auto ref = mcpp::build::canonical_configuration_flags(base());
    { auto m = base(); m.buildConfig.cxxflags = {"-DFOO"};
      expect(eq(mcpp::build::canonical_configuration_flags(m), ref)); }
    { auto m = base(); m.buildConfig.cflags = {"-DBAR"};
      expect(eq(mcpp::build::canonical_configuration_flags(m), ref)); }
    { auto m = base(); m.buildConfig.ldflags = {"-lm"};
      expect(eq(mcpp::build::canonical_configuration_flags(m), ref)); }
    { auto m = base(); m.buildConfig.cStandard = "c17";
      expect(eq(mcpp::build::canonical_configuration_flags(m), ref)); }
}};

// ── macos_deployment_target: TARGET-keyed, not HOST-keyed (mcpp#685) ────────
//
// The fingerprint's flag string used to fold this value in only
// `if constexpr (mcpplibs::os::is_macos)` -- the platform mcpp ITSELF was
// built for -- so `mcpp build --target aarch64-macos` on a Linux host kept
// the exact same fingerprint (hence the exact same `target/<triple>/<fp>/`
// directory) no matter what `[build] macos_deployment_target` was edited to,
// and a std.pcm built for one deployment target could be replayed for
// another. `targetIsMacos` is now an explicit parameter, answered by the
// caller from the TARGET triple, so these assertions hold on whatever host
// runs this suite -- reverting to the old `if constexpr` would fail
// `TargetIsMacosFoldsDeploymentTargetIntoTheFingerprint` on any non-Apple CI
// runner.

UTest non_macos_target_ignores_deployment_target_in_the_fingerprint{"BuildProfile/NonMacosTargetIgnoresDeploymentTargetInTheFingerprint", [] {
    ScopedDeploymentTargetEnv noEnv(nullptr);
    auto ref = base();
    auto changed = base(); changed.buildConfig.macosDeploymentTarget = "11.0";
    // `targetIsMacos` defaults to false: a caller with no target in hand (or
    // one building for something other than macOS) must not see the
    // fingerprint move just because the manifest names a macOS floor it will
    // never apply.
    expect(eq(mcpp::build::canonical_configuration_flags(ref), mcpp::build::canonical_configuration_flags(changed)));
    expect(eq(mcpp::build::canonical_configuration_flags(ref, /*targetIsMacos=*/false), mcpp::build::canonical_configuration_flags(changed, /*targetIsMacos=*/false)));
}};

UTest target_is_macos_folds_deployment_target_into_the_fingerprint{"BuildProfile/TargetIsMacosFoldsDeploymentTargetIntoTheFingerprint", [] {
    ScopedDeploymentTargetEnv noEnv(nullptr);
    auto ref = base();
    auto changed = base(); changed.buildConfig.macosDeploymentTarget = "11.0";
    expect(ne(mcpp::build::canonical_configuration_flags(ref, /*targetIsMacos=*/true), mcpp::build::canonical_configuration_flags(changed, /*targetIsMacos=*/true)));

    // AND THE ENV OVERRIDE, folded the same way `deployment_target` resolves
    // it: with MACOSX_DEPLOYMENT_TARGET set, the manifest value stops
    // mattering to the fingerprint too, because it stops mattering to what
    // actually gets compiled.
    ScopedDeploymentTargetEnv env("12.3");
    auto manifestA = base(); manifestA.buildConfig.macosDeploymentTarget = "11.0";
    auto manifestB = base(); manifestB.buildConfig.macosDeploymentTarget = "13.0";
    expect(eq(mcpp::build::canonical_configuration_flags(manifestA, /*targetIsMacos=*/true), mcpp::build::canonical_configuration_flags(manifestB, /*targetIsMacos=*/true)));
}};

// ── cache-mode parsing ──────────────────────────────────────────────────────

UTest cache_mode_parses_the_three_modes{"BuildProfile/CacheModeParsesTheThreeModes", [] {
    using mcpp::build::CacheMode;
    expect(eq(mcpp::build::parse_cache_mode("global"), CacheMode::Global));
    expect(eq(mcpp::build::parse_cache_mode("local"), CacheMode::Local));
    expect(eq(mcpp::build::parse_cache_mode("off"), CacheMode::Off));
    // `none` accepted as a synonym for off; anything else is refused rather
    // than silently meaning "global", which would be the worst default for a
    // typo to land on.
    expect(eq(mcpp::build::parse_cache_mode("none"), CacheMode::Off));
    expect(!(mcpp::build::parse_cache_mode("on").has_value()));
    expect(!(mcpp::build::parse_cache_mode("").has_value()));
    expect(!(mcpp::build::parse_cache_mode("GLOBAL").has_value()));
}};

UTest cache_mode_name_round_trips{"BuildProfile/CacheModeNameRoundTrips", [] {
    using mcpp::build::CacheMode;
    for (auto m : {CacheMode::Global, CacheMode::Local, CacheMode::Off})
        expect(eq(mcpp::build::parse_cache_mode(mcpp::build::cache_mode_name(m)), m));
}};

// ── cache-mode resolution ───────────────────────────────────────────────────
// Same shape as resolve_profile_name and for the same reason: the fast paths
// skip prepare_build, so they must settle the mode from one shared pure rule.
// Without that, a graph generated under `global` (which contains stage_file
// edges reading the cache) got replayed for a request that asked for `local` —
// and ruling the cache out is `local`'s entire purpose.

UTest cache_mode_override_beats_manifest{"BuildProfile/CacheModeOverrideBeatsManifest", [] {
    auto m = base();
    m.buildConfig.cacheMode = "local";
    expect(eq(mcpp::build::resolve_cache_mode(m, "global"), mcpp::build::CacheMode::Global));
}};

UTest cache_mode_manifest_beats_default{"BuildProfile/CacheModeManifestBeatsDefault", [] {
    auto m = base();
    m.buildConfig.cacheMode = "off";
    expect(eq(mcpp::build::resolve_cache_mode(m, ""), mcpp::build::CacheMode::Off));
}};

UTest cache_mode_defaults_to_global{"BuildProfile/CacheModeDefaultsToGlobal", [] {
    expect(eq(mcpp::build::resolve_cache_mode(base(), ""), mcpp::build::CacheMode::Global));
}};

// An unparseable value must not silently mean "global" at THIS level either: it
// falls through to the next source, and prepare_build reports it separately.
UTest unknown_manifest_cache_mode_falls_through_to_default{"BuildProfile/UnknownManifestCacheModeFallsThroughToDefault", [] {
    auto m = base();
    m.buildConfig.cacheMode = "bogus";
    expect(eq(mcpp::build::resolve_cache_mode(m, ""), mcpp::build::CacheMode::Global));
    // ...and an unparseable override does not shadow a valid manifest value.
    m.buildConfig.cacheMode = "local";
    expect(eq(mcpp::build::resolve_cache_mode(m, "bogus"), mcpp::build::CacheMode::Local));
}};

// ── #649 E9: the override the command line states, one rule for every verb ───
//
// `build` preferred `--profile` over the shorthands and `run` preferred the
// shorthands, so one command line built two profiles. Every verb now reads this.
UTest profile_option_beats_the_shorthands{"BuildProfile/ProfileOptionBeatsTheShorthands", [] {
    expect(eq(mcpp::build::profile_override_from_flags("dev", true, false), "dev"));
    expect(eq(mcpp::build::profile_override_from_flags("release", false, true), "release"));
}};

UTest shorthands_name_their_profile{"BuildProfile/ShorthandsNameTheirProfile", [] {
    expect(eq(mcpp::build::profile_override_from_flags("", true, false), "release"));
    expect(eq(mcpp::build::profile_override_from_flags("", false, true), "dev"));
    expect(eq(mcpp::build::profile_override_from_flags("", false, false), ""));
}};

// ── debug = "lines" (2026-11-12 §17, W5) ─────────────────────────────────────

// The line tables are a configuration of their own, and `debug = true` keeps the
// string it had: a directory built before the level existed keeps its name.
UTest debug_lines_is_its_own_configuration{"BuildProfile/DebugLinesIsItsOwnConfiguration", [] {
    auto full = base();  full.buildConfig.debug = true;
    auto lines = full;   lines.buildConfig.debugLines = true;
    const auto f = mcpp::build::canonical_configuration_flags(full);
    expect(ne(f.find(" debug=1 "), std::string::npos)) << f;
    expect(ne(mcpp::build::canonical_configuration_flags(lines), f));
}};

UTest debug_lines_is_read_from_the_profile{"BuildProfile/DebugLinesIsReadFromTheProfile", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "demo"
version = "0.1.0"

[profile.ci]
debug = "lines"
strip = true

[profile.odd]
debug = "full"
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    auto const& ci = m->profiles.at("ci");
    expect(ci.debug && ci.debugLines && ci.debugDeclared);
    // A string other than "lines" is kept as written, for prepare to refuse
    // with the profile's name.
    auto const& odd = m->profiles.at("odd");
    expect(eq(odd.debugWritten, "full"));
    expect(!odd.debug);
}};
