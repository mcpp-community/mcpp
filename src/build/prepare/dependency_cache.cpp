// dependency_cache.cpp -- P13: the global dependency cache: whose sources are
// named, each package's key folded bottom-up, the units an entry serves and the
// entries a build writes. Split from plan.cpp at its own banner ("Global
// dependency cache") when naming a path dependency's sources (mcpp#974) took
// the file and the step past their limits.

module mcpp.build.prepare;
import :state;

import std;
import mcpp.diag;
import mcpp.home;
import mcpp.build.source_digest;
import nlohmann.json;
import mcpp.manifest;
import mcpp.toolchain.cppfly;
import mcpp.toolchain.model;
import mcpp.toolchain.triple;
import mcpp.build.plan;
import mcpp.build.cache_key;
import mcpp.graph;
import mcpp.ui;
import mcpp.bmi_cache;

namespace mcpp::build {

namespace ck = mcpp::build::cache_key;

// WHETHER A PACKAGE'S SOURCES ARE NAMED, so that its build is shared through the
// global cache by that name (mcpp#974): an index package by its version, a git
// dependency by its resolved commit, a path dependency by the digest of its
// tree's content. The project's own trees -- the root, a selected member, any
// package below this build's root or the root of a build above it -- are named
// by nothing and built in place: they are what is being edited.
bool sources_named(const PrepareState& state, std::size_t i) {
    if (i == 0 || i - 1 >= state.dep_cache_identities.size()) return false;
    const auto& id = state.dep_cache_identities[i - 1];
    if (id.sourceKind == "version") return true;
    if (id.sourceKind == "git") return !id.sourceRef.empty();
    if (id.sourceKind != "path" || state.packages[i].selectedMember) return false;
    std::vector<std::filesystem::path> own = state.overrides.own_roots;
    own.push_back(state.workRoot);
    return !mcpp::build::path_is_under_any(state.packages[i].root, own);
}

// The root's own `[dev-dependencies]` serve its tests, not its library, which a
// framework that uses the package under test reaches (names_the_root,
// graph_edges.cpp): the root's key does not fold them in.
static bool serves_root_tests(const DependencyEdge& e) {
    return e.consumerPackageIndex == 0 && e.devTable;
}

// The package graph the keys fold along: the declared edges, and the imports no
// declared edge covers. An import a declared edge already covers adds nothing.
// One that a declared path runs the other way is a module this build generates
// for the importer below its consumer's root, filed under the consumer by its
// path (e2e 849): not an upstream, and left out of `importedFrom` rather than
// closing a cycle.
static mcpp::graph::AdjacencyList key_graph(const PrepareState& state,
                                            std::vector<std::set<std::size_t>>& importedFrom) {
    mcpp::graph::AdjacencyList pkgDeps(state.packages.size());
    for (auto& e : state.dependencyEdges)
        if (e.consumerPackageIndex < pkgDeps.size() && !serves_root_tests(e))
            pkgDeps[e.consumerPackageIndex].push_back(e.dependencyPackageIndex);
    for (std::size_t p = 0; p < importedFrom.size(); ++p)
        std::erase_if(importedFrom[p], [&](std::size_t q) {
            if (std::ranges::find(pkgDeps[p], q) != pkgDeps[p].end()) return true;
            const auto above = mcpp::graph::closure(pkgDeps, std::array{q}, /*include_roots=*/false);
            return std::ranges::find(above, p) != above.end();
        });
    for (std::size_t p = 0; p < importedFrom.size(); ++p)
        pkgDeps[p].insert(pkgDeps[p].end(), importedFrom[p].begin(), importedFrom[p].end());
    return pkgDeps;
}

namespace {

// Each package's key and its inputs, and whether its upstream holds a package
// whose sources are not named.
struct PackageKeys {
    std::vector<std::string>    keys;
    std::vector<nlohmann::json> inputs;
    std::vector<char>           localTaint;
};

} // namespace

static std::expected<PackageKeys, std::string> package_keys(PrepareState& state, BuildContext& ctx) {
    // ── Per-package keys, bottom-up ──────────────────────────────────
    // Axes A/B/C are whole-graph, so they are computed once. Axes D/E are
    // per package. Axis F is each direct dependency's key, which forces a
    // bottom-up order: `dependencyEdges` is a DAG (the modgraph validator
    // rejects cycles), so a simple memoized recursion suffices — with an
    // explicit in-progress guard so a cycle that slipped past validation
    // fails loudly instead of recursing until the stack dies.
    auto axes = ck::build_axes(
        *state.tc, *state.m, state.stdFlagAndDialect,
        mcpp::toolchain::cppfly::effective_dialect_flags(
            *state.tc, state.m->cppStandard.experimental,
            mcpp::manifest::dialect_flags(state.m->buildConfig)),
        // ONE SLOT, BOTH PLATFORMS. See `min_platform_version`: a target
        // is either Apple or Android, and the level selects which bionic
        // symbols are visible, so two levels must be two build
        // directories.
        [&] {
            auto tt = mcpp::toolchain::triple::parse(state.tc->targetTriple);
            return tt ? min_platform_version(*state.m, *tt, state.tc->binaryPath)
                      : std::string{};
        }(),
        // The GLOBAL registry root — the same one `fill_package_config`
        // relativizes against below, so both halves of the key describe
        // payload paths the same way.
        state.storeRoots.empty() ? std::filesystem::path{} : state.storeRoots.front(),
        // The bit `make_plan` decided and `compute_flags` emits. Reading
        // it here rather than re-deriving is what keeps the objects a
        // cache entry HOLDS and the objects a build ASKS FOR describable
        // by one sentence.
        ctx.plan.needsPic);

    // Sources belonging to each package, package-root-relative and sorted.
    std::vector<std::vector<std::string>> pkgSources(state.packages.size());
    std::vector<char> pkgProvidesModule(state.packages.size(), 0);
    // The package each unit is filed under, and the package that provides
    // each module: the import edges below read both.
    std::vector<std::size_t> unitPackage;
    std::map<std::string, std::size_t> moduleProvider;
    for (auto& cu : ctx.plan.compileUnits) {
        // Longest matching root wins. Package roots can nest — a workspace
        // member lives under the workspace root — and taking the first match
        // would file the member's sources under the outer package, putting
        // them in the wrong key. (Index payloads live in the xpkgs store and
        // cannot be shadowed this way, so no cached entry is affected today;
        // resolving it by specificity rather than by iteration order is what
        // keeps that true if roots ever move.)
        std::size_t best = state.packages.size();
        std::size_t bestLen = 0;
        std::string bestRel;
        for (std::size_t p = 0; p < state.packages.size(); ++p) {
            std::error_code ec;
            auto rel = std::filesystem::relative(cu.source, state.packages[p].root, ec);
            if (ec || rel.empty()) continue;
            auto rels = rel.generic_string();
            if (rels.starts_with("..")) continue;
            auto len = state.packages[p].root.generic_string().size();
            if (best == state.packages.size() || len > bestLen) {
                best = p; bestLen = len; bestRel = std::move(rels);
            }
        }
        unitPackage.push_back(best);
        if (best != state.packages.size()) {
            if (!cu.providesModule.empty()) {
                pkgProvidesModule[best] = 1;
                moduleProvider.emplace(cu.providesModule, best);
            }
            pkgSources[best].push_back(std::move(bestRel));
        }
    }
    for (auto& v : pkgSources) std::ranges::sort(v);

    // A package whose unit imports another package's module reads that
    // package's BMI, whether a manifest states the edge or not: the engine
    // interface (`mcpp`) the libraries of a build program import is named
    // by none of theirs. Its key is an upstream as a declared
    // dependency's is; without it an entry compiled against another mcpp
    // version's interface was served and failed with "import 'mcpp' has
    // CRC mismatch" (2026.10.27.1 -> 2026.10.28.1, mcpp.plugins).
    std::vector<std::set<std::size_t>> importedFrom(state.packages.size());
    for (std::size_t u = 0; u < unitPackage.size(); ++u) {
        const auto p = unitPackage[u];
        if (p == state.packages.size()) continue;
        for (auto const& m : ctx.plan.compileUnits[u].imports)
            if (auto it = moduleProvider.find(m); it != moduleProvider.end() && it->second != p)
                importedFrom[p].insert(it->second);
    }

    PackageKeys out{std::vector<std::string>(state.packages.size()),
                    std::vector<nlohmann::json>(state.packages.size(), nlohmann::json::object()),
                    std::vector<char>(state.packages.size(), 0)};
    auto& pkgKeys = out.keys;
    auto& pkgInputs = out.inputs;
    // Does this package's own transitive upstream contain anything that is
    // not an immutable index payload? If so it cannot be cached either, even
    // when the package itself is an index package.
    //
    // A key covers an upstream package by folding in that package's KEY, and
    // a local package's key covers its file list but not its file CONTENTS —
    // nothing could, without hashing a tree that may change between the hash
    // and the compile. So editing a local upstream's source would leave a
    // downstream entry looking valid. No index descriptor can declare a path
    // dependency today, which makes this shape unreachable in practice; it is
    // enforced structurally anyway, because "unreachable today" is how the
    // transitive path-dep leak got in.
    auto& localTaint = out.localTaint;

    // Axis F is each direct dependency's OWN key, which forces a
    // bottom-up order: `dependencyEdges` is a DAG (the modgraph
    // validator rejects cycles among module imports, but this is the
    // package graph, which has no such upstream guard), so the fold
    // below walks `mcpp::graph::topological_order` — dependencies before
    // dependents — instead of recursing, with an explicit cycle check up
    // front standing in for the old in-progress guard.
    auto keyOrder = mcpp::graph::topological_order(key_graph(state, importedFrom));
    if (!keyOrder)
        return std::unexpected(std::format(
            "dependency cycle through package '{}' while computing "
            "its build-cache key",
            state.packages[keyOrder.error().cycle.front()].manifest.package.name));

    for (auto idx : *keyOrder) {
        ck::PackageAxes pa;
        if (idx > 0 && idx - 1 < state.dep_cache_identities.size()) {
            pa.indexName   = state.dep_cache_identities[idx - 1].indexName;
            pa.packageName = state.dep_cache_identities[idx - 1].packageName;
            pa.version     = state.dep_cache_identities[idx - 1].version;
            // A git or path dependency's sources by their own name (mcpp#974),
            // so its key, and every key that folds it in, moves with them.
            if (sources_named(state, idx)) pa.version += source_identity_suffix(state, idx);
        }
        if (pa.packageName.empty()) {
            // The root package, or a package with no resolution identity.
            // It is never cached, but its key still has to exist because
            // downstream packages fold it in via axis F.
            pa.packageName = state.packages[idx].manifest.package.namespace_.empty()
                ? state.packages[idx].manifest.package.name
                : std::format("{}.{}", state.packages[idx].manifest.package.namespace_,
                              state.packages[idx].manifest.package.name);
        }
        if (pa.version.empty()) pa.version = state.packages[idx].manifest.package.version;
        // The GLOBAL registry root — index 0 by construction above. Include
        // dirs are relativized against it so a key survives a different
        // MCPP_HOME; a project-local payload falls back to the `<pkg>`
        // prefix inside fill_package_config and is equally stable.
        ck::fill_package_config(pa, state.packages[idx],
                                state.storeRoots.empty() ? std::filesystem::path{}
                                                   : state.storeRoots.front());
        pa.sources = pkgSources[idx];
        const auto qname = mcpp::build::qualified_package_name(state.packages[idx].manifest);
        // A GCC BMI records its imports' paths as the module map spells
        // them: the BMI directory and the package's instance, whose digest
        // folds in its imports' (`plan.cppm`). See `bmiPathConvention`.
        if (pkgProvidesModule[idx] && state.tc->compiler == mcpp::toolchain::CompilerId::GCC) {
            auto inst = ctx.plan.instanceDirs.find(qname);
            pa.bmiPathConvention = "$root=bmi-dir";
            if (inst != ctx.plan.instanceDirs.end())
                pa.bmiPathConvention += ";instance=" + inst->second;
        }
        // What `scope_lto` appended to this package's units is part of
        // their command, so it is part of the entry's identity.
        if (state.ltoWithheld.contains(qname)) {
            pa.cflags.push_back(state.ltoOffWord);
            pa.cxxflags.push_back(state.ltoOffWord);
        }
        if (!sources_named(state, idx)) localTaint[idx] = 1;
        for (auto& e : state.dependencyEdges) {
            if (e.consumerPackageIndex != idx || serves_root_tests(e)) continue;
            // `idx`'s dependencies precede it in `keyOrder`, so their
            // keys and taint are already folded in below.
            auto const& up = pkgKeys[e.dependencyPackageIndex];
            if (!up.empty()) pa.upstreamKeys.push_back(up);
            if (localTaint[e.dependencyPackageIndex]) localTaint[idx] = 1;
            for (auto& f : e.requestedFeatures) pa.features.push_back(f);
        }
        for (auto q : importedFrom[idx]) {
            if (!pkgKeys[q].empty()) pa.upstreamKeys.push_back(pkgKeys[q]);
            if (localTaint[q]) localTaint[idx] = 1;
        }
        std::ranges::sort(pa.upstreamKeys);
        pa.upstreamKeys.erase(std::unique(pa.upstreamKeys.begin(),
                                          pa.upstreamKeys.end()),
                              pa.upstreamKeys.end());
        std::ranges::sort(pa.features);
        pa.features.erase(std::unique(pa.features.begin(), pa.features.end()),
                          pa.features.end());

        pkgKeys[idx]   = ck::key_hex(axes, pa);
        pkgInputs[idx] = ck::to_json(axes, pa);
    }
    return out;
}

// One package's entry: its units are served from the cache when the entry holds
// what this build asks for; otherwise a task fills it after the build.
static void serve_or_fill(PrepareState& state, BuildContext& ctx, std::size_t i, const PackageKeys& keys) {
    const auto& pkgRoot   = state.packages[i];
    const auto* depIdent  = i - 1 < state.dep_cache_identities.size()
        ? &state.dep_cache_identities[i - 1]
        : nullptr;
    // Only index ("version") packages are cacheable, and the identity
    // recorded at resolution time is the ONLY admissible evidence.
    //
    // The predicate this replaces looked the package up in the ROOT
    // manifest's dependencies/dev-dependencies and skipped it when the
    // spec was path/git. A transitively-reached package is in neither
    // map, so `specIt == end()` left skipCache false and local sources
    // were cached — with `indexName` falling back to defaultIndex, so a
    // workspace member `B` landed on disk as `mcpplibs/B@0.1.0`. Its
    // sources can then change without changing name@version, i.e. the
    // cache key cannot see the change.
    //
    // Note the direction of the judgment: `mcpp add`'s existence gate
    // admits anything it cannot disprove. A build cache must do the
    // opposite — anything it cannot prove came from the immutable
    // xpkgs store stays out, because the failure mode here is a
    // silently wrong object rather than a rejected command.
    //
    // A GIT OR PATH DEPENDENCY NAMES ITS SOURCES TOO (mcpp#974): its
    // resolved commit, or the digest of its tree's content, is in its
    // key (`sources_named`), so a change to the sources is a change of
    // key. The project's own trees are not named and stay out.
    if (!depIdent || !sources_named(state, i)) return;
    // ...and neither may anything it was built against be unnamed.
    if (keys.localTaint[i]) return;
    // ...and the package's sources must ACTUALLY be in the immutable
    // store, not merely labelled as coming from it.
    //
    // The rule stated three paragraphs up is about provenance on disk;
    // `sourceKind` is a label recorded at resolution time, which is a
    // weaker proxy — and there is already a case where the two
    // disagree. Multi-version mangling re-anchors a consumer package's
    // root at `<project>/target/.mangled/<pkg>/__self__` and REWRITES
    // its sources (module/import declarations renamed) while leaving
    // `sourceKind == "version"` and `localTaint` clear. Nothing about
    // that copy is immutable or shareable. It stays out of the cache
    // today only because axis F happens to fold in the mangled
    // secondary's differing key — one axis away from serving objects
    // compiled against renamed modules, which is the silent-wrong-`.o`
    // failure this gate exists to prevent.
    //
    // Judge the location, not the label.
    //
    // LEXICALLY, not via std::filesystem::relative. `relative()` runs
    // weakly_canonical on both sides, which RESOLVES SYMLINKS — and a
    // store whose entries are symlinks into another store is ordinary
    // (tests/e2e/_inherit_toolchain.sh builds exactly that, and so do
    // CI caches that link a warm payload tree into a fresh
    // MCPP_HOME). Canonicalizing turns
    // `<home>/registry/data/xpkgs/<pkg>` into wherever the link points
    // and the package stops looking like a store package at all. The
    // question here is where the payload was INSTALLED, which is a
    // statement about the path, not about the inode.
    const bool fromStore = depIdent->sourceKind == "version";
    if (fromStore && !mcpp::build::path_is_under_any(pkgRoot.root, state.storeRoots))
        return;

    const auto& depName = depIdent->packageName;
    const auto depVer  = (depIdent->version.empty()
        ? pkgRoot.manifest.package.version
        : depIdent->version) + (fromStore ? std::string{} : source_identity_suffix(state, i));

    auto bmiT = mcpp::toolchain::bmi_traits(*state.tc);
    mcpp::bmi_cache::CacheKey key {
        .cacheRoot   = mcpp::home::cache_root(),
        .indexName   = depIdent->indexName,
        .packageName = depName,
        .version     = depVer,
        .keyHex      = keys.keys[i],
        .inputs      = keys.inputs[i],
        .bmiDirName  = std::string(bmiT.bmiDir),
        .manifestTag = std::string(bmiT.manifestPrefix),
    };

    // The artifacts this package contributes, and the compile units
    // that produce them. Collected together so a hit can mark exactly
    // those units — the artifact list alone would not say which edges
    // must stop being compile edges.
    mcpp::bmi_cache::DepArtifacts arts;
    std::vector<std::size_t> unitIdx;
    bool addressable = true;
    for (std::size_t u = 0; u < ctx.plan.compileUnits.size(); ++u) {
        auto& cu = ctx.plan.compileUnits[u];
        std::error_code ec;
        auto rel = std::filesystem::relative(cu.source, pkgRoot.root, ec);
        if (ec || rel.empty()) continue;
        auto rels = rel.string();
        if (rels.starts_with("..")) continue;       // not under depRoot

        // ALL OR NOTHING. A unit plan.cppm could not give a
        // machine-independent entry address to takes its whole package
        // out of the cache, rather than leaving the package half
        // staged. Mixing cached and freshly built artifacts within one
        // package is the case GCC reports as a BMI CRC mismatch in a
        // consumer three edges away, which is far harder to read than
        // one extra compile.
        if (cu.packageObjectRel.empty()) { addressable = false; break; }

        // The entry names a BMI by its module; the build places it
        // below its package's directory (B1), and the entry is filled
        // from there.
        if (!cu.providesModule.empty()) {
            std::string bmi;
            for (char c : cu.providesModule)
                bmi.push_back(c == ':' ? '-' : c);
            bmi += std::string(bmiT.bmiExt);
            if (!cu.bmiFile.empty() && cu.bmiFile != bmi)
                arts.bmiPlacements.push_back({bmi, cu.bmiFile});
            arts.bmiFiles.push_back(std::move(bmi));
        }
        arts.objFiles.push_back({cu.packageObjectRel.generic_string(),
                                 cu.object});
        unitIdx.push_back(u);
    }
    if (!addressable) return;

    // Validate the entry against THIS build's artifact list, not
    // against the entry's own (mcpp#344). Anything short of a full
    // match is a miss — never a failure: the stage edges below are
    // simply not emitted and the units compile normally.
    auto probe = mcpp::bmi_cache::probe_cached(key, arts);
    if (probe.ok) {
        // Mark the units. The backend turns each into a stage_file
        // edge; nothing is copied here. Copying behind ninja's back is
        // exactly what made the old cache a no-op: the staged file was
        // still declared as a compile edge's output, and an output with
        // no .ninja_log command-line record is dirty, so every unit was
        // recompiled while the CLI printed "Cached".
        for (auto u : unitIdx) {
            auto& cu = ctx.plan.compileUnits[u];
            cu.servedFromCache = true;
            cu.cachedObject = mcpp::bmi_cache::cached_obj_path(
                key, cu.packageObjectRel.generic_string());
            if (!cu.providesModule.empty()) {
                std::string bmi;
                for (char c : cu.providesModule)
                    bmi.push_back(c == ':' ? '-' : c);
                bmi += std::string(bmiT.bmiExt);
                cu.cachedBmi = mcpp::bmi_cache::cached_bmi_path(key, bmi);
            }
        }
        mcpp::bmi_cache::touch_accessed(key);
        state.usedCacheRecords.insert(key.entryFile().string());
        // By the name the package report gives it: a dependency key
        // is not always its qualified name (`greet` for the default
        // namespace's `mcpplibs.greet`), and the line then read
        // `Compiling` for a package the cache served.
        ctx.cachedDeps.push_back({mcpp::build::qualified_package_name(pkgRoot.manifest), depVer,
                                  unitIdx.size()});
        return;       // no populate task; it is already cached
    }
    // A valid entry that does not hold what we asked for means the
    // entry and this build disagree about the layout under one key.
    // After #344 that is unreachable; say so out loud if it ever
    // happens again, because the alternative presentation is "the
    // cache silently never hits", and a cache that lies about its own
    // effectiveness went unnoticed for three months once already.
    if (!probe.layoutMismatch.empty()) {
        mcpp::ui::warning(std::format(
            "build cache entry for {}@{} [{}] does not contain the "
            "artifacts this build needs ({} of {} missing, e.g. `{}`); "
            "treating it as a miss. Run `mcpp cache verify` for details.",
            depName, depVer, key.keyHex,
            probe.layoutMismatch.size(),
            arts.bmiFiles.size() + arts.objFiles.size(),
            probe.layoutMismatch.front()));
    }
    for (auto u : unitIdx) ctx.plan.compileUnits[u].fillsCache = true;
    BuildContext::CacheTask task{ std::move(key), std::move(arts) };
    // The tree is read again before its entry is written: a source
    // edited while it compiled is not stored under the digest of the
    // sources before the edit (mcpp#974).
    if (depIdent->sourceKind == "path") {
        task.treeRoot = depIdent->sourceRef.empty() ? pkgRoot.root
                                                    : std::filesystem::path(depIdent->sourceRef);
        task.treeRows = tree_digest_rows(task.treeRoot);
        task.treeDigest = tree_content_digest(state, task.treeRoot);
    }
    ctx.depsToPopulate.push_back(std::move(task));
}

std::expected<void, std::string> step13_dependency_cache(PrepareState& state, BuildContext& ctx) {
    // ─── Global dependency cache: per-package keys, hit → stage edges ──
    //
    // Every index package gets a key over the axes that actually reach its
    // compiler command lines (mcpp.build.cache_key), computed bottom-up so a
    // package's key includes its direct dependencies' keys. A hit marks that
    // package's compile units `servedFromCache`, and the ninja backend emits
    // `stage_file` edges instead of compile edges for them — which is the only
    // way ninja will accept a cached artifact. A miss records a populate task
    // for after the build.
    //
    // `--cache=local|off` skips this block entirely: nothing is read and, in
    // run_build_plan, nothing is written.
    auto cfg2 = state.get_cfg(true);
    if (!cfg2 || ctx.cacheMode != CacheMode::Global) return {};
    std::error_code mkEc;
    std::filesystem::create_directories(ctx.outputDir, mkEc);

    // NOTE (mcpp#344): there is deliberately no local "derive the entry
    // address from the object path" helper here any more. There used to be
    // one, and it was the SECOND derivation of a fact plan.cppm already
    // owns — it stripped `obj/` off the consumer's build path, so the entry
    // layout followed the consumer's package mix while the key did not.
    // `CompileUnit::packageObjectRel` is now the only answer to "where does
    // this object live inside a cache entry", and it is computed in exactly
    // one place. Do not reintroduce a second one.

    auto keys = package_keys(state, ctx);
    if (!keys) return std::unexpected(keys.error());
    for (std::size_t i = 1; i < state.packages.size(); ++i)  // skip [0] = main
        serve_or_fill(state, ctx, i, *keys);
    return {};
}

// AN EDITED TREE'S PREVIOUS ENTRY IS REPLACED (mcpp#974), as a `path` host
// tool's is (tool_store::write_entry, mcpp#874). Every edit of a `path`
// dependency is a new key, and none would be collected before `[cache]
// unused_days`: the tree an entry was built from is written beside it, and an
// entry of the same package from the same tree under another digest is removed
// as this one is written. Another checkout of the package is another tree, and
// keeps its entries.
static void replace_tree_siblings(const mcpp::bmi_cache::CacheKey& key, const std::filesystem::path& tree) {
    namespace fs = std::filesystem;
    const auto plus = key.version.find("+path.");
    if (plus == std::string::npos) return;
    const auto origin = tree.lexically_normal().generic_u8string();
    const std::string text(origin.begin(), origin.end());
    std::error_code ec;
    std::ofstream(key.dir() / "source", std::ios::binary | std::ios::trunc) << text;
    const auto versionDir = key.dir().parent_path();
    const auto prefix = std::format("{}@{}", key.packageName, key.version.substr(0, plus + 6));
    auto from_this_tree = [&](const fs::path& entry) {
        std::ifstream in(entry / "source", std::ios::binary);
        return in && std::string{std::istreambuf_iterator<char>(in), {}} == text;
    };
    for (auto const& v : fs::directory_iterator(versionDir.parent_path(), ec)) {
        const auto name = v.path().filename().u8string();
        if (v.path() == versionDir || !std::string(name.begin(), name.end()).starts_with(prefix)) continue;
        std::error_code kec;
        for (auto const& entry : fs::directory_iterator(v.path(), kec))
            if (from_this_tree(entry.path())) fs::remove_all(entry.path(), kec);
        fs::remove(v.path(), kec);   // only when nothing else is left in it
    }
}

void populate_dependency_cache(BuildContext& ctx) {
    if (ctx.cacheMode != CacheMode::Global) ctx.depsToPopulate.clear();
    for (auto& task : ctx.depsToPopulate) {
        if (!task.treeRoot.empty()) {
            if (mcpp::build::source_tree_digest(task.treeRoot, task.treeRows) != task.treeDigest) {
                const auto tree = task.treeRoot.lexically_normal().generic_u8string();
                mcpp::diag::degraded("build/cache", std::format(
                    "the sources of {} changed while it was built ({}), so its build was not stored in the global cache",
                    task.key.packageName, std::string(tree.begin(), tree.end())),
                    "the next build compiles it again, from the sources it then finds");
                continue;
            }
        }
        auto pr = mcpp::bmi_cache::populate_from(task.key, ctx.outputDir, task.artifacts);
        if (pr && !task.treeRoot.empty()) replace_tree_siblings(task.key, task.treeRoot);
        if (!pr) {
            mcpp::ui::warning(std::format(
                "bmi cache populate failed for {}@{}: {}",
                task.key.packageName, task.key.version, pr.error()));
        }
    }
    // Once: a second drive of the same plan has nothing new to add.
    ctx.depsToPopulate.clear();
}

} // namespace mcpp::build
