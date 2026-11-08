
import std;
import mcpplibs.mtest;
import mcpp.toolchain.model;
import mcpp.toolchain.platform;

using namespace mcpplibs::mtest;

// Whether the produced artifact can be fully statically linked (`-static`) is
// a property of the TARGET, never of the machine doing the build.
//
// mcpp used to answer it with `mcpp::toolchain::platform::supports_full_static`, a host
// constant defined as `is_linux`. On a Linux host that happens to give the
// right answer for every Linux target, so the bug stayed invisible — it only
// surfaces once a non-Linux host cross-compiles to Linux, where `-static`
// silently disappeared and the defining feature of the musl targets (a fully
// static, portable ELF) was lost.
//
// The host capability is an explicit parameter precisely so this can be
// pinned from any host: `hostCapability=false` models a Windows/macOS host.

namespace {

namespace tc = mcpp::toolchain;

// ── The regression itself ───────────────────────────────────────────────────
// Every one of these runs a NON-Linux host (hostCapability=false) against a
// Linux target. Before the fix all four returned false.
UTest linux_target_from_non_linux_host{"TargetSupportsFullStatic/LinuxTargetFromNonLinuxHost", [] {
    expect(bool(tc::target_supports_full_static("x86_64-linux-musl", false)));
    expect(bool(tc::target_supports_full_static("aarch64-linux-musl", false)));
    expect(bool(tc::target_supports_full_static("x86_64-linux-gnu", false)));
    // Legacy/alternate spellings must resolve identically — the triple parser
    // is the single source of truth, not a substring match.
    expect(bool(tc::target_supports_full_static("x86_64-unknown-linux-musl", false)));
}};

// A Linux host keeps answering exactly as before: this fix must be a no-op
// on every path that works today.
UTest linux_target_from_linux_host_unchanged{"TargetSupportsFullStatic/LinuxTargetFromLinuxHostUnchanged", [] {
    expect(bool(tc::target_supports_full_static("x86_64-linux-musl", true)));
    expect(bool(tc::target_supports_full_static("aarch64-linux-musl", true)));
    expect(bool(tc::target_supports_full_static("x86_64-linux-gnu", true)));
}};

// ── Targets that genuinely cannot be fully static ───────────────────────────
// macOS: libSystem must stay dynamic, no matter who builds it.
UTest macos_target_never_static{"TargetSupportsFullStatic/MacosTargetNeverStatic", [] {
    expect(!tc::target_supports_full_static("aarch64-macos", true));
    expect(!tc::target_supports_full_static("aarch64-macos", false));
    expect(!tc::target_supports_full_static("x86_64-macos", true));
}};

// PE targets take their `-static` from the C++ runtime distribution contract
// (dist::Format::Pe in flags.cppm), not from this predicate. Answering false
// here is what keeps the two mechanisms from both emitting the flag.
UTest pe_targets_defer_to_contract_table{"TargetSupportsFullStatic/PeTargetsDeferToContractTable", [] {
    expect(!tc::target_supports_full_static("x86_64-windows-gnu", true));
    expect(!tc::target_supports_full_static("x86_64-windows-msvc", true));
    expect(!tc::target_supports_full_static("x86_64-w64-mingw32", true));
}};

// ── Host target (empty triple) ──────────────────────────────────────────────
// Empty means "build for this machine", so the host capability IS the answer.
// This is the one case where the host constant remains correct.
UTest empty_triple_falls_back_to_host{"TargetSupportsFullStatic/EmptyTripleFallsBackToHost", [] {
    expect(bool(tc::target_supports_full_static("", true)));
    expect(!tc::target_supports_full_static("", false));
}};

// An unparseable triple must not silently become "static" — fall back to the
// host answer rather than guessing from a substring.
UTest unknown_triple_falls_back_to_host{"TargetSupportsFullStatic/UnknownTripleFallsBackToHost", [] {
    expect(bool(tc::target_supports_full_static("wasm32-unknown-unknown", true)));
    expect(!tc::target_supports_full_static("wasm32-unknown-unknown", false));
    expect(!tc::target_supports_full_static("not-a-triple", false));
}};

} // namespace
