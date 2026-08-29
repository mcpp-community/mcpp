// mcpp.platform.runtime_env_contract — where a runtime search directory is
// allowed to be published.
//
// There are two ways to tell a dynamic loader where to look, and they have
// very different blast radii:
//
//   DT_RUNPATH        baked into ONE object. Reaches that object's own
//                     DT_NEEDED closure and any dlopen() it performs. Nothing
//                     else on the machine can observe it.
//
//   LD_LIBRARY_PATH   an environment variable. Reaches the target process AND
//                     every process it ever spawns, transitively, forever —
//                     including binaries mcpp never built, loaded by a loader
//                     mcpp never chose.
//
// That second property is not a detail. mcpp ships a PRIVATE glibc, and a
// glibc's `libc.so.6` and its `ld.so` are version-locked to each other through
// GLIBC_PRIVATE symbols: 2.44's libc.so.6 carries an undefined reference to
// `__pointer_chk_guard`, which only 2.44's own `ld-linux-x86-64.so.2` exports.
// An mcpp-built program is fine — its PT_INTERP names the private loader. But
// a program that calls popen()/system() spawns `/bin/sh`, whose PT_INTERP
// names the HOST loader and cannot be overridden by any environment variable.
// If LD_LIBRARY_PATH points that host loader at the private libc, the shell
// dies during relocation, before main:
//
//   sh: symbol lookup error: …/xim-x-glibc/2.44/lib64/libc.so.6:
//       undefined symbol: __pointer_chk_guard, version GLIBC_PRIVATE
//
// mcpp#401. The same shape, one hop closer in, was mcpp#291 (a nested mcpp's
// own host tools). Both are the same mistake: publishing a private-libc search
// path through a channel that does not stop at the process that needs it.
//
// So the rule is scoped, not conditional — "only export it when a dependency
// might dlopen" still exports it, and the failing child does not care why. A
// private libc directory is BINARY-scoped: it is written into the artifacts
// mcpp itself links, and never into the environment. dlopen() from the program
// still resolves, because the loader consults the calling object's DT_RUNPATH.
//
// Note the asymmetry with ordinary dependency runtime directories: those are
// plain shared libraries with no loader coupling, so a host binary that
// stumbles onto them is at worst confused, not killed. They keep their
// existing environment scope.

export module mcpp.platform.runtime_env_contract;

import std;

export namespace mcpp::platform {

enum class RuntimeSearchScope {
    Binary,       // DT_RUNPATH on the objects mcpp links
    Environment,  // LD_LIBRARY_PATH, inherited by the whole process subtree
};

// The private libc payload. See the module comment for why this is not a
// tunable: no build-level condition can make an inherited variable safe for a
// process that mcpp did not launch and cannot see.
inline constexpr RuntimeSearchScope kPrivateLibcSearchScope =
    RuntimeSearchScope::Binary;

constexpr bool publishes_via_environment(RuntimeSearchScope scope) {
    return scope == RuntimeSearchScope::Environment;
}

constexpr bool publishes_via_binary(RuntimeSearchScope scope) {
    return scope == RuntimeSearchScope::Binary;
}

} // namespace mcpp::platform
