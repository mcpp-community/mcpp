// bench.toolchain — WHICH compiler every engine is handed, and where it lives.
//
// This module exists because the same decision was being made in two places.
// The fixture's generated `mcpp.toml` pinned `gcc@16.1.0`, and the CI workflow
// separately resolved `command -v g++` for cmake/xmake/bazel. Those two are not
// the same compiler, and nothing anywhere said so:
//
//   * mcpp built the fixture with the registry's gcc 16.1.0 and passed;
//   * cmake and xmake were handed the runner's gcc 13.3.0, which cannot build
//     C++23 modules at all — cmake failed to configure, xmake crashed gcc with
//     an internal compiler error, and both were recorded as `failed`.
//
// Forty-eight of the seventy-two cells in a "passing" matrix job failed that
// way. The suite's own fairness rule (see `resolve_cxx`) says every engine that
// can be told which compiler to use MUST be told the same one; this module is
// what makes that rule reachable, by naming ONE payload and handing it to
// everybody including mcpp.
//
// The versions are pinned rather than "whatever is newest" for the reason every
// other pin in this repository exists: a benchmark whose toolchain moves under
// it reports the toolchain's change as the engine's.
export module bench.toolchain;

import std;
import bench.platform;

export namespace bench::toolchain {

// The payload every arm of the benchmark compiles against.
//
// Windows is on llvm 20.1.7 rather than 22.1.8 because that is the version
// mcpp's registry actually ships for the PE target; pinning a version that is
// not there does not produce a slower number, it produces `unavailable`.
inline constexpr std::string_view kGcc          = "16.1.0";
inline constexpr std::string_view kLlvm         = "22.1.8";
inline constexpr std::string_view kLlvmWindows  = "20.1.7";

bool on_windows();

// Is this compiler request a clang one? The single spelling of that test, used
// by both the manifest emitter and the payload lookup.
bool is_clang_request(std::string_view compiler);

// Which FAMILY a compiler request resolves to on this host — the single
// decision `mcpp_pin` and `payload_cxx` both read, so the toolchain mcpp is told
// to use and the driver every other engine is handed cannot disagree.
//
// THE HOST IS PART OF THE ANSWER. There is no gcc payload for macOS in mcpp's
// registry (bench/matrix.json excludes the macos/gcc cell for exactly that
// reason), and the Windows payload is llvm. A pin that reads `gcc@16.1.0`
// everywhere fails on those hosts with
//
//     error: toolchain 'gcc@16.1.0': package 'xim:gcc@16.1.0' not found
//
// which is what happened the moment this replaced the old emitter's explicit
// `macos = "llvm@..."` / `windows = "llvm@..."` overrides with one `default`.
bool resolves_to_clang(std::string_view compiler);

// What the fixture's `mcpp.toml` must say so that mcpp uses the same compiler
// every other engine was handed.
std::string mcpp_pin(std::string_view compiler);

// Where mcpp keeps its packages. MCPP_HOME first, matching mcpp's own
// resolution order and the CMake helper in projects/common/.
std::filesystem::path registry_xpkgs();

// The C++ driver for `compiler` inside that payload, or nullopt with a reason.
//
// Returning the REASON rather than a bare nullopt matters: "the payload is not
// unpacked" and "this machine has no mcpp" lead to different fixes, and a
// benchmark that silently falls back to the host compiler when it cannot find
// the payload is the exact failure this module was written to end.
struct Resolved {
    std::filesystem::path driver;
    std::string           why;      // set when `driver` is empty
};

Resolved payload_cxx(std::string_view compiler);

// The flags a FOREIGN engine needs so that a payload compiler can actually
// build and link — the generated fixture's counterpart of
// bench/projects/common/cmake/hermetic_payload.cmake.
//
// WHY THIS EXISTS AT ALL, given that file exists. The checked-in project
// descriptions `include()` it; the fixture is GENERATED into a scratch
// directory by a binary that may live anywhere, so it has no path to include.
// The two are the same decision in two places and must be kept in step — the
// alternative considered (emit an `include()` of an absolute path) makes every
// generated fixture depend on this checkout still being where it was.
//
// It is needed because `--compiler payload:gcc` hands cmake and xmake a
// compiler out of mcpp's registry, and a bare registry gcc has no idea where
// its assembler, linker or libc are:
//
//     /usr/bin/ld: cannot find crt1.o: No such file or directory
//     /usr/bin/ld: cannot find -lm: No such file or directory
//
// which cmake reports as "the C++ compiler is not able to compile a simple
// test program", i.e. as a configure failure with no mention of a sysroot.
struct PayloadFlags {
    std::string compile;
    std::string link;
};

PayloadFlags payload_flags(std::string_view compiler);

}  // namespace bench::toolchain
