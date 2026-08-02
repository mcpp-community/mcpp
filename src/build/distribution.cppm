// mcpp.build.distribution — the C++ runtime distribution contract.
//
// WHY THIS MODULE EXISTS
//
// "Does this artifact carry its own C++ runtime?" used to be derived
// independently in five places (issue #336): `ldStdlibDefault` and
// `ldStdlibTest` in flags.cppm, the `-static-libstdc++` string a few lines
// above them, the MinGW `-static` branch, and the `LinkUnit::TestBinary`
// two-way switch in ninja_backend.cppm. New semantics landed in some of them
// and not others, which is exactly how `[build] static_stdlib = false` came
// to be silently ignored for test binaries between 0.0.86 and today while the
// documentation kept promising it worked.
//
// The model is three layers, and only the third one knows about flags:
//
//   Role      — intrinsic to the link unit. Not user-specified.
//   Contract  — what the artifact promises about the machine that runs it.
//               Defaulted per role, overridable via [build] cxx_runtime.
//   Mechanism — (contract x stdlib x binary format) -> link flags.
//
// The mechanism table is a TOTAL function: every cell has an answer, and a
// cell that cannot be honored returns `degraded` plus a non-empty
// `diagnostic` that the caller MUST surface. A silent no-op is the one
// outcome this module is built to make impossible — before it, asking for a
// self-contained artifact on a Linux/libc++ toolchain produced no flag, no
// warning and a toolchain-coupled binary.
//
// Analysis: .agents/docs/2026-08-02-issue336-pr142-analysis.md

export module mcpp.build.distribution;

import std;

export namespace mcpp::build::dist {

// ---------------------------------------------------------------- Layer 1

// What the link unit is FOR. Intrinsic — derived from LinkUnit::Kind, never
// written by a user. It exists so "tests run on the build machine, shipped
// artifacts do not" is a readable policy instead of an `if (kind ==
// TestBinary)` buried in the ninja emitter.
//
// `build.mcpp` host helpers are deliberately NOT a role here: their link
// policy (`staticHostHelper` in build_program.cppm) is about the libc axis —
// a musl helper needs `-static` because of PT_INTERP, a PE helper because
// DLLs resolve via PATH — not about the C++ runtime this module governs.
enum class Role {
    Distributable,  // Binary / SharedLibrary — leaves this machine
    Test,           // TestBinary — runs here, right now, then is discarded
    Intermediate,   // StaticLibrary — carries no runtime, contract is vacuous
};

// ---------------------------------------------------------------- Layer 2

// What the artifact promises about the machine that runs it. This is a
// DISTRIBUTION property, not a build one: it describes the runtime dependency
// set, and the flags that produce it differ per platform.
enum class Contract {
    // No C++ runtime dependency outside the artifact. The default, and what
    // makes the macOS deployment floor (14.0) real rather than aspirational.
    SelfContained,
    // Links the C++ runtime of the toolchain mcpp installed, found again at
    // run time through the toolchain's rpath. The artifact travels only with
    // that toolchain present.
    ToolchainCoupled,
    // Links whatever C++ runtime the driver resolves by default — the system
    // one on most hosts. The form distro packaging (Debian/Homebrew) requires,
    // and the form `static_stdlib = false` has always been documented to mean.
    HostCoupled,
};

// NOTE ON SCOPE: the contract governs the C++ runtime (stdlib + its ABI and
// unwinder). The libc axis is separate and stays with `linkage`/`--static`
// (a musl `-static` link), and the deployment floor is a third axis
// (`macos_deployment_target`). Conflating them is what made a single
// `static_stdlib` bool expand into three different platform meanings.
//
// KNOWN LIMIT: `HostCoupled` promises only that mcpp adds nothing to embed a
// C++ runtime. It does not strip the toolchain rpath that the link already
// carries for other reasons, so on ELF a HostCoupled artifact may still find
// the toolchain's libraries first. Removing that rpath is a packaging axis of
// its own and is not part of this contract.

// The binary format decides which mechanisms even exist — Mach-O has no
// priority-ordered initializer section, PE has no rpath, ELF has both.
enum class Format { Elf, MachO, Pe };

std::string_view to_string(Contract c) {
    switch (c) {
        case Contract::SelfContained:    return "self-contained";
        case Contract::ToolchainCoupled: return "toolchain-coupled";
        case Contract::HostCoupled:      return "host-coupled";
    }
    return "self-contained";
}

std::string_view to_string(Role r) {
    switch (r) {
        case Role::Distributable: return "distributable";
        case Role::Test:          return "test";
        case Role::Intermediate:  return "intermediate";
    }
    return "distributable";
}

std::optional<Contract> parse_contract(std::string_view s) {
    if (s == "self-contained")    return Contract::SelfContained;
    if (s == "toolchain-coupled") return Contract::ToolchainCoupled;
    if (s == "host-coupled")      return Contract::HostCoupled;
    return std::nullopt;
}

// The role -> contract policy, in one place.
//
// Test binaries default to SelfContained rather than the HostCoupled that
// their role alone would suggest, and that is deliberate: on macOS a test
// linked against the SYSTEM libc++ while compiled against the toolchain's
// libc++ HEADERS is a version split that detonated once already (undefined
// `__hash_memory` when libc++ 22 moved string hashing out of line, #202). The
// role model makes that trade visible instead of hard-coding it in the
// emitter; a project that wants the other side of it writes
// `cxx_runtime = { tests = "host-coupled" }` and now actually gets it.
Contract default_contract(Role r) {
    switch (r) {
        case Role::Distributable: return Contract::SelfContained;
        case Role::Test:          return Contract::SelfContained;
        case Role::Intermediate:  return Contract::SelfContained;
    }
    return Contract::SelfContained;
}

// ---------------------------------------------------------------- Layer 3

struct MechanismInput {
    Contract         requested = Contract::SelfContained;
    Role             role      = Role::Distributable;
    // Did a human write this contract down, or is it just our default?
    //
    // The distinction decides whether a cell with no mechanism SPEAKS. A
    // diagnostic is for a BROKEN PROMISE: mcpp said the artifact would be
    // self-contained and it is not. Under the MSVC runtime mcpp never made
    // that promise — there is no /MT emission at all — so warning on every
    // Windows build would be noise nobody can act on. Write
    // `cxx_runtime = "self-contained"` there and you get told, once, that it
    // is not implemented. Cells where mcpp DOES promise something (a missing
    // libc++.a under the default, say) report regardless.
    bool             explicitRequest = false;
    // Toolchain capability id: "libstdc++", "libc++", or an MSVC STL spelling.
    std::string_view stdlibId;
    Format           format = Format::Elf;
    // MinGW targets are PE + libstdc++ and take the whole-link `-static`
    // rather than the piecemeal `-static-libstdc++` (the latter still leaves
    // libwinpthread-1.dll behind).
    bool             mingw = false;
    // Host is Windows. Only reason it is here: the historical flag string
    // adds `-static-libgcc` on a Windows HOST, and this table reproduces the
    // existing bytes rather than quietly "improving" them.
    bool             hostIsWindows = false;
    // `linkage = "static"` — the libc axis. On PE it shares the one `-static`
    // spelling with the C++ runtime axis, so the table has to see it.
    bool             fullStaticLibc = false;
    // Already-escaped archive paths for the explicit-archive mechanisms.
    // Empty string = that archive is not available on this toolchain.
    std::string      libcxxArchive;
    std::string      libcxxAbiArchive;
    std::string      libunwindArchive;
    // macOS only: the libc++ archive actually defines the ABI symbol the
    // initializer-ordering shim binds to. Checked against the archive rather
    // than assumed, so an unexpected spelling disables the shim instead of
    // producing an undefined reference at link time.
    bool             streamInitSymbolPresent = false;
    // macOS only: a deployment floor was resolved. The static-libc++
    // mechanism exists to make that floor real, so without one there is
    // nothing to make real.
    bool             macosFloor = false;
};

struct Mechanism {
    // Flags for this link unit, each with a leading space. Per-unit rather
    // than global precisely so two roles in one build can differ.
    std::string unitFlags;
    Contract    effective = Contract::SelfContained;
    // effective != requested. `diagnostic` is then non-empty and the caller
    // is required to surface it — see INV-1/INV-4 in the analysis doc.
    bool        degraded = false;
    std::string diagnostic;
    // Mach-O + static libc++: the archive's stream initializer is appended
    // LAST in __init_offsets (Mach-O has no priority-ordered init section and
    // libc++'s <iostream> carries no `ios_base::Init` guard of its own), so a
    // global constructor that touches std::cout runs before the streams
    // exist. Asks the backend for the ordering shim. See issue #336.
    bool        streamInitShim = false;
};

namespace detail {

inline bool is_libstdcxx(std::string_view id) { return id == "libstdc++"; }
inline bool is_libcxx(std::string_view id)    { return id == "libc++"; }

}  // namespace detail

// The one table. Total by construction: every return path sets `effective`,
// and every path where `effective != requested` also sets `diagnostic`.
Mechanism resolve(const MechanismInput& in) {
    Mechanism m;
    m.effective = in.requested;

    // An archive is linked, not run. It embeds no runtime and imposes none —
    // the contract belongs to whatever eventually links it.
    if (in.role == Role::Intermediate)
        return m;

    const bool haveCxxArchives =
        !in.libcxxArchive.empty() && !in.libcxxAbiArchive.empty();

    switch (in.format) {

    // ------------------------------------------------------------- Mach-O
    case Format::MachO: {
        if (!detail::is_libcxx(in.stdlibId)) {
            // Mach-O without libc++ is not a configuration mcpp produces.
            m.effective = Contract::HostCoupled;
            m.unitFlags = " -lc++";
            if (in.requested != Contract::HostCoupled && in.explicitRequest) {
                m.degraded = true;
                m.diagnostic = std::format(
                    "cxx_runtime = \"{}\" is not available for stdlib '{}' on "
                    "Mach-O; using host-coupled", to_string(in.requested), in.stdlibId);
            }
            return m;
        }
        if (in.requested == Contract::ToolchainCoupled) {
            // The toolchain's libc++.dylib is a dead end on this
            // distribution: LLVM's macOS libc++abi/libunwind dylibs
            // upward-link /usr/lib/libc++, so the SYSTEM libc++ loads
            // alongside the toolchain's and objects freed across the two
            // copies abort in libmalloc (#202 crash forensics).
            m.effective = Contract::SelfContained;
            m.degraded  = true;
            m.diagnostic =
                "cxx_runtime = \"toolchain-coupled\" is not supported on macOS "
                "(LLVM's libc++abi/libunwind dylibs upward-link /usr/lib/libc++, "
                "which loads a second libc++ into the process); using self-contained";
        }
        if (m.effective == Contract::SelfContained) {
            if (!haveCxxArchives || !in.macosFloor) {
                m.effective = Contract::HostCoupled;
                m.degraded  = true;
                m.diagnostic = haveCxxArchives
                    ? "no macOS deployment floor resolved, so the static libc++ "
                      "that makes the floor real is pointless; using host-coupled"
                    : "this toolchain ships no libc++.a/libc++abi.a; "
                      "using host-coupled (the artifact then runs only on the "
                      "build machine's macOS version and above)";
                m.unitFlags = " -lc++";
                return m;
            }
            // -Wl,-load_hidden,<path> rather than a plain by-path link: it
            // forces the ARCHIVE (never a sibling dylib) AND gives its
            // symbols hidden visibility. Without the hidden part, dyld
            // unifies them with the system libc++ from the shared cache and
            // ostream<<int crosses into the other copy's locale machinery
            // (PR #117 forensics).
            m.unitFlags = " -nostdlib++"
                          " -Wl,-load_hidden," + in.libcxxArchive +
                          " -Wl,-load_hidden," + in.libcxxAbiArchive;
            // The ordering shim binds a libc++ INTERNAL ABI symbol, so its
            // presence is verified against the archive instead of assumed.
            // Getting this wrong must not break the link — hence a check
            // here rather than a weak reference in the shim: Mach-O's
            // weak-undefined form is `weak_import` and applies to dylib
            // symbols, so a plain weak declaration would NOT have saved a
            // missing archive symbol (it did not: ld64.lld errored outright).
            m.streamInitShim = in.streamInitSymbolPresent;
            if (!m.streamInitShim) {
                m.diagnostic =
                    "this libc++ does not export the stream initializer mcpp "
                    "orders first on macOS; a global object whose constructor "
                    "uses std::cout may crash at startup (mcpp#336). Use "
                    "cxx_runtime = \"host-coupled\" if you hit it";
            }
            return m;
        }
        // HostCoupled
        m.unitFlags = " -lc++";
        return m;
    }

    // ---------------------------------------------------------------- PE
    case Format::Pe: {
        if (!detail::is_libstdcxx(in.stdlibId)) {
            // MSVC STL (cl.exe, or clang on the MSVC ABI). The driver default
            // is the DLL runtime and mcpp emits no runtime selection flag, so
            // host-coupled is the only form that actually exists here. Both
            // other contracts are refused BY NAME rather than quietly
            // producing the same bytes and reporting success.
            m.effective = Contract::HostCoupled;
            if (in.requested == Contract::SelfContained) {
                m.degraded   = in.explicitRequest;
                m.diagnostic = in.explicitRequest
                    ? "cxx_runtime = \"self-contained\" is not implemented for the "
                      "MSVC runtime yet (it would need the /MT runtime); using "
                      "host-coupled — the artifact needs the VC++ redistributable"
                    : "";
            } else if (in.requested == Contract::ToolchainCoupled) {
                // Only reachable from an explicit request: it is never a default.
                m.degraded = true;
                m.diagnostic =
                    "cxx_runtime = \"toolchain-coupled\" has no meaning for the "
                    "MSVC runtime (it ships with the OS/redistributable, not with "
                    "the toolchain); using host-coupled";
            }
            return m;
        }
        // MinGW. `-static` is the standalone-exe convention here: the
        // piecemeal -static-libstdc++ recipe still leaves libwinpthread-1.dll.
        // It is also the spelling the libc axis uses, so `linkage = "static"`
        // keeps it regardless of the C++ runtime contract.
        const bool wantStatic =
            m.effective == Contract::SelfContained || in.fullStaticLibc;
        if (wantStatic) m.unitFlags += " -static";
        if (m.effective == Contract::SelfContained) {
            m.unitFlags += " -static-libstdc++";
            if (in.hostIsWindows) m.unitFlags += " -static-libgcc";
        }
        return m;
    }

    // --------------------------------------------------------------- ELF
    case Format::Elf:
    default: {
        if (detail::is_libstdcxx(in.stdlibId)) {
            if (m.effective == Contract::SelfContained)
                m.unitFlags = " -static-libstdc++";
            // ToolchainCoupled and HostCoupled are the same emission on ELF
            // (no flag); they differ in the rpath the link already carries,
            // which is the documented limit of this contract.
            return m;
        }
        if (detail::is_libcxx(in.stdlibId)) {
            if (m.effective != Contract::SelfContained)
                return m;   // driver default: the toolchain's libc++.so via rpath
            if (!haveCxxArchives) {
                m.effective = Contract::ToolchainCoupled;
                m.degraded  = true;
                m.diagnostic =
                    "this toolchain ships no libc++.a/libc++abi.a; using "
                    "toolchain-coupled (the artifact keeps a run-time dependency "
                    "on the toolchain's libc++.so)";
                return m;
            }
            // Verified locally: -nostdlib++ plus the three archives leaves
            // NEEDED = libc/libm/loader only. Without libunwind.a the binary
            // still pulls libunwind.so.1, which is not self-contained — so it
            // is part of the mechanism, not an optional extra.
            m.unitFlags = " -nostdlib++ " + in.libcxxArchive
                        + " " + in.libcxxAbiArchive;
            if (!in.libunwindArchive.empty()) {
                m.unitFlags += " " + in.libunwindArchive;
            } else {
                m.degraded   = true;   // effective stays SelfContained: the C++
                                       // runtime IS embedded; the unwinder is not
                m.diagnostic =
                    "this toolchain ships no libunwind.a; the C++ runtime is "
                    "embedded but the artifact keeps a run-time dependency on "
                    "libunwind.so";
            }
            return m;
        }
        // Unknown stdlib on ELF: emit nothing rather than guess, but say so
        // when something was actually asked for.
        m.effective = Contract::HostCoupled;
        if (in.requested != Contract::HostCoupled && in.explicitRequest) {
            m.degraded = true;
            m.diagnostic = std::format(
                "cxx_runtime = \"{}\" has no mechanism for stdlib '{}'; "
                "using host-coupled", to_string(in.requested), in.stdlibId);
        }
        return m;
    }
    }
}

// The Mach-O initializer-ordering shim, as a C translation unit.
//
// WHY C: it needs no standard library, no module flags and no C++ ABI of its
// own — it only has to run before everything else and poke one symbol.
//
// WHY THE NAME IS SPELLED WITH TWO UNDERSCORES: an `__asm__` label is used
// VERBATIM — clang does not add Mach-O's global `_` prefix to it. The C++
// symbol `_ZNSt3__18ios_base4InitC1Ev` therefore has to be written
// `__ZNSt3__18ios_base4InitC1Ev` here. Getting this wrong is not a silent
// no-op: ld64.lld reports `undefined symbol: ZNSt3__18ios_base4InitC1Ev` and
// every link fails, which is exactly what the first CI round did.
//
// WHY `weak_import` AND a presence check: Mach-O's weak-undefined form is
// `weak_import` (plain `weak` on a declaration does NOT make an undefined
// reference optional there). Even so, the real safety net is upstream — the
// backend only generates this TU when the archive actually defines the
// symbol, so an unexpected libc++ spelling disables the shim rather than
// breaking the link. The attribute is the second line of defence.
//
// The reference does not itself drag iostream.cpp.o out of the archive: if the
// program never touches a stream, there is nothing to order.
//
// WHY IT WORKS: libc++'s `ios_base::Init::Init()` is not empty — it is a
// guarded function-local static that calls `DoIOSInit::DoIOSInit()`, and THAT
// is the function whose relocations placement-new cin/cout/cerr. Calling it
// early constructs the streams; the archive's own `_GLOBAL__I_000100` then
// hits the same `__cxa_guard` and does nothing. `this` is never read by that
// constructor (verified by disassembly), but real storage is passed anyway.
//
// The object must be FIRST on the link line — see the backend, which prepends
// it to the link unit's inputs. Mach-O runs __init_offsets in link order.
std::string_view stream_init_shim_source() {
    return
        "/* Generated by mcpp. macOS + self-contained (static libc++) only.\n"
        " * Mach-O has no priority-ordered initializer section, so the stream\n"
        " * initializer pulled out of libc++.a lands LAST in __init_offsets and\n"
        " * a global constructor that touches std::cout sees an unconstructed\n"
        " * stream (null vptr -> SIGSEGV at process start). libc++'s <iostream>\n"
        " * has no ios_base::Init guard of its own, unlike libstdc++/MSVC STL,\n"
        " * so the header cannot fix it either. mcpp-community/mcpp#336.\n"
        " *\n"
        " * Weak: a toolchain without this exact ABI symbol links as before.\n"
        " */\n"
        "extern void mcpp_libcxx_ios_init(void *)\n"
        "    __attribute__((weak_import))\n"
        "    __asm__(\"__ZNSt3__18ios_base4InitC1Ev\");\n"
        "\n"
        "static char mcpp_libcxx_ios_init_storage[8];\n"
        "\n"
        "__attribute__((constructor))\n"
        "static void mcpp_force_std_streams(void) {\n"
        "    if (mcpp_libcxx_ios_init)\n"
        "        mcpp_libcxx_ios_init(mcpp_libcxx_ios_init_storage);\n"
        "}\n";
}

}  // namespace mcpp::build::dist
