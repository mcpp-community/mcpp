// mcpp.toolchain.model - stable toolchain data model.

export module mcpp.toolchain.model;

import std;
import mcpp.toolchain.triple;

export namespace mcpp::toolchain {

enum class CompilerId { Unknown, GCC, Clang, MSVC };

// WHERE A COMPILER CAME FROM. Two values, and there will only ever be two.
//
// `Managed`     an xlings payload the manifest named. The answer is in the
//               manifest; the machine only decides whether it has been
//               downloaded yet.
// `SystemMsvc`  probed on this machine. The answer depends on what happens to
//               be installed here.
//
// THE SECOND ONE IS NOT A GENERAL CAPABILITY, and reading it as one is a
// mistake this comment exists to prevent. mcpp is built on xlings, a
// user-space OS, and the whole design is to drive host dependencies to a
// minimum — there is no `gcc@system`, deliberately. `msvc@system` is a
// concession to ONE platform: Visual Studio is very often already installed
// and cannot always be redistributed, so refusing to use it would cost more
// than it buys. (The bare, family-less `system` spec is a different thing
// again: a deliberate escape hatch to the PATH compiler.)
//
// Lives in the data model rather than in msvc.cppm so the toolchain-spec
// side (registry) and the located-compiler side (msvc) name the SAME axis.
// They used to answer it independently, in 26 scattered branches, which is
// how "is this managed" and "is this a system MSVC" came to be asked with
// different predicates in the same build.
enum class Origin { Managed, SystemMsvc };

inline std::string_view origin_name(Origin o) {
    return o == Origin::SystemMsvc ? "system" : "managed";
}

// Fine-grained sysroot paths derived from xpkgs payloads.
// When populated, flags are assembled from these paths instead of --sysroot.
// One environment variable a toolchain needs at tool-invocation time.
struct EnvVar {
    std::string key;
    std::string value;
};

// The runtime this toolchain builds AGAINST, in xlings's spelling
// ("glibc@2.39"). Resolved before payload probing from the root-selected
// RuntimeBinding snapshot. Empty means no libc payload applies, and payload
// resolution then declines rather than guessing.
struct PayloadPaths {
    std::filesystem::path glibcInclude;     // glibc headers (features.h, bits/)
    std::filesystem::path glibcLib;          // glibc runtime (libc.so, crt*.o, ld-linux)
    std::filesystem::path linuxInclude;      // linux kernel headers (linux/, asm/)
};

struct Toolchain {
    CompilerId                          compiler        = CompilerId::Unknown;
    std::string                         version;            // "15.1.0"
    std::filesystem::path               binaryPath;
    std::string                         driverIdent;        // normalized --version output
    std::string                         targetTriple;       // "x86_64-linux-gnu"
    // The runtime this toolchain builds AGAINST, in xlings's own spelling
    // ("glibc@2.39"). Resolved BEFORE payload probing from the root-selected
    // RuntimeBinding snapshot.
    //
    // Empty is a refusal, not a default: payload resolution declines rather
    // than picking a libc by directory order. That guess is what let the
    // compile side and the artifact's interpreter name different glibc
    // versions, with nothing in the resulting binary looking wrong until it
    // loaded a library built against the other one.
    std::string                         runtimeBinding;
    // Hash of the complete immutable RuntimeBinding snapshot (selection,
    // provider, runtime and environment), not merely its libc label. Two
    // named SubOS environments may both say glibc@2.39 and still require
    // different loader/driver contracts, so the build cache must separate
    // them. Empty keeps compatibility for low-level detector unit tests.
    std::string                         runtimeContractHash;
    std::string                         stdlibId;           // "libstdc++"
    std::string                         stdlibVersion;
    std::filesystem::path               stdModuleSource;    // bits/std.cc / std.cppm
    std::filesystem::path               stdCompatSource;    // bits/std_compat.cc / std.compat.cppm
    // Flags the std module source needs that the compiler cannot supply itself.
    //
    // Empty for every toolchain that ships its own standard library: there the
    // module source is the compiler's, and the compiler finds its own headers.
    // Non-empty when the source comes from a PACKAGE instead --- a standard
    // library configured for a target the compiler knows nothing about --- and
    // then the include path and the configuration are the package's, so they
    // have to be carried here.
    //
    // They reach the cache key without anything further being done: the key is
    // derived from the build COMMANDS, and these are part of them.
    std::string                         stdModuleFlags;
    // ⭐⭐ THE PART OF THE ABOVE THAT SAYS WHICH MACHINE, SEPARATED FROM THE
    // PART THAT SAYS WHERE THE HEADERS ARE.
    //
    // `stdModuleFlags` is one string carrying two different facts: the target
    // and its ABI-affecting options, and the include paths the module's SOURCE
    // needs. Building the module has two steps, and only the first needs both —
    // the second compiles a BMI, which already contains everything the headers
    // contributed.
    //
    // ⚠️ Passing the whole string to the second step is not wrong, it is noisy,
    // and the noise is the kind that hides things:
    //
    //     clang++: warning: argument unused during compilation: '-nostdinc++'
    //     clang++: warning: argument unused during compilation: '-isystem …'
    //       (× 17, once per include directory)
    //
    // Seventeen warnings that are correct and mean nothing, in front of any
    // warning that would mean something. ⚠️ They were present on every platform
    // and visible on none: the non-Windows command ends in `2>&1` and mcpp
    // discards a successful command's output, so the Windows leg — which has no
    // redirection — is where they first appeared.
    std::string                         stdModuleTargetFlags;
    // A package in the graph supplies a C++ runtime built FOR THIS TARGET.
    // Read by the freestanding flag table, which otherwise forces exceptions
    // and run-time type information off for every unit — right when nothing can
    // throw, and wrong when something can.
    bool                                targetCxxRuntime = false;

    // ⭐⭐ DOES THE TARGET'S C LIBRARY COME FROM A DIRECTORY THAT EXISTED
    // BEFORE DEPENDENCY RESOLUTION? — `TargetSide::cAbi.prebuilt()`, recorded
    // here so the three producers of a compile line read one value.
    //
    // ⚠️ THIS IS NOT `targetCxxRuntime` AND THE DIFFERENCE IS THE ONE
    // 2026.8.25.1 WAS ABOUT. That field says a package supplies a C++ RUNTIME;
    // this one says where the C LIBRARY comes from. A pure C program over
    // openkal has no C++ runtime and its C library still comes from the graph,
    // and a backend that implements openkal ON TOP OF Linux takes its kernel
    // interface from the graph while its C library stays the payload's. The
    // two come apart in both directions.
    //
    // ⚠️ RECORDED, NOT DERIVED. `mcpp.targetside` answers it once, after the
    // dependency graph exists; every consumer reads this. The predicate it
    // replaced on the compile side was `!crossTargetFlag.empty()` — "is there a
    // `--target=` on the command line" — which is true for a project that names
    // its host's own target and depends on nothing. See
    // `HostFlagOptions::cAbiPrebuilt` for the measurement.
    //
    // Defaults to true (prebuilt): a build whose graph supplies nothing, and
    // every host-targeting toolchain, means exactly that.
    bool                                cAbiPrebuilt = true;

    // ⭐⭐ THE `--target=` A RETARGETABLE DRIVER HAS TO BE GIVEN, OR EMPTY.
    //
    // Non-empty only when the user asked for a cross AND the resolved compiler
    // is one binary that emits many targets (clang). For a native build, and
    // for a cross served by a driver that has exactly one target of its own
    // (`x86_64-w64-mingw32-g++`), this stays empty and nothing is added.
    //
    // ⚠️ IT CANNOT BE DERIVED FROM `targetTriple` BEING NON-EMPTY. A native
    // build has a `targetTriple` too — the probed one — so a consumer that
    // tested for non-empty would add `--target=<host>` to every compile in
    // every project. Measured: it does, and what it produces is not a
    // diagnostic about targets but `/bin/sh: 1: Syntax error: word unexpected`
    // out of the generated build file.
    //
    // So the fact is recorded where it is KNOWN — at target resolution, which
    // is the only place that has both the request and the compiler — and read
    // verbatim everywhere else.
    std::string                         crossTargetFlag;
    std::filesystem::path               sysroot;            // -print-sysroot output (or empty)
    std::optional<PayloadPaths>         payloadPaths;        // fine-grained sysroot from xpkgs
    // The TARGET's C library, for targets whose row in kKnownTargets names one
    // (today: bare metal). Resolved during prepare, where the config is
    // already open, rather than in the flag builder — computing it there would
    // put a config load on a path that runs per build and answers a question
    // prepare has already answered.
    //
    // Empty for every hosted target: they get their libc from the compiler
    // payload or from PayloadPaths, which is why nobody ever had to write one
    // down for them.
    std::filesystem::path               targetSysrootRoot;
    std::filesystem::path               targetSysrootInclude;
    // The C library's package NAME (`picolibc-riscv`), recorded beside its
    // paths so a build program can be told which C library it is without
    // reverse-engineering the store layout from the path above. Empty on the
    // zero-libc tier and on hosted targets.
    std::string                         targetSysrootPkg;
    std::filesystem::path               targetSysrootLib;
    std::vector<std::filesystem::path>   compilerRuntimeDirs; // LD_LIBRARY_PATH for private tools
    std::vector<std::filesystem::path>   linkRuntimeDirs;     // -L/-rpath dirs for produced binaries
    // Environment the toolchain's tools need when invoked (set on the ninja
    // process, inherited by compiler/linker children). Empty for GCC/Clang
    // (their LD_LIBRARY_PATH need goes through compilerRuntimeDirs); the
    // MSVC backend fills INCLUDE/LIB/PATH here (design §5.1).
    // (Own struct, not std::pair — GCC 16 modules choke on a std::pair
    // member added to this exported class: "failed to load pendings".)
    std::vector<EnvVar> envOverrides;
    bool                                hasImportStd = false;
    // Lowest -std= level this toolchain can build the std module at. 0 = the
    // provider did not say, callers fall back to the plain hasImportStd
    // question. Filled next to hasImportStd by each provider, because "which
    // std module does this compiler ship" is provider-local knowledge — a
    // central table would derive the same decision in a second place.
    // GCC/libc++ answer 20; MSVC answers 20 from cl 19.38 (VS 2022 17.8,
    // microsoft/STL#3977) and 23 below that.
    int                                 importStdMinLevel = 0;
    // The Windows SDK this toolchain compiles and links against
    // ("10.0.26100.0"), once resolved. Empty everywhere else — and empty on
    // Windows too when no SDK was found, which detection tolerates so that
    // toolchain SELECTION still works on an SDK-less box.
    //
    // Recorded rather than re-derived because two different questions read it
    // and they must not be able to disagree: the compile environment
    // (INCLUDE/LIB) and the runtime identity (`ucrt@<version>`, which enters
    // the runtime contract hash). Deriving the second from a second search is
    // how the SDK came to have no identity in the first place.
    std::string                         windowsSdkVersion;
    // Something about HOW this toolchain was resolved that the user has to be
    // told, but which is not a failure. Non-empty ⇒ the caller MUST surface it.
    //
    // Today's only producer is the Windows SDK axis: a managed toolset binds
    // the SDK that came with it, so a `WindowsSdkDir` in the environment is
    // ignored — and an override that is ignored SILENTLY is indistinguishable
    // from one that did not exist. That is the failure shape this whole round
    // kept finding: "it did not happen" and "it succeeded" producing identical
    // output.
    std::string                         resolutionNote;

    std::string label() const {
        return std::format("{} {} ({})", compiler_name(), version, targetTriple);
    }

    std::string_view compiler_name() const {
        switch (compiler) {
            case CompilerId::GCC:   return "gcc";
            case CompilerId::Clang: return "clang";
            case CompilerId::MSVC:  return "msvc";
            default:                return "unknown";
        }
    }

    // ⚠️ THE FAMILY NAME, WHICH IS NOT THE DRIVER'S NAME, AND THEY DIFFER FOR
    // EXACTLY ONE FAMILY — THE COMMON ONE.
    //
    // The driver is `clang`; the family is `llvm`. Everything a user or a
    // package WRITES uses the family: `mcpp toolchain default llvm@22.1.8`,
    // `[toolchain] default = "llvm@22.1.8"`, `requires = ["mcpp:compiler=llvm"]`.
    // Reporting or matching against `clang` would mean a requirement stated in
    // the spelling the ecosystem uses could never be satisfied.
    std::string_view compiler_family() const {
        switch (compiler) {
            case CompilerId::GCC:   return "gcc";
            case CompilerId::Clang: return "llvm";
            case CompilerId::MSVC:  return "msvc";
            default:                return "unknown";
        }
    }
};

struct DetectError { std::string message; };

bool is_gcc(const Toolchain& tc);
bool is_clang(const Toolchain& tc);
bool is_musl_target(const Toolchain& tc);
bool is_msvc_target(const Toolchain& tc);
bool is_mingw_target(const Toolchain& tc);

// ⭐⭐ THE FLAGS A WHOLE GRAPH HAS TO AGREE ON WHEN THE RUNTIME COMES FROM IT.
//
// An ordinary flag is a package's business. These two are not: they change what
// a translation unit EMITS for constructs the language guarantees work across a
// program — a `throw` and a `thread_local`. Two objects that disagree link, and
// then the disagreement is the bug.
//
// Both become necessary from one fact, `Toolchain::targetCxxRuntime`: the C++
// runtime, the unwinder and the C library are packages rather than the
// compiler's payload. The compiler's defaults for these are chosen for the
// platform's OWN runtime, which is exactly the thing that is not being used.
//
//   -fdwarf-exceptions   PE only. clang defaults to SEH there, whose personality
//                        (`__gxx_personality_seh0`) and `.pdata`/`.xdata` come
//                        from the operating system's unwinder. The graph brings
//                        libunwind, which reads `.eh_frame`.
//   -femulated-tls       PE and Mach-O. Both reach a `thread_local` through
//                        something the DYNAMIC LOADER bootstraps — `_tls_index`
//                        on PE, `_tlv_bootstrap` on Mach-O. A self-contained
//                        image has no loader to do it, so the access becomes an
//                        ordinary call into compiler-rt against a key the C
//                        library owns.
//   -fvisibility=hidden  Mach-O only. There, a symbol with DEFAULT visibility
//   -fvisibility-inlines-  and weak (linkonce_odr) linkage — which is what every
//     hidden              template instantiation and inline function is — is
//                        coalesced BY THE DYNAMIC LOADER, so the linker routes
//                        calls to it through a stub and a GOT slot the loader
//                        fills. That is how one definition wins across dylibs,
//                        and it is machinery a self-contained image has no use
//                        for.
//
// ⚠️⚠️ AND THE THIRD ONE WAS FOUND BY A PROGRAM THAT LINKED, WAS SIGNED, AND
// CRASHED ON THE REAL MACHINE — which is the whole argument for running the
// artefact rather than inspecting it. On an arm64 Mac:
//
//     stop reason = EXC_BAD_ACCESS (code=1, address=0x0)
//     frame #0: 0x0000000000000000
//
// No output, no frames: the program jumped to address zero at its first
// indirect call. The image had 1238 `__stubs` entries and 1335 `__got` slots
// for THREE undefined symbols, and the stubs' names were its own —
// `std::vector<int>::__init_with_size`, `operator new`, and a thousand more
// libc++ internals. `main`'s first statement constructs a `std::vector<int>`.
//
// The package builds libc++ with `_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS`,
// which is correct for a static build and leaves every instantiation at default
// visibility. On ELF that is inert — a static link resolves weak definitions at
// LINK time and nothing survives to run time. On Mach-O it produces the
// coalescing machinery above.
//
// ⇒ Measured after adding the two flags: `__stubs` 0x3a08 → 0x6c, `__got`
// 0x29b8 → 0x58. Nine stubs and eleven slots, which is the size a program with
// three imports should have.
//
// ⚠️ ELF IS DELIBERATELY ABSENT FROM THE SECOND, and it is not an oversight:
// there a `thread_local` is a fixed offset from the thread pointer, which the C
// library establishes itself. Adding the flag would work and cost an indirection
// on every access — but it would also make ELF the only target whose thread
// locals are laid out differently from every OTHER build of the same target.
//
// ⚠️ AND THE REASON THIS IS A FUNCTION RATHER THAN TWO `if`s: the compile
// command is assembled in two places (`hostflags.cppm` for every ordinary unit,
// `prepare.cppm` for the `std` module), and a `std.pcm` built with SEH imported
// by units built with DWARF is a defect that neither file can see. "One fact,
// two channels" has produced an identical bug three times in this ecosystem;
// here the second channel is removed instead of being told to remember.
std::vector<std::string> graph_runtime_compile_flags(const Toolchain& tc);

// Can the artifact we are building be fully statically linked (`-static`)?
//
// This is a property of the TARGET, not of the machine doing the build —
// a Windows host cross-compiling to x86_64-linux-musl still produces a fully
// static ELF. `mcpp::platform::supports_full_static` answers a DIFFERENT
// question ("can THIS machine's own binaries be static"), and using it here
// silently dropped `-static` from every Windows→Linux cross build.
//
// `hostCapability` is threaded in explicitly rather than read from
// mcpp::platform so the decision is testable on any host: passing false
// models a Windows/macOS host. It is only consulted for the host target
// (empty triple), where target *is* host. Callers pass
// mcpp::platform::supports_full_static — keeping that dependency at the call
// site leaves this module free of any platform import.
bool target_supports_full_static(std::string_view targetTriple, bool hostCapability);

struct BmiTraits {
    std::string_view bmiDir;     // "gcm.cache" | "pcm.cache" | "ifc.cache"
    std::string_view bmiExt;     // ".gcm"      | ".pcm"      | ".ifc"
    std::string_view manifestPrefix; // "gcm"   | "pcm"       | "ifc"
    bool needsExplicitModuleOutput = false;
    bool needsPrebuiltModulePath = false;
    bool scanNeedsFModules = true;
    // Module-flag spellings (leading space included; empty = not emitted).
    std::string_view compileModulesFlag;    // " -fmodules" (GCC) | ""
    std::string_view stdBmiUsePrefix;       // "" | " -fmodule-file=std=" | " /reference std="
    std::string_view stdCompatBmiUsePrefix; // "" | " -fmodule-file=std.compat=" | " /reference std.compat="
    std::string_view moduleOutputPrefix;    // "" | " -fmodule-output=" | " /ifcOutput "
    std::string_view bmiSearchPrefix;       // "" | " -fprebuilt-module-path=" | " /ifcSearchDir "
    // How this compiler is TOLD that a translation unit is a module interface.
    // Emitted UNCONDITIONALLY on every module compile — mcpp never asks
    // "does this driver recognize this extension?".
    //
    // WHY UNCONDITIONALLY. Every driver has a private, version-dependent
    // suffix->language table, and the three disagree in both directions:
    // measured 2026-08-11, Clang 22.1.8 does not recognize `.ixx` at all
    // (it hands the file to the LINKER, warns, and exits 0 with no BMI —
    // a silent no-op), while cl.exe does not recognize `.cppm`. Maintaining
    // "who knows which suffix" would be a table that expires with every
    // compiler release AND whose errors are silent.
    //
    // Saying it every time costs nothing: the flag is IDEMPOTENT on a suffix
    // the driver already knows. Measured on the same day — Clang's `.cppm`
    // BMI is byte-identical with and without `-x c++-module` (18896 bytes
    // both); GCC's `.gcm` is unchanged too (its output is not byte-
    // reproducible run to run, so the comparison is same-size plus a diff
    // offset indistinguishable from the run-to-run noise).
    //
    // NOT interchangeable between families: `-x c++-module` makes GCC exit
    // with "language c++-module not recognized", and `-x c++` makes Clang
    // emit a 174-byte stub instead of a module BMI.
    //
    // Positional on GNU, so the emitter must place it before `-c $in`.
    std::string_view moduleInterfaceLangFlag; // " -x c++" | " -x c++-module" | " /interface /TP"

    // Non-empty ⇔ the driver can emit the BMI *and stop*, producing the SAME
    // BMI an ordinary compile of that TU would have produced. Both halves
    // matter, and the second one is the trap.
    //
    // MEASURED (clang 22.1.8, src/build/prepare.cppm):
    //   -fmodule-output= … -c        7.35s   BMI  9,102,984 B  (reduced)
    //   --precompile                 1.81s   BMI 18,402,920 B  (FULL)
    //   --precompile
    //     -Xclang -emit-reduced-
    //          module-interface      1.67s   BMI  9,102,968 B  (reduced)
    //
    // `--precompile` alone is fast but emits a *full* BMI, because its output
    // is meant to be fed back in for codegen. Publishing those to importers is
    // not a drop-in substitution: BMIs grow ~16x on small modules, and on
    // mcpp's own graph clang 22.1.8 then miscompiles a downstream TU outright —
    //   error: call to implicitly-deleted default constructor of
    //          'formatter<basic_string<char>, wchar_t>'
    // on a narrow format string, from inside `std`. The same TU compiles
    // against reduced BMIs. So the reduced form is not an optimisation here,
    // it is the contract: this flag must reproduce it byte for byte.
    //
    // GCC leaves this empty even though `-fmodule-only` exists: MEASURED, it
    // does not skip the back end (~99% of a full compile). GCC's split is a
    // different mechanism — see Strategy::DetachCodegen.
    std::string_view bmiOnlyFlags;
};

BmiTraits bmi_traits(const Toolchain& tc);

} // namespace mcpp::toolchain

namespace mcpp::toolchain {

bool is_gcc(const Toolchain& tc) {
    return tc.compiler == CompilerId::GCC;
}

bool is_clang(const Toolchain& tc) {
    return tc.compiler == CompilerId::Clang;
}

// Target-shape predicates read the parsed canonical Triple (triple.cppm is
// the single triple parser), with the old substring heuristics kept only as
// a fallback for triples outside the language. This makes them spelling-
// independent: "x86_64-w64-mingw32" and canonical "x86_64-windows-gnu" give
// the same answer.
bool is_musl_target(const Toolchain& tc) {
    if (auto t = triple::parse(tc.targetTriple)) return t->is_musl();
    return tc.targetTriple.find("-musl") != std::string::npos;
}

bool is_msvc_target(const Toolchain& tc) {
    if (auto t = triple::parse(tc.targetTriple)) return t->is_msvc_env();
    return tc.targetTriple.find("msvc") != std::string::npos;
}

bool is_mingw_target(const Toolchain& tc) {
    if (auto t = triple::parse(tc.targetTriple)) return t->is_windows_gnu();
    // "x86_64-w64-mingw32" (mingw-w64) / legacy "*-pc-mingw32".
    return tc.targetTriple.find("mingw32") != std::string::npos;
}

std::vector<std::string> graph_runtime_compile_flags(const Toolchain& tc) {
    std::vector<std::string> out;
    if (!tc.targetCxxRuntime) return out;
    auto t = triple::parse(tc.targetTriple);
    if (!t) return out;
    // ⚠️⚠️ AND aarch64's RUNTIME LIBRARY CHOICE, WHICH IS A CODEGEN FACT THERE.
    //
    // On aarch64 `--rtlib=compiler-rt` is not a link-time preference: it moves
    // the target-feature set. Measured by `openkal-llvm-runtime`, whose own
    // manifest records it:
    //
    //   --target=aarch64-…-musl                      -fmv +fp-armv8 +neon +v8a
    //   --target=aarch64-…-musl --rtlib=compiler-rt  +fp-armv8 +neon +outline-atomics
    //
    // The package declared it in `std-module-flags`, which reaches the std
    // module's command and nothing else, so `std.pcm` and every consumer TU
    // disagreed. Measured on macos-14 building `aarch64-linux-musl` over the
    // graph:
    //
    //   error: precompiled file 'std.pcm' was compiled with the target feature
    //          '+outline-atomics' but the current translation unit is not
    //   error: current translation unit is compiled with the target feature
    //          '-fmv' but the precompiled file 'std.pcm' was not
    //
    // ⭐ SAME DEFECT AS `-fdwarf-exceptions`, ONE FLAG LATER — see the note in
    // hostflags.cppm, which describes that one in these words: "its objects
    // agreed with each other and nothing else did". A property of the graph
    // cannot be declared by one package for one command.
    //
    // ⚠️ x86_64 HAS NO SUCH FEATURE, so both sides listed nothing there and the
    // defect was invisible until a second architecture was built.
    if (t->arch == "aarch64") out.emplace_back("--rtlib=compiler-rt");
    if (t->is_pe()) out.emplace_back("-fdwarf-exceptions");
    if (t->is_pe() || t->os == "macos") out.emplace_back("-femulated-tls");
    // ⭐⭐ MACH-O ONLY, AND THE REASON IS THAT WEAK-DEF IS A RUN-TIME MECHANISM
    // THERE. See the note on this function for the measurement.
    if (t->os == "macos") {
        out.emplace_back("-fvisibility=hidden");
        out.emplace_back("-fvisibility-inlines-hidden");
    }
    return out;
}

bool target_supports_full_static(std::string_view targetTriple, bool hostCapability) {
    // Empty triple means "build for this machine" — target IS host, so the
    // host answer is the correct one. This is the only case where the host
    // capability legitimately decides.
    if (targetTriple.empty()) return hostCapability;

    auto t = triple::parse(targetTriple);
    // Outside the triple language we have nothing to reason from. Fall back
    // to the host answer rather than guessing from a substring — a wrong
    // `true` here would emit `-static` at a target that cannot honour it.
    if (!t) return hostCapability;

    // PE targets get their `-static` from the C++ runtime distribution
    // contract (dist::Format::Pe in flags.cppm), never from here. Returning
    // false is what keeps the two mechanisms from both emitting the flag.
    if (t->is_pe()) return false;

    // macOS cannot fully static-link: libSystem must stay dynamic.
    if (t->os == "macos") return false;

    // Linux ELF — glibc or musl, native or cross. This is the line that was
    // previously gated on the HOST being Linux.
    return t->os == "linux";
}

BmiTraits bmi_traits(const Toolchain& tc) {
    if (tc.compiler == CompilerId::MSVC) {
        // Native cl.exe builds are gated off until the .ifc pipeline lands;
        // these traits exist so nothing silently reuses the GCC defaults.
        return {
            .bmiDir = "ifc.cache",
            .bmiExt = ".ifc",
            .manifestPrefix = "ifc",
            .needsExplicitModuleOutput = true,
            .needsPrebuiltModulePath = true,
            .scanNeedsFModules = false,
            .compileModulesFlag = "",
            .stdBmiUsePrefix = " /reference std=",
            .stdCompatBmiUsePrefix = " /reference std.compat=",
            .moduleOutputPrefix = " /ifcOutput ",
            .bmiSearchPrefix = " /ifcSearchDir ",
            // Pre-existing behaviour, unchanged: cl has always been told
            // explicitly, because mcpp's interfaces are `.cppm` and cl does
            // not know that suffix. The other two families now match it.
            .moduleInterfaceLangFlag = " /interface /TP",
        };
    }
    if (is_clang(tc)) {
        return {
            .bmiDir = "pcm.cache",
            .bmiExt = ".pcm",
            .manifestPrefix = "pcm",
            .needsExplicitModuleOutput = true,
            .needsPrebuiltModulePath = true,
            .scanNeedsFModules = false,
            .compileModulesFlag = "",
            .stdBmiUsePrefix = " -fmodule-file=std=",
            .stdCompatBmiUsePrefix = " -fmodule-file=std.compat=",
            .moduleOutputPrefix = " -fmodule-output=",
            .bmiSearchPrefix = " -fprebuilt-module-path=",
            .moduleInterfaceLangFlag = " -x c++-module",
            .bmiOnlyFlags = " --precompile -Xclang -emit-reduced-module-interface",
        };
    }
    return {
        .bmiDir = "gcm.cache",
        .bmiExt = ".gcm",
        .manifestPrefix = "gcm",
        .needsExplicitModuleOutput = false,
        .needsPrebuiltModulePath = false,
        .scanNeedsFModules = true,
        // GCC: -fmodules on every TU; BMIs implicit in cwd/gcm.cache, no
        // std=/search flags needed.
        .compileModulesFlag = " -fmodules",
        .stdBmiUsePrefix = "",
        .stdCompatBmiUsePrefix = "",
        .moduleOutputPrefix = "",
        .bmiSearchPrefix = "",
        // GCC decides interface-ness from the content (`export module`), so
        // it only needs to be told the LANGUAGE. `-x c++-module` is not a
        // value GCC accepts.
        .moduleInterfaceLangFlag = " -x c++",
    };
}

} // namespace mcpp::toolchain
