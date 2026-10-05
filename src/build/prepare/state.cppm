// mcpp.build.prepare:state -- the implementation partition holding
// PrepareState and the phase functions' declarations. Imported only by
// this module's own implementation units (driver.cpp and every
// phaseN.cpp): never by the primary interface, which must not import any
// partition (see the file-header comment in prepare.cppm for why -- a
// GCC 16.1 constraint). An implementation partition rather than an
// interface partition for the same reason.
module mcpp.build.prepare:state;

// A partition sees only what it explicitly imports -- unlike an
// implementation unit of this module, it does not implicitly see the
// primary interface. PrepareState needs BuildOverrides, BuildContext,
// PlanNote, CacheMode, TcOrigin and ToolPurpose, all exported from there.
import mcpp.build.prepare;

import mcpp.build.prepare_inputs;

import std;
import mcpp.targetside;
import mcpp.build.version_floor;
import mcpp.platform.axis;
import mcpp.libs.json;
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
import mcpp.toolchain.triple;
import mcpp.build.linkage_form;   // #519 — which form each dependency takes
import mcpp.build.plan;
import mcpp.build.flags;          // compute_flags — the per-role contracts (#418)
import mcpp.build.graph_shape;  // #407: the graph says which mode wrote it
import mcpp.build.build_program;
import mcpp.build.directives;   // directive table: mark / fold_private_tail
import mcpp.build.dep_graph;    // queries over the resolved edge graph
import mcpp.build.provisions;   // #359 build-time provisions: table + propagation
import mcpp.build.backend;      // BuildOptions for the tool sub-build
import mcpp.build.ninja;        // make_ninja_backend — driving that sub-build
import mcpp.config;
import mcpp.xlings;
import mcpp.xlings.address_set;   // mcpp#755: override checks reuse the unification's claims
import mcpp.xlings.runtime_selection;
import mcpp.runtime.binding;
import mcpp.toolchain.post_install;
import mcpp.platform;
import mcpp.pm.resolver;
import mcpp.pm.index_spec;
import mcpp.pm.index_contract;
import mcpp.pm.index_route;
import mcpp.pm.index_refresh;
import mcpp.pm.mangle;
import mcpp.pm.dep_spec;
import mcpp.pm.dependency_selector;
import mcpp.pm.lock_io;
import mcpp.project;

namespace mcpp::build {


// The platform key used to read `[toolchain].<platform>` — a pure constant,
// so it lives at file scope rather than in PrepareState: nothing about it
// depends on any phase's state, and every phase that reads it (P1's
// `tcSpecSource` included) can just name it.
constexpr std::string_view kCurrentPlatform = mcpp::platform::name;

// Namespace aliases used across several phases (P4's own declarations and
// P7/P9's reuse of its helpers); hoisted here so every phase function sees
// them under the short spelling instead of each repeating the full path.
namespace prov = mcpp::build::provisions;
namespace dg = mcpp::build::dep_graph;

// Sentinel for "the consumer is the main package" (no dep_manifests entry).
// A pure constant; used by both halves of the P4 split (graph_load, graph).
constexpr std::size_t kMainConsumer = static_cast<std::size_t>(-1);

    struct DepCacheIdentity {
        std::string indexName;
        std::string packageName;
        std::string version;
        // "version" | "path" | "git". Only "version" is cacheable: an index
        // package's payload lives in the immutable xpkgs store under a
        // version-keyed directory, so name@version identifies its sources.
        // Path and git checkouts can change under an unchanged identity.
        std::string sourceKind;
        // What identifies the SOURCES when the version does not: the resolved
        // commit for `git`, the package root for `path`, empty for an index
        // package. Read by the tool store, whose key must hold everything
        // that can change a built tool's bytes (#630, item 6).
        std::string sourceRef;
    };

    struct GitLockIdentity {
        std::string source;
        std::string hash;
    };

    struct DependencyEdge {
        std::size_t consumerPackageIndex = 0;
        std::size_t dependencyPackageIndex = 0;
        mcpp::modgraph::DependencyVisibility visibility =
            mcpp::modgraph::DependencyVisibility::Public;
        // #242/#243: the per-edge feature request that THIS consumer made of
        // THIS dependency. Feature activation must consume these off the edge
        // graph (union over all incoming edges) rather than re-scanning only
        // the root manifest's direct deps — otherwise a transitive dep's
        // requested features and its consumer's `default-features = false` are
        // silently dropped (resolution honors them per-edge; activation did not).
        std::vector<std::string> requestedFeatures;
        bool defaultFeatures = true;
        // #355: HOST tools this consumer asked the dependency for. Aggregated
        // off the edge graph exactly like requestedFeatures — a transitive
        // consumer's request must not be silently dropped, which is the
        // #242/#243 failure shape.
        std::vector<std::string> requestedTools;
        // mcpp#711: the dependency's programs this consumer ships, built in
        // THIS plan for its target. A package asked for them is scanned and
        // configured here even when every target it declares is a program.
        std::vector<std::string> requestedArtifacts;
        // #355 step 5 / #359: does this edge ask for the dependency's lib-root
        // interface as a HOST module, and does it hand its build-time
        // provisions on to this consumer's own consumers?
        bool hostModule = false;
        bool reexport = false;
        // Did this edge come from `[build-dependencies]`? Such an edge serves
        // the BUILD and never the target, and the property is inherited by
        // everything reachable through it. It is a property of the edge and
        // not of the package: the same package may be an ordinary dependency
        // of someone else in the same build, and then it does reach the
        // target.
        bool buildOnly = false;
    };

    struct GraphRequest {
        std::size_t consumerPackageIndex = 0;
        std::size_t dependencyPackageIndex = 0;
        std::string key;        // the dependency key as the requester wrote it
        std::string table;      // `DependencySpec::declaredIn`
    };

    struct DependencyLinkForm {
        mcpp::build::linkage_form::PackageFacts facts;
        mcpp::build::linkage_form::Resolution   answer;
        // The package has a library form to report: a package of programs or
        // rules has none, and is neither recorded nor offered to a program.
        bool recorded = false;
    };

    struct ResolvedKey {
        std::string ns;
        std::string shortName;
        auto operator<=>(const ResolvedKey&) const = default;
    };

    struct ResolvedRecord {
        std::string version;            // empty for path/git deps
        std::string constraint;         // AND-combined original constraints (version src only)
        std::string requestedBy;        // human-readable for error messages
        std::string source;             // "version" | "path" | "git" — for type-clash check
        // The declaration's identity beyond `source`, so a SECOND declaration
        // of the same (ns, name) can be compared for "the same reference"
        // rather than merely "the same kind". `git`: "<url>#<refKind>=<ref>",
        // from the DECLARED ref (never the resolved commit — comparing two
        // branch names must not need a network round trip to decide whether
        // they conflict). `path`: the canonical absolute directory. `version`:
        // the original constraint string ("*" for none). See the
        // `dependency/source-override` decision at the resolve hit (2026-09-13
        // #630 record, §2.2).
        std::string sourceRef;
        // True when this record's declaration came from the root manifest's
        // own [dependencies]/[dev-dependencies]/[build-dependencies]
        // (`item.consumerDepIndex == kMainConsumer` at the time the record
        // was created). Bounds the root's privilege to override a
        // conflicting declaration of the SAME identity the way
        // `DependencySpec::linkage` is honoured only on the root's own
        // edges — see dep_spec.cppm.
        bool        fromRoot = false;
        // True when that declaration is a selected workspace member's own:
        // `fromRoot` without being the virtual root's member edge. Two
        // selected members that disagree about one dependency are refused,
        // since neither outranks the other.
        bool        fromSelectedMember = false;
        // Reached ONLY through [dev-dependencies]. mcpp.lock excludes these:
        // dev-deps are resolved under `mcpp test` and not under `mcpp build`, so
        // recording them makes a VCS-committed file depend on which command ran
        // last and ping-pong between the two. The lock must be a function of the
        // MANIFEST, not of the command. Cleared the moment a non-dev consumer
        // asks for the same package.
        bool        devOnly = false;
        std::size_t depIndex = 0;       // index into dep_manifests/packages-1 (for in-place re-fetch)
        std::vector<std::string> linkFlagsAdded;  // entries appended to m->buildConfig.ldflags by this dep
    };

    struct WorkItem {
        std::string                          name;                // dep map key as written
        mcpp::manifest::DependencySpec       spec;                // copy (we may mutate version)
        std::string                          requestedBy;         // who asked for it
        std::string                          originalConstraint;  // spec.version BEFORE pinning (for SemVer merge)
        std::size_t                          consumerDepIndex;    // dep_manifests slot of who pushed this child; kMainConsumer for main
        std::filesystem::path                resolveRoot;         // base dir for relative path deps (empty = use project root)
        bool                                 devOnly = false;     // seeded from [dev-dependencies]; inherited by children
        // Seeded from `[build-dependencies]`, and inherited by children the
        // same way `devOnly` is. It answers "does this serve the build or the
        // target", which is a different question from "which build-time
        // product do I want" — that one is answered per edge by `tools` and
        // `host-module`, and the two are orthogonal. A package linked into the
        // target that also provides a tool is written once, in
        // `[dependencies]`, with a `tools` request on it.
        bool                                 buildOnly = false;
    };

    struct DeclaringManifest {
        std::string path;
        bool        namespaceDeclared = false;
    };

    struct GitClone {
        std::filesystem::path root;
        std::string url, refKind, ref;
    };

// PrepareState carries prepare_build's working state across the phases it
// decomposes into (see the layout comment at the top of prepare.cppm). Each
// phase is an ordinary function taking `PrepareState&`; the state itself is
// constructed once, in driver.cpp's prepare_build(), and lives for the
// whole call — including across a phase that stores a closure for a LATER
// phase to call (resolve_target_toolchain, defined in toolchain.cpp's P2 and
// called from its own P5): such a closure captures `state` itself rather
// than individual locals, so it stays valid no matter which phase's stack
// frame created it.
//
// A member exists here because some phase after the one that computes it
// still reads it, or because a stored closure needs it to remain valid past
// its own phase — several members exist ONLY for that second reason and are
// never read by name from another phase (bootstrap_checked, kMainConsumer's
// siblings). A value read and written within a single phase stays an
// ordinary local in that phase's function body; it does not move here.
//
// Not copied: copying this by value would copy every dependency-graph and
// plan structure prepare_build ever builds, silently, at whichever call
// happened to pass it by value instead of by reference.
struct PrepareState {
    PrepareState(bool print_fingerprint_, bool includeDevDeps_,
                 std::vector<mcpp::manifest::Target> extraTargets_,
                 BuildOverrides overrides_)
        : print_fingerprint(print_fingerprint_),
          includeDevDeps(includeDevDeps_),
          extraTargets(std::move(extraTargets_)),
          overrides(std::move(overrides_)),
          // Which tool tiers this invocation needs. Named once so the two
          // provisioning passes cannot disagree — a `mcpp build` that
          // installed the run tier and a `mcpp run` that did not would be the
          // same defect twice.
          toolPurpose(overrides.will_run ? ToolPurpose::Run : ToolPurpose::Build) {}

    PrepareState(const PrepareState&) = delete;
    PrepareState& operator=(const PrepareState&) = delete;

    // ── prepare_build's parameters, unchanged for every phase ──────────────
    bool print_fingerprint;
    bool includeDevDeps;
    std::vector<mcpp::manifest::Target> extraTargets;
    BuildOverrides overrides;
    const ToolPurpose toolPurpose;

    // ── P0: manifest and workspace resolution ───────────────────────────────
    std::string unservedTargetDiagnosis;
    std::optional<std::filesystem::path> appleSdkLocated;
    bool iosFloorFromSdk = false;
    std::string pinReplacedDefault;
    std::optional<std::string> hostSpecBeforeRowPin;
    std::string graphCompilerRequiredBy;
    std::string graphCompilerFamily;
    std::string graphCompilerReplaced;
    std::string requestedCAbi;
    std::string targetDisplayName;
    std::string targetPinCandidate;
    bool targetPinIsCapability = false;
    std::string targetRowPin;
    std::string targetRowName;
    bool tcSpecIsMsvc = false;
    std::optional<std::filesystem::path> root;
    std::optional<mcpp::project::EffectiveManifest> effective;
    std::expected<mcpp::manifest::Manifest, std::string> m = std::unexpected(std::string{});
    std::optional<mcpp::manifest::Manifest> wsManifest;  // keep workspace manifest alive
    // The members this plan builds (workspace design 2026-09-29 §15), by
    // normalised directory, each with the directory below `bin/` its products
    // are placed in. Empty outside a workspace plan. The members as written in
    // `[workspace] members`, in selection order, beside it.
    std::map<std::filesystem::path, std::string> selectedMembers;
    std::vector<std::string> selectedMemberPaths;
    // Each selected member's directory and manifest, as loaded when the
    // selection was made: read by what runs before the graph is walked (the
    // index refresh), which a root's own declarations reach and a member's
    // must as well (`declaredByRoot`).
    std::vector<std::pair<std::filesystem::path, mcpp::manifest::Manifest>> selectedMemberManifests;
    // `--features <dependency>/<feature>` tokens, by the selected member that
    // declares the dependency key: the forwards its edges receive.
    std::map<std::filesystem::path, std::vector<std::pair<std::string, std::string>>>
        memberCliForwards;
    // The test targets a selected member receives (`BuildOverrides::
    // member_targets`), by the same key.
    std::map<std::filesystem::path, std::vector<mcpp::manifest::Target>> memberTargets;
    // The profile's own compile flags (`[profile.<name>] cflags`/`cxxflags`),
    // which a root receives; in a workspace plan every selected member does.
    std::vector<std::string> profileCflags, profileCxxflags;
    bool workspacePlan() const { return !selectedMemberPaths.empty(); }
    // ── The packaging pass: which packed member a package acts for ──────────
    //
    // `mcpp pack` over several members plans them as one graph (member
    // selection design 2026-09-30, K1), and a package of it may serve one
    // member, several, or be a member itself. A program that provides a pack
    // format submits an action against the staged tree of the member it packs,
    // so each program must be told which member that is: its own when it is a
    // selected member, the one selected member whose closure reaches it when
    // exactly one does, and none when several do (`pack_owner`). The closures
    // are read from the dependency edges recorded while the graph was walked,
    // which is why this is asked for only after the graph is complete.
    //
    // Empty, and never asked for, when the plan holds fewer than two selected
    // members: its one subject is what every package acts for.
    std::map<std::string, std::vector<std::string>> packReach;
    bool packReachComputed = false;
    std::size_t selectedMemberCount() const {
        std::size_t n = 0;
        for (std::size_t i = 1; i < packages.size(); ++i)
            if (packages[i].selectedMember) ++n;
        return n;
    }
    void computePackReach() {
        if (packReachComputed) return;
        packReachComputed = true;
        if (selectedMemberCount() < 2) return;
        for (std::size_t m = 1; m < packages.size(); ++m) {
            if (!packages[m].selectedMember) continue;
            std::vector<bool> reached(packages.size(), false);
            reached[m] = true;
            for (bool grew = true; grew;) {
                grew = false;
                for (auto const& r : graphRequests)
                    if (r.consumerPackageIndex < reached.size() && reached[r.consumerPackageIndex]
                        && r.dependencyPackageIndex < reached.size()
                        && !reached[r.dependencyPackageIndex]) {
                        reached[r.dependencyPackageIndex] = true;
                        grew = true;
                    }
            }
            const auto member = mcpp::build::qualified_package_name(packages[m].manifest);
            for (std::size_t i = 1; i < packages.size(); ++i)
                if (reached[i])
                    packReach[mcpp::build::qualified_package_name(packages[i].manifest)]
                        .push_back(member);
        }
    }
    // Why package `i` has no stage when several packed members reach it: such a
    // package acts for none of them (`pack_owner`). Empty otherwise.
    std::string packSharedWhy(std::size_t i) {
        computePackReach();
        if (i >= packages.size()) return {};
        const auto name = mcpp::build::qualified_package_name(packages[i].manifest);
        auto reach = packReach.find(name);
        if (reach == packReach.end() || reach->second.size() < 2) return {};
        std::string members;
        for (auto const& m : reach->second) members += (members.empty() ? "'" : ", '") + m + "'";
        return std::format("package '{}' is reached by the packed members {}", name, members);
    }
    // What the packaging pass tells the programs that act for package `i`, or
    // null when there is none to tell: outside a packaging pass, for a package
    // several packed members reach, and for a member that has no stage.
    const BuildOverrides::PackStage* packStageOf(std::size_t i) {
        auto& stages = overrides.pack_stages;
        if (stages.empty()) return nullptr;
        if (selectedMemberCount() < 2) return &stages.begin()->second;
        computePackReach();
        if (i >= packages.size()) return nullptr;
        const auto name = mcpp::build::qualified_package_name(packages[i].manifest);
        auto reach = packReach.find(name);
        if (reach == packReach.end()) return nullptr;
        auto stage = stages.find(pack_owner(name, reach->second));
        return stage == stages.end() ? nullptr : &stage->second;
    }
    // The selected member package `i` acts for in a plan of several members:
    // itself when it is one, the one member whose closure reaches it when
    // exactly one does, and none (empty) otherwise, by `pack_owner`'s rule. It
    // is what a name the package's actions use is resolved against when two
    // members give that name to different things (`${mcpp.target_file:}`).
    std::string actingMemberOf(std::size_t i) {
        if (selectedMemberCount() < 2 || i >= packages.size()) return {};
        computePackReach();
        const auto name = mcpp::build::qualified_package_name(packages[i].manifest);
        auto reach = packReach.find(name);
        return reach == packReach.end() ? std::string{} : pack_owner(name, reach->second);
    }
    // The packaging pass's values of a build program's environment, for the
    // program of package `i`. A package that acts for no packed member is told
    // nothing, not even the format: its answer cannot depend on a request it
    // has no member to serve, so its program is not run again for it, and one
    // run serves every member that reaches it.
    void fillPackEnv(mcpp::build::BuildProgramEnv& env, std::size_t i) {
        if (overrides.pack_stages.empty()) {
            env.packFormat = overrides.pack_format;
            return;
        }
        if (const auto* stage = packStageOf(i)) {
            env.packFormat          = overrides.pack_format;
            env.packStageDir        = stage->dir;
            env.packStrip           = stage->strip;
            env.packDebugSymbolsDir = stage->debugSymbolsDir;
        }
    }
    // `--features` as the command gave it. A workspace plan hands the tokens to
    // its members and clears `overrides.features`; the request is still what
    // the build was asked for, and what its fast-path record names.
    std::string requestedFeatures;
    // A package of a workspace plan that is a member of the workspace, selected
    // or reached as another member's dependency (the workspace's own package
    // included): its build program runs where a root's does (§15).
    std::function<bool(std::size_t)> isWorkspaceMemberPackage;
    // The selected member whose directory is `dir`, when there is one: the
    // key into `selectedMembers` and `memberCliForwards` (weakly canonical,
    // so a symbolic link in the path names the same member).
    std::optional<std::filesystem::path> selectedMemberAt(const std::filesystem::path& dir) const {
        if (selectedMembers.empty()) return std::nullopt;
        std::error_code ec;
        auto key = std::filesystem::weakly_canonical(dir, ec);
        if (ec) key = dir.lexically_normal();
        if (selectedMembers.contains(key)) return key;
        return std::nullopt;
    }
    // A declaration the root makes: the root manifest's own
    // (`consumerDepIndex == kMainConsumer`), or, in a workspace plan, a
    // selected member's. Each selected member stands where the root stood
    // before a workspace became one plan (workspace design 2026-09-29 §15):
    // the virtual root of 2026.9.29.1 declares nothing but its members, so a
    // rule that read only its edges treated a rooted workspace's own
    // declarations as a dependency's. Every privilege the root's own edges
    // hold asks this one question: the dependency kind clash and the
    // reference clash, the git lock, the identity a declaration adopts, a
    // declared `linkage`, which target-side candidates are direct, and the
    // index refresh. (The refusal to mangle the root's sources stays with the
    // root alone; see the multi-version branch in graph.cpp.)
    bool declaredByRoot(std::size_t consumerDepIndex) const {
        if (consumerDepIndex == kMainConsumer) return true;
        const auto i = consumerDepIndex + 1;
        return i < packages.size() && packages[i].selectedMember;
    }
    // The refusal of two selected members that point one dependency at two
    // checkouts, `first` and `second` each saying how and by whom: neither
    // outranks the other, and one configuration holds one checkout.
    std::string twoMembersRefusal(const ResolvedKey& key, std::string_view first,
                                  std::string_view second) const {
        const auto name = qualifiedKey(key);
        return std::format("dependency '{}' is declared as {} and as {}, two members "
                           "this build selects.\n       A configuration holds one "
                           "checkout of a package: declare '{}' alike in both.",
                           name, first, second, name);
    }
    // The manifests that hold the root declarations of `consumerDepIndex`:
    // the root manifest, or a selected member's two copies (its load record
    // in `dep_manifests` and its package's snapshot). Empty for any other
    // consumer. A write that states what a root declaration resolved to
    // writes all of them, so no reader sees the stale one.
    std::vector<mcpp::manifest::Manifest*> rootDeclarationManifests(std::size_t consumerDepIndex) {
        if (consumerDepIndex == kMainConsumer) {
            if (!m) return {};
            return {&*m};
        }
        if (!declaredByRoot(consumerDepIndex)) return {};
        std::vector<mcpp::manifest::Manifest*> out{&packages[consumerDepIndex + 1].manifest};
        if (consumerDepIndex < dep_manifests.size() && dep_manifests[consumerDepIndex])
            out.push_back(dep_manifests[consumerDepIndex].get());
        return out;
    }
    std::filesystem::path runtimeWorkspaceRoot;
    mcpp::xlings::runtime::RuntimeSelection runtimeSelection;
    std::filesystem::path workRoot;
    std::vector<PlanNote> planNotes;
    std::map<std::string, mcpp::pm::LockedGitSource> gitLockAnchors;
    std::map<std::string, std::string> packageIdentityLockAnchors;
    CacheMode cacheMode{};

    // ── P1: toolchain spec resolution, target axis, L1 cfg merge ────────────
    // `get_cfg`, `provide_runtime_payload`, `report_fixup`,
    // `msvc_usable_either_origin`, `native_first_run_spec`, `tcSpecSource`,
    // `resolvedAccel`, `cfgCtx` and `add_once` are closures that outlive P1
    // (later phases call them by name): each captures `state` itself, not the
    // individual fields below, so it stays valid regardless of which phase's
    // stack frame created it (see the PrepareState comment above).
    std::filesystem::path explicit_compiler;
    std::optional<mcpp::config::GlobalConfig> cfg_opt;
    bool bootstrap_checked = false;
    std::function<std::expected<mcpp::config::GlobalConfig*, std::string>(bool)> get_cfg;
    mcpp::platform::runtime::RuntimeBinding runtimeBindingSnapshot;
    std::string projectSubosBin;
    std::string runtimePayload;
    std::filesystem::path runtimeLibDir;
    bool runtimePayloadProvided = false;
    std::function<void(const mcpp::toolchain::XimToolchainPackage&)> provide_runtime_payload;
    std::function<void(const mcpp::toolchain::FixupOutcome&,
                        const std::filesystem::path&)> report_fixup;
    std::string effectiveProfile;
    std::vector<std::filesystem::path> storeRoots;
    std::optional<std::string> tcSpec;
    TcOrigin tcOrigin{};
    bool tcFromCommandLine = false;
    bool tcFromConsumer = false;
    std::function<std::string()> tcSpecSource;
    std::function<bool()> msvc_usable_either_origin;
    std::function<std::string()> native_first_run_spec;
    bool windowsGnuFirstRun = false;
    std::function<std::string()> resolvedAccel;
    std::function<cfgpred::Ctx()> cfgCtx;
    std::optional<mcpp::platform::TargetPlatform> targetPlatform;
    bool abiThreadsRendered = false;
    std::function<void(std::vector<std::string>&, std::string_view)> add_once;
    std::optional<mcpp::toolchain::Toolchain> tc;
    bool firstRunNeedsTargetPass = false;
    bool targetPassDone = false;

    // ── P2: the toolchain resolver, defined here and called from P5 ─────────
    // A std::function, not auto, because the first-run branch inside calls
    // back into it (see the comment at its definition). Captures `state`
    // itself, like every other stored closure here.
    std::function<std::expected<void, std::string>()> resolve_target_toolchain;

    // ── P3: xlings payload before the dependency graph is built ─────────────
    std::function<std::string()> host_spec_for_build_program;
    std::optional<std::pair<std::filesystem::path, mcpp::toolchain::Toolchain>> hostTcCache;
    std::function<std::expected<
        std::pair<std::filesystem::path, mcpp::toolchain::Toolchain>, std::string>()>
        host_tc_for_build_program;
    std::string resolvedTargetCanonical;
    // See its assignment: a pointer because a PrepareState member cannot be a
    // reference, not because ownership is in question -- it always aliases
    // either wsManifest or m, both of which outlive every phase.
    const mcpp::manifest::Manifest* runtimeOwnerManifest = nullptr;

    // ── P4: the dependency graph ─────────────────────────────────────────────
    // Escaping closures called again from P6/P7/P13 (fillXpkgDirs,
    // fillDepDirs, adoptActionOutputs, markDirectiveTail, computeUsageRequirements,
    // graph_xlings_split), plus the closures THEY call that are themselves
    // referenced by reference from inside those (compilesHere,
    // bareBindingsFor, isProgramOnlyPackage, isArtifactPackage,
    // publishedNamesFor, appendUniqueFlags, appendUniquePaths,
    // appendUniquePath): every one of the latter is a local lambda that would
    // otherwise be captured by reference into one of the former and dangle
    // the moment that former's phase returns, which the compiler cannot
    // detect (nothing outside the escaping closure's own body names them).
    std::vector<mcpp::modgraph::PackageRoot> packages;
    std::vector<std::vector<std::string>> activeFeaturesByPackage;
    // #734 E7: per consumer, the dormant features of its host-module providers.
    std::map<std::size_t, std::vector<mcpp::build::BuildProgramEnv::DormantFeature>>
        dormantFeaturesByConsumer;
    std::map<std::string, std::string> xlingsWinner;

    // ── Sources (mcpp#755): see sources.cpp ─────────────────────────────────
    // The decision record, one entry per subject, and the subjects whose
    // `Using` line has been printed (each is printed once per prepare).
    std::vector<SourceDecision> sources;
    std::set<std::string> announcedSources;
    // Where an overridden payload comes from, resolved once per package key:
    // the environment variable, the project's `[xlings.overrides]`, then
    // config.toml. A key with no override maps to nullopt.
    struct PayloadOverride {
        std::string root;      // the directory `xpkg_dir` answers
        std::string program;   // the program, when the override named one
        std::string version;   // the version it states, empty when none
        SourceClass cls = SourceClass::Custom;
        std::string originKind, originFile, originKey;
        int         originLine = 0;
    };
    std::map<std::string, std::optional<PayloadOverride>> payloadOverrideCache;
    // Package keys whose payload this build does not install: overridden, or
    // declared `provision = "on-request"` and not asked for (yet). And the
    // addresses so skipped, for the run-tier record in plan.cpp.
    std::set<std::string> xlingsOverridden;
    std::set<std::string> xlingsDeferred;
    std::set<std::string> xlingsSkipped;
    // Package keys a build program asked for, installed on request.
    std::set<std::string> requestedPayloads;
    // A toolchain named by path, or stated by the build program (mcpp#755):
    // the description the resolver uses, and the decision it is reported as.
    std::optional<mcpp::manifest::LocalToolchain> localToolchain;
    SourceDecision localToolchainOrigin;
    // `[toolchain] <key> = { configure = "build.mcpp" }` and this is the first
    // pass: the toolchain resolved is the bootstrap, and the root build
    // program runs its toolchain phase once host modules are registered.
    bool toolchainConfigure = false;
    // The toolchain that compiles and runs build programs, when it is not the
    // build toolchain on a native build: `[toolchain] bootstrap`, or the
    // toolchain that ran the toolchain phase. Empty otherwise.
    std::string bootstrapSpec;
    std::vector<std::unique_ptr<mcpp::manifest::Manifest>> dep_manifests;
    std::vector<DepCacheIdentity> dep_cache_identities;
    std::map<std::string, GitLockIdentity> root_git_lock_identities;
    std::vector<DependencyEdge> dependencyEdges;
    std::vector<GraphRequest> graphRequests;
    std::map<std::size_t, DependencyLinkForm> dependencyLinkForms;
    std::map<std::size_t, std::vector<std::size_t>> hostModuleProvidersByConsumer;
    std::string runnerProvider;
    std::map<std::string, std::string> namedRunnerProvider;
    std::vector<std::string> rootReq;
    mcpp::build::provisions::Propagation provisionGraph;

    std::function<std::expected<
        std::pair<std::vector<std::string>, std::vector<std::string>>,
        std::string>()> graph_xlings_split;
    std::function<void()> computeUsageRequirements;
    std::function<void(mcpp::manifest::Manifest&, const std::filesystem::path&,
                        std::size_t)> adoptActionOutputs;
    std::function<void(mcpp::build::BuildProgramEnv&, const mcpp::manifest::Manifest&,
                        std::size_t)> fillXpkgDirs;
    std::function<void(mcpp::build::BuildProgramEnv&, std::size_t,
                        const std::map<std::size_t, std::string>*)> fillDepDirs;
    std::function<mcpp::build::directives::Mark(const mcpp::manifest::Manifest&)>
        markDirectiveTail;
    std::function<std::map<std::string, mcpp::build::provisions::BareBinding>(std::size_t)>
        bareBindingsFor;
    std::function<bool(std::size_t)> compilesHere;
    std::function<bool(const mcpp::manifest::Manifest&)> isProgramOnlyPackage;
    std::function<bool(std::vector<std::string>&, const std::vector<std::string>&)>
        appendUniqueFlags;
    std::function<bool(std::vector<std::filesystem::path>&,
                        const std::vector<std::filesystem::path>&)> appendUniquePaths;
    std::function<bool(std::vector<std::filesystem::path>&, const std::filesystem::path&)>
        appendUniquePath;
    std::function<bool(std::size_t)> isArtifactPackage;
    std::function<std::vector<std::string>(std::size_t,
        const std::map<std::string, mcpp::build::provisions::BareBinding>&)>
        publishedNamesFor;

    // ── the graph-loading half of P4 (graph_load.cpp), called from the
    // worklist engine (graph.cpp): the same escaping-closure pattern as
    // everything above, one level deeper. loadVersionDep is defined once
    // (with these helpers as its own captures) and called repeatedly from
    // the worklist loop, well after its defining call returns. ────────────
    std::deque<WorkItem> worklist;
    std::map<std::string, ResolvedKey> identityBySource;
    std::map<ResolvedKey, DeclaringManifest> declaringManifest;
    std::set<std::pair<std::string, std::string>> adoptionsReported;
    // The adoptions that are corrections (rule W-b), collected while the graph
    // is walked and reported once after it, grouped by the consumer manifest
    // and the namespace its dependencies declare.
    struct AdoptionGroup {
        std::string              consumer;       // manifest path, relative to the project root
        std::string              declaredNs;
        std::set<std::string>    writtenNs;      // the namespaces the keys stated
        std::vector<std::string> keys;           // as written, in walk order
        std::string              example;        // a TOML line for the hint
    };
    std::map<std::pair<std::string, std::string>, AdoptionGroup> adoptionGroups;
    std::map<std::string, GitClone> gitCloneBySource;
    std::set<std::string> selectorMigrationWarnings;
    std::set<std::string> preinstallStack;
    std::set<std::string> preinstallDone;
    std::function<std::string(std::string_view)> cache_index_name;
    std::function<std::optional<std::string>(const std::filesystem::path&,
                                              const ResolvedKey&)> gitMemberDeclaring;
    std::function<std::string(const ResolvedKey&)> qualifiedKey;
    std::function<void(const WorkItem&, const ResolvedKey&)> stateAdoptedIdentity;
    std::function<void(const WorkItem&, const ResolvedKey&,
                        const ResolvedKey&)> reportAdoption;
    std::function<void()> emitAdoptionWarnings;
    std::function<mcpp::pm::IndexRoute(mcpp::config::GlobalConfig*)> index_route;
    std::function<const mcpp::pm::IndexSpec*(const std::string&)> findIndexForNs;
    std::function<std::expected<void, std::string>(mcpp::manifest::DependencySpec&,
                                                    const std::string&)> resolveSemver;
    std::function<std::optional<std::string>(const mcpp::pm::DependencyCoordinate&)>
        readStrictLuaForCandidate;
    std::function<bool(const mcpp::pm::DependencyCoordinate&, std::string_view, bool)>
        xpkgLuaMatchesCandidate;
    std::function<std::vector<mcpp::pm::DependencyCoordinate>(
        const mcpp::manifest::DependencySpec&, const std::string&)> dependencyCoordinates;
    std::function<std::expected<void, std::string>(mcpp::manifest::DependencySpec&,
                                                    const std::string&)> selectDependencyCandidate;
    std::function<std::expected<std::pair<std::filesystem::path, mcpp::manifest::Manifest>,
                                 std::string>(const std::string&, const std::string&,
                                              const std::string&, const std::string&)>
        loadVersionDep;

    std::map<ResolvedKey, ResolvedRecord> resolved;
    std::map<std::size_t, std::vector<std::pair<std::string, std::string>>>
        toolEnvByConsumer;
    std::map<std::size_t, std::vector<mcpp::build::BuildProgramEnv::HostModuleRef>>
        hostModulesByConsumer;
    std::function<void(mcpp::modgraph::PackageRoot&, const mcpp::manifest::Manifest&,
                        const mcpp::build::directives::Mark&)>
        foldDirectiveTailIntoPrivateBuild;

    // ── P6-P8: feature activation, capability/ABI accumulation, target side,
    // host tool provisioning ─────────────────────────────────────────────────
    std::set<std::string> activeRootFeatures;
    std::map<std::string, std::vector<std::string>> capProviders;
    std::vector<std::pair<std::string, std::string>> capRequires;
    std::vector<std::pair<std::string, std::string>> abiRequires;
    std::vector<std::pair<std::string, std::string>> abiRequiresExceptions;
    std::map<std::string, std::vector<std::string>> capExclusive;
    std::map<std::string, std::vector<std::string>> deviceSourcesByPackage;
    // Keyed like `deviceSourcesByPackage`, by `pkg.root.string()` (root
    // package included: `packages[0].root == *root`). Holds a package whose
    // build program failed IN THIS PASS, under `plan_only` (`emit
    // build-database`) — the one case a failed program does not already end
    // the whole call (SPEC-005 R5.2, #699 item 2, E3). A check whose premise
    // is that program's directives must not run for such a package: with no
    // directives applied, every premise reads as unmet, which is a symptom of
    // the recorded `MCPP_BUILD_DATABASE_PROGRAM_FAILED`, not a second defect
    // (design 2026-09-27 §4.2, mcpp#724 side finding A).
    std::set<std::string> programFailedPackages;
    std::function<std::optional<std::string>()> checkVersionFloors;
    mcpp::targetside::TargetSide resolvedTargetSide;
    std::optional<std::size_t> cxxLayerProviderIndex;
    bool targetSideResolved = false;
    mcpp::modgraph::UsageRequirements targetSideUsage;

    // ── P9/P11: the module scan, its validation, and the fingerprint they
    // feed; read again by P13 (the plan, resolution.json) ───────────────────
    mcpp::toolchain::Fingerprint fp;
    std::filesystem::path stdBmiPath;
    std::filesystem::path stdObjectPath;
    std::filesystem::path stdCompatBmiPath;
    // A unit of the plan imports `std.compat` (2026.10.5.2): its BMI is built
    // only then.
    bool needsStdCompat = false;
    std::filesystem::path stdCompatObjectPath;
    std::optional<mcpp::toolchain::StdModuleDescription> describedStdModule;
    std::string stdFlagAndDialect;
    // The entry of package `i`; with `requesters`, only the requests made by
    // the packages it marks (a build program's closure).
    std::function<nlohmann::json(std::size_t, bool, const std::vector<bool>*)> graph_package_entry;
    mcpp::modgraph::ScanResult scan;
    mcpp::modgraph::ValidateReport report;
};

// Phase declarations. Defined across manifest.cpp, toolchain.cpp,
// xlings.cpp, graph_load.cpp, graph.cpp, features.cpp, target_side.cpp,
// scan.cpp and plan.cpp; called in order from driver.cpp's
// prepare_build(). Ordinary (non-static) module-linkage declarations --
// static would give each definition internal linkage, invisible outside
// its own file.
std::expected<void, std::string> phase0_manifest_and_workspace(PrepareState& state);

// plan.cpp: the member path of a package root within the workspace this build
// runs in, or empty (W3).
std::string workspace_member_of(const PrepareState& state, const std::filesystem::path& root);
// windows_resources.cpp: the Windows resources of the plan's images (P13).
std::expected<void, std::string> step13_windows_resources(PrepareState& state, BuildContext& ctx);
// link_scope.cpp: what an image links with beyond its objects (P13, 2026.10.5.2).
void note_root_link_words_withheld(const PrepareState& state, const BuildContext& ctx);
std::expected<void, std::string> scope_lto(PrepareState& state, BuildContext& ctx);
// graph.cpp: a dependency's link flags as its consumer's link reads them --
// word by word, each search path made absolute against the package.
std::vector<std::string> normalized_dependency_ldflags(
    const std::filesystem::path& depRoot, const std::vector<std::string>& ldflags);
std::expected<void, std::string> phase1_toolchain_spec_and_axes(PrepareState& state);
std::expected<void, std::string> phase2_define_toolchain_resolver(PrepareState& state);
std::expected<void, std::string> phase3_xlings_before_graph(PrepareState& state);
std::expected<void, std::string> phase4a_graph_load(PrepareState& state);
std::expected<void, std::string> phase4b_graph_worklist(PrepareState& state);
std::expected<void, std::string> phase5_toolchain_after_graph(PrepareState& state);
std::expected<void, std::string> phase6_features_and_host_tools(PrepareState& state);
std::expected<void, std::string> phase9_target_side(PrepareState& state);
std::expected<void, std::string> phase11_scan(PrepareState& state);
// E9 (#734): every package's `mcpp = ">=V"` against this binary. Checked for the
// root right after its manifest is final and for the whole graph after loading,
// so a too-new root fails before any later phase can fail on a key it uses.
std::expected<void, std::string> check_engine_floors(const PrepareState& state,
                                                     bool rootOnly);
std::expected<BuildContext, std::string> phase13_finish(PrepareState& state);
// P13's records half (records.cpp), called by phase13_finish.
std::expected<void, std::string> step13_lockfile(PrepareState& state, BuildContext& ctx);
void step13_resolution_json(PrepareState& state, BuildContext& ctx);

// ── Helpers the phases share, defined in the files named below ─────────────

// config.cpp: manifest conditional merges, build flags and defines, workspace inheritance, feature requests, std-module detection
void warn_unknown_xpkg_keys(const mcpp::manifest::Manifest& dm,
                                   std::string_view depLabel);
std::expected<void, std::string>
materialize_generated_files(const std::filesystem::path& root,
                            const mcpp::manifest::Manifest& manifest,
                            std::vector<std::filesystem::path>* stale = nullptr);
bool same_dependency_identity(const mcpp::manifest::DependencySpec& a,
                              const mcpp::manifest::DependencySpec& b);
void replace_dependencies(
    std::map<std::string, mcpp::manifest::DependencySpec>& into,
    const std::map<std::string, mcpp::manifest::DependencySpec>& from);
std::vector<std::pair<std::string, std::string>>& pending_flag_words_notes();
void report_flag_words_changes(const mcpp::manifest::Manifest& m);
std::optional<std::string>
inherit_as_workspace_member(mcpp::manifest::Manifest& member,
                            const mcpp::manifest::Manifest& workspace,
                            const std::filesystem::path& workspaceRoot,
                            const std::filesystem::path& memberDir);
std::optional<std::pair<mcpp::manifest::Manifest, std::filesystem::path>>
workspace_listing(const std::filesystem::path& memberDir,
                  const std::filesystem::path& bound);
bool merge_layer_conditional_config(mcpp::manifest::Manifest& m,
                                    const cfgpred::Ctx& ctx);
std::vector<std::string> feature_closure(const mcpp::manifest::Manifest& pm,
                                         const std::vector<std::string>& requested,
                                         bool seedDefault = true);
bool is_std_module(std::string_view name);
bool graph_or_targets_import_std(const mcpp::modgraph::Graph& graph,
                                 const mcpp::manifest::Manifest& manifest,
                                 const std::filesystem::path& projectRoot,
                                 const std::vector<mcpp::modgraph::PackageRoot>& packages);
bool graph_or_targets_import_std_compat(const mcpp::modgraph::Graph& graph,
                                        const mcpp::manifest::Manifest& manifest,
                                        const std::filesystem::path& projectRoot,
                                        const std::vector<mcpp::modgraph::PackageRoot>& packages);
bool graph_or_targets_import(const mcpp::modgraph::Graph& graph,
                             const mcpp::manifest::Manifest& manifest,
                             const std::filesystem::path& projectRoot,
                             const std::vector<mcpp::modgraph::PackageRoot>& packages,
                             const std::function<bool(std::string_view)>& wanted);

// toolchain_env.cpp: target rows, sysroots, the MSVC binding, build-program environments
const mcpp::manifest::TargetEntry*
find_target_entry(const mcpp::manifest::Manifest& m,
                  const mcpp::toolchain::triple::Triple& t);
const std::string*
sysroot_override(const mcpp::manifest::Manifest& m,
                 const mcpp::toolchain::triple::Triple& t);
std::expected<void, std::string>
bind_msvc_sysroot(mcpp::toolchain::Toolchain& tc,
                  const mcpp::manifest::Manifest& m,
                  const std::function<std::expected<mcpp::config::GlobalConfig*,
                                                    std::string>()>& cfgOf);
std::expected<void, std::string>
check_cl_row_sysroot(const mcpp::toolchain::Toolchain& tc,
                     const mcpp::manifest::Manifest& m);
void fill_package_build_env(mcpp::build::BuildProgramEnv& e,
                            const mcpp::manifest::Manifest& m);
void fill_target_build_env(mcpp::build::BuildProgramEnv& e,
                           const mcpp::manifest::Manifest& m,
                           const mcpp::toolchain::Toolchain* tc,
                           const mcpp::config::GlobalConfig* cfg);
std::string min_platform_version(const mcpp::manifest::Manifest& m,
                                 const mcpp::toolchain::triple::Triple& t,
                                 const std::filesystem::path& compilerPath);

// fetch.cpp: git remotes, network retries, xlings addresses and their provisioning
std::string git_cache_head(const std::filesystem::path& gitRoot);
// `progressLabel`, when given, draws git's `--progress` download phase as
// one bar labelled with it (W11); the command must pass `--progress`.
mcpp::platform::process::RunResult run_with_network_retry(
        std::string_view command,
        const std::function<void()>& between = {},
        std::string_view progressLabel = {});
std::vector<std::string>
applicable_xlings_addresses(const mcpp::manifest::Manifest& man,
                            const std::vector<std::string>& activeFeatures,
                            ToolPurpose purpose, bool isRoot);
std::expected<void, std::string>
provision_xlings_addresses(const mcpp::config::GlobalConfig& cfg,
                           const std::vector<std::string>& declaredDeps,
                           const std::filesystem::path& legacyStampRoot,
                           std::string_view label);
std::string with_index_cause(std::string msg);

// ── Sources (mcpp#755), defined in sources.cpp ─────────────────────────────
// Record (or replace) the decision for `d.subject`; a non-default source is
// announced once with its `Using` line.
void record_source(PrepareState& state, SourceDecision d);
// The override for a package key, resolved and cached; nullptr when none.
std::expected<const PrepareState::PayloadOverride*, std::string>
payload_override(PrepareState& state, std::string_view key);
// The winners of one unification that this build installs from the registry
// now: overridden and deferred (`on-request`, not asked for) packages are left
// out, recorded, and announced. `onRequest[i]` says whether claim `i` was
// declared `provision = "on-request"`.
std::expected<std::vector<std::string>, std::string>
payloads_to_provision(PrepareState& state,
                      const mcpp::xlings::addrset::Resolution& unified,
                      std::span<const mcpp::xlings::addrset::Claim> claims,
                      const std::vector<char>& onRequest);
// A dependency that states `[xlings.overrides]` is refused: where a payload
// comes from is the root's statement (mcpp#755).
std::optional<std::string> dependency_override_refusal(const PrepareState& state);
// `MCPP_XPKG_<NS>_<NAME>_{DIR,SOURCE,PROGRAM}` for what `owner` and the host
// modules compiled into consumer `consumer`'s program declared.
void fill_xpkg_env(PrepareState& state, mcpp::build::BuildProgramEnv& e,
                   const mcpp::manifest::Manifest& owner, std::size_t consumer);
// After a run of `who`'s build program into `m`: when it asked for payloads,
// install them in one batch and refill `e`; true when the program must run
// again. Under `plan_only` nothing is installed: a note is recorded and false
// is returned.
std::expected<bool, std::string>
answer_payload_requests(PrepareState& state, mcpp::manifest::Manifest& m,
                        mcpp::build::BuildProgramEnv& e, std::size_t consumer,
                        std::string_view who);
// Run a build program through `run`, answering its payload requests: when a
// run asked for payloads, install them and run it again (at most three runs).
std::expected<void, std::string>
run_answering_requests(PrepareState& state, mcpp::manifest::Manifest& m,
                       mcpp::build::BuildProgramEnv& e, std::size_t consumer,
                       std::string_view who,
                       const std::function<std::expected<void, std::string>()>& run);
// The `mcpp:decision=` lines every program of this build stated, as decisions.
void record_tool_decisions(PrepareState& state);
// The toolchain's own decision, the plugins' tool decisions, `--managed-only`,
// and `ctx.sources`: the last step of the record, before `resolution.json`.
std::expected<void, std::string> step13_sources(PrepareState& state, BuildContext& ctx);
// A toolchain named by path, and the toolchain phase (local_toolchain.cpp).
std::expected<void, std::string> step1_local_toolchain(PrepareState& state);
std::expected<void, std::string>
step2_use_local_toolchain(PrepareState& state, const mcpp::toolchain::ToolchainSpec& spec);
std::expected<void, std::string> step2_apply_local_toolchain(PrepareState& state);
std::expected<void, std::string> step6_toolchain_phase(PrepareState& state);
// The statement and bootstrap spec of a toolchain phase that asked prepare to
// start again, taken once.
std::optional<std::pair<std::vector<std::string>, std::string>> take_toolchain_restart();
// `--managed-only`: a refusal naming every source that is not the ecosystem's.
std::optional<std::string> managed_only_refusal(const PrepareState& state);
// The `Finished`-line summary of the non-default sources, empty when none.
std::string sources_summary(const std::vector<SourceDecision>& sources);

} // namespace mcpp::build
