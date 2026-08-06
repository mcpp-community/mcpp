// mcpp.version — this binary's own version, and nothing else.
//
// WHY IT IS ITS OWN MODULE
//
// The constant used to live in mcpp.toolchain.fingerprint, next to the struct
// that folds it into the BMI cache key. That is a reasonable place for a
// *consumer* of the version and a bad place for the version itself: fingerprint
// imports mcpp.toolchain.detect, which imports mcpp.xlings, so "what version is
// this binary" transitively dragged in the entire toolchain-detection and
// package-manager subsystem.
//
// That is not a stylistic complaint. mcpp.pm.index_contract needs the version
// for one comparison (is this binary new enough for this index?), and pulling
// it from fingerprint made index_contract depend on xlings — which made it
// impossible for xlings to depend on index_contract, which is exactly where the
// index-refresh guard has to live (mcpp.pm.index_snapshot, and see
// mcpp::xlings::update_index). The cycle was the layering telling us the
// constant was in the wrong place.
//
// A leaf module with no imports beyond `std` can be used by anyone. Keep it
// that way: nothing else belongs in this file.
//
// SINGLE SOURCE OF TRUTH. `.github/tools/check_version_pins.sh` reads the
// literal below and cross-checks it against `mcpp.toml`'s `[package].version`;
// tests/e2e/01_help_and_version.sh checks it against `mcpp --version` at
// runtime. Both must be updated together — see docs/09-release.md.

export module mcpp.version;

import std;

export namespace mcpp {

inline constexpr std::string_view MCPP_VERSION = "2026.8.6.1";

} // namespace mcpp
