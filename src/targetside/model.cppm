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

// ── The four ways a layer can be supplied ────────────────────────────────────
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
// Three layers, and their correspondence to the triple is not decoration:
//
//   kernelAbi  ← the triple's OS field       linux / macos / windows / none
//   cAbi       ← the triple's ENV field      gnu / musl / (msvc)
//   cxx        ← no field of the triple      because it sits above the ABI
//
// The middle layer is implicit on a traditional stack — a C library issues
// syscalls or calls Win32 directly, and nothing names the seam. openkal's whole
// contribution is to name it, which is why `kernelAbi` reads `—` for a picolibc
// bare-metal build and `openkal` for an openkal one ON THE SAME TARGET.
struct TargetSide {
    // The triple the driver is actually given, which is NOT the one the user
    // wrote. Measured: `--target=aarch64-macos` produces a Mach-O whose
    // MinVersion load command carries no platform and version 10.4, while
    // `arm64-apple-macos14.0` carries `macos 14.0`. Right format, right
    // architecture, wrong platform metadata — so the translation is load
    // bearing and belongs in the report.
    std::string llvmTriple;

    Layer kernelAbi;
    Layer cAbi;
    Layer cxx;

    // The single question the five former derivation sites actually asked.
    //
    // It is about the SYSTEM, not about the C++ runtime. A C program over
    // openkal has no C++ runtime and its target side still comes from the
    // graph — that case is exactly the measured defect above.
    bool system_from_graph() const {
        return kernelAbi.fromGraph() || cAbi.fromGraph();
    }
};

// ── Capability grammar: mcpp:<layer>[=<interface>] ───────────────────────────
//
// mcpp HARDCODES LAYER NAMES AND NEVER HARDCODES IMPLEMENTATIONS.
//
// The three layer names below are a closed set compiled into the engine. The
// implementations that fill them — openkal, musl, picolibc, and whatever comes
// next — appear nowhere in this file or any other. That line is what separates
// this design from the string comparison it replaces (`fam == "openkal-llvm"`
// in prepare), which put a product name inside the engine.
//
// Layer names may be hardcoded because the layers are fixed by the C and C++
// build model and do not grow. Implementations may not, because growing is
// precisely what they do: the ecosystem's combinations are 2×N×M while its
// packages are 2+N+M.
enum class CapLayer { KernelAbi, CAbi, CxxAbi };

constexpr std::string_view cap_layer_name(CapLayer l) {
    switch (l) {
        case CapLayer::KernelAbi: return "kernel-abi";
        case CapLayer::CAbi:      return "c-abi";
        case CapLayer::CxxAbi:    return "c++-abi";
    }
    return {};
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
    if      (layer == "kernel-abi") d.layer = CapLayer::KernelAbi;
    else if (layer == "c-abi")      d.layer = CapLayer::CAbi;
    else if (layer == "c++-abi")    d.layer = CapLayer::CxxAbi;
    else
        return std::unexpected(std::format(
            "`provides = [\"{}\"]` names no capability mcpp knows.\n"
            "       The `mcpp:` prefix is reserved for the target-side layers "
            "this engine resolves, and there are three:\n"
            "         mcpp:kernel-abi[=<name>]   the platform interface a C library sits on\n"
            "         mcpp:c-abi[=<name>]        the C library\n"
            "         mcpp:c++-abi[=<name>]      the C++ runtime\n"
            "       A capability of your own needs no prefix; those are passed "
            "through untouched.", entry));

    if (!iface.empty()) d.interfaceName = std::string(iface);
    return std::optional<CapDecl>{d};
}

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
    ts.llvmTriple = in.llvmTriple;

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
        ts.cAbi = { Origin::Payload, in.targetEnv.empty() ? "glibc" : in.targetEnv,
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

// The same rule stated for the explicit-override path, where the resolver's
// structure no longer guarantees it.
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

// ── Report ───────────────────────────────────────────────────────────────────
//
// The build prints what it RESOLVED, and that is why this design adds no
// manifest field for the same information. A line in a manifest states an
// intention that goes stale when the packages beneath it change; this states
// the outcome and cannot.
inline std::string format_report(const TargetSide& ts, std::string_view targetName) {
    // Thirteen spaces so the layer names sit under the triple rather than under
    // the status verb: the caller's status line right-aligns a verb in twelve
    // columns and follows it with one space.
    constexpr std::string_view kIndent = "             ";
    auto line = [&](std::string_view label, const Layer& l) {
        if (l.absent())
            return std::format("{}{:<11} —\n", kIndent, label);
        std::string suffix = l.subset ? ", subset" : "";
        if (l.impl.empty())
            return std::format("{}{:<11} {} ({}{})\n", kIndent, label,
                               l.interfaceName, origin_name(l.origin), suffix);
        return std::format("{}{:<11} {:<14} ({}, {}{})\n", kIndent, label,
                           l.interfaceName, l.impl, origin_name(l.origin), suffix);
    };

    // The head carries no verb of its own: the caller supplies one through the
    // status line's own padding, and the layer lines below are indented to sit
    // under it.
    std::string head = (ts.llvmTriple.empty() || ts.llvmTriple == targetName)
        ? std::format("{}\n", targetName)
        : std::format("{} → {}\n", targetName, ts.llvmTriple);

    auto body = line("kernel-abi", ts.kernelAbi)
              + line("c-abi",      ts.cAbi)
              + line("c++",        ts.cxx);
    if (!body.empty() && body.back() == '\n') body.pop_back();
    return head + body;
}

} // namespace mcpp::targetside
