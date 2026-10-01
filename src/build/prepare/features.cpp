// features.cpp -- P6 to P8: feature activation, capability and ABI
// requirements, host-tool provisioning, and the dependencies' build programs.

module mcpp.build.prepare;
import :state;

import mcpp.build.prepare_inputs;

import std;
import mcpp.diag;
import mcpp.build.refusal;
import mcpp.build.version_floor;
import mcpp.home;
import mcpp.platform.axis;
import mcpp.manifest;
import mcpp.source_kind;
import mcpp.modgraph.glob;
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;
import mcpp.modgraph.validate;
import mcpp.graph;
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
import mcpp.build.plan;
import mcpp.build.flags;          // compute_flags — the per-role contracts (#418)
import mcpp.build.graph_shape;  // #407: the graph says which mode wrote it
import mcpp.pack.abi_tag;      // the tag a prebuilt dependency is checked against
import mcpp.pack.prebuilt;     // …and the check itself
import mcpp.pack.stage_tree;   // where `${mcpp.stage_dir}` points, and its manifest
import mcpp.build.build_program;
import mcpp.build.tool_store;   // #355 host tools: store layout + key + overrides
import mcpp.build.backend;      // BuildOptions for the tool sub-build
import mcpp.build.ninja;        // make_ninja_backend — driving that sub-build
import mcpp.xlings;
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
import mcpp.ui;
import mcpp.wire;               // Severity, for PlanNote (#699 item 2, E3)

namespace mcpp::build {

// STEP FUNCTIONS (mcpp#722 / T6), one per section phase6's own banners
// already named. Statements moved verbatim; `aggregatedRequest` (used by
// two of these sections) is promoted from a local lambda to a file-scope
// function of PrepareState&, the same treatment #719 gave every closure
// that a later phase needed.

static std::pair<std::vector<std::string>, bool>
aggregatedRequest(PrepareState& state, std::size_t depPkgIndex) {
            std::vector<std::string> feats;
            bool anyEdge = false, anyDefault = false;
            for (auto const& edge : state.dependencyEdges) {
                if (edge.dependencyPackageIndex != depPkgIndex) continue;
                anyEdge = true;
                if (edge.defaultFeatures) anyDefault = true;
                for (auto const& f : edge.requestedFeatures)
                    if (std::find(feats.begin(), feats.end(), f) == feats.end())
                        feats.push_back(f);
            }
            return { std::move(feats), anyEdge ? anyDefault : true };
}

static void step6_check_version_floors_closure(PrepareState& state) {
    // ─── Feature activation (Cargo-style, additive) ────────────────────
    // activated(pkg) = pkg.[features].default ∪ features requested for it
    // (root: --features; deps: the root dep spec's `features = [...]`).
    // Implied features expand transitively. Each active feature becomes
    // -DMCPP_FEATURE_<NAME> on that package's compile flags.
    // (Transitive dep→dep feature requests are not yet propagated.)
    // Also captured here: the root package's active feature set, reused below
    // for the [targets.*] required_features gate.
    // Capability accumulation (Stage 3): which packages provide each capability,
    // and which (capability, requiring-package) pairs need binding. Filled by
    // apply() as each package's features activate; bound after the loops below.
    // `requires_abi`: (what, requirer). See Manifest::requiresAbiThreads.
    // Same shape, for the second `abi` member (A1/A6). Two vectors rather
    // than one tagged one, because every reader below already asks "threads
    // or exceptions" as two separate questions.
    // Who claimed sole provision of what. Separate from capProviders because
    // the question it answers is different: capProviders asks "can this
    // requirement be satisfied", this asks "can these two coexist at all".
    // Callable twice: once here, for what the manifests and the
    // dependencies' build programs declared, and once more after the
    // root's build program has run -- a rule package it imports states
    // its facts and floors from there (`mcpp::fact` / `mcpp::floor`),
    // and a check that ran only before it would never see them.
    // package name -> device-kind sources of its effective source set, filled
    // by the narrowing pass after feature application and read at both
    // build-program run sites (MCPP_DEVICE_SOURCES).
    // Keyed by the package's ROOT DIRECTORY, not by its name. Two packages in
    // one graph may share a bare name and differ only by namespace — that is
    // what namespaces are for — and a name key would hand one package's
    // device sources to the other's build program with nothing reporting it.
    state.checkVersionFloors = [&]() -> std::optional<std::string> {
        std::map<std::string, std::pair<std::string, std::string>> facts;  // name -> (version, who)
        // #634, A9: THE TARGET'S PLATFORM FLOOR IS A FACT THE ENGINE STATES,
        // in the platform's own words. A dependency that needs Android API 23
        // writes `android.api-level >= 23` as an ordinary `version-floor`
        // requirement and is refused before compiling when the application
        // targets less. The floor is not raised for it: the value is already
        // inside the compiler's `--target` by now, and which devices an
        // application installs on is the application's decision. A row that
        // states no such fact (a desktop Linux build) leaves the requirement
        // silent, so a requirement needs no selector. The engine's value is
        // entered first, so a package stating the same name cannot replace it.
        std::map<std::string, std::string> platformFactOrigin;   // name -> the key that sets it
        if (state.tc) {
            if (auto t = mcpp::toolchain::triple::parse(state.tc->targetTriple);
                t && (t->is_android() || t->is_apple())) {
                const auto value = min_platform_version(*state.m, *t, state.tc->binaryPath);
                std::string name, origin;
                if (t->is_android()) {
                    auto row = state.m->targetOverrides.find(t->str());
                    name = "android.api-level";
                    origin = row != state.m->targetOverrides.end() && row->second.minApiLevel > 0
                        ? std::format("[target.{}] min_api_level", t->str())
                        : std::format("the toolchain's lowest supported level, because "
                                      "[target.{}] min_api_level is not set", t->str());
                } else if (t->is_ios()) {
                    name = "ios.deployment-target";
                    origin = state.iosFloorFromSdk
                        ? std::string("the located SDK's version, because [build] "
                                      "ios_deployment_target is not set")
                        : std::string("[build] ios_deployment_target");
                } else {
                    name = "macos.deployment-target";
                    origin = state.m->buildConfig.macosDeploymentTarget.empty()
                        ? std::string("mcpp's default for macOS, because [build] "
                                      "macos_deployment_target is not set")
                        : std::string("[build] macos_deployment_target");
                }
                if (!value.empty()) {
                    facts.emplace(name, std::pair{value, std::string{}});
                    platformFactOrigin.emplace(name, std::move(origin));
                }
            }
        }
        for (std::size_t pi = 0; pi < state.packages.size(); ++pi) {
            // The root's claims live in *m: its build program mutates
            // *m, and packages[0] is a snapshot taken before it ran.
            const auto& mf  = pi == 0 ? *state.m : state.packages[pi].manifest;
            const auto who = mf.package.name;
            for (auto const& entry : mf.runtimeConfig.provides) {
                auto fact = mcpp::build::parse_version_fact(entry);
                if (fact.valid()) facts.emplace(fact.name, std::pair{fact.version, who});
            }
        }
        for (std::size_t pi = 0; pi < state.packages.size(); ++pi) {
            // The root's claims live in *m: its build program mutates
            // *m, and packages[0] is a snapshot taken before it ran.
            const auto& mf  = pi == 0 ? *state.m : state.packages[pi].manifest;
            const auto who = mf.package.name;
            for (auto const& req : mf.runtimeConfig.requirements) {
                if (req.kind != "version-floor") continue;
                auto floor = mcpp::build::parse_version_floor(req.value);
                if (!floor.valid()) {
                    return std::format(
                        "`{}` declares a version-floor requirement mcpp "
                        "cannot read: '{}'.\n"
                        "       The shape is `<name> >= <version>`, e.g. "
                        "`cuda.driver >= 12.0`.", who, req.value);
                }
                auto it = facts.find(floor.name);
                if (it == facts.end()) continue;      // nobody stated it
                auto met = mcpp::build::version_at_least(it->second.first,
                                                        floor.version);
                if (!met || *met) continue;
                refusal::record(refusal::Code::VersionFloorUnmet);
                if (auto origin = platformFactOrigin.find(floor.name);
                    origin != platformFactOrigin.end())
                    return std::format(
                        "`{}` requires {} >= {}, and this build targets {}.\n"
                        "         set by: {}\n"
                        "       This is checked before anything is compiled "
                        "because the failure it prevents is not:\n"
                        "       a library that needs a newer platform links "
                        "cleanly and fails on the device that lacks it.",
                        who, floor.name, floor.version, it->second.first,
                        origin->second);
                return std::format(
                    "`{}` requires {} >= {}, and {} is stated as {}.\n"
                    "         stated by: {}\n"
                    "       This is checked before anything is compiled "
                    "because the failure it prevents is not:\n"
                    "       a build against too-new a runtime links "
                    "cleanly and fails at first use.",
                    who, floor.name, floor.version, floor.name,
                    it->second.first, it->second.second);
            }
        }
        return std::nullopt;
    };
}

static std::expected<void, std::string> step6_activate_features(PrepareState& state) {
        auto sanitize = [](std::string f) {
            for (auto& c : f)
                c = std::isalnum(static_cast<unsigned char>(c))
                  ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : '_';
            return f;
        };
        auto activate = [](const mcpp::manifest::Manifest& pm,
                           const std::vector<std::string>& requested,
                           bool seedDefault = true) {
            return feature_closure(pm, requested, seedDefault); // single shared implementation
        };
        auto apply = [&](mcpp::modgraph::PackageRoot& pkg,
                         const std::vector<std::string>& requested,
                         bool seedDefault = true) {
            auto active = activate(pkg.manifest, requested, seedDefault);
            // Capability accumulation: package-level provides always count;
            // feature-scoped provides/requires count only when the feature is
            // active. Requirements are bound after all packages are processed.
            const auto& pcap = pkg.manifest.package.name;
            for (auto& cap : pkg.manifest.provides) state.capProviders[cap].push_back(pcap);
            for (auto& cap : pkg.manifest.exclusive) state.capExclusive[cap].push_back(pcap);
            for (auto& f : active) {
                if (auto it = pkg.manifest.featureProvides.find(f);
                    it != pkg.manifest.featureProvides.end())
                    for (auto& cap : it->second) state.capProviders[cap].push_back(pcap);
                if (auto it = pkg.manifest.featureRequires.find(f);
                    it != pkg.manifest.featureRequires.end())
                    for (auto& cap : it->second) state.capRequires.emplace_back(cap, pcap);
                if (auto it = pkg.manifest.featureRequiresAbiThreads.find(f);
                    it != pkg.manifest.featureRequiresAbiThreads.end() && it->second)
                    state.abiRequires.emplace_back(std::format("feature `{}`", f), pcap);
                if (auto it = pkg.manifest.featureRequiresAbiExceptions.find(f);
                    it != pkg.manifest.featureRequiresAbiExceptions.end() && it->second)
                    state.abiRequiresExceptions.emplace_back(std::format("feature `{}`", f), pcap);
                // The TARGET-AXIS per-feature form (A6):
                // `[target.<sel>.feature-requires-abi] <f>`, already reduced
                // by merge_conditional_config to the selectors that matched
                // and asked. Named by the selector, as written, not "feature
                // `f`" -- the feature only decided whether the section counts;
                // the selector is what asked for the switch.
                if (auto it = pkg.manifest.targetFeatureRequiresAbiThreads.find(f);
                    it != pkg.manifest.targetFeatureRequiresAbiThreads.end() && !it->second.empty())
                    state.abiRequires.emplace_back(
                        std::format("[target.'{}']", it->second.front()), pcap);
                if (auto it = pkg.manifest.targetFeatureRequiresAbiExceptions.find(f);
                    it != pkg.manifest.targetFeatureRequiresAbiExceptions.end() && !it->second.empty())
                    state.abiRequiresExceptions.emplace_back(
                        std::format("[target.'{}']", it->second.front()), pcap);
            }
            if (pkg.manifest.requiresAbiThreads)
                state.abiRequires.emplace_back("the package", pcap);
            if (pkg.manifest.requiresAbiExceptions)
                state.abiRequiresExceptions.emplace_back("the package", pcap);
            // `[target.<sel>] requires_abi` (A6): the package-wide form of the
            // same target-axis requirement, one entry per matching selector
            // that asked.
            for (auto const& sel : pkg.manifest.targetRequiresAbiThreads)
                state.abiRequires.emplace_back(std::format("[target.'{}']", sel), pcap);
            for (auto const& sel : pkg.manifest.targetRequiresAbiExceptions)
                state.abiRequiresExceptions.emplace_back(std::format("[target.'{}']", sel), pcap);
            // A DEPENDENCY'S OWN `[target.<selector>.abi]` DOES NOT CHANGE THE
            // BUILD. The switch belongs to the artefact, which the root decides;
            // a table written in a dependency is reported rather than silently
            // ignored, and points at the key a dependency does have. Covers
            // BOTH members: a dependency that declares only `exceptions` must
            // be reported exactly as one that declares only `threads`.
            if (pcap != state.m->package.name
                && (pkg.manifest.buildConfig.abiThreadsDeclared
                    || pkg.manifest.buildConfig.abiExceptionsDeclared))
                mcpp::diag::warning("abi/dependency-table", std::format(
                    "`{}` declares [target.<selector>.abi], which only the root "
                    "manifest decides; a dependency states what it needs with "
                    "`requires_abi = {{ threads = true }}` or "
                    "`requires_abi = {{ exceptions = true }}`", pcap));
            // `[targets.*] required_features` on a DEPENDENCY.
            //
            // THIS GATE EXISTED ONLY FOR THE ROOT. The root's targets are
            // filtered further down against the root's own active features;
            // a dependency's were never filtered at all, so a descriptor that
            // wrote `required_features` on a target got the opposite of what
            // it asked for: the target was built for EVERY consumer, whether
            // or not the feature was active. For a `kind = "shared"` target
            // that is not a cosmetic difference — its mere presence changes
            // how the whole package is linked into every consumer.
            //
            // Gated against THIS package's active set, not the root's. A
            // feature name is package-scoped, so the root's set is a different
            // vocabulary that happens to share a type.
            //
            // LIBRARY TARGETS ONLY, and the exclusion is load-bearing.
            //
            // A target requested as a HOST TOOL is what was ASKED FOR, so its
            // `required_features` become that sub-build's INPUTS instead of a
            // gate — docs/05 §2.2 says so in as many words. An earlier
            // revision of this gate erased every kind, with a comment claiming
            // the tool path "re-enters prepare_build with the dependency as
            // the ROOT, so it never reaches this code". That was written from
            // memory rather than read: the tool LOOKUP runs several hundred
            // lines BELOW this point, against this very manifest, and it found
            // an empty target list. `187_dep_host_tool.sh` caught it.
            //
            // Restricting the gate to libraries is not a workaround, it is the
            // rule: a dependency's `bin` target produces no link unit in this
            // build (make_plan only walks the ROOT's targets), so leaving it in
            // place costs nothing. What the gate exists for is the shape where
            // a target's mere presence changes how the package is linked into
            // every consumer — and that is exactly a `shared` or `lib` target.
            // A selected workspace member builds every target, so every one
            // of its targets is gated, as a root's are (§15).
            std::erase_if(pkg.manifest.targets,
                          [&](const mcpp::manifest::Target& t) {
                if (!pkg.selectedMember
                    && t.kind != mcpp::manifest::Target::Library
                    && t.kind != mcpp::manifest::Target::SharedLibrary)
                    return false;
                for (auto const& rf : t.requiredFeatures)
                    if (std::find(active.begin(), active.end(), rf) == active.end())
                        return true;
                return false;
            });

            for (auto& f : active) {
                auto def = "-DMCPP_FEATURE_" + sanitize(f);
                pkg.manifest.buildConfig.cflags.push_back(def);
                pkg.manifest.buildConfig.cxxflags.push_back(def);
                pkg.privateBuild.cflags.push_back(def);
                pkg.privateBuild.cxxflags.push_back(def);
                // Feature System v2 Stage 1: package-owned `defines` declared on
                // this feature ride alongside the automatic MCPP_FEATURE_ macro.
                // Bare names desugar to -D<x>, matching [targets.*] `defines`.
                if (auto it = pkg.manifest.buildConfig.featureDefines.find(f);
                    it != pkg.manifest.buildConfig.featureDefines.end())
                    for (auto& d : it->second) {
                        auto fdef = mcpp::manifest::flag_element("-D" + d);
                        pkg.manifest.buildConfig.cflags.push_back(fdef);
                        pkg.manifest.buildConfig.cxxflags.push_back(fdef);
                        pkg.privateBuild.cflags.push_back(fdef);
                        pkg.privateBuild.cxxflags.push_back(fdef);
                        // Interface-propagate the user-declared feature define:
                        // a header-only dependency's switch (e.g. EIGEN_USE_BLAS)
                        // only takes effect in the TU that includes its headers,
                        // so consumers that enable the feature must see it too.
                        // computeUsageRequirements() flows publicUsage flags into
                        // each consumer's privateBuild along Public/Interface
                        // edges, mirroring include_dirs. The automatic
                        // MCPP_FEATURE_<NAME> macro stays private to the owning
                        // package (it is a build signal, not a public contract).
                        pkg.publicUsage.cflags.push_back(fdef);
                        pkg.publicUsage.cxxflags.push_back(fdef);
                    }
            }
            // Feature-gated sources (e.g. gtest's gtest_main.cc behind "main"):
            // drop EVERY feature-listed glob from the default build, then add
            // back only the ones whose feature is active. Runs even when no
            // feature is active, so a gated source is excluded by default.
            //
            // The DROP is build-mode only (!state.includeDevDeps). `mcpp test`
            // (state.includeDevDeps) keeps the full surface so the dev-dependency
            // track's per-test main detection (run_tests / make_plan) still sees
            // gtest_main.cc and prunes it per test — the two tracks stay
            // decoupled; gtest's descriptor keeps gtest_main.cc in base `sources`
            // too, so skipping the drop leaves it visible.
            //
            // The ADD runs in BOTH modes. A descriptor may list a glob ONLY under
            // `features` and never in base `sources` (xpkg's `features.X.sources`
            // lands in featureSources alone — compat.spdlog's `compiled`,
            // compat.cjson's `utils`, compat.eigen's `eigen_blas`). Gating the add
            // on !state.includeDevDeps meant those sources were never compiled under
            // `mcpp test` → link-time `undefined reference` (the eigen_blas
            // `dgemm_` failure, long misread as a linking follow-up: it was
            // source-set resolution, not linking). Add is dedup'd so gtest's
            // doubly-listed gtest_main.cc cannot land twice.
            auto& bc = pkg.manifest.buildConfig;
            if (!bc.featureSources.empty()) {
                // WHETHER A FEATURE *GATES* A SOURCE OR *PROVIDES* IT, AND
                // THE ANSWER IS WRITTEN IN THE MANIFEST ALREADY.
                //
                // Two families of package reach this code and they want
                // opposite things under `mcpp test`:
                //
                //   gtest         lists `*/googletest/src/gtest_main.cc` in
                //                 base `sources` AND under `features.main`.
                //                 The package provides the file unconditionally;
                //                 the feature is a gate over it. The
                //                 dev-dependency track's per-test main detection
                //                 must still SEE it in order to prune it per
                //                 test, so an inactive gate must not make it
                //                 vanish.
                //
                //   riscv-virt-rt names `src/kal/**` under `features.openkal`
                //                 and nowhere else. The package does not provide
                //                 those files at all without the feature — the
                //                 headers they include arrive through that
                //                 feature's `[feature-deps]` — so compiling them
                //                 fails on `'openkal/abort.h' file not found`.
                //
                // The discriminator is membership in base `sources`, evaluated
                // BEFORE the drop below removes it. A glob in both places is a
                // gate; a glob in one place is a provider.
                //
                // THIS IS THE FOURTH ATTEMPT, AND THE THIRD WAS ABANDONED ON
                // A MISTAKEN READING. It was recorded as failing because
                // "gtest's base entry is a glob that MATCHES the file rather
                // than the same string". Measured against the descriptor the
                // index actually carries, the two entries are byte-identical
                // (`compat.gtest.lua` lines 71 and 90). The criterion was
                // sound; what it was applied to was not — the earlier attempt
                // compared against `bc.sources` AFTER `drop()` had already
                // removed the entry, so the membership test could only ever be
                // false.
                std::set<std::string> baseGlobs(bc.sources.begin(), bc.sources.end());
                baseGlobs.insert(pkg.manifest.modules.sources.begin(),
                                 pkg.manifest.modules.sources.end());
                if (!state.includeDevDeps) {
                    // glob → owned by at least one ACTIVE feature?
                    std::set<std::string> activeNow(active.begin(), active.end());
                    std::map<std::string, bool> gated;
                    for (auto& [f, globs] : bc.featureSources)
                        for (auto& g : globs)
                            gated[g] = gated[g] || activeNow.contains(f);
                    auto drop = [&](std::vector<std::string>& v) {
                        std::erase_if(v, [&](const std::string& s) { return gated.contains(s); });
                    };
                    drop(bc.sources);
                    drop(pkg.manifest.modules.sources);
                    // Dropping the glob STRING is not enough: files it matches
                    // may still be covered by a broader base glob (the default
                    // src/** — the mcpp.toml G5 case). An inactive gate becomes
                    // a `!` exclusion so the gate actually gates; active gates
                    // are re-added below.
                    for (auto& [g, isActive] : gated) {
                        if (isActive || g.starts_with("!")) continue;
                        bc.sources.push_back("!" + g);
                        pkg.manifest.modules.sources.push_back("!" + g);
                    }
                }
                else {
                    // `mcpp test`. The gate that build mode applies wholesale is
                    // applied here only to the globs the package provides
                    // NOWHERE ELSE, which leaves gtest's doubly-listed source
                    // visible and stops riscv-virt-rt's feature-only sources
                    // from being compiled without their feature.
                    //
                    // THE `!` EXCLUSION IS THE WHOLE MECHANISM, NOT THE GLOB
                    // REMOVAL. `src/kal/**` is never IN `bc.sources` — the
                    // package declares no `sources` at all and its files are
                    // matched by the inferred `src/**`. Erasing the string
                    // erases nothing; only an exclusion gates.
                    std::set<std::string> activeNow(active.begin(), active.end());
                    std::map<std::string, bool> gated;
                    for (auto& [f, globs] : bc.featureSources)
                        for (auto& g : globs)
                            gated[g] = gated[g] || activeNow.contains(f);
                    for (auto& [g, isActive] : gated) {
                        if (isActive || g.starts_with("!")) continue;
                        if (baseGlobs.contains(g)) continue;   // a gate, not a provider
                        bc.sources.push_back("!" + g);
                        pkg.manifest.modules.sources.push_back("!" + g);
                    }
                }
                std::set<std::string> activeSet(active.begin(), active.end());
                auto add = [](std::vector<std::string>& v, const std::string& g) {
                    if (std::ranges::find(v, g) == v.end()) v.push_back(g);
                };
                for (auto& [f, globs] : bc.featureSources) {
                    if (!activeSet.contains(f)) continue;
                    for (auto& g : globs) {
                        add(bc.sources, g);
                        add(pkg.manifest.modules.sources, g);
                    }
                }
            }
            // #253: per-feature per-glob flags — fold each ACTIVE feature's
            // entries into the base globFlags funnel. Everything downstream
            // (scanner glob match, per-TU flag landing, zero-hit warning,
            // fingerprint serialization) consumes the ONE vector unchanged.
            // Appended AFTER base entries, features in map (= name) order, so
            // application order is deterministic and a feature rule wins over
            // a broader base rule via "last flag wins". An inactive feature
            // contributes nothing — its dead globs no longer exist to warn
            // about. Deliberately OUTSIDE any state.includeDevDeps gate: like the
            // sources ADD above, `mcpp build` and `mcpp test` must agree
            // (0.0.94 dual-path invariant). featureOrigin tags the entry so
            // the scanner's zero-hit warning can name the owning feature.
            //
            // Routed through the SAME append(BuildInputs&) the cfg axis uses
            // (#258): both axes are contributing additive build inputs, so
            // "how does a contribution combine with the base" must have one
            // answer. Only the flags half of the feature axis is expressible
            // that way — feature `sources` above carry DROP-then-ADD
            // semantics, and feature `defines` are interface contributions
            // that propagate along Public edges, so neither is a plain
            // append and neither belongs in BuildInputs.
            for (auto& [f, entries] : bc.featureFlags) {
                if (std::ranges::find(active, f) == active.end()) continue;
                mcpp::manifest::BuildInputs contribution;
                for (auto const& gf : entries) {
                    auto tagged = gf;
                    tagged.featureOrigin = f;
                    contribution.globFlags.push_back(std::move(tagged));
                }
                mcpp::manifest::append(bc, contribution);
            }
        };
        if (!state.packages.empty()) {
            auto rootReq = parse_feature_request(state.overrides.features);
            // Strict schema check: a requested feature must exist in the
            // target package's [features] table when one is declared (a
            // package with no [features] accepts any request — pure-define
            // usage). Covers backend= sugar (feature backend-<x>) too.
            auto unknown_requested = [](const mcpp::manifest::Manifest& pm,
                                        const std::vector<std::string>& requested)
                -> std::optional<std::string> {
                if (pm.featuresMap.empty()) return std::nullopt;
                for (auto& f : requested)
                    if (!pm.featuresMap.contains(f)) return f;
                return std::nullopt;
            };
            if (auto bad = unknown_requested(state.packages[0].manifest, rootReq)) {
                auto msg = std::format(
                    "--features requests '{}' which [features] does not declare", *bad);
                if (state.overrides.strict) return std::unexpected(msg);
                mcpp::diag::warning("features/request", msg);
            }
            apply(state.packages[0], rootReq);
            for (auto& f : activate(*state.m, rootReq)) state.activeRootFeatures.insert(f);
        }
        // #242/#243: the feature request for a dependency PACKAGE, aggregated
        // over ALL its incoming edges (a package may be depended on by several
        // consumers — diamond — or reached only transitively). Cargo semantics:
        // requested features UNION; default-features stays on unless EVERY
        // consumer opted out. Sourcing this from the authoritative edge graph —
        // rather than scanning only the root manifest's direct deps — makes
        // activation AGREE with resolution (mergeActiveFeatureDeps, which reads
        // the true per-edge spec): a transitive dep's requested features and its
        // consumer's `default-features = false` are no longer silently dropped.
        for (std::size_t i = 1; i < state.packages.size(); ++i) {
            auto& pname = state.packages[i].manifest.package.name;
            auto [req, depDefaultFeatures] = aggregatedRequest(state, i);
            if (!req.empty() && !state.packages[i].manifest.featuresMap.empty()) {
                for (auto& f : req) {
                    if (state.packages[i].manifest.featuresMap.contains(f)) continue;
                    auto msg = std::format(
                        "dependency '{}' does not declare requested feature '{}' "
                        "in its [features] table", pname, f);
                    if (state.overrides.strict) return std::unexpected(msg);
                    mcpp::diag::warning("features/request", msg);
                }
            }
            // Always apply: even with no requested/default feature, a dep with
            // feature-gated sources must have those sources dropped by default.
            // depDefaultFeatures carries the consumer's `default-features = false`
            // (#242): when opted out, the dep's [features].default is not seeded.
            apply(state.packages[i], req, depDefaultFeatures);
            if (state.activeFeaturesByPackage.size() <= i)
                state.activeFeaturesByPackage.resize(i + 1);
            state.activeFeaturesByPackage[i] =
                feature_closure(state.packages[i].manifest, req, depDefaultFeatures);
        }

    return {};
}

static std::expected<void, std::string> step6_device_extensions_and_rules(PrepareState& state) {
        // ─── Device extensions a rule dependency declared ──────────────────
        //
        // A rule package states which device extensions it compiles, on the
        // feature that provides the rule. Collected here, after features are
        // activated, because only an ACTIVE feature's declaration applies: a
        // collection carrying a CUDA rule and a shader rule must not make `.cu`
        // a device source in a project that asked for the shader rule alone.
        //
        // Written into the CONSUMER's `[build]` so every site that already
        // builds an extension table for a package picks it up without a second
        // plumbing route.
        //
        // THE POSITION IS LOAD-BEARING. It sits after feature activation and
        // before the extension table that narrows the constrained globs, which
        // is the first reader. Placed after that table instead, the declared
        // extensions arrive too late to classify anything: the device source
        // list comes out empty, the rule is handed nothing, it generates no
        // module, and the failure surfaces three edges away as `failed to read
        // compiled module` on the interface the consumer imported. Measured.
        //
        // It is what makes a new device language cost no engine change. Adding
        // `.slang` to the built-in table required an mcpp release and a version
        // bump in the rule package's CI before its rule could route one file;
        // a language arriving this way needs neither.
        // What each package's active rules claim, kept until its device
        // sources are known (#715, the filter below).
        struct RuleClaim {
            std::string              module;
            std::vector<std::string> extensions;
            std::string              provider;   // "<ns>:<name>", for the report
        };
        std::vector<std::vector<RuleClaim>> ruleClaims(state.packages.size());
        for (std::size_t ci = 0; ci < state.packages.size(); ++ci) {
            std::vector<std::string> collected;
            std::vector<std::string> ruleModules;
            for (auto const& edge : state.dependencyEdges) {
                if (edge.consumerPackageIndex != ci) continue;
                if (edge.dependencyPackageIndex >= state.packages.size()) continue;
                auto const& dep = state.packages[edge.dependencyPackageIndex];
                const auto& depFeatures =
                    edge.dependencyPackageIndex < state.activeFeaturesByPackage.size()
                        ? state.activeFeaturesByPackage[edge.dependencyPackageIndex]
                        : edge.requestedFeatures;
                for (auto const& f : depFeatures) {
                    auto it = dep.manifest.featureDeviceExtensions.find(f);
                    if (it == dep.manifest.featureDeviceExtensions.end()) continue;
                    for (auto const& e : it->second)
                        if (std::ranges::find(collected, e) == collected.end())
                            collected.push_back(e);
                    // The module a synthesised build program imports for this
                    // rule. Declared by the feature rather than scanned out of
                    // its source, because the program has to be WRITTEN before
                    // anything is compiled and a build that scanned a
                    // dependency to decide what to write would order the two
                    // the wrong way round.
                    if (auto mit = dep.manifest.featureRuleModule.find(f);
                        mit != dep.manifest.featureRuleModule.end()
                        && std::ranges::find(ruleModules, mit->second) == ruleModules.end()) {
                        ruleModules.push_back(mit->second);
                        ruleClaims[ci].push_back(RuleClaim{
                            mit->second, it->second,
                            std::format("{}:{}", dep.manifest.package.namespace_,
                                        dep.manifest.package.name)});
                    }
                }
            }
            if (!collected.empty()) {
                if (ci == 0) state.m->buildConfig.deviceExtensions = collected;
                state.packages[ci].manifest.buildConfig.deviceExtensions = std::move(collected);
            }
            if (!ruleModules.empty()) {
                // THE ROOT'S MANIFEST IS TWO OBJECTS. `packages[0]` holds a COPY
                // made by `makePackageRoot`, and the build-program environment for the
                // root reads `*m`. Writing only the copy left the synthesis with
                // an empty list and the shaders uncompiled, with a refusal that
                // named the missing build program rather than the missing write.
                if (ci == 0) state.m->buildConfig.ruleModules = ruleModules;
                state.packages[ci].manifest.buildConfig.ruleModules = std::move(ruleModules);
            }
        }

        // ── Constrained source globs: narrow to what this build targets ────
        //
        // A `{ glob = "...", accel = "..." }` entry in `[build] sources` says
        // what its files are FOR. Three outcomes, all decided here and none in
        // the scanner, which keeps reading a plain list of globs:
        //
        //   - the glob matches nothing: refused, naming the glob. An empty
        //     match is a typo or a moved directory, not a no-op, and the
        //     failure it would otherwise become is a kernel that is never
        //     compiled and a link that resolves nothing.
        //   - the build asks for no accelerator: the glob is EXCLUDED, with the
        //     same `!` mechanism feature gates use -- removing the string is not
        //     enough when a broader glob (the default `src/**`) covers the same
        //     files. This is how `--no-accel` yields the CPU-only variant.
        //   - the build asks for one: the constraint must lie within it, or the
        //     build is refused naming both. A file compiled for sm_89 under a
        //     build that targets sm_80 is not a variant, it is a mismatch.
        //
        // Device-kind files the effective set still matches are collected per
        // package for the build program (MCPP_DEVICE_SOURCES); the engine has
        // no compile rule for them and never will.
        {
            const auto buildAccel = mcpp::pack::parse_accel(state.resolvedAccel());
            for (std::size_t i = 0; i < state.packages.size(); ++i) {
                auto& pkg = state.packages[i];
                auto& bc  = pkg.manifest.buildConfig;
                std::set<std::string> excludedGlobs;
                for (auto const& sc : bc.sourceConstraints) {
                    const auto hits = mcpp::modgraph::expand_glob(pkg.root, sc.glob);
                    if (hits.empty()) {
                        return std::unexpected(std::format(
                            "`{}`: [build] sources entry '{}' (accel = \"{}\") matches no file.\n"
                            "       A constrained glob names the files a device build needs; an\n"
                            "       empty match would leave nothing to compile for that device\n"
                            "       and say so only at the link, or never.",
                            pkg.manifest.package.name, sc.glob, sc.accel));
                    }
                    // A backend the package never declared. Checked BEFORE
                    // the build's own accel is consulted, because it is a
                    // property of the manifest alone and because the exclusion
                    // below would otherwise turn `accel = "cude12.9"` into a
                    // glob that is quietly never built. Only when the package
                    // states its backends -- `[package] accelerators` is
                    // optional, and a package that omits it has said nothing to
                    // contradict.
                    if (!pkg.manifest.package.accelerators.empty()) {
                        for (auto const& w : mcpp::pack::parse_accel(sc.accel)) {
                            if (std::ranges::find(pkg.manifest.package.accelerators,
                                                  w.backend)
                                != pkg.manifest.package.accelerators.end()) continue;
                            std::string declared;
                            for (auto const& a : pkg.manifest.package.accelerators)
                                declared += (declared.empty() ? "" : ", ") + a;
                            refusal::record(refusal::Code::AccelBackendUndeclared);
                            return std::unexpected(std::format(
                                "`{}`: [build] sources entry '{}' names accelerator "
                                "backend \"{}\", which this package does not declare.\n"
                                "         [package] accelerators = [{}]\n"
                                "       A constrained glob is left out of builds that do "
                                "not name its\n"
                                "       backend, so a backend spelled wrong here is a file "
                                "that is never\n"
                                "       compiled and never mentioned.\n"
                                "       fix: correct the spelling, or add the backend to "
                                "`[package] accelerators`.",
                                pkg.manifest.package.name, sc.glob, w.backend, declared));
                        }
                    }
                    if (buildAccel.empty()) { excludedGlobs.insert(sc.glob); continue; }
                    const auto want = mcpp::pack::parse_accel(sc.accel);

                    // A GLOB WHOSE BACKEND THIS BUILD NEVER NAMED IS NOT A
                    // MISMATCH, IT IS ABSENT.
                    //
                    // The refusal below is about a real disagreement: a file
                    // written for sm_89 in a build that targets sm_80 is not a
                    // variant. Across DIFFERENT backends there is no such
                    // disagreement. A package with a CUDA island and a Vulkan
                    // one, built with `--accel vulkan1.2`, is asking for the
                    // Vulkan half; refusing it made a build that names a SUBSET
                    // of a package's backends impossible, so a package could
                    // have several device backends only if every build took all
                    // of them.
                    //
                    // The glob is dropped exactly as `--no-accel` drops it, and
                    // the `cfg(accelerator = ...)` section carrying that
                    // backend's host half does not activate either, so the two
                    // halves stay together.
                    //
                    // What keeps a TYPO from becoming a silent exclusion is the
                    // check below, against `[package] accelerators`: a backend
                    // the package never declared is refused before this point.
                    bool backendNamed = false;
                    for (auto const& w : want)
                        for (auto const& b : buildAccel)
                            if (b.backend == w.backend) backendNamed = true;
                    if (!backendNamed) { excludedGlobs.insert(sc.glob); continue; }

                    if (!mcpp::pack::accel_accepts(buildAccel, want)) {
                        refusal::record(refusal::Code::AccelMismatch);
                        return std::unexpected(std::format(
                            "`{}`: [build] sources entry '{}' is constrained to accel \"{}\",\n"
                            "       which this build does not cover.\n"
                            "         this build targets: {}\n"
                            "       fix: build with `--accel` covering it, or `--no-accel` to\n"
                            "       leave every constrained glob out (the CPU-only variant).",
                            pkg.manifest.package.name, sc.glob,
                            mcpp::pack::accel_str(want),
                            mcpp::pack::accel_str(buildAccel)));
                    }
                }
                for (auto const& g : excludedGlobs) {
                    bc.sources.push_back("!" + g);
                    pkg.manifest.modules.sources.push_back("!" + g);
                }
                // The device-kind files the EFFECTIVE set matches, for the
                // build program. Exclusions are honoured the way the scanner
                // honours them: positives first, then `!` entries removed.
                const auto extTable = mcpp::extension_table_for(bc.moduleExtensions,
                                                                bc.deviceExtensions);
                std::set<std::filesystem::path> matched, dropped;
                for (auto const& g : pkg.manifest.modules.sources) {
                    if (g.empty()) continue;
                    if (g[0] == '!') { for (auto& f : mcpp::modgraph::expand_glob(pkg.root, g.substr(1))) dropped.insert(f); }
                    else if (!std::filesystem::path(g).is_absolute())
                        for (auto& f : mcpp::modgraph::expand_glob(pkg.root, g)) matched.insert(f);
                }
                std::vector<std::string> device;
                for (auto const& f : matched) {
                    if (dropped.contains(f)) continue;
                    if (mcpp::classify(f, extTable) != mcpp::SourceKind::Device) continue;
                    device.push_back(f.lexically_relative(pkg.root).generic_string());
                }
                state.deviceSourcesByPackage[pkg.root.string()] = std::move(device);
            }
        }

        // ── A rule applies to a package through a source it claims (#715) ──
        //
        // A feature activates a rule for the consumer; whether the rule has
        // anything to do there is answered by the consumer's sources. The
        // synthesised build program used to be written for every active rule,
        // so a package that only imports Qt -- no `.ui`, `.qrc` or `.ts`, no
        // `build.mcpp` -- compiled and ran a program that could only report
        // "nothing to do", on every configure. A rule now reaches the program
        // only when one of the package's device sources has an extension the
        // rule declared, classified by the same table the source scan uses. A
        // device source no active rule claims is still the orphan refused
        // below, and a package that wants a rule to run without claimed
        // sources writes its own `build.mcpp`.
        for (std::size_t ci = 0; ci < state.packages.size(); ++ci) {
            if (ruleClaims[ci].empty()) continue;
            auto& bc = state.packages[ci].manifest.buildConfig;
            const auto dit = state.deviceSourcesByPackage.find(state.packages[ci].root.string());
            std::vector<std::string> applies;
            for (auto const& claim : ruleClaims[ci]) {
                const auto table = mcpp::extension_table_for(bc.moduleExtensions,
                                                             claim.extensions);
                const bool claimed = dit != state.deviceSourcesByPackage.end()
                    && std::ranges::any_of(dit->second, [&](const std::string& rel) {
                           return mcpp::classify(std::filesystem::path(rel), table)
                               == mcpp::SourceKind::Device;
                       });
                if (!claimed) continue;
                applies.push_back(claim.module);
                // Said out loud, for the same reason the resolved toolchain is:
                // the manifest states the intent and the build states what that
                // came to.
                mcpp::ui::info("Rules", std::format("{} ({})", claim.module, claim.provider));
            }
            if (ci == 0) state.m->buildConfig.ruleModules = applies;
            bc.ruleModules = std::move(applies);
        }
        state.activeFeaturesByPackage.resize(state.packages.size());
    return {};
}

static std::expected<void, std::string> step6_xlings_workspace_from_graph(PrepareState& state) {
        // ── The GRAPH's `[xlings.workspace]`, provisioned BEFORE build.mcpp ──
        //
        // Same ordering rule as the host-tool block directly below, and for the
        // same reason: a build program consumes what was provisioned, so
        // provisioning after it has run is provisioning that did not happen.
        //
        // MEASURED, on the published `ggml-org:llamacpp@b10069.2`. That package
        // declares its shader compiler under the feature that needs it:
        //
        //     [feature-xlings.backend-vulkan]
        //     "xim:shaderc" = "2026.3"
        //
        // and its build program asks for it with `xpkg_dir`. As the ROOT it
        // works, because the root's pass runs early. As a DEPENDENCY it did
        // not: the graph's pass ran ~1700 lines further down, after every
        // build.mcpp, so `xpkg_dir` answered "" and the package refused its own
        // headline feature with the very declaration it had already made. A
        // clean `MCPP_HOME` pulled twenty-four xim payloads for that graph and
        // not shaderc.
        //
        // IT WAS INVISIBLE ON ANY MACHINE THAT HAD BUILT THE PACKAGE ITSELF.
        // Once `xim:shaderc` is in the registry for any reason, `xpkg_dir`
        // finds it and the ordering stops mattering; only an empty registry can
        // see this. The sandbox run is what caught it.
        //
        {
            // ONE CHECK OVER THE WHOLE GRAPH, at the site whose consequence it
            // describes. `merge_conditional_config` has three call sites and
            // returns void; this loop sees the root and every package that
            // reached the graph, and it runs before the first payload is
            // fetched, so a refusal costs nothing that has to be undone.
            if (auto why = layer_predicated_xlings_refusal(*state.runtimeOwnerManifest))
                return std::unexpected(*why);
            for (auto const& pkg : state.packages)
                if (auto why = layer_predicated_xlings_refusal(pkg.manifest))
                    return std::unexpected(*why);
            auto split = state.graph_xlings_split();
            if (!split) {
                refusal::record(refusal::Code::ToolVersionConflict);
                return std::unexpected(split.error());
            }
            auto const& fromGraph = split->second;
            if (!fromGraph.empty()) {
                if (auto cfg = state.get_cfg(true)) {
                    if (auto pv = provision_xlings_addresses(
                            **cfg, fromGraph, *state.root,
                            "[xlings.workspace] entries declared by dependencies");
                        !pv) return std::unexpected(pv.error());
                }
            }
        }
    return {};
}

// Every host module one package contributes, the lib root first.
//
// The lib root is what a rule package has always been: one unit,
// compiled alone, registered under the name it declares. A package
// that offers several rules through features (mcpp 2026.9.5.3+)
// lists their sources under `[features.<f>] sources`, and those
// globs have been folded into `buildConfig.sources` by now for
// exactly the features the consumer activated. Every module
// INTERFACE unit among them is therefore a host module of its own,
// under its own declared name, and nothing else in the host-module
// path assumes one unit per package: `build_host_module` is per
// unit and the compile loop accumulates BMIs in list order, so a
// feature unit may import the lib root, which precedes it.
//
// Only sources the manifest LISTS take part. The inferred `src/**`
// of a package with no `sources` is not consulted, so a rule
// package published before this round exposes exactly what it
// exposed then; widening that implicitly would compile units that
// were written to be part of an ordinary library, alone.
//
// Split out of step6_host_module_registration (#734).
static std::vector<prov::HostModule> host_module_units(const PrepareState& st, std::size_t p) {
    auto identity = [&](std::size_t q) {
        auto const& pkg = st.packages[q].manifest.package;
        return pkg.namespace_.empty() ? pkg.name : pkg.namespace_ + "." + pkg.name;
    };
    auto const& depPkg = st.packages[p];
    auto const& pkg    = depPkg.manifest.package;
    std::vector<prov::HostModule> out;
    auto push = [&](std::filesystem::path iface, std::string name) {
        prov::HostModule hm;
        hm.module    = std::move(name);
        hm.package   = identity(p);
        hm.nameSpace = pkg.namespace_;
        hm.interface = std::move(iface);
        out.push_back(std::move(hm));
    };
    // PROBING form: a host-module dependency whose interface is
    // `.ixx` resolves to a `src/<tail>.cppm` that does not exist,
    // and the consumer's build.mcpp is then handed a path to
    // nothing.
    auto rel   = mcpp::manifest::resolve_lib_root_path(
        depPkg.manifest, depPkg.root);
    auto iface = depPkg.root / rel;
    auto rootName = prov::host_module_name(iface, pkg.name);
    // A missing lib root is reported as such by build_host_module,
    // and that has to stay the diagnostic. Enumerating the listed
    // units first would let one of them collide with the missing
    // root's fallback name and report a collision between a file
    // and a file that does not exist.
    std::error_code ec;
    if (!std::filesystem::exists(iface, ec)) {
        push(iface, std::move(rootName));
        return out;
    }

    std::set<std::filesystem::path> matched, dropped;
    for (auto const& g : depPkg.manifest.buildConfig.sources) {
        if (g.empty()) continue;
        if (g[0] == '!') {
            for (auto& f : mcpp::modgraph::expand_glob(depPkg.root, g.substr(1)))
                dropped.insert(f.lexically_normal());
        } else {
            for (auto& f : mcpp::modgraph::expand_glob(depPkg.root, g))
                matched.insert(f.lexically_normal());
        }
    }
    const auto root = iface.lexically_normal();
    // ORDERED BY WHAT THEY IMPORT, NOT BY WHERE THEY SIT.
    //
    // The compile loop accumulates BMIs in list order, so each
    // entry sees only what precedes it. Path order was the previous
    // rule and it is not a valid one: `rules/spirv.cppm` sorts
    // before `src/surface.cppm`, so a member importing a unit its
    // package shares was compiled first and failed with "failed to
    // read compiled module ... imports must be built before being
    // imported". Reproduced, and reproduced in both directions --
    // renaming the shared unit so its path sorted first made the
    // same package build, which is what says the cause is the sort
    // and nothing else.
    //
    // A package that works today is ordered IDENTICALLY: the sort
    // below keeps path order wherever no import constrains it, so
    // it differs only where the old order was already broken.
    struct Unit {
        std::filesystem::path        path;
        std::string                  name;
        std::vector<std::string>     imports;
    };
    // The lib root is the first node of the same sort (mcpp#720).
    // Placing it ahead of the sort assumed that it imports no
    // other unit of its package; a root that does was compiled
    // before the unit it imports and failed with "module not
    // found". As the first node it is still emitted first whenever
    // it imports nothing of its own package, so the order of every
    // package that built before is unchanged.
    std::vector<Unit> pending;
    {
        std::ifstream is(root);
        std::stringstream buf;
        if (is) buf << is.rdbuf();
        pending.push_back({root, std::move(rootName),
                           prov::declared_imports(buf.str())});
    }
    for (auto const& f : matched) {          // std::set: sorted
        if (dropped.contains(f)) continue;
        if (std::filesystem::equivalent(f, root, ec)) continue;
        std::ifstream is(f);
        if (!is) continue;
        std::stringstream buf;
        buf << is.rdbuf();
        auto text = buf.str();
        auto name = prov::declared_interface_name(text);
        if (name.empty()) continue;
        pending.push_back({f, std::move(name), prov::declared_imports(text)});
    }

    // Only names this package itself declares constrain anything.
    // `import std;` is ahead of every entry here, and a name from
    // another package is ordered by the cross-package DFS below
    // rather than by this sort.
    std::map<std::string, std::size_t> byName;
    for (std::size_t i = 0; i < pending.size(); ++i)
        byName.emplace(pending[i].name, i);

    // deps[u] = the pending-list indices `u` imports BY NAME within this
    // same package, in import order. A depth-first order from every unit in
    // path order emits the first unit that can be emitted, which is what
    // preserves path order in the unconstrained case, and is the order the
    // build program's objects are linked in.
    mcpp::graph::AdjacencyList deps(pending.size());
    for (std::size_t u = 0; u < pending.size(); ++u)
        for (auto const& want : pending[u].imports)
            if (auto it = byName.find(want); it != byName.end())
                deps[u].push_back(it->second);
    std::vector<std::size_t> roots(pending.size());
    std::iota(roots.begin(), roots.end(), std::size_t{0});

    // A CYCLE IS LEFT TO THE COMPILER, ON PURPOSE. It is ill-formed C++ and
    // the compiler says so with the two units named; refusing here would
    // report the same fact in a worse place, and getting the ordering wrong
    // is no longer possible either way -- so the edge that closes a cycle is
    // skipped.
    std::vector<std::size_t> order;
    order = *mcpp::graph::depth_first_order(deps, roots, mcpp::graph::Cycles::Skip);

    for (auto i : order) push(pending[i].path, std::move(pending[i].name));
    return out;
}

static std::expected<std::map<std::size_t, std::set<std::string>>, std::string>
step6_host_module_registration(PrepareState& state) {
        // ── #355: HOST tool provisioning ────────────────────────────────────
        //
        // Runs AFTER feature activation (a tool target's gate is a feature) and
        // BEFORE any build.mcpp (which is what consumes the tools). That
        // ordering is the whole point: build.mcpp runs inside prepare, so a
        // tool produced by the main ninja graph would arrive far too late —
        // and under --target it would be the wrong architecture besides.
        //
        // Each tool is built by re-entering prepare_build with the DEPENDENCY
        // as the root and no --target, i.e. for the build machine. That is
        // Cargo's [build-dependencies] / Bazel's exec configuration shape.
        // It is affordable because an executable has zero ABI contact with the
        // main build: the sub-build may use the tool package's own toolchain,
        // its own profile, and its own resolution — none of it has to agree
        // with the consumer.
            // Aggregate off the authoritative edge graph, exactly like feature
            // activation — a transitive consumer's request must not be
            // silently dropped (#242/#243).
            // A FEATURE'S TOOLS ARE REQUESTED ON EVERY EDGE INTO ITS PACKAGE
            // (#709). `[features.<f>] tools` states that enabling `f` needs
            // those programs, so a consumer enabling it receives them exactly
            // as if its edge had written `tools = [...]`. Features are unified
            // per package, so the set is the package's active features, the
            // same set `[feature-xlings]` is answered from. Added before the
            // aggregation below, so building, visibility (`dep_bin`) and the
            // store key are the edge-requested tool's in every respect.
            for (auto& edge : state.dependencyEdges) {
                const auto d = edge.dependencyPackageIndex;
                if (d >= state.packages.size() || d >= state.activeFeaturesByPackage.size()) continue;
                auto const& ft = state.packages[d].manifest.featureTools;
                if (ft.empty()) continue;
                for (auto const& f : state.activeFeaturesByPackage[d])
                    if (auto it = ft.find(f); it != ft.end())
                        for (auto const& t : it->second)
                            if (std::ranges::find(edge.requestedTools, t)
                                == edge.requestedTools.end())
                                edge.requestedTools.push_back(t);
            }
            std::map<std::size_t, std::set<std::string>> toolRequests;
            for (auto const& edge : state.dependencyEdges)
                for (auto const& t : edge.requestedTools)
                    toolRequests[edge.dependencyPackageIndex].insert(t);

            // #359: one fixpoint decides who SEES what. `toolRequests` above
            // still decides what gets BUILT — the two questions are separate,
            // and conflating them is what made a re-exported tool impossible:
            // the tool was built, but its path was recorded against the library
            // that asked for it rather than the project that needs it.
            state.provisionGraph = prov::propagate(state.dependencyEdges, state.packages.size());

            // #355 step 5: dependencies offering HOST build rules. Nothing is
            // compiled here — the interface is handed to build_program.cppm,
            // which compiles it in the SAME command as build.mcpp so the BMI
            // and its consumer agree on standard, dialect and compiler by
            // construction rather than by luck.
            //
            // Driven off the visible set rather than the root manifest, so a
            // rule a library re-exports is importable from the consumer's
            // build.mcpp without the consumer naming it. The name matching the
            // old loop needed is gone with it: the edge already knows which
            // package it points at.
            //
            // The registered name is the one the rule's SOURCE declares, not
            // the package's name. See provisions::host_module_name for why the
            // two had drifted apart and what that cost on Clang and MSVC.
            std::set<std::string> prefixWarned;
            // Providers of host modules that THIS package sees directly.
            auto directHostProviders = [&](std::size_t p) {
                std::vector<std::size_t> out;
                if (p >= state.provisionGraph.visible.size()) return out;
                for (auto const& pr : state.provisionGraph.visible[p]) {
                    if (pr.kind != prov::Kind::HostModule) continue;
                    if (pr.provider >= state.packages.size()) continue;
                    out.push_back(pr.provider);
                }
                return out;
            };
            auto identity = [&](std::size_t p) {
                auto const& pkg = state.packages[p].manifest.package;
                return pkg.namespace_.empty()
                     ? pkg.name : pkg.namespace_ + "." + pkg.name;
            };
            auto units = [&](std::size_t p) { return host_module_units(state, p); };
            // Host-module provider edges over EVERY package, built once:
            // pkgHostDeps[p] is what directHostProviders(p) computes, reused
            // below instead of re-querying provisionGraph per consumer.
            mcpp::graph::AdjacencyList pkgHostDeps(state.packages.size());
            for (std::size_t p = 0; p < state.packages.size(); ++p)
                pkgHostDeps[p] = directHostProviders(p);
            for (std::size_t c = 0; c < state.provisionGraph.visible.size(); ++c) {
                const auto direct = directHostProviders(c);
                if (direct.empty()) continue;
                std::set<std::size_t> isDirect(direct.begin(), direct.end());

                // #734 E7: what each direct provider could offer with a feature
                // it does not have enabled here. Globs are expanded, nothing is
                // read: the module names are looked up only if build.mcpp
                // imports something no provider offers.
                for (auto p : direct) {
                    auto const& dm = state.packages[p].manifest;
                    const auto& active = p < state.activeFeaturesByPackage.size()
                        ? state.activeFeaturesByPackage[p] : std::vector<std::string>{};
                    for (auto const& [fname, globs] : dm.buildConfig.featureSources) {
                        if (std::ranges::find(active, fname) != active.end()) continue;
                        mcpp::build::BuildProgramEnv::DormantFeature df;
                        df.package = identity(p);
                        df.feature = fname;
                        for (auto const& g : globs)
                            for (auto& f : mcpp::modgraph::expand_glob(state.packages[p].root, g))
                                df.files.push_back(f);
                        if (!df.files.empty())
                            state.dormantFeaturesByConsumer[c].push_back(std::move(df));
                    }
                }

                // Post-order DFS, so a rule's own host modules are compiled
                // BEFORE it. That ordering is the entire mechanism: the
                // compile loop in build_program.cppm accumulates the module
                // flags as it goes, so each entry sees the BMIs of everything
                // ahead of it, and "a rule may import another rule" needs no
                // second machinery — only this sort. It walks only what `c`
                // reaches, so a cycle elsewhere never fails `c`'s build.
                auto topo = mcpp::graph::depth_first_order(pkgHostDeps, direct);
                if (!topo) {
                    // A cycle, reported AS a cycle and naming the packages
                    // on it. A depth limit would answer a different
                    // question and would answer it later.
                    std::string ring;
                    bool first = true;
                    for (auto q : topo.error().cycle) {
                        if (!first) ring += " -> ";
                        first = false;
                        ring += identity(q);
                    }
                    return std::unexpected(std::format(
                        "build rules form an import cycle: {}\n"
                        "       A rule's host modules are compiled before "
                        "it, so a cycle has no order that could satisfy "
                        "all of them.", ring));
                }

                std::vector<prov::HostModule> ordered;
                // The package each entry of `ordered` came from, for where its
                // compiled module may be kept (#748, B1).
                std::vector<std::size_t> orderedProvider;
                for (auto p : *topo) {
                    for (auto& hm : units(p)) {
                        hm.importable = isDirect.contains(p);
                        ordered.push_back(std::move(hm));
                        orderedProvider.push_back(p);
                    }
                }

                // Every provider on this consumer's rule closure, transitive
                // ones included, and a rule imported by another rule declares
                // payloads just as directly.
                std::set<std::size_t> reached(topo->begin(), topo->end());
                state.hostModuleProvidersByConsumer[c].assign(reached.begin(), reached.end());

                if (auto clash = prov::host_module_collision(ordered))
                    return std::unexpected(*clash);
                for (std::size_t k = 0; k < ordered.size(); ++k) {
                    auto const& hm = ordered[k];
                    // Warned once per (package, module), not once per consumer:
                    // a rule re-exported down a chain is visible to every
                    // package on it, and repeating one naming remark N times
                    // reads as N problems.
                    if (auto w = prov::reserved_prefix_warning(
                            hm.module, hm.nameSpace, hm.package)) {
                        if (prefixWarned.insert(hm.package + "\x1e" + hm.module).second)
                            mcpp::diag::warning("build/rule-namespace", *w);
                    }
                    mcpp::build::BuildProgramEnv::HostModuleRef ref{
                        hm.module, hm.interface, hm.importable};
                    // Where the compiled module may be kept (#748, B1): in the
                    // global cache only when the provider is an index package
                    // whose sources are in the immutable store, the rule the
                    // dependency cache applies to a package (plan.cpp). The
                    // module is compiled alone, so nothing it was built against
                    // can be local: the provider's own location decides.
                    {
                        const auto pi = orderedProvider[k];
                        const auto& provider = state.packages[pi];
                        const auto* ident = pi >= 1 && pi - 1 < state.dep_cache_identities.size()
                            ? &state.dep_cache_identities[pi - 1] : nullptr;
                        ref.providerName = identity(pi);
                        ref.providerVersion = provider.manifest.package.version;
                        ref.providerRoot = provider.root;
                        if (ident && ident->sourceKind == "version"
                            && mcpp::build::path_is_under_any(provider.root, state.storeRoots)) {
                            ref.immutableSource = true;
                            ref.providerIndex   = ident->indexName;
                            ref.providerName    = ident->packageName;
                            if (!ident->version.empty()) ref.providerVersion = ident->version;
                        }
                    }
                    state.hostModulesByConsumer[c].push_back(std::move(ref));
                }
            }

            // A build rule is BUILD-TIME ONLY. Registering the module is not
            // enough: the package is still an ordinary node of the consumer's
            // graph, so its interface was ALSO compiled as a normal library and
            // linked into the target. That is wrong on its own terms — a rule
            // has no business in the consumer's binary — and it made the
            // feature nearly unusable, because in that second compile the
            // bundled `mcpp` module does not exist: any rule that actually used
            // the API it exists to wrap died with `fatal error: module 'mcpp'
            // not found` (2026.8.5.1).
            //
            // Emptying the source globs is how a package is removed from the
            // compile set here — the same mechanism the feature-gated-sources
            // drop above uses. Resolution is untouched: the package still lands
            // on disk, which is what `resolve_lib_root_path` just read.
            //
            // Guarded on EVERY edge into the package being a host-module edge.
            // A package can legitimately be both a rule and a library, and
            // silently dropping its objects then would surface as an undefined
            // reference far from here. (The predicate used to be "no consumer
            // other than the root", which said the same thing only while the
            // root was the only possible requester.)
            //
            // Stated as FORWARD REACHABILITY rather than as exclusion, and the
            // difference is not cosmetic.
            //
            // The predicate used to be per-edge: "every in-edge into this
            // package is a host-module edge". That is right about the rule
            // itself and wrong about everything BEHIND it — a rule's own
            // `[dependencies]` are reached by ordinary edges, so they kept
            // their globs and were compiled and LINKED INTO THE CONSUMER'S
            // BINARY, while the rule could not even import them. Both halves
            // were wrong, and the sharper harm was that a rule's dependency
            // versions took part in the consumer's real resolution, so a rule
            // could create a version conflict in a project that never asked
            // for one.
            //
            // Exclusion would also get the dual-role case backwards. A package
            // the project depends on directly must stay in the target even
            // when some rule's build dependencies also reach it: the
            // build-time path never subtracts from what the project asked to
            // link. Asking "can the target reach it" answers both cases with
            // one rule and no special case.
            {
                auto reachable = [&](bool targetEdgesOnly) {
                    std::vector<bool> seen(state.packages.size(), false);
                    if (state.packages.empty()) return seen;
                    std::vector<std::size_t> stack{0};
                    seen[0] = true;
                    while (!stack.empty()) {
                        auto p = stack.back();
                        stack.pop_back();
                        for (auto const& e : state.dependencyEdges) {
                            if (e.consumerPackageIndex != p) continue;
                            if (targetEdgesOnly && (e.hostModule || e.buildOnly))
                                continue;
                            auto d = e.dependencyPackageIndex;
                            if (d >= seen.size() || seen[d]) continue;
                            seen[d] = true;
                            stack.push_back(d);
                        }
                    }
                    return seen;
                };
                const auto viaTarget = reachable(/*targetEdgesOnly=*/true);
                const auto viaAny    = reachable(/*targetEdgesOnly=*/false);
                for (std::size_t d = 1; d < state.packages.size(); ++d) {
                    // `viaAny` is the guard that keeps this from acting on a
                    // package no edge ever mentioned. Such a package is a
                    // bookkeeping gap, not a build dependency, and clearing
                    // its sources would turn that gap into an undefined
                    // reference a long way from here.
                    if (viaTarget[d] || !viaAny[d]) continue;
                    auto& dm = state.packages[d].manifest;
                    dm.buildConfig.sources.clear();
                    dm.buildConfig.featureSources.clear();
                    dm.modules.sources.clear();
                }
            }

            if (state.overrides.tool_depth >= mcpp::build::tool_store::kMaxDepth
                && !toolRequests.empty()) {
                return std::unexpected(std::format(
                    "tool provisioning nested more than {} levels deep — this is "
                    "almost certainly a cycle.\n  chain: {}",
                    mcpp::build::tool_store::kMaxDepth, state.overrides.tool_chain));
            }

    return toolRequests;
}

// A phase-local struct passed by reference to the steps of ONE requested
// host tool, the same way WorklistItemCtx (graph.cpp) is passed to the steps
// of one worklist item and TargetSideGather (target_side.cpp) to the steps
// of one target. Each field is a local the original single-function loop
// body declared once and read again in a later part of the same tool's
// provisioning.
struct HostToolCtx {
    std::size_t depIdx = 0;
    std::string toolName;
    std::string depName;
    std::string depShort;
    const mcpp::manifest::Target* tgt = nullptr;
    prov::Provision want{};
    std::string toolSource;
    std::vector<std::string> closure;
    std::string toolTcSpec;
    mcpp::build::tool_store::Key key;
    std::filesystem::path cacheRoot;
    std::filesystem::path entry;
    std::filesystem::path binOut;
    BuildOverrides sub;
    std::filesystem::path goal;
    std::filesystem::path subOutputDir;
};

// The body of the `record` closure the single-function version of this step
// captured per requested tool. It is called from four sites below (an
// override, a store cache hit, a deferred plan-only tool, and a freshly
// built one), so it is a named helper taking the context rather than a
// per-tool closure.
static void step6_record_tool_provision(PrepareState& state, HostToolCtx& ctx,
                                         const std::filesystem::path& p) {
    for (std::size_t c = 0; c < state.provisionGraph.visible.size(); ++c) {
        if (!state.provisionGraph.visible[c].contains(ctx.want)) continue;
        auto& v = state.toolEnvByConsumer[c];
        std::vector<std::string> vars;
        for (auto const& n : state.publishedNamesFor(ctx.depIdx, state.bareBindingsFor(c))) {
            auto var = mcpp::build::tool_store::env_var_name(n, ctx.toolName);
            if (std::ranges::find(vars, var) != vars.end()) continue;
            vars.push_back(var);
            v.emplace_back(std::move(var), p.string());
        }
    }
}

static std::expected<void, std::string>
step6_resolve_tool_target(PrepareState& state, HostToolCtx& ctx) {
    auto& depPkg = state.packages[ctx.depIdx];
    // The target must exist and be a binary. Naming the
    // alternatives matters: the consumer wrote a string, and a
    // typo is the likeliest cause.
    //
    // #622 A3: deliberately still `Binary`, not `is_program()`.
    // A host tool is exec'd directly ON THE BUILD MACHINE
    // during THIS build, so it is "literally an executable
    // link" — the question this site was already asking — and
    // an `app` whose row form happened to be a library (never
    // the host row in practice, but the check would be a
    // silent trap if the host itself were ever Android) could
    // not stand in for it. A build-time tool is declared
    // `kind = "bin"`; that is what the word means here.
    std::string binList;
    for (auto const& t : depPkg.manifest.targets) {
        if (t.kind != mcpp::manifest::Target::Binary) continue;
        if (!binList.empty()) binList += ", ";
        binList += t.name;
        if (t.name == ctx.toolName) ctx.tgt = &t;
    }
    if (!ctx.tgt) {
        // A package may declare a bin target on some platforms
        // only. When the request came from a LIBRARY rather
        // than from the user, the user cannot edit it away, so
        // point at the knob that library needs (#359 D3a).
        return std::unexpected(std::format(
            "dependency '{}' has no `kind = \"bin\"` target named "
            "'{}' (requested via tools = [...]).\n"
            "  available bin targets: [{}]\n"
            "  If the requesting package is a library, it can "
            "scope the request per platform with\n"
            "  [target.'cfg(...)'.feature-deps.<feature>].",
            ctx.depName, ctx.toolName,
            binList.empty() ? std::string("none") : binList));
    }
    return {};
}

// Returns true when the tool is already resolved (an escape-hatch override
// found and recorded), in which case the caller's per-tool work is done.
static std::expected<bool, std::string>
step6_check_tool_override(PrepareState& state, HostToolCtx& ctx) {
    // Escape hatch first: it is the cheapest resolution and the
    // one a user reaches for precisely when building is not an
    // option. Deliberately not part of the store key — see
    // tool_store.cppm.
    if (auto ovr = mcpp::build::tool_store::find_override(
            *state.m, ctx.depName, ctx.depShort, ctx.toolName)) {
        if (!std::filesystem::exists(*ovr)) {
            return std::unexpected(std::format(
                "tool override for '{}:{}' points at '{}', which "
                "does not exist", ctx.depName, ctx.toolName, ovr->string()));
        }
        mcpp::ui::info("Tool", std::format(
            "{}:{} → {} (override)", ctx.depName, ctx.toolName, ovr->string()));
        step6_record_tool_provision(state, ctx, *ovr);
        return true;
    }
    return false;
}

static std::expected<void, std::string>
step6_check_tool_self_request(PrepareState& state, HostToolCtx& ctx) {
    auto& depPkg = state.packages[ctx.depIdx];
    // A TOOL WHOSE OWN BUILD REQUESTS IT AGAIN IS REFUSED AT
    // THE FIRST REPETITION (#649 E6). The depth bound below
    // caught it only after four nested sub-builds, with the
    // same prefix repeated four times and no word about which
    // edge asked. The edge is the one whose request reached
    // this package in THIS graph.
    ctx.toolSource = std::format(
        "{}|{}", depPkg.root.lexically_normal().generic_string(), ctx.toolName);
    if (std::ranges::find(state.overrides.tool_chain_sources, ctx.toolSource)
        != state.overrides.tool_chain_sources.end()) {
        std::string askedBy;
        for (auto const& edge : state.dependencyEdges) {
            if (edge.dependencyPackageIndex != ctx.depIdx) continue;
            if (std::ranges::find(edge.requestedTools, ctx.toolName)
                == edge.requestedTools.end()) continue;
            if (edge.consumerPackageIndex < state.packages.size()) {
                askedBy = mcpp::build::qualified_package_name(
                    state.packages[edge.consumerPackageIndex].manifest);
                break;
            }
        }
        return std::unexpected(std::format(
            "the host tool '{}:{}' is requested by its own build: "
            "{} -> {}:{}.\n"
            "       The request comes from '{}', which the tool's "
            "sub-build resolves with the feature or dependency that "
            "asks for the tool.\n"
            "       fix: the tool's own graph must not activate "
            "that request (a feature it does not enable, or a "
            "`[target.<sel>.feature-deps]` row it does not match).",
            ctx.depName, ctx.toolName,
            state.overrides.tool_chain.empty() ? "root" : state.overrides.tool_chain,
            ctx.depName, ctx.toolName,
            askedBy.empty() ? std::string("a package of its graph") : askedBy));
    }
    return {};
}

// Returns true when the tool is already resolved (a valid store entry, or a
// plan-only deferral), in which case the caller's per-tool work is done.
static std::expected<bool, std::string>
step6_resolve_tool_key(PrepareState& state, HostToolCtx& ctx) {
    auto& depPkg = state.packages[ctx.depIdx];
    // Build it. The feature set is the tool package's own
    // defaults PLUS the target's required_features — in a tool
    // sub-build the target is what was ASKED FOR, so its
    // requirements are inputs rather than a gate. (Same field,
    // opposite resolution direction; docs/05 says so.)
    std::vector<std::string> feats = ctx.tgt->requiredFeatures;
    ctx.closure = feature_closure(depPkg.manifest, feats, true);

    // WHICH COMPILER BUILDS THE TOOL IS DECIDED HERE, ONCE
    // (#710). The key used to record this build's host
    // toolchain while the sub-build chose its own -- the tool
    // package's `[toolchain]`, else the global default -- so an
    // entry could name gcc 15.1 over a binary gcc 16.1 had
    // produced, and a member tool built for a consumer used a
    // different compiler than `mcpp build -p <tool>`. The
    // choice is `--toolchain` when given, else the tool
    // package's own (its workspace's, for a member), else the
    // compiler this build compiles its build programs with. It
    // is handed to the sub-build as an override and recorded in
    // the key, so the two cannot disagree.
    if (const char* e = std::getenv("MCPP_TOOLCHAIN"); e && *e)
        ctx.toolTcSpec = e;
    else if (auto own = host_tool_declared_toolchain(
                 depPkg.manifest, depPkg.root, kCurrentPlatform))
        ctx.toolTcSpec = *own;
    std::string compilerIdentity;
    if (ctx.toolTcSpec.empty()) {
        auto hostTc = state.host_tc_for_build_program();
        if (!hostTc) return std::unexpected(hostTc.error());
        ctx.toolTcSpec = state.host_spec_for_build_program();
        compilerIdentity = std::format("{}|{}|{}",
            hostTc->second.label(), hostTc->second.version,
            hostTc->first.string());
    } else {
        compilerIdentity = "spec|" + ctx.toolTcSpec;
    }

    ctx.key.indexName = ctx.depIdx >= 1 && ctx.depIdx - 1 < state.dep_cache_identities.size()
                  ? state.dep_cache_identities[ctx.depIdx - 1].indexName
                  : std::string(mcpp::pm::kDefaultNamespace);
    ctx.key.packageName      = ctx.depName;
    // THE VERSION IDENTIFIES THE SOURCES ONLY FOR AN INDEX
    // PACKAGE. A `git` package is keyed by its commit and a
    // `path` package by a stamp of its tree, because both
    // change under an unchanged version and the store then
    // serves a binary built from sources that no longer exist
    // (#630, item 6; measured 2026-09-08 with examples/12).
    // The same rule applies to every upstream below.
    auto source_keyed_version = [&](std::size_t pkgIdx) {
        const auto& man = state.packages[pkgIdx].manifest.package;
        std::string v = man.version;
        if (pkgIdx >= 1 && pkgIdx - 1 < state.dep_cache_identities.size()) {
            const auto& id = state.dep_cache_identities[pkgIdx - 1];
            if (id.sourceKind == "git" && !id.sourceRef.empty())
                v += "+git." + id.sourceRef;
            else if (id.sourceKind == "path")
                v += "+path." + mcpp::build::tool_store::tree_stamp(
                    id.sourceRef.empty() ? state.packages[pkgIdx].root
                                         : std::filesystem::path(id.sourceRef));
        }
        return v;
    };
    ctx.key.version          = source_keyed_version(ctx.depIdx);
    ctx.key.targetName       = ctx.toolName;
    ctx.key.hostTriple       = mcpp::toolchain::triple::host_triple().str();
    ctx.key.compilerIdentity = compilerIdentity;
    ctx.key.profile          = "release";
    ctx.key.features         = ctx.closure;
    std::ranges::sort(ctx.key.features);
    // The tool package's TRANSITIVE dependency closure, not just
    // its direct edges. Direct-only would be enough for index
    // packages (a frozen version cannot change its own deps),
    // but a path dependency can: bump something two levels down
    // and the tool's direct list is unchanged, so a stale binary
    // stays in the store — a silently wrong artifact.
    for (auto up : dg::transitive_dependencies(state.dependencyEdges, ctx.depIdx))
        ctx.key.upstreamKeys.push_back(std::format("{}@{}",
            state.packages[up].manifest.package.name,
            source_keyed_version(up)));
    std::ranges::sort(ctx.key.upstreamKeys);

    ctx.cacheRoot = mcpp::home::cache_root();
    ctx.entry     = mcpp::build::tool_store::entry_dir(ctx.cacheRoot, ctx.key);
    const auto exeSuffix = std::string(mcpp::platform::exe_suffix);
    ctx.binOut    = mcpp::build::tool_store::bin_path(
        ctx.entry, ctx.toolName, exeSuffix);

    if (mcpp::build::tool_store::entry_valid(ctx.entry, ctx.key, ctx.toolName,
                                             exeSuffix)) {
        step6_record_tool_provision(state, ctx, ctx.binOut);
        return true;
    }

    // PLANNING BUILDS NO TOOL (SPEC-005 R2.5, v1.4; #707).
    // `emit build-database` describes a build; it does not
    // perform one (R2.2), and a tool sub-build is a whole
    // compile of another package, with its own prepare
    // actions -- measured on a fresh store, a single `emit`
    // compiled the tool and ran the tool package's `prepare`
    // action. A tool already in the store is used as above. One
    // that is not is deferred: the build program receives the
    // path the tool will be published at (`binOut`, fixed
    // before anything is built), which is the answer it gets
    // after a successful build, and a note names the tool. A
    // build program that must RUN the tool while configuring
    // meets the same missing file it meets when the tool fails
    // to build (SPEC-007 R5.3), so no new contract follows.
    if (state.overrides.plan_only) {
        state.planNotes.push_back({"MCPP_BUILD_DATABASE_HOST_TOOL_DEFERRED",
            std::format("host tool '{}' of package '{}' is not in "
                        "the tool store and is not built while "
                        "planning; the plan names the path it will "
                        "be published at: {}",
                        ctx.toolName, ctx.depName, ctx.binOut.string()),
            mcpp::wire::Severity::Note});
        step6_record_tool_provision(state, ctx, ctx.binOut);
        return true;
    }

    return false;
}

static std::expected<void, std::string>
step6_build_tool(PrepareState& state, HostToolCtx& ctx) {
    auto& depPkg = state.packages[ctx.depIdx];
    mcpp::ui::status("Building", std::format(
        "host tool {}:{} from {} v{} (once per package source and "
        "host toolchain)", ctx.depName, ctx.toolName, ctx.depName,
        depPkg.manifest.package.version));

    auto& sub = ctx.sub;
    sub.project_root = depPkg.root;
    // Never the package root: it is shared across projects and
    // may be read-only. This is the reason work_dir exists.
    //
    // Scratch is keyed on the CONSUMING project, not shared:
    // the store is GLOBAL, so two projects can want the same
    // tool at once. A single `<entry>/build` would have them
    // writing one ninja tree concurrently, and whichever
    // finished first would `remove_all` it out from under the
    // other. The published binary is what gets shared; the
    // scratch is not.
    //
    // Hashed rather than random so a re-run reuses its own
    // scratch (ninja stays incremental if the publish step
    // never got to delete it).
    //
    // Beside the entries rather than inside one: every
    // directory name of the entry is repeated in each object
    // path the sub-build writes, and on Windows those paths
    // crossed the 260-character limit (mcpp#641, item 3).
    sub.work_dir     = mcpp::build::tool_store::scratch_dir(
        ctx.cacheRoot, ctx.entry, state.workRoot);
    sub.target_triple = "";            // HOST — the whole point
    sub.toolchain     = ctx.toolTcSpec;
    sub.profile       = "release";
    sub.cache_mode    = state.overrides.cache_mode;
    sub.tool_depth    = state.overrides.tool_depth + 1;
    sub.tool_chain_sources = state.overrides.tool_chain_sources;
    sub.tool_chain_sources.push_back(ctx.toolSource);
    // The PRISTINE manifest the resolver produced for this
    // package — `packages[depIdx].manifest` is a copy that
    // feature activation has already mutated, and re-activating
    // on top of it would fold the same feature sources in
    // twice. A `compat` (Form B) package has no mcpp.toml on
    // disk at all, so without this the sub-build could not read
    // a manifest for it in the first place.
    //
    // UNMERGED, because the sub-build targets the HOST: the
    // resolver merged this manifest's conditional sections for
    // the consumer's target, and the sub-build merges them for
    // its own (#690, F12).
    if (ctx.depIdx >= 1 && ctx.depIdx - 1 < state.dep_manifests.size()
        && state.dep_manifests[ctx.depIdx - 1]) {
        auto const& dep = *state.dep_manifests[ctx.depIdx - 1];
        sub.preloaded_manifest = dep.beforeConditionalMerge
            ? dep.beforeConditionalMerge
            : std::make_shared<const mcpp::manifest::Manifest>(dep);
    }
    sub.inherited_runtime_selection = std::make_shared<
        const mcpp::xlings::runtime::RuntimeSelection>(
            state.runtimeSelection);
    sub.inherited_runtime_binding = std::make_shared<
        const mcpp::platform::runtime::RuntimeBinding>(
            state.runtimeBindingSnapshot);
    sub.tool_chain    = state.overrides.tool_chain.empty()
        ? std::format("root → {}:{}", ctx.depName, ctx.toolName)
        : std::format("{} → {}:{}", state.overrides.tool_chain, ctx.depName,
                      ctx.toolName);
    for (auto const& f : ctx.closure) {
        if (!sub.features.empty()) sub.features += ",";
        sub.features += f;
    }

    // #359 (D3b): a sub-build failure must be attributable and
    // REPRODUCIBLE. The Windows tool sub-build has been failing
    // on three abseil TUs since #355 and is still unlocated,
    // because what reached the log was a one-line summary with
    // no scratch path, no chain, and — on the ninja branch below
    // — a filtered view of the inner output. Naming the scratch
    // directory is what lets a maintainer re-run the exact inner
    // build; MCPP_TOOL_BUILD_VERBOSE turns off the filtering.
    auto subContext = [&] {
        return std::format(
            "\n  chain: {}\n  sub-build scratch: {}\n"
            "  re-run it directly:  mcpp build -p {} --release\n"
            "  (set MCPP_TOOL_BUILD_VERBOSE=1 for the inner "
            "build's unfiltered output)",
            sub.tool_chain, sub.work_dir.string(),
            depPkg.root.string());
    };
    auto subCtx = prepare_build(/*print_fingerprint=*/false,
                                /*includeDevDeps=*/false,
                                /*extraTargets=*/{}, sub);
    if (!subCtx) {
        return std::unexpected(std::format(
            "building host tool '{}:{}' failed: {}{}",
            ctx.depName, ctx.toolName, subCtx.error(), subContext()));
    }

    // Build ONLY the requested target (#274 gave the backend
    // explicit goals) — a tool request must not drag the whole
    // package's other artifacts along.
    std::filesystem::path goal;
    for (auto const& lu : subCtx->plan.linkUnits) {
        if (lu.targetName == ctx.toolName) { goal = lu.output; break; }
    }
    if (goal.empty()) {
        return std::unexpected(std::format(
            "host tool '{}:{}' produced no link unit — its "
            "required_features may not be satisfiable on this "
            "platform", ctx.depName, ctx.toolName));
    }

    auto be = mcpp::build::make_ninja_backend();
    mcpp::build::BuildOptions bopt;
    bopt.ninjaTargets = { goal.generic_string() };
    // A build inside planning: its output is this function's error message,
    // not lines of the command's report (pack drive and selection design
    // 2026-10-01, A2).
    bopt.report = mcpp::build::BuildOptions::Report::Caller;
    // Unfiltered inner output on demand: the filter drops
    // ninja's own progress and command echoes, which is right
    // for a normal build and wrong when the question is "what
    // did the inner build actually do".
    if (const char* v = std::getenv("MCPP_TOOL_BUILD_VERBOSE");
        v && *v && std::string_view(v) != "0")
        bopt.verbose = true;
    auto br = be->build(subCtx->plan, bopt);
    if (!br) {
        auto diag = br.error().diagnosticOutput;
        if (diag.empty())
            diag = "(the inner build produced no diagnostic "
                   "output; re-run with MCPP_TOOL_BUILD_VERBOSE=1)";
        return std::unexpected(std::format(
            "building host tool '{}:{}' failed: {}{}\n{}",
            ctx.depName, ctx.toolName, br.error().message,
            subContext(), diag));
    }
    if (br->exitCode != 0) {
        return std::unexpected(std::format(
            "building host tool '{}:{}' failed (exit {}){}",
            ctx.depName, ctx.toolName, br->exitCode, subContext()));
    }

    // `goal` and the sub-build's output directory outlive this step: the
    // publish step below re-derives `produced` from them, the same way this
    // function derived it the first time in the single-function version.
    ctx.goal = goal;
    ctx.subOutputDir = subCtx->plan.outputDir;
    return {};
}

static std::expected<void, std::string>
step6_publish_tool(PrepareState& state, HostToolCtx& ctx) {
    // Publish into the store: build out of place, then move —
    // the same discipline mcpp.build.stage follows, so a
    // concurrent consumer never observes a half-written entry.
    std::error_code cpEc;
    auto produced = ctx.subOutputDir / ctx.goal;
    if (!std::filesystem::exists(produced, cpEc)) {
        return std::unexpected(std::format(
            "host tool '{}:{}' built but '{}' is missing",
            ctx.depName, ctx.toolName, produced.string()));
    }
    std::filesystem::create_directories(ctx.binOut.parent_path(), cpEc);
    auto tmp = ctx.binOut;
    tmp += ".tmp";
    std::filesystem::remove(tmp, cpEc);
    std::filesystem::copy_file(produced, tmp,
        std::filesystem::copy_options::overwrite_existing, cpEc);
    if (cpEc) {
        return std::unexpected(std::format(
            "staging host tool '{}:{}' failed: {}",
            ctx.depName, ctx.toolName, cpEc.message()));
    }
    std::filesystem::permissions(tmp,
        std::filesystem::perms::owner_exec
        | std::filesystem::perms::group_exec
        | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add, cpEc);
    std::filesystem::rename(tmp, ctx.binOut, cpEc);
    if (cpEc) {
        return std::unexpected(std::format(
            "publishing host tool '{}:{}' failed: {}",
            ctx.depName, ctx.toolName, cpEc.message()));
    }
    mcpp::build::tool_store::write_entry(ctx.entry, ctx.key);
    // The sub-build tree is large (protoc is several hundred
    // objects) and the key covers every input, so a hit never
    // needs it again. Removes only THIS consumer's scratch.
    std::filesystem::remove_all(ctx.sub.work_dir, cpEc);
    step6_record_tool_provision(state, ctx, ctx.binOut);
    return {};
}

// Drives one requested tool through resolution, override/cycle checks,
// store lookup, sub-build and publish -- the sequence the single-function
// version ran as one pass down its loop body, with each `continue` above
// becoming an early `return {}` here (there is no next iteration inside a
// per-tool function; the outer loop in step6_provision_host_tools moves on
// on its own).
static std::expected<void, std::string>
step6_provision_one_tool(PrepareState& state, HostToolCtx& ctx) {
    if (auto r = step6_resolve_tool_target(state, ctx); !r)
        return std::unexpected(r.error());

    // #359: every consumer that can SEE this tool gets it, not
    // just the one whose edge asked for it. The bare spelling
    // is emitted only where the namespace ladder binds the tail
    // to this package — otherwise two libraries re-exporting a
    // same-tailed tool would decide the winner by append order.
    // The spellings are `publishedNamesFor`'s, so a tool is
    // addressed by exactly the names its directory is.
    ctx.want = prov::Provision{ prov::Kind::Tool, ctx.depIdx, ctx.toolName };

    auto overridden = step6_check_tool_override(state, ctx);
    if (!overridden) return std::unexpected(overridden.error());
    if (*overridden) return {};

    if (auto r = step6_check_tool_self_request(state, ctx); !r)
        return std::unexpected(r.error());

    auto resolved = step6_resolve_tool_key(state, ctx);
    if (!resolved) return std::unexpected(resolved.error());
    if (*resolved) return {};

    if (auto r = step6_build_tool(state, ctx); !r)
        return std::unexpected(r.error());

    return step6_publish_tool(state, ctx);
}

static std::expected<void, std::string>
step6_provision_host_tools(PrepareState& state,
                           const std::map<std::size_t, std::set<std::string>>& toolRequests) {
            for (auto const& [depIdx, wanted] : toolRequests) {
                auto& depPkg = state.packages[depIdx];
                const auto& depName = depPkg.manifest.package.name;
                std::string depShort = depName;
                if (auto dot = depName.rfind('.');
                    dot != std::string::npos && dot + 1 < depName.size())
                    depShort = depName.substr(dot + 1);

                for (auto const& toolName : wanted) {
                    HostToolCtx ctx;
                    ctx.depIdx = depIdx;
                    ctx.toolName = toolName;
                    ctx.depName = depName;
                    ctx.depShort = depShort;
                    if (auto r = step6_provision_one_tool(state, ctx); !r)
                        return std::unexpected(r.error());
                }
            }

    return {};
}

static std::expected<void, std::string> step6_dependency_build_programs(PrepareState& state) {
        // ── G2: dependency build.mcpp (Cargo build.rs model) ────────────────
        // Runs AFTER feature activation (the env contract exposes the dep's
        // active features) and BEFORE the modgraph scan (generated sources
        // must be visible to the glob walk). Scope is Cargo's: flag directives
        // land in the dep's own buildConfig (its TUs only); link directives
        // ride the dep's ldflags to the final link. Artifacts and generated
        // files live in the CONSUMING project's tree — a registry package
        // root is shared across projects and may be read-only; it is never
        // written to.
        for (std::size_t i = 1; i < state.packages.size(); ++i) {
            auto& pkg = state.packages[i];
            // A package of programs runs its build program in its own tool
            // sub-build, where its sources are compiled (#649 E6).
            if (!state.compilesHere(i)) continue;
            // A workspace member's program runs where a root's does
            // (target_side.cpp, step9_member_build_programs).
            if (state.isWorkspaceMemberPackage(i)) continue;
            std::error_code bpEc;
            if (!std::filesystem::exists(pkg.root / "build.mcpp", bpEc)
                && pkg.manifest.buildConfig.ruleModules.empty()) continue;
            auto host = state.host_tc_for_build_program();
            if (!host) return std::unexpected(host.error());
            // Same edge-graph aggregation as feature activation above, so a
            // dep build.mcpp sees the SAME active feature set the dep is built
            // with (incl. transitive requests / default-features opt-out).
            auto [req, depDefaultFeatures] = aggregatedRequest(state, i);
            auto dirSafe = [](std::string s) {
                for (auto& c : s) if (c == '/' || c == '\\' || c == ':') c = '_';
                return s;
            };
            mcpp::build::BuildProgramEnv bpEnv;
            bpEnv.targetTriple = state.resolvedTargetCanonical;
            // Everything the engine already knows and a build program would
            // otherwise hardcode: the payload ROOT (not the driver), the
            // target's C library, which compiler and which C++ standard
            // library resolved, and the three answers that keep a board
            // package from naming a toolchain. One call, so a new answer
            // reaches every build program at once — see fill_target_build_env.
            fill_target_build_env(bpEnv, *state.m, state.tc ? &*state.tc : nullptr, state.cfg_opt ? &*state.cfg_opt : nullptr);
            bpEnv.toolsBin = state.projectSubosBin;
            bpEnv.profile      = state.effectiveProfile;
            bpEnv.accel        = state.resolvedAccel();
            // The DECLARING package's setting, not the root project's: a rule
            // generating a declaration for this package must match how this
            // package is compiled.
            fill_package_build_env(bpEnv, pkg.manifest);
            state.fillPackEnv(bpEnv, i);
            bpEnv.requested       = pkg.selectedMember;
            bpEnv.languageModules = pkg.manifest.language.modules;
            bpEnv.ruleModules  = pkg.manifest.buildConfig.ruleModules;
            if (auto dit = state.deviceSourcesByPackage.find(pkg.root.string()); dit != state.deviceSourcesByPackage.end())
                bpEnv.deviceSources = dit->second;
            bpEnv.features     = feature_closure(pkg.manifest, req, depDefaultFeatures);
            bpEnv.artifactsDir = state.workRoot / "target" / ".build-mcpp" / "deps"
                / (dirSafe(pkg.manifest.package.name) + "@" + pkg.manifest.package.version);
            bpEnv.genBase      = bpEnv.artifactsDir / "out";
            // What the program imports is kept once for the workspace, and in the
            // global cache where it comes from the engine or the index (#748).
            bpEnv.moduleStore  = state.workRoot / "target" / ".build-mcpp" / "host-modules";
            if (state.cacheMode == CacheMode::Global)
                bpEnv.moduleCacheRoot = mcpp::home::cache_root();
            // mcpp#241: this package's resolved dependencies as
            // MCPP_DEP_<NAME>_DIR, from the authoritative edge graph (no
            // name-guessing); covers feature-activated deps too
            // (mergeActiveFeatureDeps folded them in before the edges were
            // recorded). Shared owner — see fillDepDirs.
            state.fillDepDirs(bpEnv, i, nullptr);
            // …and the xlings packages this package itself declared. Its own
            // manifest, not the root's: a dependency's `[xlings] deps` is what
            // its build.mcpp asks about.
            state.fillXpkgDirs(bpEnv, state.packages[i].manifest, i);
            // #355: the host tools THIS package requested (resolved above).
            if (auto tit = state.toolEnvByConsumer.find(i); tit != state.toolEnvByConsumer.end())
                bpEnv.toolPaths = tit->second;
            bpEnv.hostModules = state.hostModulesByConsumer.count(i)
                ? state.hostModulesByConsumer.at(i)
                : decltype(bpEnv.hostModules){};
            bpEnv.dormantFeatures = state.dormantFeaturesByConsumer.count(i)
                ? state.dormantFeaturesByConsumer.at(i)
                : decltype(bpEnv.dormantFeatures){};
            auto& bcDep = pkg.manifest.buildConfig;
            const auto mark = state.markDirectiveTail(pkg.manifest);
            const auto ldN = bcDep.ldflags.size();
            const auto actN = bcDep.actions.size();
            const auto runnerN = bcDep.runner.size();
            auto namedBefore = bcDep.namedRunners;   // by value: the delta below
            const bool exclusiveBefore = bcDep.runExclusive;
            if (auto r = mcpp::build::run_build_program(
                    pkg.manifest, pkg.root, host->first, host->second,
                    pkg.manifest.cppStandard, bpEnv);
                !r) {
                // #699 item 2 (E3): under `emit build-database` (`plan_only`),
                // a failing build program describes its package without that
                // program's directives, instead of costing the whole plan —
                // the manifest's own configuration, the toolchain and the
                // module graph are still worth describing. Nothing is applied
                // either way: `run_build_program` returns before
                // `Directives::apply` on every failure path. A later failure
                // that follows from the missing directives (a source the
                // program would have added, say) fails the member under the
                // ordinary rule (E1).
                if (state.overrides.plan_only) {
                    state.planNotes.push_back({"MCPP_BUILD_DATABASE_PROGRAM_FAILED",
                        std::format("dependency '{}': {}",
                                    pkg.manifest.package.name, r.error()),
                        mcpp::wire::Severity::Error,
                        (pkg.root / "build.mcpp").string()});
                    // Same reason as the root's mirror of this in
                    // target_side.cpp: a later check whose premise is this
                    // program's directives (the device-source check) must be
                    // able to tell this package apart from one with no program
                    // at all.
                    state.programFailedPackages.insert(pkg.root.string());
                    continue;
                }
                return std::unexpected(std::format(
                    "dependency '{}': {}", pkg.manifest.package.name, r.error()));
            }
            // Cargo scope wiring: compile-visible tail → privateBuild (the
            // shared fold above; the dep's TUs read privateBuild, not bc —
            // its consumers read publicUsage, which the fold never touches;
            // the bcDep entries themselves are inert here: the descriptor
            // include_dirs propagation snapshotted publicUsage at
            // makePackageRoot, long before this pass). Dep residue: link
            // flags — dep ldflags were propagated to the root during the
            // BFS walk, which ran before this pass — forward the new tail
            // (link-search paths are already absolute from parse_line).
            state.foldDirectiveTailIntoPrivateBuild(pkg, pkg.manifest, mark);
            state.adoptActionOutputs(pkg.manifest, pkg.root, actN);

            // Scope::RunGlobal — how the artifact is EXECUTED, forwarded to
            // the root like link flags but with the opposite merge rule.
            //
            // EXACTLY ONE provider. Link flags from two dependencies
            // concatenate and that is correct; two runners cannot — appending
            // produces an argv that is neither one's and fails at exec time
            // with nothing to say which package contributed which token. So
            // the second provider is a hard error that names BOTH, because
            // naming only the loser tells the reader half of what they need.
            // THE DEFAULT RUNNER AND EVERY NAMED ONE, BY ONE RULE.
            //
            // Link flags from two dependencies concatenate and that is correct;
            // two runners for the same name cannot — appending produces an argv
            // that is neither one's and fails at exec with nothing to say which
            // package contributed which token.
            //
            // MISSING THIS SITE IS HOW THE FEATURE FAILED FIRST. `apply()`
            // merges a package's directives into its OWN config; this is where a
            // dependency's RunGlobal entries reach the ROOT. Wiring only the
            // first left `mcpp run --runner flash` reporting "no such runner"
            // while `mcpp run` found the runner the same build program emitted
            // three lines away — measured.
            if (bcDep.runner.size() > runnerN) {
                std::vector<std::string> supplied(
                    bcDep.runner.begin() + static_cast<std::ptrdiff_t>(runnerN),
                    bcDep.runner.end());
                if (!state.m->buildConfig.runner.empty() && !state.runnerProvider.empty()) {
                    return std::unexpected(std::format(
                        "two dependencies both supply a runner for this target: "
                        "'{}' and '{}'.\n"
                        "       A runner is how the artifact is reached — there "
                        "can only be one.\n"
                        "       Drop one of them, or override both with an "
                        "explicit [target.<triple>].runner.",
                        state.runnerProvider, pkg.manifest.package.name));
                }
                state.m->buildConfig.runner = std::move(supplied);
                state.runnerProvider = pkg.manifest.package.name;
            }
            for (auto const& [name, nr] : bcDep.namedRunners) {
                auto before = namedBefore.find(name);
                const bool grew = (before == namedBefore.end())
                               || nr.argv.size() > before->second.argv.size()
                               || (nr.longLived && !before->second.longLived);
                if (!grew) continue;
                auto& slot = state.m->buildConfig.namedRunners[name];
                auto& who  = state.namedRunnerProvider[name];
                if (!slot.argv.empty() && !who.empty()) {
                    return std::unexpected(std::format(
                        "two dependencies both supply a runner named '{}' for "
                        "this target: '{}' and '{}'.\n"
                        "       Drop one of them, or override both with an "
                        "explicit [target.<triple>.runners].{}.",
                        name, who, pkg.manifest.package.name, name));
                }
                slot = nr;
                who  = pkg.manifest.package.name;
            }
            // A CLAIM THAT ONLY EVER TIGHTENS.
            if (bcDep.runExclusive && !exclusiveBefore)
                state.m->buildConfig.runExclusive = true;
            state.m->buildConfig.ldflags.insert(state.m->buildConfig.ldflags.end(),
                bcDep.ldflags.begin() + ldN, bcDep.ldflags.end());
        }

        // apply() may have added interface defines to packages' publicUsage
        // flags (a dependency's active-feature `defines`). Re-run the usage
        // fixpoint so those flags flow into each consumer's privateBuild — the
        // first pass (above) ran before features were activated. Idempotent:
        // include-dir/flag propagation is unique-append.
        state.computeUsageRequirements();
    return {};
}

static std::expected<void, std::string> step6_capability_binding(PrepareState& state) {
        // ─── Capability binding (Stage 3) ──────────────────────────────────
        // For each required capability, bind exactly one provider from the
        // graph. Deterministic: an explicit [capabilities] pin wins; otherwise
        // 0 providers / ≥2 providers are hard errors (never a silent guess); a
        // single provider binds with no config. The provider's link/include
        // requirements already flow through normal dependency mechanics — this
        // pass is the selection-and-validation layer. See the capability-model
        // design doc.
        // --cap cap=provider[,cap=provider] overrides [capabilities] pins.
        for (std::size_t p = 0; p < state.overrides.capabilities.size();) {
            auto c = state.overrides.capabilities.find_first_of(", ", p);
            auto tok = state.overrides.capabilities.substr(
                p, c == std::string::npos ? std::string::npos : c - p);
            if (auto eq = tok.find('='); eq != std::string::npos)
                state.m->capabilityPins[tok.substr(0, eq)] = tok.substr(eq + 1);
            if (c == std::string::npos) break;
            p = c + 1;
        }

        // EXCLUSIVE CAPABILITIES, CHECKED BEFORE REQUIREMENTS ARE BOUND.
        //
        // Ordering is deliberate. A requirement conflict is reported by naming
        // the requirement; this one exists whether or not anything requires the
        // capability, because the defect is that two implementations of one
        // interface are in the same link. Reporting it first means the message
        // names the real problem rather than a symptom of it.
        for (auto const& [cap, claimers] : state.capExclusive) {
            auto it = state.capProviders.find(cap);
            if (it == state.capProviders.end()) continue;
            std::vector<std::string> providers;
            for (auto const& p : it->second)
                if (std::find(providers.begin(), providers.end(), p) == providers.end())
                    providers.push_back(p);
            if (providers.size() < 2) continue;

            std::string list, claimed;
            for (auto const& p : providers) list += (list.empty() ? "" : ", ") + p;
            for (auto const& c : claimers) claimed += (claimed.empty() ? "" : ", ") + c;
            refusal::record(refusal::Code::ExclusiveCapability);
            return std::unexpected(std::format(
                "capability '{}' is provided by more than one package, and {} "
                "declares it EXCLUSIVE.\n"
                "         providers: [{}]\n"
                "         exclusive: [{}]\n"
                "       Two implementations of one interface define the same "
                "symbols, so the link would\n"
                "       resolve every call to whichever archive it reached "
                "first. Keep one of them —\n"
                "       a `[capabilities]` pin selects a provider for a "
                "REQUIREMENT and cannot make two\n"
                "       definitions of one symbol safe.",
                cap, claimers.size() == 1 ? "it" : "they", list, claimed));
        }

        // VERSION FLOORS. A package states what it needs of the machine; a
        // package that established a fact about the machine states it. Neither
        // string means anything to this code -- `cuda.driver` is data flowing
        // through -- which is why a second backend needs no change here and why
        // `test_runtime_contract`'s gate stays satisfied.
        //
        // A FLOOR WITH NO FACT IS SILENT. A machine that never declared what
        // it has is not a machine that fails the floor; it is one nobody asked.
        // Reporting a refusal there would turn "we do not know" into "no", and
        // the whole reason this exists is that a wrong answer is worse than no
        // answer.
        if (auto err = state.checkVersionFloors(); err) return std::unexpected(*err);

        // `requires_abi`: a package needs the artefact's ABI switch on. The
        // root's `[target.<selector>.abi]` is the only table that sets it
        // (whether the value is written there directly, or reaches it
        // through a matching `[target.<sel>.abi]` predicate resolved by
        // merge_conditional_config), so a mismatch is refused naming both
        // halves, before anything compiles -- otherwise
        // it surfaces as a precompiled-module configuration mismatch that
        // names neither. Two members (A1's `exceptions` beside the original
        // `threads`), checked and refused the same way, parametrised so a
        // wording change to one cannot drift from the other.
        auto checkAbiRequirement = [](std::string_view member, bool rootHasIt,
                std::vector<std::pair<std::string, std::string>> const& reqs)
                -> std::optional<std::string> {
            if (rootHasIt || reqs.empty()) return std::nullopt;
            auto const& [what, requirer] = reqs.front();
            return std::format(
                "`{}` requires the artefact's ABI to have {} ({}), and this "
                "build does not state it.\n"
                "       Add to the root manifest, for the targets that need it:\n"
                "\n"
                "           [target.'cfg(os = \"<os>\")'.abi]\n"
                "           {} = true", requirer, member, what, member);
        };
        if (auto err = checkAbiRequirement(
                "threads", state.m->buildConfig.abiThreads, state.abiRequires))
            return std::unexpected(*err);
        if (auto err = checkAbiRequirement(
                "exceptions", state.m->buildConfig.abiExceptions, state.abiRequiresExceptions))
            return std::unexpected(*err);

        std::set<std::string> boundCaps;
        for (auto& [cap, requirer] : state.capRequires) {
            if (!boundCaps.insert(cap).second) continue;   // one diagnosis per cap
            auto& pins = state.m->capabilityPins;
            // Dedup candidates, preserve first-seen order.
            std::vector<std::string> cands;
            if (auto it = state.capProviders.find(cap); it != state.capProviders.end())
                for (auto& p : it->second)
                    if (std::find(cands.begin(), cands.end(), p) == cands.end())
                        cands.push_back(p);
            if (auto pit = pins.find(cap); pit != pins.end()) {
                const auto& pin = pit->second;
                if (std::find(cands.begin(), cands.end(), pin) == cands.end()) {
                    std::string list;
                    for (auto& c : cands) list += (list.empty() ? "" : ", ") + c;
                    return std::unexpected(std::format(
                        "capability '{}' pinned to provider '{}' (via [capabilities]), "
                        "but no such provider is in the graph; candidates: [{}]",
                        cap, pin, list));
                }
                continue;   // pin satisfied
            }
            if (cands.empty())
                return std::unexpected(std::format(
                    "no package provides capability '{}' required by '{}'; add a "
                    "dependency that declares `provides = [\"{}\"]`", cap, requirer, cap));
            if (cands.size() > 1) {
                std::string list;
                for (auto& c : cands) list += (list.empty() ? "" : ", ") + c;
                return std::unexpected(std::format(
                    "capability '{}' has multiple providers in the graph: [{}]; select "
                    "one with [capabilities] {} = \"<provider>\" or --cap {}=<provider>",
                    cap, list, cap, cap));
            }
            // exactly one → bound implicitly.
        }

    // The package that supplies the C++ layer when the graph does, as an index
    // into `packages`. Recorded where the provider is found so that the check
    // after planning (#641) reads the same package the resolution chose.
    // Whether the block below ran at all. `resolvedTargetSide` is default
    // constructed, so "no layer resolved" and "resolution has not happened"
    // read identically off its members — and the layer-conditional pass must
    // tell them apart: the first is an answer a predicate may legitimately
    // fail to match, the second means the pass has no business running.
    state.targetSideResolved = false;

    // What the packages supplying the target side's layers publish: the header
    // directories and interface flags the whole build is compiled against.
    //
    // ONE SET, TWO READERS, and that is deliberate: it is merged into every
    // package's `privateBuild` (so every compile edge sees it) and handed to
    // the `std` module's own command line (which is one more translation unit
    // of the same build). Before this existed, only the second reader was
    // written, and it derived the set itself — which is how the two could
    // describe different worlds.
    return {};
}

std::expected<void, std::string> phase6_features_and_host_tools(PrepareState& state) {
    step6_check_version_floors_closure(state);

    if (auto r = step6_activate_features(state); !r) return std::unexpected(r.error());
    if (auto r = step6_device_extensions_and_rules(state); !r) return std::unexpected(r.error());
    if (auto r = step6_xlings_workspace_from_graph(state); !r) return std::unexpected(r.error());

    auto toolRequests = step6_host_module_registration(state);
    if (!toolRequests) return std::unexpected(toolRequests.error());
    if (auto r = step6_provision_host_tools(state, *toolRequests); !r)
        return std::unexpected(r.error());

    if (auto r = step6_dependency_build_programs(state); !r) return std::unexpected(r.error());
    if (auto r = step6_capability_binding(state); !r) return std::unexpected(r.error());


    return {};
}

} // namespace mcpp::build
