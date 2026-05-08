// mcpp.lockfile — backward-compat shim. The implementation has moved
// to `mcpp.pm.lock_io` as part of the package-management subsystem
// refactor (PR-R2 in
// `.agents/docs/2026-05-08-pm-subsystem-architecture.md`).
//
// All existing callers continue to use `mcpp::lockfile::Lockfile`,
// `mcpp::lockfile::load`, etc. — the aliases below resolve those to
// the new `mcpp::pm` types. Once `cli.cppm` migrates to `mcpp::pm::`
// directly this shim can be deleted.

export module mcpp.lockfile;

import std;
export import mcpp.pm.lock_io;

export namespace mcpp::lockfile {

using LockedPackage = mcpp::pm::LockedPackage;
using Lockfile      = mcpp::pm::Lockfile;
using LockError     = mcpp::pm::LockError;

inline std::expected<Lockfile, LockError>
load(const std::filesystem::path& path) {
    return mcpp::pm::load(path);
}

inline std::expected<void, LockError>
write(const Lockfile& lock, const std::filesystem::path& path) {
    return mcpp::pm::write(lock, path);
}

inline std::string serialize(const Lockfile& lock)    { return mcpp::pm::serialize(lock); }
inline std::string compute_hash(const Lockfile& lock) { return mcpp::pm::compute_hash(lock); }

} // namespace mcpp::lockfile
