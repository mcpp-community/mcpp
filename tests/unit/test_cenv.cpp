// [c-abi] realisation: a request → compiler-configuration mapping.
//
// design 2026-09-18 (openkal, "C environment declared by the C library
// layer"), §3.2-§3.3. `mcpp.toolchain.cenv::realise` is a pure function of
// plain data — the same reason `test_targetside.cpp` exists for
// `mcpp.targetside::resolve` — so the whole mapping table §3.3 publishes can
// be asserted here without compiling anything.


import std;
import mcpplibs.mtest;
import mcpp.targetside;
import mcpp.toolchain.cenv;

using namespace mcpplibs::mtest;

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

UTest linux_posix_arch_default_is_a_no_op{"CEnv/LinuxPosixArchDefaultIsANoOp", [] {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 32);
    auto r = cenv::realise(d, "linux", "x86_64", false);
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << r.error();
    expect(r->tokens.empty());
    expect(eq(r->expectLongBytes, 8));
    expect(eq(r->expectWcharBits, 32));
}};

// NOT a no-op — a correction from the design's original text, caught by the
// verification probe on a real build (coordinator report): Apple's clang
// predefines `__APPLE__`/`__MACH__` on its default triple, never `__unix__`.
// The rule (design §3.3, revised): realising `presents = "posix"` means the
// SAME observable fact everywhere — `__unix__` defined, `_WIN32` not — and
// macOS's cost for that fact is one token, `-D__unix__`, same as a
// freestanding target's (`FreestandingPosixDefinesUnix` below) and for the
// identical reason: neither already has it.
UTest macos_posix_arch_default_defines_unix{"CEnv/MacosPosixArchDefaultDefinesUnix", [] {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 32);
    auto r = cenv::realise(d, "macos", "aarch64", false);
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << r.error();
    expect(fatal(bool(has(r->tokens, "-D__unix__"))));
    expect(fatal(bool(has(r->expectDefined, "__unix__"))));
    expect(fatal(bool(has(r->expectUndefined, "_WIN32"))));
}};

// The flagship case: Cygwin-flavoured Windows.
//
// THE SUBSTITUTION HIDES THE TARGET, AND WHAT STATES IT ANYWAY IS NOT HERE.
// Suppressing `_WIN32` is the point of presenting POSIX, but the ABI did not
// change with the environment --- the register save areas are still Win64's,
// and two INSTALLED headers in this ecosystem size records by that fact.
// `__MCPP_TARGET_<OS>__` answers it, for every target rather than only this
// one, and it belongs to `mcpp.toolchain.predefines`: a fact about the
// TARGET is true whether or not any `[c-abi]` block exists, so realising it
// from a declaration would make its absence ambiguous. This test therefore
// asserts that the realisation does NOT carry it --- the separation is the
// property, and a token appearing in both places would mean two owners.
//
// `__CYGWIN__` IS WITHDRAWN, AND THAT WAS THE LAST STEP OF A SEQUENCE. The
// 30-member measurement settled that the borrowed name costs four members,
// which read it as "Win32 is available" and reach `#include <windows.h>`.
// Withdrawing it FIRST was tried in this branch and broke the two headers
// above, because they read it too and nothing else named the target for them;
// libunwind's `static_assert` failed loudly, setjmp.h's equivalent would not
// have. So the order was: name it here, publish packages that read the new
// name, and only then stop defining the borrowed one.
//
// WHAT THIS TEST CAN AND CANNOT PIN. It pins the engine's half --- the
// tokens, and that the probe will verify them. It cannot pin the ORDERING,
// which holds between repositories: that `openkal-musl@0.19.0` and
// `openkal-llvm-runtime@0.14.0` were published BEFORE this release. That was
// measured instead, by building an openkal program for `x86_64-windows-gnu`
// against the PUBLISHED 0.13.0 on this engine, which fails on libunwind's
// two `static_assert`s exactly as predicted. An ordering between
// repositories has no assertion in either of them.
UTest windows_posix_arch_default_substitutes_the_cygwin_triple{"CEnv/WindowsPosixArchDefaultSubstitutesTheCygwinTriple", [] {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 32);
    auto r = cenv::realise(d, "windows", "x86_64", false);
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << r.error();
    expect(bool(has(r->tokens, "--target=x86_64-pc-cygwin")));
    // The target's identity is not this module's to state: see above.
    expect(!has(r->tokens, "-D__MCPP_TARGET_WINDOWS__=1"));
    // The borrowed name is undefined on the compile line. `-U` rather than a
    // different triple: the Cygwin identity is what supplies `__unix__` and
    // suppresses `_WIN32`, which is the point of `presents = "posix"`; only
    // the NAME was unwanted.
    expect(bool(has(r->tokens, "-U__CYGWIN__")));
    expect(bool(has(r->tokens, "-U__CYGWIN32__")));
    // wchar 32 differs from Cygwin's own default (16) — the flag is added.
    expect(bool(has(r->tokens, "-fno-short-wchar")));
    expect(eq(r->expectLongBytes, 8));
    expect(eq(r->expectWcharBits, 32));
    expect(fatal(bool(has(r->expectDefined, "__unix__"))));
    expect(fatal(bool(has(r->expectUndefined, "_WIN32"))));
    // THE PROBE CHECKS THESE, which is what keeps the tokens from being ones
    // the compiler could ignore: `cenv_probe::verify` compares the realised
    // configuration's predefines against these lists and refuses on a
    // mismatch. A `-U` that did not take effect is a verification failure,
    // not a silent one --- and silence is the whole risk here, since a `#if`
    // that finds the name still defined simply compiles the other branch.
    expect(fatal(bool(has(r->expectUndefined, "__CYGWIN__"))));
    expect(fatal(bool(has(r->expectUndefined, "__CYGWIN32__"))));
    expect(!has(r->expectDefined, "__CYGWIN__"));
}};

// wchar = 16 on the Cygwin substitution matches Cygwin's own default, so no
// `-f[no-]short-wchar` token is needed.
UTest windows_posix_wchar16_needs_no_wchar_flag{"CEnv/WindowsPosixWchar16NeedsNoWcharFlag", [] {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 16);
    auto r = cenv::realise(d, "windows", "x86_64", false);
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << r.error();
    expect(!has(r->tokens, "-fno-short-wchar"));
    expect(!has(r->tokens, "-fshort-wchar"));
}};

// `presents = "windows"` on Windows is the base triple's own identity.
UTest windows_windows_arch_default_is_a_no_op{"CEnv/WindowsWindowsArchDefaultIsANoOp", [] {
    auto d = decl(ts::CAbiPresents::Windows, ts::CAbiDataModel::ArchDefault, 16);
    auto r = cenv::realise(d, "windows", "x86_64", false);
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << r.error();
    expect(r->tokens.empty());
}};

// ── `builtins = "iso"` — the Apple case is the only one this survey found ────

UTest builtins_iso_on_macos_disables_memset_pattern16{"CEnv/BuiltinsIsoOnMacosDisablesMemsetPattern16", [] {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 32,
                  ts::CAbiBuiltins::Iso);
    auto r = cenv::realise(d, "macos", "x86_64", false);
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << r.error();
    // `-fno-builtin`, NOT the per-function spelling. The narrower form was
    // emitted here and measured to do nothing: clang checks `-fno-builtin-X`
    // against its builtin table, `memset_pattern16` is an LLVM TLI libfunc,
    // and the call is produced by a pass that consults TLI. clang accepts an
    // unknown name in that family in silence, so the flag looked right and
    // was not.
    expect(bool(has(r->builtinsTokens, "-fno-builtin")));
    expect(!has(r->builtinsTokens, "-fno-builtin-memset_pattern16"));
}};

UTest builtins_iso_on_linux_adds_nothing_measurable{"CEnv/BuiltinsIsoOnLinuxAddsNothingMeasurable", [] {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 32,
                  ts::CAbiBuiltins::Iso);
    auto r = cenv::realise(d, "linux", "x86_64", false);
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << r.error();
    expect(r->builtinsTokens.empty());
}};

UTest builtins_platform_adds_nothing_on_any_target{"CEnv/BuiltinsPlatformAddsNothingOnAnyTarget", [] {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 32,
                  ts::CAbiBuiltins::Platform);
    auto r = cenv::realise(d, "macos", "x86_64", false);
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << r.error();
    expect(r->builtinsTokens.empty());
}};

// ── refusals: everything outside the table §3.3 publishes ───────────────────

UTest windows_posix_on_a_non_x86_64_arch_is_refused{"CEnv/WindowsPosixOnANonX86_64ArchIsRefused", [] {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 32);
    auto r = cenv::realise(d, "windows", "aarch64", false);
    expect(fatal(!r.has_value()));
    if (auto&& failed = expect(ne(r.error().find("x86_64"), std::string::npos)); !failed) failed << r.error();
}};

UTest linux_windows_presents_is_refused{"CEnv/LinuxWindowsPresentsIsRefused", [] {
    auto d = decl(ts::CAbiPresents::Windows, ts::CAbiDataModel::ArchDefault, 32);
    auto r = cenv::realise(d, "linux", "x86_64", false);
    expect(fatal(!r.has_value()));
    if (auto&& failed = expect(ne(r.error().find("no known way"), std::string::npos)); !failed) failed << r.error();
}};

UTest windows_presents_none_is_refused{"CEnv/WindowsPresentsNoneIsRefused", [] {
    auto d = decl(ts::CAbiPresents::None, ts::CAbiDataModel::ArchDefault, 32);
    auto r = cenv::realise(d, "windows", "x86_64", false);
    expect(fatal(!r.has_value()));
}};

UTest ll_p64_requested_on_linux_is_refused{"CEnv/LlP64RequestedOnLinuxIsRefused", [] {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::Llp64, 32);
    auto r = cenv::realise(d, "linux", "x86_64", false);
    expect(fatal(!r.has_value()));
    if (auto&& failed = expect(ne(r.error().find("data-model"), std::string::npos)); !failed) failed << r.error();
}};

UTest freestanding_accepts_none{"CEnv/FreestandingAcceptsNone", [] {
    auto ok = decl(ts::CAbiPresents::None, ts::CAbiDataModel::ArchDefault, 32);
    auto r1 = cenv::realise(ok, "none", "riscv64", true);
    if (auto&& failed = expect(fatal(r1.has_value())); !failed) failed << r1.error();
    // The wchar branch ALWAYS emits on a freestanding target now (wave
    // 2026-09-18, openkal-llvm-runtime#24, Windows-host × riscv64-none-elf
    // measurement): the toolchain's host-contaminated default is not
    // something the engine can trust, so `wchar = 32` produces
    // `-fno-short-wchar` regardless of what `presents` says. Identity
    // macros stay absent (the `none` presents value's whole point),
    // but the wchar realisation still applies — and so does the
    // expectation, which the probe then measures.
    expect(bool(has(r1->tokens, "-fno-short-wchar")));
    expect(r1->expectUndefined.empty())
        << "presents = none declares NO environment-identity macros, "
           "neither defined nor undefined";
    expect(r1->expectDefined.empty());
    // Data-model is not checked on freestanding (no C library runtime to
    // present it), so `expectLongBytes` stays at 0.
    expect(eq(r1->expectLongBytes, 0));
}};

// A freestanding target ALSO realises `presents = "posix"` (design §3.3,
// coordinator revision — this used to be an unconditional refusal, which is
// exactly the bug openkal-llvm-runtime's CI hit: refused on riscv64-none-elf
// before reaching any other target, for a fact this engine can in fact
// deliver). Bare metal starts with neither `_WIN32` nor `__unix__` defined,
// same as macOS's default triple, so it costs the identical one token.
UTest freestanding_posix_defines_unix{"CEnv/FreestandingPosixDefinesUnix", [] {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 32);
    auto r = cenv::realise(d, "none", "riscv64", true);
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << r.error();
    expect(fatal(bool(has(r->tokens, "-D__unix__"))));
    expect(fatal(bool(has(r->expectDefined, "__unix__"))));
    expect(fatal(bool(has(r->expectUndefined, "_WIN32"))));
}};

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
UTest freestanding_wchar32_always_emits_no_short_wchar{"CEnv/FreestandingWchar32AlwaysEmitsNoShortWchar", [] {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 32);
    auto r = cenv::realise(d, "none", "riscv64", true);
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << r.error();
    expect(bool(has(r->tokens, "-fno-short-wchar")))
        << "freestanding host leakage on Windows would compile a 16-bit "
           "wchar_t otherwise; the wave's measurement is the source for "
           "this assertion.";
    expect(eq(r->expectWcharBits, 32));
}};

// The matching case for `wchar = 16`: `-fshort-wchar` always emitted on
// freestanding when the declaration asks for 16, so a Linux/macOS host
// (which defaults to 32 bits on a freestanding target) is brought down to
// the declaration. Mirrors the wave's measurement in the other direction.
UTest freestanding_wchar16_always_emits_short_wchar{"CEnv/FreestandingWchar16AlwaysEmitsShortWchar", [] {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 16);
    auto r = cenv::realise(d, "none", "riscv64", true);
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << r.error();
    expect(bool(has(r->tokens, "-fshort-wchar")));
    expect(eq(r->expectWcharBits, 16));
}};

// `windows` still has no realisation on a freestanding target — there is no
// operating system under it for that identity to belong to. Unaffected by
// the `posix` fix above; pinned so it stays that way.
UTest freestanding_windows_is_still_refused{"CEnv/FreestandingWindowsIsStillRefused", [] {
    auto d = decl(ts::CAbiPresents::Windows, ts::CAbiDataModel::ArchDefault, 16);
    auto r = cenv::realise(d, "none", "riscv64", true);
    expect(fatal(!r.has_value()));
}};

// A 32-bit architecture's own native data model is ILP32 — the Cygwin
// mechanism is x86_64-only, so `arch-default` there needs no substitution and
// stays a no-op regardless.
UTest ilp32_arch_default_on_a32_bit_arch_is_a_no_op{"CEnv/Ilp32ArchDefaultOnA32BitArchIsANoOp", [] {
    auto d = decl(ts::CAbiPresents::Posix, ts::CAbiDataModel::ArchDefault, 32);
    auto r = cenv::realise(d, "linux", "arm", false);
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << r.error();
    expect(eq(r->expectLongBytes, 4));
}};
