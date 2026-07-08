// mcpp.manifest — manifest data model + the two descriptor formats.
//
// Umbrella module: one data model, two independent surface grammars.
// Split as SEPARATE modules (not partitions): GCC 15's partition handling
// drops implicit template instantiations of module-attached types
// (std::map<..., ScanOverride> etc.) from partition objects, breaking the
// aarch64 cross link; plain modules re-exported here behave identically
// for importers and link everywhere.
//
//   mcpp.manifest.types  shared data model (Manifest, Target, errors)
//   mcpp.manifest.toml   mcpp.toml parsing (projects / packages on disk)
//   mcpp.manifest.xpkg   xpkg .lua `mcpp = {}` segment (index descriptors)

export module mcpp.manifest;

export import mcpp.manifest.types;
export import mcpp.manifest.toml;
export import mcpp.manifest.xpkg;
