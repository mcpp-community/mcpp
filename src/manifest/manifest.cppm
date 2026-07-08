// mcpp.manifest — manifest data model + the two descriptor formats.
//
// Split into partitions for architectural clarity (one data model, two
// independent surface grammars):
//   :types  shared data model (Manifest, Target, BuildConfig, errors)
//   :toml   mcpp.toml parsing (projects / packages on disk)
//   :xpkg   xpkg .lua `mcpp = {}` segment parsing (index descriptors)

export module mcpp.manifest;

export import :types;
export import :toml;
export import :xpkg;
