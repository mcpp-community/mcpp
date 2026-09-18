// mcpp.toolchain.cenv_probe — "a declaration is checked, never trusted"
// (design 2026-09-18 §3.2), exercised against a REAL compiler.
//
// `tests/unit/test_cenv.cpp` covers `mcpp.toolchain.cenv::realise` as a pure
// function; `tests/e2e/741_...sh`'s leg A only proves the probe does not
// reject a MATCHING declaration (the build succeeds), and leg B's refusal
// comes from `cenv::realise` itself refusing an unrealisable REQUEST before
// the probe is ever reached — neither exercises what happens when the probe
// actually MEASURES a disagreement. This file is that missing piece: it
// calls `cenv_probe::verify` directly, bypassing `cenv::realise` entirely, so
// the "declared" side can be deliberately wrong regardless of whether any
// (request, target) pair in `cenv::realise`'s own mapping table could
// produce it — the module under test does not know or care where its
// expectations came from, and neither does this test.
//
// NO `--target=` HERE ON PURPOSE. `cenv_probe::verify` itself is
// compiler-family-agnostic (it just runs `-E -dM`); only `cenv::realise`'s
// OWN Cygwin-substitution tokens are Clang-specific, and prepare.cppm gates
// those behind `is_clang` separately. Probing a plain host compile with no
// extra tokens keeps this test honest against whatever C++ compiler the
// test-running machine has (gcc or clang), rather than requiring the
// project's own resolved llvm payload to exist at a guessable path.
//
// EVERY TEST GETS ITS OWN CACHE DIRECTORY. The dump the probe caches is
// keyed on (compiler, argv) alone — not on what expectations it is compared
// against, since the expectations are applied in memory to whatever dump
// comes back. Two tests that probe the identical (compiler, empty argv)
// against the SAME default cache root would have the second one silently
// read the first one's cached dump (`ran = false`) rather than genuinely
// probe — correct behaviour for the module, but it makes the test suite's
// outcome depend on run order and on what a previous run left on disk.
// Passing a fresh temp directory per test is what `verify`'s own `cacheRoot`
// parameter exists for; using it here is not a workaround, it is the
// intended way to keep two callers' cached dumps apart.

#include <gtest/gtest.h>

import std;
import mcpp.toolchain.cenv_probe;

namespace cp = mcpp::toolchain::cenv_probe;

namespace {

// The narrowest possible "does a working C++ compiler exist on this
// machine" check. `cenv_probe::verify` takes a real filesystem path (it
// calls `last_write_time` on it for the cache key), not a bare command name
// relying on PATH resolution inside the child process — so this resolves
// one, rather than handing the module something it was never asked to
// resolve itself.
std::filesystem::path find_a_cxx_compiler() {
    static constexpr std::string_view kCandidates[] = {
        "/usr/bin/c++", "/usr/bin/g++", "/usr/bin/clang++",
    };
    for (auto c : kCandidates) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec)) return c;
    }
    return {};
}

std::filesystem::path cxx() {
    static const std::filesystem::path p = find_a_cxx_compiler();
    return p;
}

// The host's own word size, measured the same way `cenv::realise`'s callers
// would declare it — this test's "declared" values are deliberately chosen
// relative to the REAL host, so a correct declaration is one that could
// plausibly ever be written, not a magic constant.
constexpr int kHostLongBytes = sizeof(long);
constexpr int kHostWcharBits = sizeof(wchar_t) * 8;

// A fresh, per-test cache directory — see the file header. Removed on
// destruction so a failed run does not leave temp directories behind.
struct TmpCache {
    std::filesystem::path dir = std::filesystem::temp_directory_path()
        / std::format("mcpp-cenv-probe-test-{}", std::random_device{}());
    ~TmpCache() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
};

} // namespace

TEST(CenvProbe, AMatchingDeclarationProducesNoMismatches) {
    if (cxx().empty()) GTEST_SKIP() << "no C++ compiler found to probe";
    TmpCache cache;
    auto r = cp::verify(cxx(), {}, kHostWcharBits, kHostLongBytes, {}, {},
                        cache.dir);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_TRUE(r->ran) << "a fresh cache directory must actually probe";
    EXPECT_TRUE(r->mismatches.empty());
}

// THE CASE THE COORDINATOR ASKED TO SEE: a declared word width the compiler
// disagrees with. This is exactly what a C library's [c-abi] block getting
// `data-model` wrong would produce once it reached the probe, independent
// of whether `cenv::realise`'s own mapping table would ever have accepted
// that request in the first place.
TEST(CenvProbe, ADeclaredLongWidthTheCompilerDisagreesWithIsAMismatch) {
    if (cxx().empty()) GTEST_SKIP() << "no C++ compiler found to probe";
    TmpCache cache;
    const int wrongLongBytes = kHostLongBytes == 8 ? 4 : 8;
    auto r = cp::verify(cxx(), {}, 0, wrongLongBytes, {}, {}, cache.dir);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_TRUE(r->ran);
    ASSERT_EQ(r->mismatches.size(), 1u);
    EXPECT_EQ(r->mismatches[0].fact, "sizeof(long)");
    EXPECT_EQ(r->mismatches[0].declared, std::to_string(wrongLongBytes));
    EXPECT_EQ(r->mismatches[0].measured, std::to_string(kHostLongBytes));
}

TEST(CenvProbe, ADeclaredWcharWidthTheCompilerDisagreesWithIsAMismatch) {
    if (cxx().empty()) GTEST_SKIP() << "no C++ compiler found to probe";
    TmpCache cache;
    const int wrongWcharBits = kHostWcharBits == 32 ? 16 : 32;
    auto r = cp::verify(cxx(), {}, wrongWcharBits, 0, {}, {}, cache.dir);
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->mismatches.size(), 1u);
    EXPECT_EQ(r->mismatches[0].fact, "__SIZEOF_WCHAR_T__ (bits)");
    EXPECT_EQ(r->mismatches[0].declared, std::to_string(wrongWcharBits));
    EXPECT_EQ(r->mismatches[0].measured, std::to_string(kHostWcharBits));
}

// `expectDefined` names a macro the declaration says must be there and
// isn't — the shape a `presents = "posix"` declaration takes when the
// compiler did not actually end up defining `__unix__`.
TEST(CenvProbe, AMacroDeclaredDefinedButAbsentIsAMismatch) {
    if (cxx().empty()) GTEST_SKIP() << "no C++ compiler found to probe";
    TmpCache cache;
    auto r = cp::verify(cxx(), {}, 0, 0,
                        {"__MCPP_TEST_MACRO_THAT_DOES_NOT_EXIST__"}, {},
                        cache.dir);
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->mismatches.size(), 1u);
    EXPECT_EQ(r->mismatches[0].fact,
             "__MCPP_TEST_MACRO_THAT_DOES_NOT_EXIST__");
    EXPECT_EQ(r->mismatches[0].declared, "defined");
    EXPECT_EQ(r->mismatches[0].measured, "undefined");
}

// The reverse: a macro the declaration says must be ABSENT but the compiler
// defines anyway — the shape `presents = "posix"` on a target whose
// compiler still predefines `_WIN32` would take. `__cplusplus` is always
// defined by a working C++ compiler, so this needs no special setup.
TEST(CenvProbe, AMacroDeclaredUndefinedButPresentIsAMismatch) {
    if (cxx().empty()) GTEST_SKIP() << "no C++ compiler found to probe";
    TmpCache cache;
    auto r = cp::verify(cxx(), {}, 0, 0, {}, {"__cplusplus"}, cache.dir);
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->mismatches.size(), 1u);
    EXPECT_EQ(r->mismatches[0].fact, "__cplusplus");
    EXPECT_EQ(r->mismatches[0].declared, "undefined");
    EXPECT_EQ(r->mismatches[0].measured, "defined");
}

// CACHED PER CONFIGURATION (module header) — a second call with the
// identical compiler + argv must not recompile. Distinguished from the
// first call via `ran`, exactly as `Result::ran`'s own doc comment says: a
// cache hit still answers the mismatch question, it just does no work to.
TEST(CenvProbe, TheSecondCallForTheIdenticalConfigurationIsACacheHit) {
    if (cxx().empty()) GTEST_SKIP() << "no C++ compiler found to probe";
    TmpCache cache;
    auto first = cp::verify(cxx(), {}, kHostWcharBits, kHostLongBytes, {}, {},
                            cache.dir);
    ASSERT_TRUE(first.has_value()) << first.error();
    EXPECT_TRUE(first->ran);

    auto second = cp::verify(cxx(), {}, kHostWcharBits, kHostLongBytes, {}, {},
                             cache.dir);
    ASSERT_TRUE(second.has_value()) << second.error();
    EXPECT_FALSE(second->ran) << "identical compiler + argv must hit the cache";
    EXPECT_TRUE(second->mismatches.empty());
}

// TWO different argv strings must NOT share a cache slot — the whole point
// of hashing argv into the key. A wrong hit here would mean this build's
// probe silently answered a DIFFERENT configuration's question.
TEST(CenvProbe, DifferentArgvDoesNotShareACacheSlot) {
    if (cxx().empty()) GTEST_SKIP() << "no C++ compiler found to probe";
    TmpCache cache;
    auto a = cp::verify(cxx(), {"-DMCPP_PROBE_TEST_A=1"}, 0, 0, {}, {}, cache.dir);
    auto b = cp::verify(cxx(), {"-DMCPP_PROBE_TEST_B=1"}, 0, 0, {}, {}, cache.dir);
    ASSERT_TRUE(a.has_value()) << a.error();
    ASSERT_TRUE(b.has_value()) << b.error();
    EXPECT_TRUE(a->ran);
    EXPECT_TRUE(b->ran) << "a different argv must not read A's cache entry";
}
