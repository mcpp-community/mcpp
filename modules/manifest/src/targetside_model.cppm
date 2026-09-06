// mcpp.targetside — WHERE THE TARGET SIDE COMES FROM, RESOLVED ONCE.
//
// THE DEFECT THIS MODULE EXISTS TO REMOVE.
//
// A build must answer one question before it can emit a command line: where do
// the target's platform interface, C library and C++ runtime come from. Until
// this module, that question was answered in three places with three different
// criteria:
//
//   prepare  `openkalTargetSide`  the toolchain family name is "openkal-llvm"
//   flags    `graphTargetSide`    targetCxxRuntime && !crossTargetFlag.empty()
//   dist     `graphCxxRuntime`    targetCxxRuntime
//
// Measured 2026-08-23, a pure C program crossed to macOS over the openkal
// stack:
//
//   ld64.lld: error: …/xim-x-llvm/22.1.8/lib/x86_64-unknown-linux-gnu/libc++.so:
//                    unhandled file type
//
// The first criterion admitted the build; the second rejected it, because a C
// program has no C++ runtime in its graph. ⇒ The link line kept the payload's
// own libc++ and handed a Linux shared object to a Mach-O linker.
//
// The three did not disagree by accident. mcpp serves two ways of supplying
// a target side, and the moment at which each is KNOWABLE is opposite:
//
//   prebuilt  a directory (compiler payload / xpkg sysroot)  known BEFORE
//             dependency resolution
//   composed  a set of packages built from source            known AFTER it
//
// All three criteria ran at the prebuilt moment and guessed the composed
// answer. Three guesses at a fact that does not yet exist do not agree. ⇒ The
// fix is not a better guess. It is to resolve once, after the graph is known,
// and to have every consumer read that one value.
//
// WHY THIS IS A SEPARATE MODULE WITH NO DEPENDENCIES ON THE PIPELINE.
//
// `resolve` below is a pure function of plain data. It performs no I/O, reads
// no global state and knows nothing of ninja, toolchains or manifests. That is
// deliberate: the capability that preceded it (`hosted-standard-library`) drove
// seven behaviours from inside a 7000-line translation unit and had, measured,
// ZERO test coverage — there was no way to assert it short of running a whole
// build. Everything here can be asserted from a table.
export module mcpp.targetside;

import std;

export namespace mcpp::targetside {

// ── The four ways a layer can be supplied ───────────────────────────────────
//
// `Xpkg` and `Payload` are both "prebuilt", and they are still distinct: a
// payload directory is chosen by the toolchain and an xpkg by the target table
// or the manifest. A consumer that only needs "is this prebuilt" asks
// `prebuilt()`; one that needs to name the thing needs to know which.
enum class Origin { Payload, Xpkg, Graph, None };

constexpr std::string_view origin_name(Origin o) {
    switch (o) {
        case Origin::Payload: return "payload";
        case Origin::Xpkg:    return "prebuilt";
        case Origin::Graph:   return "graph";
        case Origin::None:    break;
    }
    return "none";
}
// What the env segment of a target triple names — an axis that changes with the
// OS, which is why the triple alone cannot answer "is `gnu` a C library".
//
//   linux    `gnu` / `musl`     the C LIBRARY
//   windows  `gnu` / `msvc`     the OBJECT ABI (both allow several C libraries)
//   none     `elf`              the OBJECT FORMAT
//
// The distinction is load-bearing twice over. It decides whether a mismatch
// between the segment and the resolved C library is a contradiction worth
// reporting, and — when it is NOT — it lets the report say what the segment
// does mean, so that a reader looking at `x86_64-windows-gnu` above a line
// reading `c-abi musl` is not left to work out which row `gnu` belongs to.
enum class EnvAxis { Unknown, CLibrary, ObjectAbi, ObjectFormat };

// The noun for a segment, as it appears in the report. Empty for `Unknown`,
// because a report that cannot name the axis says nothing rather than guessing.
//
// THE VALUE, NOT ONLY THE AXIS. `gnu` and `msvc` sit on the same axis and
// select opposite ABIs, so a noun fixed per axis would print "the Itanium C++
// ABI" for an MSVC build. Naming the ABI rather than the axis is deliberate:
// `Itanium` and `MSVC` appear in no row of the report, so neither can be
// mistaken for a layer the way "the C++ ABI" would be.
inline std::string_view env_axis_noun(EnvAxis a, std::string_view segment = {}) {
    switch (a) {
        case EnvAxis::CLibrary:     return "a C library";
        case EnvAxis::ObjectAbi:
            if (segment == "gnu")  return "the Itanium C++ ABI";
            if (segment == "msvc") return "the MSVC C++ ABI";
            return "the object ABI";
        case EnvAxis::ObjectFormat: return "the object format";
        case EnvAxis::Unknown:      break;
    }
    return {};
}


// ── One layer of the target side ─────────────────────────────────────────────
//
// INTERFACE AND IMPLEMENTATION ARE TWO FIELDS, AND THE DISTINCTION CARRIES
// THE POINT OF THE ECOSYSTEM.
//
// `openkal` is an interface; `openkal-macos`, `openkal-windows`,
// `openkal-opensbi` are implementations of it. Collapsing them would hide the
// fact that one source reaches four machines because four packages answer to
// one name. On a traditional stack the two are often the same object — macOS
// supplies its kernel interface and its C library as one library — and that
// sameness is itself worth showing.
struct Layer {
    Origin      origin = Origin::None;
    std::string interfaceName;   // openkal / linux / win32 / darwin / musl / glibc / libc++
    std::string impl;            // package@version, or an xpkg reference
    bool        subset = false;  // C++ layer: a freestanding subset, not the whole library

    bool absent()   const { return origin == Origin::None; }
    bool prebuilt() const { return origin == Origin::Payload || origin == Origin::Xpkg; }
    bool fromGraph() const { return origin == Origin::Graph; }
};

// ── The resolved target side ─────────────────────────────────────────────────
//
// FIVE LAYERS. A layer is a seam at which one implementation can be exchanged
// for another; a thing is a layer when three conditions hold at once — at least
// two interchangeable implementations exist, it can be replaced independently of
// its neighbours, and it stands in a definite "was configured for" relation to
// the layer beneath it.
//
//   compiler         who compiles              llvm / gcc / msvc
//   compilerRuntime  the compiler's own        compiler-rt+libunwind / libgcc
//                    runtime: builtins, the
//                    unwinder
//   kernelAbi        the platform interface    linux / windows / darwin / openkal
//   cAbi             the C library             glibc / musl / picolibc
//   cxx              the C++ library and its   libc++ / libstdc++ / MSVC STL
//                    ABI runtime
//
// `compilerRuntime` IS NOT PART OF `cxx`, AND THE DISTINCTION WAS MEASURED
// BEFORE IT WAS NAMED. The builtins (`__udivti3` and its relatives) are what a
// PURE C PROGRAM needs. Counting them as part of the C++ runtime is the same
// error as the one recorded at the head of this file: a C program crossed to
// macOS was asked "is there a C++ runtime" and answered "no", after which the
// link line kept the payload's own libc++ and handed a Linux shared object to a
// Mach-O linker. A layer that only some programs need is still a layer.
//
// `kernelAbi` HAS NO NAME ON A TRADITIONAL STACK. A C library issues system
// calls or calls the platform's own entry points directly, and nothing names the
// seam. Naming it is what lets one C library sit above four platforms, which is
// why this field reads `—` for a picolibc bare-metal build and `openkal` for an
// openkal one ON THE SAME TARGET.
//
// The correspondence to the triple is partial and that is the point:
//
//   kernelAbi  ← the triple's OS field
//   cAbi       ← the triple's ENV field, AS A REQUEST rather than as the answer
//   the rest   ← no field of the triple
struct TargetSide {
    // The triple the driver is actually given, which is NOT the one the user
    // wrote. Measured: `--target=aarch64-macos` produces a Mach-O whose
    // MinVersion load command carries no platform and version 10.4, while
    // `arm64-apple-macos14.0` carries `macos 14.0`. Right format, right
    // architecture, wrong platform metadata — so the translation is load
    // bearing and belongs in the report.
    std::string llvmTriple;


    Layer compiler;
    Layer compilerRuntime;
    Layer kernelAbi;
    Layer cAbi;
    Layer cxx;

    // What the triple asked the C library to be, empty when it did not ask.
    // Kept beside the resolved value rather than replacing it: the report
    // states the outcome, and this exists so a mismatch can be named.
    std::string requestedCAbi;
    // The same target with that segment removed — the spelling to suggest when
    // the request turns out to describe nothing. Built by the caller, which is
    // the only place that still holds mcpp's own triple.
    std::string requestFreeTarget;
    // What the env segment names on this platform. See the member of the same
    // name on `Inputs`.
    EnvAxis envAxis = EnvAxis::Unknown;

    // The single question the five former derivation sites actually asked.
    //
    // It is about the SYSTEM, not about the C++ runtime. A C program over
    // openkal has no C++ runtime and its target side still comes from the
    // graph — that case is exactly the measured defect above.
    bool system_from_graph() const {
        return kernelAbi.fromGraph() || cAbi.fromGraph();
    }

    // AND THE C LIBRARY IS A SEPARATE QUESTION, WHICH THE ONE ABOVE WAS
    // ANSWERING FOR IT AND GETTING WRONG.
    //
    // `system_from_graph` is an OR over two layers, and the link line's
    // decision about the payload's C-library flags depends on ONE of them.
    // The two coincide in the arrangement it was written for — an openkal
    // target takes both its kernel interface and its C library from the graph
    // — and come apart in one that is just as ordinary:
    //
    //     [dependencies]
    //     openkal-linux = "0.5.4"
    //
    // A backend that implements openkal ON TOP OF Linux, linked by a program
    // that still uses the payload's glibc. `kernelAbi.fromGraph()` is true,
    // `cAbi.origin` is `Payload`, and the link side replaced `f.ld` — dropping
    // the `-B` that lets the driver find startup files for a C library it was
    // still going to link:
    //
    //     error: hermetic link check failed
    //              crt1.o (bare name — the linker cannot resolve it)
    //
    // THIS SHIPPED, in 2026.8.24.1, and reached every backend in the
    // ecosystem — that shape is how a backend is tested. Their CI was pinned
    // to an older mcpp and kept passing.
    //
    // THERE IS NO PREDICATE HERE, BECAUSE `Layer::prebuilt()` ALREADY IS
    // ONE. The question the link line asks is whether the C library came from
    // a DIRECTORY that existed before resolution (a payload, an xpkg sysroot)
    // or from packages that had to be resolved first — which is the very
    // distinction the head of this file draws, and the one `prebuilt()` was
    // named for. `cAbi.prebuilt()` says it; anything spelled out case by case
    // answers three of the four origins and goes quiet about the fourth.
};

// ── Capability grammar: mcpp:<layer>[=<interface>] ───────────────────────────
//
// mcpp HARDCODES LAYER NAMES AND NEVER HARDCODES IMPLEMENTATIONS.
//
// The five layer names below are a closed set compiled into the engine. The
// implementations that fill them — openkal, musl, picolibc, and whatever comes
// next — appear nowhere in this file or any other. That line is what separates
// this design from the string comparison it replaces (`fam == "openkal-llvm"`
// in prepare), which put a product name inside the engine.
//
// Layer names may be hardcoded because the layers are fixed by the C and C++
// build model and do not grow. Implementations may not, because growing is
// precisely what they do: the ecosystem's combinations are 2×N×M while its
// packages are 2+N+M.
enum class CapLayer { Compiler, CompilerRuntime, KernelAbi, CAbi, CxxAbi };

constexpr std::string_view cap_layer_name(CapLayer l) {
    switch (l) {
        case CapLayer::Compiler:        return "compiler";
        case CapLayer::CompilerRuntime: return "compiler-runtime";
        case CapLayer::KernelAbi:       return "kernel-abi";
        case CapLayer::CAbi:            return "c-abi";
        case CapLayer::CxxAbi:          return "c++-abi";
    }
    return {};
}

// The layers a PACKAGE may supply. `compiler` is not among them: a compiler is
// a payload this engine installs and drives, and the differences between
// families — flag spellings, the module model, the BMI format, the driver
// config file — are things the engine must know rather than data a package can
// describe. It remains a layer, and it remains one a package may REQUIRE.
constexpr bool layer_is_suppliable_by_package(CapLayer l) {
    return l != CapLayer::Compiler;
}

struct CapDecl {
    CapLayer    layer;
    std::string interfaceName;   // the `=<interface>` part; empty when omitted
};

// Parse one entry of a package's `provides` array.
//
// Returns:
//   * an error          — the name is in mcpp's reserved namespace and is not a
//                         layer this engine knows;
//   * an empty optional — the name is not in mcpp's namespace at all, and
//                         belongs to the feature system (`freestanding-allocator`);
//   * a declaration     — a layer this engine acts on.
//
// THE MIDDLE CASE IS WHY THE PREFIX EXISTS. `provides` serves two
// populations: layer names the engine consumes, and capabilities packages match
// among themselves. Making the whole array a closed set would reject the
// second; leaving it entirely open means a typo in the first silently disables
// behaviour and the build still reports success. The reserved prefix keeps a
// closed set where one is needed and an open one everywhere else.
inline std::expected<std::optional<CapDecl>, std::string>
parse_capability(std::string_view entry) {
    constexpr std::string_view kPrefix = "mcpp:";
    if (!entry.starts_with(kPrefix)) return std::optional<CapDecl>{};

    auto body = entry.substr(kPrefix.size());
    std::string_view layer = body, iface;
    if (auto eq = body.find('='); eq != std::string_view::npos) {
        layer = body.substr(0, eq);
        iface = body.substr(eq + 1);
    }

    CapDecl d{};
    if      (layer == "compiler")         d.layer = CapLayer::Compiler;
    else if (layer == "compiler-runtime") d.layer = CapLayer::CompilerRuntime;
    else if (layer == "kernel-abi")       d.layer = CapLayer::KernelAbi;
    else if (layer == "c-abi")            d.layer = CapLayer::CAbi;
    else if (layer == "c++-abi")          d.layer = CapLayer::CxxAbi;
    else
        return std::unexpected(std::format(
            "`{}` names no capability mcpp knows.\n"
            "       The `mcpp:` prefix is reserved for the target-side layers "
            "this engine resolves, and there are five:\n"
            "         mcpp:compiler[=<name>]          who compiles\n"
            "         mcpp:compiler-runtime[=<name>]  the compiler's own runtime "
            "(builtins, unwinder)\n"
            "         mcpp:kernel-abi[=<name>]        the platform interface a C "
            "library sits on\n"
            "         mcpp:c-abi[=<name>]             the C library\n"
            "         mcpp:c++-abi[=<name>]           the C++ runtime\n"
            "       A capability of your own needs no prefix; those are passed "
            "through untouched.", entry));

    if (!iface.empty()) d.interfaceName = std::string(iface);
    return std::optional<CapDecl>{d};
}

// ── Requirements: `requires = ["mcpp:<layer>=<implementation>"]` ─────────────
//
// THE SYMMETRIC HALF OF `provides`, AND THE ONLY WAY THE LAYERING RULE CAN BE
// ENFORCED WITHOUT PUTTING A PRODUCT NAME IN THE ENGINE.
//
// A C++ runtime built from libc++'s sources is compiled, and its module, by
// clang; gcc cannot consume it. That fact belongs to the package, not to mcpp —
// writing `if (stdlib == "libc++" && compiler == gcc)` here would hardcode two
// implementation names, which rule four forbids. The package states it:
//
//     requires = ["mcpp:compiler=llvm"]
//
// and this engine checks a relation it can state generically: the layer named
// must resolve to the interface named.
//
// It is also how `compiler-runtime` stays honest. libgcc is configured for
// gcc and compiler-rt for clang; a build whose compiler is one and whose
// runtime is the other resolves `__udivti3` differently from every other link in
// the same program. mcpp does not know which runtime belongs to which family —
// the runtime package says so.
struct Requirement {
    std::string requiredBy;      // package id that stated it, for the diagnostic
    CapLayer    layer;
    std::string interfaceName;   // what that layer must resolve to
};

// ── Resolver input ───────────────────────────────────────────────────────────
//
// Plain data, assembled by the caller after dependency resolution. Keeping the
// pipeline out of this module is what makes the table in the unit tests a
// complete specification of the behaviour.
struct Provider {
    std::string name;
    std::string version;
    std::string interfaceName;   // from `mcpp:<layer>=<interface>`; may be empty
    // Whether this package declares `[package] std-module`, which is what
    // distinguishes a whole standard library from a freestanding subset. The
    // distinction needs no second capability name: the capability says a layer
    // has a supplier, and this key says how far the supply goes.
    bool        hasStdModule = false;

    std::string id() const {
        return version.empty() ? name : std::format("{}@{}", name, version);
    }
    std::string display_interface() const {
        return interfaceName.empty() ? name : interfaceName;
    }
};

struct Inputs {
    std::string llvmTriple;
    std::string targetOs;              // mcpp's own OS field, for the payload interface name
    std::string targetEnv;             // mcpp's own ENV field ("musl", "gnu", …)
    bool        freestandingTarget = false;

    // The compiler, which is always a payload and never a package (see
    // `layer_is_suppliable_by_package`). Present here so that the layering rule
    // has something to check requirements against, and so that the report can
    // show the whole stack rather than the part of it packages happen to fill.
    std::string compilerFamily;        // "llvm" / "gcc" / "msvc"
    std::string compilerVersion;

    // THE C LIBRARY THE TRIPLE ASKED FOR, WHICH IS NOT THE SAME QUESTION AS
    // WHICH ONE RESOLVED.
    //
    // `x86_64-linux-musl` states a request; `x86_64-linux` declines to. The
    // parser fills the second one in as `gnu` so that the identity stays
    // canonical, so `targetEnv` alone cannot tell the two apart — see
    // `Triple::envExplicit`. Empty here means the project said nothing, and a
    // build that says nothing cannot be contradicted.
    std::string requestedCAbi;
    // The same target spelled without that segment, for the suggestion.
    std::string requestFreeTarget;
    // WHAT THE ENV SEGMENT NAMES ON THIS PLATFORM. It is a different axis
    // per OS, and a boolean here was a lossy encoding of that.
    //
    // On Linux the segment names the C library — `gnu` is glibc, `musl` is musl
    // — which is the case the request check was written for. On Windows it
    // names the OBJECT ABI: `gnu` is PE with the GNU ABI and `msvc` is PE with
    // Microsoft's, and both are compatible with more than one C library. On a
    // target with no operating system it names the object FORMAT.
    //
    // Reporting a Windows build as "asking for the `gnu` C ABI" describes an
    // axis the name never addressed, and the correction it suggested named a
    // target that does not exist. Knowing which axis it IS lets the report say
    // so instead of merely staying silent.
    EnvAxis envAxis = EnvAxis::Unknown;

    std::optional<Provider> compilerRuntime;
    std::optional<Provider> kernelAbi;
    std::optional<Provider> cAbi;
    std::optional<Provider> cxxAbi;

    // The C library the prebuilt systems would supply, already resolved by the
    // caller from `[target.X].sysroot` over the target table's column.
    bool        sysrootDeclaredEmpty = false;   // `sysroot = ""` — the zero-libc tier
    std::string sysrootXpkg;                    // an xpkg reference, or empty

    // What the payload would contribute, for display only.
    std::string payloadSystemRef;
    std::string payloadLibcRef;
    std::string payloadCxxRef;
    std::string payloadCxxInterface;            // "libc++" / "libstdc++" / "MSVC STL"
};

// The name of the C library a compiler payload carries for a target.
//
// THE TRIPLE'S ENV FIELD ANSWERS THIS ONLY WHERE THE TRIPLE HAS ONE, AND
// FALLING BACK TO `glibc` NAMED A LIBRARY THAT DOES NOT EXIST ON THE PLATFORM.
// Measured on macOS, where the canonical triple carries no env segment:
//
//   c-abi             glibc          (payload)
//
// The value was invisible while the report printed only three layers on a
// build that had something to say; showing the whole stack made a wrong label
// into a wrong statement.
//
// These names are PAYLOAD facts, which is why they may be written here at
// all: mcpp ships those payloads and knows what is inside them. What must never
// be written here is what a PACKAGE supplies — that is the difference the
// reserved-capability grammar exists to keep.
// THE TRIPLE'S ENV SEGMENT IS A TRIPLE SPELLING, NOT A C LIBRARY'S NAME, AND
// THE TWO COINCIDE ONLY SOMETIMES. `musl` is both. `gnu` is neither: on Linux it
// means glibc, and on Windows it names the MinGW flavour of the toolchain, whose
// C runtime is the same UCRT the MSVC flavour links.
//
// Returning the segment verbatim reported `c-abi gnu (payload)` for an ordinary
// Linux build while docs/14 has always named the implementations `glibc`,
// `musl`, `picolibc` — and e2e 296's own header describes the report it expects
// as `c-abi glibc (payload)`. That was a cosmetic disagreement for as long as
// the value was only printed. It stopped being cosmetic when
// `[target.'cfg(c-abi = "glibc")'.build]` became a predicate a user writes: the
// documented spelling would have matched nothing, silently, which is the exact
// defect class this release exists to remove.
//
// The REQUEST keeps the segment's spelling — `requestedCAbi` is the triple's
// env verbatim by definition (docs/specs/target-side.md §3.4) — so the two are
// compared through `c_abi_request_satisfied` rather than by equality.
inline std::string payload_libc_name(std::string_view os, std::string_view env) {
    if (env == "gnu") return os == "windows" ? "ucrt" : "glibc";
    if (!env.empty()) return std::string(env);
    if (os == "macos")   return "libSystem";
    if (os == "windows") return "ucrt";
    return "glibc";
}

// Does the resolved C library answer what the triple's env segment asked for?
// Equality plus the one alias the segment carries, so that renaming the ANSWER
// above does not turn every `-gnu` build into a reported request mismatch.
inline bool c_abi_request_satisfied(std::string_view requested,
                                    std::string_view resolved) {
    if (requested == resolved) return true;
    if (requested == "gnu")   return resolved == "glibc" || resolved == "ucrt";
    if (requested == "glibc") return resolved == "gnu";
    return false;
}

// An xpkg reference is `<namespace>:<name>[@<version>]`; the interface a reader
// wants to see is the name, not the whole address.
inline std::string xpkg_interface(std::string_view ref) {
    auto colon = ref.find(':');
    auto body  = colon == std::string_view::npos ? ref : ref.substr(colon + 1);
    auto at    = body.find('@');
    return std::string(at == std::string_view::npos ? body : body.substr(0, at));
}

// ── The resolution ───────────────────────────────────────────────────────────
inline TargetSide resolve(const Inputs& in) {
    TargetSide ts;
    ts.llvmTriple    = in.llvmTriple;
    ts.requestedCAbi     = in.requestedCAbi;
    ts.requestFreeTarget = in.requestFreeTarget;
    ts.envAxis           = in.envAxis;

    // compiler — always a payload, never a package.
    if (!in.compilerFamily.empty())
        ts.compiler = { Origin::Payload, in.compilerFamily,
                        in.compilerVersion, false };

    // compiler-runtime — the builtins and the unwinder.
    //
    // ABSENT FROM THE GRAPH DOES NOT MEAN ABSENT. Every compiler payload
    // ships one; a package supplies it only when the payload's own is the wrong
    // one for this target, which is the same shape as every other layer here.
    // The payload's is reported under the compiler's own name because that is
    // what it is — a family's runtime, not a separately chosen implementation.
    if (in.compilerRuntime)
        ts.compilerRuntime = { Origin::Graph,
                               in.compilerRuntime->display_interface(),
                               in.compilerRuntime->id(), false };
    else if (!in.compilerFamily.empty())
        ts.compilerRuntime = { Origin::Payload, in.compilerFamily, {}, false };

    // kernel-abi ← the triple's OS field.
    if (in.kernelAbi)
        ts.kernelAbi = { Origin::Graph, in.kernelAbi->display_interface(),
                         in.kernelAbi->id(), false };
    else if (in.freestandingTarget)
        // Not a gap. A bare machine has no kernel, and saying so is the
        // information: the same target reads `openkal` when an implementation
        // of a kernel interface is in the graph, which is why one source can
        // reach it at all.
        ts.kernelAbi = { Origin::None, {}, {}, false };
    else
        ts.kernelAbi = { Origin::Payload, in.targetOs, in.payloadSystemRef, false };

    // c-abi ← the triple's ENV field.
    if (in.cAbi)
        ts.cAbi = { Origin::Graph, in.cAbi->display_interface(), in.cAbi->id(), false };
    else if (in.sysrootDeclaredEmpty)
        ts.cAbi = { Origin::None, {}, {}, false };
    else if (!in.sysrootXpkg.empty())
        ts.cAbi = { Origin::Xpkg, xpkg_interface(in.sysrootXpkg), in.sysrootXpkg, false };
    else if (in.freestandingTarget)
        ts.cAbi = { Origin::None, {}, {}, false };
    else
        ts.cAbi = { Origin::Payload, payload_libc_name(in.targetOs, in.targetEnv),
                    in.payloadLibcRef, false };

    // c++ — no field of the triple, because it sits above the ABI.
    if (in.cxxAbi)
        ts.cxx = { Origin::Graph, in.cxxAbi->display_interface(), in.cxxAbi->id(),
                   !in.cxxAbi->hasStdModule };
    else if (ts.cAbi.origin == Origin::Payload)
        // THE LAYERING RULE, AS STRUCTURE RATHER THAN AS A LATER CHECK.
        //
        // An implementation must have been configured for the layer beneath it.
        // The payload's libc++ was configured against the payload's C library —
        // its `__config_site` records that configuration — so it is eligible
        // only when the C library is also the payload's. Writing the rule here
        // means the default path CANNOT construct the combination that produced
        // the measured `unhandled file type`; a diagnostic is then needed only
        // where an author overrides the contract explicitly.
        ts.cxx = { Origin::Payload, in.payloadCxxInterface, in.payloadCxxRef, false };
    else
        ts.cxx = { Origin::None, {}, {}, false };

    return ts;
}

// The layer a capability name refers to, so a check can be written once for all
// five rather than once per layer.
inline const Layer& layer_of(const TargetSide& ts, CapLayer l) {
    switch (l) {
        case CapLayer::Compiler:        return ts.compiler;
        case CapLayer::CompilerRuntime: return ts.compilerRuntime;
        case CapLayer::KernelAbi:       return ts.kernelAbi;
        case CapLayer::CAbi:            return ts.cAbi;
        case CapLayer::CxxAbi:          return ts.cxx;
    }
    return ts.cxx;
}

// ── Rule two, part one: what the resolver's structure cannot guarantee ───────
//
// The default path cannot construct the payload-C++-over-foreign-C-library
// combination, because `resolve` only reaches the payload's C++ runtime when the
// C library is also the payload's. An explicit `[target.X]` override can, so the
// rule is stated again here for that path.
inline std::optional<std::string> check_layering(const TargetSide& ts) {
    if (ts.cxx.origin == Origin::Payload && ts.cAbi.origin != Origin::Payload
        && ts.cAbi.origin != Origin::None)
        return std::format(
            "the toolchain payload's C++ runtime cannot be used with a C "
            "library that does not come from the payload.\n"
            "         c-abi  {} ({}, {})\n"
            "         c++    {} ({}, payload)\n"
            "       The payload's C++ runtime was configured against the "
            "payload's C library, and its configuration is recorded in the "
            "headers it ships. It was never configured for this one.\n"
            "       Supply a C++ runtime from the dependency graph, or take "
            "both from the payload.",
            ts.cAbi.interfaceName, ts.cAbi.impl, origin_name(ts.cAbi.origin),
            ts.cxx.interfaceName, ts.cxx.impl);
    return std::nullopt;
}

// ── Rule two, part two: declared requirements ────────────────────────────────
//
// A REQUIREMENT IS CHECKED AGAINST THE RESOLVED LAYER, NOT AGAINST THE
// REQUEST. `requires = ["mcpp:compiler=llvm"]` is satisfied by whatever the
// compiler layer actually resolved to, which is the only value that will be on
// the command line.
//
// Nothing here knows what `llvm` or `compiler-rt` mean. The comparison is
// between two strings a package chose and a supplier declared, and a mismatch is
// reported by naming both — which is what a reader needs and what an engine
// hardcoding a table of families could not produce for a family it had not
// heard of.
// `compilerStatedBy` NAMES WHERE THE COMPILER CAME FROM, AND THE ADVICE IS
// WRONG WITHOUT IT.
//
// Until 2026.8.26.2 a compiler requirement mcpp could satisfy by itself still
// refused, and the first remedy it offered was `mcpp toolchain default <fam>` —
// a GLOBAL change, made because ONE project's dependency asked. Now the graph's
// requirement is applied wherever mcpp's own answer was revisable, so the only
// way to reach this branch is a compiler the project stated itself. In that
// situation the global default is not what is being used and changing it fixes
// nothing: the advice has to point at the statement that actually decided.
//
// Empty = the caller does not know (unit tests, and any future caller); the
// generic wording then applies.
inline std::optional<std::string>
check_requirements(const TargetSide& ts, std::span<const Requirement> reqs,
                   std::string_view compilerStatedBy = {}) {
    constexpr std::string_view kPad = "         ";
    for (auto const& r : reqs) {
        // An entry with no `=<implementation>` asks only that the layer be
        // supplied by someone, which the absence check below still answers.
        auto const& have = layer_of(ts, r.layer);
        if (!r.interfaceName.empty() && have.interfaceName == r.interfaceName)
            continue;
        if (r.interfaceName.empty() && !have.absent()) continue;

        auto name = cap_layer_name(r.layer);
        // What to do about it depends on which layer disagreed, and there are
        // only two answers: the compiler is chosen by the toolchain axis, and
        // every other layer by the dependency graph.
        std::string advice =
            r.layer == CapLayer::Compiler
                ? (compilerStatedBy.empty()
                    ? std::format(
                          "       Select that compiler for this project:\n"
                          "           [toolchain]\n"
                          "           default = \"{}\"\n"
                          "       or, for one target only:\n"
                          "           [target.<triple>]\n"
                          "           toolchain = \"{}\"",
                          r.interfaceName, r.interfaceName)
                    : std::format(
                          "       This build's compiler is stated in {}, and a "
                          "compiler the project states\n"
                          "       outranks one its dependencies ask for.\n"
                          "       Change it to `{}`, or remove it — with nothing "
                          "stated, mcpp takes the\n"
                          "       compiler the graph requires and changes no "
                          "configuration to do it.",
                          compilerStatedBy, r.interfaceName))
                : std::format(
                      "       Depend on a package that declares `provides = "
                      "[\"mcpp:{}={}\"]`,\n"
                      "       or remove the package that requires it.",
                      name, r.interfaceName);

        if (have.absent())
            return std::format(
                "`{}` requires the {} to be `{}`, and nothing supplies that "
                "layer.\n"
                "{}{:<17} {}\n{}",
                r.requiredBy, name,
                r.interfaceName.empty() ? "supplied" : r.interfaceName,
                kPad, name, "—", advice);

        std::string resolved = have.impl.empty()
            ? std::format("{:<14} ({})", have.interfaceName,
                          origin_name(have.origin))
            : std::format("{:<14} ({}, {})", have.interfaceName, have.impl,
                          origin_name(have.origin));
        return std::format(
            "`{}` requires the {} to be `{}`.\n"
            "{}{:<17} {}\n"
            "{}{:<17} {:<14} (required by {})\n"
            "       An implementation is configured for the layer beneath it, "
            "and this one\n"
            "       was never configured for the one that resolved.\n{}",
            r.requiredBy, name, r.interfaceName,
            kPad, name, resolved,
            kPad, "required", r.interfaceName, r.requiredBy,
            advice);
    }
    return std::nullopt;
}

// ── The triple is a request; the target side is the fact ────────────────────
//
// REPORTED RATHER THAN REFUSED, AND THE SEVERITY WAS DECIDED BY A
// MEASUREMENT RATHER THAN BY THE PRINCIPLE.
//
// The first version refused. It is the semantically clean answer — the name
// says one C library, the artifact contains another, and only one of the two
// can be true. It also broke every project and every CI configuration that
// spells the host target `x86_64-linux-gnu`, which is what `mcpp toolchain
// list` prints and therefore what people write. mcpp's own openkal matrix was
// the first casualty.
//
// What decides the severity is that the request changes NOTHING. The graph
// supplies the C library either way; the segment is ignored, not violated. A
// build that would be identical without the segment is not a build to refuse —
// it is a build whose name misdescribes it, and saying so is the whole
// remedy.
//
// The remedy has to be actionable, which is why `x86_64-linux` had to work
// first. Telling someone their target name is wrong is only useful once there
// is a right one to give them.
inline std::optional<std::string> check_request(const TargetSide& ts) {
    // TWO AXES REACH HERE, AND EXEMPTING THE SECOND WAS THE DEFECT.
    //
    // `CLibrary` is the obvious one: on Linux the segment names the C library
    // outright. `ObjectAbi` was exempted on the grounds that `gnu` on Windows
    // names the Itanium C++ ABI rather than a C library — true, and incomplete.
    // The segment there bundles TWO things: the object ABI, which is honoured,
    // and MinGW's C runtime, which a graph-supplied C library replaces. The
    // second half is a name/fact disagreement of exactly the shape this
    // function exists to report, and it went unreported.
    //
    // Measured, one project, two spellings, same graph:
    //
    //     --target x86_64-linux-gnu     c-abi musl (graph)   warned
    //     --target x86_64-windows-gnu   c-abi musl (graph)   silent   ← the defect
    //
    // `ObjectFormat` STAYS EXEMPT, and not for symmetry. `elf` never names a
    // C library on any platform, so "the target name asks for the `elf` C ABI"
    // would be nonsense rather than merely noisy. That axis is glossed in the
    // report instead.
    if (ts.envAxis != EnvAxis::CLibrary && ts.envAxis != EnvAxis::ObjectAbi)
        return std::nullopt;
    if (ts.requestedCAbi.empty()) return std::nullopt;
    if (ts.cAbi.absent()) return std::nullopt;
    // Through the alias helper, not by equality: the request keeps the triple's
    // `gnu` spelling and the answer now names the library (`glibc`/`ucrt`), so
    // plain equality would report a mismatch on every ordinary `-gnu` build.
    if (c_abi_request_satisfied(ts.requestedCAbi, ts.cAbi.interfaceName))
        return std::nullopt;
    // A prebuilt or payload C library IS what the request selected — the
    // request is how it was selected. Only a supplier chosen by something else
    // can disagree with it.
    if (!ts.cAbi.fromGraph()) return std::nullopt;

    // On the object-ABI axis the name is half honoured, and saying only that it
    // is "inaccurate" would suggest the ABI changed too. It did not.
    std::string aside;
    if (ts.envAxis == EnvAxis::ObjectAbi)
        aside = std::format(
            "\n       The object ABI `{}` selects is unaffected; what it does "
            "not select here is the\n       C library.",
            ts.requestedCAbi);

    return std::format(
        "the target name asks for the `{}` C ABI and the dependency graph "
        "supplies `{}`.\n"
        "       The graph decides, so the build below uses `{}` — the name is "
        "what is inaccurate,\n"
        "       not the artifact.{}\n"
        "       Drop the segment to say what is actually meant:\n"
        "           --target {}",
        ts.requestedCAbi, ts.cAbi.interfaceName, ts.cAbi.interfaceName, aside,
        ts.requestFreeTarget.empty() ? std::string("<arch>-<os>")
                                     : ts.requestFreeTarget);
}

// ── Rule one: one supplier per layer ─────────────────────────────────────────
//
// A C library, a kernel interface and a C++ runtime are MUTUALLY EXCLUSIVE
// CHOICES, not additive contributions. Two suppliers is an error, and it must be
// an error rather than a silent pick: the failure mode of choosing wrong is not
// a link error but a program that runs and occasionally does not.
struct Conflict {
    CapLayer    layer;
    std::string first;        // package id
    std::string firstVia;     // empty when a direct dependency
    std::string second;
    std::string secondVia;
};

inline std::string format_conflict(const Conflict& c) {
    auto via = [](std::string_view v) {
        return v.empty() ? std::string("a direct dependency")
                         : std::format("via {}", v);
    };
    return std::format(
        "two packages supply the {}, and it is a choice rather than a "
        "contribution.\n"
        "         {}  ({})\n"
        "         {}  ({})\n"
        "       A build has exactly one {}. Remove one of them, or depend on a "
        "package that reexports the one you want.",
        cap_layer_name(c.layer),
        c.first, via(c.firstVia), c.second, via(c.secondVia),
        cap_layer_name(c.layer));
}

// ── Report ───────────────────────────────────────────────────────────────────
//
// The build prints what it RESOLVED, and that is why this design adds no
// manifest field for the same information. A line in a manifest states an
// intention that goes stale when the packages beneath it change; this states
// the outcome and cannot.
//
// BY DEFAULT IT PRINTS ONLY THE LAYERS THE COMPILER PAYLOAD DID NOT SUPPLY.
// A zero-configuration build resolves all five from one payload, and five lines
// reading `(payload)` carry no information — they are the answer to a question
// nobody asked. What earns a line is a layer that came from somewhere else.
//
// `verbose` prints all five, and DIAGNOSTICS ALWAYS DO: an error must show the
// evidence it rests on, including the parts that are ordinary.
inline std::string format_layers(const TargetSide& ts, bool verbose) {
    // Thirteen spaces so the layer names sit under the triple rather than under
    // the status verb: the caller's status line right-aligns a verb in twelve
    // columns and follows it with one space.
    constexpr std::string_view kIndent = "             ";
    const std::pair<std::string_view, const Layer&> rows[] = {
        { "compiler",         ts.compiler         },
        { "compiler-runtime", ts.compilerRuntime  },
        { "kernel-abi",       ts.kernelAbi        },
        { "c-abi",            ts.cAbi             },
        { "c++-abi",          ts.cxx              },
    };

    // WHETHER THE STACK IS SHOWN AT ALL IS DECIDED BEFORE ANY ROW IS
    // WRITTEN, AND THE FIRST VERSION DECIDED IT PER ROW.
    //
    // An absent layer is a statement rather than a gap, so it belongs in a
    // report that is showing the stack and nowhere else. Asking "has anything
    // been printed yet" made that depend on ORDER: a bare-metal build has an
    // absent `kernel-abi` above a prebuilt `c-abi`, so the statement was
    // swallowed while the line below it printed. Two passes, and the question
    // is asked once.
    const bool showing = verbose || std::any_of(
        std::begin(rows), std::end(rows), [](auto const& r) {
            return !r.second.absent() && r.second.origin != Origin::Payload;
        });
    if (!showing) return {};

    std::string out;
    for (auto const& [label, l] : rows) {
        if (!verbose && l.origin == Origin::Payload) continue;
        if (l.absent()) {
            out += std::format("{}{:<17} —\n", kIndent, label);
            continue;
        }
        std::string suffix = l.subset ? ", subset" : "";
        // One column layout whether or not an implementation is named, so the
        // rows read as a table rather than as a list of sentences.
        if (l.impl.empty())
            out += std::format("{}{:<17} {:<14} ({}{})\n", kIndent, label,
                               l.interfaceName, origin_name(l.origin), suffix);
        else
            out += std::format("{}{:<17} {:<14} ({}, {}{})\n", kIndent, label,
                               l.interfaceName, l.impl, origin_name(l.origin),
                               suffix);
    }
    return out;
}

inline std::string format_report(const TargetSide& ts, std::string_view targetName,
                                 bool verbose = false) {
    // The head carries no verb of its own: the caller supplies one through the
    // status line's own padding, and the layer lines below are indented to sit
    // under it.
    std::string head = (ts.llvmTriple.empty() || ts.llvmTriple == targetName)
        ? std::format("{}", targetName)
        : std::format("{} → {}", targetName, ts.llvmTriple);

    // WHEN THE SEGMENT IS NOT A C LIBRARY, SAY WHAT IT IS — HERE, WHERE THE
    // READER IS LOOKING AT IT.
    //
    // `x86_64-windows-gnu` above a line reading `c-abi musl` is not a
    // contradiction. Measured on the artefact of exactly that build:
    //
    //     imports        ntdll, KERNEL32, SHELL32 — no msvcrt, no ucrtbase
    //     `_Z…` symbols  4507
    //     `?…`  symbols  0
    //
    // The first line is why `c-abi musl` is honest: none of MinGW's C runtime
    // is linked. The other two are what `gnu` actually selected — the Itanium
    // C++ ABI rather than Microsoft's.
    //
    // AND IT CORRESPONDS TO NO ROW OF THIS REPORT, WHICH IS THE POINT. The
    // five layers record who SUPPLIES each layer; `gnu` names a convention the
    // OBJECTS FOLLOW, and several layers must agree on it. Pointing the reader
    // at `c++-abi libc++` would be a second wrong answer: libc++ is one
    // implementation of the standard library, libstdc++ is another, and both
    // sit on the Itanium ABI. The gloss therefore names the ABI itself, whose
    // name appears in no row and so cannot be mistaken for one.
    //
    // A warning would be wrong — it would fire on every legitimate MinGW build
    // and would say something false. A noun on the head line is not a
    // diagnostic; it is the missing half of a name the report was already
    // showing.
    //
    // Scoped to the case that actually reads as a contradiction: the segment is
    // present, it does not name a C library here, and the C library came from
    // somewhere the segment did not choose. A payload C library IS selected by
    // the triple, so `gnu → ucrt` follows visibly and needs no gloss.
    // `ObjectFormat` ONLY. The object-ABI axis used to be glossed here and is
    // now WARNED about instead — see `check_request`. Leaving both in place
    // would say the same thing twice, once as an aside and once as a warning,
    // which reads as two different findings.
    if (ts.envAxis == EnvAxis::ObjectFormat
        && !ts.requestedCAbi.empty()
        && !ts.cAbi.absent() && ts.cAbi.fromGraph()
        && !c_abi_request_satisfied(ts.requestedCAbi, ts.cAbi.interfaceName)) {
        head += std::format("   ({} selects {}, not a C library)",
                            ts.requestedCAbi,
                            env_axis_noun(ts.envAxis, ts.requestedCAbi));
    }
    head += '\n';

    auto body = format_layers(ts, verbose);
    if (!body.empty() && body.back() == '\n') body.pop_back();
    if (body.empty()) { head.pop_back(); return head; }
    return head + body;
}

} // namespace mcpp::targetside
