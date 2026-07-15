// mcpp.toolchain.compat — legacy-spelling compatibility layer.
//
// THE ONLY FILE that knows pre-0.0.93 toolchain/triple spellings. Core code
// (registry, prepare, lifecycle) sees canonical forms exclusively; the two
// public parse entry points call normalize_* first. Deleting this module
// would break exactly one thing: old inputs — never a canonical path.
//
// Owns four responsibilities (design §4.7):
//   1. spec aliases     musl-gcc@V / gcc@V-musl / <triple>-gcc@V / mingw@V /
//                       mingw-cross@V / clang@V → (family, version, target)
//   2. triple aliases   handled by triple::parse itself (GNU/LLVM/Apple
//                       spellings are grammar, not legacy) — compat only
//                       decides WHEN a compiler token is really a triple
//   3. persisted-state migration: old config/manifest spec strings normalize
//                       on the read path via the same normalize_spec
//   4. the one-line canonical hint text (printed at most once per process)
//
// NOT compat: xim package names (mingw-cross-gcc, musl-gcc, …). Those are
// the distribution layer's CURRENT identity — "cross" is legitimate there
// (musl.cc's -cross tarballs, Debian's g++-mingw-w64 precedent) — and they
// are produced by registry.cppm's payload mapping, not parsed from users.
//
// See .agents/docs/2026-07-15-toolchain-target-naming-unification-design.md.

module;
#include <cstdio>

export module mcpp.toolchain.compat;

import std;
import mcpp.platform;
import mcpp.toolchain.triple;

export namespace mcpp::toolchain::compat {

// A user/config spec token pair, normalized to the two-axis identity model.
struct NormalizedSpec {
    std::string    family;    // "gcc" | "llvm" | "msvc"
    std::string    version;   // numeric (possibly partial), or "system"; never "-musl"-suffixed
    triple::Triple target;    // empty = host
    // Set when a legacy spelling was rewritten; `hint` is the one-line note.
    bool           changed = false;
    std::string    hint;
};

// Normalize a (compiler, version) token pair. The caller has already split
// a combined "name@ver" form. Returns nullopt for a compiler token outside
// the family set and its aliases — the caller owns the error message.
std::optional<NormalizedSpec> normalize_spec(std::string_view compiler,
                                             std::string_view version);

// Print a normalization hint at most once per process (quiet by design:
// note-level, aliases are permanently supported — this is a pointer to the
// canonical spelling, not a deprecation warning).
void print_hint_once(std::string_view hint);

} // namespace mcpp::toolchain::compat

namespace mcpp::toolchain::compat {

namespace {

bool ends_with(std::string_view s, std::string_view suf) {
    return s.size() >= suf.size()
        && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

std::string_view strip_namespace(std::string_view compiler) {
    if (auto colon = compiler.find(':'); colon != std::string_view::npos)
        return compiler.substr(colon + 1);
    return compiler;
}

triple::Triple host_musl_triple() {
    triple::Triple t;
    t.arch = std::string(mcpp::platform::host_arch);
    t.os   = "linux";
    t.env  = "musl";
    return t;
}

triple::Triple windows_gnu_triple() {
    return { "x86_64", "windows", "gnu" };
}

NormalizedSpec with_hint(NormalizedSpec s, std::string_view oldSpelling) {
    s.changed = true;
    std::string canonical = std::format("{}@{}", s.family,
        s.version.empty() ? std::string("<version>") : s.version);
    if (!s.target.empty())
        canonical += std::format(" targeting '{}'", s.target.str());
    s.hint = std::format("'{}' is now {}", oldSpelling, canonical);
    return s;
}

} // namespace

std::optional<NormalizedSpec> normalize_spec(std::string_view compilerIn,
                                             std::string_view versionIn) {
    std::string_view compiler = strip_namespace(compilerIn);
    std::string version(versionIn);

    // Legacy: musl flavor as a version suffix ("gcc@15.1.0-musl", "15-musl").
    bool muslVersionSuffix = ends_with(version, "-musl");
    if (muslVersionSuffix) version.resize(version.size() - 5);

    NormalizedSpec out;
    out.version = version;

    // ── canonical families pass through ─────────────────────────────────────
    if (compiler == "gcc" || compiler == "llvm" || compiler == "msvc") {
        out.family = std::string(compiler);
        if (muslVersionSuffix && compiler == "gcc") {
            out.target = host_musl_triple();
            return with_hint(std::move(out),
                std::format("{}@{}-musl", compiler, version));
        }
        if (muslVersionSuffix) return std::nullopt;  // llvm/msvc have no musl flavor
        return out;
    }

    // ── legacy spellings ─────────────────────────────────────────────────────
    if (compiler == "clang") {                      // alias family → llvm
        out.family = "llvm";
        return with_hint(std::move(out), std::format("clang@{}", version));
    }
    if (compiler == "musl-gcc") {                   // musl as compiler-name prefix
        out.family = "gcc";
        out.target = host_musl_triple();
        return with_hint(std::move(out), std::format("musl-gcc@{}", version));
    }
    if (compiler == "mingw" || compiler == "mingw-cross"
        || compiler == "mingw-gcc" || compiler == "mingw-cross-gcc") {
        // One concept, two host-split legacy names: GCC targeting Windows PE
        // (GNU CRT). Which payload serves it (native winlibs vs Linux-hosted
        // cross) is decided by registry's payload mapping from the HOST —
        // never by the name the user typed.
        out.family = "gcc";
        out.target = windows_gnu_triple();
        return with_hint(std::move(out), std::format("{}@{}", compiler, version));
    }
    if (ends_with(compiler, "-gcc")) {              // triple-named: aarch64-linux-musl-gcc
        auto tripleStr = compiler.substr(0, compiler.size() - 4);
        if (auto t = triple::parse(tripleStr)) {
            out.family = "gcc";
            out.target = *t;
            return with_hint(std::move(out), std::format("{}@{}", compiler, version));
        }
    }

    return std::nullopt;
}

void print_hint_once(std::string_view hint) {
    if (hint.empty()) return;
    static bool printed = false;
    if (printed) return;
    printed = true;
    std::println(stderr, "note: {}", hint);
}

} // namespace mcpp::toolchain::compat
