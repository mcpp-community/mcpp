// mcpp.toolchain.cenv — a `[c-abi]` REQUEST turned into COMPILER CONFIGURATION.
//
// design 2026-09-18 (openkal, "C environment declared by the C library
// layer"), §3.2–§3.4. The C library package states the environment it
// PRESENTS to source (`posix` / `windows` / `none`), its data model and its
// `wchar_t` width; this module holds the engine's map from that request,
// together with the target it is requested for, onto the tokens a compile
// command needs to realise it — or a refusal naming what is missing.
//
// GENERIC KNOWLEDGE, NO PACKAGE NAMES. Exactly as `mcpp.targetside` hardcodes
// the five layer names and never an implementation, this module hardcodes the
// request→triple/flags MAPPING and never a C library's identity. openkal-musl
// is nowhere in this file; a second POSIX C library on Windows would realise
// through the same table.
//
// ONE MAPPING TABLE, THE ONE §3.3 PUBLISHES (coordinator-revised: realising
// `presents = "posix"` means the SAME observable fact everywhere — `__unix__`
// defined, `_WIN32` not — and targets differ only in what it COSTS to get
// there; the original text's "macOS: already satisfied" and "freestanding:
// refused" were both wrong in that specific sense, caught by real builds —
// see the note below the table):
//
//   target        request                realisation
//   Linux         posix / arch-default   the default triple already satisfies it
//   macOS         posix / arch-default   one token, `-D__unix__` — Apple's clang
//                                        predefines `__APPLE__`/`__MACH__`, never
//                                        `__unix__`, on its default triple
//   freestanding  posix / arch-default   the same one token, `-D__unix__`, for the
//                                        same reason: nothing here defines it either
//   Windows       posix / arch-default   Cygwin-flavoured: `--target=x86_64-pc-cygwin`,
//                                        `__CYGWIN__`/`__CYGWIN32__` STAY DEFINED (see
//                                        the note below the table — this is a design
//                                        revision, not the original §3.3 text)
//   *             builtins = iso         turn off the platform-C-library idioms the
//                                        code generator assumes (§3.2.1) — Apple's
//                                        `memset_pattern16` is the one measured case
//   anything else                        refused, naming the target, the request and
//                                        what is missing — never a silent downgrade
//
// THE macOS/freestanding CORRECTION, AND WHY IT MATTERS BEYOND THOSE TWO
// TARGETS. Assuming a target's default already presents an identity, when it
// does not, is not a smaller mistake than refusing a target that could have
// realised one — both hand a package a declaration that does not deliver a
// uniform answer, which is the one thing this whole feature exists to give
// it. macOS's case was caught by the VERIFICATION PROBE on a real build
// (declared `__unix__` defined, measured undefined — precisely the class of
// error §3.2's probe exists to catch, not a hole in it); freestanding's was a
// refusal that blocked a whole target (openkal-llvm-runtime on
// riscv64-none-elf) rather than delivering a fact this engine can in fact
// produce. The known cost, named in the design and left to the mcpp-index
// 30-member measurement to weigh rather than assumed away: portable code
// written as `#ifdef __unix__ ... #elif defined(__APPLE__)` now takes the
// Unix branch on macOS too, which is correct only if that branch is written
// to also be correct there.
//
// `wchar` is realised the same way on every target through one clang pair,
// `-fshort-wchar` / `-fno-short-wchar`, relative to the triple's OWN default
// width — that generalisation is this module's, not the design record's, and
// is why the VERIFICATION probe (mcpp.toolchain.cenv_probe) exists: a flag
// this table applies with confidence is still checked against what the
// compiler actually did, per §3.2's own rule that a declaration is checked,
// never trusted.
//
// COMPILE-TIME ONLY, AND DELIBERATELY SO. What changes here is the
// preprocessor identity and the `--target=` used for COMPILING; the LINK line
// keeps the triple the graph resolved (`x86_64-w64-windows-gnu`). §1.4 of the
// design measured that the object format and the calling convention are
// IDENTICAL between `x86_64-w64-windows-gnu` and `x86_64-pc-cygwin` — same PE,
// same Win64 argument placement — so an object compiled under the Cygwin
// identity links exactly like one compiled under the MinGW one. Only the
// preprocessor saw a different environment; the linker never has to know.
//
// `__CYGWIN__`/`__CYGWIN32__` ARE NOT REMOVED, AND THE FIRST VERSION OF THIS
// MODULE GOT THAT WRONG. §3.3's original text called for `-U__CYGWIN__
// -U__CYGWIN32__` on the reasoning that a real Cygwin userland is not in the
// graph. A first reading of the openkal-musl spike's libunwind build failure
// blamed a missing `__CYGWIN__` branch in libunwind itself; reading the
// vendored source shows that is wrong — upstream libunwind has no such
// branch, so defining it there would have changed nothing (the actual break
// was a downstream package selecting on `_WIN32` and is being fixed there).
// The reason to keep them defined is narrower and still real: third-party
// portable code that has to know the OBJECT FORMAT — as opposed to which C
// environment or which platform API — has no name for "PE format with a
// POSIX-presenting C environment" other than `__CYGWIN__`, and such code
// cannot be patched the way this ecosystem's own packages can. The cost is
// symmetric: a library that reaches for `__CYGWIN__` may also reach for a
// real Cygwin interface (`sys/cygwin.h`, `cygwin_conv_path`) that does not
// exist here. This is a TRADE-OFF for the 30-member measurement to settle —
// if defining it produces more new failures than it fixes, the answer flips
// — not a fact this module is asserting as closed.
export module mcpp.toolchain.cenv;

import std;
import mcpp.targetside;

export namespace mcpp::toolchain::cenv {

// What a realised `[c-abi]` declaration adds to a compile command, and what
// the verification probe must observe to confirm it actually happened.
struct Realisation {
    // Tokens appended AFTER the base `--target=<triple>` and after every other
    // token `mcpp.toolchain.hostflags` already emits. Clang takes the LAST
    // `--target=` on a command line, so a triple substitution here overrides
    // the base one without that producer having to know this module exists.
    std::vector<std::string> tokens;

    // `[c-abi].builtins = "iso"` tokens, kept apart from `tokens` only so a
    // caller that wants to report the two requests separately can.
    std::vector<std::string> builtinsTokens;

    // What the probe (mcpp.toolchain.cenv_probe) checks the FINAL command
    // line actually produced, against what was declared. 0 = not checked
    // (the declaration said nothing about that fact).
    int                      expectWcharBits = 0;
    int                      expectLongBytes = 0;   // 8 = LP64/ILP32-on-64bit is N/A; see below
    std::vector<std::string> expectDefined;
    std::vector<std::string> expectUndefined;

    // NO "REVERSAL" FIELD FOR `c-environment = "platform"` (design §3.4), AND
    // DELIBERATELY. `tokens`/`builtinsTokens` are broadcast ADDITIVELY into
    // every ordinary package's own compile inputs (`prepare.cppm`, beside the
    // existing target-side-usage broadcast) and left OUT of a platform
    // package's — the base command line these tokens are appended to is
    // therefore untouched either way, so "opting out" needs no counter-flags,
    // only the absence of the ones everyone else received.
};

// Heuristic bit width from an ARCH SPELLING, not a lookup table of
// architectures — mcpp's own arch vocabulary (docs/21) is `x86_64`,
// `aarch64`, `riscv64`, `wasm32`, and every 64-bit member of it contains
// "64" while every 32-bit one does not. A future arch this heuristic gets
// wrong fails LOUDLY, at the refusal below, rather than silently picking a
// data model nobody asked for — `realise` only trusts this for `arch-default`
// and refuses every explicit request it cannot itself verify by triple
// substitution.
inline bool arch_is_64bit(std::string_view arch) {
    return arch.find("64") != std::string_view::npos;
}

// WHAT `data-model = "arch-default"` MEANS: THE CONVENTION THE DECLARED
// ENVIRONMENT ITSELF USES ON THAT ARCHITECTURE, NOT THE TRIPLE'S. design
// §3.2's own example is explicit about this — "该架构上 musl 自己的模型，
// 64 位即 LP64" (the model musl itself uses on that architecture; 64-bit
// means LP64) — for a `presents = "posix"` C library: POSIX's own `long`
// convention on a 64-bit architecture is LP64, full stop, independent of
// which OS is underneath. A `presents = "windows"` C library's own
// convention is the Windows CRT's, LLP64, for the same reason `env = "gnu"`
// on a Windows triple names UCRT's data model and not glibc's (docs/21).
// `presents = "none"` never reaches this function with a data model to
// decide: the identity check above refuses it on every hosted target this
// table covers, and a freestanding target skips the data-model check
// entirely (there is no OS-conventional `long` to compare against).
//
// COMPILED WRONG ONCE, AND THE BUG IS WORTH NAMING. An earlier revision took
// no `presents` parameter here at all and always answered the POSIX
// convention, which happened to be right for the flagship case
// (`presents = "posix"` on Windows/x86_64: LP64) and wrong for the control
// case beside it in the unit tests (`presents = "windows"`, `arch-default`,
// on Windows/x86_64: this function answered LP64, `realise` then refused
// because native Windows is LLP64). The fix is the `presents` parameter:
// `arch-default` asks what THIS DECLARED PRESENTS value's own convention is,
// `triple_native_data_model` below asks what the BASE TRIPLE already has
// with no declaration at all, and the two are compared to decide whether any
// realisation is needed — never confused for one function.
inline mcpp::targetside::CAbiDataModel arch_default_data_model(
    mcpp::targetside::CAbiPresents presents, std::string_view arch) {
    using mcpp::targetside::CAbiDataModel;
    using mcpp::targetside::CAbiPresents;
    if (presents == CAbiPresents::Windows) return CAbiDataModel::Llp64;
    return arch_is_64bit(arch) ? CAbiDataModel::Lp64 : CAbiDataModel::Ilp32;
}

// The data model the BASE, UNMODIFIED triple already has — what deciding
// "is any realisation needed at all" is compared against, and what
// `presents = "none"`'s `arch-default` takes since it states no convention
// of its own. Windows is LLP64 on every architecture it supports, MinGW and
// MSVC alike; every other OS this table covers follows the architecture's
// own convention.
inline mcpp::targetside::CAbiDataModel triple_native_data_model(
    std::string_view os, std::string_view arch) {
    using mcpp::targetside::CAbiDataModel;
    if (os == "windows") return CAbiDataModel::Llp64;
    return arch_is_64bit(arch) ? CAbiDataModel::Lp64 : CAbiDataModel::Ilp32;
}

// The `wchar_t` width a target's default triple already has. Measured
// (design §1.4): 16 on Windows (MinGW and Cygwin alike, absent
// `-fno-short-wchar`), 32 everywhere else clang targets (Linux, macOS,
// hosted ELF/Mach-O).
//
// WHAT IS NOT IN THIS FUNCTION. The freestanding case used to be answered
// "32", on the reasoning that `riscv64-none-elf` etc. measure as 32 bits on
// Linux and macOS hosts. The wave's Windows-host × riscv64-none-elf
// measurement showed that assumption to be wrong: clang on a Windows host
// still uses MinGW's `<winnt.h>` defaults even with `--target=riscv64-none-elf`,
// and `__SIZEOF_WCHAR_T__` measures 2 (16 bits) there. The probe correctly
// flagged this as a mismatch with `decl.wcharBits = 32`. The fix is in the
// `wchar` realisation below — always add `-fno-short-wchar` when the
// declaration asks for 32, regardless of what the toolchain would have
// defaulted to — so the probe then measures the state the engine actually
// produced, not the host's leak.
inline int native_wchar_bits(std::string_view os) {
    return os == "windows" ? 16 : 32;
}

// `decl.declared` must be true — callers hold `TargetSide::cAbiDecl`, which is
// only ever set when a `[c-abi]` block was read (§3.2). `os`/`arch` are mcpp's
// own triple fields (`docs/21`); `freestanding` is `Triple::is_freestanding()`.
//
// Returns the tokens to append, or a refusal naming the target, the request
// and what is missing (§3.2: "无法满足...明确拒绝并说明缺什么，不静默降级").
inline std::expected<Realisation, std::string> realise(
    const mcpp::targetside::CAbiDecl& decl, std::string_view os,
    std::string_view arch, bool freestanding) {
    using mcpp::targetside::CAbiPresents;
    using mcpp::targetside::CAbiDataModel;

    Realisation r;
    auto refuse = [&](std::string_view missing) -> std::expected<Realisation, std::string> {
        return std::unexpected(std::format(
            "the C library's [c-abi] declaration cannot be realised for "
            "'{}{}'.\n"
            "       requested   presents = {}, data-model = {}, wchar = {}\n"
            "       missing     {}\n"
            "       mcpp knows one realisation for a POSIX-presenting "
            "environment on a non-POSIX target: the Cygwin-flavoured "
            "compile on Windows/x86_64 (design 2026-09-18 §3.3). Every "
            "other combination this build asked for has no known mapping "
            "and is refused rather than silently approximated.",
            os, freestanding ? " (freestanding)" : "",
            mcpp::targetside::c_abi_presents_name(decl.presents),
            mcpp::targetside::c_abi_data_model_name(decl.dataModel),
            decl.wcharBits, missing));
    };

    // Whether the identity branch below chose the Cygwin substitution — the
    // one mechanism that also MOVES the data model (native LLP64 to LP64) as
    // a side effect. The data-model check right after needs to compare
    // against what the compile line ACTUALLY has at that point, not against
    // the triple's original default, or a `data-model = "llp64"` requested
    // alongside `presents = "posix"` would read as "already satisfied" (the
    // triple's own native IS LLP64) while the identity switch already moved
    // it to LP64 — a contradiction the mapping should refuse outright rather
    // than leave for the verification probe to catch after a real compile.
    bool cygwinIdentity = false;

    // ── `presents`: the environment-identity macros ─────────────────────────
    //
    // THE RULE (design 2026-09-18 §3.3, coordinator revision from the
    // openkal-musl/openkal-llvm-runtime spikes, superseding the original
    // per-OS table): realising `presents = "posix"` means the SAME
    // observable thing on every target — `__unix__` defined, `_WIN32` not —
    // and targets differ only in what it COSTS to get there. Linux: nothing,
    // the default triple already presents it. macOS and a freestanding
    // target: the cost is one token, `-D__unix__` — Apple's clang predefines
    // `__APPLE__`/`__MACH__`, never `__unix__` (measured; the ORIGINAL design
    // text assumed macOS's default already satisfied `presents = "posix"`,
    // which the verification probe itself caught as false — declared
    // defined, measured undefined — which is exactly what that probe exists
    // to catch), and a freestanding target starts with neither macro defined
    // at all for the identical reason. Windows: the Cygwin-flavoured
    // substitution (below). This is the point of the whole feature: a
    // package that reads `presents = "posix"` gets ONE fact to check, not a
    // target-shaped one it may as well have kept detecting for itself
    // (`#ifdef __unix__ ... #elif defined(__APPLE__)` still branches WRONG
    // on a `-D__unix__` macOS build if it does not also treat `__unix__` as
    // authoritative — a real cost, not a hidden one, and a measurement
    // question for the 30-member wave rather than a reason to withhold the
    // define).
    if (freestanding) {
        if (decl.presents == CAbiPresents::Posix) {
            r.tokens.push_back("-D__unix__");
            r.expectDefined.push_back("__unix__");
            r.expectUndefined.push_back("_WIN32");
        } else if (decl.presents != CAbiPresents::None) {
            // `none` is the value the (deferred) reduced-ISO-C form uses and
            // needs no realisation; `windows` asks for an identity that
            // cannot exist without an operating system under it.
            return refuse("a freestanding target has no platform identity "
                          "macros to become `windows`");
        }
    } else if (os == "windows") {
        if (decl.presents == CAbiPresents::Posix) {
            if (arch != "x86_64")
                return refuse("the Cygwin-flavoured realisation is measured "
                              "on x86_64 only; this arch has no verified "
                              "substitute triple");
            // `--target=x86_64-pc-cygwin`, on the COMPILE line only (module
            // header above). `__CYGWIN__`/`__CYGWIN32__` are LEFT AS THE
            // TRIPLE SUBSTITUTION DEFINES THEM — not undefined (see the
            // module header's note: portable third-party code that needs to
            // know the object format has no other name for "PE format,
            // POSIX-presenting environment", and this is a trade-off for the
            // 30-member measurement, not a settled fact).
            r.tokens.push_back("--target=x86_64-pc-cygwin");
            r.expectDefined.push_back("__unix__");
            r.expectDefined.push_back("__CYGWIN__");
            r.expectUndefined.push_back("_WIN32");
            cygwinIdentity = true;
        } else if (decl.presents == CAbiPresents::Windows) {
            // Already the base triple's own identity — nothing to add.
            r.expectDefined.push_back("_WIN32");
        } else {
            return refuse("no known way to suppress every environment-"
                          "identity macro on a hosted Windows triple");
        }
    } else if (os == "linux") {
        if (decl.presents == CAbiPresents::Posix) {
            // The default triple already presents POSIX — nothing to add.
            r.expectDefined.push_back("__unix__");
            r.expectUndefined.push_back("_WIN32");
        } else {
            return refuse(std::format(
                "no known way to make a {} target present `{}` — that "
                "identity belongs to a different object format",
                os, mcpp::targetside::c_abi_presents_name(decl.presents)));
        }
    } else if (os == "macos") {
        if (decl.presents == CAbiPresents::Posix) {
            // UNLIKE LINUX: Apple's clang predefines `__APPLE__`/`__MACH__`
            // on its default triple, never `__unix__` (measured — this is
            // the correction, not the original design text: the identity
            // was assumed to be free here the same way it is on Linux, and
            // the verification probe below caught that assumption as false
            // on a real build). One token closes it.
            r.tokens.push_back("-D__unix__");
            r.expectDefined.push_back("__unix__");
            r.expectUndefined.push_back("_WIN32");
        } else {
            return refuse(std::format(
                "no known way to make a {} target present `{}` — that "
                "identity belongs to a different object format",
                os, mcpp::targetside::c_abi_presents_name(decl.presents)));
        }
    } else {
        return refuse("this OS is outside the mapping table §3.3 publishes");
    }

    // ── `data-model` ─────────────────────────────────────────────────────────
    if (!freestanding) {
        // `want`: what `arch-default` MEANS — the C LIBRARY's own convention
        // for this architecture (`arch_default_data_model`), never the
        // triple's. `tripleNative`: what the BASE, unmodified triple already
        // has, which decides whether anything needs to change at all.
        const auto want = decl.dataModel == CAbiDataModel::ArchDefault
                         ? arch_default_data_model(decl.presents, arch) : decl.dataModel;
        // What the compile line ALREADY has at this point — the triple's own
        // native model, unless the identity switch above already moved it
        // (the Cygwin mechanism: native LLP64 to LP64, as a side effect of
        // the `--target=` substitution, not a second flag). Comparing `want`
        // against THIS rather than against the triple's original default is
        // what makes `presents = "posix", data-model = "llp64"` a refusal
        // instead of a false "already satisfied": the identity switch that
        // `presents = "posix"` required already left LLP64 behind.
        const auto effectiveNative = cygwinIdentity ? CAbiDataModel::Lp64
                                                     : triple_native_data_model(os, arch);
        if (want == effectiveNative) {
            // Already satisfied — the common case, and the only one that
            // needs no further check: `arch-default` on Linux/macOS/native
            // Windows, or on the Cygwin identity once it has already moved
            // the model to LP64.
        } else {
            return refuse(std::format(
                "data-model = {} has no realisation on {}/{} once `presents "
                "= \"{}\"` is realised (that leaves the model at {})",
                mcpp::targetside::c_abi_data_model_name(want), os, arch,
                mcpp::targetside::c_abi_presents_name(decl.presents),
                mcpp::targetside::c_abi_data_model_name(effectiveNative)));
        }
        r.expectLongBytes =
            (want == CAbiDataModel::Lp64) ? 8
          : (want == CAbiDataModel::Llp64 || want == CAbiDataModel::Ilp32) ? 4
          : 0;
    }

    // ── `wchar` ──────────────────────────────────────────────────────────────
    //
    // The freestanding case used to be "assumed native, no token added".
    // The wave's Windows-host × riscv64-none-elf measurement (openkal-
    // llvm-runtime#24, 2026-09-18) caught that assumption as wrong:
    // clang on a Windows host uses MinGW's `<winnt.h>` defaults even with
    // `--target=riscv64-none-elf`, so the toolchain's own default for
    // `wchar_t` is 16 bits there — and a `decl.wcharBits = 32` (musl's
    // declaration) would compile to a 16-bit `wchar_t` because no token
    // was added. The probe caught this; the right fix is here, not in the
    // probe: a freestanding target ALWAYS gets `-fno-short-wchar` for
    // `wchar=32`, so the compiler produces 32-bit `wchar_t` regardless of
    // what its host-contaminated default would have been. Symmetric for
    // `wchar=16`: a freestanding target's toolchain default on Linux/macOS
    // is 32, so `-fshort-wchar` is always needed too. The hosted cases
    // are unchanged — `native_wchar_bits(os)` is correct for every hosted
    // target, because the host's toolchain default is what the real
    // compile actually sees (Windows host has `-fshort-wchar` baked in
    // via MinGW headers; Linux/macOS hosts have 32 bits).
    //
    // The probe below then measures what this engine actually produced
    // (32 bits on a freestanding target, with the flag) — not the host's
    // leak — and the declaration holds. There is no measurement-side
    // workaround here: the previous version had a "freestanding target
    // skips the wchar flag" rule that was wrong on Windows hosts, and
    // adding it costs nothing on Linux/macOS hosts (they default to 32,
    // so the flag is redundant but harmless on the hosted freestanding
    // builds that don't exist; for hosted Linux/macOS, no flag is added).
    if (decl.hasWchar) {
        if (freestanding) {
            // Freestanding: the toolchain's host-contaminated default is
            // not something this engine can trust. Always emit the
            // token that makes the compile match the declaration.
            r.tokens.push_back(decl.wcharBits == 32 ? "-fno-short-wchar"
                                                     : "-fshort-wchar");
        } else if (decl.wcharBits != native_wchar_bits(os)) {
            // Hosted: the target's own default is what the compile sees,
            // and `native_wchar_bits(os)` correctly captures it
            // (16 on Windows because MinGW headers bake in
            // `-fshort-wchar`; 32 on Linux and macOS).
            r.tokens.push_back(decl.wcharBits == 32 ? "-fno-short-wchar"
                                                     : "-fshort-wchar");
        }
        r.expectWcharBits = decl.wcharBits;
    }

    // ── `builtins` ───────────────────────────────────────────────────────────
    //
    // §3.2.1: the compiler assumes the PLATFORM C library provides certain
    // symbols, and that assumption survives the identity switch above because
    // it happens at code generation, after preprocessing. Surveyed against
    // clang 22's `-fno-builtin-<name>` family (one flag per recognised
    // idiom):
    //
    //   Apple targets    `memset_pattern16`/`memset_pattern4`/`memset_pattern8`
    //                     — libSystem loop-idiom recognition (measured, §3.2.1).
    //   Windows targets   NO ANALOGOUS CODE-GENERATION IDIOM FOUND. The
    //                     design's own Windows row (§3.2.1) is a PREPROCESSOR
    //                     assumption instead — clang's bundled `intrin.h` /
    //                     `mm_malloc.h` expecting `__mingw_aligned_malloc` —
    //                     and that is already closed by the EXISTING
    //                     `-nostdlibinc` isolation (`mcpp.toolchain.hostflags`,
    //                     #662/#664) whenever the graph supplies the C
    //                     library, independently of `builtins`. This survey
    //                     found no Windows loop-idiom builtin to disable.
    //   Linux targets     NONE FOUND. glibc's own loop-idiom builtins
    //                     (`__memset_chk` and relatives) are FORTIFY_SOURCE
    //                     machinery that is off by default and unrelated to
    //                     code-generation idiom recognition.
    //
    // So `iso` realises to one flag, on Apple targets only, and does nothing
    // measurable elsewhere today. That is reported rather than silently
    // accepted: a caller that wants to know what changed reads `builtinsTokens`.
    if (decl.builtins == mcpp::targetside::CAbiBuiltins::Iso) {
        if (!freestanding && (os == "macos" || os == "ios"))
            r.builtinsTokens.push_back("-fno-builtin-memset_pattern16");
    }

    return r;
}

} // namespace mcpp::toolchain::cenv
