// graph_load.cpp -- P4a: loading one dependency (git, path or index
// version) into the graph; the worklist in graph.cpp calls it.

module mcpp.build.prepare;
import :state;

import mcpp.build.prepare_inputs;

import std;
import mcpp.diag;
import mcpp.build.refusal;
import mcpp.xlings.address_set;
import mcpp.build.version_floor;
import mcpp.platform.axis;
import mcpp.log;
import mcpp.manifest;
import mcpp.source_kind;
import mcpp.modgraph.glob;
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;
import mcpp.modgraph.validate;
// For `resolve_version_match` / `list_installed_versions`: a bare compiler
// family named by the dependency graph resolves to a concrete version through
// exactly the path `mcpp toolchain default <family>` uses.
import mcpp.build.plan;
import mcpp.build.flags;          // compute_flags — the per-role contracts (#418)
import mcpp.build.graph_shape;  // #407: the graph says which mode wrote it
import mcpp.build.build_program;
import mcpp.build.backend;      // BuildOptions for the tool sub-build
import mcpp.build.ninja;        // make_ninja_backend — driving that sub-build
import mcpp.config;
import mcpp.xlings;
import mcpp.platform;
import mcpp.fetcher;
import mcpp.fetcher.progress;
import mcpp.pm.resolver;
import mcpp.pm.index_spec;
import mcpp.pm.index_contract;
import mcpp.pm.index_route;
import mcpp.pm.index_refresh;
import mcpp.pm.mangle;
import mcpp.pm.compat;
import mcpp.pm.dep_spec;
import mcpp.pm.dependency_selector;
import mcpp.pm.lock_io;
import mcpp.ui;
import mcpp.log;
import mcpp.fallback.install_integrity;
import mcpp.project;

namespace mcpp::build {

// STEP FUNCTIONS (mcpp#722 / T6): the closures phase4a_graph_load
// assigns onto `state` (each captures only `state`) are split into two
// groups; `LoadedDep` is hoisted here so it stays visible to
// `state.loadVersionDep`, which is defined further down, in the
// orchestrator itself (see the file's own comment for why it is not
// split further).
using LoadedDep = std::pair<std::filesystem::path, mcpp::manifest::Manifest>;

static void step4a_define_split_and_identity_closures(PrepareState& state) {
    // The features each package ends up built with, index-aligned with
    // `packages`. Recorded at activation because the passes that run after it
    // — `[feature-xlings]` provisioning among them — otherwise have no way to
    // ask, and re-deriving it there would be a second copy of the aggregation
    // rule.

    // WHICH VERSION OF EACH TOOL PACKAGE THIS BUILD USES, decided once.
    //
    // Keyed by `(namespace, name)` — the identity, with the version treated as
    // a constraint on it. Filled by the first `graph_xlings_split()` below and
    // read by `fillXpkgDirs`, because those two answer the same question from
    // different ends: one decides what is installed, the other tells a build
    // program where it landed. They used to derive it separately, and the
    // failure that produced is the quiet one — installed A, answered B.

    // The split is computed HERE and reused by the late pass, so the two
    // cannot disagree about what "the graph declared" means.
    //
    // ONE PACKAGE, ONE VERSION. This used to compare whole address strings, so
    // `xim:cuda-nvcc@13.3.33` from the project and `xim:cuda-nvcc@>=12.9.86`
    // from a rule package were two packages: both installed, gigabytes each,
    // and `xpkg_dir` answered one of them. The unification below adjudicates
    // (the declaration nearer the artifact wins) and validates (the winner must
    // satisfy every requirement that lost) — see mcpp.xlings.address_set.
    state.graph_xlings_split = [&]() -> std::expected<
            std::pair<std::vector<std::string>, std::vector<std::string>>,
            std::string> {
        namespace addrset = mcpp::xlings::addrset;
        auto describe = [&](std::size_t i) {
            auto const& pkg = state.packages[i].manifest.package;
            return pkg.namespace_.empty() ? pkg.name
                                          : pkg.namespace_ + ":" + pkg.name;
        };
        std::vector<addrset::Claim> claims;
        for (auto const& spec : applicable_xlings_addresses(
                 *state.runtimeOwnerManifest, state.activeFeaturesByPackage.empty()
                     ? std::vector<std::string>{} : state.activeFeaturesByPackage[0],
                 state.toolPurpose, /*isRoot=*/true))
            claims.push_back({spec, "this project", 0});
        // THE BUCKET IS DECIDED BY WHERE THE WINNING CLAIM SITS IN THIS LIST,
        // not by its distance. The root's own pass provisions exactly the
        // addresses collected above; anything else has to reach the graph pass
        // or nothing installs it. Those two lists are the same one whenever the
        // project is its own runtime owner, and differ under a workspace.
        const std::size_t rootClaims = claims.size();
        for (std::size_t i = 0; i < state.packages.size(); ++i) {
            const auto& man = state.packages[i].manifest;
            const auto feats = i < state.activeFeaturesByPackage.size()
                ? state.activeFeaturesByPackage[i] : std::vector<std::string>{};
            for (auto const& spec : applicable_xlings_addresses(
                     man, feats, state.toolPurpose, /*isRoot=*/i == 0
                         || state.packages[i].selectedMember))
                claims.push_back({spec, describe(i), i == 0 ? 0 : 1});
        }
        auto unified = addrset::unify(claims);
        if (!unified) return std::unexpected(unified.error());
        std::vector<std::string> rootSpecs, fromGraph;
        for (auto const& w : unified->winners) {
            (w.claim < rootClaims ? rootSpecs : fromGraph).push_back(w.address);
            state.xlingsWinner[addrset::package_key(w.address)] = w.address;
        }
        // REPORTED, NOT INFERRED. An override that is only visible as "two
        // versions were declared and one directory exists" is a fact the reader
        // has to reconstruct from the filesystem.
        //
        // This lambda runs twice per build (the early pass and the late one),
        // and the reader sees each note ONCE: `mcpp::diag` deduplicates by the
        // whole payload, which is a designed property rather than an accident
        // of where these two calls sit.
        for (auto const& note : unified->overrides)
            mcpp::diag::warning("xlings/version-override", note);
        return std::pair{std::move(rootSpecs), std::move(fromGraph)};
    };
    state.packages.push_back({*state.root, *state.m});

    // dep_manifests is kept around purely so the build plan can move it
    // out at the end (PackageRoot stores a `Manifest` by value, so the
    // unique_ptr is not load-bearing for liveness — it's a leftover from
    // an earlier design and harmless).
    state.cache_index_name = [](std::string_view ns) {
        if (ns.empty()) return std::string(mcpp::pm::kDefaultNamespace);
        return std::string(ns);
    };


    // Sentinel for "the consumer is the main package" (no dep_manifests entry).

    // #634, A2. A `path` or `git` dependency's identity is the one its manifest
    // declares (SPEC-001 §1.2), and the key a consumer wrote is one way of
    // reaching it. `identityBySource` maps a canonical source (the directory,
    // or the repository and reference) to the identity resolved from it, so a
    // second key over the same source finds that record without loading the
    // manifest again. `declaringManifest` holds the manifest each such
    // identity came from, and records whether that manifest named its
    // namespace: one that does not takes the key's, so two keys over it would
    // be two identities over one source.
    // A GIT DEPENDENCY NAMES A REPOSITORY, AND THE KEY NAMES WHICH PACKAGE OF
    // IT (#649 E7). The root manifest's package is one; each `[workspace]
    // members` entry of that manifest is another. Before this a git source
    // always yielded the root package, so a repository holding a framework and
    // its tools could be pinned by revision for the framework only. The clone
    // of every git source resolved so far is kept with the reference it was
    // resolved from, so a later key over the same source, and a member's
    // `path` edge that stays inside the clone, find it.
    // The member of the repository at `cloneRoot` whose manifest declares
    // `want`, when the root manifest's package is not `want` itself.
    state.gitMemberDeclaring = [&](const std::filesystem::path& cloneRoot,
                                  const ResolvedKey& want)
        -> std::optional<std::string> {
        auto declares = [&](const mcpp::manifest::Manifest& mm) {
            auto rn = mcpp::pm::compat::resolve_package_name(
                mm.package.name, mm.package.namespace_);
            // A manifest that names no namespace takes the key's, as a path
            // dependency's does (#634 A2), so only the short name is compared.
            const bool nsDeclared = !mm.package.namespace_.empty() || rn.usedLegacySplit;
            return rn.shortName == want.shortName
                && (!nsDeclared || rn.namespace_ == want.ns);
        };
        std::error_code ec;
        if (!std::filesystem::exists(cloneRoot / "mcpp.toml", ec)) return std::nullopt;
        auto rootManifest = mcpp::manifest::load(cloneRoot / "mcpp.toml");
        if (!rootManifest || declares(*rootManifest)
            || !rootManifest->workspace.present)
            return std::nullopt;
        for (auto const& member : rootManifest->workspace.members) {
            const auto path = cloneRoot / member / "mcpp.toml";
            if (!std::filesystem::exists(path, ec)) continue;
            auto mm = mcpp::manifest::load(path, {.insideWorkspace = true});
            if (mm && declares(*mm))
                return std::filesystem::path(member).lexically_normal().generic_string();
        }
        return std::nullopt;
    };
    state.qualifiedKey = [](const ResolvedKey& k) {
        return k.ns.empty() ? k.shortName : std::format("{}.{}", k.ns, k.shortName);
    };
    // A root edge that adopted an identity states it on the root's own
    // declaration too, which is what every later reader of the root manifest
    // (the build banner, the resolution record) sees.
    state.stateAdoptedIdentity = [&](const WorkItem& item, const ResolvedKey& declared) {
        if (item.consumerDepIndex != kMainConsumer) return;
        if (auto it = state.m->dependencies.find(item.name); it != state.m->dependencies.end()) {
            it->second.namespace_ = declared.ns;
            it->second.shortName = declared.shortName;
        }
    };
    // A key that names an identity other than the one its `path` or `git`
    // manifest declares is a statement the manifest contradicts, and is
    // reported (#719). A BARE key states no namespace for such a source: the
    // source fixes the package, and the manifest there names its namespace, so
    // reading the key as `mcpplibs.<key>` and then "correcting" it was the
    // engine holding the author to a reading the author never wrote (rule W-b,
    // docs/specs/package-identity.md 4.2). Adoption itself is unchanged; only
    // the report is. `namespaceOmitted` is true exactly for a bare key: a
    // namespace table (`[dependencies.ns]`) and a dotted key both state one.
    //
    // The reports are collected here and written once after resolution
    // (emitAdoptionWarnings), one per consumer manifest and declared
    // namespace, so a manifest that lists eight such keys is one warning.
    state.reportAdoption = [&](const WorkItem& item, const ResolvedKey& normalised,
                              const ResolvedKey& declared) {
        if (item.spec.namespaceOmitted) return;
        if (!state.adoptionsReported.emplace(item.requestedBy, item.name).second) return;
        const auto base = state.workRoot.empty() ? *state.root : state.workRoot;
        const auto consumerFile = ((item.resolveRoot.empty() ? *state.root
                                                             : item.resolveRoot)
                                   / "mcpp.toml").lexically_normal();
        auto rel = consumerFile.lexically_relative(base.lexically_normal());
        const bool inside = !rel.empty() && !rel.generic_string().starts_with("..");
        auto& g = state.adoptionGroups[{ consumerFile.generic_string(), declared.ns }];
        if (g.keys.empty()) {
            g.consumer   = inside ? rel.generic_string() : consumerFile.generic_string();
            g.declaredNs = declared.ns;
            g.example    = item.spec.isGit()
                ? std::format("{} = {{ git = \"{}\", {} = \"{}\" }}",
                              declared.shortName, item.spec.git,
                              item.spec.gitRefKind, item.spec.gitRev)
                : std::format("{} = {{ path = \"{}\" }}",
                              declared.shortName,
                              std::filesystem::path(item.spec.path).generic_string());
        }
        g.writtenNs.insert(normalised.ns);
        g.keys.push_back(item.name);
    };
    state.emitAdoptionWarnings = [&] {
        for (auto const& [id, g] : state.adoptionGroups) {
            std::string written;
            for (auto const& ns : g.writtenNs) written += (written.empty() ? "" : ", ") + ns;
            std::string keys;
            for (auto const& k : g.keys) keys += (keys.empty() ? "" : ", ") + k;
            mcpp::diag::warning("dependency/identity", std::format(
                "{} names {} {} in namespace {}, and the {} {} {} {}; "
                "the declared identity is used: {}",
                g.consumer, g.keys.size(),
                g.keys.size() == 1 ? "dependency" : "dependencies", written,
                g.keys.size() == 1 ? "manifest" : "manifests",
                g.keys.size() == 1 ? "it reaches" : "they reach",
                g.keys.size() == 1 ? "declares" : "declare", g.declaredNs, keys),
                std::format("write {} in a [dependencies.{}] table, for example `{}`",
                            g.keys.size() == 1 ? "it" : "them", g.declaredNs, g.example));
        }
        state.adoptionGroups.clear();
    };


    // Index routing — WHICH index answers for a namespace and how its
    // descriptors are read — lives in mcpp.pm.index_route, shared with the
    // `mcpp add` existence gate so the two cannot disagree about which
    // packages are real (#305/#307). `cfg` is filled in per call: the route is
    // rebuilt on demand because `root` moves when a workspace member is
    // selected above.
    state.index_route = [&](mcpp::config::GlobalConfig* cfg = nullptr) {
        return mcpp::pm::IndexRoute{ &state.m->indices, *state.root, cfg };
    };
    state.findIndexForNs = [&](const std::string& ns)
        -> const mcpp::pm::IndexSpec*
    {
        return state.index_route(nullptr).find_for_ns(ns);
    };

    // SemVer constraint resolver, shared across the worklist so transitive
    // deps with caret/range constraints (`^1.0`) also get pinned to a
    // concrete version before fetch.
    state.resolveSemver = [&](mcpp::manifest::DependencySpec& s,
                              const std::string& depName)
        -> std::expected<void, std::string>
    {
        if (s.isPath() || s.isGit()) return {};
        if (!mcpp::pm::is_version_constraint(s.version)) return {};
        auto cfg = state.get_cfg(true);
        if (!cfg) return std::unexpected(cfg.error());
        // 0.0.10+: use structured namespace from DependencySpec. The route (not
        // a bare Fetcher) is what reaches a descriptor served by a project
        // `[indices]` entry — see #308.
        auto resolved = mcpp::pm::resolve_semver(
            s.namespace_, s.shortName.empty() ? depName : s.shortName,
            s.version, state.index_route(*cfg), *state.targetPlatform);
        if (!resolved) return std::unexpected(resolved.error());
        mcpp::ui::info("Resolved",
            std::format("{} {} → v{}", depName, s.version, *resolved));
        s.version = std::move(*resolved);
        return {};
    };
}

static void step4a_define_candidate_selection_closures(PrepareState& state) {
    // Identity-first candidate probe. A candidate is DISAMBIGUATED by the
    // DECLARED (namespace, name) of whatever descriptor the index holds — never
    // by whether a canonically-named file `<ns>.<short>.lua` happens to exist on
    // disk. It routes through the same identity-verified readers the load path
    // uses (`read_xpkg_lua*`, which gate every hit on the descriptor's declared
    // identity and already cover non-canonical filenames), so candidate selection
    // and loading can never disagree about what a candidate resolves to.
    //
    // SCOPE (#278, do not over-read the paragraph above): identity governs which
    // hits are ACCEPTED, not which files are REACHED. Discovery is still bounded
    // by the candidate-filename list from `compat::xpkg_lua_candidates` — there
    // is no index-wide scan of `pkgs/*/*.lua` anywhere in mcpp, so a descriptor
    // whose filename matches none of the candidates is simply not found. The
    // `IdentityIndex` that would lift that bound was deferred with §5 of the
    // 2026-06-26 design and is deliberately NOT being added: see
    // .agents/docs/2026-07-25-issue278-descriptor-name-form-canonicalization-design.md
    // §3.2/§4.2 for why bare-name discovery across arbitrary namespaces is a
    // reproducibility hazard rather than a convenience.
    //
    // Before this, selection probed the canonical filename only, so a descriptor
    // filed under a non-canonical name (e.g. `aimol.tensorvia-cpu` declared in the
    // mcpplibs index as bare `pkgs/t/tensorvia-cpu.lua`) was invisible to its own
    // peer-root candidate `(aimol, tensorvia-cpu)`, leaving the request pinned to
    // the wrong front candidate `(mcpplibs.aimol, …)`. See
    // .agents/docs/2026-06-26-identity-first-resolution-no-filename.md.
    state.readStrictLuaForCandidate =
        [&](const mcpp::pm::DependencyCoordinate& coord)
            -> std::optional<std::string>
    {
        auto cfg = state.get_cfg(true);
        if (!cfg) return std::nullopt;
        return state.index_route(*cfg).read(coord);
    };

    state.xpkgLuaMatchesCandidate =
        [&](const mcpp::pm::DependencyCoordinate& coord,
            std::string_view luaContent,
            bool allowLegacyBareDefault) {
            // Single source of truth: the descriptor identity gate lives in
            // mcpp.manifest and is shared with the read_xpkg_lua family.  A
            // descriptor served by a declared project index inherits that
            // index's namespace when package.namespace is omitted; preserve
            // the same owner context during this second, stricter check.
            const auto route = state.index_route(nullptr);
            const auto* owner = route.find_for_ns(coord.namespace_);
            const std::string_view ownerNs = owner
                ? std::string_view{owner->name} : std::string_view{};
            return mcpp::manifest::xpkg_lua_identity_matches(
                luaContent, coord.namespace_, coord.shortName,
                allowLegacyBareDefault, ownerNs);
        };

    state.dependencyCoordinates =
        [](const mcpp::manifest::DependencySpec& spec,
           const std::string& depName) {
            if (!spec.candidates.empty()) return spec.candidates;
            std::vector<mcpp::pm::DependencyCoordinate> out;
            out.push_back({
                .namespace_ = spec.namespace_.empty()
                    ? std::string(mcpp::pm::kDefaultNamespace)
                    : spec.namespace_,
                .shortName = spec.shortName.empty() ? depName : spec.shortName,
            });
            return out;
        };


    state.selectDependencyCandidate =
        [&](mcpp::manifest::DependencySpec& spec,
            const std::string& depName) -> std::expected<void, std::string>
    {
        auto candidates = state.dependencyCoordinates(spec, depName);
        if (candidates.empty()) {
            return std::unexpected(
                with_index_cause(std::format(
                    "dependency '{}' has no lookup candidates", depName)));
        }

        // One release train of migration support for the former dotted
        // candidate search. A lockfile records the identity an existing
        // project already selected, so keep that identity stable until the
        // user rewrites the selector explicitly. Without a lock anchor, never
        // fall back: only diagnose a valid old-primary package and continue
        // with the new exact coordinate.
        if (spec.legacyCandidateSearch) {
            const auto exact = candidates.front();
            bool lockExpressesIntent = false;
            if (auto locked = state.packageIdentityLockAnchors.find(depName);
                locked != state.packageIdentityLockAnchors.end()) {
                lockExpressesIntent = true;
                if (locked->second != exact.namespace_) {
                    mcpp::pm::DependencyCoordinate lockedCoordinate{
                        .namespace_ = locked->second,
                        .shortName = exact.shortName,
                    };
                    if (state.selectorMigrationWarnings.insert(depName).second) {
                        mcpp::ui::warning(std::format(
                            "dependency selector '{}' now means exact package "
                            "'{}', but mcpp.lock records '{}'; keeping the "
                            "locked identity for this migration release. "
                            "Write '{}' to keep it explicitly, or remove the "
                            "lock and keep '{}' to migrate",
                            depName,
                            mcpp::pm::format_package_selector(exact),
                            mcpp::pm::format_package_selector(lockedCoordinate),
                            mcpp::pm::format_package_selector(lockedCoordinate),
                            mcpp::pm::format_package_selector(exact)));
                    }
                    candidates.assign(1, std::move(lockedCoordinate));
                }
            }

            if (!lockExpressesIntent && spec.isVersion()) {
                if (auto old = mcpp::pm::legacy_prefixed_coordinate(exact)) {
                    auto oldLua = state.readStrictLuaForCandidate(*old);
                    if (oldLua && state.xpkgLuaMatchesCandidate(
                            *old, *oldLua,
                            /*allowLegacyBareDefault=*/false)
                        && state.selectorMigrationWarnings.insert(depName).second) {
                        mcpp::ui::warning(std::format(
                            "dependency selector '{}' now resolves exactly to "
                            "'{}'; an older mcpp would select the existing "
                            "package '{}'. Write '{}' to keep the old identity "
                            "or keep '{}' for the new exact identity",
                            depName,
                            mcpp::pm::format_package_selector(exact),
                            mcpp::pm::format_package_selector(*old),
                            mcpp::pm::format_package_selector(*old),
                            mcpp::pm::format_package_selector(exact)));
                    }
                }
            }
        }

        auto selected = candidates.front();
        bool matched  = false;
        if (spec.isVersion()) {
            for (auto& candidate : candidates) {
                auto lua = state.readStrictLuaForCandidate(candidate);
                if (!lua) continue;
                if (auto violation = mcpp::manifest::
                        xpkg_name_form_violation_from_lua(*lua)) {
                    return std::unexpected(std::format(
                        "dependency '{}': {}", depName, *violation));
                }
                if (!state.xpkgLuaMatchesCandidate(
                        candidate, *lua, /*allowLegacyBareDefault=*/false)) {
                    continue;
                }

                // INV-RESOLVE (#278) — the discovery rung `(∅, name)` is the
                // "upstream package that declares no namespace" rung, NOT a
                // cross-namespace wildcard. The identity gate is intentionally
                // permissive here (`ns.empty() → name match is enough`, because
                // `mcpp new --template X` legitimately discovers by short name),
                // so the narrowing lives at THIS call site rather than in the
                // gate — tightening the gate would break template discovery.
                //
                // Rejecting the hit keeps a third-party-namespaced package from
                // being reachable by a bare name: resolution must not depend on
                // which indices happen to be present, or adding an index could
                // silently retarget an existing dependency (design §3.2).
                auto declaredNs =
                    mcpp::manifest::extract_xpkg_namespace(*lua);
                if (candidate.namespace_.empty() && !declaredNs.empty()) {
                    continue;
                }

                // P3 (#278) — resolve the discovery rung to a REAL identity
                // before anything downstream sees it. `selected.namespace_`
                // used to be the CANDIDATE's namespace, so a discovery hit
                // wrote an empty namespace into the spec and on into the
                // lockfile and install layer. Read the DECLARED one instead.
                //
                // An empty `declaredNs` is a legal identity here, not a hole to
                // fill: an upstream package with no `namespace` (xim `opencv`,
                // `musl-gcc`) is keyed by its bare name, and the derived
                // fqname == shortName is exactly right for it. Attributing such
                // a descriptor to its owning index (`xim-pkgindex → xim`) is
                // §4.1 of the 2026-06-26 design and is still unimplemented.
                selected = candidate;
                if (selected.namespace_.empty()) selected.namespace_ = declaredNs;
                matched = true;
                break;
            }

            // One-release bare-name migration. Namespace omission means exactly
            // `mcpplibs`, but every published `compat.*` package and every user
            // manifest written before this release spells the dependency bare —
            // `gtest = "1.15.2"`, `ftxui = "6.1.9"`. Making that an immediate
            // hard error means an mcpp upgrade breaks builds against data that
            // is already published and cannot be edited retroactively; the
            // symmetric rule ("published data must not break the program") is
            // why the index floor degrades instead of bricking.
            //
            // The defect #278 removed was the SILENCE, not the reach: mcpp used
            // to continue with a namespace the user never wrote and never say
            // so. A hit here is announced, is recorded downstream under its
            // canonical identity, and names the exact edit that removes the
            // warning. Only a selector whose namespace was OMITTED is eligible —
            // `mcpplibs.gtest` states an identity and must still miss.
            if (!matched && spec.isVersion() && spec.namespaceOmitted) {
                for (auto& legacy :
                         mcpp::pm::legacy_bare_candidates(candidates.front())) {
                    auto lua = state.readStrictLuaForCandidate(legacy);
                    if (!lua) continue;
                    if (mcpp::manifest::xpkg_name_form_violation_from_lua(*lua))
                        continue;
                    if (!state.xpkgLuaMatchesCandidate(
                            legacy, *lua, /*allowLegacyBareDefault=*/false))
                        continue;
                    auto declaredNs =
                        mcpp::manifest::extract_xpkg_namespace(*lua);
                    // Same narrowing as the exact loop: the namespace-less rung
                    // is "upstream package that declares no namespace", not a
                    // cross-namespace wildcard.
                    if (legacy.namespace_.empty() && !declaredNs.empty())
                        continue;

                    selected = legacy;
                    if (selected.namespace_.empty())
                        selected.namespace_ = declaredNs;
                    matched = true;
                    // Downstream — lock, install, cache label — must see the
                    // canonical identity, so the ambiguous spelling survives in
                    // exactly one place: the user's manifest, until they edit it.
                    candidates.assign(1, selected);

                    if (state.selectorMigrationWarnings.insert(depName).second) {
                        mcpp::ui::warning(std::format(
                            "dependency '{}' resolved to '{}' through the "
                            "deprecated bare-name search; namespace omission "
                            "means `{}` only. Write the exact package:"
                            "\n    [dependencies.{}]"
                            "\n    {} = \"{}\""
                            "\n  (or run `mcpp add {}@{}`). This fallback is "
                            "removed in {}.",
                            depName,
                            mcpp::pm::format_package_selector(selected),
                            mcpp::pm::kDefaultNamespace,
                            selected.namespace_, selected.shortName,
                            spec.version,
                            mcpp::pm::format_package_selector(selected),
                            spec.version,
                            mcpp::pm::kBareNameFallbackRemovedIn));
                    }
                    break;
                }
            }

            // A custom GIT index is cloned lazily by xlings during install, so
            // at selection time its descriptors may legitimately not be on disk
            // yet. "Not found" is therefore not conclusive for those namespaces
            // — keep the historical fall-through rather than hard-failing on a
            // package that would have materialized a moment later. Local path
            // indices and the builtin index are both readable here, so they stay
            // under the strict rule below.
            bool anyLazyGitIndex = std::ranges::any_of(candidates,
                [&](const mcpp::pm::DependencyCoordinate& c) {
                    return state.index_route(nullptr).lazy_git(c.namespace_);
                });

            // An exact coordinate that a readable index cannot serve fails at
            // resolution. Never carry it into install-time compatibility
            // retries, which would reintroduce cross-namespace guessing.
            if (!matched && !anyLazyGitIndex) {
                std::string tried;
                for (auto& c : candidates) {
                    if (!tried.empty()) tried += ", ";
                    tried += c.namespace_.empty()
                        ? std::format("(no namespace, {})", c.shortName)
                        : std::format("({}, {})", c.namespace_, c.shortName);
                }

                // T12 — did-you-mean. DIAGNOSTIC ONLY: the scan runs solely on
                // this already-failed path and its result never leaves the
                // error string (see Fetcher::scan_short_name_matches).
                std::string hint;
                if (auto cfg = state.get_cfg(true)) {
                    auto suggestions = mcpp::pm::cross_namespace_suggestions(
                        state.index_route(*cfg), candidates.front().shortName);
                    if (!suggestions.empty()) {
                        hint += "\n  a package with this name exists under "
                                "another namespace:";
                        for (auto& suggestion : suggestions)
                            hint += "\n    " + suggestion.fqn
                                  + suggestion.versions_label();
                        if (auto suggested = mcpp::pm::parse_package_selector(
                                suggestions.front().fqn); suggested
                            && suggested->namespace_) {
                            hint += std::format(
                                "\n  namespace omission means `{}`. write the "
                                "exact package:"
                                "\n    [dependencies.{}]"
                                "\n    {} = \"{}\"",
                                mcpp::pm::kDefaultNamespace,
                                *suggested->namespace_, suggested->name,
                                spec.version.empty() ? "<version>"
                                                     : spec.version);
                        }
                    }
                }

                // Advisory, never a gate (#315): now that a build only refreshes
                // the index on a miss, "not found" and "your copy of the index
                // is from last month" are easy to confuse. State which index
                // answered and how old it is, so the next step is obvious
                // instead of guessed at.
                if (auto cfgA = state.get_cfg(true)) {
                    hint += std::format("\n  index: {}\n  hint: `mcpp index update` "
                                        "if it was published recently",
                        mcpp::pm::staleness_note(
                            mcpp::config::make_xlings_env(**cfgA)));
                }
                // Offline with no local copy of the index at all, the miss says
                // nothing about the package: nothing has been downloaded to look
                // in. That is a download the run needs, not a wrong selector.
                if (mcpp::platform::env::offline_mode()) {
                    if (auto cfgO = state.get_cfg(true);
                        cfgO && !mcpp::xlings::default_index_status(
                                     mcpp::config::make_xlings_env(**cfgO), 0).present) {
                        hint += "\n  offline: the package index has never been fetched; "
                                "run `mcpp index update` without --offline";
                        refusal::record(refusal::Code::OfflineDownloadRequired);
                    }
                }
                return std::unexpected(with_index_cause(std::format(
                    "dependency '{}': no package found for exact selector"
                    "\n  tried: {}{}",
                    depName, tried, hint)));
            }
        }

        spec.namespace_ = std::move(selected.namespace_);
        spec.shortName = std::move(selected.shortName);
        spec.candidates = std::move(candidates);
        return {};
    };
}

// A phase-local struct passed by reference to the steps of ONE
// loadVersionDep call -- the same pattern WorklistItemCtx (graph.cpp) and
// HostToolCtx (features.cpp) use for the steps of one worklist item / one
// requested tool. loadVersionDep is recursive (a preinstall hook may call
// state.loadVersionDep again for one of its own dependencies below), so this
// struct is local to one call's stack frame, not shared across calls.
struct LoadVersionDepCtx {
    std::string depName;
    std::string ns;
    std::string shortName;
    std::string version;
    const mcpp::pm::IndexSpec* idxSpec = nullptr;
    bool useProjectEnv = false;
    std::optional<std::string> luaContent;
    std::optional<std::filesystem::path> installed;
};

// The body of the `readLuaContent` closure the single-function version of
// this step captured per call. Used at two points below (the initial read,
// and the re-check after a fresh install), so it is a named helper rather
// than a per-call closure. `state.get_cfg` is memoized (state.cfg_opt), so
// reconstructing `fetcher` here costs nothing beyond the first call.
static std::optional<std::string>
step4a_read_lua_content(PrepareState& state, LoadVersionDepCtx& ctx) {
    auto cfg = state.get_cfg(true);
    if (!cfg) return std::nullopt; // already validated once at closure entry
    mcpp::fetcher::Fetcher fetcher(**cfg);
    if (ctx.idxSpec && ctx.idxSpec->is_local()) {
        auto indexPath = mcpp::config::resolve_project_index_path(*state.root, *ctx.idxSpec);
        return mcpp::fetcher::Fetcher::read_xpkg_lua_from_path(
            indexPath, ctx.ns, ctx.shortName);
    }
    if (ctx.idxSpec && !ctx.idxSpec->is_builtin()) {
        return mcpp::fetcher::Fetcher::read_xpkg_lua_from_project_data(
            *state.root, ctx.ns, ctx.shortName);
    }
    return fetcher.read_xpkg_lua(ctx.ns, ctx.shortName);
}

// The body of the `findRawInstalled` closure. Used at two points below (the
// initial completeness probe, and again after a fresh install), so it is a
// named helper rather than a per-call closure.
static std::optional<std::filesystem::path>
step4a_find_raw_installed(PrepareState& state, LoadVersionDepCtx& ctx) {
    auto cfg = state.get_cfg(true);
    if (!cfg) return std::nullopt; // already validated once at closure entry
    mcpp::fetcher::Fetcher fetcher(**cfg);
    if (ctx.useProjectEnv) {
        if (auto p = mcpp::fetcher::Fetcher::install_path_from_project_data(
                *state.root, ctx.ns, ctx.shortName, ctx.version)) {
            return p;
        }
    }
    return fetcher.install_path(ctx.ns, ctx.shortName, ctx.version);
}

// The body of the `markInstalled` closure, called once below after a fresh
// install completes.
static void step4a_mark_installed(const std::filesystem::path& p) {
    mcpp::fallback::mark_install_complete(p);
}

static std::expected<void, std::string>
step4a_load_version_dep_locate(PrepareState& state, LoadVersionDepCtx& ctx) {
        // ─── Routing: check if this dep's namespace maps to a custom index ──
        ctx.idxSpec = state.findIndexForNs(ctx.ns);

        ctx.useProjectEnv = ctx.idxSpec && !ctx.idxSpec->is_builtin();

        ctx.luaContent = step4a_read_lua_content(state, ctx);
        if (ctx.idxSpec && ctx.idxSpec->is_local() && !ctx.luaContent) {
            auto indexPath = mcpp::config::resolve_project_index_path(*state.root, *ctx.idxSpec);
            return std::unexpected(with_index_cause(std::format(
                "dependency '{}': not found in local index at '{}'",
                ctx.depName, indexPath.string())));
        }

        auto installedLayoutMatchesIndex = [&](const std::filesystem::path& verRoot) -> bool {
            if (!ctx.luaContent) return false;

            auto field = mcpp::manifest::extract_mcpp_field(*ctx.luaContent);
            if (field.kind == mcpp::manifest::McppField::StringPath) {
                return !mcpp::modgraph::expand_glob(verRoot, field.value).empty();
            }
            if (field.kind == mcpp::manifest::McppField::TableBody) {
                auto dm = mcpp::manifest::synthesize_from_xpkg_lua(
                    *ctx.luaContent, ctx.shortName, ctx.version, *state.targetPlatform);
                if (!dm) return false;
                for (auto const& [generatedPath, _] : dm->buildConfig.generatedFiles) {
                    if (!generatedPath.empty()) return true;
                }
                for (auto const& glob : dm->modules.sources) {
                    if (!glob.empty() && glob.front() == '!') continue;
                    if (!mcpp::modgraph::expand_glob(verRoot, glob).empty()) {
                        return true;
                    }
                }
                return false;
            }

            for (auto pat : { "mcpp.toml", "*/mcpp.toml" }) {
                if (!mcpp::modgraph::expand_glob(verRoot, pat).empty()) {
                    return true;
                }
            }
            return false;
        };

        // THE DESCRIPTOR'S REVISION IS PART OF WHAT IS INSTALLED (#524 A,
        // openxlings/xlings#620). A descriptor that changes what it installs
        // keeps its version and raises the entry's `revision`; a payload whose
        // recorded revision differs is not this version any more, however
        // complete it is, and goes back through xlings, which reinstalls it
        // and records the new revision. A payload with no xlings record at
        // all is judged by the marker alone, as before.
        const int recipeRevision = [&] {
            if (!ctx.luaContent) return 0;
            for (auto const& e : mcpp::manifest::list_xpkg_version_entries(
                     *ctx.luaContent, *state.targetPlatform))
                if (e.version == ctx.version) return e.revision;
            return 0;
        }();
        auto revisionIsCurrent = [&](const std::filesystem::path& p) {
            const auto installed = mcpp::xlings::paths::installed_revision(p);
            if (!installed || *installed == recipeRevision) return true;
            mcpp::log::verbose("fetcher", std::format(
                "{}@{}: installed revision {}, descriptor revision {}; reinstalling",
                ctx.depName, ctx.version, *installed, recipeRevision));
            return false;
        };

        auto findCompleteInstalled = [&]() -> std::optional<std::filesystem::path> {
            auto p = step4a_find_raw_installed(state, ctx);
            if (!p) return std::nullopt;
            if (!revisionIsCurrent(*p)) return std::nullopt;
            if (mcpp::fallback::is_install_complete(*p)) return p;
            if (installedLayoutMatchesIndex(*p)) {
                mcpp::fallback::mark_install_complete(*p);
                return p;
            }
            mcpp::fallback::clean_incomplete_install(*p);
            return std::nullopt;
        };

        // For custom indices, try project-level xlings data roots first.
        // Existing directories without the mcpp completion marker are treated
        // as stale/incomplete on this active resolve path and reinstalled.
        ctx.installed = findCompleteInstalled();

    return {};
}

static std::expected<void, std::string>
step4a_load_version_dep_fetch(PrepareState& state, LoadVersionDepCtx& ctx) {
        auto cfg = state.get_cfg(true);
        if (!cfg) return std::unexpected(cfg.error());
        mcpp::fetcher::Fetcher fetcher(**cfg);
        auto const& depName = ctx.depName;
        auto const& ns = ctx.ns;
        auto const& shortName = ctx.shortName;
        auto const& version = ctx.version;

        // #278 masking guard. The hard INV-NAME check lives on the install path
        // below, so a machine that already has the package from an older index
        // snapshot keeps building. That asymmetry is exactly the trap the issue
        // names — local green, clean CI red — so make it visible here instead of
        // letting it stay silent.
        if (ctx.installed && ctx.luaContent) {
            if (auto violation = mcpp::manifest::
                    xpkg_name_form_violation_from_lua(*ctx.luaContent)) {
                mcpp::ui::warning(std::format(
                    "dependency '{}': {}\n"
                    "       resolving from the already-installed copy; a clean "
                    "environment (CI) will fail here",
                    depName, *violation));
            }
        }

        if (!ctx.installed) {
            if (ctx.luaContent) {
                auto field = mcpp::manifest::extract_mcpp_field(*ctx.luaContent);
                if (field.kind == mcpp::manifest::McppField::TableBody) {
                    auto depManifest = mcpp::manifest::synthesize_from_xpkg_lua(
                        *ctx.luaContent, shortName, version, *state.targetPlatform);
                    if (!depManifest) {
                        return std::unexpected(std::format(
                            "dependency '{}': {}", depName, depManifest.error().format()));
                    }
                    warn_unknown_xpkg_keys(*depManifest, depName);

                    auto preinstallKey = std::format("{}:{}@{}", ns, shortName, version);
                    if (state.preinstallStack.contains(preinstallKey)) {
                        return std::unexpected(std::format(
                            "dependency '{}': cyclic mcpp.deps while preparing install hooks",
                            depName));
                    }

                    if (!state.preinstallDone.contains(preinstallKey)) {
                        state.preinstallStack.insert(preinstallKey);
                        for (auto [childName, childSpec] : depManifest->dependencies) {
                            mcpp::pm::compat::normalize_nested_namespace(
                                childSpec.namespace_,
                                childSpec.shortName,
                                childSpec.legacyDottedKey);

                            if (auto r = state.selectDependencyCandidate(
                                    childSpec, childName); !r) {
                                state.preinstallStack.erase(preinstallKey);
                                return std::unexpected(r.error());
                            }

                            if (auto r = state.resolveSemver(childSpec, childName); !r) {
                                state.preinstallStack.erase(preinstallKey);
                                return std::unexpected(r.error());
                            }

                            if (!childSpec.isVersion()) continue;

                            ResolvedKey childKey{
                                childSpec.namespace_,
                                childSpec.shortName.empty() ? childName : childSpec.shortName,
                            };
                            if (auto child = state.loadVersionDep(
                                    childName,
                                    childKey.ns,
                                    childKey.shortName,
                                    childSpec.version); !child) {
                                state.preinstallStack.erase(preinstallKey);
                                return std::unexpected(child.error());
                            }
                        }
                        state.preinstallStack.erase(preinstallKey);
                        state.preinstallDone.insert(preinstallKey);
                    }
                }
            }

            // The address xlings is asked for is `<effectiveNamespace>:<literal
            // package.name>` (SPEC-001 §6), and BOTH halves come from the
            // descriptor the identity gate accepted — see
            // `mcpp::manifest::xpkg_wire_address` for why splitting the two
            // sources is the bug it is.
            auto wireAddr = mcpp::manifest::xpkg_wire_address(
                ctx.luaContent ? std::string_view(*ctx.luaContent) : std::string_view{},
                ns, shortName);
            if (ctx.luaContent) {
                if (auto violation = mcpp::manifest::
                        xpkg_name_form_violation_from_lua(*ctx.luaContent)) {
                    return std::unexpected(std::format(
                        "dependency '{}': {}", depName, *violation));
                }
            }
            // Human-facing name stays the resolved identity `<ns>.<short>` —
            // that is what the user wrote in [dependencies], so it is what the
            // progress line and errors should echo back.
            auto displayName = ns.empty() ? shortName
                : std::format("{}.{}", ns, shortName);

            // Offline (#315). Checked HERE, at the point of download, and not
            // any earlier: everything above this line — reading descriptors,
            // resolving versions, reusing an already-installed package — is
            // local, and an offline build that has its dependencies must
            // succeed. Only the download itself is refused, and it names the
            // package rather than surfacing a socket error from three layers
            // down. (The toolchain payload path has its own gate; this is the
            // dependency path, which does not go through resolve_xpkg_path.)
            if (mcpp::platform::env::offline_mode()) {
                refusal::record(refusal::Code::OfflineDownloadRequired);
                return std::unexpected(std::format(
                    "offline mode: dependency '{}' v{} is not installed and "
                    "cannot be downloaded\n"
                    "       run without --offline (or unset MCPP_OFFLINE) to fetch it",
                    displayName, version));
            }
            mcpp::ui::info("Downloading", std::format("{} v{}", displayName, version));

            // #238: retain whatever error/warn text the child DID emit so we
            // can fold it into a diagnostic if install_packages exits non-zero.
            std::string capturedChildError;
            // xlings' own error lines, after its structured summary (#614).
            auto append_xlings_stderr = [](std::string& into,
                                           const mcpp::xlings::CallResult& r) {
                if (r.exitCode == 0) return;
                for (auto const& line : r.stderrTail)
                    into += (into.empty() ? "" : "\n  ") + std::string("xlings: ") + line;
            };
            auto install_one = [&](std::string target) -> std::expected<mcpp::xlings::CallResult, mcpp::pm::CallError> {
                if (ctx.useProjectEnv) {
                    // Project/custom-index deps install into the project-local
                    // xlings data root (so a package's install hook can find
                    // sibling packages from the same index). The NDJSON
                    // interface honors this: in the pinned xlings the
                    // `install_packages` capability and the `install` CLI share
                    // `xim::cmd_install`, and the install destination is chosen
                    // by package *scope* (project vs global), not by transport.
                    // Using the interface (rather than the silenced direct CLI)
                    // restores the live `Downloading … [bar] X/Y Z/s` UI here,
                    // matching the toolchain and builtin-index paths.
                    auto projEnv = mcpp::config::make_project_xlings_env(**cfg, *state.root);
                    auto argsJson = std::format(
                        R"({{"targets":["{}"],"yes":true}})", target);
                    mcpp::fetcher::InstallProgressHandler progress;
                    auto r = mcpp::xlings::call(
                        projEnv, "install_packages", argsJson, &progress);
                    capturedChildError = progress.captured_error();
                    if (!r) return std::unexpected(mcpp::pm::CallError{r.error()});
                    append_xlings_stderr(capturedChildError, *r);
                    return *r;
                }
                std::vector<std::string> targets{ std::move(target) };
                mcpp::fetcher::InstallProgressHandler progress;
                auto r = fetcher.install(targets, &progress);
                capturedChildError = progress.captured_error();
                if (r) append_xlings_stderr(capturedChildError, *r);
                return r;
            };
            // Target = `<namespace>:<literal name>@<version>` (SPEC-001 §6).
            //
            // The colon prefix is xlings' *effective namespace*, matched against
            // the descriptor's own `package.namespace` (xlings issue-381 design
            // §2.2) — NOT the index name. mcpp's `[indices] <ns> = {...}` keys
            // ARE namespaces, so the two coincide for a qualified request; for a
            // bare one they do NOT, which is exactly why the namespace has to be
            // read off the descriptor rather than off `ns`.
            //
            // A namespace-less upstream package (xim `opencv`) is addressed by
            // its bare literal name, with no prefix.
            auto target = std::format("{}@{}", wireAddr.target, version);
            // Keep every address we actually put on the wire. Diagnosing the
            // 2026-07-25 breakage needed MCPP_VERBOSE=1 to discover that mcpp
            // had asked for `mcpplibs:gtest` — the error itself only named the
            // dependency, which is the one thing nobody doubts.
            std::vector<std::string> attempted{ target };
            // #613: THE INSTALL HOOK'S ENVIRONMENT, under the names and the rule
            // a build program gets: always emitted, empty when not applicable,
            // so a hook never reads a value inherited from a parent process.
            // Computed by `install_hook_env`, the function the build-program
            // environment takes the same six values from.
            //
            // THE TOOLCHAIN VALUES ARE EMPTY HERE ON THE ORDINARY PATH. `tc` is
            // resolved after the dependency graph (see its declaration: a
            // package in the graph may supply a target-side layer), so when a
            // dependency installs there is no resolved compiler or standard
            // library to state, and a guessed one would be worse than none.
            // Measured with tests/e2e/648. The target names are decided, and a
            // package states the standard library it was built for with
            // `requires = ["mcpp:c++-abi=..."]`, checked once `tc` exists. A
            // hook must not build a variant into a store directory that does
            // not name the variant, because the store is keyed by package and
            // version. Scoped: restored when this dependency's install returns,
            // compat retries below included.
            mcpp::build::BuildProgramEnv hookEnv;
            fill_target_build_env(hookEnv, *state.m, state.tc ? &*state.tc : nullptr, state.cfg_opt ? &*state.cfg_opt : nullptr);
            hookEnv.targetTriple = state.overrides.target_triple;
            // Six names, fixed by install_hook_env; one guard each.
            const auto hookVars = mcpp::build::install_hook_env(hookEnv);
            mcpp::platform::env::ScopedEnv hookVar0(hookVars.at(0).first, hookVars.at(0).second);
            mcpp::platform::env::ScopedEnv hookVar1(hookVars.at(1).first, hookVars.at(1).second);
            mcpp::platform::env::ScopedEnv hookVar2(hookVars.at(2).first, hookVars.at(2).second);
            mcpp::platform::env::ScopedEnv hookVar3(hookVars.at(3).first, hookVars.at(3).second);
            mcpp::platform::env::ScopedEnv hookVar4(hookVars.at(4).first, hookVars.at(4).second);
            mcpp::platform::env::ScopedEnv hookVar5(hookVars.at(5).first, hookVars.at(5).second);
            auto r = install_one(target);
            if (r && r->exitCode != 0 &&
                (ns.empty() || ns == mcpp::pm::kDefaultNamespace)) {
                // Compat retry for a bare/default-namespace request whose
                // descriptor could not be read (no `wireAddr` to trust): the
                // package may still be a `compat` one. Try BOTH spellings — a
                // SPEC-001 index keys it `compat:<short>`, a pre-SPEC-001 index
                // keys it by the literal `compat.<short>`. Sending only the
                // latter is what left the retry pointing at a name the migrated
                // index no longer has.
                for (auto&& compatTarget : {
                         std::format("compat:{}@{}", shortName, version),
                         std::format("compat.{}@{}", shortName, version) }) {
                    if (compatTarget == target) continue;
                    mcpp::ui::info("Downloading", std::format("{} v{}",
                        compatTarget.substr(0, compatTarget.rfind('@')), version));
                    attempted.push_back(compatTarget);
                    r = install_one(compatTarget);
                    if (!r || r->exitCode == 0) break;
                }
            }
            if (!r) return std::unexpected(std::format(
                "fetch '{}@{}': {}", depName, version, r.error().message));
            if (r->exitCode != 0) {
                // #238: the opaque `fetch failed (exit 1)` hid the actionable
                // context mcpp actually has. Reconstruct it: the target, the
                // configured index repos (read back from the seeded
                // .xlings.json — project scope when useProjectEnv, else the
                // global xlings home), any child error text we captured, plus
                // a hint about the known ≥2-repo xlings resolution gap. The
                // real fix lives in openxlings/xlings; this only surfaces WHY.
                auto xlingsJson = (ctx.useProjectEnv
                        ? (state.workRoot / ".mcpp")
                        : (*cfg)->xlingsHome())
                    / ".xlings.json";
                auto indexRepos = mcpp::pm::read_seeded_index_repos(xlingsJson);
                std::string childErr = capturedChildError;
                if (r->error) {
                    if (!childErr.empty()) childErr += "; ";
                    childErr += r->error->message;
                }
                auto target = std::format("{}@{}", depName, version);
                auto diag = mcpp::pm::format_install_failure_diagnostic(
                    target, r->exitCode, indexRepos, childErr);
                std::string tried;
                for (auto& a : attempted) {
                    if (!tried.empty()) tried += ", ";
                    tried += a;
                }
                diag += std::format("\n  wire address{} tried: {}",
                                    attempted.size() == 1 ? "" : "es", tried);
                return std::unexpected(std::move(diag));
            }
            // After install, check project data first for custom index packages.
            ctx.installed = step4a_find_raw_installed(state, ctx);
            if (!ctx.installed) return std::unexpected(std::format(
                "package '{}@{}' install path missing after fetch", depName, version));
            step4a_mark_installed(*ctx.installed);
        }

    return {};
}

static std::expected<LoadedDep, std::string>
step4a_load_version_dep_read_manifest(PrepareState& state, LoadVersionDepCtx& ctx) {
        auto const& depName = ctx.depName;
        auto const& shortName = ctx.shortName;
        auto const& version = ctx.version;
        std::filesystem::path verRoot = *ctx.installed;

        // Route xpkg.lua reading through the appropriate index.
        if (!ctx.luaContent) {
            ctx.luaContent = step4a_read_lua_content(state, ctx);
        }
        if (!ctx.luaContent) return std::unexpected(with_index_cause(std::format(
            "dependency '{}': index entry not found in local clone", depName)));
        auto field = mcpp::manifest::extract_mcpp_field(*ctx.luaContent);

        // 0.0.6+: read explicit namespace from xpkg lua if present.
        auto luaNs = mcpp::manifest::extract_xpkg_namespace(*ctx.luaContent);

        std::optional<mcpp::manifest::Manifest> manifest;
        std::filesystem::path effRoot = verRoot;
        auto loadFrom = [&](const std::filesystem::path& mcppToml)
            -> std::expected<void, std::string>
        {
            // A manifest that is a member of a workspace inside the archive
            // receives that workspace's inheritance, as it does from a git
            // clone of the same commit (#690).
            auto repoWorkspace = workspace_listing(mcppToml.parent_path(), verRoot);
            auto dm = mcpp::manifest::load(
                mcppToml, {.insideWorkspace = repoWorkspace.has_value()});
            if (!dm) return std::unexpected(std::format(
                "dependency '{}' (at '{}'): {}",
                depName, mcppToml.string(), dm.error().format()));
            if (repoWorkspace) {
                if (auto bad = inherit_as_workspace_member(
                        *dm, repoWorkspace->first, repoWorkspace->second,
                        mcppToml.parent_path()))
                    return std::unexpected(std::format(
                        "dependency '{}': {}", depName, *bad));
            }
            if (auto bad = mcpp::project::unresolved_workspace_dependency_error(
                    *dm, mcppToml.parent_path()))
                return std::unexpected(std::format("dependency '{}': {}", depName, *bad));
            manifest = std::move(*dm);
            effRoot  = mcppToml.parent_path();
            return {};
        };
        if (field.kind == mcpp::manifest::McppField::StringPath) {
            auto matches = mcpp::modgraph::expand_glob(verRoot, field.value);
            if (matches.empty()) return std::unexpected(std::format(
                "dependency '{}': mcpp pointer '{}' did not match any "
                "file under '{}'", depName, field.value, verRoot.string()));
            if (matches.size() > 1) return std::unexpected(std::format(
                "dependency '{}': mcpp pointer '{}' matched {} files "
                "(expected exactly one)", depName, field.value, matches.size()));
            if (auto r = loadFrom(matches.front()); !r) return std::unexpected(r.error());
        } else if (field.kind == mcpp::manifest::McppField::TableBody) {
            auto dm = mcpp::manifest::synthesize_from_xpkg_lua(
                *ctx.luaContent, shortName, version, *state.targetPlatform);
            if (!dm) return std::unexpected(std::format(
                "dependency '{}': {}", depName, dm.error().format()));
            warn_unknown_xpkg_keys(*dm, depName);
            manifest = std::move(*dm);
            // effRoot stays as verRoot
        } else {
            std::vector<std::filesystem::path> matches;
            for (auto pat : { "mcpp.toml", "*/mcpp.toml" }) {
                matches = mcpp::modgraph::expand_glob(verRoot, pat);
                if (!matches.empty()) break;
            }
            // Name the directory actually searched. `<verdir>` was a literal
            // placeholder, so the message could not distinguish "the package
            // is Form B and you forgot the mcpp field" from "the verdir mcpp
            // resolved is not this package's at all" — the second is what a
            // cross-namespace install_path hit produces, and it sent this
            // investigation down the wrong path for a while.
            if (matches.empty()) return std::unexpected(std::format(
                "dependency '{}': index entry has no `mcpp = ...` field, "
                "and no mcpp.toml was found at '{}/mcpp.toml' or "
                "'{}/*/mcpp.toml' — add an explicit `mcpp = \"<path>\"` "
                "or `mcpp = {{ ... }}` block to the .lua descriptor. "
                "(If that directory belongs to a DIFFERENT package, the "
                "install step resolved the wrong verdir.)",
                depName, verRoot.string(), verRoot.string()));
            if (matches.size() > 1) return std::unexpected(std::format(
                "dependency '{}': default mcpp.toml lookup matched {} "
                "files; pin one with explicit `mcpp = \"<path>\"`.",
                depName, matches.size()));
            if (auto r = loadFrom(matches.front()); !r) return std::unexpected(r.error());
        }
        // Propagate lua-level namespace into the loaded manifest when
        // the manifest itself doesn't carry one (Form A descriptors
        // whose upstream mcpp.toml predates the namespace field).
        // Guard: if the manifest's name already starts with luaNs+"."
        // (e.g. name="mcpplibs.tinyhttps" with luaNs="mcpplibs"),
        // the namespace is already embedded in the name — don't inject
        // it again or the scanner will produce a double-prefixed
        // qualified name like "mcpplibs.mcpplibs.tinyhttps".
        if (manifest->package.namespace_.empty() && !luaNs.empty()) {
            auto prefix = luaNs + ".";
            if (!manifest->package.name.starts_with(prefix)) {
                manifest->package.namespace_ = luaNs;
            }
        }

        if (auto r = materialize_generated_files(effRoot, *manifest); !r) {
            return std::unexpected(std::format(
                "dependency '{}': {}", depName, r.error()));
        }

        // Dependency-side L1 cfg merge (flags + sources): a descriptor's
        // `target_cfg` / a dep mcpp.toml's [target.'cfg(...)'.build] must
        // evaluate here too — before its globs expand. This is the version/
        // registry-dep half of the #229 funnel: every loadVersionDep() caller
        // (the main per-dependency loop, the multi-version mangling
        // secondary, and the SemVer-merge re-fetch) shares this one call site,
        // so a version dep is merged exactly once regardless of which of the
        // three paths loaded it. The path/git-dep half is the mirror-image
        // call right after ITS manifest load (dependency-manifest-acquisition
        // block below) — same function, same one-merge-per-package guarantee,
        // just keyed off a different loading branch since path/git deps never
        // pass through loadVersionDep.
        if (!manifest->conditionalConfigs.empty()) {
            merge_conditional_config(*manifest,
                                    state.cfgCtx());
        }
        report_flag_words_changes(*manifest);
        fold_build_defines_into_flags(manifest->buildConfig);
        // The root's `abi.threads` reaches a dependency's C translation units
        // here; its C++ units already receive it through the dialect flag set.
        if (state.abiThreadsRendered) state.add_once(manifest->buildConfig.cflags, "-pthread");

        return std::pair{effRoot, std::move(*manifest)};
}

std::expected<void, std::string> phase4a_graph_load(PrepareState& state) {
    step4a_define_split_and_identity_closures(state);
    step4a_define_candidate_selection_closures(state);

    // 0.0.10+: loadVersionDep accepts structured (ns, shortName) for
    // namespace-aware lookup. depName is the map key (qualified or bare),
    // kept for install() target formatting and error messages.
    //
    // The body itself is split into the three steps a call passes through in
    // sequence (locate an already-installed copy, fetch one if none is
    // installed, then read its manifest) over a LoadVersionDepCtx that holds
    // this call's parameters and the locals more than one step reads --
    // step4a_load_version_dep_locate, step4a_load_version_dep_fetch and
    // step4a_load_version_dep_read_manifest above. This lambda stays the
    // entry point with the same signature, so `state.loadVersionDep`'s
    // callers, including its own recursive call for a preinstall hook's
    // dependencies, do not change.
    state.loadVersionDep = [&](const std::string& depName,
                         const std::string& ns,
                         const std::string& shortName,
                         const std::string& version)
        -> std::expected<LoadedDep, std::string>
    {
        auto cfg = state.get_cfg(true);
        if (!cfg) return std::unexpected(cfg.error());

        LoadVersionDepCtx ctx;
        ctx.depName   = depName;
        ctx.ns        = ns;
        ctx.shortName = shortName;
        ctx.version   = version;

        if (auto r = step4a_load_version_dep_locate(state, ctx); !r)
            return std::unexpected(r.error());
        if (auto r = step4a_load_version_dep_fetch(state, ctx); !r)
            return std::unexpected(r.error());
        return step4a_load_version_dep_read_manifest(state, ctx);
    };
    return {};
}


} // namespace mcpp::build
