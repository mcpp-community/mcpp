// mcpp.pm — package-management subsystem façade.
//
// This is the single import point that callers in `cli.cppm`,
// `build/`, `pack/`, etc. should use to reach pm types and high-level
// operations. The internal modules (`pm.resolver`, `pm.commands`,
// `pm.package_fetcher`, `pm.publisher`) stay deliberately out of the
// public surface — re-exporting them would invite call sites to
// reach into pm internals and undo the encapsulation gained by R1–R6.
//
// What this module re-exports:
//
//   * Data types only:
//       - `mcpp::pm::DependencySpec`         (from pm.dep_spec)
//       - `mcpp::pm::kDefaultNamespace`      (from pm.dep_spec)
//       - everything currently in pm.index_spec (placeholder; the
//         index-config implementation will land here)
//       - `mcpp::pm::Lockfile` and friends   (from pm.lock_io)
//
// Internal pm modules (resolver, package_fetcher, commands, publisher)
// MUST be imported directly by callers that need them; they are
// intentionally **not** re-exported here. As the call-site migration
// progresses, an additional high-level operation
//   `prepare_dependencies(Manifest&, Lockfile&, ...)` → vector<...>
// will appear here, replacing the ~250-line dependency-resolution
// loop currently inlined in `cli.cppm::prepare_build`. That landing
// is tracked separately so this PR stays a pure additive facade.
//
// See `.agents/docs/2026-05-08-pm-subsystem-architecture.md` §4.10.

export module mcpp.pm;

export import mcpp.pm.dep_spec;
export import mcpp.pm.index_spec;
export import mcpp.pm.lock_io;
