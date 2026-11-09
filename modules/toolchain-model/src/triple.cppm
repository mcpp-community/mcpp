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
import mcpplibs.os.platform;
import mcpp.toolchain.platform;
import mcpp.toolchain.target_schema;
import mcpp.toolchain.target_rows;
import mcpp.toolchain.builtin_targets;

export namespace mcpp::toolchain::triple {

// WHAT A TARGET PRODUCES, AS ONE ANSWER RATHER THAN A DERIVATION AT EACH SITE.
//
// The binary format used not to be anything: it was re-derived from `os`
// wherever it was needed -- `is_pe()` asked `os == "windows"`, artifact naming
// asked again, the packer asked a third time -- and that is affordable only
// while the answer has two values. A THIRD produces an addition at every such
// site, and a site that was missed does not fail: it silently answers "ELF",
// because ELF is what every `else` branch in the tree assumes.
//
// That is why this exists before wasm needs it rather than after. `wasm32` is
// the first target in mcpp's vocabulary whose object format is neither of the
// two the tree was written around, and #597 is a target-model change for
// exactly this reason -- not because a table row is hard.
//
// IT IS NOT THE SAME QUESTION AS `is_freestanding()`, and merging them would be
// the mistake this replaces. "Which container do objects come in" and "is there
// an operating system to link against" are different axes: a bare-metal
// RISC-V image is ELF with no OS, and a wasm module has an OS-like layer
// (Emscripten's POSIX emulation) and is not ELF.
enum class ObjectFormat { Elf, MachO, Pe, Wasm };

std::string_view to_string(ObjectFormat f) {
    switch (f) {
        case ObjectFormat::Elf:   return "ELF";
        case ObjectFormat::MachO: return "Mach-O";
        case ObjectFormat::Pe:    return "PE";
        case ObjectFormat::Wasm:  return "wasm";
    }
    return "ELF";
}

struct Triple {
    std::string arch;   // "x86_64" | "aarch64" | "riscv64" | ... (GNU spelling)
    std::string os;     // "linux" | "macos" | "windows"
    std::string env;    // "gnu" | "musl" | "msvc" | "" (always empty on macos)

    // WHETHER THE ENV SEGMENT WAS WRITTEN, AS OPPOSED TO SUPPLIED BY THIS
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

    // THE SPELLING A COMPILER TAKES, WHICH IS NOT THE SPELLING mcpp USES.
    //
    // `str()` is mcpp's vocabulary: short, unambiguous, and the thing a user
    // types. LLVM's is a four-field form with a vendor, and on Apple platforms
    // the architecture has a different name and the OS carries a version.
    //
    // THIS EXISTS BECAUSE CROSS-COMPILING USED TO MEAN SOMETHING NARROWER.
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
    // Measured 2026-08-23, before this existed: a build for `aarch64-macos`
    // resolved the whole graph, took musl's aarch64 headers, and compiled with
    // NO `--target` at all — so the host's x86_64 code generation met aarch64
    // declarations. What caught it was the port's own assertion, which exists
    // for exactly this:
    //
    //     okm_float_assert.c: the C library and the compiler disagree about
    //     LDBL_DIG  ('33 == 18')
    //
    // 33 is aarch64's binary128; 18 is x87. Two machines in one command line.
    // THE EFFECTIVE TRIPLE, WHICH IS NOT THE CANONICAL ONE.
    //
    // `str()` is mcpp's vocabulary and is the identity: the output directory,
    // `cfg()`, the packed ABI tag and the fingerprint all derive from it. This
    // is the spelling a COMPILER takes, and the two are deliberately different
    // -- `aarch64-macos` against `arm64-apple-macos14.0`.
    //
    // `minPlatformVersion` IS THE PROJECT'S STATEMENT, passed in rather than
    // stored, because it is a manifest value and this function must stay pure
    // of the manifest. Two platforms fuse it into the triple and each names it
    // in its own words:
    //
    //   macOS    the deployment target        `[build] macos_deployment_target`
    //   Android  the minimum API level        `[target.<t>] min_api_level`
    //
    // Keeping it one parameter rather than two is the point: both answer "the
    // oldest OS release this artefact must run on", and a second parameter
    // would let a caller supply one platform's answer for the other's.
    std::string llvm_triple(std::string_view minPlatformVersion = {}) const {
        if (empty()) return {};
        // A ROW A PLATFORM DEFINES STATES ITS OWN: `clang-triple`, with
        // `{version}` the project's minimum system version and
        // `{version:<default>}` the row's when the project states none.
        if (auto* d = defined()) {
            if (d->clangTriple.empty()) return str();
            return expand_version(d->clangTriple, minPlatformVersion);
        }
        if (os == "macos") {
            // Apple spells the 64-bit ARM architecture `arm64`, and the OS
            // component carries the deployment target: `arm64-apple-macos14`.
            // Without a version clang picks its own default, which is a
            // decision belonging to the project rather than to the compiler.
            const std::string a = (arch == "aarch64") ? "arm64" : arch;
            std::string t = a + "-apple-macos";
            t += minPlatformVersion.empty() ? std::string("14.0")
                                            : std::string(minPlatformVersion);
            return t;
        }
        if (os == "windows") {
            if (is_msvc_env()) return arch + "-pc-windows-msvc";
            return arch + "-w64-windows-gnu";
        }
        // APPLE'S OTHER OS. Same `arm64` spelling and the same vendor segment;
        // what differs is the SDK and the deployment-target flag.
        //
        // NO VERSION IS BAKED IN, unlike the macOS branch above, and that is a
        // decision rather than an omission. macOS carries a built-in floor
        // because its static libc++ archives have one; iOS takes libc++ from
        // the located SDK, which has no such constraint, so an unstated
        // version means the SDK's own default -- which clang supplies for an
        // Apple target. Android is the platform where that is false, and it
        // is false there because bionic refuses an unversioned triple.
        //
        // THE PROJECT'S STATEMENT IS CARRIED WHEN THERE IS ONE, on the same
        // parameter the other two platforms use: `arm64-apple-ios18.0` and
        // `arm64-apple-ios18.0-simulator` are Apple's own spellings, with the
        // version on the OS segment and the `-simulator` suffix after it.
        if (os == "ios") {
            const std::string a = (arch == "aarch64") ? "arm64" : arch;
            // AND THE SIMULATOR IS A DIFFERENT EFFECTIVE TRIPLE, WHICH IS WHY
            // IT IS A DIFFERENT ROW. Apple spells it with a trailing
            // `-simulator` on the OS segment; mcpp carries it as `env = "sim"`
            // in three fields, which is Rust's `aarch64-apple-ios-sim` modulo
            // the vendor elision this whole table already does. The SDK, the
            // object and the `-mios-simulator-version-min` flag all differ from
            // the device's, so folding the two into one identity would be the
            // mistake `x86_64-windows-musl` was added to undo.
            const std::string v = std::string(minPlatformVersion);
            if (env == "sim") return a + "-apple-ios" + v + "-simulator";
            return a + "-apple-ios" + v;
        }
        // ANDROID IS LINUX, AND THE ENV SEGMENT IS WHERE IT SAYS SO -- with the
        // API level fused onto it when the project stated one.
        //
        // Measured: `clang -target aarch64-linux-android21 -print-effective-triple`
        // answers `aarch64-unknown-linux-android21`, so the level belongs on
        // the ENV segment of the effective triple and nowhere else. It selects
        // which bionic symbols are visible, which is why it is in the
        // fingerprint; and it is the project's statement rather than the
        // toolchain's, because one NDK serves a range of levels.
        //
        // ABSENT, THERE IS NO ANSWER, AND THIS COMMENT USED TO SAY OTHERWISE.
        // It read "clang's own default applies [...] so omitting the key is a
        // legal answer and not a gap", which was never verified. Measured:
        //
        //     --target=aarch64-unknown-linux-android   (no level)
        //     sys/cdefs.h:365:2: error: Unversioned target triples are not
        //       supported!
        //
        // bionic refuses an unversioned triple outright, so the level is
        // mandatory. The project still does not have to state it: mcpp reads
        // the NDK's own declared floor from `meta/platforms.json` and uses that
        // (see `min_platform_version` and `ndk_min_api_level`), which is the
        // payload answering for itself rather than a constant compiled in.
        if (os == "linux") {
            std::string e = env.empty() ? std::string("gnu") : env;
            if (env == "android" && !minPlatformVersion.empty())
                e += std::string(minPlatformVersion);
            return arch + "-unknown-linux-" + e;
        }
        // Emscripten's own effective triple. The vendor segment is `unknown`
        // and the OS segment is the platform layer rather than a kernel, which
        // is why `object_format()` reads the ARCH for this row.
        if (os == "emscripten") return arch + "-unknown-emscripten";
        // An ok image (openkal SPEC clause 12) is compiled for no operating
        // system: what it calls is `kal_*`, bound by the loader that runs it.
        if (os == "openkal") return arch + "-unknown-none-elf";
        if (os == "none")  return str();   // freestanding: already LLVM's form
        return str();
    }

    bool is_musl() const        { return env == "musl"; }
    bool is_msvc_env() const    { return env == "msvc"; }
    bool is_windows_gnu() const { return os == "windows" && env == "gnu"; }

    // THE SINGLE DERIVATION. Every question about the container objects come in
    // is answered here and nowhere else -- see `ObjectFormat` for why a third
    // value makes that a requirement rather than a preference.
    //
    // The arch test precedes the ELF fallback because a wasm target's OS
    // segment names a platform layer (`emscripten`), not a format, and the
    // fallback would otherwise claim ELF for it -- the silent wrong answer this
    // whole axis exists to remove.
    ObjectFormat object_format() const {
        if (auto* d = defined()) {
            if (d->objectFormat == "pe") return ObjectFormat::Pe;
            if (d->objectFormat == "mach-o") return ObjectFormat::MachO;
            if (d->objectFormat == "wasm") return ObjectFormat::Wasm;
            return ObjectFormat::Elf;
        }
        if (os == "windows")                  return ObjectFormat::Pe;
        if (os == "macos" || os == "ios")     return ObjectFormat::MachO;
        if (arch.starts_with("wasm"))         return ObjectFormat::Wasm;
        return ObjectFormat::Elf;
    }

    // Kept as its own name because it is what 30-odd sites already ask, and now
    // reads the single answer rather than re-deriving one.
    bool is_pe() const          { return object_format() == ObjectFormat::Pe; }
    bool is_mach_o() const      { return object_format() == ObjectFormat::MachO; }
    bool is_wasm() const        { return object_format() == ObjectFormat::Wasm; }

    // DOES THIS TARGET'S TOOLCHAIN ARRIVE WITH ITS OWN COMPLETE SYSTEM?
    //
    // mcpp assembles a target's system for most rows: a glibc-targeting build
    // gets `xim:glibc` and `xim:linux-headers` reconstructed onto the command
    // line by hand, because the payload's clang alone does not have them. For
    // `emscripten` and `android` that reconstruction is not merely unnecessary,
    // it is WRONG -- both SDKs ship a complete sysroot and resolve it
    // themselves (`em++` bakes `--sysroot=<payload>/.../cache/sysroot` into
    // every invocation; the NDK's clang derives its bionic sysroot from its own
    // install prefix).
    //
    // Measured before this predicate existed: `mcpp build --target
    // wasm32-emscripten` failed inside the std module precompile, at
    //
    //   .../sysroot/include/c++/v1/cstdint:149:
    //   .../xim-x-glibc/2.44/include/stdint.h:26
    //
    // -- the HOST's glibc headers pulled into a wasm compile. The site that
    // did it asked `is_freestanding()`, which is a correct question about the
    // rows it was written for and says nothing about this one: a wasm target
    // is not freestanding, it simply is not this host.
    //
    // A PROPERTY OF THE TOOLCHAIN, KEYED ON THE TARGET, and the two coincide
    // because the target decides the payload (see registry.cppm's
    // to_xim_package). If a row ever gains a second toolchain that does NOT
    // ship a sysroot, this has to move onto the toolchain.
    bool has_own_sysroot() const;   // after the table: the row's provider answers

    // APPLE, AS ONE QUESTION. `os == "macos"` was the whole of it while macOS
    // was the only Apple row; iOS shares the object format, the linker, the
    // `arm64` spelling and `codesign`, and differs in the SDK and the
    // deployment-target flag. A site that means "Apple" and asks "macOS" gets
    // iOS wrong in the direction that still links.
    bool is_apple() const       { return os == "macos" || os == "ios"; }
    // APPLE'S OTHER OS, AND THE SIMULATOR IS THE SAME OS. Three rows answer
    // true: the device and both simulator arches. What separates the
    // simulator is `env == "sim"`, which changes the SDK, the effective
    // triple's OS segment and the name of the deployment-target flag -- not
    // the platform.
    bool is_ios() const         { return os == "ios"; }
    bool is_ios_simulator() const { return os == "ios" && env == "sim"; }
    // Android is Linux with a different C library and a different loader path.
    // `os` stays `linux` for that reason -- it is the kernel, and every
    // Linux-shaped decision in the tree is right about it -- and the env
    // segment carries what differs.
    bool is_android() const     { return env == "android"; }

    // Bare metal: there is no OS to link against. THE predicate every
    // freestanding decision keys off, spelled once here so no consumer
    // re-derives it from `os == "none"` and drifts.
    bool is_freestanding() const { return os == "none"; }
    // AN OK IMAGE: ONE ARTIFACT PER ARCHITECTURE THAT A LOADER RUNS ON EVERY
    // SYSTEM (openkal SPEC clause 12; design 2026-10-26 part 2, §8.5).
    //
    // Not freestanding: the program is hosted -- a C library and a C++ run-time
    // may be in the image, above `kal_*` -- and the kernel interface is
    // openkal's, fixed for the target. Not any operating system either: the
    // compiler is told none (`<arch>-unknown-none-elf`), `cfg(unix)` and
    // `cfg(windows)` are both false, and the system it runs on is the
    // loader's, known at run time (`os::host()`).
    bool is_openkal() const { return os == "openkal"; }
    // THE LOADER SUPPLIES THE SYSTEM. The compiler is told no operating
    // system, and what lies beneath the program is bound when it is loaded.
    // The layer rules of such a target are the engine's (a C++ header set
    // only from the graph, a freestanding C environment when the graph has no
    // C library); everything else about it -- flags, link, naming, runner --
    // is its platform's (SPEC-013).
    bool loader_supplies_os() const { return is_openkal(); }

    // WHETHER THIS ROW'S TOOLCHAIN PIN IS A CAPABILITY RATHER THAN A
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
    // SPELLED HERE RATHER THAN AT EACH DECISION, because the first version
    // said `is_freestanding()` at two of them and `x86_64-windows-musl` — a row
    // added later — was a convention at both. Measured: declaring gcc for it
    // resolved the host's Linux musl payload and reported a missing C++
    // frontend.
    // IS THIS ROW'S PIN A CAPABILITY STATEMENT RATHER THAN A CONVENTION?
    //
    // A convention pin is mcpp's preference and a declared toolchain overrides
    // it. A capability pin cannot be overridden, because no other toolchain
    // can emit the target at all: only clang/lld cross-compile bare metal, no
    // gcc emits a PE with a musl C library, and -- added with the wasm row --
    // nothing but Emscripten emits WebAssembly. A declared `gcc@16.1.0`
    // against such a row is a request that cannot be honoured, and saying so
    // is better than resolving gcc and failing inside it.
    //
    // ANDROID BELONGS HERE FOR A REASON THE OTHER THREE DO NOT SHOW, and it
    // was left out when the row got its pin. The other three are refused
    // because the toolchain CANNOT emit the format; a stock clang emits
    // aarch64 ELF perfectly well, so nothing about the output rules it out.
    // What it cannot supply is the SYSTEM: bionic's headers, its per-API-level
    // stubs and its loader path live inside the NDK, and there is no package
    // that adds them to another compiler.
    //
    // Measured on the path a declared `llvm@22.1.8` takes without this:
    // clang resolves, and the build stops inside it with
    // `'__config' file not found`, then -- once told the target --
    // `Unversioned target triples are not supported!` from bionic's own
    // <sys/cdefs.h>. Two diagnostics, neither naming the toolchain that cannot
    // serve the row. `-D__BIONIC_CTYPE_INLINE=` and an API level fused onto
    // the triple are the NDK's own requirements, not flags a user can be
    // expected to supply to a different compiler.
    bool pin_is_capability() const;   // after the table: the row's provider answers

    // cfg() `family` dimension: unix | windows.
    //
    // iOS and Android are unix for the reason macOS and Linux are: the
    // predicate answers about the API surface a source can assume, and both are
    // POSIX. Emscripten is unix on the same test rather than on a claim about
    // wasm -- it supplies a POSIX emulation, and a source guarded by
    // `cfg(unix)` compiles against it. A target with no OS still answers
    // nothing, unchanged: `cfg(unix)` on bare metal would be false in a way no
    // source could act on.
    std::string family() const {
        if (auto* d = defined()) return std::string(d->family);
        if (os == "windows") return "windows";
        if (os == "linux" || os == "macos" || os == "ios"
            || os == "emscripten")           return "unix";
        return {};
    }

    // THE 32-BIT x86 ISA, ASKED IN ONE PLACE.
    //
    // Four spellings name it, and to clang they are not synonyms -- each sets
    // a different baseline CPU. They are one answer to the question every
    // caller here asks: the NASM format, the COFF machine of a resource, the
    // MSVC directory, the decoration of a C symbol. Six sites used to answer it
    // for themselves, with four different sets; `i386-windows-msvc` reached the
    // x64 toolset directory through two of them. `x86` is not among the
    // spellings because `parse` writes it `i686`.
    bool is_x86_32() const {
        return arch == "i386" || arch == "i486" || arch == "i586" || arch == "i686";
    }

    // MSVC's name for this architecture: the directory under a toolset's
    // `bin/Host<h>/`, the Windows SDK's `bin/<version>/`, and the redistributable
    // CRT. Empty for an architecture MSVC ships no directory for.
    std::string_view msvc_arch() const {
        if (arch == "x86_64")  return "x64";
        if (arch == "aarch64") return "arm64";
        if (is_x86_32())       return "x86";
        return {};
    }

    // NASM `-f` output format for this target. NASM is x86-family only:
    // nullopt off x86, and the caller must hard-error (suggesting cfg-gated
    // sources) rather than pick a format.
    std::optional<std::string> nasm_format() const {
        bool x64 = arch == "x86_64";
        bool x32 = is_x86_32();
        if (!x64 && !x32) return std::nullopt;
        if (os == "windows") return x64 ? "win64" : "win32";
        if (os == "macos")   return x64 ? "macho64" : "macho32";
        if (os == "linux")   return x64 ? "elf64" : "elf32";
        return std::nullopt;
    }

    // IDENTITY IS THE THREE SEGMENTS, AND `envExplicit` IS DELIBERATELY NOT
    // AMONG THEM — WHICH IS WHY THIS IS NOT `= default`.
    //
    // The flag records where the env segment came from, not what the target is.
    // A defaulted comparison would make `x86_64-linux-gnu` written by a user
    // unequal to the same triple derived by `host_triple`, and the first thing
    // that breaks is the `host` tag in `mcpp toolchain list`, which compares
    // exactly those two.
    // The row a platform defined for this triple, if one did (SPEC-016 §2):
    // its facts answer where the engine's conventions for a system know none.
    const mcpp::toolchain::target::Row* defined() const {
        return mcpp::toolchain::target::defined_row(str());
    }

    static std::string expand_version(std::string_view pattern, std::string_view version) {
        std::string out(pattern);
        const auto at = out.find("{version");
        if (at == std::string::npos) return out;
        const auto end = out.find('}', at);
        if (end == std::string::npos) return out;
        std::string fallback;
        if (out[at + 8] == ':') fallback = out.substr(at + 9, end - at - 9);
        out.replace(at, end - at + 1, version.empty() ? fallback : std::string(version));
        return out;
    }

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
//   preview   — it builds and links; no execution has been recorded for the row
//   planned   — registered intent; payload or CI row not wired yet
struct TargetInfo {
    std::string_view canonical;   // "x86_64-linux-musl"
    std::string_view tier;        // "verified" | "preview" | "planned"
    std::string_view note;        // display annotation: "static" / "PE" / ""
    // Convention toolchain pin for `--target <canonical>` with no explicit
    // [target.X] toolchain override. Empty = no convention (host default).
    std::string_view pin;
    // The TARGET's C library, resolved at compile time exactly the way `pin`
    // resolves the compiler. Empty = none applies.
    //
    // This axis exists because bare metal was the one target class without
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
    // THE PLATFORM a build of this target uses when nothing selects one
    // (SPEC-013 §7): a pinned version of a platform package, which is what
    // "built-in support" for a class of targets is. Empty = the engine's own
    // conventions for the target (every hosted row). A project selects
    // another with `[target.<selector>] platform`, a machine in config.toml,
    // one invocation with `--platform`.
    std::string_view platform = {};
    // The row this view is of, and its default provider (null: none serves it).
    const mcpp::toolchain::target::Row*      row      = nullptr;
    const mcpp::toolchain::target::Provider* provider = nullptr;
};

// THE TABLE IS DATA (design 2026-11-09; SPEC-016). The rows and the providers
// that serve them are data/builtin.toml, compiled into
// mcpp.toolchain.builtin_targets by this package's build.mcpp; what this view
// adds is each row's DEFAULT provider -- the first built-in provider serving it,
// by priority and then by order -- whose toolchain, C library, linkage and
// platform are the columns every consumer of the table reads. Which provider a
// build USES is a selection (mcpp.toolchain.target_select), which also asks
// whether each candidate is available on the host; this view does not.
//
// (note deliberately excludes "static" — the display layer derives that tag
// from defaultStatic, so listing it here would duplicate it.)
constexpr const mcpp::toolchain::target::Provider* default_provider_of(std::string_view triple) {
    const mcpp::toolchain::target::Provider* best = nullptr;
    for (auto const& p : mcpp::toolchain::builtin::kProviders)
        if (mcpp::toolchain::target::serves(p, triple) && (!best || p.priority > best->priority))
            best = &p;
    return best;
}

constexpr TargetInfo info_of(const mcpp::toolchain::target::Row& r) {
    TargetInfo t{ r.triple, r.tier, r.note, {}, {}, false };
    t.row = &r;
    t.provider = default_provider_of(r.triple);
    if (t.provider) {
        t.pin = t.provider->toolchain;
        t.sysroot = t.provider->sysroot == "toolchain" ? std::string_view{} : t.provider->sysroot;
        t.defaultStatic = t.provider->linkage == "static";
        t.platform = t.provider->platform;
    }
    return t;
}

inline constexpr auto kKnownTargets = [] {
    std::array<TargetInfo, std::size(mcpp::toolchain::builtin::kRows)> out{};
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = info_of(mcpp::toolchain::builtin::kRows[i]);
    return out;
}();

inline std::span<const TargetInfo> known_targets() { return kKnownTargets; }

// A PROVIDER WHOSE TOOLCHAIN SHIPS THE WHOLE SYSROOT (`sysroot = "toolchain"`):
// the Emscripten and Android SDKs, which resolve their sysroot themselves. A
// triple outside the table answers by its system, as before the table was data.
inline bool Triple::has_own_sysroot() const {
    const auto s = str();
    for (auto const& k : kKnownTargets)
        if (k.canonical == s) return k.provider && k.provider->sysroot == "toolchain";
    return os == "emscripten" || os == "android" || env == "android";
}

// A PIN IS A CAPABILITY WHEN THE PROVIDER PAIRS WITH ONE COMPILER FAMILY ONLY:
// nothing else emits the target (bare metal, ok images, PE with musl, wasm) or
// supplies its system (bionic). A provider that pairs with several families
// (MinGW: gcc and clang) states a convention among them.
inline bool Triple::pin_is_capability() const {
    const auto s = str();
    for (auto const& k : kKnownTargets)
        if (k.canonical == s) return k.provider && k.provider->pairs.size() == 1;
    return is_freestanding() || is_openkal() || (is_pe() && is_musl()) || is_wasm() || is_android();
}

inline const TargetInfo* find_known_target(const Triple& t) {
    auto s = t.str();
    for (auto& k : kKnownTargets)
        if (k.canonical == s) return &k;
    return nullptr;
}

inline bool is_known_target(const Triple& t) { return find_known_target(t) != nullptr; }

// ── What a request reached ───────────────────────────────────────────────────
//
// `parse` fills the env segment lexically, so the identity stays total and
// independent of the host. Which row a request is given is the selection's
// (mcpp.toolchain.target_select, SPEC-016 §4); this record is what the tier
// gates read of it: the identity taken, whether it differs from the request,
// and the rows of the request's system with those this mcpp builds.
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
// A POINTER AND NOT AN `std::optional<std::string>`. The tri-state is the
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
    t.arch = std::string(mcpp::toolchain::platform::host_arch);
    t.os   = std::string(mcpp::toolchain::platform::name);
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
// Changing a pin: update this block, then sync docs/20-toolchains.md and the
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
    // The llvm pin moves with the LLVM line (SPEC-009 §10). 23.1.3 is the
    // first point release carrying the macOS 27 SDK's arm64e.x1 linker fix
    // (mcpp#669; landed on release/23.x as ee66426) — every earlier release
    // fails to link against that SDK with a malformed-TAPI error.
    inline constexpr std::string_view kFirstRunMac          = "llvm@23.1.3";
    // Windows WITH a usable MSVC (STL + SDK, see msvc::has_usable_msvc()):
    // unchanged behavior. The MSVC ABI is what lets a project link vcpkg /
    // third-party .lib artifacts, so it stays the answer when it can work.
    inline constexpr std::string_view kFirstRunWinMsvc      = "llvm@23.1.3";
    // Windows WITHOUT one: winlibs GCC targeting PE/GNU. Fully self-contained
    // (static libstdc++/libgcc, its own UCRT), zero Visual Studio dependency,
    // `import std` works. Must stay equal to the x86_64-windows-gnu row's
    // `pin` in kKnownTargets above — test_windows_defaults.cpp enforces it.
    inline constexpr std::string_view kFirstRunWinGnu       = "gcc@16.1.0";
    inline constexpr std::string_view kFirstRunWinGnuTarget = "x86_64-windows-gnu";
    // Linux x86_64 keeps the gcc family as its host default (SPEC-009
    // §4.1 reason, recorded 2026.10 with the LLVM 23.1.3 line move): native
    // glibc ABI, so X11/OpenGL and other system libraries link directly.
    // This is the platform's answer, not a lag behind the llvm line — no
    // exit condition.
    inline constexpr std::string_view kFirstRunLinuxX86_64  = "gcc@16.1.0";
    // Native Linux ARM64 uses the managed LLVM payload and glibc ABI.
    inline constexpr std::string_view kFirstRunLinuxAarch64 = "llvm@23.1.3";
    // Other Linux hosts have no managed glibc gcc payload, so the
    // default is the fully static musl one: the only self-contained choice
    // on this axis (SPEC-009 §4.1 reason; re-evaluate if a managed glibc
    // gcc for these hosts ships).
    inline constexpr std::string_view kFirstRunLinuxOther   = "gcc@15.1.0-musl";
    // Suggested install spellings used by help / MCPP_NO_AUTO_INSTALL errors.
    inline constexpr std::string_view kSuggestLlvm          = "llvm 23.1.3";
    inline constexpr std::string_view kSuggestGccMusl       = "gcc 15.1.0-musl";
    inline constexpr std::string_view kSuggestGccMingw      = "gcc 16.1.0";

    // THE DEFAULT TOOLCHAIN OF THIS HOST, ANSWERED ONCE (the 2026-09-28
    // design, WS8): what a build with nothing configured resolves. The first
    // run installs and records it; `mcpp self env --format json` reports it
    // as `defaultToolchain`, and docs/01 and docs/20 are checked against that
    // report on each host's CI row. `msvcUsable` is the one input a constant
    // cannot know -- whether a usable MSVC (STL and SDK, from Visual Studio or
    // a managed toolset) is on this machine, which decides the Windows row.
    inline std::string_view linux_default_toolchain(std::string_view arch) {
        if (arch == "x86_64") return kFirstRunLinuxX86_64;
        if (arch == "aarch64") return kFirstRunLinuxAarch64;
        return kFirstRunLinuxOther;
    }

    inline std::string_view host_default_toolchain(bool msvcUsable) {
        if constexpr (mcpplibs::os::is_macos) {
            return kFirstRunMac;
        } else if constexpr (mcpplibs::os::is_windows) {
            return msvcUsable ? kFirstRunWinMsvc : kFirstRunWinGnu;
        } else {
            return linux_default_toolchain(mcpp::toolchain::platform::host_arch);
        }
    }
} // namespace pins

// ── Artifact naming conventions ──────────────────────────────────────────────
//
// How a built artifact is NAMED is a property of the TARGET, never of the
// machine doing the build. `mcpp::toolchain::platform::{lib_prefix,…}` answer a
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
    // A shared library's own prefix where a platform names it
    // (`[platform.link] shared = "lib{name}.okso"`); the static library keeps
    // `libPrefix`.
    std::string_view sharedLibPrefix    = {};
    bool             sharedLibPrefixSet = false;
};

// Naming for an explicit target triple. An EMPTY triple means "build for this
// machine", and only then is the host answer the correct one — so the caller
// passes it in rather than this module reaching for the host's facts, which keeps
// the decision testable from any host (and keeps this module dependency-free).
inline ArtifactNaming artifact_naming(const Triple& t, const ArtifactNaming& hostNaming) {
    if (t.empty()) return hostNaming;
    if (auto* d = t.defined()) {
        const bool pe = t.is_pe();
        return ArtifactNaming{
            .exeSuffix = d->exeSuffix, .libPrefix = "lib",
            .staticLibExt = ".a",
            .sharedLibExt = !d->sharedSuffix.empty() ? d->sharedSuffix
                          : pe ? std::string_view(".dll")
                          : t.is_mach_o() ? std::string_view(".dylib") : std::string_view(".so"),
            .sharedNeedsImportLib = pe,
        };
    }

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
    // SPEC 12: an executable image is `*.oke`, an image another loads `*.okso`.
    if (t.os == "openkal") {
        return ArtifactNaming{
            .exeSuffix = ".oke", .libPrefix = "lib",
            .staticLibExt = ".a", .sharedLibExt = ".okso",
            .sharedNeedsImportLib = false,
        };
    }
    if (t.os == "emscripten") {
        // `artifact_naming`'s one sentence, unchanged: the executable is the
        // file a runner executes, in the row's own convention. On this row
        // that file is the JavaScript launcher — what the emsdk payload's
        // `node` runs and what a browser loads — so the row's convention for
        // "the executable" is `.js`, not the ELF/host-borrowed bare name this
        // used to fall back to (a different file on Linux vs. Windows hosts).
        //
        // Read, not recalled: Emscripten's own CMake toolchain fixes
        // `CMAKE_EXECUTABLE_SUFFIX ".js"` (`Platform/Emscripten.cmake`) and
        // Rust's `wasm32-unknown-emscripten` target spec sets
        // `exe_suffix: ".js"` — both because emcc's `-o` extension selects
        // what it emits, and anything but `.js`/`.html`/`.wasm` falls back to
        // the `.js` case. A future `wasm32-wasi` row would answer `.wasm` by
        // the same sentence, the way Rust's other wasm targets do, because
        // there the module IS the executable a runner executes.
        //
        // `sharedLibExt` is empty and `sharedNeedsImportLib` is set as the
        // "shared libraries are not supported for this target" marker (see
        // the field comment above): a wasm side module needs `-sSIDE_MODULE`,
        // a link contract this engine does not render, so the fallback answer
        // (a `.so`-shaped file that does not work as one) is refused instead
        // of produced — see the plan-time refusal in prepare.cppm.
        return ArtifactNaming{
            .exeSuffix = ".js", .libPrefix = "lib",
            .staticLibExt = ".a", .sharedLibExt = "",
            .sharedNeedsImportLib = true,
        };
    }
    // Outside the triple language: fall back to the host answer rather than
    // guessing. A wrong guess here silently misnames every artifact.
    return hostNaming;
}

// ── The link form of `kind = "app"` ──────────────────────────────────────────
//
// #622 A3. An application is "the thing a user launches" on every row, but
// what that MEANS is not the same file shape everywhere: on Android there is
// no executable form of an application at all -- the platform loads a shared
// library into a Java process (`System.loadLibrary`, `android:name` in the
// manifest) -- while on ELF, PE, Mach-O and the Emscripten row it is an
// ordinary executable, exactly like `kind = "bin"`.
//
// This is a property of the ROW ALONE, the same reason `artifact_naming` sits
// here rather than in the manifest: the manifest says what the author means
// (`app`), and the engine answers how THIS row spells it. A manifest that
// tried to say this itself would need a predicate over `[targets.<name>]`,
// which is exactly the shape §2.3 of the #622 design record rejects.
enum class ApplicationForm { Executable, SharedObject };

inline ApplicationForm application_form(const Triple& t) {
    // `env == "android"` is the same test `is_android()` uses (and the same
    // one `artifact_naming` would use for an Android row, if it had one) --
    // deliberately not `os == "linux"`, which would also catch a plain
    // ELF/glibc or musl row that has an ordinary executable `main`.
    return t.env == "android" ? ApplicationForm::SharedObject
                              : ApplicationForm::Executable;
}

// THE ANDROID ABI NAME OF A ROW'S ARCHITECTURE (#630 A9), for the
// `lib/<abi>/lib<name>.so` layout every Android packaging tool uses
// (`aapt2`'s `lib/` convention, and mcpp's own multi-target staging when a
// `kind = "app"` target whose form is `SharedObject` is packed for more than
// one triple at once). Android coined its own vocabulary for the
// architectures it supports rather than reusing GNU's; the two rows mcpp
// currently produces a shared object for (`aarch64`, `x86_64`) are the only
// ones this translates, and an architecture Android has no name for keeps
// its GNU spelling — mcpp has never had reason to invent a fourth
// architecture vocabulary, and guessing one here would be exactly that.
inline std::string android_abi(const Triple& t) {
    if (t.arch == "aarch64") return "arm64-v8a";
    if (t.arch == "x86_64")  return "x86_64";
    return t.arch;
}

// THE PLATFORM NAME OF A ROW, for `[package] platforms` (#622 A7). A platform
// name is the triple's `os`, except where an `env` names a platform of its
// own: Android is `linux` with `env = "android"` in the triple, and a
// package that supports Linux does not thereby support Android. The Web row
// keeps its `os` word, `emscripten`, which is also the word the `cfg()`
// grammar uses; a WASI row would be another `os`.
//
// `kPlatformNames` is the closed vocabulary the `[package] platforms`
// validator, `mcpp doctor` and the pack coverage check share. It is derived by
// hand rather than from `kKnownTargets` so that a `planned` row does not admit
// a claim nothing can yet check; the unit test asserts every known row's
// `platform_name` is a member.
inline constexpr std::string_view kPlatformNames[] = {
    "linux", "macos", "windows", "ios", "android", "emscripten", "openkal",
};

inline std::string platform_name(const Triple& t) {
    if (t.env == "android") return "android";
    return t.os;
}

inline bool is_platform_name(std::string_view name) {
    for (auto p : kPlatformNames)
        if (p == name) return true;
    return false;
}

inline std::string platform_names_joined() {
    std::string out;
    for (auto p : kPlatformNames) {
        if (!out.empty()) out += " | ";
        out += p;
    }
    return out;
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
    // MSVC's spelling. No LLVM tool accepts it as an architecture -- measured:
    // clang and llvm-windres both answer `unknown target triple
    // 'x86-pc-windows-msvc'` -- and Windows' 32-bit baseline is i686, the
    // triple MSVC's own `x86` toolset reports.
    if (a == "x86")   return "i686";
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
        // APPLE'S SECOND OS. `starts_with` for the same reason the macOS
        // branch above uses it: an effective triple carries the deployment
        // target on this segment (`arm64-apple-ios17.0`). The simulator is a
        // different row and is deliberately not spelled here -- it has a
        // different SDK and a different object, so folding it into this one
        // would make two targets share an identity.
        if (starts_with(k, "iphoneos") || starts_with(k, "ios"))
                                                { t.os = "ios";     sawOs = true; t.env.clear(); continue; }
        // EMSCRIPTEN IS AN OS SEGMENT, NOT AN ENV. It names the platform layer
        // a wasm module is compiled against -- its POSIX emulation, its
        // filesystem shim, its `main` loop -- which is the same kind of thing
        // `linux` names and not the same kind of thing `musl` names.
        if (starts_with(k, "emscripten"))       { t.os = "emscripten"; sawOs = true; t.env.clear(); continue; }
        if (k == "openkal")                     { t.os = "openkal"; sawOs = true; t.env.clear(); continue; }

        // Bare-metal object-format / ABI segments. Only meaningful with
        // os=none: `riscv64-none-elf`, `arm-none-eabi`, `arm-none-eabihf`.
        // Gated on the OS so a hosted triple cannot pick them up by accident.
        if (t.os == "none") {
            if (k == "elf")                { t.env = "elf";    t.envExplicit = true; continue; }
            if (k == "eabihf")             { t.env = "eabihf"; t.envExplicit = true; continue; }
            if (k == "eabi")               { t.env = "eabi";   t.envExplicit = true; continue; }
        }

        if (t.os != "macos") {
            // ANDROID IS AN ENV SEGMENT ON A LINUX OS, and that placement is
            // the whole of the modelling decision. The kernel IS Linux, so
            // every Linux-shaped answer in the tree -- ELF, the `unix` family,
            // `nasm -f elf64` -- is already right; what differs is the C
            // library (bionic), the loader path and the SDK. An `os = "android"`
            // would have made all three of those wrong by default and required
            // a new answer at each site.
            //
            // `androideabi` is the 32-bit ARM spelling and resolves to the same
            // env: the EABI half is the ARM calling convention, which `armv7a`
            // already carries in the arch segment.
            //
            // AND THE API LEVEL RIDES THIS SEGMENT, SO THE MATCH HAS TO BE A
            // PREFIX. This read `k == "android"`, which cannot parse
            // `aarch64-unknown-linux-android21` -- a string mcpp PRINTS
            // itself, one line above the build it describes:
            //
            //     Target aarch64-linux-android -> aarch64-unknown-linux-android21
            //
            // A reader who pastes that back was told mcpp had never heard of
            // it. The msvc branch below already carries the identical note for
            // the identical reason ("…-windows-msvc19.44.35211"); Android has
            // the same shape and was missed. One prefix covers all four
            // spellings: `android`, `android21`, `androideabi`,
            // `androideabi21`.
            if (starts_with(k, "android")) {
                t.env = "android"; t.envExplicit = true; continue;
            }
            if (k == "musl" || starts_with(k, "musleabi")) { t.env = "musl"; t.envExplicit = true; continue; }
            if (k == "gnu"  || starts_with(k, "gnueabi"))  { t.env = "gnu";  t.envExplicit = true; continue; }
            // starts_with: clang effective triples can carry a version suffix
            // on the env segment ("…-windows-msvc19.44.35211").
            if (starts_with(k, "msvc"))                    { t.env = "msvc"; t.envExplicit = true; continue; }
        }
        // THE SIMULATOR SEGMENT, WHICH IS THE ONE PLACE AN APPLE ROW HAS AN
        // env. Both spellings arrive: `sim` is mcpp's and Rust's, `simulator`
        // is Apple's own and appears in any effective triple clang prints.
        if (t.os == "ios" && (k == "sim" || k == "simulator")) {
            t.env = "sim"; t.envExplicit = true; continue;
        }
        // musl ON macOS (openkal-platform's hosted part, whose C library is
        // openkal-musl rather than libSystem). The native identity keeps no env
        // segment; this one names the C library it has.
        if (t.os == "macos" && k == "musl") { t.env = "musl"; t.envExplicit = true; continue; }
        // A SYSTEM A PLATFORM DEFINES (mcpp#911 G1; SPEC-016 §2): the os and
        // env words of the rows a platform read in this process states.
        if (!sawOs && mcpp::toolchain::target::defines_os(k)) { t.os = std::string(k); sawOs = true; continue; }
        if (sawOs && t.env.empty() && mcpp::toolchain::target::defines_env(t.os, k)) {
            t.env = std::string(k); t.envExplicit = true; continue;
        }
        // Unrecognized segment (wasi, …): not in mcpp's target language —
        // treat as unparseable rather than guessing.
        return std::nullopt;
    }

    if (!sawOs) return std::nullopt;
    // macOS carries no env segment at all, so nothing was declined there. iOS
    // and Emscripten are the same shape: the platform layer is the whole of the
    // identity past the arch, and there is no C-library axis to decline.
    if ((t.os == "macos" && t.env != "musl") || t.os == "emscripten" || t.os == "openkal"
        || (t.os == "ios" && t.env != "sim")) {
        t.env.clear(); t.envExplicit = false;
    }
    // THE FILL STAYS, AND THE FACT THAT IT WAS A FILL IS NOW RECORDED.
    // `x86_64-linux` is the canonical identity `x86_64-linux-gnu` — every
    // directory name and cache key downstream depends on that — but it is NOT
    // the request `x86_64-linux-gnu`, which names a C library. See
    // `Triple::envExplicit`.
    if (t.os == "linux" && t.env.empty()) t.env = "gnu";
    // AND THE SAME ON WINDOWS AND ON BARE METAL, WHICH WERE MISSING AND MADE
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
    // `gnu` AND NOT THE HOST'S OWN ENV. `host_triple()` answers `msvc` on a
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

// ── target sets (SPEC-015) ──────────────────────────────────────────────────
//
// A `--target` value is either a target or a set of targets. A target is a
// triple and means what it always meant. A SET is written by leaving the
// architecture out: `openkal`, `linux-musl`, `none-eabihf`. It names every row
// of the target table whose system and environment are the ones written, and
// a command over a set acts once per member, each member being one LEG.
//
// THE SET IS RECOGNISED BY ITS FIRST SEGMENT, NOT BY A FAILED PARSE. The
// triple parser does not validate the architecture: `unknown-linux-gnu` parses
// as a triple whose architecture is `unknown`, and the four-segment GNU forms
// put a vendor first. A spelling is a set exactly when its first segment is
// one of the system words below; everything else is a triple, refused or
// accepted where triples always were.
//
// THE ENVIRONMENT IS FILLED AS A WRITTEN TRIPLE FILLS IT. `linux` is
// `linux-gnu` and `windows` is `windows-gnu`, the fills docs/21 states for
// `<arch>-linux` and `<arch>-windows`; a system without an environment segment
// (`macos`, `openkal`, `emscripten`, `ios`) has nothing to fill. Bare metal has
// no default environment, so `none` alone names no set and the refusal lists
// the environments that do.
//
// WHICH PLATFORM SERVES THE MEMBERS IS ASKED, NOT ASSUMED. Each candidate is
// put to the platform selection the build uses (the flag, the environment,
// the project, this machine's configuration, the target table), through the
// callback the caller passes. A set whose members select different platforms
// is refused, naming the split: one command acting on two platforms at once
// is not what a set was written to mean. Whether the selected platform serves
// every member is a fact of the platform package, which is read when a leg is
// planned; a member it does not serve is left out there, and the leaving out
// is reported.

export namespace mcpp::toolchain::target_set {

// The words a set begins with. `android` is the one word that is an
// environment rather than a system (`<arch>-linux-android`); it is a word here
// because a user names the platform by it.
inline constexpr std::string_view kSystemWords[] = {
    "linux", "macos", "windows", "openkal", "emscripten", "ios", "android", "none",
};

// Whether `spelling` names a set: its first segment is a system word.
bool is_set_spelling(std::string_view spelling);

// The platform a candidate selects, as one comparable string ("" = the
// engine's own conventions), or the selection's own error.
using PlatformOf =
    std::function<std::expected<std::string, std::string>(const mcpp::toolchain::triple::Triple&)>;

struct Leg {
    std::string triple;     // canonical for a set's member; as written for a target
    std::string fromSet;    // the set spelling it came from; empty for a target
};

struct Expansion {
    std::vector<std::string> members;   // canonical triples, in target-table order
    std::string              platform;  // the platform every member selected
};

// The members of one set spelling.
std::expected<Expansion, std::string> expand_set(std::string_view spelling,
                                                 const PlatformOf& platformOf);

// Every `--target` of one command: sets expanded, targets kept as written, the
// union in target-table order (targets the table does not hold keep the order
// they were written in, after the table's), a target named twice once.
std::expected<std::vector<Leg>, std::string> resolve(std::span<const std::string> requests,
                                                     const PlatformOf& platformOf);

} // namespace mcpp::toolchain::target_set

// ── implementation ──────────────────────────────────────────────────────────

namespace mcpp::toolchain::target_set {

namespace tt = mcpp::toolchain::triple;

namespace {

std::string_view first_segment(std::string_view s) {
    auto d = s.find('-');
    return d == std::string_view::npos ? s : s.substr(0, d);
}

// The table row index of a canonical triple; the table's size when absent.
std::size_t table_rank(std::string_view canonical) {
    auto rows = tt::known_targets();
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows[i].canonical == canonical) return i;
    return rows.size();
}

struct Wanted {
    std::string os;
    std::string env;
};

std::expected<Wanted, std::string> wanted_of(std::string_view spelling) {
    std::vector<std::string_view> seg;
    for (std::size_t b = 0; b <= spelling.size();) {
        auto d = spelling.find('-', b);
        seg.push_back(spelling.substr(b, d == std::string_view::npos ? std::string_view::npos : d - b));
        if (d == std::string_view::npos) break;
        b = d + 1;
    }
    const auto word = seg.front();
    if (seg.size() > 2 || (seg.size() == 2 && seg[1].empty()))
        return std::unexpected(std::format(
            "'{}' is not a target set: a set is a system and at most one environment "
            "(`openkal`, `linux-musl`, `none-eabihf`)", spelling));
    const std::string written = seg.size() == 2 ? std::string(seg[1]) : std::string{};
    if (word == "android") {
        if (!written.empty())
            return std::unexpected(std::format(
                "'{}' is not a target set: `android` is the environment of `<arch>-linux-android` "
                "and takes no further segment", spelling));
        return Wanted{"linux", "android"};
    }
    if (word == "linux" || word == "windows")
        return Wanted{std::string(word), written.empty() ? std::string("gnu") : written};
    if (word == "none") {
        if (written.empty())
            return std::unexpected(
                "`none` names no target set: bare metal has no default environment.\n"
                "  use: none-elf | none-eabi | none-eabihf");
        return Wanted{"none", written};
    }
    if (word == "ios") return Wanted{"ios", written};
    if (!written.empty())
        return std::unexpected(std::format(
            "'{}' is not a target set: `{}` has no environment segment", spelling, word));
    return Wanted{std::string(word), {}};
}

} // namespace

bool is_set_spelling(std::string_view spelling) {
    const auto w = first_segment(spelling);
    return std::ranges::find(kSystemWords, w) != std::end(kSystemWords);
}

std::expected<Expansion, std::string> expand_set(std::string_view spelling,
                                                 const PlatformOf& platformOf) {
    auto want = wanted_of(spelling);
    if (!want) return std::unexpected(want.error());

    Expansion out;
    std::vector<std::pair<std::string, std::string>> chosen;   // member, platform
    std::vector<std::string_view> planned;
    for (auto const& row : tt::known_targets()) {
        auto t = tt::parse(row.canonical);
        if (!t || t->os != want->os || t->env != want->env) continue;
        if (row.tier == "planned") { planned.push_back(row.canonical); continue; }
        auto p = platformOf ? platformOf(*t) : std::expected<std::string, std::string>{};
        if (!p) return std::unexpected(std::format("{}: {}", row.canonical, p.error()));
        chosen.emplace_back(std::string(row.canonical), *p);
    }
    if (chosen.empty()) {
        std::string rows;
        for (auto r : planned) rows += std::format("{}{}", rows.empty() ? "" : ", ", r);
        return std::unexpected(std::format(
            "the target set '{}' names no target this mcpp builds{}.\n"
            "  `mcpp toolchain list` lists the targets", spelling,
            rows.empty() ? std::string{} : std::format(" (planned, not yet built: {})", rows)));
    }
    for (auto const& [member, platform] : chosen) {
        if (platform == chosen.front().second) continue;
        std::string split;
        for (auto const& [m, p] : chosen)
            split += std::format("\n    {}  {}", m, p.empty() ? "(the engine's conventions)" : p);
        return std::unexpected(std::format(
            "the members of the target set '{}' select different platforms:{}\n"
            "  a set acts on one platform; name the targets one by one, or select one "
            "platform for all of them ([target.<selector>] platform, --platform)",
            spelling, split));
    }
    out.platform = chosen.front().second;
    for (auto& [member, platform] : chosen) out.members.push_back(std::move(member));
    return out;
}

std::expected<std::vector<Leg>, std::string> resolve(std::span<const std::string> requests,
                                                     const PlatformOf& platformOf) {
    struct Ranked { std::size_t rank; std::size_t order; Leg leg; std::string key; };
    std::vector<Ranked> all;
    std::set<std::string> seen;
    auto add = [&](Leg leg, std::string key) {
        if (!seen.insert(key).second) return;
        all.push_back({table_rank(key), all.size(), std::move(leg), std::move(key)});
    };
    for (auto const& r : requests) {
        if (is_set_spelling(r)) {
            auto e = expand_set(r, platformOf);
            if (!e) return std::unexpected(e.error());
            for (auto& m : e->members) {
                std::string key = m;
                add(Leg{std::move(m), r}, std::move(key));
            }
            continue;
        }
        // A target keeps its spelling: the leg is planned as the command
        // would have planned it alone, completion and report included.
        auto t = tt::parse(r);
        add(Leg{r, {}}, t ? t->str() : r);
    }
    std::ranges::stable_sort(all, [](Ranked const& a, Ranked const& b) {
        return a.rank != b.rank ? a.rank < b.rank : a.order < b.order;
    });
    std::vector<Leg> legs;
    for (auto& r : all) legs.push_back(std::move(r.leg));
    return legs;
}

} // namespace mcpp::toolchain::target_set
