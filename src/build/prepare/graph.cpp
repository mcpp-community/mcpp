// graph.cpp -- P4b: the dependency worklist, the resolved graph and the
// package-cycle check.

module mcpp.build.prepare;
import :state;

import mcpp.build.prepare_inputs;

import std;
import mcpp.diag;
import mcpp.build.refusal;
import mcpp.xlings.address_set;
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
import mcpp.build.plan;
import mcpp.build.flags;          // compute_flags — the per-role contracts (#418)
import mcpp.build.graph_shape;  // #407: the graph says which mode wrote it
import mcpp.build.build_program;
import mcpp.build.directives;   // directive table: mark / fold_private_tail
import mcpp.build.backend;      // BuildOptions for the tool sub-build
import mcpp.build.ninja;        // make_ninja_backend — driving that sub-build
import mcpp.config;
import mcpp.xlings;
import mcpp.toolchain.post_install;
import mcpp.platform;
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
import mcpp.version_req;
import mcpp.ui;
import mcpp.project;

namespace mcpp::build {

// STEP FUNCTIONS (mcpp#722 / T6). The preamble closures below that
// captured only `state`, or nothing, are ordinary file-scope functions:
// statements moved verbatim, only their header changed (a name and a
// return type in place of `auto x = [&](...) {`, and an explicit
// `PrepareState& state` parameter where the body used to capture it).
// Internal linkage: these names belong to this file, not to
// mcpp.build.prepare's surface.

static mcpp::modgraph::DependencyVisibility parseVisibility(std::string_view visibility) {
        if (visibility == "private")
            return mcpp::modgraph::DependencyVisibility::Private;
        if (visibility == "interface")
            return mcpp::modgraph::DependencyVisibility::Interface;
        return mcpp::modgraph::DependencyVisibility::Public;
}

static std::size_t packageIndexForConsumer(std::size_t consumerDepIndex) {
        if (consumerDepIndex == kMainConsumer) return std::size_t{0};
        return consumerDepIndex + 1;
}

static std::vector<std::filesystem::path> expandIncludeDirs(
        PrepareState& state,
        const std::filesystem::path& packageRoot,
        const mcpp::manifest::Manifest& manifest)
{
        std::vector<std::filesystem::path> dirs;
        for (auto const& inc : manifest.buildConfig.includeDirs) {
            if (inc.is_absolute()) {
                // Native spelling: a TOML `C:/SDL2/include` stays mixed on
                // MSVC and leaks into the CDB's -I otherwise. Direct
                // make_preferred — no generic_string round trip, which can
                // throw for names the ANSI codepage cannot spell (mcpp#230).
                auto n = inc;
                n.make_preferred();
                state.appendUniquePath(dirs, std::move(n));
                continue;
            }
            for (auto& dir : mcpp::modgraph::expand_dir_glob(
                     packageRoot, inc.generic_string())) {
                state.appendUniquePath(dirs, dir);
            }
        }
        return dirs;
}

    // #249: same glob expansion for `include_dirs_after` (the -idirafter
    // channel — searched after the toolchain's system dirs).
static std::vector<std::filesystem::path> expandIncludeDirsAfter(
        PrepareState& state,
        const std::filesystem::path& packageRoot,
        const mcpp::manifest::Manifest& manifest)
{
        std::vector<std::filesystem::path> dirs;
        for (auto const& inc : manifest.buildConfig.includeDirsAfter) {
            if (inc.is_absolute()) {
                auto n = inc;
                n.make_preferred();
                state.appendUniquePath(dirs, std::move(n));
                continue;
            }
            for (auto& dir : mcpp::modgraph::expand_dir_glob(
                     packageRoot, inc.generic_string())) {
                state.appendUniquePath(dirs, dir);
            }
        }
        return dirs;
}

    // The same expansion for `private_include_dirs`, so a private entry may be
    // a glob and still name exactly the directories it expands to.
static std::vector<std::filesystem::path> expandPrivateIncludeDirs(
        PrepareState& state,
        const std::filesystem::path& packageRoot,
        const mcpp::manifest::Manifest& manifest)
{
        std::vector<std::filesystem::path> dirs;
        for (auto const& inc : manifest.buildConfig.privateIncludeDirs) {
            if (inc.is_absolute()) {
                auto n = inc;
                n.make_preferred();
                state.appendUniquePath(dirs, std::move(n));
                continue;
            }
            for (auto& dir : mcpp::modgraph::expand_dir_glob(
                     packageRoot, inc.generic_string())) {
                state.appendUniquePath(dirs, dir);
            }
        }
        return dirs;
}

static std::expected<mcpp::modgraph::PackageRoot, std::string> makePackageRoot(
        PrepareState& state,
        const std::filesystem::path& packageRoot,
        const mcpp::manifest::Manifest& manifest)
{
        // THE SNAPSHOT READS A NORMALISED MANIFEST; IT DOES NOT NORMALISE ONE.
        //
        // Every merge that feeds a package's build inputs (workspace
        // inheritance, the conditional `[target.<sel>.build]` sections) runs
        // at the package's LOAD site, and `fold_build_defines_into_flags` runs
        // after all of them. This lambda only captures the result.
        //
        // `[workspace.build]` inheritance used to run here (#539). The root
        // had already inherited at load time, so it received the workspace
        // entries twice; a member reached as a sibling's `path` dependency
        // inherited after its `defines` had been folded, so the workspace
        // `defines` never reached its compile lines (#690). Both follow from
        // performing a merge at the snapshot, and both are removed by
        // performing it at the load site, where the root already did.
        //
        // The post-condition below is what keeps it removed: a merge placed
        // after the fold leaves `defines` non-empty here, and the build stops
        // with an internal error instead of dropping the macros in silence.
        if (auto unfolded = unfolded_defines_error(manifest))
            return std::unexpected(*unfolded);

        mcpp::modgraph::PackageRoot pkg;
        pkg.root = packageRoot;
        pkg.manifest = manifest;
        pkg.usageResolved = true;

        pkg.privateBuild.includeDirs = expandIncludeDirs(state, packageRoot, manifest);
        pkg.privateBuild.includeDirsAfter = expandIncludeDirsAfter(state, packageRoot, manifest);
        pkg.privateBuild.cflags = manifest.buildConfig.cflags;
        pkg.privateBuild.cxxflags = manifest.buildConfig.cxxflags;
        // NOT `= privateBuild` ANY MORE — a package may now say which of
        // its include directories stop at its own boundary.
        //
        // This line took the whole set for as long as the two were the same
        // set, which they are for almost every package. The one shape where
        // they are not is a package that vendors a library with an internal
        // header overlay: musl's `src/include` adds `hidden`, `weak` and
        // `weak_alias` for musl's own sources, and publishing it hands those
        // names to every consumer. See BuildInputs::privateIncludeDirs.
        //
        // THE FILTER IS APPLIED AFTER GLOB EXPANSION, so a private entry may
        // itself be a glob and still name exactly the directories it expands
        // to. Comparing the unexpanded spellings would let `musl/src/*` be
        // published because it is not literally equal to `musl/src/include`.
        {
            const auto privateExpanded =
                expandPrivateIncludeDirs(state, packageRoot, manifest);
            for (auto const& d : pkg.privateBuild.includeDirs)
                if (std::ranges::find(privateExpanded, d) == privateExpanded.end())
                    pkg.publicUsage.includeDirs.push_back(d);

            // AN ENTRY THAT WITHHOLDS NOTHING IS REPORTED, because the way
            // it fails is the very defect this key exists to prevent: a
            // directory the author believes is private stays published, and
            // nothing about the build looks different until a consumer trips
            // over a name months later.
            //
            // A WARNING AND NOT AN ERROR, for consistency with `include_dirs`
            // itself: that key silently ignores a glob matching nothing, and a
            // conditional manifest can legitimately name a directory that
            // exists on one platform only. Refusing here would be stricter
            // than the list this one filters.
            for (auto const& want : privateExpanded) {
                if (std::ranges::find(pkg.privateBuild.includeDirs, want)
                    != pkg.privateBuild.includeDirs.end())
                    continue;
                mcpp::diag::warning("manifest", std::format(
                    "package '{}': `private_include_dirs` names '{}', which is "
                    "not among this package's `include_dirs`.\n"
                    "       It withholds nothing — `private_include_dirs` says "
                    "which entries OF `include_dirs`\n"
                    "       stop at this package's boundary, and an entry that "
                    "is not one of them is published\n"
                    "       exactly as before.",
                    manifest.package.name, want.generic_string()));
            }
        }
        pkg.publicUsage.includeDirsAfter = pkg.privateBuild.includeDirsAfter;
        pkg.linkUsage.ldflags = manifest.buildConfig.ldflags;
        return pkg;
}

static void recordDependencyEdge(
        PrepareState& state,
        std::size_t consumerDepIndex,
        std::size_t dependencyPackageIndex,
        const mcpp::manifest::DependencySpec& spec,
        bool buildOnly,
        const std::string& writtenKey)
{
        const auto consumerPackageIndex = packageIndexForConsumer(consumerDepIndex);
        if (consumerPackageIndex >= state.packages.size()
            || dependencyPackageIndex >= state.packages.size()) {
            return;
        }
        if (std::ranges::none_of(state.graphRequests, [&](const GraphRequest& r) {
                return r.consumerPackageIndex == consumerPackageIndex
                    && r.dependencyPackageIndex == dependencyPackageIndex
                    && r.key == writtenKey && r.table == spec.declaredIn;
            }))
            state.graphRequests.push_back(GraphRequest{
                .consumerPackageIndex = consumerPackageIndex,
                .dependencyPackageIndex = dependencyPackageIndex,
                .key = writtenKey,
                .table = spec.declaredIn,
            });
        const auto visibility = parseVisibility(spec.visibility);
        auto same = [&](const DependencyEdge& edge) {
            return edge.consumerPackageIndex == consumerPackageIndex
                && edge.dependencyPackageIndex == dependencyPackageIndex
                && edge.visibility == visibility;
        };
        auto it = std::find_if(state.dependencyEdges.begin(), state.dependencyEdges.end(), same);
        if (it != state.dependencyEdges.end()) {
            // One consumer naming one dependency in BOTH tables. The ordinary
            // declaration wins, because the build-time path never subtracts
            // from what the project asked to link — stating the rule the other
            // way round would let a `[build-dependencies]` line quietly drop a
            // library the target needs.
            if (!buildOnly) it->buildOnly = false;
            // AND THE SECOND DECLARATION'S REQUESTS ARE KEPT (#649 E7). Both
            // declarations name one edge, so what each asks of the dependency
            // is asked of that edge: this used to return here and lose the
            // second one's `tools`, `features`, `host-module` and `reexport`
            // without a word, under `--strict` too. The rule is the one
            // `mergeActiveFeatureDeps` already applies to a feature's
            // restatement: additive fields union, `default-features` stays on
            // unless every declaration opts out.
            for (auto const& t : spec.tools)
                if (std::ranges::find(it->requestedTools, t) == it->requestedTools.end())
                    it->requestedTools.push_back(t);
            for (auto const& a : spec.artifacts)
                if (std::ranges::find(it->requestedArtifacts, a)
                    == it->requestedArtifacts.end())
                    it->requestedArtifacts.push_back(a);
            for (auto const& f : spec.features)
                if (std::ranges::find(it->requestedFeatures, f)
                    == it->requestedFeatures.end())
                    it->requestedFeatures.push_back(f);
            it->defaultFeatures = it->defaultFeatures || spec.defaultFeatures;
            if (spec.hostModule) it->hostModule = true;
            else if (dependencyPackageIndex < state.packages.size())
                for (auto const& f : spec.features)
                    if (state.packages[dependencyPackageIndex].manifest.featureRuleModule.contains(f)) {
                        it->hostModule = true;
                        break;
                    }
            it->reexport = it->reexport || spec.reexport;
            return;
        }
        // A REQUESTED FEATURE THAT IS A BUILD RULE IMPLIES `host-module`.
        //
        // `host-module = true` says "compile this dependency's interface unit
        // for the host so my build program can import it", and a feature
        // declaring `rule_module` has already said that is the only way to use
        // it. Requiring both was a second spelling of one fact, and the failure
        // when only the feature was written landed in the consumer's build as
        // an unresolved import rather than in the line that was incomplete.
        bool hostModule = spec.hostModule;
        if (!hostModule && dependencyPackageIndex < state.packages.size()) {
            auto const& depManifest = state.packages[dependencyPackageIndex].manifest;
            for (auto const& f : spec.features)
                if (depManifest.featureRuleModule.contains(f)) { hostModule = true; break; }
        }
        state.dependencyEdges.push_back(DependencyEdge{
            .consumerPackageIndex = consumerPackageIndex,
            .dependencyPackageIndex = dependencyPackageIndex,
            .visibility = visibility,
            .requestedFeatures = spec.features,
            .defaultFeatures = spec.defaultFeatures,
            .requestedTools = spec.tools,
            .requestedArtifacts = spec.artifacts,
            .hostModule = hostModule,
            .reexport = spec.reexport,
            .buildOnly = buildOnly,
        });
}

static std::vector<std::string> propagateLinkFlags(
        PrepareState& state,
        const std::filesystem::path& depRoot,
        const mcpp::manifest::Manifest& depManifest)
{
        // Word by word (SPEC-004 §8, #703): see normalized_dependency_ldflags.
        // In a workspace plan the pooled list serves the units no member owns
        // (a dependency's shared library); a member's units link with its own
        // closure's flags, which the plan reads from each package (§15).
        auto added = normalized_dependency_ldflags(depRoot, depManifest.buildConfig.ldflags);
        for (auto const& f : added) state.m->buildConfig.ldflags.push_back(f);
        return added;
}

static void removeLinkFlags(PrepareState& state, const std::vector<std::string>& flags) {
        auto& ldflags = state.m->buildConfig.ldflags;
        for (auto const& flag : flags) {
            auto pos = std::find(ldflags.begin(), ldflags.end(), flag);
            if (pos != ldflags.end()) ldflags.erase(pos);
        }
}

static std::expected<std::set<std::filesystem::path>, std::string> package_source_files(
        const std::filesystem::path& srcRoot,
        const mcpp::manifest::Manifest& depManifest)
{
        // Resolve the source globs against the original root, falling
        // back to the convention default if the manifest didn't set any.
        std::vector<std::string> globs = depManifest.modules.sources;
        if (globs.empty()) {
            // Was a fourth hand-written copy of the convention default, and it
            // had already drifted: all three assembly extensions were missing,
            // so staging a dependency with .S/.s/.asm silently dropped them.
            globs = mcpp::default_source_globs(
                mcpp::extension_table_for(depManifest.buildConfig.moduleExtensions,
                                          depManifest.buildConfig.deviceExtensions));
        }
        // Glob exclusion (same as scan_one_into): `!` prefix removes.
        std::set<std::filesystem::path> sourceFiles;
        std::set<std::filesystem::path> excluded;
        for (auto const& g : globs) {
            if (!g.empty() && g[0] == '!') {
                for (auto& p : mcpp::modgraph::expand_glob(srcRoot, g.substr(1)))
                    excluded.insert(p);
            } else {
                for (auto& p : mcpp::modgraph::expand_glob(srcRoot, g))
                    sourceFiles.insert(p);
            }
        }
        for (auto& p : excluded) sourceFiles.erase(p);
        if (sourceFiles.empty()) {
            return std::unexpected(std::format(
                "stage: no source files found under '{}' (globs={})",
                srcRoot.string(), globs.size()));
        }
        return sourceFiles;
}

    // Stage a dep's source files into a fresh directory, rewriting their
    // module / import declarations against `rename`. Used by the multi-
    // version mangling fallback (Level 1) so two cross-major copies of
    // the same package can coexist with distinct module names.
    //
    // Headers reached through `[build].include_dirs` are NOT staged — those
    // keep pointing at the original install dir via absolutized include paths.
    //
    // HEADERS BESIDE A SOURCE ARE A DIFFERENT CASE, AND THEY ARE STAGED.
    //
    // `#include "detail.h"` is resolved relative to the directory of the file
    // holding the directive, so moving the source moves the search. No
    // `include_dirs` entry is involved and absolutizing one cannot help: the
    // package never declared a path because it never needed one. Measured
    // before this, on a package whose `src/time.cpp` includes `src/sbi.h`:
    //
    //     target/.mangled/openkal-opensbi/__self__/src/time.cpp:44:10:
    //         fatal error: 'sbi.h' file not found
    //
    // THE DIAGNOSIS THIS PRODUCES POINTS AT THE WRONG THING. The path in it
    // is a staging directory the author never wrote, for a header sitting
    // exactly where the source expects it, and the build that triggered it
    // asked for nothing unusual — two majors of one dependency is a supported
    // arrangement, and this is its most ordinary consequence.
    //
    // What is copied is every file in a directory that contains a staged
    // source and is not itself staged, verbatim: rewriting applies to module
    // declarations, and a header has none. Directories with no staged source
    // are not visited, so this stays proportional to what is being staged.
static std::expected<void, std::string> stage_with_rewrite(
        const std::filesystem::path& srcRoot,
        const std::filesystem::path& dstRoot,
        const mcpp::manifest::Manifest& depManifest,
        const std::map<std::string, std::string>& rename)
{
        std::error_code ec;
        std::filesystem::create_directories(dstRoot, ec);
        if (ec) return std::unexpected(std::format(
            "stage: cannot create '{}': {}", dstRoot.string(), ec.message()));

        auto sources = package_source_files(srcRoot, depManifest);
        if (!sources) return std::unexpected(sources.error());

        for (auto const& f : *sources) {
            auto rel = std::filesystem::relative(f, srcRoot, ec);
            if (ec) return std::unexpected(std::format(
                "stage: cannot relativize '{}': {}", f.string(), ec.message()));
            auto dst = dstRoot / rel;
            std::filesystem::create_directories(dst.parent_path(), ec);

            std::ifstream is(f);
            if (!is) return std::unexpected(std::format(
                "stage: cannot read '{}'", f.string()));
            std::stringstream buf; buf << is.rdbuf();
            std::string content = buf.str();

            std::string out = mcpp::pm::rewrite_module_decls(content, rename);
            std::ofstream os(dst);
            if (!os) return std::unexpected(std::format(
                "stage: cannot write '{}'", dst.string()));
            os << out;
        }

        // The files beside those sources, carried across unchanged so a quoted
        // include still finds what it named.
        std::set<std::filesystem::path> sourceDirs;
        for (auto const& f : *sources) sourceDirs.insert(f.parent_path());
        for (auto const& dir : sourceDirs) {
            for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
                if (ec) break;
                if (!entry.is_regular_file()) continue;
                if (sources->contains(entry.path())) continue;
                auto rel = std::filesystem::relative(entry.path(), srcRoot, ec);
                if (ec) continue;
                auto dst = dstRoot / rel;
                std::filesystem::create_directories(dst.parent_path(), ec);
                std::filesystem::copy_file(
                    entry.path(), dst,
                    std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) return std::unexpected(std::format(
                    "stage: cannot copy '{}': {}",
                    entry.path().string(), ec.message()));
            }
            ec.clear();
        }
        return {};
}

static std::expected<std::vector<std::string>, std::string> declared_modules_for(
        const std::filesystem::path& srcRoot,
        const mcpp::manifest::Manifest& depManifest)
{
        auto sources = package_source_files(srcRoot, depManifest);
        if (!sources) return std::unexpected(sources.error());
        std::vector<std::string> modules;
        for (auto const& file : *sources) {
            std::ifstream is(file);
            if (!is) return std::unexpected(std::format(
                "mangle: cannot read '{}'", file.string()));
            std::stringstream buf; buf << is.rdbuf();
            for (auto& name : mcpp::pm::declared_module_roots(buf.str())) {
                if (std::ranges::find(modules, name) == modules.end())
                    modules.push_back(std::move(name));
            }
        }
        if (modules.empty()) return std::unexpected(std::format(
            "mangle: package '{}' declares no named C++ module to rewrite",
            depManifest.package.name));
        return modules;
}

    // Stage 2a — feature-activated optional dependencies. Static file-scope
    // functions (mcpp#722 / T6 split), not local lambdas as originally
    // written: the GCC 16 modules bug this comment used to warn about
    // ("failed to load pendings for __normal_iterator") is triggered by an
    // EXPORTED declaration's reachable set including a std::map
    // instantiation; a `static` function has no external linkage and is
    // never reachable from mcpp.build.prepare's exported interface, so it
    // cannot pollute the BMI the bug reads from. Verified by a full build
    // (mcpp itself, GCC 16.1): every consumer of this module still
    // compiles clean.
static std::vector<std::string> activateFeatures(
        const mcpp::manifest::Manifest& pm,
        const std::vector<std::string>& requested,
        bool seedDefault = true) {
        return feature_closure(pm, requested, seedDefault); // single shared implementation
}

static std::string dependencySourceOf(const mcpp::manifest::DependencySpec& s) {
        if (s.inheritWorkspace) return std::string("workspace = true");
        if (s.isPath()) {
            auto norm = std::filesystem::path(s.path).lexically_normal().generic_string();
            while (norm.size() > 1 && norm.back() == '/') norm.pop_back();
            return std::format("path = \"{}\"", norm);
        }
        if (s.isGit())
            return std::format("git = \"{}\", {} = \"{}\"", s.git,
                               s.gitRefKind.empty() ? "rev" : s.gitRefKind, s.gitRev);
        return std::format("version = \"{}\"", s.version);
}

    // What the comparison is made on. The message shows the declaration as it
    // was written; the judgement drops the whitespace inside a constraint, so
    // the two declarations are compared on what they mean.
static std::string dependencySourceKey(const mcpp::manifest::DependencySpec& s) {
        auto spelled = dependencySourceOf(s);
        if (!s.inheritWorkspace && !s.isPath() && !s.isGit())
            std::erase_if(spelled, [](char c) { return c == ' ' || c == '\t'; });
        return spelled;
}

    // Merge a manifest's active feature-deps into its `dependencies` map so the
    // worklist below pulls them like any normal dep. A top-level dep of the same
    // key is never overwritten; deps declared only under a feature appear only
    // when that feature is active. `seedDefault` carries consumer-side
    // `default-features = false` (#242): when a consumer opts out of this dep's
    // default set, feature-deps behind the default pseudo-feature stay dormant.
    //
    // A RESTATEMENT NAMES THE SAME SOURCE OR IS REFUSED (#647 E4.2). The grammar
    // asks a `[feature-deps]` entry to restate its dependency's source, and the
    // merge below takes only the additive fields from it, so a restatement
    // that names another path, repository or version was dropped without a
    // word, under `--strict` too: the tool came from the declaration in effect
    // while the manifest said it came from somewhere else. The comparison runs
    // against `dependencies` after the conditional fold, so a row's replacement
    // (#634 A1) is the declaration a restatement is held to.
    //
    // TWO SPELLINGS OF ONE SOURCE ARE ONE SOURCE. The comparison below decides
    // whether a restatement names something else, so it has to be made on what
    // the two declarations MEAN, not on their bytes: a path is normalised, and
    // a version constraint is compared with its whitespace removed, because
    // `">= 1.2.0"` and `">=1.2.0"` are one constraint and the manifest that
    // spells them differently built on 2026.9.15.2. A gate added for #647 E4.2
    // must refuse a restatement that names another source, and nothing else.
static std::expected<void, std::string> mergeActiveFeatureDeps(
        mcpp::manifest::Manifest& pm,
        const std::vector<std::string>& requested,
        bool seedDefault = true) {
        if (pm.featureDeps.empty()) return {};
        for (auto& f : activateFeatures(pm, requested, seedDefault)) {
            auto it = pm.featureDeps.find(f);
            if (it == pm.featureDeps.end()) continue;
            for (auto& [k, spec] : it->second) {
                auto [pos, fresh] = pm.dependencies.try_emplace(k, spec);
                if (fresh) continue;
                if (!pos->second.inheritWorkspace && !spec.inheritWorkspace) {
                    const auto inEffect = dependencySourceOf(pos->second);
                    const auto restated = dependencySourceOf(spec);
                    if (dependencySourceKey(pos->second) != dependencySourceKey(spec))
                        return std::unexpected(std::format(
                            "[feature-deps.{}] of '{}' restates the dependency '{}' "
                            "with {}, while the declaration in effect on this row "
                            "names {}.\n"
                            "       One dependency has one source, so the "
                            "restatement would be ignored.\n"
                            "       fix: restate the same source ({}), or declare "
                            "'{}' only under the feature.",
                            f, pm.package.name, k, restated, inEffect, inEffect, k));
                }
                // #359: the key already exists unconditionally, and dropping
                // the feature's spec here loses REQUESTS the feature exists to
                // make. gRPC is the shape: it depends on compat.protobuf
                // always, and its `codegen` feature has to add
                // `tools = ["protoc"], reexport = true` to that same edge —
                // which is precisely what must NOT be paid for by a consumer
                // who did not ask for codegen, so moving it to the
                // unconditional entry is not an option either.
                //
                // Additive fields merge; identity fields (version/path/git) do
                // not, keeping "a conditional section never silently
                // overrides an unconditional one" intact. Same rule the
                // per-edge feature request already follows.
                auto& dst = pos->second;
                for (auto const& t : spec.tools)
                    if (std::find(dst.tools.begin(), dst.tools.end(), t)
                        == dst.tools.end())
                        dst.tools.push_back(t);
                for (auto const& f2 : spec.features)
                    if (std::find(dst.features.begin(), dst.features.end(), f2)
                        == dst.features.end())
                        dst.features.push_back(f2);
                dst.hostModule = dst.hostModule || spec.hostModule;
                dst.reexport   = dst.reexport   || spec.reexport;
            }
        }
        return {};
}

    // #243: dep/feat forwarding. When a resolved package's feature F is active,
    // it may forward features to its dependencies (Cargo `[features] F =
    // ["dep/feat"]`). Injecting the forwarded feature into the child's request
    // BEFORE the child is pushed onto the worklist makes BOTH consumption points
    // observe it: resolution (mergeActiveFeatureDeps reads the child's
    // spec.features) and activation (recordDependencyEdge stores spec.features on
    // the P->D edge, which aggregatedRequest unions and apply() activates).
    // Transitive forwarding rides the BFS forward edge (root -> mid -> leaf).
static void injectForwards(
        const mcpp::manifest::Manifest& parent,
        const std::vector<std::string>& parentActive,
        const std::string& childKey,
        mcpp::manifest::DependencySpec& childSpec) {
        if (parent.featureForwards.empty()) return;
        for (auto const& f : parentActive) {
            auto it = parent.featureForwards.find(f);
            if (it == parent.featureForwards.end()) continue;
            for (auto const& [depKey, depFeat] : it->second) {
                if (depKey != childKey) continue;
                if (std::find(childSpec.features.begin(), childSpec.features.end(),
                              depFeat) == childSpec.features.end())
                    childSpec.features.push_back(depFeat);
            }
        }
}

static bool declaresDependencyKey(const mcpp::manifest::Manifest& pm,
                                  const std::string& key) {
        auto inFeatureDeps = [&](const auto& byFeature) {
            for (auto const& [f, deps] : byFeature)
                if (deps.contains(key)) return true;
            return false;
        };
        if (pm.dependencies.contains(key) || pm.devDependencies.contains(key)
            || pm.buildDependencies.contains(key) || inFeatureDeps(pm.featureDeps))
            return true;
        for (auto const& cc : pm.conditionalConfigs)
            if (cc.dependencies.contains(key) || cc.devDependencies.contains(key)
                || cc.buildDependencies.contains(key)
                || inFeatureDeps(cc.featureDeps))
                return true;
        return false;
}

    // #243: a forward whose active feature targets a dependency that is not
    // declared is a manifest bug — name it instead of silently dropping. Only
    // active features' forwards are checked (lazy, like the
    // unknown-requested-feature gate at ~2875).
    //
    // THE VALIDATOR ASKS WHAT THE FORWARD LANGUAGE DEFINES: IS THE KEY DECLARED
    // IN ANY DEPENDENCY TABLE OF THIS MANIFEST, ON ANY ROW, UNDER ANY FEATURE
    // (#647 E4.1). It used to look in `dependencies` and `devDependencies`
    // only, while `injectForwards` applies a forward to the build-dependency
    // edge as well, so a forward along `[build-dependencies]` was applied and
    // reported as undeclared in the same run, and `--strict` refused a build
    // whose forward had worked. A key declared only for another row, or only
    // under an inactive feature, is declared: on this row the forward reaches
    // no edge and does nothing, which is what a portable manifest means by it.
static std::expected<void, std::string> validateForwards(
        PrepareState& state,
        const mcpp::manifest::Manifest& parent,
        const std::vector<std::string>& parentActive,
        std::string_view parentName) {
        for (auto const& f : parentActive) {
            auto it = parent.featureForwards.find(f);
            if (it == parent.featureForwards.end()) continue;
            for (auto const& [depKey, depFeat] : it->second) {
                if (declaresDependencyKey(parent, depKey)) continue;
                auto msg = std::format(
                    "feature '{}' of '{}' forwards to dependency '{}' (as "
                    "'{}/{}') which no dependency table declares ([dependencies], "
                    "[build-dependencies], [dev-dependencies] or [feature-deps], "
                    "on any row)", f, parentName, depKey, depKey, depFeat);
                if (state.overrides.strict) return std::unexpected(msg);
                mcpp::diag::warning("features/forwarding", msg);
            }
        }
        return {};
}

    // `ResolvedRecord::sourceRef` for a given declaration — see the field's
    // comment. Computed from what was AUTHORED, not from a network round
    // trip: a `branch` reference is compared by name here, and the two
    // clones it may eventually resolve to are a question `resolveSemver`-style
    // ANSWERING code, not this IDENTITY code, would have to ask.
static std::string sourceRefOf(
        PrepareState& state,
        const std::string& kind,
        const mcpp::manifest::DependencySpec& s,
        const std::filesystem::path& resolveRoot,
        const std::string& originalConstraint) {
        if (kind == "git") {
            return std::format("{}#{}={}", s.git, s.gitRefKind, s.gitRev);
        }
        if (kind == "path") {
            std::filesystem::path p = s.path;
            auto base = resolveRoot.empty() ? *state.root : resolveRoot;
            if (p.is_relative()) p = base / p;
            std::error_code ec;
            auto canon = std::filesystem::weakly_canonical(p, ec);
            return (ec ? p : canon).lexically_normal().generic_string();
        }
        // "version": the constraint as authored; empty means unconstrained,
        // matching `addrset::unify`'s treatment of a bare-name claim.
        return originalConstraint.empty() ? std::string("*") : originalConstraint;
}

// The worklist's per-item locals that cross a step boundary within one
// iteration (mcpp#722 / T6) -- the PrepareState pattern one level deeper:
// a phase-local struct passed by reference to the steps of ONE worklist
// item, the way the phase itself is passed PrepareState. Each field is
// a local the original single-function loop body declared once and read
// again in a later part of the same iteration.
struct WorklistItemCtx {
    WorkItem item;
    std::string sourceKind;
    ResolvedKey key;
    // The commit a `git` dependency resolved to, carried out of the clone
    // branch below for the cache identity.
    std::string sourceCommit;
    // The repository member a `git` dependency selected; empty for the
    // repository's root package (#649 E7).
    std::string gitMember;
    std::filesystem::path gitMemberCloneRoot;
    std::filesystem::path dep_root;
    std::optional<mcpp::manifest::Manifest> dep_manifest;
};

static std::expected<void, std::string>
step4b_resolve_identity(PrepareState& state, WorklistItemCtx& ctx) {
    auto& item = ctx.item;

        const auto& name = item.name;
        auto& spec = item.spec;

        mcpp::pm::compat::normalize_nested_namespace(
            spec.namespace_, spec.shortName, spec.legacyDottedKey);
        if (spec.legacyDottedKey) {
            spec.candidates = {{
                .namespace_ = spec.namespace_,
                .shortName = spec.shortName,
            }};
        }

        if (auto r = state.selectDependencyCandidate(spec, name); !r) {
            return std::unexpected(r.error());
        }
        // A root's declaration (a selected member's included) states the
        // identity it selected, for its readers: the lock names, the record.
        for (auto* mf : state.rootDeclarationManifests(item.consumerDepIndex)) {
            if (auto it = mf->dependencies.find(name); it != mf->dependencies.end()) {
                it->second.namespace_ = spec.namespace_;
                it->second.shortName = spec.shortName;
                it->second.candidates = spec.candidates;
            }
        }

        // A `path` edge that stays inside a git clone this graph already
        // resolved names the same git source at the same commit (#649 E7):
        // a member's `spike.fw = { path = ".." }` reaches the repository the
        // application pinned by revision, and is that package rather than a
        // second, path-sourced declaration of it. Only the clone root itself
        // and its `[workspace] members` are mapped; any other directory keeps
        // being an ordinary path.
        if (spec.isPath() && !state.gitCloneBySource.empty()) {
            std::filesystem::path p = spec.path;
            auto base = item.resolveRoot.empty() ? *state.root : item.resolveRoot;
            if (p.is_relative()) p = base / p;
            std::error_code ec;
            auto canon = std::filesystem::weakly_canonical(p, ec);
            if (ec) canon = p.lexically_normal();
            for (auto const& [src, clone] : state.gitCloneBySource) {
                auto rel = canon.lexically_relative(clone.root).generic_string();
                if (rel.empty() || rel.starts_with("..")) continue;
                bool mapped = rel == ".";
                if (!mapped) {
                    if (auto rm = mcpp::manifest::load(clone.root / "mcpp.toml");
                        rm && rm->workspace.present)
                        for (auto const& member : rm->workspace.members)
                            if (std::filesystem::path(member).lexically_normal()
                                    .generic_string() == rel)
                                mapped = true;
                }
                if (!mapped) continue;
                spec.path.clear();
                spec.git = clone.url;
                spec.gitRefKind = clone.refKind;
                spec.gitRev = clone.ref;
                break;
            }
        }

        // Pin SemVer constraint before dedup/fetch.
        if (auto r = state.resolveSemver(spec, name); !r) {
            return std::unexpected(r.error());
        }

        auto& key = ctx.key;
        key = ResolvedKey{
            spec.namespace_,
            spec.shortName.empty() ? name : spec.shortName,
        };
        auto& sourceKind = ctx.sourceKind;
        sourceKind =
            spec.isPath()    ? "path"
            : spec.isGit()    ? "git"
            : "version";
        // A second key over a source that is already resolved takes the
        // identity resolved there; its manifest is not loaded again.
        if (sourceKind != "version") {
            const auto source = sourceRefOf(state, sourceKind, spec, item.resolveRoot,
                                            item.originalConstraint);
            // A key naming another package of the same repository is that
            // member, not a second key over the root's identity (#649 E7).
            bool namesMember = false;
            if (sourceKind == "git")
                if (auto clone = state.gitCloneBySource.find(source);
                    clone != state.gitCloneBySource.end())
                    namesMember = state.gitMemberDeclaring(clone->second.root, key).has_value();
            if (auto bySource = state.identityBySource.find(source);
                !namesMember && bySource != state.identityBySource.end()
                && !(bySource->second == key)) {
                const auto& existing = state.resolved.at(bySource->second);
                const auto& declaring = state.declaringManifest.at(bySource->second);
                if (!declaring.namespaceDeclared) {
                    return std::unexpected(std::format(
                        "one source is reached as two packages: '{}' names it {} "
                        "and '{}' names it {}, and its manifest '{}' declares no "
                        "namespace, so each key gives it its own identity and "
                        "its modules would be compiled twice.\n"
                        "       fix: declare `namespace` in '{}', or write the "
                        "same key in both places.",
                        existing.requestedBy, state.qualifiedKey(bySource->second),
                        item.requestedBy, state.qualifiedKey(key),
                        declaring.path, declaring.path));
                }
                state.reportAdoption(item, key, bySource->second);
                key = bySource->second;
                state.stateAdoptedIdentity(item, key);
            }
        }

    return {};
}

static std::expected<void, std::string>
step4b_identity_version_merge(PrepareState& state, WorklistItemCtx& ctx,
                               std::map<ResolvedKey, ResolvedRecord>::iterator it) {
    auto& item = ctx.item;
    auto& name = item.name;
    auto& spec = item.spec;
    auto& key = ctx.key;
    auto& sourceKind = ctx.sourceKind;

                // SemVer merge attempt: AND-combine the two original
                // constraint strings and ask the index for a single version
                // satisfying both. Same-major caret/tilde/exact pairs that
                // overlap converge here; cross-major or otherwise
                // unsatisfiable pairs fall through to a hard error (a future
                // PR adds multi-version mangling as a Level-1 fallback).
                auto cfg = state.get_cfg(true);
                if (!cfg) return std::unexpected(cfg.error());

                auto merged = mcpp::pm::try_merge_semver(
                    key.ns, key.shortName,
                    it->second.constraint,
                    item.originalConstraint,
                    state.index_route(*cfg), *state.targetPlatform);
                if (!merged) {
                    // Level 1 fallback: multi-version mangling. Two
                    // versions can't be reconciled by SemVer, but they
                    // can coexist in the same build if we mangle the
                    // secondary copy's module name and rewrite the one
                    // consumer that asked for it. The primary keeps its
                    // authored module name so consumers that don't care
                    // about the secondary see no churn.
                    //
                    // MVP scope (these limits surface as clear errors):
                    //   * The conflicting consumer must be a dep, not
                    //     the main package — main-package mangling
                    //     would mean rewriting user-authored sources,
                    //     which is too surprising for a fallback path.
                    //     Only `kMainConsumer`: a selected member reaches here
                    //     only when two members pin two versions, which one
                    //     plan builds by mangling (not `declaredByRoot`).
                    //   * The secondary version must be a leaf (no own
                    //     transitive deps) — recursive mangling is
                    //     deferred to a follow-up.
                    if (item.consumerDepIndex == kMainConsumer) {
                        return std::unexpected(std::format(
                            "dependency '{}{}{}' has irreconcilable versions:\n"
                            "  '{}' (constraint '{}') requested by '{}'\n"
                            "  '{}' (constraint '{}') requested by '{}'\n"
                            "SemVer merge: {}\n"
                            "Multi-version mangling can't help here — the conflict "
                            "involves the main package directly. Pin one version "
                            "explicitly in your mcpp.toml.",
                            key.ns, key.ns.empty() ? "" : ".", key.shortName,
                            it->second.version, it->second.constraint, it->second.requestedBy,
                            spec.version, item.originalConstraint, item.requestedBy,
                            merged.error()));
                    }

                    auto loaded = state.loadVersionDep(name, key.ns, key.shortName, spec.version);
                    if (!loaded) return std::unexpected(loaded.error());
                    auto& [secondaryRoot, secondaryManifest] = *loaded;

                    if (!secondaryManifest.dependencies.empty()) {
                        return std::unexpected(std::format(
                            "dependency '{}{}{}' has irreconcilable versions:\n"
                            "  '{}' requested by '{}'\n"
                            "  '{}' requested by '{}'\n"
                            "Multi-version mangling fallback only handles leaf "
                            "secondaries in 0.0.3 — but the secondary v{} declares "
                            "its own dependencies, which would need recursive "
                            "mangling. Pin one version explicitly, or wait for "
                            "the recursive-mangling extension.",
                            key.ns, key.ns.empty() ? "" : ".", key.shortName,
                            it->second.version, it->second.requestedBy,
                            spec.version, item.requestedBy,
                            spec.version));
                    }

                    // Module names are authored API and are not required to
                    // mirror package identity. Discover every provided module
                    // root from the secondary's source text, then rewrite the
                    // same map in both the secondary and its consumer.
                    auto moduleNames = declared_modules_for(
                        secondaryRoot, secondaryManifest);
                    // The two branches above name both versions and who asked
                    // for them; this one used to report only that the package
                    // declares no named C++ module, which is a true statement
                    // about a package the reader never asked to be staged. A
                    // C package -- compat.vulkan-runtime is one -- reaches
                    // here whenever a manifest pins one version of it and
                    // another dependency asks for a second, and the message
                    // has to say that before it says anything about modules.
                    if (!moduleNames) return std::unexpected(std::format(
                        "dependency '{}{}{}' has irreconcilable versions:\n"
                        "  '{}' requested by '{}'\n"
                        "  '{}' requested by '{}'\n"
                        "Multi-version mangling cannot separate them: {}.\n"
                        "A package with no named C++ module has nothing to "
                        "rewrite, so the two requests must agree. Align the "
                        "pin in your mcpp.toml with the version the other "
                        "dependency asks for.",
                        key.ns, key.ns.empty() ? "" : ".", key.shortName,
                        it->second.version, it->second.requestedBy,
                        spec.version, item.requestedBy,
                        moduleNames.error()));
                    std::map<std::string, std::string> rename;
                    for (auto const& module : *moduleNames) {
                        rename.emplace(module,
                            mcpp::pm::mangle_name(module, spec.version));
                    }
                    const auto& moduleName = moduleNames->front();
                    const auto& mangledModule = rename.at(moduleName);
                    const std::string mangledPackage = mcpp::pm::mangle_name(
                        key.shortName, spec.version);

                    // Stage layout:
                    //   <root>/target/.mangled/<consumerPkg>/<dep>__<version>/    ← rewritten secondary source
                    //   <root>/target/.mangled/<consumerPkg>/__self__/             ← rewritten consumer source
                    auto& consumerManifest = *state.dep_manifests[item.consumerDepIndex];
                    auto consumerRoot      = state.packages[item.consumerDepIndex + 1].root;
                    // Under the write root, not the source root: the stage
                    // is build output, and BuildOverrides::work_dir promises
                    // that everything the build writes moves with it.
                    auto stageBase         = state.workRoot / "target" / ".mangled"
                                             / consumerManifest.package.name;
                    auto secStage          = stageBase
                                             / std::format("{}__{}", key.shortName, spec.version);
                    auto consumerStage     = stageBase / "__self__";

                    if (auto r = stage_with_rewrite(secondaryRoot, secStage,
                                                     secondaryManifest, rename); !r)
                        return std::unexpected(r.error());
                    if (auto r = stage_with_rewrite(consumerRoot, consumerStage,
                                                     consumerManifest, rename); !r)
                        return std::unexpected(r.error());

                    // Re-anchor the consumer's PackageRoot at its staged copy
                    // so the modgraph scanner picks up the rewritten imports.
                    state.packages[item.consumerDepIndex + 1].root = consumerStage;

                    // Record the staged secondary as a brand-new dep entry
                    // under its mangled name, so future encounters of this
                    // exact (ns, mangled) pair dedup cleanly. The original
                    // primary entry (it->second) is untouched.
                    auto stagedManifest = secondaryManifest;
                    // Give the staged package a distinct atomic identity too;
                    // authored module names remain independent and are carried
                    // exclusively by the rename map above.
                    stagedManifest.package.name = mangledPackage;
                    if (stagedManifest.package.namespace_.empty()) {
                        stagedManifest.package.namespace_ = key.ns.empty()
                            ? std::string(mcpp::pm::kDefaultNamespace) : key.ns;
                    }
                    stagedManifest.package.sourceProvenance = std::format(
                        "index+{}@{}", state.cache_index_name(key.ns), spec.version);
                    // Absolutize secondary's include_dirs against its original
                    // install root so the staged copy still finds headers.
                    for (auto& inc : stagedManifest.buildConfig.includeDirs) {
                        if (inc.is_relative()) inc = secondaryRoot / inc;
                    }
                    for (auto& inc : stagedManifest.buildConfig.includeDirsAfter) {
                        if (inc.is_relative()) inc = secondaryRoot / inc;
                    }

                    state.dep_manifests.push_back(
                        std::make_unique<mcpp::manifest::Manifest>(std::move(stagedManifest)));
                    state.dep_cache_identities.push_back({
                        .indexName   = state.cache_index_name(key.ns),
                        .packageName = mangledPackage,
                        .version     = spec.version,
                        .sourceKind  = "version",
                    });
                    const auto depPackageIndex = state.packages.size();
                    auto secPackage = makePackageRoot(state, secStage, *state.dep_manifests.back());
                    if (!secPackage) return std::unexpected(secPackage.error());
                    state.packages.push_back(std::move(*secPackage));
                    recordDependencyEdge(state, item.consumerDepIndex, depPackageIndex,
                                         spec, item.buildOnly, name);
                    auto linkFlagsAdded = propagateLinkFlags(state, secStage, *state.dep_manifests.back());

                    ResolvedKey mangledKey{key.ns, mangledPackage};
                    state.resolved[mangledKey] = ResolvedRecord{
                        .version           = spec.version,
                        .constraint        = item.originalConstraint,
                        .requestedBy       = item.requestedBy,
                        .source            = "version",
                        .sourceRef         = item.originalConstraint.empty()
                                                 ? std::string("*") : item.originalConstraint,
                        // The mangling fallback refuses a main-package
                        // participant earlier (see the branch's comment
                        // above), so this record's requester is always a
                        // dependency.
                        .fromRoot          = false,
                        .devOnly           = item.devOnly,
                        .depIndex          = state.dep_manifests.size() - 1,
                        .linkFlagsAdded    = std::move(linkFlagsAdded),
                    };

                    mcpp::ui::info("Mangled",
                        std::format("{} v{} ↔ v{} → {} (cross-major fallback)",
                            moduleName, it->second.version, spec.version,
                            mangledModule));
                    return {};
                }

                // Combine the constraint strings so future merges AND with
                // both. Empty originalConstraint means "any" — use "*".
                const std::string& addCstr =
                    item.originalConstraint.empty() ? std::string("*")
                                                    : item.originalConstraint;
                if (it->second.constraint.empty())
                    it->second.constraint = addCstr;
                else
                    it->second.constraint += "," + addCstr;

                if (*merged == it->second.version) {
                    // The existing pin already satisfies the new constraint —
                    // no re-fetch needed; just record this consumer edge.
                    recordDependencyEdge(state, item.consumerDepIndex,
                                         it->second.depIndex + 1,
                                         spec, item.buildOnly, name);
                    return {};
                }

                // Merged version differs from the previously-pinned one.
                // Re-fetch the dep at the merged version and replace the
                // earlier slot in dep_manifests / packages so the build plan
                // sees only one version. Old include_dir entries are evicted
                // and the new manifest's entries are appended.
                mcpp::ui::info("Merged",
                    std::format("{}{}{} {} ⨯ {} → v{}",
                        key.ns, key.ns.empty() ? "" : ".", key.shortName,
                        it->second.version, spec.version, *merged));
                auto reloaded = state.loadVersionDep(name, key.ns, key.shortName, *merged);
                if (!reloaded) return std::unexpected(reloaded.error());
                auto& [newRoot, newManifest] = *reloaded;

                // Name match against the re-loaded manifest.
                {
                    const std::string& expectedShort =
                        spec.shortName.empty() ? name : spec.shortName;
                    // Also accept the fully-qualified form (ns.short) since
                    // synthesize_from_xpkg_lua may set package.name to the
                    // composite name for backward compat.
                    auto expectedComposite = spec.namespace_.empty()
                        ? std::string{}
                        : std::format("{}.{}", spec.namespace_, expectedShort);
                    const bool nameOk =
                        newManifest.package.name == expectedShort
                        || newManifest.package.name == name
                        || (!expectedComposite.empty()
                            && newManifest.package.name == expectedComposite);
                    if (!nameOk) {
                        return std::unexpected(std::format(
                            "dependency '{}' (merged to v{}) resolved to "
                            "package '{}' (mismatch with declared name '{}')",
                            name, *merged, newManifest.package.name,
                            expectedShort));
                    }
                }
                if (newManifest.package.namespace_.empty()) {
                    newManifest.package.namespace_ = key.ns.empty()
                        ? std::string(mcpp::pm::kDefaultNamespace) : key.ns;
                }
                newManifest.package.sourceProvenance = std::format(
                    "index+{}@{}", state.cache_index_name(key.ns), *merged);

                removeLinkFlags(state, it->second.linkFlagsAdded);
                auto linkFlagsAdded = propagateLinkFlags(state, newRoot, newManifest);

                // Replace in dep_manifests + packages. depIndex is the slot
                // in dep_manifests; packages = [main, dep_0, dep_1, …], so
                // packages[depIndex+1] is the same dep.
                *state.dep_manifests[it->second.depIndex] = std::move(newManifest);
                auto mergedPackage =
                    makePackageRoot(state, newRoot, *state.dep_manifests[it->second.depIndex]);
                if (!mergedPackage) return std::unexpected(mergedPackage.error());
                state.packages[it->second.depIndex + 1] = std::move(*mergedPackage);
                recordDependencyEdge(state, item.consumerDepIndex,
                                     it->second.depIndex + 1,
                                     spec, item.buildOnly, name);

                it->second.version            = *merged;
                it->second.linkFlagsAdded     = std::move(linkFlagsAdded);
                if (it->second.depIndex < state.dep_cache_identities.size())
                    state.dep_cache_identities[it->second.depIndex].version = *merged;

                // Walk the *new* manifest's deps so their constraints feed
                // future merges. Already-resolved children dedup via the
                // resolved map.
                const std::string newLabel = std::format("{}{}{}@{}",
                    key.ns, key.ns.empty() ? "" : ".",
                    key.shortName, *merged);
                for (auto& [child_name, child_spec] :
                        state.dep_manifests[it->second.depIndex]->dependencies) {
                    state.worklist.push_back({child_name, child_spec, newLabel,
                                        child_spec.version,
                                        it->second.depIndex, {}, item.devOnly});
                }
                return {};
    return {};
}

static std::expected<void, std::string>
step4b_handle_already_resolved(PrepareState& state, WorklistItemCtx& ctx,
                                std::map<ResolvedKey, ResolvedRecord>::iterator it) {
    auto& item = ctx.item;
    auto& name = item.name;
    auto& spec = item.spec;
    auto& key = ctx.key;
    auto& sourceKind = ctx.sourceKind;

            // A package is dev-only until some non-dev consumer wants it. Order
            // of arrival must not decide, so this is an AND over every request.
            it->second.devOnly = it->second.devOnly && item.devOnly;
            // Conflict detection: a KIND clash (`path`/`git`/`version` differ).
            // Rows 4 and 5 of the decision table in the 2026-09-13-630 record
            // §2.2. Two non-root requesters keep the outright refusal (row
            // 5); when the root is a party, its declaration wins instead
            // (row 4) — a whole-graph choice of WHICH checkout an identity
            // resolves to is exactly the kind of decision
            // `DependencySpec::linkage` already reserves to the root's own
            // edges (dep_spec.cppm).
            if (it->second.source != sourceKind) {
                const bool existingIsRoot = it->second.fromRoot;
                const bool incomingIsRoot = state.declaredByRoot(item.consumerDepIndex);

                if (!existingIsRoot && !incomingIsRoot) {
                    return std::unexpected(std::format(
                        "dependency '{}{}{}' is requested as both a {} dep "
                        "(by '{}') and a {} dep (by '{}'). Pick one.\n"
                        "       declare '{}{}{}' in the root to settle it.",
                        key.ns, key.ns.empty() ? "" : ".", key.shortName,
                        it->second.source, it->second.requestedBy,
                        sourceKind, item.requestedBy,
                        key.ns, key.ns.empty() ? "" : ".", key.shortName));
                }
                // Two selected members, each a root: refused, naming both.
                if (it->second.fromSelectedMember && incomingIsRoot && item.consumerDepIndex
                    != kMainConsumer && it->second.requestedBy != item.requestedBy)
                    return std::unexpected(state.twoMembersRefusal(key,
                        std::format("a {} dep by '{}'", it->second.source, it->second.requestedBy),
                        std::format("a {} dep by '{}'", sourceKind, item.requestedBy)));
                if (incomingIsRoot && !existingIsRoot) {
                    // FIFO SEEDING MAKES THIS UNREACHABLE. Every root-declared
                    // identity is pushed onto `worklist` before this loop
                    // starts; a transitive dependency's request is pushed
                    // onto the BACK of the same deque while the loop runs.
                    // The root's own entry for any identity is therefore
                    // always dequeued — and resolved — before any
                    // dependency's request for that identity can arrive. If
                    // this branch is ever reached, the invariant broke
                    // upstream (the seed reordered, or a new seed source was
                    // added after the loop starts): refusing and naming the
                    // invariant is safer than silently letting whichever side
                    // arrived first win, which is the accident #630 reports.
                    return std::unexpected(std::format(
                        "internal: dependency '{}{}{}': the root's "
                        "declaration arrived after '{}' had already resolved "
                        "it. This is unreachable under first-in-first-out "
                        "worklist seeding; please report this as an mcpp "
                        "engine defect.",
                        key.ns, key.ns.empty() ? "" : ".", key.shortName,
                        it->second.requestedBy));
                }

                // The root already holds this identity (existingIsRoot); the
                // incoming, non-root declaration is overridden. When the
                // OVERRIDDEN declaration is a version requirement, it is
                // still a promise about the graph and is checked against
                // what the root's checkout actually is — the same
                // Holds/Violated test `addrset::unify` runs for a tool pin
                // (address_set.cppm).
                if (sourceKind == "version") {
                    const std::string winnerVersion = it->second.source == "version"
                        ? it->second.version
                        : (it->second.depIndex < state.dep_manifests.size()
                               ? state.dep_manifests[it->second.depIndex]->package.version
                               : std::string{});
                    auto req = mcpp::version_req::parse_req(item.originalConstraint);
                    auto ver = mcpp::version_req::parse_version(winnerVersion);
                    // An unparseable requirement or checkout version is
                    // reported as an override below rather than refused: a
                    // refusal manufactured from ignorance is worse than the
                    // silent override it would be preventing (the same
                    // reasoning `addrset::check` states for an unparseable
                    // spelling).
                    if (req && ver && !mcpp::version_req::matches(*req, *ver)) {
                        return std::unexpected(std::format(
                            "'{}{}{}' is pinned to {} (version {}) by '{}', "
                            "and '{}' requires {}.\n"
                            "       One checkout of a package is used, so the "
                            "two cannot both hold.\n"
                            "       fix: relax the requirement, or point the "
                            "root's pin at a checkout satisfying it.",
                            key.ns, key.ns.empty() ? "" : ".", key.shortName,
                            it->second.sourceRef, winnerVersion,
                            it->second.requestedBy,
                            item.requestedBy, item.originalConstraint));
                    }
                }

                mcpp::diag::warning("dependency/source-override", std::format(
                    "'{}{}{}' is declared as a {} dep (by '{}', {}) and as a "
                    "{} dep (by '{}', {}); the root's declaration wins.",
                    key.ns, key.ns.empty() ? "" : ".", key.shortName,
                    it->second.source, it->second.requestedBy, it->second.sourceRef,
                    sourceKind, item.requestedBy,
                    sourceKind == "version" ? item.originalConstraint
                                            : sourceRefOf(state, sourceKind, spec,
                                                          item.resolveRoot,
                                                          item.originalConstraint)),
                    std::format("declare '{}{}{}' in the root to choose the other.",
                        key.ns, key.ns.empty() ? "" : ".", key.shortName));

                if (it->second.depIndex + 1 < state.packages.size()) {
                    recordDependencyEdge(state, item.consumerDepIndex,
                                         it->second.depIndex + 1,
                                         spec, item.buildOnly, name);
                }
                return {};
            }
            if (sourceKind == "version" && it->second.version != spec.version) {
                if (auto r = step4b_identity_version_merge(state, ctx, it); !r)
                    return std::unexpected(r.error());
                return {};
            }
            // SAME kind, possibly DIFFERENT reference: two `git` declarations
            // of different rev/tag/branch, or two `path` declarations of
            // different directories. Row 3 of the decision table (`version`
            // vs `version` is handled above and never reaches here). Before
            // this comparison existed, the second declaration's reference was
            // never even read — the record kept no `path`/`gitRev`, so there
            // was nothing to compare, and the winner was whichever request
            // happened to be dequeued first (the #630 "accident of queue
            // order").
            if (sourceKind != "version") {
                const std::string incomingRef =
                    sourceRefOf(state, sourceKind, spec, item.resolveRoot, item.originalConstraint);
                if (incomingRef != it->second.sourceRef) {
                    const bool existingIsRoot = it->second.fromRoot;
                    const bool incomingIsRoot = state.declaredByRoot(item.consumerDepIndex);
                    if (incomingIsRoot && !existingIsRoot) {
                        // See the identical comment in the kind-clash branch
                        // above: unreachable under FIFO seeding, and refused
                        // by name rather than silently swapped in.
                        return std::unexpected(std::format(
                            "internal: dependency '{}{}{}': the root's "
                            "declaration arrived after '{}' had already "
                            "resolved it. This is unreachable under "
                            "first-in-first-out worklist seeding; please "
                            "report this as an mcpp engine defect.",
                            key.ns, key.ns.empty() ? "" : ".", key.shortName,
                            it->second.requestedBy));
                    }
                    // Two selected members, as in the kind clash above.
                    if (it->second.fromSelectedMember && incomingIsRoot && item.consumerDepIndex
                        != kMainConsumer && it->second.requestedBy != item.requestedBy)
                        return std::unexpected(state.twoMembersRefusal(key,
                            std::format("{} '{}' by '{}'", sourceKind, it->second.sourceRef,
                                        it->second.requestedBy),
                            std::format("{} '{}' by '{}'", sourceKind, incomingRef,
                                        item.requestedBy)));
                    // The already-resolved record wins either way: it is the
                    // root's (existingIsRoot) or it is simply the first one
                    // dequeued (neither party is the root). Both are "the
                    // first requester" in the sense row 3 states — the root
                    // is dequeued before any transitive request under FIFO
                    // seeding, so "the root wins" and "the first dequeued
                    // wins" never disagree about WHICH record already sits in
                    // `resolved`.
                    mcpp::diag::warning("dependency/source-override", std::format(
                        "'{}{}{}' is declared as {} '{}' (by '{}') and as {} "
                        "'{}' (by '{}'); {} wins.",
                        key.ns, key.ns.empty() ? "" : ".", key.shortName,
                        sourceKind, it->second.sourceRef, it->second.requestedBy,
                        sourceKind, incomingRef, item.requestedBy,
                        existingIsRoot && !incomingIsRoot
                            ? std::string("the root's declaration")
                            : std::format("'{}', declared first",
                                          it->second.requestedBy)),
                        std::format("declare '{}{}{}' in the root to choose "
                                    "the other.",
                            key.ns, key.ns.empty() ? "" : ".", key.shortName));
                }
            }
            // Same key, same version (or compatible path/git) — already
            // processed; still record the dependency edge before skipping.
            // Usage propagation is per edge, not per unique package: two
            // consumers can need the same dep's public surface even though
            // the dep itself is fetched/scanned once.
            if (it->second.depIndex + 1 < state.packages.size()) {
                recordDependencyEdge(state, item.consumerDepIndex,
                                     it->second.depIndex + 1,
                                     spec, item.buildOnly, name);
            }
            return {};
    return {};
}

static std::expected<void, std::string>
step4b_acquire_dependency_source(PrepareState& state, WorklistItemCtx& ctx) {
    auto& item = ctx.item;
    auto& name = item.name;
    auto& spec = item.spec;
    auto& key = ctx.key;
    auto& sourceCommit = ctx.sourceCommit;
    auto& gitMember = ctx.gitMember;
    auto& gitMemberCloneRoot = ctx.gitMemberCloneRoot;

        auto& dep_root = ctx.dep_root;

        if (spec.isPath()) {
            // Path-based: resolve relative to the consumer's root dir.
            // For top-level deps this is the project root; for transitive
            // deps it's the parent dep's directory (stored in resolveRoot).
            dep_root = spec.path;
            auto base = item.resolveRoot.empty() ? *state.root : item.resolveRoot;
            if (dep_root.is_relative()) dep_root = base / dep_root;
            dep_root = std::filesystem::weakly_canonical(dep_root);
        } else if (spec.isGit()) {
            // Git-based (M4 #5): clone into ~/.mcpp/git/<hash>/ and treat
            // as a path dep from there.
            //
            // Two independent questions, each answered by at most one network
            // operation and therefore guarded by exactly one --offline gate:
            //
            //   1. WHICH COMMIT?  `tag`/`rev` name one outright. A `branch` is
            //      floating: mcpp.lock answers it, else `git ls-remote` does.
            //   2. IS IT ON DISK? The commit selects the cache directory, so a
            //      miss — or a clone parked on the wrong commit — is a clone.
            //
            // mcpp.lock is authoritative for (1), not a hint that (2) has to
            // confirm: a recorded commit is used whether or not the clone
            // survived, so evicting ~/.mcpp/git can never quietly move a build
            // onto a newer branch tip. `mcpp update <dep>` drops the entry and
            // stays the one way a branch advances.
            auto mcppHome = mcpp::home::root();   // single resolver (#311)

            const bool remoteIsLocal = is_local_git_remote(spec.git);
            auto refuse_offline = [&](std::string_view need,
                                      std::string_view why,
                                      std::string_view verb) {
                refusal::record(refusal::Code::OfflineDownloadRequired);
                return std::unexpected(std::format(
                    "offline mode: git dependency '{}' needs {} of '{}'\n"
                    "       {}\n"
                    "       run without --offline (or unset MCPP_OFFLINE) to {} it",
                    name, need, spec.git, why, verb));
            };
            const bool offline =
                !remoteIsLocal && mcpp::platform::env::offline_mode();

            // ── 1. which commit ──
            std::string resolvedGitRev = spec.gitRev;
            bool fromLock = false;
            if (spec.gitRefKind == "branch") {
                auto it = state.gitLockAnchors.find(name);
                if (it != state.gitLockAnchors.end()
                    && it->second.url     == spec.git
                    && it->second.refKind == spec.gitRefKind
                    && it->second.ref     == spec.gitRev
                    && it->second.resolvedCommit) {
                    resolvedGitRev = *it->second.resolvedCommit;
                    fromLock = true;
                } else {
                    if (offline)
                        return refuse_offline("`git ls-remote`",
                            std::format("mcpp.lock records no commit for branch "
                                        "'{}'", spec.gitRev),
                            "resolve");
                    // The FIRST network step of a git dependency, and therefore
                    // the one a transient fault is most likely to meet.
                    auto r = run_with_network_retry(std::format(
                        "git ls-remote {} {} 2>&1",
                        mcpp::platform::shell::quote(spec.git),
                        mcpp::platform::shell::quote(
                            std::format("refs/heads/{}", spec.gitRev))));
                    if (r.exit_code != 0)
                        return std::unexpected(std::format(
                            "git ls-remote of '{}' failed:\n{}",
                            spec.git, r.output));
                    // Cleared first: `operator>>` leaves the target untouched
                    // when the stream is already at EOF, which would otherwise
                    // let the declared branch name pass the emptiness check.
                    resolvedGitRev.clear();
                    std::istringstream is(r.output);
                    is >> resolvedGitRev;
                    if (resolvedGitRev.empty())
                        return std::unexpected(std::format(
                            "git branch '{}' not found in '{}'",
                            spec.gitRev, spec.git));
                }
            }

            // ── 2. is it on disk ──
            // Cache key: hash(url + refkind + declared ref + resolved commit).
            // For fixed rev/tag deps the declared ref is also the resolved ref.
            // Deterministic across hosts: `std::hash` is not (see the note on
            // mcpp::pm::index_package_digest). This key names the git cache
            // directory AND the lock hash below, so a host-dependent hash made
            // both the cache directory and mcpp.lock differ by platform.
            auto H = [](std::string_view s) -> std::string {
                return mcpp::toolchain::hash_string(s);
            };
            auto gitRoot = mcppHome / "git" / H(spec.git + "|" + spec.gitRefKind
                  + "|" + spec.gitRev + "|" + resolvedGitRev);
            std::error_code ec;
            std::filesystem::create_directories(gitRoot.parent_path(), ec);

            // A branch's resolved rev is always a sha by now, so the clone can
            // be checked against it — catching one killed between `git clone`
            // and `git checkout`, which would otherwise serve the wrong commit
            // from a correctly-named directory forever. tag/rev keep their ref
            // name as the identity, so there is nothing to compare.
            bool cachePresent = std::filesystem::exists(gitRoot / ".git");
            if (cachePresent && spec.gitRefKind == "branch"
                && git_cache_head(gitRoot) != resolvedGitRev) {
                std::filesystem::remove_all(gitRoot, ec);
                cachePresent = false;
            }

            // Reported before the clone, not instead of it: when the cache is
            // gone this line is the whole explanation for why the build is on
            // an older commit than the branch now points at.
            if (fromLock)
                mcpp::ui::info("Resolved",
                    std::format("{} (branch = {}) from mcpp.lock",
                        spec.git, spec.gitRev));

            if (!cachePresent) {
                if (offline)
                    return refuse_offline("a clone",
                        std::format("no cached clone at {}", gitRoot.string()),
                        "fetch");
                mcpp::ui::info("Cloning",
                    std::format("{} ({} = {})", spec.git, spec.gitRefKind, spec.gitRev));
                // A commit taken from the lock may sit behind the branch tip,
                // and a tag/rev may sit anywhere in history — both need full
                // history before the checkout. Only a tip just read from
                // ls-remote is guaranteed present in a depth-1 clone.
                //
                // `git -C` rather than `cd <dir> &&`: on Windows `cd` does not
                // change drive without /d, and the cache root routinely lives
                // on a different one than the project.
                auto cloneCmd = (spec.gitRefKind == "branch" && !fromLock)
                    ? std::format(
                        "git clone --progress --depth 1 --branch {} {} {} && "
                        "git -C {} checkout --quiet {} 2>&1",
                        mcpp::platform::shell::quote(spec.gitRev),
                        mcpp::platform::shell::quote(spec.git),
                        mcpp::platform::shell::quote(gitRoot.string()),
                        mcpp::platform::shell::quote(gitRoot.string()),
                        mcpp::platform::shell::quote(resolvedGitRev))
                    : std::format(
                        "git clone --progress {} {} && git -C {} checkout --quiet {} 2>&1",
                        mcpp::platform::shell::quote(spec.git),
                        mcpp::platform::shell::quote(gitRoot.string()),
                        mcpp::platform::shell::quote(gitRoot.string()),
                        mcpp::platform::shell::quote(resolvedGitRev));
                // See `run_with_network_retry` for why, and for what the
                // callback is removing between attempts.
                auto r = run_with_network_retry(cloneCmd, [&] {
                    std::filesystem::remove_all(gitRoot, ec);
                }, spec.git);
                if (r.exit_code != 0) {
                    std::filesystem::remove_all(gitRoot, ec);
                    return std::unexpected(std::format(
                        "git clone of '{}' failed:\n{}", spec.git, r.output));
                }
            }
            // A selected workspace member's own git dependencies are locked
            // as a root's are (workspace design 2026-09-29 §15).
            if (state.declaredByRoot(item.consumerDepIndex)) {
                // Only root deps are locked: the writer below walks the root
                // manifest's [dependencies], so a transitive git branch dep
                // has no anchor and still resolves over the network.
                auto source = std::format("git+{}#{}={}",
                    spec.git, spec.gitRefKind, spec.gitRev);
                if (spec.gitRefKind == "branch") source += "@" + resolvedGitRev;
                state.root_git_lock_identities[name] = GitLockIdentity{
                    .source = std::move(source),
                    .hash = "fnv1a:" + H(spec.git + "|"
                        + spec.gitRefKind + "|" + spec.gitRev + "|"
                        + resolvedGitRev),
                };
            }
            sourceCommit = resolvedGitRev;
            dep_root = gitRoot;
            state.gitCloneBySource.try_emplace(
                sourceRefOf(state, "git", spec, item.resolveRoot, item.originalConstraint),
                GitClone{ gitRoot, spec.git, spec.gitRefKind, spec.gitRev });
            if (auto member = state.gitMemberDeclaring(gitRoot, key)) {
                gitMember = *member;
                gitMemberCloneRoot = gitRoot;
                dep_root = gitRoot / *member;
            }
        }
        // (version-source: dep_root + manifest are loaded together via
        // loadVersionDep below since the index entry drives both.)

        // Manifest acquisition.
        //   - Path/git dep: dep_root is the source tree, mcpp.toml at root.
        //   - Version dep: delegate to loadVersionDep — the index entry's
        //     `mcpp` field decides where mcpp.toml lives (StringPath /
        //     TableBody / default lookup).
        auto& dep_manifest = ctx.dep_manifest;
        if (spec.isPath() || spec.isGit()) {
            if (!std::filesystem::exists(dep_root / "mcpp.toml")) {
                return std::unexpected(std::format(
                    "{} dependency '{}' (at '{}') has no mcpp.toml",
                    spec.isGit() ? "git" : "path", name, dep_root.string()));
            }
            // A MEMBER IS A MEMBER HOWEVER IT IS REACHED.
            //
            // A workspace member that omits `package.version` because
            // `[workspace.package]` supplies it is legal — and it is reached
            // here as a sibling's `path` dependency, which is the ordinary
            // shape rather than an exotic one. Loading it as an anonymous path
            // dependency would refuse it for a field the workspace does
            // provide, and the message would name the member's manifest rather
            // than the table that answers.
            //
            // `is_workspace_member` asks the workspace's own `members` list, so
            // a vendored copy or an example living inside the tree is still
            // refused for a missing version, exactly as before.
            const bool depIsMember =
                state.wsManifest && !state.runtimeWorkspaceRoot.empty()
                && mcpp::project::is_workspace_member(
                       *state.wsManifest, state.runtimeWorkspaceRoot, dep_root);
            auto dm = mcpp::manifest::load(
                dep_root / "mcpp.toml",
                {.insideWorkspace = depIsMember || !gitMember.empty()});
            if (!dm) {
                return std::unexpected(std::format(
                    "dependency '{}' (at '{}'): {}",
                    name, dep_root.string(), dm.error().format()));
            }
            dep_manifest = std::move(*dm);
            // A member reached as a dependency inherits here, at its load
            // site; see `inherit_as_workspace_member`. A member of a
            // git-hosted workspace inherits from ITS repository, anchored at
            // the clone.
            auto inheritAsMember = [&](const mcpp::manifest::Manifest& ws,
                                       const std::filesystem::path& wsRoot) {
                return inherit_as_workspace_member(*dep_manifest, ws, wsRoot, dep_root);
            };
            // A rooted workspace's own package, the member "." of a workspace
            // plan (§15), is the workspace's manifest: it reads its own
            // `[workspace.dependencies]`, as it did as the root, and receives
            // `[workspace.package]` and `[workspace.build]` as every member does.
            const bool depIsWorkspacePackage = state.workspacePlan() && state.wsManifest
                && dep_root.lexically_normal() == state.runtimeWorkspaceRoot.lexically_normal();
            if (depIsWorkspacePackage) {
                mcpp::project::merge_workspace_deps(*dep_manifest, *state.wsManifest,
                                                    state.runtimeWorkspaceRoot);
                mcpp::project::inherit_as_root_package(*dep_manifest,
                                                       state.runtimeWorkspaceRoot);
            } else if (depIsMember) {
                if (auto bad = inheritAsMember(*state.wsManifest, state.runtimeWorkspaceRoot))
                    return std::unexpected(*bad);
            } else if (!gitMember.empty()) {
                if (auto rm = mcpp::manifest::load(gitMemberCloneRoot / "mcpp.toml")) {
                    if (auto bad = inheritAsMember(*rm, gitMemberCloneRoot))
                        return std::unexpected(*bad);
                }
            }
            if (auto bad = mcpp::project::unresolved_workspace_dependency_error(
                    *dep_manifest, dep_root))
                return std::unexpected(std::format("dependency '{}': {}", name, *bad));
            // #229: path/git-dep half of the L1 cfg funnel — mirrors the
            // loadVersionDep call site above (loadFrom's L1 cfg merge, ~1740
            // lines up). Before this fix, path/git deps never ran this merge
            // at all: their `[target.'cfg(...)'.build] sources` were parsed
            // into `conditionalConfigs` but never folded into
            // `buildConfig.sources` / `modules.sources`, so the modgraph scan
            // never saw the file — link-time `undefined reference`. Must run
            // BEFORE `propagateLinkFlags`/`makePackageRoot` below, which
            // snapshot this manifest's flags/sources into `packages[]`.
            if (!dep_manifest->conditionalConfigs.empty()) {
                merge_conditional_config(*dep_manifest,
                    state.cfgCtx());
            }
            report_flag_words_changes(*dep_manifest);
            fold_build_defines_into_flags(dep_manifest->buildConfig);
            // The root's `abi.threads` reaches this dependency's C translation
            // units here, as it does for a version dependency.
            if (state.abiThreadsRendered) state.add_once(dep_manifest->buildConfig.cflags, "-pthread");
        } else {
            auto loaded = state.loadVersionDep(name, key.ns, key.shortName, spec.version);
            if (!loaded) return std::unexpected(loaded.error());
            dep_root     = std::move(loaded->first);
            dep_manifest = std::move(loaded->second);
        }
    return {};
}

static std::expected<void, std::string>
step4b_finalize_dependency(PrepareState& state, WorklistItemCtx& ctx) {
    auto& item = ctx.item;
    auto& name = item.name;
    auto& spec = item.spec;
    auto& key = ctx.key;
    auto& sourceKind = ctx.sourceKind;
    auto& sourceCommit = ctx.sourceCommit;
    auto& gitMember = ctx.gitMember;

        // Name match via compat::resolve_package_name — handles both
        // canonical (explicit namespace field) and legacy (dotted name)
        // forms transparently.
        {
            auto resolved = mcpp::pm::compat::resolve_package_name(
                ctx.dep_manifest->package.name, ctx.dep_manifest->package.namespace_);
            const std::string& expectedShort =
                spec.shortName.empty() ? name : spec.shortName;
            const bool nameOk =
                resolved.shortName == expectedShort
                || ctx.dep_manifest->package.name == expectedShort
                || ctx.dep_manifest->package.name ==
                    mcpp::pm::compat::qualified_name(spec.namespace_, expectedShort);
            if (!nameOk) {
                return std::unexpected(std::format(
                    "dependency '{}' resolved to package '{}' (mismatch with declared name '{}')",
                    name, ctx.dep_manifest->package.name, expectedShort));
            }
        }

        // The identity a `path` or `git` manifest declares is the package's,
        // whatever key reached it (#634, A2). Before this, only the short name
        // was compared, so `fw` reaching a manifest that declares `huxdemo.fw`
        // resolved as `mcpplibs.fw` while every reader that builds a name from
        // the manifest saw `huxdemo.fw`, and a second edge written
        // `huxdemo.fw` put the same sources into the build twice.
        const bool namespaceDeclared = !ctx.dep_manifest->package.namespace_.empty();
        const std::string manifestPath = sourceKind == "version"
            ? std::string{}
            : (ctx.dep_root / "mcpp.toml").lexically_normal().generic_string();
        if (sourceKind != "version" && namespaceDeclared) {
            auto declaredName = mcpp::pm::compat::resolve_package_name(
                ctx.dep_manifest->package.name, ctx.dep_manifest->package.namespace_);
            ResolvedKey declared{ ctx.dep_manifest->package.namespace_,
                                  declaredName.shortName };
            if (!(declared == key)) {
                state.reportAdoption(item, key, declared);
                state.stateAdoptedIdentity(item, declared);
                if (state.resolved.contains(declared)) {
                    // Another source already resolved the declared identity,
                    // and the rules for two declarations of one identity
                    // decide (the #630 decision table, at the resolved-record
                    // hit above). The edge is queued again stating that
                    // identity, which sends it there.
                    item.spec.namespace_ = declared.ns;
                    item.spec.shortName = declared.shortName;
                    item.spec.candidates = {{ .namespace_ = declared.ns,
                                              .shortName = declared.shortName }};
                    item.spec.namespaceOmitted = false;
                    item.spec.legacyCandidateSearch = false;
                    item.spec.legacyDottedKey = false;
                    state.worklist.push_front(std::move(item));
                    return {};
                }
                key = declared;
            }
        }

        // Stamp the identity with the resolver's exact coordinate and source.
        // A descriptor that omitted namespace inherits the coordinate that
        // answered it; otherwise two indices containing the same short name
        // collapse in runtime provenance even though resolution distinguished
        // them correctly.
        //
        // A workspace member of a workspace plan is the project being
        // developed, and keeps the identity its own manifest states, as when
        // it was the root of its own build (workspace design 2026-09-29 §15):
        // a member that declares no namespace is named by its bare name.
        const bool workspaceMemberHere = state.workspacePlan() && sourceKind == "path"
            && (ctx.dep_root.lexically_normal() == state.runtimeWorkspaceRoot.lexically_normal()
                || !workspace_member_of(state, ctx.dep_root).empty());
        if (ctx.dep_manifest->package.namespace_.empty() && !workspaceMemberHere) {
            ctx.dep_manifest->package.namespace_ = key.ns.empty()
                ? std::string(mcpp::pm::kDefaultNamespace) : key.ns;
        }
        if (sourceKind == "version") {
            ctx.dep_manifest->package.sourceProvenance = std::format(
                "index+{}@{}", state.cache_index_name(key.ns), spec.version);
        } else if (sourceKind == "git") {
            ctx.dep_manifest->package.sourceProvenance = std::format(
                "git+{}#{}={}", spec.git, spec.gitRefKind, spec.gitRev);
        } else {
            ctx.dep_manifest->package.sourceProvenance =
                "path+" + ctx.dep_root.lexically_normal().generic_string();
        }

        // Stage 2a: merge this dependency's active feature-deps into its own
        // dependency set before its children are pushed, so a dep's feature can
        // transitively pull a provider. `spec.features` = features the consumer
        // requested for this dep.
        if (auto fm = mergeActiveFeatureDeps(*ctx.dep_manifest, spec.features,
                                             spec.defaultFeatures); !fm)
            return std::unexpected(fm.error());

        // A PACKAGE OF PROGRAMS HAS NOTHING TO LINK (#649 E6). Its tools are
        // built by the tool sub-build, which resolves the package as its own
        // root; in this graph it is a provider of tools and of its directory,
        // and nothing more. Walking its dependencies here put a tool's own
        // library into the application's link (a tool depending on `z` gave the
        // application `z.o`), compiled its sources in the consumer's build, and
        // made a tool that depends on the package declaring it a cycle of the
        // consumer's graph although the two builds never meet.
        // A selected workspace member is built in this plan whatever its
        // targets are (workspace design 2026-09-29 §15).
        const auto selectedMember = state.selectedMemberAt(ctx.dep_root);
        const bool depProgramOnly = state.isProgramOnlyPackage(*ctx.dep_manifest)
                                 && spec.artifacts.empty() && !selectedMember;
        // A root receives the profile's own compile flags; in a workspace plan
        // each selected member does, as it did when it was the root. The
        // tests `mcpp test` discovered are the selected member's targets; a
        // plan of several members receives each member's own.
        if (selectedMember && state.selectedMembers.size() == 1)
            for (auto const& t : state.extraTargets) ctx.dep_manifest->targets.push_back(t);
        if (selectedMember)
            if (auto t = state.memberTargets.find(*selectedMember); t != state.memberTargets.end())
                for (auto const& target : t->second) ctx.dep_manifest->targets.push_back(target);
        if (selectedMember) {
            auto& mbc = ctx.dep_manifest->buildConfig;
            mbc.cflags.insert(mbc.cflags.end(), state.profileCflags.begin(),
                              state.profileCflags.end());
            mbc.cxxflags.insert(mbc.cxxflags.end(), state.profileCxxflags.begin(),
                                state.profileCxxflags.end());
        }
        auto linkFlagsAdded = depProgramOnly
            ? std::vector<std::string>{}
            : propagateLinkFlags(state, ctx.dep_root, *ctx.dep_manifest);

        // Move the manifest into stable storage so we can later look it up
        // by depIndex (the SemVer merger needs to overwrite the slot).
        state.dep_manifests.push_back(
            std::make_unique<mcpp::manifest::Manifest>(std::move(*ctx.dep_manifest)));
        state.dep_cache_identities.push_back({
            .indexName   = state.cache_index_name(key.ns),
            .packageName = name,
            .version     = sourceKind == "version"
                ? spec.version
                : state.dep_manifests.back()->package.version,
            .sourceKind  = sourceKind,
            .sourceRef   = sourceKind == "git"  ? sourceCommit
                         : sourceKind == "path" ? ctx.dep_root.string()
                         : std::string{},
        });
        const auto depPackageIndex = state.packages.size();
        auto depPackage = makePackageRoot(state, ctx.dep_root, *state.dep_manifests.back());
        if (!depPackage) return std::unexpected(depPackage.error());
        if (selectedMember) {
            depPackage->selectedMember = true;
            depPackage->memberProducts = state.selectedMembers.at(*selectedMember);
        }
        state.packages.push_back(std::move(*depPackage));
        recordDependencyEdge(state, item.consumerDepIndex, depPackageIndex, spec,
                             item.buildOnly, name);

        // Record this dep as resolved so future encounters of the same
        // (ns, name) hit the fast path (skip / merge / conflict).
        if (sourceKind != "version") {
            state.identityBySource.emplace(
                sourceRefOf(state, sourceKind, spec, item.resolveRoot, item.originalConstraint)
                    + (gitMember.empty() ? std::string{} : "#member=" + gitMember),
                key);
            state.declaringManifest[key] = DeclaringManifest{ manifestPath, namespaceDeclared };
        }
        state.resolved[key] = ResolvedRecord{
            .version           = sourceKind == "version" ? spec.version : "",
            .constraint        = sourceKind == "version" ? item.originalConstraint : "",
            .requestedBy       = item.requestedBy,
            .source            = sourceKind,
            .sourceRef         = sourceRefOf(state, sourceKind, spec, item.resolveRoot,
                                             item.originalConstraint),
            .fromRoot          = state.declaredByRoot(item.consumerDepIndex),
            .fromSelectedMember = item.consumerDepIndex != kMainConsumer
                                  && state.declaredByRoot(item.consumerDepIndex),
            .devOnly           = item.devOnly,
            .depIndex          = state.dep_manifests.size() - 1,
            .linkFlagsAdded    = std::move(linkFlagsAdded),
        };

        // Recurse: the dep's own [dependencies] become new worklist items.
        // dev-dependencies are intentionally NOT walked — those are
        // private to the dep's test runs, not part of its public ABI.
        // A package of programs is not walked at all; see `depProgramOnly`.
        if (depProgramOnly) return {};
        const std::string thisDepLabel = std::format(
            "{}{}{}@{}",
            key.ns,
            key.ns.empty() ? "" : ".",
            key.shortName,
            sourceKind == "version" ? spec.version : sourceKind);
        const std::size_t selfIdx = state.dep_manifests.size() - 1;
        // #243: forward this dep's active features to ITS children before they
        // are pushed (transitive dep->dep forwarding rides the BFS forward
        // edge). Uses the SAME closure inputs as mergeActiveFeatureDeps above
        // (this edge's spec.features, already carrying any forward injected by
        // this dep's own consumer, + defaultFeatures), so activation agrees
        // with resolution.
        auto depActive = feature_closure(*state.dep_manifests.back(), spec.features,
                                         spec.defaultFeatures);
        if (auto fe = validateForwards(state, *state.dep_manifests.back(), depActive,
                                       state.dep_manifests.back()->package.name); !fe)
            return std::unexpected(fe.error());
        // `--features <dependency>/<feature>` for a selected member: a forward
        // of that member, applied to its edges as the root's are.
        auto injectMemberCliForwards = [&](const std::string& childKey,
                                           mcpp::manifest::DependencySpec& childSpec) {
            if (!selectedMember) return;
            auto fw = state.memberCliForwards.find(*selectedMember);
            if (fw == state.memberCliForwards.end()) return;
            for (auto const& [depKey, depFeat] : fw->second)
                if (depKey == childKey
                    && std::ranges::find(childSpec.features, depFeat) == childSpec.features.end())
                    childSpec.features.push_back(depFeat);
        };
        for (auto& [child_name, child_spec] : state.dep_manifests.back()->dependencies) {
            auto childReq = child_spec;
            injectForwards(*state.dep_manifests.back(), depActive, child_name, childReq);
            injectMemberCliForwards(child_name, childReq);
            state.worklist.push_back({child_name, childReq, thisDepLabel,
                                childReq.version, selfIdx, ctx.dep_root,
                                item.devOnly, item.buildOnly});
        }
        // A selected member's `[dev-dependencies]` under `mcpp test`, as a
        // root's: its own, and never walked into its dependencies' tests.
        if (selectedMember && state.includeDevDeps) {
            for (auto& [child_name, child_spec] : state.dep_manifests.back()->devDependencies) {
                auto childReq = child_spec;
                injectForwards(*state.dep_manifests.back(), depActive, child_name, childReq);
                injectMemberCliForwards(child_name, childReq);
                state.worklist.push_back({child_name, childReq, thisDepLabel + " (dev-dep)",
                                    childReq.version, selfIdx, ctx.dep_root,
                                    /*devOnly=*/true, item.buildOnly});
            }
        }
        // A dependency's own `[build-dependencies]` — the only channel through
        // which a package can speak about what IT needs at build time. Both
        // live channels (`tools`, `host-module`) are written by the CONSUMER
        // on an edge, so before this a build rule could not request anything
        // on its own behalf. That, and not a design decision, is why a rule
        // was a leaf.
        //
        // These are build-only regardless of how this package was reached: a
        // library's build dependency has no business in its consumer's binary
        // either.
        for (auto& [child_name, child_spec] :
                 state.dep_manifests.back()->buildDependencies) {
            auto childReq = child_spec;
            injectForwards(*state.dep_manifests.back(), depActive, child_name, childReq);
            injectMemberCliForwards(child_name, childReq);
            state.worklist.push_back({child_name, childReq,
                                thisDepLabel + " (build-dep)",
                                childReq.version, selfIdx, ctx.dep_root,
                                item.devOnly, /*buildOnly=*/true});
        }
    return {};
}

static std::expected<void, std::string> step4b_cycle_check(PrepareState& state) {
    // ONE PLACE DETECTS A CYCLE OF PACKAGES, AND IT IS HERE, WHERE THE GRAPH
    // IS RESOLVED (#649 E6). The build-cache key walk was the only reader that
    // noticed, and it runs for the global cache only, so the same manifest was
    // refused by default and built under `--cache=local`. Every edge counts,
    // build-only ones included, as the key walk counts them.
    {
        mcpp::graph::AdjacencyList deps(state.packages.size());
        for (auto const& e : state.dependencyEdges) {
            if (e.consumerPackageIndex >= deps.size()) continue;
            if (e.dependencyPackageIndex >= state.packages.size()) continue;
            deps[e.consumerPackageIndex].push_back(e.dependencyPackageIndex);
        }
        if (auto order = mcpp::graph::topological_order(deps); !order) {
            std::string path;
            for (auto p : order.error().cycle) {
                if (!path.empty()) path += " -> ";
                path += std::format("'{}'",
                    mcpp::build::qualified_package_name(state.packages[p].manifest));
            }
            refusal::record(refusal::Code::PackageCycle);
            return std::unexpected(std::format(
                "dependency cycle: {}.\n"
                "       A package cannot reach itself through its own dependencies.\n"
                "       fix: remove one of these edges. A program that depends on the "
                "package requesting it builds without a cycle when its package "
                "declares only `kind = \"bin\"` targets: it is then built by its "
                "own tool sub-build.", path));
        }
    }
    return {};
}

static void step4b_define_lookup_closures(PrepareState& state) {
    state.bareBindingsFor = [&](std::size_t consumer) {
        std::vector<std::string> fqns;
        if (consumer < state.provisionGraph.visible.size())
            for (auto const& pr : state.provisionGraph.visible[consumer]) {
                if (pr.provider >= state.packages.size()) continue;
                auto const& n = state.packages[pr.provider].manifest.package.name;
                if (std::find(fqns.begin(), fqns.end(), n) == fqns.end())
                    fqns.push_back(n);
            }
        return prov::bind_bare_names(fqns);
    };
    // THE NAMES UNDER WHICH ONE PROVIDER IS PUBLISHED TO ONE CONSUMER, derived
    // once for every channel (#647 E4.3). The manifest's `name`, the qualified
    // `namespace.name` when the manifest writes the two apart, and the bare
    // tail where the namespace ladder binds it to this provider for this
    // consumer. `dep_dir`/`dep_linkage` and `dep_bin` used to derive this list
    // separately; #642 added the qualified spelling to the first and the second
    // kept publishing `MCPP_DEP_INSTALLER_BIN_*` alone for a package written
    // `namespace = "spike"`, `name = "installer"`, so
    // `dep_bin("spike.installer", ...)` read nothing.
    state.publishedNamesFor =
        [&](std::size_t provider,
            const std::map<std::string, prov::BareBinding>& bind) {
        std::vector<std::string> out;
        auto const& manifest = state.packages[provider].manifest;
        auto const& canon = manifest.package.name;
        out.push_back(canon);
        if (auto qualified = mcpp::build::qualified_package_name(manifest);
            qualified != canon)
            out.push_back(std::move(qualified));
        if (auto tail = prov::tail_of(canon); tail != canon) {
            auto it = bind.find(tail);
            if (it != bind.end() && it->second.owner == canon)
                out.push_back(std::move(tail));
        }
        return out;
    };

    // A package whose DECLARED targets are all programs (#649 E6). See the
    // worklist, where such a package is not walked into a consumer's graph.
    state.isProgramOnlyPackage = [](const mcpp::manifest::Manifest& pm) {
        if (pm.targetsInferred || pm.targets.empty()) return false;
        return std::ranges::none_of(pm.targets, [](const mcpp::manifest::Target& t) {
            return t.kind == mcpp::manifest::Target::Library
                || t.kind == mcpp::manifest::Target::SharedLibrary;
        });
    };
    // A package some edge asked for programs to SHIP (mcpp#711). Its programs
    // are linked in this plan, so it is scanned and configured here like any
    // library dependency, even when every target it declares is a program.
    state.isArtifactPackage = [&](std::size_t i) {
        return std::ranges::any_of(state.dependencyEdges, [&](const DependencyEdge& e) {
            return e.dependencyPackageIndex == i && !e.requestedArtifacts.empty();
        });
    };
    // Compiled in this plan: not a package of programs, or one whose programs
    // this plan ships.
    state.isWorkspaceMemberPackage = [&](std::size_t i) {
        if (i == 0 || i >= state.packages.size() || !state.workspacePlan()) return false;
        if (state.packages[i].selectedMember) return true;
        const auto& r = state.packages[i].root;
        if (r.lexically_normal() == state.runtimeWorkspaceRoot.lexically_normal()) return true;
        return !workspace_member_of(state, r).empty();
    };
    // A selected workspace member is compiled here whatever its targets are
    // (workspace design 2026-09-29 §15).
    state.compilesHere = [&](std::size_t i) {
        return i == 0 || !state.isProgramOnlyPackage(state.packages[i].manifest)
            || state.isArtifactPackage(i) || state.packages[i].selectedMember;
    };


    state.appendUniquePath =
        [](std::vector<std::filesystem::path>& dirs,
           const std::filesystem::path& dir) -> bool
    {
        if (std::find(dirs.begin(), dirs.end(), dir) != dirs.end()) return false;
        dirs.push_back(dir);
        return true;
    };

    state.appendUniquePaths =
        [&](std::vector<std::filesystem::path>& dirs,
            const std::vector<std::filesystem::path>& additions) -> bool
    {
        bool changed = false;
        for (auto const& dir : additions) {
            changed = state.appendUniquePath(dirs, dir) || changed;
        }
        return changed;
    };

    // "Which compile-visible channels a build.mcpp directive lands in" is a
    // property of the DIRECTIVE TABLE, not of this call site, so both the mark
    // and the fold now live with the table in mcpp.build.directives. This pair
    // used to be defined here and was already incomplete — the comment it
    // replaced admitted that link/source residues stayed at the call sites,
    // which is the #242 two-derivations shape.
    //
    // The fold is PRIVATE by design (Cargo discipline — a build-time program
    // must not widen the package's public interface): privateBuild only, never
    // publicUsage. The after-dirs ride the typed #249 channel, which owns the
    // per-dialect degradations (cl.exe /I, NASM -I).
    using DirectiveMark = mcpp::build::directives::Mark;
    state.markDirectiveTail = [](const mcpp::manifest::Manifest& mm) {
        return mcpp::build::directives::mark(mm);
    };
    state.foldDirectiveTailIntoPrivateBuild =
        [](mcpp::modgraph::PackageRoot& pkg, const mcpp::manifest::Manifest& ran,
           const DirectiveMark& t)
    {
        mcpp::build::directives::fold_private_tail(pkg.privateBuild, ran, t);
    };

    // mcpp#241: the (name → dir) pairs a package's build.mcpp receives as
    // MCPP_DEP_<NAME>_DIR. ONE owner: the dependency loop and the root call
    // site had drifted into two near-identical copies of this, and #355 was
    // about to add a third. Each dependency is emitted under BOTH its
    // canonical name and its namespace-stripped tail, so
    // `mcpp::dep_dir("compat.zlib")` and `mcpp::dep_dir("zlib")` both resolve
    // regardless of which spelling the author used in `deps`.
    //
    // #359: the set is now the consumer's VISIBLE provisions rather than its
    // direct edges, so a re-exported dependency's directory reaches it too.
    // That is what makes a rule package able to find data files belonging to a
    // dependency the user never declared — protoc's well-known .proto files
    // are exactly such a directory, and `grpcgen` reads them through dep_dir.
    //
    // The bare tail is emitted only when the namespace ladder binds it here.
    // Emitting it unconditionally was safe while only the root's own
    // declarations reached build.mcpp; with re-export, two packages that never
    // heard of each other can share a tail and the later emplace_back would
    // silently win.
    // The xlings half of fillDepDirs. Same question ("where did my declared
    // dependency's payload land"), different namespace and store layout, so it
    // cannot ride the mcpp dependency channel — but it must be an INTERFACE on
    // the build.mcpp side for the same reason that one is: a program that
    // reconstructs the store path is coupled to internals mcpp is free to
    // change. See mcpp::build::hostprogram::xpkg_dir.

}

static std::expected<void, std::string>
step4b_define_provisioning_closures(PrepareState& state) {
    // Which dependency supplied the runner, for the exactly-one-provider
    // error below. A name rather than a bool: the message has to name both.
    // ONE PROVIDER PER RUNNER NAME. `runner` has had this rule since #544;
    // a NAMED runner inherits it per name, because a board may legitimately
    // supply `flash` while a different package supplies `monitor`.

    // The payload facts a build program receives: where each declared payload
    // is, and where it came from (mcpp#755). One definition, in sources.cpp,
    // beside the override and on-request rules it reads.
    state.fillXpkgDirs = [&](mcpp::build::BuildProgramEnv& e,
                            const mcpp::manifest::Manifest& owner,
                            std::size_t consumer) {
        fill_xpkg_env(state, e, owner, consumer);
    };

    // `linkForms` (#642 E2): when given, each dependency that has a resolved
    // library form is also offered under exactly the names its directory is,
    // so `dep_linkage(n)` answers for every `n` that `dep_dir(n)` answers for.
    // Only the root's program passes it; see the root call site for why.
    state.fillDepDirs = [&](mcpp::build::BuildProgramEnv& e, std::size_t consumer,
                           const std::map<std::size_t, std::string>* linkForms = nullptr) {
        if (consumer >= state.provisionGraph.visible.size()) return;
        auto bind = state.bareBindingsFor(consumer);
        for (auto const& [tail, b] : bind) {
            if (auto note = prov::contest_note(tail, b); !note.empty())
                mcpp::diag::warning("provisions/ambiguous", note);
        }
        for (auto const& pr : state.provisionGraph.visible[consumer]) {
            if (pr.kind != prov::Kind::DepDir) continue;
            if (pr.provider >= state.packages.size()) continue;
            auto const& depPkg = state.packages[pr.provider];
            auto const& canon  = depPkg.manifest.package.name;
            const std::string* form = nullptr;
            if (linkForms)
                if (auto f = linkForms->find(pr.provider); f != linkForms->end())
                    form = &f->second;
            // Every spelling of `publishedNamesFor`: the manifest's name, the
            // qualified name a manifest writing `namespace = "ns"` and
            // `name = "fw"` is addressed by (#642: the framework's rule asks
            // `dep_linkage("huxerui.huxerui")`), and the bound tail.
            for (auto const& n : state.publishedNamesFor(pr.provider, bind)) {
                e.depDirs.emplace_back(n, depPkg.root);
                if (form) e.depLinkages.emplace_back(n, *form);
            }
        }
    };

    // A declared build-graph node's Source outputs must be visible to the
    // scan, so they are materialized as placeholders and joined to the source
    // set here — the same two lists `generated=` feeds, for the same reason
    // (the scanner walks the legacy modules.sources mirror). ninja overwrites
    // the placeholder before the compile edge runs, because that compile
    // depends on the action's output.
    state.adoptActionOutputs = [](mcpp::manifest::Manifest& mm,
                                 const std::filesystem::path& pkgRoot,
                                 std::size_t firstNewAction) {
        if (firstNewAction >= mm.buildConfig.actions.size()) return;
        std::vector<mcpp::manifest::BuildAction> fresh(
            mm.buildConfig.actions.begin()
                + static_cast<std::ptrdiff_t>(firstNewAction),
            mm.buildConfig.actions.end());
        // The package that DECLARED the outputs classifies them: a dependency
        // generating a `.ixx` asks its own manifest, not the root project's.
        // Built once per package, not once per output — and BEFORE
        // `prepare_actions`, which needs the same table to decide which
        // outputs get a placeholder (a header does not; see mcpp#534).
        const auto pkgExtTable =
            mcpp::extension_table_for(mm.buildConfig.moduleExtensions,
                                      mm.buildConfig.deviceExtensions);
        mcpp::build::directives::prepare_actions(fresh, pkgRoot, pkgExtTable);
        std::copy(fresh.begin(), fresh.end(),
                  mm.buildConfig.actions.begin()
                      + static_cast<std::ptrdiff_t>(firstNewAction));
        for (auto const& a : fresh) {
            if (a.role != mcpp::manifest::BuildAction::Role::Source) continue;
            for (auto const& o : a.outputs) {
                if (o.find("${mcpp.") != std::string::npos) continue;
                // Companion outputs (protoc's .pb.h next to its .pb.cc) are
                // produced by the edge but are NOT translation units.
                if (!mcpp::build::directives::is_compilable_output(o, pkgExtTable))
                    continue;
                mm.buildConfig.sources.push_back(o);
                mm.modules.sources.push_back(o);
            }
        }
    };


    state.appendUniqueFlags =
        [](std::vector<std::string>& flags,
           const std::vector<std::string>& additions) -> bool
    {
        bool changed = false;
        for (auto const& f : additions) {
            if (std::find(flags.begin(), flags.end(), f) != flags.end()) continue;
            flags.push_back(f);
            changed = true;
        }
        return changed;
    };





    {
        auto rootPackage = makePackageRoot(state, *state.root, *state.m);
        if (!rootPackage) return std::unexpected(rootPackage.error());
        state.packages[0] = std::move(*rootPackage);
    }


    state.computeUsageRequirements = [&] {
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto const& edge : state.dependencyEdges) {
                if (edge.consumerPackageIndex >= state.packages.size()
                    || edge.dependencyPackageIndex >= state.packages.size()) {
                    continue;
                }
                auto& consumer = state.packages[edge.consumerPackageIndex];
                auto const& dependency = state.packages[edge.dependencyPackageIndex];
                // A package of programs publishes no usage requirements to its
                // consumers (#649 E6): nothing of it is compiled or linked here.
                if (edge.dependencyPackageIndex > 0
                    && state.isProgramOnlyPackage(dependency.manifest)) continue;

                if (edge.visibility == mcpp::modgraph::DependencyVisibility::Private
                    || edge.visibility == mcpp::modgraph::DependencyVisibility::Public) {
                    changed = state.appendUniquePaths(consumer.privateBuild.includeDirs,
                                                dependency.publicUsage.includeDirs)
                              || changed;
                    // #249: after-dirs ride the same edges but keep their
                    // after-ness — consumers receive them as -idirafter,
                    // never upgraded to -I.
                    changed = state.appendUniquePaths(consumer.privateBuild.includeDirsAfter,
                                                dependency.publicUsage.includeDirsAfter)
                              || changed;
                    // Interface defines (a dependency's active-feature `defines`)
                    // ride the same edges as include dirs: they must reach the
                    // consumer's own TUs so header-only switches like
                    // EIGEN_USE_BLAS take effect where the headers are used.
                    changed = state.appendUniqueFlags(consumer.privateBuild.cflags,
                                                dependency.publicUsage.cflags)
                              || changed;
                    changed = state.appendUniqueFlags(consumer.privateBuild.cxxflags,
                                                dependency.publicUsage.cxxflags)
                              || changed;
                }
                if (edge.visibility == mcpp::modgraph::DependencyVisibility::Public
                    || edge.visibility == mcpp::modgraph::DependencyVisibility::Interface) {
                    changed = state.appendUniquePaths(consumer.publicUsage.includeDirs,
                                                dependency.publicUsage.includeDirs)
                              || changed;
                    changed = state.appendUniquePaths(consumer.publicUsage.includeDirsAfter,
                                                dependency.publicUsage.includeDirsAfter)
                              || changed;
                    changed = state.appendUniqueFlags(consumer.publicUsage.cflags,
                                                dependency.publicUsage.cflags)
                              || changed;
                    changed = state.appendUniqueFlags(consumer.publicUsage.cxxflags,
                                                dependency.publicUsage.cxxflags)
                              || changed;
                }
            }
        }
    };

    return {};
}

std::expected<void, std::string> phase4b_graph_worklist(PrepareState& state) {

    // #634, X: every request that reached a package, as the requester wrote
    // it, for the `graph` section of resolution.json. Kept apart from
    // `dependencyEdges`, which merges two requests of one consumer for one
    // dependency into one edge; the record has to keep both keys, because two
    // keys over one identity (A2) and the table a declaration came from (A1)
    // are what it exists to show.
    // The link form each dependency takes and the facts it was decided from,
    // by package index. COMPUTED ONCE, before the root build program runs, so
    // that program can read the answer (#642 E2); APPLIED after the scan, where
    // it always was. Every reader below reads this, never a second resolution.
    // #355: consumer package index → (env var, absolute path) for each host
    // tool that consumer requested. Filled by the provisioning pass below;
    // read by BOTH build.mcpp call sites (the dependency loop and the root),
    // which is why it lives out here rather than inside the resolution block.
    // #355 step 5: consumer package index → (logical module name, interface
    // path) for each dependency that offers HOST build rules. Same fan-out
    // shape as toolEnvByConsumer, and read by the same two call sites.
    // The same providers by INDEX, and the reason they are needed twice.
    //
    // A rule's code runs inside its CONSUMER's build program, so
    // `mcpp::xpkg_dir("cuda-nvcc")` is asked there -- while the payload that
    // answers it was declared by the RULE, under `[feature-xlings.<f>]`, which
    // is where it belongs: which packages a device compiler needs is the
    // rule's knowledge and no project should have to rediscover it.
    //
    // The graph pass already INSTALLS what a dependency declares. Only the
    // answer was missing: `fillXpkgDirs` read one manifest, so the address was
    // fetched, unpacked, and then unreachable from the only code that wanted
    // it -- a failure that reads as "the toolkit is not installed" while it
    // sits on disk.
    //
    // The set is the host-module providers rather than every dependency: the
    // code that can call `xpkg_dir` in this build program is the consumer's
    // own `build.mcpp` plus exactly the rule modules compiled into it.
    // #359: who can see which build-time provision. Computed once by the
    // provisioning pass below (a fixpoint over `dependencyEdges`, the same
    // shape as computeUsageRequirements) and read by every consumer of the
    // three env channels above. Declared here because `fillDepDirs` closes
    // over it and is defined long before the pass runs; every call site is
    // after it.
    // The spellings a given consumer may address a provider by. The qualified
    // name always works; the bare tail only when the namespace ladder binds it
    // to exactly this package FOR THIS CONSUMER. Scoped per consumer rather
    // than globally because two packages sharing a tail only collide inside an
    // environment that contains both.
    step4b_define_lookup_closures(state);

    if (auto r = step4b_define_provisioning_closures(state); !r)
        return std::unexpected(r.error());

    // Pull the root package's active feature-deps into its dependency set before
    // seeding, so `mcpp build --features X` resolves X's optional deps.
    state.rootReq = parse_feature_request(state.overrides.features);
    if (auto fm = mergeActiveFeatureDeps(*state.m, state.rootReq); !fm)
        return std::unexpected(fm.error());
    // #243: the root's active features may forward features to its direct deps.
    std::vector<std::string> rootActive = feature_closure(*state.m, state.rootReq, true);
    if (auto fe = validateForwards(state, *state.m, rootActive, state.m->package.name); !fe)
        return std::unexpected(fe.error());
    state.activeFeaturesByPackage.assign(1, rootActive);

    // `--features <dependency key>/<feature>` (#649 E8): a forward of the root,
    // applied to the edges exactly as a `[features]` forward is and checked
    // against the same tables. Named whether or not the root declares
    // `[features]`: the token cannot be a macro of the root, so there is no
    // "pure macro usage" to preserve for it.
    std::vector<std::pair<std::string, std::string>> cliForwards;
    for (auto const& tok : feature_forward_request_tokens(state.overrides.features)) {
        auto fwd = mcpp::pm::split_feature_forward_token(tok);
        std::string msg;
        if (!fwd)
            msg = std::format("--features requests '{}', which names neither a "
                              "feature nor `<dependency>/<feature>`", tok);
        else if (!declaresDependencyKey(*state.m, fwd->first))
            msg = std::format("--features requests '{}', and no dependency table "
                              "of '{}' declares '{}'", tok, state.m->package.name,
                              fwd->first);
        if (!msg.empty()) {
            if (state.overrides.strict) return std::unexpected(msg);
            mcpp::diag::warning("features/request", msg);
            continue;
        }
        cliForwards.push_back(std::move(*fwd));
    }
    auto injectCliForwards = [&](const std::string& childKey,
                                 mcpp::manifest::DependencySpec& childSpec) {
        for (auto const& [depKey, depFeat] : cliForwards)
            if (depKey == childKey
                && std::ranges::find(childSpec.features, depFeat)
                       == childSpec.features.end())
                childSpec.features.push_back(depFeat);
    };

    // Seed the worklist from the main manifest. Dev-deps only when the
    // caller wants them; they're never propagated transitively.
    const std::string mainPkgLabel = state.m->package.name;
    for (auto& [n, s] : state.m->dependencies) {
        auto req = s;
        injectForwards(*state.m, rootActive, n, req);
        injectCliForwards(n, req);
        state.worklist.push_back({n, req, mainPkgLabel, req.version, kMainConsumer, {}});
    }
    if (state.includeDevDeps) {
        for (auto& [n, s] : state.m->devDependencies) {
            auto req = s;
            injectForwards(*state.m, rootActive, n, req);
            injectCliForwards(n, req);
            state.worklist.push_back({n, req, mainPkgLabel + " (dev-dep)",
                                req.version, kMainConsumer, {}, /*devOnly=*/true});
        }
    }
    // `[build-dependencies]`. Parsed since 0.0.x, merged across workspace
    // members, conditionalised by target predicate — and until now read by
    // nothing that made a decision, so writing it produced a manifest that
    // loaded, no diagnostic, and no effect. Seeded here, and unlike dev-deps
    // it IS walked transitively: a build dependency's own dependencies are
    // what make it work, and they inherit its build-only nature.
    for (auto& [n, s] : state.m->buildDependencies) {
        auto req = s;
        injectForwards(*state.m, rootActive, n, req);
        injectCliForwards(n, req);
        state.worklist.push_back({n, req, mainPkgLabel + " (build-dep)",
                            req.version, kMainConsumer, {}, /*devOnly=*/false,
                            /*buildOnly=*/true});
    }


    while (!state.worklist.empty()) {
        WorklistItemCtx ctx;
        ctx.item = std::move(state.worklist.front());
        state.worklist.pop_front();

        if (auto r = step4b_resolve_identity(state, ctx); !r)
            return std::unexpected(r.error());

        if (auto it = state.resolved.find(ctx.key); it != state.resolved.end()) {
            if (auto r = step4b_handle_already_resolved(state, ctx, it); !r)
                return std::unexpected(r.error());
            continue;
        }

        if (auto r = step4b_acquire_dependency_source(state, ctx); !r)
            return std::unexpected(r.error());
        if (auto r = step4b_finalize_dependency(state, ctx); !r)
            return std::unexpected(r.error());
    }

    if (auto r = step4b_cycle_check(state); !r) return std::unexpected(r.error());

    state.emitAdoptionWarnings();
    state.computeUsageRequirements();

    return {};
}


} // namespace mcpp::build
