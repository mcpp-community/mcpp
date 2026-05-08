// mcpp.pm.index_spec — package-index repository configuration.
//
// Reserved for the upcoming `[indices]` parsing & IndexSpec data type;
// see `.agents/docs/2026-05-08-package-index-config.md` for the full
// design. The module placeholder is created early so the rest of the
// pm/ subsystem can land its imports against a stable module path
// while the implementation arrives.

export module mcpp.pm.index_spec;

import std;

export namespace mcpp::pm {

// Placeholder. The full `IndexSpec` (url / rev / tag / branch / path)
// + `[indices]` TOML parsing lands in a dedicated PR per the
// package-index-config design doc.

} // namespace mcpp::pm
