// [c-abi] realisation: a request → compiler-configuration mapping.
//
// design 2026-09-18 (openkal, "C environment declared by the C library
// layer"), §3.2-§3.3. `mcpp.toolchain.cenv::realise` is a pure function of
// plain data — the same reason `test_targetside.cpp` exists for
// `mcpp.targetside::resolve` — so the whole mapping table §3.3 publishes can
// be asserted here without compiling anything.

#include <gtest/gtest.h>

import std;
import mcpp.targetside;
import mcpp.toolchain.cenv;

namespace ts = mcpp::targetside;
namespace cenv = mcpp::toolchain::cenv;

namespace {

ts::CAbiDecl decl(ts::CAbiPresents p, ts::CAbiDataModel dm, int wchar,
                  ts::CAbiBuiltins b = ts::CAbiBuiltins::Platform) {
    ts::CAbiDecl d;
    d.declared = true;
    d.presents = p; d.hasPresents = true;
    d.dataModel = dm; d.hasDataModel = true;
    d.wcharBits = wchar; d.hasWchar = true;
    d.builtins = b;
    return d;
}

bool has(const std::vector<std::string>& v, std::string_view tok) {
    return std::find(v.begin(), v.end(), tok) != v.end();
}

} // namespace

// ── §3.3's own table: the three realisable rows ──────────────────────────────

TEST(CEnv, LinuxPosixArchDefaultIsANoOp) {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 32);
    auto r = cenv::realise(d, "linux", "x86_64", false);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_TRUE(r->tokens.empty());
    EXPECT_EQ(r->expectLongBytes, 8);
    EXPECT_EQ(r->expectWcharBits, 32);
}

// NOT a no-op — a correction from the design's original text, caught by the
// verification probe on a real build (coordinator report): Apple's clang
// predefines `__APPLE__`/`__MACH__` on its default triple, never `__unix__`.
// The rule (design §3.3, revised): realising `presents = "posix"` means the
// SAME observable fact everywhere — `__unix__` defined, `_WIN32` not — and
// macOS's cost for that fact is one token, `-D__unix__`, same as a
// freestanding target's (`FreestandingPosixDefinesUnix` below) and for the
// identical reason: neither already has it.
TEST(CEnv, MacosPosixArchDefaultDefinesUnix) {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 32);
    auto r = cenv::realise(d, "macos", "aarch64", false);
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_TRUE(has(r->tokens, "-D__unix__"));
    ASSERT_TRUE(has(r->expectDefined, "__unix__"));
    ASSERT_TRUE(has(r->expectUndefined, "_WIN32"));
}

// The flagship case: Cygwin-flavoured Windows.
//
// `__CYGWIN__`/`__CYGWIN32__` STAY DEFINED — a design revision from the
// openkal-musl spike, not the original §3.3 text. Third-party portable code
// that needs to know the OBJECT FORMAT (as opposed to the C environment or
// the platform API) has no name for "PE format, POSIX-presenting
// environment" other than `__CYGWIN__`, and such code cannot be patched the
// way this ecosystem's own packages can. This is a trade-off for the
// 30-member measurement to settle, not a settled fact: the cost is that a
// library reaching for `__CYGWIN__` may also reach for a real Cygwin
// interface that does not exist here.
TEST(CEnv, WindowsPosixArchDefaultSubstitutesTheCygwinTriple) {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 32);
    auto r = cenv::realise(d, "windows", "x86_64", false);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_TRUE(has(r->tokens, "--target=x86_64-pc-cygwin"));
    EXPECT_FALSE(has(r->tokens, "-U__CYGWIN__"));
    EXPECT_FALSE(has(r->tokens, "-U__CYGWIN32__"));
    // wchar 32 differs from Cygwin's own default (16) — the flag is added.
    EXPECT_TRUE(has(r->tokens, "-fno-short-wchar"));
    EXPECT_EQ(r->expectLongBytes, 8);
    EXPECT_EQ(r->expectWcharBits, 32);
    ASSERT_TRUE(has(r->expectDefined, "__unix__"));
    ASSERT_TRUE(has(r->expectDefined, "__CYGWIN__"));
    ASSERT_TRUE(has(r->expectUndefined, "_WIN32"));
}

// wchar = 16 on the Cygwin substitution matches Cygwin's own default, so no
// `-f[no-]short-wchar` token is needed.
TEST(CEnv, WindowsPosixWchar16NeedsNoWcharFlag) {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 16);
    auto r = cenv::realise(d, "windows", "x86_64", false);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_FALSE(has(r->tokens, "-fno-short-wchar"));
    EXPECT_FALSE(has(r->tokens, "-fshort-wchar"));
}

// `presents = "windows"` on Windows is the base triple's own identity.
TEST(CEnv, WindowsWindowsArchDefaultIsANoOp) {
    auto d = decl(ts::CAbiPresents::Windows, ts::CAbiDataModel::ArchDefault, 16);
    auto r = cenv::realise(d, "windows", "x86_64", false);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_TRUE(r->tokens.empty());
}

// ── `builtins = "iso"` — the Apple case is the only one this survey found ────

TEST(CEnv, BuiltinsIsoOnMacosDisablesMemsetPattern16) {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 32,
                  ts::CAbiBuiltins::Iso);
    auto r = cenv::realise(d, "macos", "x86_64", false);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_TRUE(has(r->builtinsTokens, "-fno-builtin-memset_pattern16"));
}

TEST(CEnv, BuiltinsIsoOnLinuxAddsNothingMeasurable) {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 32,
                  ts::CAbiBuiltins::Iso);
    auto r = cenv::realise(d, "linux", "x86_64", false);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_TRUE(r->builtinsTokens.empty());
}

TEST(CEnv, BuiltinsPlatformAddsNothingOnAnyTarget) {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 32,
                  ts::CAbiBuiltins::Platform);
    auto r = cenv::realise(d, "macos", "x86_64", false);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_TRUE(r->builtinsTokens.empty());
}

// ── refusals: everything outside the table §3.3 publishes ───────────────────

TEST(CEnv, WindowsPosixOnANonX86_64ArchIsRefused) {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 32);
    auto r = cenv::realise(d, "windows", "aarch64", false);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("x86_64"), std::string::npos) << r.error();
}

TEST(CEnv, LinuxWindowsPresentsIsRefused) {
    auto d = decl(ts::CAbiPresents::Windows, ts::CAbiDataModel::ArchDefault, 32);
    auto r = cenv::realise(d, "linux", "x86_64", false);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("no known way"), std::string::npos) << r.error();
}

TEST(CEnv, WindowsPresentsNoneIsRefused) {
    auto d = decl(ts::CAbiPresents::None, ts::CAbiDataModel::ArchDefault, 32);
    auto r = cenv::realise(d, "windows", "x86_64", false);
    ASSERT_FALSE(r.has_value());
}

TEST(CEnv, LlP64RequestedOnLinuxIsRefused) {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::Llp64, 32);
    auto r = cenv::realise(d, "linux", "x86_64", false);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("data-model"), std::string::npos) << r.error();
}

TEST(CEnv, FreestandingAcceptsNone) {
    auto ok = decl(ts::CAbiPresents::None, ts::CAbiDataModel::ArchDefault, 32);
    auto r1 = cenv::realise(ok, "none", "riscv64", true);
    ASSERT_TRUE(r1.has_value()) << r1.error();
    // The wchar branch ALWAYS emits on a freestanding target now (wave
    // 2026-09-18, openkal-llvm-runtime#24, Windows-host × riscv64-none-elf
    // measurement): the toolchain's host-contaminated default is not
    // something the engine can trust, so `wchar = 32` produces
    // `-fno-short-wchar` regardless of what `presents` says. Identity
    // macros stay absent (the `none` presents value's whole point),
    // but the wchar realisation still applies — and so does the
    // expectation, which the probe then measures.
    EXPECT_TRUE(has(r1->tokens, "-fno-short-wchar"));
    EXPECT_TRUE(r1->expectUndefined.empty())
        << "presents = none declares NO environment-identity macros, "
           "neither defined nor undefined";
    EXPECT_TRUE(r1->expectDefined.empty());
    // Data-model is not checked on freestanding (no C library runtime to
    // present it), so `expectLongBytes` stays at 0.
    EXPECT_EQ(r1->expectLongBytes, 0);
}

// A freestanding target ALSO realises `presents = "posix"` (design §3.3,
// coordinator revision — this used to be an unconditional refusal, which is
// exactly the bug openkal-llvm-runtime's CI hit: refused on riscv64-none-elf
// before reaching any other target, for a fact this engine can in fact
// deliver). Bare metal starts with neither `_WIN32` nor `__unix__` defined,
// same as macOS's default triple, so it costs the identical one token.
TEST(CEnv, FreestandingPosixDefinesUnix) {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 32);
    auto r = cenv::realise(d, "none", "riscv64", true);
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_TRUE(has(r->tokens, "-D__unix__"));
    ASSERT_TRUE(has(r->expectDefined, "__unix__"));
    ASSERT_TRUE(has(r->expectUndefined, "_WIN32"));
}

// A freestanding target with `wchar = 32` USED TO skip the
// `-fno-short-wchar` token on the reasoning that the toolchain default was
// already 32 bits on every freestanding target. The wave's
// Windows-host × riscv64-none-elf measurement (openkal-llvm-runtime#24,
// 2026-09-18) caught that assumption as wrong: clang on a Windows host uses
// MinGW's `<winnt.h>` defaults even with `--target=riscv64-none-elf`, and
// `__SIZEOF_WCHAR_T__` is 2 there. The realisation now ALWAYS emits
// `-fno-short-wchar` for `decl.wcharBits = 32`, regardless of what the
// host's toolchain would default to — so the probe then measures the state
// the engine actually produced (32 bits, with the flag), not the host's
// leak. Pinned here so a future "freestanding skips the flag" optimisation
// cannot return without a regression test.
TEST(CEnv, FreestandingWchar32AlwaysEmitsNoShortWchar) {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 32);
    auto r = cenv::realise(d, "none", "riscv64", true);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_TRUE(has(r->tokens, "-fno-short-wchar"))
        << "freestanding host leakage on Windows would compile a 16-bit "
           "wchar_t otherwise; the wave's measurement is the source for "
           "this assertion.";
    EXPECT_EQ(r->expectWcharBits, 32);
}

// The matching case for `wchar = 16`: `-fshort-wchar` always emitted on
// freestanding when the declaration asks for 16, so a Linux/macOS host
// (which defaults to 32 bits on a freestanding target) is brought down to
// the declaration. Mirrors the wave's measurement in the other direction.
TEST(CEnv, FreestandingWchar16AlwaysEmitsShortWchar) {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 16);
    auto r = cenv::realise(d, "none", "riscv64", true);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_TRUE(has(r->tokens, "-fshort-wchar"));
    EXPECT_EQ(r->expectWcharBits, 16);
}

// `windows` still has no realisation on a freestanding target — there is no
// operating system under it for that identity to belong to. Unaffected by
// the `posix` fix above; pinned so it stays that way.
TEST(CEnv, FreestandingWindowsIsStillRefused) {
    auto d = decl(ts::CAbiPresents::Windows, ts::CAbiDataModel::ArchDefault, 16);
    auto r = cenv::realise(d, "none", "riscv64", true);
    ASSERT_FALSE(r.has_value());
}

// A 32-bit architecture's own native data model is ILP32 — the Cygwin
// mechanism is x86_64-only, so `arch-default` there needs no substitution and
// stays a no-op regardless.
TEST(CEnv, Ilp32ArchDefaultOnA32BitArchIsANoOp) {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 32);
    auto r = cenv::realise(d, "linux", "arm", false);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->expectLongBytes, 4);
}
