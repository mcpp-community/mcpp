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

TEST(CEnv, MacosPosixArchDefaultIsANoOp) {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 32);
    auto r = cenv::realise(d, "macos", "aarch64", false);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_TRUE(r->tokens.empty());
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

TEST(CEnv, FreestandingAcceptsOnlyNone) {
    auto ok = decl(ts::CAbiPresents::None, ts::CAbiDataModel::ArchDefault, 32);
    auto r1 = cenv::realise(ok, "none", "riscv64", true);
    EXPECT_TRUE(r1.has_value()) << (r1 ? "" : r1.error());

    auto bad = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 32);
    auto r2 = cenv::realise(bad, "none", "riscv64", true);
    ASSERT_FALSE(r2.has_value());
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
