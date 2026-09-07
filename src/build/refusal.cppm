// mcpp.build.refusal — the machine-readable identity of a target refusal.
//
// WHY A CODE AND NOT THE SENTENCE.
//
// mcpp refuses a target for several distinct reasons, and every one of them
// leaves the same trace: `prepare_build` returns `std::unexpected(<prose>)`.
// A caller that needs to know WHICH reason has, until now, had exactly one
// option — match the prose. `tests/matrix/scan.sh` did, and so did three e2e
// tests, and the cost was measured on 2026-08-26 within a single session:
// reworded `cannot emit it` to `cannot be emitted by`, and a test that had
// been asserting a refusal silently began asserting nothing.
//
// THE MESSAGE IS STILL A PROMISE. This does not replace it — a refusal that
// does not name the target, the reason and the way out is a defect whether or
// not it carries a code, and the e2e tests keep asserting exactly that. What
// the code replaces is *classification*: "did it refuse, and for which reason"
// is a question with a finite answer set, and a finite answer set should not be
// spelled as a substring search.
//
// ── Why a sink rather than a return type ───────────────────────────────────
//
// `prepare_build` returns `std::expected<BuildContext, std::string>` and it has
// dozens of error returns, of which these are six. Widening the error type
// would touch every consumer of `.error()` to express something only one of
// them reads. `mcpp::diag` settled the same trade-off the same way: a per-run
// sink, written where the fact is known and read where it is needed.
//
// SET IMMEDIATELY BEFORE THE `return`, NEVER EARLIER. A code set at the top
// of a branch that then does not refuse is worse than no code at all: it makes
// a successful build report a reason.

export module mcpp.build.refusal;

import std;

export namespace mcpp::build::refusal {

// ONE CODE PER DECISION, and the decisions are the ones a reader of the
// target matrix has to tell apart. Adding a refusal branch means adding a code
// here — an unnamed branch reports `other`, which is a visible admission
// rather than a silent merge into a neighbouring reason.
enum class Code {
    None,                  // no refusal
    UnknownTarget,         // the spelling names no row, and no (arch, os) group
    AmbiguousRequest,      // several rows serve (arch, os) and none is the default
    // The compiler a package requires cannot be used for this build: the
    // project stated a different one, two packages disagree, the target row
    // names a family that is the only one able to emit it, or no version of the
    // required family exists to use. One code, because what a consumer does
    // about all four is the same — read the message.
    CompilerRequirementConflict,
    TierPlanned,           // the row exists in the vocabulary, nothing is wired
    HostCannotServe,       // no payload here, and no graph supplied the system
    CapabilityPin,         // the row's toolchain is a capability, not a preference
    ConventionUnreplaced,  // the convention was overridden and nothing replaced it
    OsMismatch,            // requested and resolved triples name different systems
    LayerRequirement,      // a package requires a layer the resolution did not give it
    LayerOrdering,         // the five layers do not stack (check_layering)
    LldRequiredAbsent,     // the row links through lld directly and none is here
    HostToolToolchain,     // build.mcpp under a cross target has no host toolchain
    StdModulePrecompile,   // the std module could not be precompiled
    // Two packages provide one capability and at least one of them declared it
    // exclusive. Distinct from CapabilityPin: that one is about SELECTING a
    // provider for a requirement, this one is about two implementations of one
    // interface being in the same link at all.
    ExclusiveCapability,
    // A package requires more of the machine than the machine was declared to
    // have. Distinct from a capability that is missing entirely: here the thing
    // exists and is too old.
    VersionFloorUnmet,
    // A source glob is constrained to a device set this build does not cover.
    // Distinct from VersionFloorUnmet (the machine is too old) and from a
    // prebuilt artifact's tag mismatch (that refusal is about consuming): here
    // the project's own sources ask for a device the build was not told to
    // target.
    AccelMismatch,
    // A source glob names an accelerator backend its own package does not list
    // in `[package] accelerators`. Distinct from AccelMismatch, which is about
    // THIS build's device set: this one is a property of the manifest alone and
    // is refused whether or not the build names any accelerator at all.
    AccelBackendUndeclared,
    // A device-kind source reached no action. The engine has no compile rule
    // for those extensions, so a file the package's build program did not claim
    // is a file nothing compiles -- reported here rather than as the undefined
    // reference it used to become at the link.
    DeviceSourceUnconsumed,
    // `build.mcpp` imports a module no dependency supplies as a host module.
    // Distinct from an ordinary missing dependency: the package may well be in
    // the graph and reaching the target, and still not be compiled for the
    // build program, which is what `host-module = true` asks for.
    HostModuleMissing,
    // Two declarations name one xlings package at versions that cannot both
    // hold. Distinct from VersionFloorUnmet, which is about the machine: this
    // one is about two manifests disagreeing over a tool.
    ToolVersionConflict,
    Other,                 // a refusal that has not been given a code yet
};

constexpr std::string_view name(Code c) {
    switch (c) {
        case Code::None:                 return "none";
        case Code::UnknownTarget:        return "unknown-target";
        case Code::AmbiguousRequest:     return "ambiguous-request";
        case Code::CompilerRequirementConflict:
                                         return "compiler-requirement-conflict";
        case Code::TierPlanned:          return "tier-planned";
        case Code::HostCannotServe:      return "host-cannot-serve";
        case Code::CapabilityPin:        return "capability-pin";
        case Code::ConventionUnreplaced: return "convention-unreplaced";
        case Code::OsMismatch:           return "os-mismatch";
        case Code::LayerRequirement:     return "layer-requirement";
        case Code::LayerOrdering:        return "layer-ordering";
        case Code::LldRequiredAbsent:    return "lld-required-absent";
        case Code::HostToolToolchain:    return "host-tool-toolchain";
        case Code::StdModulePrecompile:  return "std-module-precompile";
        case Code::ExclusiveCapability:  return "exclusive-capability";
        case Code::VersionFloorUnmet:    return "version-floor-unmet";
        case Code::AccelMismatch:        return "accel-mismatch";
        case Code::AccelBackendUndeclared:
                                         return "accel-backend-undeclared";
        case Code::DeviceSourceUnconsumed:
                                         return "device-source-unconsumed";
        case Code::HostModuleMissing:    return "host-module-missing";
        case Code::ToolVersionConflict:  return "tool-version-conflict";
        case Code::Other:                return "other";
    }
    return "other";
}

// Written by `prepare_build`'s refusal sites; read by the machine-readable
// output layer. `record` is what a refusal site calls; `take` reads and clears,
// so a later successful run cannot inherit an earlier run's reason.
void record(Code c);
Code take();

} // namespace mcpp::build::refusal

// ── implementation ──────────────────────────────────────────────────────────

namespace mcpp::build::refusal {
namespace {
// `thread_local`: `prepare_build` recurses for nested host sub-builds, and
// those run on the calling thread — but a build program's own sub-build must
// not leave its refusal behind for the outer one. Same thread, so the sink is
// shared deliberately; the outer refusal is recorded last and wins, which is
// the one the user was asking about.
thread_local Code g_last = Code::None;
} // namespace

void record(Code c) { g_last = c; }

Code take() { Code c = g_last; g_last = Code::None; return c; }

} // namespace mcpp::build::refusal
