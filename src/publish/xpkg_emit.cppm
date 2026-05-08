// mcpp.publish.xpkg_emit — backward-compat shim. The implementation has
// moved to `mcpp.pm.publisher` as part of the package-management
// subsystem refactor (PR-R6 in
// `.agents/docs/2026-05-08-pm-subsystem-architecture.md`).
//
// Existing call sites continue to use `mcpp::publish::ReleaseInfo`,
// `mcpp::publish::emit_xpkg`, etc. — the using-declarations below
// introduce those names into the legacy namespace as alternates for
// the new `mcpp::pm::*` symbols (no new functions — keeps ADL
// unambiguous when callers pass `ReleaseInfo` whose own namespace is
// `mcpp::pm`). The shim disappears once `cli.cppm` migrates to the
// `mcpp::pm::` qualified names.

export module mcpp.publish.xpkg_emit;

import std;
export import mcpp.pm.publisher;

export namespace mcpp::publish {

using ReleaseInfo = mcpp::pm::ReleaseInfo;

using mcpp::pm::emit_xpkg;
using mcpp::pm::placeholder_release;
using mcpp::pm::release_tarball_url;
using mcpp::pm::sha256_of_file;
using mcpp::pm::make_release_tarball;
using mcpp::pm::make_release_info;

} // namespace mcpp::publish
