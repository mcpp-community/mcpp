// mcpp.build.prepare — BuildContext + prepare_build: the build-orchestration
// core (workspace -> toolchain -> dependency resolution -> features ->
// modgraph -> fingerprint -> plan -> lockfile).
//
// LAYOUT. This file is the primary interface: the exported types, the exported
// inline functions, and the declarations of every other exported function,
// default arguments included (they belong on the declaration, not the
// definition). Nothing else is defined here. The implementation lives under
// src/build/prepare/:
//   state.cppm         implementation partition `:state`: PrepareState (the
//                      working state every phase reads and writes, by
//                      reference, in place of prepare_build's former ~180
//                      locals), the phase functions' declarations, and the
//                      declarations of the helpers the phases share.
//   driver.cpp         prepare_build: construct PrepareState, run the phases
//                      in order, return what the last one builds.
//   manifest.cpp       P0 -- the manifest and its workspace.
//   toolchain.cpp      P1, P2 -- the toolchain specification and target axis;
//                      the toolchain resolver's definition.
//   xlings.cpp         P3 -- xlings payloads before the graph.
//   graph_load.cpp     P4a -- loading one git, path or version dependency.
//   graph.cpp          P4b -- the worklist, the graph, the cycle check.
//   toolchain_decision.cpp  P5 -- the toolchain, decided once the graph exists.
//   features.cpp       P6-P8 -- features, capabilities, host tools, and the
//                      dependencies' build programs.
//   target_side.cpp    P9, P10 -- the target side and each dependency's link form.
//   scan.cpp           P11, P12 -- the module scan, validation, fingerprint.
//   plan.cpp           P13 -- the BuildContext.
//   records.cpp        P13 -- mcpp.lock and resolution.json.
//   config.cpp, options.cpp, toolchain_env.cpp, fetch.cpp
//                      the helpers the phases share: manifest merges and
//                      feature requests; invocation options; target rows,
//                      sysroots and build-program environments; git remotes
//                      and xlings provisioning.
// Every file stays at or below 2,500 lines (.github/tools/check_file_lengths.sh).
//
// A GCC 16.1 CONSTRAINT SHAPES ALL OF THIS. Measured locally and recorded in
// mcpp-community/mcpp#721 (the archived attempt is branch wip/prepare-split):
// inserting a new INTERFACE unit into
// mcpp.build.prepare's import chain — a separately named module, or an
// interface partition (`export module mcpp.build.prepare:x;`) — makes GCC
// 16.1 segfault in add_imported_namespace while reading `import mcpp.cli;`
// in src/main.cpp, regardless of that unit's content. Implementation units,
// and an implementation partition imported only by implementation units, do
// not trigger it. Therefore: this file imports no partition and defines
// nothing beyond the declarations above; `:state` is an implementation
// partition, never an interface partition, and only driver.cpp and the
// phaseN files import it. Anyone adding a new interface partition or a new
// named module to this chain should build with GCC 16.1 first — the failure
// is immediate and unambiguous.
//
// Bodies moved verbatim from the CLI layer, then from one file into many.
// Zero behavior change either time.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.build.prepare;

export import mcpp.build.prepare_inputs;

import std;
import mcpp.build.version_floor;
import mcpp.platform.axis;
import mcpp.manifest;
import mcpp.source_kind;
import mcpp.modgraph.glob;
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;
import mcpp.modgraph.validate;
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
import mcpp.xlings;
import mcpp.xlings.runtime_selection;
import mcpp.runtime.binding;
import mcpp.toolchain.post_install;
import mcpp.platform;
import mcpp.wire;               // Severity, for PlanNote (#699 item 2, E3)
import mcpp.bmi_cache;

namespace mcpp::build {

// L1 cfg merge for ONE package's manifest (root or ANY dependency — path,
// git, or version/registry): append the matching conditional
// cflags/cxxflags/ldflags and sources (G1b) to its buildConfig. Sources also
// update the legacy modules.sources mirror — the scanner walks that.
//
// #229: this is the SINGLE funnel for cfg-conditional sources/flags — every
// package's manifest passes through exactly one call to this function,
// always immediately BEFORE that manifest is captured into `packages[]` via
// makePackageRoot()/propagateLinkFlags() (which snapshot buildConfig into
// privateBuild/linkUsage and into the root's propagated ldflags — merging
// any later than that point is silently lost for flags, though not for
// sources, which the modgraph scan re-reads live). Three call sites, one per
// loading branch, together cover every package exactly once: the root
// (before its own makePackageRoot), the path/git-dep branch, and
// loadVersionDep() (shared by the main per-dependency loop, the
// multi-version mangling secondary, and the SemVer-merge re-fetch — all three
// of ITS callers get the merge for free from the one call inside it).
// The dependency MAPS ride the same funnel (#359). They used to be merged by
// a hand-written loop at the root call site only, with a comment declaring a
// dependency's own conditional deps "out of scope". That was the #229 shape
// one level up: three call sites merged build inputs, ONE of them also merged
// deps, and nothing said why. A package's `[target.windows.dependencies]` is
// its own statement about itself and means the same thing whether the package
// is the root or someone's dependency.
// The resolved triple travels INSIDE `ctx` (cfgpred::Ctx::triple). It used to
// be a third parameter here too, which is how a bare-triple predicate came to
// disagree with a cfg() one about the same native build — see the note on Ctx.
// `[target.<sel>.xlings…]` — the TARGET axis of the tool plane (SPEC-004 §4).
//
// Folded into `m.xlings` exactly as the conditional build inputs fold into
// `m.buildConfig`, so every downstream reader — the two provisioning passes,
// `fillXpkgDirs`, the materialised `.xlings.json` — stays on one flat list and
// none of them has to learn that a second axis exists.
//
// THE HOST AXIS IS NOT TOUCHED. Top-level `[xlings]` has already resolved its
// platform-keyed values against the host by the time this runs; folding here
// puts the target's entries beside them.
//
// DEDUP IS BY PACKAGE, NOT BY ADDRESS. `xim:glibc` and `xim:glibc@2.40` are two
// addresses for ONE install, and keeping both asks xlings for the same package
// twice at two versions — which is not a build that fails, it is a build whose
// environment depends on iteration order. Where both axes name a package, the
// conditional entry wins, because it is the more specific statement; that is
// the rule the flag half of this merge follows by appending after the base
// entries. A disagreement is reported, because it is the one case where the
// author wrote two things and only one of them can happen.
export void merge_conditional_xlings(mcpp::manifest::Manifest& m,
                                     const mcpp::manifest::ConditionalConfig& cc);

// A `[target.<selector>.xlings…]` selector MUST NOT name a RESOLVED layer.
//
// Not a style rule, a schedule one. The five resolved layer keys (`c-abi`,
// `compiler`, …) are answered by dependency RESOLUTION, so a predicate naming
// one is held back to the second merge pass further down — which runs after
// every package's build.mcpp has already run and after the root's tool
// provisioning. An entry admitted there would be declared and never installed,
// and the failure it produces is the worst-shaped one there is: the build
// succeeds and the tool is simply absent.
//
// `accelerator` IS ADMITTED, and used to be refused here with the rest. It is
// not resolved from anything: it is `--accel`, or `[build] accel`, read before
// the first package is looked up, so a payload predicated on it is merged in
// the FIRST pass and installed like any other. Refusing it had a cost paid on
// every build of every project with a device island — the vendor toolkit is
// declared unconditionally or not at all, so a CPU-only build downloaded
// gigabytes for a device it was not compiling for.
//
// Refused rather than deferred, and refused at the earliest point that can
// see the predicate. The gate plane answers the case this shape is reached
// for: `[feature-xlings.<f>]` selects a tool by what the project asked for,
// and a feature is known before anything is provisioned.
export std::optional<std::string>
layer_predicated_xlings_refusal(const mcpp::manifest::Manifest& m);

export void merge_conditional_config(mcpp::manifest::Manifest& m,
                                    const cfgpred::Ctx& ctx);

// Desugar `[build].defines` into `-D<x>` on both C and C++ flag channels.
//
// ORDER (both halves are load-bearing): this must run AFTER every merge that
// contributes `defines` (workspace inheritance, then the package's own table,
// then a matching `[target.'cfg(...)'.build]`), and BEFORE the manifest is
// snapshotted into packages[] / fingerprinted, because that snapshot (not the
// manifest) is what the P1689 scan, the compile edges and compute_fingerprint
// actually read. `makePackageRoot` refuses a manifest whose `defines` are
// still unfolded.
//
// `defines` IS A SET KEYED BY MACRO NAME (SPEC-004 §8). A later entry for a
// name replaces the earlier one in place, so a member that restates an
// inherited `NAME=value` produces one `-DNAME=value` word instead of two
// words and a redefinition diagnostic; an entry `!NAME` removes the name.
// A list is not enough for this, because the compiler resolves a repeated
// `-D` by warning (an error under `-Werror`) and a `-U` written in `cxxflags`
// precedes every folded `-D` and so cannot remove one.
//
// The key covers every `-D<NAME>` word already in the flag lists as well:
// those written in `cflags`/`cxxflags` of the same tables, and those folded
// by an earlier call (the layer-conditional pass calls this again with only
// its own entries). A name this call defines or removes supersedes them, so
// the package's compile lines carry at most one definition per name.
//
// Idempotent: clearing the vector after folding makes repeated calls harmless.
// Both `cflags` and `cxxflags` get the macro; assembly units pick it up for
// free via the -D/-U/-I subset the ninja backend filters out of packageCflags.
// A define is a value, so it enters the flag list as one word
// (`flag_element`): `N="x"` reaches the compiler as `-DN="x"` on every host.
export void fold_build_defines_into_flags(mcpp::manifest::BuildConfig& bc);

// The post-condition of the normalisation pipeline, as the snapshot checks it:
// every `defines` entry has been folded into the flag lists. A non-empty list
// here means a merge ran after the fold, and the entries would otherwise be
// dropped without a diagnostic (#690). Returns the internal-error text, or
// nothing when the manifest may be captured.
export std::optional<std::string>
unfolded_defines_error(const mcpp::manifest::Manifest& m);

// How this invocation may use the global dependency cache.
//
//   Global  read + write  (default)
//   Local   neither — every dependency is compiled inside this project's
//           target/, which is what every build did before the cache worked
//   Off     neither, and this build's target/<triple>/<fp>/ directory is
//           cleared first (a full cold rebuild). Sibling build dirs — other
//           profiles, other targets — are left alone.
//
// `--no-cache` used to be the only switch and it meant "clear the build dir",
// which says nothing about a cache (and its help text claimed all of target/);
// it stays as a deprecated alias for Off.
// Where the resolved toolchain spec came from.
//
// This exists so mcpp can tell its own guesses apart from the user's
// instructions. When a resolved toolchain turns out to be unusable on this
// machine (the motivating case: a Windows default that targets the MSVC ABI
// on a box with no Visual Studio), mcpp may quietly revise a default it
// picked itself — but a spec the user wrote into mcpp.toml must produce an
// error instead. A project that needs the MSVC ABI to link vcpkg-built .lib
// files is worse off with a silent ABI swap than with a failed build.
//
// Deliberately derived from the two config layers that already exist rather
// than persisted: no new field, nothing to keep in sync on disk.
export enum class TcOrigin {
    None,               // nothing resolved yet
    ManifestToolchain,  // mcpp.toml [toolchain]           — user explicit
    TargetSection,      // mcpp.toml [target.X].toolchain  — user explicit
    GlobalDefault,      // `mcpp toolchain default`        — user explicit
    TargetPin,          // triple.cppm vocabulary convention
    GraphRequirement,   // `requires = ["mcpp:compiler=…"]` in the graph
    FirstRun,           // chosen and persisted by this very invocation
};

// `GlobalDefault` IS DELIBERATELY NOT LISTED, AND THE REASON IS A MEASURED
// REGRESSION RATHER THAN A JUDGEMENT ABOUT WHOSE OPINION COUNTS.
//
// A target row's pin does not name a preferred compiler. It names the payload
// that supplies THAT TARGET'S C library — the mingw payload for
// `x86_64-windows-gnu`, the musl-gcc payload for `*-linux-musl`. Whether the
// user's own default can serve the target instead depends on whether something
// ELSE supplies the target side, and that is knowable only after the dependency
// graph is resolved, which is after this line.
//
// Making the global default outrank the pin was tried and measured: a project
// with no dependencies, a global default of `llvm@22.1.8` and
// `--target x86_64-windows-gnu` stopped building, because clang alone carries no
// C runtime for that target while the payload the row names does. That is a
// working build turned into a failing one by an upgrade.
//
// What the user actually loses is ergonomics, and that is addressed where it is
// visible: when the pin replaces a default the user wrote down, the status line
// SAYS SO and names the one-line override. The structural fix is to defer the
// pin the way the target side itself was deferred — resolve it after the graph,
// where the question it answers has an answer.
export inline bool tc_origin_is_user_explicit(TcOrigin o) {
    return o == TcOrigin::ManifestToolchain || o == TcOrigin::TargetSection;
}

// MAY A BUILD THAT RESOLVED THIS WAY WRITE THE MACHINE'S DEFAULT?
//
// `GraphRequirement` is the one origin that must not: it is a property of a
// package this project depends on, not of this machine. Two branches persist a
// default — the Windows first-run diversion, whose condition is
// `tcSpec.has_value()`, and the MSVC repair, whose gate is "mcpp chose this
// itself" — and a compiler chosen by `requires = ["mcpp:compiler=…"]` satisfies
// both. Measured against the design rather than a run, because it needs a
// Windows box with no toolchain: a bare machine building ONE llvm-requiring
// project would have handed llvm to every later project that asked for nothing.
//
// NAMED RATHER THAN SPELLED INLINE AT EACH SITE. There are two today; the
// third would be written by someone who never read this note, and a predicate
// with a name is something they can find.
export inline bool tc_origin_may_persist(TcOrigin o) {
    return o != TcOrigin::GraphRequirement;
}

// How a resolution came about, for the status line. A convention that replaced
// nothing needs no explanation; one that replaced a user's stated preference is
// a decision the user did not make and must be told about.
export constexpr std::string_view tc_origin_name(TcOrigin o) {
    switch (o) {
        case TcOrigin::ManifestToolchain: return "[toolchain] in mcpp.toml";
        case TcOrigin::TargetSection:     return "[target.<triple>] in mcpp.toml";
        case TcOrigin::GlobalDefault:     return "your default";
        case TcOrigin::TargetPin:         return "target default";
        case TcOrigin::GraphRequirement:  return "required by the dependency graph";
        case TcOrigin::FirstRun:          return "first-run default";
        case TcOrigin::None:              break;
    }
    return {};
}

// What to tell a user whose build targets the MSVC ABI on a machine that
// cannot serve it. Two shapes, because the two states need different fixes:
//
//   • cl.exe was found but the Windows SDK was not — a half-installed VS.
//     Point at the missing SDK component; switching toolchains would be an
//     over-correction for someone who clearly wants MSVC.
//   • nothing usable at all — the bare-Windows case. Lead with the MinGW-w64
//     route, which needs no Visual Studio and is already a verified target,
//     and keep the "install the C++ workload" option second.
export std::string msvc_unavailable_guidance(const mcpp::toolchain::Toolchain& tc);

export enum class CacheMode { Global, Local, Off };

export std::optional<CacheMode> parse_cache_mode(std::string_view v);

export std::string_view cache_mode_name(CacheMode m);

// A condition a planning pass reports instead of acting on (plan_only): the
// code is stable and the message is for people. Emitted as diagnostics by the
// command that asked for the plan, at the severity carried here — most notes
// are warnings the document is still complete despite (a lock that would
// change, a generated file left unmaterialized); a build program whose run
// failed under `plan_only` (#699 item 2, E3) is an error, because the sets it
// would have shaped are described without its directives.
export struct PlanNote {
    std::string code;
    std::string message;
    mcpp::wire::Severity severity = mcpp::wire::Severity::Warning;
    // The absolute, native path of the file the condition is about (a
    // package's `build.mcpp`), empty when the note names no file.
    // `mcpp.build.build_database::render` rewrites it to the workspace-
    // relative form every other `path` in the document uses.
    std::string path;
};

export struct BuildContext {
    // THE PER-MACHINE JOB DEFAULT, carried so it is read once.
    //
    // `[build] default_jobs` in `$MCPP_HOME/config.toml` is the machine's
    // answer to "how many at once". `prepare_build` resolves it into the build
    // schedule itself; this field exists for the SECOND reader --
    // `mcpp test`'s runner concurrency (execute.cppm) -- which calls
    // `resolve_jobs` again after this function has returned. Recorded rather
    // than re-read, because a second `load_or_init` there would be a second
    // parser of one file, and because the two readers must not be able to
    // disagree about the machine.
    int                             globalDefaultJobs = 0;
    // --strict: degradations reported through mcpp::diag become errors.
    // Carried on the context because the build's degradations are discovered
    // during backend emission, i.e. after prepare_build has returned — the
    // single place that settles the policy is run_build_plan (execute.cppm).
    bool                            strict = false;
    mcpp::manifest::Manifest        manifest;
    mcpp::toolchain::Toolchain      tc;
    mcpp::toolchain::Fingerprint    fp;
    mcpp::xlings::runtime::RuntimeSelection runtimeSelection;
    mcpp::platform::runtime::RuntimeBinding runtimeBinding;
    std::filesystem::path           projectRoot;
    // THE SOURCE TREES THIS BUILD READ THAT ARE NOT UNDER `projectRoot`.
    //
    // A `path` dependency — which is what every workspace member is to its
    // siblings — contributes translation units from a directory the fast path
    // has no way to name. `sources_newer_than` sweeps the project being built,
    // so a NEW FILE appearing in such a tree is invisible to it: ninja cannot
    // report an edge that was never emitted, and the fast path replays a
    // build.ninja that predates the file. Measured before this field existed:
    // `mcpp build` printed `Finished dev in 0.00s` and the module was never
    // compiled.
    //
    // Recorded rather than re-derived, because the authoritative answer is
    // which packages this build ACTUALLY read from source — the fast path
    // cannot resolve dependencies without becoming prepare_build, and a second
    // derivation would drift from the first exactly when a resolution rule
    // changes. Written into `.build_cache`; see BuildCacheEntry::depSourceRoots.
    std::vector<std::filesystem::path> depSourceRoots;
    // `<payload>/bin` and then `<payload>` of every installed `[xlings] deps`
    // payload of the runtime-owner manifest, in declaration order (#544); the
    // pair comes from runner_lookup::payload_search_dirs. Read by
    // choose_runner's lookup (mcpp.build.runner_lookup) so a runner may name
    // a program the project declared without writing the payload's
    // home-and-version path into the manifest. Computed by the same
    // resolution `fillXpkgDirs` uses for build programs; a payload that is
    // declared but not installed contributes nothing, and the lookup then
    // continues to PATH.
    std::vector<std::filesystem::path> xlingsDepBinDirs;
    // The payload directory of every `[xlings]` address resolved above, for
    // the fast path's presence check (#716). See BuildCacheEntry::xlingsPayloads.
    std::vector<std::filesystem::path> xlingsPayloads;
    // True when the graph declared a `when = "run"` tool that THIS invocation
    // did not provision, because it was not going to execute anything. The
    // build cache records it so `mcpp run`'s fast path declines an entry a
    // plain `mcpp build` wrote — see BuildCacheEntry::runTierPending.
    bool runTierPending = false;
    // What `--features` asked for, verbatim. Carried so the build cache entry
    // can record the set its artefacts were built with — the output directory
    // is keyed on a fingerprint that includes the features and the entry was
    // not, which let a plain build serve a featured artefact.
    std::string activeFeatureRequest;
    std::filesystem::path           outputDir;
    std::filesystem::path           stdBmi;
    std::filesystem::path           stdObject;
    // plan_only: what the std module build WOULD be (sources are on the
    // toolchain), set when the graph imports std. A build compiles it instead
    // and leaves this empty.
    std::optional<mcpp::toolchain::StdModuleDescription> stdModule;
    // The packages this build read from an editable source tree: the root, and
    // every package whose root is neither in a store nor in a hash-addressed git
    // checkout (the same test depSourceRoots applies). `sources` are the
    // package's source globs, relative to `root`. Read by the build database for
    // the inputs it lists.
    struct SourcePackage {
        std::string                 name;
        std::filesystem::path       root;
        std::vector<std::string>    sources;
    };
    std::vector<SourcePackage>      sourcePackages;
    std::vector<PlanNote>           planNotes;
    mcpp::build::BuildPlan          plan;
    // The scanned module graph. Only `mcpp pack` reads it — see the note at
    // the assignment for why the plan cannot answer its question.
    mcpp::modgraph::Graph           graph;
    // Resolved profile name (resolve_profile_name). Carried so run_build_plan
    // can record it in .build_cache — without it the fast path cannot tell
    // whether a cached build.ninja was generated for the profile being asked
    // for — and so `Finished <profile>` stops being a hardcoded "release".
    std::string                     profile;
    // WHY THIS COMPILER — carried so the QUERY can answer it too.
    //
    // A build says so on its status line. `mcpp why toolchain --format json`
    // exists precisely to answer "what would this resolve to, and why", and a
    // consumer that had to parse the prose to learn that a dependency chose the
    // compiler would be doing the substring matching the machine interface was
    // introduced to remove.
    struct CompilerChoice {
        std::string origin;      // tc_origin_name(): who decided
        std::string requiredBy;  // the package, when the graph decided
        std::string replaced;    // the spec displaced, when one was
    };
    CompilerChoice                  compilerChoice;
    // Resolved global-cache mode. Read side is honored in prepare_build; write
    // side in run_build_plan.
    CacheMode                       cacheMode = CacheMode::Global;

    // M3.2 BMI cache: deps that did NOT hit cache and therefore need
    // populate_from(...) AFTER backend.build succeeds.
    struct CacheTask {
        mcpp::bmi_cache::CacheKey       key;
        mcpp::bmi_cache::DepArtifacts   artifacts;
    };
    std::vector<CacheTask>          depsToPopulate;

    // Deps that DID hit the global cache, and how many compile units each one
    // spared. run_build_plan reports the count so the "Cached" line cannot be
    // true-looking and empty at the same time.
    struct CachedDep {
        std::string name;
        std::string version;
        std::size_t units = 0;
    };
    std::vector<CachedDep>          cachedDeps;

    // What the dependency walk actually RESOLVED, keyed by the root manifest's
    // dependency map key. The "Compiling <dep> v<version>" banner used to read
    // `manifest.dependencies[...].version` — the constraint as authored — so a
    // caret dep announced itself as `v^1.92.8` (mcpp#363). The resolution result
    // already existed inside prepare_build; the banner and mcpp.lock were simply
    // reading the input instead of the output. Both now read this.
    std::map<std::string, std::string> resolvedVersions;
    // The selected members of a workspace plan (workspace design 2026-09-29
    // §15), in selection order: each member's manifest (its hooks, its
    // `[pack]`, its identity), its root, and the directory below `bin/` its
    // products are in. Empty outside a workspace plan, whose subject is
    // `manifest` itself.
    struct WorkspaceMember {
        std::string                 name;        // qualified package name
        std::string                 memberPath;  // as `[workspace] members` writes it
        std::filesystem::path       root;
        std::string                 productDir;  // empty: `bin/` itself
        mcpp::manifest::Manifest    manifest;
    };
    std::vector<WorkspaceMember>    workspaceMembers;
    // The command's selection and this plan's group, each the member paths
    // joined by a unit separator; empty outside a workspace. A fast-path
    // record is written, and matched, per selection and group (§15).
    std::string                     workspaceRequest;
    std::string                     workspaceGroup;
    // Which packed member each package acts for, in a plan of several
    // selected members (member selection design 2026-09-30, K1): for every
    // package of the plan, the selected members whose dependency closure
    // reaches it, in discovery order, a member reaching itself. Empty for a
    // plan of one member or none, where the plan's one subject is what every
    // package acts for. Read through `pack_owner`.
    std::map<std::string, std::vector<std::string>> packReach;
    // The packages that declared each pack format with
    // `mcpp::provides_pack_format`, by format, as qualified package names.
    // Collected on every pass, with `plan.providedPackFormats`, which it
    // refines: the plan knows the set of formats and this knows who provides
    // each.
    std::map<std::string, std::vector<std::string>> packFormatProviders;
};

// The selected member a package acts for in a packaging pass: the package
// itself when it is one of the members, the one member that reaches it when
// exactly one does, and none (empty) when several do. `reach` is the package's
// entry of `BuildContext::packReach`. Written once and read by prepare, which
// hands each program the stage of the member it acts for, and by `mcpp pack`,
// which attributes each action a program submitted to a member.
export inline std::string pack_owner(std::string_view package,
                                     const std::vector<std::string>& reach) {
    if (std::ranges::find(reach, package) != reach.end()) return std::string(package);
    return reach.size() == 1 ? reach.front() : std::string{};
}

// The ONE cache-mode resolver, for the same reason resolve_profile_name exists:
// execute.cppm's fast paths deliberately skip prepare_build, so they need to
// settle the mode from the same rule. Pure in (manifest, override, environment).
//
// `--cache` on the command line already bypasses the fast path, so the override
// argument is empty there; it is threaded anyway so there is exactly one place
// where precedence is written down.
//
// Precedence: --cache > MCPP_BUILD_CACHE > [build] cache > global. An
// unparseable value falls through to the next source rather than silently
// meaning "global" — see prepare_build, which also reports it.
// A plan of one workspace member, read as that member (workspace design
// 2026-09-29 §15): the member's manifest and root become the context's, and
// its link group's runtime fields become the plan's, so a reader that asks
// "the package being built" -- `mcpp pack`, the runtime closure it stages --
// is answered about the member rather than about the virtual root. Called
// after the plan's graph is written; the plan's own fields are exchanged, not
// lost. A context of any other shape is left as it is.
export void focus_on_member(BuildContext& ctx);

// Reads `fn` with the plan describing one workspace member (workspace design
// 2026-09-29 §15): the member's link group is exchanged into the plan's own
// fields for the call, so what a member's tests run against, or what its
// package is made of -- its runtime directories, the files its runtime needs --
// is its closure's, and no other member's; and the member's manifest and root
// become the context's, so a reader that asks "the package being built" is
// answered about the member. `owner` is the member's qualified package name.
// Outside a workspace plan, for an empty `owner` and for a name the plan does
// not hold, the plan is read as it is.
//
// Every exchange is undone before the call returns, also when `fn` throws: a
// drive emits the plan and must see its own fields, and the next member is
// read from the plan as the group left it. The exchange is of fields, not of
// copies, so a member's view costs no more than a swap. This is
// `focus_on_member` for a plan of several members, and scoped.
export template <class F>
void with_member(BuildContext& ctx, std::string_view owner, F&& fn) {
    BuildPlan::LinkGroup* group = nullptr;
    BuildContext::WorkspaceMember* member = nullptr;
    if (!owner.empty()) {
        for (auto& g : ctx.plan.linkGroups)
            if (!g.linkOnly && g.member == owner) { group = &g; break; }
        for (auto& m : ctx.workspaceMembers)
            if (m.name == owner) { member = &m; break; }
    }
    if (!group && !member) { fn(); return; }
    struct Exchange {
        BuildContext& ctx;
        BuildPlan::LinkGroup* group;
        BuildContext::WorkspaceMember* member;
        void flip() {
            if (group) swap_link_group(ctx.plan, *group);
            if (member) {
                std::swap(ctx.manifest, member->manifest);
                std::swap(ctx.projectRoot, member->root);
            }
        }
        Exchange(BuildContext& c, BuildPlan::LinkGroup* g, BuildContext::WorkspaceMember* m)
            : ctx(c), group(g), member(m) { flip(); }
        ~Exchange() { flip(); }
    } exchange{ctx, group, member};
    fn();
}

export CacheMode resolve_cache_mode(const mcpp::manifest::Manifest& m,
                                    std::string_view override_mode);

// The ONE profile-name resolver. Shared with execute.cppm's fast paths:
// they deliberately skip prepare_build, so before this existed they had no
// idea which profile the request meant — and `.build_cache` keyed entries by
// target triple alone. Net effect: `mcpp build --release` followed by a bare
// `mcpp build` reported success in 0.00s and left the RELEASE artifacts in
// place. The rule is pure (manifest + one override string), so both sides can
// evaluate it without resolving a toolchain or scanning the module graph.
//
// Precedence: --profile/--release/--dev > [build].default-profile > `fallback`.
// The global default is "dev" (-O0 -g) per the dominant convention
// (Cargo/Meson/CMake/Zig/Bazel/MSBuild all default to debug).
//
// `fallback` exists for ONE caller: `mcpp pack`, where the artifact leaves this
// machine and an unoptimized build with the publisher's absolute source paths
// in it is never what was meant. It changes the LAST step only, so a manifest
// that states `[build] default-profile` still decides — packaging an artifact
// with different flags than `mcpp build` produces would be its own surprise.
// Adding a parameter here rather than a second resolver keeps the precedence
// rule in one function, which is why this function exists at all.
export std::string resolve_profile_name(const mcpp::manifest::Manifest& m,
                                        std::string_view override_name,
                                        std::string_view fallback = "dev");

// THE OVERRIDE NAME THE COMMAND LINE STATES, OR "" FOR NONE (#649 E9).
//
// `--profile NAME` > `--release` > `--dev`, and one function for every verb
// that takes the spellings (`build`, `run`, `test`, `emit build-database`,
// `pack`). The rule used to be written twice and the copies disagreed:
// `mcpp build --profile dev --release` built `dev` while `mcpp run` given the
// same line built `release`. Its result is `resolve_profile_name`'s
// `override_name`.
export std::string profile_override_from_flags(std::string_view profileOption,
                                               bool release, bool dev);

// Command-level overrides (--target / --static).
// Empty defaults preserve pre-existing behaviour exactly.
export struct BuildOverrides {
    // Where the package being built LIVES (its mcpp.toml). Empty = walk up from
    // the process cwd, which is what every user-facing invocation does. Set by
    // the tool-provisioning pass, which builds a package that lives in the
    // registry rather than under the cwd.
    std::filesystem::path project_root;
    // Where mcpp WRITES. Empty = the project root, which is the historical
    // (and for a normal build, correct) behaviour.
    //
    // The two are separate because a registry package root is shared across
    // projects and may be read-only — build_program.cppm has said so in a
    // comment since G2, and until now nothing could honour it for anything
    // bigger than build.mcpp's own scratch dir. Splitting "source" from "work"
    // is what lets mcpp build such a package at all.
    //
    // EVERYTHING derived from it moves together: target/, mcpp.lock,
    // compile_commands.json, .mcpp/, and build.mcpp's artifact dir. Moving
    // only some would be worse than moving none — a half-redirected build
    // writes into the shared root anyway, just less visibly.
    std::filesystem::path work_dir;
    // PLANNING TO DESCRIBE, NOT TO BUILD (`mcpp emit build-database`).
    //
    // With `work_dir` pointed outside the project, three things still reached
    // it or ran a compiler, and this switch settles each: the std module is
    // described (mcpp::toolchain::describe_std_module) instead of compiled; the
    // root package's `[build] generated_files`, which are sources and live in
    // the source tree, are compared instead of written, and a missing or stale
    // one is recorded in BuildContext::planNotes; and mcpp.lock is READ from the
    // project root, as the resolution input it is, while the lock this planning
    // produces is written under `work_dir`. Build programs still run.
    bool        plan_only = false;
    // #355 tool provisioning re-enters prepare_build for the tool package. A
    // tool package's own build.mcpp may legitimately want another tool (gRPC's
    // wants protoc), so the depth cannot be 1 — but an unbounded chain is a
    // bug, and hanging is a worse diagnostic than a named cycle.
    int         tool_depth = 0;
    // The request chain, for that diagnostic. "root → grpc:grpc_cpp_plugin → …"
    std::string tool_chain;
    // The (package source, tool) pairs being built by the enclosing sub-builds,
    // outermost first. A request for one of them is the tool's own build asking
    // for itself, refused at its first repetition (#649 E6).
    std::vector<std::string> tool_chain_sources;
    // The toolchain spec a host-tool sub-build uses, decided by the build that
    // requested the tool and recorded in the tool's store key (#710). Beats
    // every other source, `--toolchain` included, because the requesting build
    // already took `--toolchain` into account when it decided. Empty for every
    // user-facing invocation.
    std::string toolchain;
    // Use THIS manifest instead of reading `<project_root>/mcpp.toml`.
    //
    // Required for a `compat`-style registry package (Form B), which ships no
    // mcpp.toml at all — its manifest is synthesized from the `.lua`
    // descriptor during resolution. Without this the tool sub-build could only
    // ever handle packages that carry their own manifest (Form A), which
    // excludes most of the index, protobuf among them.
    //
    // Must be the PRISTINE manifest, before feature activation: the sub-build
    // activates its own feature set, and starting from an already-activated
    // copy would fold the same feature sources in twice.
    // A shared_ptr rather than an optional<Manifest>: BuildOverrides is an
    // EXPORTED struct, and embedding a large value type in the module
    // interface made GCC fail to write the cluster at all
    // ('failed to read compiled module cluster ...: Bad file data' when
    // mcpp.build.execute imported it). A pointer keeps the exported layout
    // trivial, and it also avoids copying the manifest per tool build.
    std::shared_ptr<const mcpp::manifest::Manifest> preloaded_manifest;
    // Nested source/tool builds inherit the consumer root's local development
    // OS. A dependency's own [xlings].subos is never consulted or propagated.
    std::shared_ptr<const mcpp::xlings::runtime::RuntimeSelection>
        inherited_runtime_selection;
    std::shared_ptr<const mcpp::platform::runtime::RuntimeBinding>
        inherited_runtime_binding;
    std::string target_triple;       // empty = host triple, fall through to [toolchain]
    // --accel: the device backends and architectures this build targets, in
    // the wire form mcpp.pack.abi_tag reads. Overrides `[build] accel`, the
    // same relationship --target has with [toolchain].
    std::string accel;
    bool        force_static = false; // --static (or implied by musl target)
    std::string package_filter;      // -p <name>: only build this workspace member
    // The workspace members this plan builds, as written in `[workspace]
    // members` ("." is a rooted workspace's own package), all of one
    // configuration group (workspace design 2026-09-29 §15). Empty: the member
    // is selected from `package_filter` or the command's directory.
    std::vector<std::string> workspace_members;
    // Every member the COMMAND selected, across its configuration groups: the
    // request a fast-path record names. Empty: `workspace_members`.
    std::vector<std::string> workspace_request;
    // The test targets each member of `workspace_members` receives, by member
    // path: what `extraTargets` is for a plan of one member, for a plan of
    // several (`--configure-only`, `mcpp emit build-database`).
    std::map<std::string, std::vector<mcpp::manifest::Target>> member_targets;
    // --profile <name>. Empty = fall through to `[build] default-profile`, then
    // to `profile_fallback` below, whose own default is "dev". The comment here
    // said "release" for as long as `mcpp build --help` did, and neither had
    // been true since the global default moved (see resolve_profile_name and
    // tests/e2e/87_build_default_profile.sh).
    std::string profile;
    // What `resolve_profile_name` falls back to when neither the command line
    // nor `[build] default-profile` says. Empty = "dev", which is every
    // interactive command. `mcpp pack` sets "release": see resolve_profile_name.
    std::string profile_fallback;
    std::string features;            // --features a,b,c (root package activation)
    bool        strict = false;      // --strict: schema warnings become errors
    std::string capabilities;        // --cap blas=openblas,lapack=mkl (provider pins)
    std::string cache_mode;          // --cache global|local|off ("" = unset)
    // Whether the caller intends to EXECUTE what it builds, which is what
    // decides the `when = "run"` tool tier. `mcpp run` and `mcpp test` set it;
    // `mcpp build`, `mcpp pack` and every internal sub-build do not.
    //
    // A SEPARATE FLAG FROM `includeDevDeps`, THOUGH `mcpp test` SETS BOTH.
    // One says which PACKAGES enter the graph, the other which TOOLS are
    // installed, and `mcpp run` needs the second without the first.
    bool        will_run = false;
    // ── The packaging pass, when this prepare is one (mcpp 2026.9.11.1+) ────
    //
    // `mcpp pack --format <name>` prepares TWICE, and these two fields are the
    // whole difference between the passes. The first sets neither: build
    // programs run, declare the formats they provide, and submit no dist
    // action because none was asked for. The second sets both, after the link
    // and after staging, so the claiming member submits an action whose input
    // is a directory that by then exists.
    //
    // NEITHER VALUE IS DERIVED HERE, AND THAT IS THE POINT. A member's stage
    // directory is a function of the package name, the version, the resolved
    // triple and the mode, and the resolved triple is not known until a
    // prepare has run. Computing it a second time before prepare -- from the
    // host triple, say -- is the shape where two derivations of one value agree
    // on every machine the author has. Both are read out of what the first pass
    // and `make_plan` already answered.
    std::string           pack_format;
    // What a packaging pass knows of one packed member: where its tree is
    // staged, and what the staging resolved for the programs that act for it.
    struct PackStage {
        // The staged tree, absolute. Empty when staging was refused, which is
        // what makes `${mcpp.stage_dir}` refuse with `reason` attached rather
        // than expand to a directory that does not exist.
        std::filesystem::path dir;
        // WHY THERE IS NO STAGED TREE, when there is none and a format was
        // still requested. Empty otherwise.
        //
        // A dispatched format does not require the built-in bundling to have
        // succeeded -- see the note in `mcpp.pack.pipeline`. When it did not,
        // the reason travels here so `${mcpp.stage_dir}`'s refusal can name it
        // instead of saying only that the placeholder is unavailable. A member
        // author reading "this build is not packaging" for a build that
        // plainly is would be sent looking in the wrong place.
        std::string           reason;
        // #649 E5: the strip decision and the debug-symbol directory that
        // member's staging resolved, "1" or "0" and absolute. A member that
        // stages libraries of its own reads them through `mcpp::pack_strip()`
        // and `mcpp::pack_debug_symbols_dir()`, so `--no-strip` reaches its
        // files as it reaches the engine's.
        std::string           strip;
        std::filesystem::path debugSymbolsDir;
    };
    // The packed members' stages, by qualified package name. One entry is what a
    // pack of one member sets, and then every program of the plan receives it,
    // as the plan's only package being packed. With several entries (`mcpp
    // pack` over several members, member selection design 2026-09-30, K1) a
    // program receives the stage of the member it acts for: its own when it is
    // a packed member, and otherwise the one packed member whose dependency
    // closure reaches it. A package that several packed members reach acts for
    // none of them, and receives no stage at all, which is what lets one run of
    // its program serve every member.
    std::map<std::string, PackStage> pack_stages;
};

// ── git dependency helpers ──────────────────────────────────────────────────

// ── Tool tiers: which of a manifest's declared packages this verb needs ─────
//
// THE AXIS PACKAGE DEPENDENCIES HAVE HAD SINCE THE BEGINNING, AND TOOLS
// DID NOT.
//
// A board-support package names an emulator (needed to run) and a debug probe
// (needed to reach real hardware). Before this, declaring either installed
// both, for everyone, on every `mcpp build` — including a consumer who only
// wanted the library to compile. Packages have `[dependencies]`,
// `[build-dependencies]` and `[dev-dependencies]`; tools had one list.
//
// The tier is written on the ENTRY (`when = "run"`), not as a second table:
// `[xlings.workspace]` was made the one table on purpose, and the entry-level
// spelling is the one `[dependencies]` already uses for the same kind of
// refinement.
//
// `Dev` IS THE ONLY TIER THAT DOES NOT PROPAGATE. It means "when the package
// that declared it is itself being developed", so `isRoot` decides it. Every
// other tier reaches a consumer, which is the whole point of a board package
// knowing its own machine.
// Exported (unlike most of this file's helpers) because PrepareState, in the
// implementation partition `:state`, holds one and needs the type visible
// through `import mcpp.build.prepare;` -- a partition sees only what its
// imports export, module-linkage is not enough across that boundary.
export enum class ToolPurpose { Build, Run };

// The toolchain a host tool's package chose for itself, read the way its own
// build reads it (#710): the package's manifest with the root-position keys of
// the workspace that lists it (`inherit_workspace_root_position`), then its
// host row's `[target.<host>] toolchain`, then `[toolchain]`. nullopt when none
// names one. Exported for its unit test
// (tests/unit/test_workspace_inheritance.cpp).
export std::optional<std::string>
host_tool_declared_toolchain(const mcpp::manifest::Manifest& tool,
                             const std::filesystem::path& toolRoot,
                             std::string_view platform);

// The pure helpers the phases share, exported for tests/unit/test_prepare_helpers.cpp;
// their definitions and design notes are in src/build/prepare/config.cpp and
// src/build/prepare/fetch.cpp.
export std::vector<std::string> previous_release_words(std::string element, bool define);
export std::string_view define_name(std::string_view entry);
export std::vector<std::string> feature_request_tokens(std::string_view s);
export std::vector<std::string> parse_feature_request(std::string_view s);
export std::vector<std::string> feature_forward_request_tokens(std::string_view s);
export bool is_local_git_remote(std::string_view url);

// PrepareState (the state every phase reads and writes) and the phase
// declarations live in the implementation partition `:state` — see the
// file-header comment above for why this file imports no partition at all,
// interface or implementation, and therefore cannot name PrepareState here.
// prepare_build's own definition is driver.cpp; this is only its declaration,
// carrying the default arguments (they belong on exactly one declaration,
// and this is the one every caller sees).
export std::expected<BuildContext, std::string>
prepare_build(bool print_fingerprint, bool includeDevDeps = false,
              std::vector<mcpp::manifest::Target> extraTargets = {},
              BuildOverrides overrides = {});

// The PlanNotes a failed call recorded before the phase that failed it.
//
// On success, `prepare_build` copies `PrepareState::planNotes` into
// `BuildContext::planNotes` (phase13_finish) — but on failure it returns only
// `r.error()`, a plain string, and the `PrepareState` that held the notes is a
// local of `prepare_build` and is gone the moment it returns. A note recorded
// by an earlier phase (`MCPP_BUILD_DATABASE_PROGRAM_FAILED`, say) was
// therefore lost on every failure of a later phase, not only the one that
// motivated this (design 2026-09-27 §4.2, mcpp#724 side finding A, fix item
// 2): under `emit build-database`'s `plan_only`, a member the caller could
// otherwise describe the ordinary way (R5.2) instead reported nothing but the
// later phase's own message.
//
// Same per-run-sink discipline as `mcpp::build::refusal` (refusal.cppm), and
// for the same reason: widening `prepare_build`'s return type would touch
// every caller of `.error()` to carry something only the failure path of one
// caller (`emit`) reads. Written immediately before prepare_build's own
// failing return, from the state that failure saw; read by the caller that
// turns that failure into diagnostics. `take` reads and clears, so neither a
// later failure of the SAME call nor a later, unrelated call inherits a stale
// set of notes.
export std::vector<PlanNote> take_notes_on_failure();

// After a drive of `ctx`'s plan succeeded: the global cache receives the
// dependencies that the drive compiled because no entry served them. Called by
// every command that builds a plan (`build`, `test`, `pack`); a pack once left
// the cache unfilled, so the next build of another configuration compiled the
// same dependencies again (pack drive and selection design 2026-10-01, A4).
// prepare_build records no such dependency under `--cache=local|off`; the mode
// is checked here as well, so the write side states its own condition.
export void populate_dependency_cache(BuildContext& ctx);

} // namespace mcpp::build
