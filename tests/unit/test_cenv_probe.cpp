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
import mcpp.platform;

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

// A CLANG, FOR THE TWO TESTS THAT NEED ONE, AND FOUND ON PATH RATHER THAN AT A
// FIXED LOCATION.
//
// `find_a_cxx_compiler` answers `/usr/bin/c++` first, which is GCC on every
// host this suite runs on --- and GCC takes no `--target`. The two tests below
// therefore skipped on EVERY host, including the CI shards that have an LLVM
// toolchain, which is the shape of coverage that looks like coverage and is
// not. Searching PATH makes them run wherever a clang is reachable; where none
// is, they skip and say so, and `CenvProbeArgv` still pins the invariant with
// no compiler at all.
// Whether this compiler both takes `--target` and has the back end the two
// tests below name. A clang that answers for neither is not a smaller version
// of one that does; it is a different tool for this purpose.
bool answers_for_a_cross_target(const std::filesystem::path& bin) {
    auto r = mcpp::platform::process::capture_stdout(
        {bin.string(), "--target=riscv64-none-elf", "-x", "c++", "-E", "-dM", "-"});
    return r.exit_code == 0
        && r.output.find("#define __riscv ") != std::string::npos;
}

std::filesystem::path find_a_clang() {
    if (const char* env = std::getenv("MCPP_TEST_CLANGXX"); env && *env)
        return env;
    const char* path = std::getenv("PATH");
    if (!path) return {};
    std::string_view rest{path};
#ifdef _WIN32
    constexpr char kSep = ';';
    constexpr std::string_view kName = "clang++.exe";
#else
    constexpr char kSep = ':';
    constexpr std::string_view kName = "clang++";
#endif
    while (!rest.empty()) {
        auto at = rest.find(kSep);
        auto dir = rest.substr(0, at);
        rest = (at == std::string_view::npos) ? std::string_view{}
                                               : rest.substr(at + 1);
        if (dir.empty()) continue;
        std::error_code ec;
        auto candidate = std::filesystem::path(dir) / kName;
        if (std::filesystem::exists(candidate, ec) && answers_for_a_cross_target(candidate))
            return candidate;
    }
    // AND THE PAYLOAD mcpp ITSELF INSTALLS. A clang on PATH is not
    // necessarily one built with the back end this test names --- the host
    // this was written on carries a vendor clang with neither RISC-V nor
    // AArch64, so a PATH-only search skipped for a reason that has nothing to
    // do with what is being tested. mcpp's own LLVM payload has them, and
    // every CI shard that resolves an `llvm@` toolchain has downloaded it.
    std::error_code ec;
    auto home = std::getenv("MCPP_HOME")
              ? std::filesystem::path(std::getenv("MCPP_HOME"))
              : std::filesystem::path(
                    std::getenv("HOME") ? std::getenv("HOME") : ".") / ".mcpp";
    auto payloads = home / "registry" / "data" / "xpkgs" / "xim-x-llvm";
    if (std::filesystem::is_directory(payloads, ec))
        for (auto const& ver : std::filesystem::directory_iterator(payloads, ec)) {
            auto candidate = ver.path() / "bin" / kName;
            if (std::filesystem::exists(candidate, ec)
                && answers_for_a_cross_target(candidate))
                return candidate;
        }
    return {};
}

std::filesystem::path clangxx() {
    static const std::filesystem::path p = find_a_clang();
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

// THE EXACT TOKEN `cenv::realise` NOW PRODUCES FOR macOS AND A FREESTANDING
// TARGET (design §3.3, coordinator revision) — `-D__unix__` — PASSED TO A
// REAL COMPILER, PROVING THE PROBE AGREES WITH IT. `test_cenv.cpp`'s
// `MacosPosixArchDefaultDefinesUnix`/`FreestandingPosixDefinesUnix` assert
// `cenv::realise` PRODUCES this token and this expectation pair, as a pure
// function; this asserts the SAME token and expectation pair, handed to an
// ACTUAL compiler, produce no mismatch — the half `cenv::realise` cannot
// check on its own. `-D__unix__` is an ordinary preprocessor define with no
// compiler-family or platform dependence, so this needs no real macOS or
// freestanding target to prove the mechanism holds: this test's own host
// (whatever it is — a Linux CI runner's compiler already defines `__unix__`
// on its own, unlike macOS or a freestanding target, but `-D__unix__` is
// harmlessly redundant there rather than wrong, and the assertion is exactly
// the same declared/measured pair regardless) still ends up matching what
// was declared.
TEST(CenvProbe, TheDUnixTokenCenvRealiseProducesForMacosAndFreestandingSatisfiesItsOwnExpectation) {
    if (cxx().empty()) GTEST_SKIP() << "no C++ compiler found to probe";
    TmpCache cache;
    auto r = cp::verify(cxx(), {"-D__unix__"}, 0, 0, {"__unix__"}, {"_WIN32"},
                        cache.dir);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_TRUE(r->mismatches.empty())
        << (r->mismatches.empty() ? "" : r->mismatches[0].fact);
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

// ── assemble_argv — the invariant, reachable without a cross toolchain ────
//
// The defect these pin shipped in 2026.9.18.3: the probe ran with no target
// selection on every freestanding build and clang answered for the host. The
// pieces are all legitimately empty in some configuration, so the invariant
// cannot live at the call site; it lives in the assembly, and these tests
// reach it with no compiler at all.

TEST(CenvProbeArgv, AFreestandingTargetWithoutASelectionIsRefused) {
    // Exactly the 2026.9.18.3 shape: `crossTargetFlag` empty (it is set for
    // hosted targets only), no freestanding flags supplied, and `realise`'s
    // freestanding output, which carries no `--target`.
    auto r = cp::assemble_argv("", {}, {"-D__unix__", "-fno-short-wchar"}, {},
                               /*freestanding=*/true, "riscv64-none-elf");
    ASSERT_FALSE(r.has_value())
        << "an argv with no target selection must be refused for a "
           "freestanding target; it measures the build host";
    // THE CLAUSE, NOT ONLY THE TRIPLE. Two other refusals this wave added were
    // found malformed by rendering them: one lost a closing parenthesis, and
    // one read as the claim it denies. Both had tests asserting an identifier
    // inside the sentence, and an identifier sits in the right place in a
    // sentence that is wrong. This message has one substitution and no paired
    // delimiters, so it has no such failure mode -- the assertion is widened
    // anyway, because that is a property of today's wording rather than of
    // the check.
    EXPECT_NE(r.error().find("would have run with no target selection"),
              std::string::npos) << r.error();
    EXPECT_NE(r.error().find("riscv64-none-elf"), std::string::npos)
        << "the refusal must name the target it was for: " << r.error();
}

TEST(CenvProbeArgv, AFreestandingTargetWithItsCompilePrefixIsAccepted) {
    std::vector<std::string> fs{"--target=riscv64-none-elf", "-march=rv64gc",
                                "-mabi=lp64d", "-ffreestanding"};
    auto r = cp::assemble_argv("", fs, {"-D__unix__", "-fno-short-wchar"}, {},
                               true, "riscv64-none-elf");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_NE(std::find(r->begin(), r->end(), "--target=riscv64-none-elf"),
              r->end());
    // The ISA flags travel with the target: they change what the compiler
    // predefines (`__riscv_xlen`, the float ABI), so an argv that took the
    // triple and left them behind would measure a different machine again.
    EXPECT_NE(std::find(r->begin(), r->end(), "-mabi=lp64d"), r->end());
}

TEST(CenvProbeArgv, ANativeHostedBuildNeedsNoSelection) {
    // The host IS the target; the absence is the decision rather than its
    // omission, and the probe must not be refused for it. openkal-musl on
    // Linux/x86_64 is this case: `realise` produces no tokens at all.
    auto r = cp::assemble_argv("", {}, {}, {}, false, "x86_64-linux-gnu");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_TRUE(r->empty());
}

TEST(CenvProbeArgv, AHostedCrossTargetCarriesItsCrossFlag) {
    auto r = cp::assemble_argv("--target=x86_64-w64-windows-gnu", {},
                               {"--target=x86_64-pc-cygwin", "-fno-short-wchar"},
                               {}, false, "x86_64-windows-gnu");
    ASSERT_TRUE(r.has_value()) << r.error();
    // Both are present and in this order: clang takes the LAST `--target`,
    // which is how the Cygwin-flavoured substitution overrides the base
    // triple without the producer of the base one knowing this module exists.
    ASSERT_GE(r->size(), 2u);
    EXPECT_EQ((*r)[0], "--target=x86_64-w64-windows-gnu");
    EXPECT_EQ((*r)[1], "--target=x86_64-pc-cygwin");
}

TEST(CenvProbeArgv, BuiltinsTokensAreCarriedLast) {
    auto r = cp::assemble_argv("", {}, {"-D__unix__"},
                               {"-fno-builtin"},
                               false, "x86_64-apple-macos");
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->size(), 2u);
    EXPECT_EQ((*r)[1], "-fno-builtin");
}

// ── The argv must select the target (mcpp#674 review, 2026-09-20) ──────────
//
// Clang is one binary that emits every target it was built with, so a probe
// command with no `--target` answers for the machine it runs on. 2026.9.18.3
// read that as a Windows host leaking `_WIN32` through a `--target=`
// substitution and added a `hostStripMacros` parameter to undefine it; there
// was no substitution in the freestanding argv to leak through, and the strip
// removed the evidence rather than the cause. The parameter is gone and these
// tests pin what replaced it.
//
// THEY DO NOT NEED A CROSS TOOLCHAIN. Every clang emits every target it was
// built with, so `--target=riscv64-none-elf` needs no payload to answer `-dM`.
// A compiler that cannot is skipped rather than reported.

namespace {
// Whether this compiler answers for a target it is not hosted on. A GCC
// driver does not take `--target`, and a clang built without the RISC-V
// backend answers nothing useful; both skip.
bool answers_for_riscv() { return !clangxx().empty(); }
} // namespace

TEST(CenvProbe, AnArgvWithATargetMeasuresThatTargetAndNotTheHost) {
    if (clangxx().empty()) GTEST_SKIP() << "no clang++ on PATH";
    if (!answers_for_riscv()) GTEST_SKIP() << "this clang does not answer for riscv64-none-elf";
    TmpCache cache;
    // The freestanding probe's own shape, with the target selection the
    // caller (`mcpp.build.prepare`) now supplies. `__linux__` must be absent
    // on every host: it is the host's own predefine, and a probe that still
    // reports it is measuring the host.
    std::vector<std::string> argv{
        "--target=riscv64-none-elf", "-ffreestanding",
        "-D__unix__", "-fno-short-wchar",
    };
    std::vector<std::string> defined{"__unix__", "__riscv"};
    std::vector<std::string> undefined{"__linux__", "_WIN32"};
    auto r = cp::verify(clangxx(), argv, 32, 0, defined, undefined, cache.dir);
    ASSERT_TRUE(r.has_value()) << r.error();
    for (auto const& mm : r->mismatches)
        ADD_FAILURE() << mm.fact << ": declared " << mm.declared
                      << ", measured " << mm.measured;
}

TEST(CenvProbe, TheSameArgvWithoutATargetMeasuresTheHost) {
    if (clangxx().empty()) GTEST_SKIP() << "no clang++ on PATH";
    if (!answers_for_riscv()) GTEST_SKIP() << "this clang does not answer for riscv64-none-elf";
    TmpCache cache;
    // The mirror of the test above, and the one that makes it mean
    // something: with the target removed, the expectation that held for the
    // target must now FAIL. A test that only asserts the fixed shape passes
    // identically against a probe that ignores its argv.
    std::vector<std::string> argv{"-ffreestanding", "-D__unix__", "-fno-short-wchar"};
    auto r = cp::verify(clangxx(), argv, 0, 0, {"__riscv"}, {}, cache.dir);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_FALSE(r->mismatches.empty())
        << "a probe with no target selection must not report the target's "
           "own predefines; if this passes, the argv is not reaching the "
           "compiler and the test above proves nothing";
}
