// toolchain_decision.cpp -- P5: the toolchain decided once the graph exists,
// through the resolver P2 defined.

module mcpp.build.prepare;
import :state;


import mcpp.build.prepare_inputs;

import std;
import mcpp.targetside;
import mcpp.build.refusal;
import mcpp.build.version_floor;
import mcpp.manifest;
import mcpp.source_kind;
import mcpp.toolchain.hostflags;   // the compile-token producer the package std module reuses
import mcpp.toolchain.detect;
import mcpp.toolchain.dialect;
import mcpp.toolchain.fingerprint;
import mcpp.toolchain.registry;
import mcpp.toolchain.linkmodel;
// For `resolve_version_match` / `list_installed_versions`: a bare compiler
// family named by the dependency graph resolves to a concrete version through
// exactly the path `mcpp toolchain default <family>` uses.
import mcpp.toolchain.lifecycle;
import mcpp.toolchain.stdmod;
import mcpp.toolchain.post_install;
import mcpp.toolchain.abi;
import mcpp.build.plan;
import mcpp.build.flags;          // compute_flags — the per-role contracts (#418)
import mcpp.build.graph_shape;  // #407: the graph says which mode wrote it
import mcpp.build.build_program;
import mcpp.build.backend;      // BuildOptions for the tool sub-build
import mcpp.build.ninja;        // make_ninja_backend — driving that sub-build
import mcpp.toolchain.post_install;
import mcpp.platform;

namespace mcpp::build {

std::expected<void, std::string> phase5_toolchain_after_graph(PrepareState& state) {

    // ─── The toolchain, resolved now that the graph exists ──────────────────
    //
    // THE TARGET AND THE COMPILER ARE NOT BOUND TOGETHER, AND THE ROW'S
    // CONVENTION IS A FALLBACK RATHER THAN A RULE.
    //
    // `x86_64-linux-musl → gcc@16.1.0` does not say "prefer gcc". It says "the
    // musl-gcc payload is what supplies this target's C library". A project
    // whose C library comes from its dependency graph does not use that payload,
    // and for it the convention is not a default but a substitution — measured,
    // it replaced a toolchain the user had set with `mcpp toolchain default` and
    // said nothing.
    //
    // The discriminator is whether anything in the graph supplies the system,
    // which is what these few lines ask. It is the same question
    // `mcpp.targetside` answers in full further down; asked here it needs only
    // the answer's shape, so it reads the manifests rather than resolving them.
    {
        bool graphSuppliesSystem = false;
        // `requires` IS READ HERE TOO, AND UNTIL THIS LOOP IT WAS ONLY EVER
        // CHECKED — A THOUSAND LINES LATER, AGAINST A DECISION THIS BLOCK HAD
        // ALREADY MADE WITHOUT IT.
        //
        // `provides` and `requires` are the two halves of one vocabulary and
        // they were read at opposite ends of the function: this block consulted
        // the first to decide the compiler, and `check_requirements` used the
        // second only to reject the outcome. Measured on 2026.8.26.1, one
        // three-line manifest, `llvm@22.1.8` already installed:
        //
        //     [dependencies]
        //     openkal-llvm-runtime = "0.1.3"     # requires mcpp:compiler=llvm
        //
        //     $ mcpp build                       # global default gcc@16.1.0
        //       error: `openkal-llvm-runtime@0.1.3` requires the compiler to be `llvm`.
        //              Select that compiler …  mcpp toolchain default llvm
        //     $ MCPP_TOOLCHAIN=llvm@22.1.8 mcpp build
        //       Finished dev [unoptimized + debuginfo] in 1.02s
        //
        // Nothing was missing. The engine knew which compiler was wanted, the
        // payload was on the machine, and the remedy it printed was to change
        // the default for EVERY project on the box because ONE project's
        // dependency asked.
        //
        // AND THIS IS THE PLACE, NOT MERELY *A* PLACE. `resolve_target_toolchain`
        // has exactly two call sites — its own one-shot recursion, and the one
        // at the bottom of this block — so every branch inside it, INCLUDING the
        // first-run install-and-persist path and all three
        // `write_default_toolchain` calls, is downstream of this line. Setting
        // `tcSpec` here therefore selects the compiler without writing anything:
        // on a machine with no toolchain at all the first-run branch is not even
        // reached, because its condition is `!tcSpec.has_value()`.
        //
        // That is the whole design. "Do not touch the user's configuration" is
        // not a rule anyone has to remember here — the writes live on a branch
        // this no longer enters.
        std::string reqCompiler, reqCompilerBy;

        // A FAMILY NAME BECOMES A CONCRETE SPEC THE SAME WAY IT DOES FOR
        // `mcpp toolchain default <family>`, AND FOR THE SAME REASON.
        //
        // `requires = ["mcpp:compiler=llvm"]` names a family; the build path
        // needs `<family>@<version>` and refuses anything else
        // (`expected '<pkg>@<version>'`). There are two honest sources for the
        // missing half and they are tried in this order:
        //
        //   1. what is already installed — highest version wins, nothing is
        //      downloaded, and it is literally the same two functions
        //      `toolchain_set_default` calls;
        //   2. the vocabulary's own pins — the version this ecosystem ships for
        //      that family's payload, already written down once per row
        //      (`pinned_versions_for`). Deriving it from
        //      there rather than from a fresh constant means the answer moves
        //      when the ecosystem moves, with nobody having to remember a
        //      second place.
        //
        // NOT `pins::kFirstRun*`. Those are per-HOST first-run defaults —
        // `llvm@23.1.3` on macOS, `gcc@16.1.0` on Linux x86_64 — so reading them
        // would make the version a package requires depend on which machine
        // built it. A requirement is a property of the package.
        auto resolve_required_family =
            [&](const std::string& family)
            -> std::expected<std::string, std::string> {
            auto spec = mcpp::toolchain::parse_toolchain_spec(family);
            if (!spec) {
                refusal::record(refusal::Code::CompilerRequirementConflict);
                return std::unexpected(std::format(
                    "`{}` requires the compiler to be `{}`, and mcpp has no "
                    "compiler family by that name.\n"
                    "       known families: gcc, llvm, msvc, emsdk, android-ndk.",
                    reqCompilerBy, family));
            }

            if (auto cfg = state.get_cfg(true); cfg) {
                auto pkg = mcpp::toolchain::to_xim_package(*spec);
                if (auto picked = mcpp::toolchain::resolve_version_match(
                        "", mcpp::toolchain::list_installed_versions(
                                (*cfg)->xlingsHome() / "data" / "xpkgs",
                                pkg.ximName)))
                    return std::format("{}@{}", family, *picked);
            }

            // Matched by the payload a pin names, not by its family: the NDK
            // and emsdk pins are llvm-family pins of other payloads (#641).
            if (auto picked = mcpp::toolchain::resolve_version_match(
                    "", mcpp::toolchain::pinned_versions_for(*spec)))
                return std::format("{}@{}", family, *picked);

            // Neither source has one. Saying which family and which two places
            // were consulted is the difference between an actionable message
            // and "something went wrong".
            // RECORDED, like every other refusal in this function. An
            // unnamed branch reports `other`, and this release exists partly
            // because one of those had a perfectly good name.
            refusal::record(refusal::Code::CompilerRequirementConflict);
            return std::unexpected(std::format(
                "`{}` requires the compiler to be `{}`, and mcpp has no version "
                "of it to use.\n"
                "       none is installed, and no target row pins one.\n"
                "       install one — `mcpp toolchain install {} <version>` "
                "(`mcpp toolchain list --available {}`).",
                reqCompilerBy, family, family, family));
        };

        for (auto const& pkg : state.packages) {
            for (auto const& entry : pkg.manifest.provides) {
                auto cap = mcpp::targetside::parse_capability(entry);
                if (!cap || !*cap) continue;
                if ((*cap)->layer == mcpp::targetside::CapLayer::KernelAbi
                 || (*cap)->layer == mcpp::targetside::CapLayer::CAbi) {
                    graphSuppliesSystem = true;
                }
            }
            for (auto const& entry : pkg.manifest.requires_) {
                auto cap = mcpp::targetside::parse_capability(entry);
                if (!cap || !*cap) continue;
                if ((*cap)->layer != mcpp::targetside::CapLayer::Compiler) continue;
                // A bare `mcpp:compiler` asks only that one exist, which it
                // always does. Only a named family selects anything.
                if ((*cap)->interfaceName.empty()) continue;
                const auto pkgId = pkg.manifest.package.version.empty()
                    ? pkg.manifest.package.name
                    : std::format("{}@{}", pkg.manifest.package.name,
                                  pkg.manifest.package.version);
                // TWO DIFFERENT FAMILIES IS AN ERROR RATHER THAN A PICK, the
                // same rule `provides` already follows one screen down. Choosing
                // by graph-traversal order would make the answer depend on an
                // order the author neither writes nor can predict — and unlike a
                // conflicting `provides`, this one would silently satisfy one
                // package's requirement and fail the other's inside a header.
                if (!reqCompiler.empty() && reqCompiler != (*cap)->interfaceName) {
                    refusal::record(refusal::Code::CompilerRequirementConflict);
                    return std::unexpected(std::format(
                        "two packages require different compilers, and a build "
                        "has only one.\n"
                        "         {:<28} requires `{}`\n"
                        "         {:<28} requires `{}`\n"
                        "       Both cannot hold. Drop one of them, or take a "
                        "version of one that is\n"
                        "       configured for the other's compiler.",
                        reqCompilerBy, reqCompiler, pkgId,
                        (*cap)->interfaceName));
                }
                if (reqCompiler.empty()) {
                    reqCompiler   = (*cap)->interfaceName;
                    reqCompilerBy = pkgId;
                }
            }
        }
        // AND A FREESTANDING PIN SURVIVES IT. `graphSuppliesSystem` spans
        // kernel-abi and c-abi, and it correctly cancels a HOSTED row's
        // convention — that row names the payload the graph is replacing.
        // A bare-metal row names the only compiler that emits the target.
        //
        // Measured 2026-08-25, on a three-line manifest:
        //
        //     provides = ["mcpp:kernel-abi=openkal"]
        //     $ mcpp build --target riscv64-none-elf
        //       Resolved gcc@16.1.0 → riscv64-none-elf → …/bin/g++
        //       g++: error: unrecognized argument in option '-mabi=lp64d'
        //       g++: error: unrecognized command-line option
        //                   '--target=riscv64-none-elf'
        //
        // A package saying which layer it supplies made the host compiler be
        // chosen for a target it cannot produce. Same shape as the four
        // defects 2026.8.25.1 fixed: a predicate spanning two layers deciding
        // something that does not depend on either of them.
        if (!state.targetPinCandidate.empty()
            && (!graphSuppliesSystem || state.targetPinIsCapability)) {
            if (state.tcOrigin == TcOrigin::GlobalDefault && state.tcSpec.has_value()
                && *state.tcSpec != state.targetPinCandidate)
                state.pinReplacedDefault = *state.tcSpec;
            // Kept for the build program's host resolution; see the
            // declaration. Taken from every origin, not only the global
            // default, because a `[toolchain]` the manifest named is just as
            // much the host's compiler as a remembered default is.
            if (state.tcSpec.has_value() && *state.tcSpec != state.targetPinCandidate)
                state.hostSpecBeforeRowPin = *state.tcSpec;
            state.tcSpec   = state.targetPinCandidate;
            state.tcOrigin = TcOrigin::TargetPin;
        }

        // THE GRAPH'S REQUIREMENT, TAKEN AS AN INSTRUCTION RATHER THAN AS A
        // TEST TO FAIL LATER.
        //
        // Everything above this line decides the compiler from what mcpp knows
        // about the TARGET. A package saying `requires = ["mcpp:compiler=llvm"]`
        // is saying something about ITSELF — its C++ runtime was configured for
        // one family and its headers record that configuration — and it is the
        // most specific statement in the build. Below the user's own word, above
        // every default mcpp keeps.
        //
        // THE RANK IS NOT NEW. `TcOrigin` already sorts these, and
        // `tc_origin_is_user_explicit` already answers "may mcpp revise this".
        // The defect was never that the answer was wrong; it was that nobody
        // asked. `GlobalDefault` is deliberately not user-explicit — see the
        // note on that function — so a remembered default is exactly the kind of
        // value this may replace.
        // `system` IS LEFT ALONE, AND IT IS THE ONE VALUE HERE THAT IS AN
        // ESCAPE HATCH RATHER THAN AN ANSWER.
        //
        // It means "the PATH compiler, whatever it is" — a deliberate opt-out
        // of the payload model. Substituting a payload for it would defeat
        // exactly what the user asked for, and mcpp cannot even tell whether
        // the requirement is already satisfied: the family of a PATH compiler
        // is not knowable from the spec. `check_requirements` reports the
        // mismatch further down against what the driver actually turned out to
        // be, which is the only place that answer exists.
        const bool tcIsSystemEscapeHatch =
            state.tcSpec.has_value() && *state.tcSpec == "system";
        if (!reqCompiler.empty() && !tcIsSystemEscapeHatch) {
            std::string haveFamily;
            if (state.tcSpec.has_value())
                if (auto s = mcpp::toolchain::parse_toolchain_spec(*state.tcSpec); s)
                    haveFamily =
                        std::string(mcpp::toolchain::family_name(s->family));

            if (haveFamily != reqCompiler) {
                // THE PROJECT'S OWN WORD IS NOT REVISED, AND THIS IS THE ONLY
                // CASE THAT STILL REFUSES. `[toolchain]`, `[target.X].toolchain`
                // and `MCPP_TOOLCHAIN` are statements about THIS build; the
                // graph disagreeing with one of them is a real contradiction and
                // `check_requirements` reports it further down with both names.
                // Nothing to do here but leave the value alone.
                if (tc_origin_is_user_explicit(state.tcOrigin)) {
                    // fall through to check_requirements
                }
                // A ROW'S PIN THAT SURVIVED TO HERE CANNOT BE OVERRIDDEN BY
                // A REQUIREMENT, AND THE REASON IS THE SAME ONE THE PIN EXISTS
                // FOR.
                //
                // The block above applied it only when the graph does NOT supply
                // the system, or when the row names a capability. In the first
                // case the row's payload is what carries this target's headers
                // and C library, and a different compiler brings none — measured
                // as `crtbeginT.o (bare name)` and as a host `crtbegin.o`, both
                // accurate about the symptom and silent about the decision. In
                // the second the row names the only compiler that emits the
                // target at all.
                //
                // Either way the requirement cannot be honoured, and saying so
                // here — where both halves are known — beats a compiler
                // complaining about a file the reader never named.
                else if (state.tcOrigin == TcOrigin::TargetPin) {
                    // THE TWO ROWS REFUSE UNDER ONE RULE AND FOR TWO
                    // REASONS, AND ONE REMEDY DOES NOT SERVE BOTH.
                    //
                    // A CONVENTION pin is cancelled by a graph that supplies the
                    // target's system — that is `graphSuppliesSystem`, one
                    // screen up — so "depend on a package that supplies it" is
                    // exactly the way out.
                    //
                    // A CAPABILITY pin is not: `targetPinIsCapability` keeps it
                    // applied no matter what the graph supplies, because no
                    // other family emits the target at all. Offering the same
                    // remedy there prints an instruction that the sentence
                    // directly above it has already ruled out — the failure
                    // this release removes from `check_requirements`, reproduced
                    // three screens away.
                    std::string_view why = state.targetPinIsCapability
                        ? "The row names its compiler as a capability: no other "
                          "family emits this target."
                        : "The row's payload is what supplies this target's "
                          "headers and C library,\n       and nothing in the "
                          "dependency graph supplies them instead.";
                    std::string remedy = state.targetPinIsCapability
                        ? std::format(
                              "       Drop the package that requires `{}`, or "
                              "take a version of it built\n"
                              "       for `{}`.",
                              reqCompiler, state.targetPinCandidate)
                        : std::format(
                              "       Depend on a package that supplies this "
                              "target's system (its kernel\n"
                              "       interface and C library) so the row's "
                              "payload is not needed, or drop\n"
                              "       the package that requires `{}`.",
                              reqCompiler);
                    refusal::record(refusal::Code::CompilerRequirementConflict);
                    return std::unexpected(std::format(
                        "`{}` requires the compiler to be `{}`, and target '{}' "
                        "cannot be built with it here.\n"
                        "         target row       {:<14} ({})\n"
                        "         required         {:<14} (required by {})\n"
                        "       {}\n{}",
                        reqCompilerBy, reqCompiler,
                        state.targetRowName.empty() ? state.overrides.target_triple
                                              : state.targetRowName,
                        state.targetPinCandidate,
                        state.targetPinIsCapability ? "capability" : "convention",
                        reqCompiler, reqCompilerBy,
                        why, remedy));
                }
                // Free to take it. `tcSpec` is either absent (nothing configured
                // anywhere) or one of mcpp's own remembered answers.
                else {
                    auto pickedSpec = resolve_required_family(reqCompiler);
                    if (!pickedSpec)
                        return std::unexpected(pickedSpec.error());
                    state.graphCompilerReplaced   = state.tcSpec.value_or("");
                    state.graphCompilerRequiredBy = reqCompilerBy;
                    state.graphCompilerFamily     = reqCompiler;
                    state.tcSpec   = *pickedSpec;
                    state.tcOrigin = TcOrigin::GraphRequirement;
                }
            }
        }
        // OVERRIDING THE CONVENTION IS ALLOWED; OVERRIDING IT AND SUPPLYING
        // NOTHING IN ITS PLACE IS NOT, AND UNTIL THIS BLOCK IT LOOKED THE SAME.
        //
        // A hosted row's pin names the payload that supplies the target's C
        // library. A project may name a different compiler — that is the escape
        // hatch the whole convention/capability distinction exists to protect —
        // and the ordinary reason to do so is that its dependency graph supplies
        // the C library instead. `examples/06-openkal-cross` is exactly that:
        // `llvm@22.1.8` plus `openkal-llvm-runtime`, and `graphSuppliesSystem`
        // is true there.
        //
        // WITH NEITHER, THE BUILD USED TO RUN ANYWAY AND FAIL SOMEWHERE ELSE.
        // Measured 2026-08-26 on Linux, `[toolchain] default = "llvm@22.1.8"`
        // and no dependencies:
        //
        //   --target x86_64-linux-musl
        //     hermetic link check failed … crtbeginT.o (bare name)
        //   --target x86_64-windows-gnu
        //     hermetic link check failed …
        //     /usr/lib/gcc/x86_64-w64-mingw32/13-win32/crtbegin.o (outside)
        //
        // Both are accurate about the symptom and silent about the decision:
        // clang is retargetable and brings no C library, so it reached for a
        // gcc installation — one that does not exist under the payload prefix
        // in the first case, and that belongs to the HOST in the second. There
        // is no llvm payload supplying either target's C library today.
        //
        // THE REFUSAL IS DECIDED HERE BECAUSE ONLY HERE ARE BOTH HALVES
        // KNOWN. The row is read a thousand lines earlier and the graph does
        // not exist then; `host_can_serve` is family-agnostic and would answer
        // "yes, some payload here produces it" — the same shape as the family
        // this release is about, a predicate answering a question narrower than
        // the one it is asked.
        if (!state.targetRowPin.empty() && !graphSuppliesSystem
            && tc_origin_is_user_explicit(state.tcOrigin) && state.tcSpec.has_value()) {
            auto declared = mcpp::toolchain::parse_toolchain_spec(*state.tcSpec);
            auto rowTc    = mcpp::toolchain::parse_toolchain_spec(state.targetRowPin);
            if (declared && rowTc && declared->family != rowTc->family) {
                refusal::record(refusal::Code::ConventionUnreplaced);
                return std::unexpected(std::format(
                    "target '{}' takes its C library from the '{}' payload, and "
                    "'{}' has none here.\n"
                    "       The row's toolchain is a convention, so naming your "
                    "own compiler overrides it —\n"
                    "       but the convention is what supplies this target's "
                    "headers and C library, and\n"
                    "       nothing in the dependency graph supplies them "
                    "instead.\n"
                    "       depend on a package that implements the target's C "
                    "library (openkal-musl and\n"
                    "       openkal-llvm-runtime are the ones in the index), or "
                    "remove the `[toolchain]`\n"
                    "       line so `{}` is used for this target.",
                    state.targetRowName, state.targetRowPin, *state.tcSpec, state.targetRowPin));
            }
        }
        if (auto r = state.resolve_target_toolchain(); !r)
            return std::unexpected(r.error());
    }

    return {};
}

} // namespace mcpp::build
