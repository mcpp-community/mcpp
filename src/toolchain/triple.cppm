// mcpp.toolchain.triple — the single source of truth for target identity.
//
// mcpp owns its target-triple language: canonical form is `arch-os[-env]`
// (three segments, no vendor — Zig-style). `x86_64-linux-musl` was already
// canonical before this module existed; this extends the same convention to
// every target. GNU/LLVM spellings (`x86_64-w64-mingw32`,
// `x86_64-unknown-linux-gnu`, `arm64-apple-darwin24`) are permanent input
// aliases, normalized here.
//
// Everything that previously parsed triples ad hoc (cfgpred::context_for,
// abi_profile, model.cppm's is_*_target, registry's musl signals) consumes
// this module now. Vocabulary: os ∈ {linux, macos, windows} (never "darwin"),
// arch is the GNU spelling ({x86_64, aarch64, riscv64, …} — never "arm64"),
// env ∈ {gnu, musl, msvc} (empty on macos). `static` is NOT part of a triple:
// it is a target's default linkage property, flipped via [build].
//
// The known-target table below is the closed vocabulary `--target` validates
// against (with an escape hatch for explicit [target.X] manifest sections)
// and the source the README platform table is drawn from. Adding a target =
// adding a row here (+ payload mapping in registry.cppm if a new payload
// shape is involved).
//
// See .agents/docs/2026-07-15-toolchain-target-naming-unification-design.md.

export module mcpp.toolchain.triple;

import std;
import mcpp.platform;

export namespace mcpp::toolchain::triple {

struct Triple {
    std::string arch;   // "x86_64" | "aarch64" | "riscv64" | ... (GNU spelling)
    std::string os;     // "linux" | "macos" | "windows"
    std::string env;    // "gnu" | "musl" | "msvc" | "" (always empty on macos)

    // ⚠️ WHETHER THE ENV SEGMENT WAS WRITTEN, AS OPPOSED TO SUPPLIED BY THIS
    // PARSER — AND THE TRIPLE HAS TO CARRY BOTH BECAUSE IT SERVES TWO ROLES.
    //
    // A triple is an IDENTITY — the output directory's name, part of a cache
    // key, the subject of a `cfg()` predicate — and identities must be total
    // and canonical. It is also a REQUEST, and a request has to be able to say
    // nothing. `parse` makes the identity total by filling `x86_64-linux` in as
    // `x86_64-linux-gnu`, and until this flag existed that filling ALSO
    // destroyed the request: the two states were indistinguishable downstream.
    //
    // Measured: a project whose graph supplies musl, built with
    // `--target x86_64-linux`, reported
    //
    //     Target x86_64-linux-gnu → x86_64-unknown-linux-gnu
    //            c-abi   musl   (openkal-musl@0.3.3, graph)
    //
    // — a name that contradicts the fact printed two lines under it. The user
    // had declined to name a C library; the parser named one for them.
    //
    // The narrow shape is deliberate. Removing the fill would make `env` empty
    // for a hosted target at 22 read sites, ten of which are in this file, and
    // every one would need a new answer for a state that never existed before.
    // A flag beside the value leaves the identity exactly as it was and gives
    // the request somewhere to live.
    bool envExplicit = false;

    bool empty() const { return arch.empty() && os.empty(); }

    // Canonical rendering: "arch-os[-env]"; "" for an empty (= host) triple.
    std::string str() const {
        if (empty()) return {};
        std::string s = arch + "-" + os;
        if (!env.empty()) { s += "-"; s += env; }
        return s;
    }

    // ⭐⭐ THE SPELLING A COMPILER TAKES, WHICH IS NOT THE SPELLING mcpp USES.
    //
    // `str()` is mcpp's vocabulary: short, unambiguous, and the thing a user
    // types. LLVM's is a four-field form with a vendor, and on Apple platforms
    // the architecture has a different name and the OS carries a version.
    //
    // ⚠️ THIS EXISTS BECAUSE CROSS-COMPILING USED TO MEAN SOMETHING NARROWER.
    // Every hosted cross mcpp could do was served by a payload whose DRIVER was
    // already specialised — `x86_64-w64-mingw32-g++` needs no `--target`,
    // because it has only one. So nothing ever needed this function, and
    // nothing emitted `--target=` outside the freestanding path.
    //
    // openkal changes the shape of the question. The target side — headers,
    // C library, C++ runtime, the OS's own openkal implementation — is a set of
    // PACKAGES in the dependency graph, built from source by whichever compiler
    // is running. What remains for the compiler is code generation, and clang
    // emits every format it was built with from one binary. There is no payload
    // to specialise, so the triple has to be said out loud.
    //
    // ⚠️ Measured 2026-08-23, before this existed: a build for `aarch64-macos`
    // resolved the whole graph, took musl's aarch64 headers, and compiled with
    // NO `--target` at all — so the host's x86_64 code generation met aarch64
    // declarations. What caught it was the port's own assertion, which exists
    // for exactly this:
    //
    //     okm_float_assert.c: the C library and the compiler disagree about
    //     LDBL_DIG  ('33 == 18')
    //
    // 33 is aarch64's binary128; 18 is x87. Two machines in one command line.
    std::string llvm_triple(std::string_view macosVersion = {}) const {
        if (empty()) return {};
        if (os == "macos") {
            // Apple spells the 64-bit ARM architecture `arm64`, and the OS
            // component carries the deployment target: `arm64-apple-macos14`.
            // Without a version clang picks its own default, which is a
            // decision belonging to the project rather than to the compiler.
            const std::string a = (arch == "aarch64") ? "arm64" : arch;
            std::string t = a + "-apple-macos";
            t += macosVersion.empty() ? std::string("14.0")
                                      : std::string(macosVersion);
            return t;
        }
        if (os == "windows") {
            if (is_msvc_env()) return arch + "-pc-windows-msvc";
            return arch + "-w64-windows-gnu";
        }
        if (os == "linux") return arch + "-unknown-linux-" + (env.empty() ? "gnu" : env);
        if (os == "none")  return str();   // freestanding: already LLVM's form
        return str();
    }

    bool is_musl() const        { return env == "musl"; }
    bool is_msvc_env() const    { return env == "msvc"; }
    bool is_windows_gnu() const { return os == "windows" && env == "gnu"; }
    bool is_pe() const          { return os == "windows"; }

    // Bare metal: there is no OS to link against. THE predicate every
    // freestanding decision keys off, spelled once here so no consumer
    // re-derives it from `os == "none"` and drifts.
    bool is_freestanding() const { return os == "none"; }

    // ⭐⭐ WHETHER THIS ROW'S TOOLCHAIN PIN IS A CAPABILITY RATHER THAN A
    // CONVENTION — the distinction that decides whether an author may override
    // it.
    //
    // A hosted row's pin answers "which payload supplies this target's C
    // library", and an author who supplies one may name any compiler. These
    // rows answer a different question, and the answer does not depend on who
    // supplies what:
    //
    //   freestanding   no per-host cross payload exists at all; clang and lld
    //                  are cross-compilers by construction and gcc is not.
    //   PE + musl      no gcc payload emits a PE with a musl C library. The
    //                  mingw payload emits PE with the MinGW CRT, which is the
    //                  separate `-gnu` row; there is no third gcc.
    //
    // ⚠️ SPELLED HERE RATHER THAN AT EACH DECISION, because the first version
    // said `is_freestanding()` at two of them and `x86_64-windows-musl` — a row
    // added later — was a convention at both. Measured: declaring gcc for it
    // resolved the host's Linux musl payload and reported a missing C++
    // frontend.
    bool pin_is_capability() const { return is_freestanding() || (is_pe() && is_musl()); }

    // cfg() `family` dimension: unix | windows.
    std::string family() const {
        if (os == "windows") return "windows";
        if (os == "linux" || os == "macos") return "unix";
        return {};
    }

    // NASM `-f` output format for this target. NASM is x86-family only:
    // nullopt off x86, and the caller must hard-error (suggesting cfg-gated
    // sources) rather than pick a format.
    std::optional<std::string> nasm_format() const {
        bool x64 = arch == "x86_64";
        bool x32 = arch == "x86" || arch == "i386" || arch == "i486"
                || arch == "i586" || arch == "i686";
        if (!x64 && !x32) return std::nullopt;
        if (os == "windows") return x64 ? "win64" : "win32";
        if (os == "macos")   return x64 ? "macho64" : "macho32";
        if (os == "linux")   return x64 ? "elf64" : "elf32";
        return std::nullopt;
    }

    // ⚠️ IDENTITY IS THE THREE SEGMENTS, AND `envExplicit` IS DELIBERATELY NOT
    // AMONG THEM — WHICH IS WHY THIS IS NOT `= default`.
    //
    // The flag records where the env segment came from, not what the target is.
    // A defaulted comparison would make `x86_64-linux-gnu` written by a user
    // unequal to the same triple derived by `host_triple`, and the first thing
    // that breaks is the `host` tag in `mcpp toolchain list`, which compares
    // exactly those two.
    bool operator==(const Triple& o) const {
        return arch == o.arch && os == o.os && env == o.env;
    }
};

// Lenient parse of any recognizable triple spelling into canonical fields.
// Handles mcpp-canonical ("x86_64-linux-musl"), GNU ("x86_64-w64-mingw32",
// "x86_64-pc-linux-gnu"), LLVM/Rust 4-segment ("x86_64-unknown-linux-musl",
// "x86_64-pc-windows-msvc") and Apple ("arm64-apple-darwin24.1.0",
// "arm64-apple-macosx15.0") forms. Returns nullopt when no OS is
// recognizable — the input is not a triple at all.
std::optional<Triple> parse(std::string_view s);

// ── Known-target registry (closed vocabulary; data, not code) ────────────────
//
// tier semantics (Rust-style):
//   verified  — CI builds AND executes the artifact end-to-end (qemu/wine count)
//   planned   — registered intent; payload or CI row not wired yet
struct TargetInfo {
    std::string_view canonical;   // "x86_64-linux-musl"
    std::string_view tier;        // "verified" | "planned"
    std::string_view note;        // display annotation: "static" / "PE" / ""
    // Convention toolchain pin for `--target <canonical>` with no explicit
    // [target.X] toolchain override. Empty = no convention (host default).
    std::string_view pin;
    // The TARGET's C library, resolved at compile time exactly the way `pin`
    // resolves the compiler. Empty = none applies.
    //
    // ⚠️ This axis exists because bare metal was the one target class without
    // it, and the gap leaked into every package. A hosted target gets its libc
    // automatically — `x86_64-linux-musl` carries musl inside its gcc payload,
    // and glibc arrives through PayloadPaths — so nobody writes `xim:glibc` in
    // a manifest. Freestanding pins a generic clang, which brings no target
    // libc at all, so before this every bare-metal package had to declare
    // `[xlings] deps = ["xim:picolibc-riscv@1.8.12"]` itself. That is not a
    // dependency of the package; it is a property of the target, and stating
    // it per-package bound a board-support package and a standard-library
    // subset alike to one libc, one ISA and one version.
    std::string_view sysroot;
    bool defaultStatic;           // target's default linkage is static
};

// (note deliberately excludes "static" — the display layer derives that tag
// from defaultStatic, so listing it here would duplicate it.)
inline constexpr TargetInfo kKnownTargets[] = {
    // canonical               tier         note   pin           sysroot                        defaultStatic
    { "x86_64-linux-gnu",      "verified",  "",    "",           "",                            false },
    { "x86_64-linux-musl",     "verified",  "",    "gcc@16.1.0", "",                            true  },
    { "aarch64-linux-musl",    "verified",  "",    "gcc@16.1.0", "",                            true  },
    { "x86_64-windows-gnu",    "verified",  "PE",  "gcc@16.1.0", "",                            true  },
    // ⚠️ musl ON WINDOWS. IT EXISTS, AND UNTIL THIS ROW mcpp HAD NO NAME FOR IT.
    //
    // LLVM's triple vocabulary offers `gnu` and `msvc` for Windows and both
    // name an ABI, so a reader concludes there is no third possibility and
    // calls a musl-based Windows build `-gnu`. The artefact disagrees. Measured
    // on one built over `openkal-musl`:
    //
    //     imports        ntdll, KERNEL32, SHELL32 — no msvcrt, no ucrtbase
    //     `_Z…` symbols  4507        `?…` symbols  0
    //
    // No MinGW C runtime is linked. That is musl on Windows, and calling it
    // `gnu` put the one thing the C library is not into its identity, its
    // output directory, its `cfg(env = …)` and its packed ABI tag.
    //
    // ⭐ THE FIX IS A NAME, NOT A MECHANISM, AND THE REASON THE MISTAKE HELD SO
    // LONG IS WORTH RECORDING. "LLVM cannot spell x86_64-windows-musl" is true
    // and is about the string handed to CLANG. mcpp's canonical form is a
    // different string — the build report prints both, either side of an arrow:
    //
    //     Target x86_64-windows-gnu → x86_64-w64-windows-gnu
    //            ^ mcpp's identity    ^ what clang is given
    //
    // Letting the compiler's vocabulary bound mcpp's own merged two axes into
    // one. `llvm_triple()` already sends every non-MSVC Windows target to
    // `…-w64-windows-gnu`, and that spelling is correct there and stays: it
    // selects the Itanium C++ ABI, which is the ABI this C library was compiled
    // for. mcpp's name answers a different question — which C library — and now
    // it can.
    //
    //     mcpp target             → clang                    c-abi
    //     x86_64-linux-musl       → x86_64-unknown-linux-musl  musl
    //     x86_64-windows-gnu      → x86_64-w64-windows-gnu     MinGW CRT
    //     x86_64-windows-musl     → x86_64-w64-windows-gnu     musl
    //
    // The last two rows differ in the first column and agree in the second,
    // which is the whole point.
    //
    // ⚠️ THE PIN IS `llvm`, AND IT IS NOT A PREFERENCE. The column names the
    // payload that supplies this target's C library everywhere else in this
    // table; here nothing supplies it, and what the column has to prevent is
    // the OPPOSITE — a global default of gcc being carried onto a target no gcc
    // can emit. Measured with `mcpp toolchain default gcc@16.1.0`:
    //
    //     error: toolchain payload 'xim:musl-gcc@16.1.0' has no known C++
    //            frontend in …/xim-x-musl-gcc/16.1.0/bin
    //
    // — a message about a missing frontend, for a target whose real problem is
    // that only clang emits it at all. The bare-metal rows carry `llvm` for the
    // same reason and say so in their own note.
    //
    // The C library still comes from the dependency graph. `host_can_serve`
    // says no for this row on a non-Windows host — correctly, for the prebuilt
    // system — and that refusal is diagnosed early and RELEASED once the graph
    // is known (see the long note at prepare.cppm's `unservedTargetDiagnosis`),
    // so a project whose C library comes from a dependency is not turned away.
    //
    // ⚠️ TIER IS `preview`, NOT `verified`. `verified` in this table means an
    // artefact was built AND RUN. Running a PE on a Linux host needs wine, and
    // openkal's CI has that step — so this is measurable, and the tier moves
    // when it has been measured rather than when it seems likely.
    { "x86_64-windows-musl",   "preview",   "PE",  "llvm@22.1.8","",                            true  },
    { "x86_64-windows-msvc",   "verified",  "PE",  "",           "",                            false },
    { "aarch64-macos",         "verified",  "",    "",           "",                            false },
    { "riscv64-linux-musl",    "planned",   "",    "",           "",                            true  },
    { "aarch64-linux-gnu",     "planned",   "",    "",           "",                            false },
    { "x86_64-macos",          "planned",   "",    "",           "",                            false },
    // Bare metal. `defaultStatic` is not a preference here — there is no
    // loader, so there is no other option. The pin is llvm on every host
    // because clang/lld are cross-compilers by construction: unlike the hosted
    // rows above, these need no per-host cross payload at all.
    // ISA profile (-march/-mabi/-mcmodel) lives in mcpp.freestanding.target,
    // which is the single place that decision is made.
    // The sysroot column is what keeps a bare-metal PACKAGE from having to
    // name a libc: the C library is the target's, like the compiler.
    { "riscv64-none-elf",      "verified",  "bare","llvm@22.1.8","xim:picolibc-riscv@1.8.12",  true  },
    { "riscv32-none-elf",      "verified",  "bare","llvm@22.1.8","xim:picolibc-riscv@1.8.12",  true  },
    // ⚠️ AN EMPTY SYSROOT COLUMN, AND IT IS A STATEMENT RATHER THAN AN OMISSION.
    //
    // The two rows above name a C library because a project targeting them
    // ordinarily wants one. This row does not, because there is no aarch64
    // build of picolibc in the index — and, more to the point, because the
    // first consumer of this row does not want one. `openarch` is a layer of
    // machine mechanism: contexts, traps, page-table entries. It references no
    // C library symbol, and a row that resolved one would make every project
    // on this target carry a payload it never calls.
    //
    // An empty column here means exactly what `[target.<triple>].sysroot = ""`
    // means in a manifest — the zero-libc tier: no headers on the compile line,
    // no library directory on the link, and `#include <stdio.h>` does not
    // resolve. A project that wants a C library on this target says so in its
    // own manifest, which is also how it would choose a different one.
    //
    // ⚠️ The tier is `preview` and not `verified`: `verified` in this table
    // means an image has been built AND RUN for the row, and running one needs
    // an emulator. `xim:qemu-arm` provides `qemu-system-aarch64`; until a probe
    // has actually booted under it, claiming `verified` would be claiming the
    // measurement rather than reporting it.
    { "aarch64-none-elf",      "preview",   "bare","llvm@22.1.8","",                            true  },
    // ⚠️ THIS ROW EXISTS SO THAT A THIRD MACHINE CAN DISAGREE WITH THE FIRST
    // TWO, WHICH IS THE ONLY THING THAT TELLS AN ABSTRACTION FROM A HABIT.
    //
    // riscv64 and aarch64 are both load/store RISC machines with a weak memory
    // model and a fixed instruction width, so an interface that fits both may
    // fit because it is right or because they are alike. x86_64 is neither: it
    // has variable-length instructions, a total-store-order memory model under
    // which three of openarch's four barriers need no instruction at all, and
    // an interrupt mechanism that is a table of gates rather than a base
    // register. What survives all three is an abstraction.
    //
    // ⚠️ The tier is `preview` for the same reason aarch64's is, and the reason
    // is stricter than it sounds: `verified` here means an image was built AND
    // RUN. `xim:qemu-x86` does not exist yet — the index carries no
    // `qemu-system-x86_64` — so nothing on this row has booted. Claiming
    // `verified` would be claiming a measurement that has not been made.
    //
    // The sysroot column is empty, the zero-libc tier, for the reason given
    // above `aarch64-none-elf`: the first consumer is `openarch`, which
    // references no C library symbol.
    { "x86_64-none-elf",       "preview",   "bare","llvm@22.1.8","",                            true  },
};

inline std::span<const TargetInfo> known_targets() { return kKnownTargets; }

inline const TargetInfo* find_known_target(const Triple& t) {
    auto s = t.str();
    for (auto& k : kKnownTargets)
        if (k.canonical == s) return &k;
    return nullptr;
}

inline bool is_known_target(const Triple& t) { return find_known_target(t) != nullptr; }

// ── Completing a request that declined to name a C library ──────────────────
//
// ⚠️⚠️ `parse` FILLS THE ENV SEGMENT LEXICALLY, AND THE TIER GATE USED TO ASK
// ABOUT THE FILLED VALUE RATHER THAN ABOUT THE REQUEST.
//
// The fill is an IDENTITY operation and has to stay exactly as it is: total,
// lexical, and independent of the host (see the note on `Triple::envExplicit`
// and the one beside the fill itself). `x86_64-linux` is the identity
// `x86_64-linux-gnu` on every machine, and a unit test says so.
//
// What it is NOT is an answer to "does mcpp support this". Measured on
// 2026.8.26.1:
//
//     $ mcpp build --target aarch64-linux
//       error: target 'aarch64-linux-gnu' is registered but not yet supported
//     $ mcpp build --target aarch64-linux-musl
//       Finished dev [unoptimized + debuginfo] in 0.99s
//
// The question asked was "aarch64, Linux". The question answered was
// "aarch64-linux-GNU", and the error even quotes a triple the user never typed.
// The same fill sends `riscv64-linux` to `riscv64-linux-gnu`, a row that does
// not exist at all, so a registered target family is reported as UNKNOWN.
//
// This function is the request's own completion, applied only where a request
// is read and only when the segment was not written. It consults the vocabulary
// — compile-time data, therefore the same on every host, so target identity
// still does not depend on where the build ran.
//
// ⭐ RULE ONE MAKES THIS RETIRE ITSELF. When `aarch64-linux-gnu` graduates from
// `planned`, rule one matches first and the completion goes back to the lexical
// answer with nobody editing this function.
struct RequestResolution {
    Triple triple;                            // the identity to use from here on
    // The lexical fill was replaced by a row from the vocabulary. For the
    // report: the user wrote one thing and mcpp resolved it to another.
    bool   completedFromVocabulary = false;
    std::vector<std::string_view> siblings;   // every row sharing (arch, os)
    std::vector<std::string_view> supported;  // of those, the ones not `planned`
    // Several rows are supported and the lexical fill names none of them, so
    // there is no basis to pick. No (arch, os) group has this shape today; the
    // rule is written down so the first one does not get an invented answer.
    bool   ambiguous = false;
};

inline RequestResolution resolve_request(const Triple& parsed) {
    RequestResolution r;
    r.triple = parsed;
    // A written segment is a request, not a gap: honour it, including when it
    // names a `planned` row (the tier gate is what refuses that, and its
    // subject is then genuinely what the user typed).
    if (parsed.envExplicit || parsed.arch.empty() || parsed.os.empty())
        return r;

    const std::string prefix = parsed.arch + "-" + parsed.os;
    for (auto& k : kKnownTargets) {
        // Exact (macOS rows carry no env) or `arch-os-<env>`. The separator
        // check is what keeps a prefix from spanning two different OS names.
        const bool exact = k.canonical == prefix;
        const bool sub   = k.canonical.size() > prefix.size()
                        && k.canonical.starts_with(prefix)
                        && k.canonical[prefix.size()] == '-';
        if (!exact && !sub) continue;
        r.siblings.push_back(k.canonical);
        if (k.tier != "planned") r.supported.push_back(k.canonical);
    }

    const std::string lexical = parsed.str();
    for (auto s : r.supported)
        if (s == lexical) return r;              // rule 1: the fill is supported

    if (r.supported.size() == 1) {               // rule 2: the only supported row
        auto only = r.supported.front();
        r.triple.env = only.size() > prefix.size()
                     ? std::string(only.substr(prefix.size() + 1))
                     : std::string{};
        // ⚠️ STILL NOT EXPLICIT. `envExplicit` records what the PROJECT asked
        // for and feeds the C-library-request check; mcpp choosing a row is not
        // the project naming a C library. Setting it here would make
        // `check_request` compare mcpp's own answer against itself, and would
        // print `aarch64-linux-musl` where the user wrote `aarch64-linux`.
        r.completedFromVocabulary = true;
        return r;
    }
    // rule 4 before rule 3: several supported rows and the fill names none.
    if (r.supported.size() > 1) r.ambiguous = true;
    // rule 3: nothing supported (empty group, or every row `planned`). Keep the
    // lexical identity and let the caller diagnose from `siblings`, which is
    // what lets the message name a row that actually exists.
    return r;
}

// The effective target C library for one build.
//
// SINGLE READ POINT, and it is one because it was two. `prepare_build` derived
// "which sysroot does this target use" in two places — once to compute the
// include/library paths and once to materialize the xim package — and adding a
// project-level override to only one of them would have produced a build that
// installs one C library and compiles against another. This codebase has paid
// for that shape repeatedly (#233/#240/#242/#344).
//
// `override_` is the project's `[target.<triple>].sysroot`, and its optionality
// is load-bearing:
//
//   nullptr  -> the project said nothing; the target table's column applies
//   "xim:..." -> the project named a different C library
//   ""        -> the project asked for NO C library (the zero-libc tier)
//
// Returning "" for the last case is deliberate: it is what a hosted target row
// already carries, and every consumer of this function already treats empty as
// "add no target sysroot paths". The tier therefore needs no new branch
// anywhere downstream — it reuses the answer the engine already knew how to
// handle.
// ⚠️ A POINTER AND NOT AN `std::optional<std::string>`. The tri-state is the
// same — null means "the project said nothing" — and a pointer parameter
// instantiates nothing in this module's interface. See the note on
// `TargetEntry::sysroot` for what the optional cost when it reached one.
inline std::string effective_sysroot(const Triple& t,
                                     const std::string* override_)
{
    if (override_) return *override_;
    if (auto* k = find_known_target(t)) return std::string(k->sysroot);
    return {};
}

// Closest known-target canonical name for a mistyped `--target` (checked
// against canonical names AND common alias spellings). nullopt when nothing
// is plausibly close.
std::optional<std::string> did_you_mean(std::string_view input);

// Host coordinates as a canonical Triple (linux hosts report env=gnu — the
// user-facing host default, independent of how mcpp itself was linked).
inline Triple host_triple() {
    Triple t;
    t.arch = std::string(mcpp::platform::host_arch);
    t.os   = std::string(mcpp::platform::name);
    // Derived from the machine rather than written by anyone, so it states no
    // request: a host build must not be refused for "contradicting" a C library
    // its own triple never asked for.
    if (t.os == "linux")        t.env = "gnu";
    else if (t.os == "windows") t.env = "msvc";
    return t;
}

// ── Version pins (single site; §4.6 of the design doc) ───────────────────────
// Every default/convention toolchain version literal lives here. Help and
// error strings format these — never inline a pinned version elsewhere.
// Changing a pin: update this block, then sync docs/03-toolchains.md and the
// README platform table (drawn from kKnownTargets above).
namespace pins {
    // First-run auto-install defaults (prepare.cppm), per host platform/arch.
    //
    // macOS and Windows shared ONE pin until 2026.8.2.1. They must not:
    // Apple ships no GCC, so upstream LLVM with bundled libc++ is the only
    // self-contained choice there — but on Windows clang targets the MSVC
    // ABI (host triple env=msvc) and therefore uses the MSVC STL, which only
    // arrives with Visual Studio's "Desktop development with C++" workload.
    // A bare Windows box got a default it could never build with, and no
    // diagnostic. The Windows pin is now chosen by detection, not by
    // sharing macOS's answer.
    inline constexpr std::string_view kFirstRunMac          = "llvm@20.1.7";
    // Windows WITH a usable MSVC (STL + SDK, see msvc::has_usable_msvc()):
    // unchanged behavior. The MSVC ABI is what lets a project link vcpkg /
    // third-party .lib artifacts, so it stays the answer when it can work.
    inline constexpr std::string_view kFirstRunWinMsvc      = "llvm@20.1.7";
    // Windows WITHOUT one: winlibs GCC targeting PE/GNU. Fully self-contained
    // (static libstdc++/libgcc, its own UCRT), zero Visual Studio dependency,
    // `import std` works. Must stay equal to the x86_64-windows-gnu row's
    // `pin` in kKnownTargets above — test_windows_defaults.cpp enforces it.
    inline constexpr std::string_view kFirstRunWinGnu       = "gcc@16.1.0";
    inline constexpr std::string_view kFirstRunWinGnuTarget = "x86_64-windows-gnu";
    inline constexpr std::string_view kFirstRunLinuxX86_64  = "gcc@16.1.0";
    inline constexpr std::string_view kFirstRunLinuxOther   = "gcc@15.1.0-musl";
    // Suggested install spellings used by help / MCPP_NO_AUTO_INSTALL errors.
    inline constexpr std::string_view kSuggestLlvm          = "llvm 20.1.7";
    inline constexpr std::string_view kSuggestGccMusl       = "gcc 15.1.0-musl";
    inline constexpr std::string_view kSuggestGccMingw      = "gcc 16.1.0";
} // namespace pins

// ── Artifact naming conventions ──────────────────────────────────────────────
//
// How a built artifact is NAMED is a property of the TARGET, never of the
// machine doing the build. `mcpp::platform::{exe_suffix,lib_prefix,…}` answer a
// different question — "what does THIS machine call its own binaries" — and
// using them to name build outputs is wrong the moment host != target.
//
// It is a function of (os, env), not of os alone. The trap:
//
//   x86_64-windows-gnu   → libfoo.a    (GNU/mingw convention)
//   x86_64-windows-msvc  → foo.lib     (MSVC convention)
//
// A single `_WIN32` branch cannot express that, which is why building a static
// library with mingw ON a Windows host produces `foo.lib` today — a GNU archive
// wearing an MSVC name. That is a pre-existing defect, unrelated to cross
// compilation.
//
// See .agents/docs/2026-08-03-b3-target-aware-artifact-naming.md.
struct ArtifactNaming {
    std::string_view exeSuffix;     // ""      | ".exe"
    std::string_view libPrefix;     // "lib"   | ""
    std::string_view staticLibExt;  // ".a"    | ".lib"
    std::string_view sharedLibExt;  // ".so"   | ".dylib" | ".dll"
    // PE consumers link against an import library, not the .dll itself. mcpp
    // does not model import libraries yet, so this currently marks "shared
    // libraries are not supported for this target" rather than describing a
    // produced artifact. Shared libraries have never been verified end-to-end
    // on PE or Mach-O — every shared-library e2e declares `# requires: elf`.
    bool             sharedNeedsImportLib;
};

// Naming for an explicit target triple. An EMPTY triple means "build for this
// machine", and only then is the host answer the correct one — so the caller
// passes it in rather than this module reaching for mcpp::platform, which keeps
// the decision testable from any host (and keeps this module dependency-free).
inline ArtifactNaming artifact_naming(const Triple& t, const ArtifactNaming& hostNaming) {
    if (t.empty()) return hostNaming;

    if (t.os == "windows") {
        // PE. The static-library convention splits on env, not on os.
        const bool msvc = t.is_msvc_env();
        return ArtifactNaming{
            .exeSuffix            = ".exe",
            .libPrefix            = msvc ? "" : "lib",
            .staticLibExt         = msvc ? ".lib" : ".a",
            .sharedLibExt         = ".dll",
            .sharedNeedsImportLib = true,
        };
    }
    if (t.os == "macos") {
        return ArtifactNaming{
            .exeSuffix = "", .libPrefix = "lib",
            .staticLibExt = ".a", .sharedLibExt = ".dylib",
            .sharedNeedsImportLib = false,
        };
    }
    if (t.os == "linux") {
        return ArtifactNaming{
            .exeSuffix = "", .libPrefix = "lib",
            .staticLibExt = ".a", .sharedLibExt = ".so",
            .sharedNeedsImportLib = false,
        };
    }
    // Outside the triple language: fall back to the host answer rather than
    // guessing. A wrong guess here silently misnames every artifact.
    return hostNaming;
}

} // namespace mcpp::toolchain::triple

namespace mcpp::toolchain::triple {

namespace {

bool starts_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(0, p.size()) == p;
}

std::string normalize_arch(std::string_view a) {
    if (a == "arm64") return "aarch64";   // Apple/xlings spelling → GNU
    if (a == "amd64") return "x86_64";
    return std::string(a);
}

// Levenshtein distance (small inputs only).
std::size_t edit_distance(std::string_view a, std::string_view b) {
    std::vector<std::size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            std::size_t sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, sub });
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

} // namespace

std::optional<Triple> parse(std::string_view s) {
    if (s.empty()) return std::nullopt;

    // Split on '-'.
    std::vector<std::string_view> tok;
    for (std::size_t b = 0; b <= s.size();) {
        auto d = s.find('-', b);
        if (d == std::string_view::npos) { tok.push_back(s.substr(b)); break; }
        tok.push_back(s.substr(b, d - b));
        b = d + 1;
    }
    if (tok.size() < 2 || tok[0].empty()) return std::nullopt;

    Triple t;
    t.arch = normalize_arch(tok[0]);

    // `none` is BOTH a vendor segment and an OS segment, and which one it is
    // depends on the rest of the triple, not on its position:
    //
    //   riscv64-none-elf        -> vendor absent, OS = none   (bare metal)
    //   x86_64-none-linux-gnu   -> vendor = none, OS = linux  (hosted)
    //
    // So it cannot be decided inside the single left-to-right pass below — by
    // the time `none` is seen, `linux` has not been read yet. Pre-scan for a
    // real OS token first; `none` is the OS only when there is no other
    // candidate. Getting this backwards is not a parse error, it is a SILENT
    // one: the triple would parse as hosted and the build would produce a host
    // binary while reporting success.
    bool hasRealOs = false;
    for (std::size_t i = 1; i < tok.size(); ++i) {
        std::string_view k = tok[i];
        if (k == "linux" || k == "windows" || k == "apple"
            || starts_with(k, "darwin") || starts_with(k, "macosx")
            || starts_with(k, "macos")  || starts_with(k, "mingw")) {
            hasRealOs = true;
            break;
        }
    }

    bool sawOs = false;
    for (std::size_t i = 1; i < tok.size(); ++i) {
        std::string_view k = tok[i];
        if (k.empty()) return std::nullopt;
        if (k == "none" && !hasRealOs) { t.os = "none"; sawOs = true; continue; }
        // Vendor segments carry no information — skip. ("w64" is mingw-w64's
        // vendor; "apple" implies macOS when no OS token follows.)
        if (k == "unknown" || k == "pc" || k == "w64" || k == "none") continue;
        if (k == "apple") { if (!sawOs) { t.os = "macos"; sawOs = true; } continue; }

        if (k == "linux")                       { t.os = "linux";   sawOs = true; continue; }
        if (k == "windows")                     { t.os = "windows"; sawOs = true; continue; }
        if (starts_with(k, "darwin")
            || starts_with(k, "macosx")
            || starts_with(k, "macos"))         { t.os = "macos";   sawOs = true; t.env.clear(); continue; }
        // "mingw32" is the GNU os segment for ALL MinGW targets (64-bit
        // included — historical residue); it means windows + gnu env.
        if (starts_with(k, "mingw"))            { t.os = "windows"; sawOs = true; t.env = "gnu"; t.envExplicit = true; continue; }

        // Bare-metal object-format / ABI segments. Only meaningful with
        // os=none: `riscv64-none-elf`, `arm-none-eabi`, `arm-none-eabihf`.
        // Gated on the OS so a hosted triple cannot pick them up by accident.
        if (t.os == "none") {
            if (k == "elf")                { t.env = "elf";    t.envExplicit = true; continue; }
            if (k == "eabihf")             { t.env = "eabihf"; t.envExplicit = true; continue; }
            if (k == "eabi")               { t.env = "eabi";   t.envExplicit = true; continue; }
        }

        if (t.os != "macos") {
            if (k == "musl" || starts_with(k, "musleabi")) { t.env = "musl"; t.envExplicit = true; continue; }
            if (k == "gnu"  || starts_with(k, "gnueabi"))  { t.env = "gnu";  t.envExplicit = true; continue; }
            // starts_with: clang effective triples can carry a version suffix
            // on the env segment ("…-windows-msvc19.44.35211").
            if (starts_with(k, "msvc"))                    { t.env = "msvc"; t.envExplicit = true; continue; }
        }
        // Unrecognized segment (androideabi, wasi, …): not in mcpp's target
        // language — treat as unparseable rather than guessing.
        return std::nullopt;
    }

    if (!sawOs) return std::nullopt;
    // macOS carries no env segment at all, so nothing was declined there.
    if (t.os == "macos") { t.env.clear(); t.envExplicit = false; }
    // ⚠️ THE FILL STAYS, AND THE FACT THAT IT WAS A FILL IS NOW RECORDED.
    // `x86_64-linux` is the canonical identity `x86_64-linux-gnu` — every
    // directory name and cache key downstream depends on that — but it is NOT
    // the request `x86_64-linux-gnu`, which names a C library. See
    // `Triple::envExplicit`.
    if (t.os == "linux" && t.env.empty()) t.env = "gnu";
    // ⚠️ AND THE SAME ON WINDOWS AND ON BARE METAL, WHICH WERE MISSING AND MADE
    // THE RULE A LIE ON TWO PLATFORMS OUT OF FOUR.
    //
    // `x86_64-linux` parsed and `x86_64-windows` did not — `unknown target` —
    // so "a request must be able to say nothing" held on Linux and macOS and
    // not elsewhere. The asymmetry forced every Windows cross build to spell
    // `gnu`, and under this ecosystem that word describes nothing present in
    // the build: the compiler is clang, the linker lld, the compiler runtime
    // compiler-rt, the C library musl, the C++ runtime libc++, the platform
    // openkal. It is LLVM's label for the non-MSVC ABI, inherited from MinGW,
    // and mcpp cannot rename it — but it can stop requiring it to be typed.
    //
    // ⚠️ `gnu` AND NOT THE HOST'S OWN ENV. `host_triple()` answers `msvc` on a
    // Windows machine, and filling from it would give one command a different
    // identity — a different output directory and cache key — on each host. A
    // target's identity may not depend on where it was built. `gnu` is the row
    // reachable from every host and the one this ecosystem targets; a project
    // wanting Microsoft's ABI writes `msvc`, and writing it there is meaningful
    // because it selects a different object ABI rather than a different C
    // library.
    if (t.os == "windows" && t.env.empty()) t.env = "gnu";
    // On a target with no operating system the segment names the object format,
    // and every row in the table carries `elf`. Filling it lets `riscv64-none`
    // mean what it plainly says.
    if (t.is_freestanding() && t.env.empty()) t.env = "elf";
    return t;
}

std::optional<std::string> did_you_mean(std::string_view input) {
    // Compare against canonical names and the common alias spellings a user
    // is likely to half-remember.
    static constexpr std::string_view kAliases[] = {
        "x86_64-w64-mingw32", "x86_64-unknown-linux-musl",
        "x86_64-unknown-linux-gnu", "x86_64-pc-windows-msvc",
        "x86_64-pc-windows-gnu", "aarch64-unknown-linux-musl",
    };
    std::optional<std::string> best;
    std::size_t bestDist = std::string_view::npos;
    auto consider = [&](std::string_view cand, std::string_view canonical) {
        auto d = edit_distance(input, cand);
        if (d < bestDist) { bestDist = d; best = std::string(canonical); }
    };
    for (auto& k : kKnownTargets) consider(k.canonical, k.canonical);
    for (auto& a : kAliases) {
        if (auto t = parse(a); t && is_known_target(*t)) consider(a, t->str());
    }
    // Only suggest when plausibly a typo: allow more slack for longer inputs.
    std::size_t budget = std::max<std::size_t>(2, input.size() / 4);
    if (best && bestDist <= budget) return best;
    return std::nullopt;
}

} // namespace mcpp::toolchain::triple
