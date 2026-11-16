
import std;
import mcpplibs.mtest;
import mcpp.build.prepare_inputs;

using namespace mcpplibs::mtest;

// #630 item 7 — "a selector that names only an operating system is a
// platform" (design record 2026-09-13-630 §8.2). `os_only_platforms` is the
// function `mcpp emit xpkg` (`mcpp::pm::emit_xpkg`) asks to decide whether a
// `[target.<selector>]` conditional tool declaration can be folded into one
// of the descriptor's three platform blocks (`linux`, `macosx`, `windows`)
// instead of only producing the `publish/target-axis-tools` advisory.
//
// Both directions are asserted: the positive claim (the seven forms §8.2
// lists each map onto the block(s) they name) and the negative claim (a
// predicate mentioning anything else — an architecture, an environment, a
// combinator, `not(...)`, even one built entirely from OS terms — is refused,
// which is what keeps the descriptor from claiming an install-time edge on a
// platform the predicate does not unconditionally name).

namespace cfgpred = mcpp::build::cfgpred;

namespace {
bool has(const std::vector<std::string>& v, std::string_view a) {
    return std::ranges::find(v, a) != v.end();
}
}  // namespace

// ── Positive: the seven OS-only forms ───────────────────────────────────────

UTest bare_alias_windows{"OsOnlyPlatforms/BareAliasWindows", [] {
    auto p = cfgpred::os_only_platforms("windows");
    expect(eq(p, std::vector<std::string>({"windows"})));
}};

UTest cfg_windows{"OsOnlyPlatforms/CfgWindows", [] {
    auto p = cfgpred::os_only_platforms("cfg(windows)");
    expect(eq(p, std::vector<std::string>({"windows"})));
}};

UTest cfg_os_equals_linux{"OsOnlyPlatforms/CfgOsEqualsLinux", [] {
    auto p = cfgpred::os_only_platforms(R"(cfg(os = "linux"))");
    expect(eq(p, std::vector<std::string>({"linux"})));
}};

UTest cfg_linux{"OsOnlyPlatforms/CfgLinux", [] {
    auto p = cfgpred::os_only_platforms("cfg(linux)");
    expect(eq(p, std::vector<std::string>({"linux"})));
}};

UTest cfg_macos{"OsOnlyPlatforms/CfgMacos", [] {
    auto p = cfgpred::os_only_platforms("cfg(macos)");
    // The descriptor's spelling is `macosx`, not `macos` — see
    // `mcpp::pm::emit_xpkg`'s three blocks.
    expect(eq(p, std::vector<std::string>({"macosx"})));
}};

UTest cfg_os_equals_windows{"OsOnlyPlatforms/CfgOsEqualsWindows", [] {
    auto p = cfgpred::os_only_platforms(R"(cfg(os = "windows"))");
    expect(eq(p, std::vector<std::string>({"windows"})));
}};

UTest cfg_os_equals_macos{"OsOnlyPlatforms/CfgOsEqualsMacos", [] {
    auto p = cfgpred::os_only_platforms(R"(cfg(os = "macos"))");
    expect(eq(p, std::vector<std::string>({"macosx"})));
}};

UTest cfg_unix_names_both_mac_and_linux{"OsOnlyPlatforms/CfgUnixNamesBothMacAndLinux", [] {
    // `unix` means `family == "unix"`, which macOS and Linux both satisfy —
    // the only one of the seven forms that names two blocks at once.
    auto p = cfgpred::os_only_platforms("cfg(unix)");
    expect(eq(p.size(), 2u));
    expect(bool(has(p, "linux")));
    expect(bool(has(p, "macosx")));
    expect(!has(p, "windows"));
}};

UTest bare_alias_unix{"OsOnlyPlatforms/BareAliasUnix", [] {
    auto p = cfgpred::os_only_platforms("unix");
    expect(eq(p.size(), 2u));
    expect(bool(has(p, "linux")));
    expect(bool(has(p, "macosx")));
}};

// ── Negative: everything the record says keeps the warning ─────────────────

UTest architecture_is_not_os_only{"OsOnlyPlatforms/ArchitectureIsNotOsOnly", [] {
    auto p = cfgpred::os_only_platforms(R"(cfg(target_arch = "aarch64"))");
    expect(p.empty());
}};

UTest combinator_of_os_and_arch_is_not_os_only{"OsOnlyPlatforms/CombinatorOfOsAndArchIsNotOsOnly", [] {
    auto p = cfgpred::os_only_platforms(
        R"(cfg(all(linux, target_arch = "x86_64")))");
    expect(p.empty());
}};

UTest not_windows_is_not_os_only{"OsOnlyPlatforms/NotWindowsIsNotOsOnly", [] {
    // Syntactically this names only an OS, but `not(...)` answers an
    // unbounded set of platforms (everything this vocabulary does not yet
    // call "windows"), not the two blocks "linux and macosx" would be if this
    // were treated as their union. Kept conservative: any combinator
    // disqualifies, `not` included.
    auto p = cfgpred::os_only_platforms("cfg(not(windows))");
    expect(p.empty());
}};

UTest env_is_not_os_only{"OsOnlyPlatforms/EnvIsNotOsOnly", [] {
    auto p = cfgpred::os_only_platforms(R"(cfg(env = "android"))");
    expect(p.empty());
}};

UTest layer_key_is_not_os_only{"OsOnlyPlatforms/LayerKeyIsNotOsOnly", [] {
    auto p = cfgpred::os_only_platforms(R"(cfg(compiler = "llvm"))");
    expect(p.empty());
}};

UTest an_all_os_combinator_is_still_not_os_only{"OsOnlyPlatforms/AnAllOsCombinatorIsStillNotOsOnly", [] {
    // Even a combinator built ENTIRELY from OS terms is refused — see the
    // comment on `os_only_platforms` for why `any(linux, macos)` is not the
    // same claim as either block alone.
    auto p = cfgpred::os_only_platforms("cfg(any(linux, macos))");
    expect(p.empty());
}};

UTest exact_triple_is_not_os_only{"OsOnlyPlatforms/ExactTripleIsNotOsOnly", [] {
    // An exact triple shares no grammar with cfg()/the bare aliases; it names
    // one target, not a platform.
    auto p = cfgpred::os_only_platforms("x86_64-linux-musl");
    expect(p.empty());
}};

UTest unrecognised_os_value_is_not_os_only{"OsOnlyPlatforms/UnrecognisedOsValueIsNotOsOnly", [] {
    auto p = cfgpred::os_only_platforms(R"(cfg(os = "freebsd"))");
    expect(p.empty());
}};
