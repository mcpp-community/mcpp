// plan.cpp -- P13: the BuildContext: the plan, prebuilt dependencies,
// assembly units, Windows resources and the global cache. mcpp.lock and
// resolution.json are written by records.cpp.

module mcpp.build.prepare;
import :state;

import mcpp.build.prepare_inputs;

import std;
import mcpp.diag;
import mcpp.build.stage;
import mcpp.build.refusal;
import mcpp.build.version_floor;
import mcpp.home;
import mcpp.platform.axis;
import mcpp.libs.json;
import mcpp.log;
import mcpp.manifest;
import mcpp.source_kind;
import mcpp.toolchain.clang;
import mcpp.toolchain.hostflags;   // the compile-token producer the package std module reuses
import mcpp.toolchain.cppfly;
import mcpp.toolchain.detect;
import mcpp.toolchain.dialect;
import mcpp.toolchain.model;      // is_msvc_target — the MSVC-ABI default (#718)
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
import mcpp.build.schedule.policy;
import mcpp.build.flags;          // compute_flags — the per-role contracts (#418)
import mcpp.build.distribution;   // dist::Role / dist::Contract to_string
import mcpp.platform.capacity;   // the host fallback handed to schedule::decide
import mcpp.build.graph_shape;  // #407: the graph says which mode wrote it
import mcpp.build.runtime_validation;  // declared artifact -> identity verdict
import mcpp.build.cache_key;
import mcpp.graph;
import mcpp.pack.abi_tag;      // the tag a prebuilt dependency is checked against
import mcpp.pack.prebuilt;     // …and the check itself
import mcpp.pack.stage_tree;   // where `${mcpp.stage_dir}` points, and its manifest
import mcpp.build.build_program;
import mcpp.build.resources;    // #365 Windows resources: synthesise / scan / find rc
import mcpp.build.backend;      // BuildOptions for the tool sub-build
import mcpp.build.ninja;        // make_ninja_backend — driving that sub-build
import mcpp.lockfile;
import mcpp.config;
import mcpp.xlings;
import mcpp.runtime.binding;
import mcpp.platform.runtime_search;
import mcpp.toolchain.post_install;
import mcpp.platform;
import mcpp.build.runner_lookup;
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
import mcpp.version_req;
import mcpp.ui;
import mcpp.log;
import mcpp.bmi_cache;

namespace mcpp::build {


// SUB-STEPS (mcpp#722 / T6). Each function below is one section of
// phase13_finish, named for what its own banner already called it,
// extracted verbatim: statements moved, not reordered or rewritten. Every
// step takes the same (PrepareState&, BuildContext&) pair phase13_finish
// held locally, called in the original order from the slimmed-down
// phase13_finish at the bottom of this file. Internal linkage: these
// names are this file's own, not part of mcpp.build.prepare's surface;
// the two records steps live in records.cpp and are declared in `:state`.

static void step13_report_packages(PrepareState& state, BuildContext& ctx);

static std::expected<void, std::string> step13_source_packages(PrepareState& state, BuildContext& ctx) {
    {
        std::error_code ec;
        const bool firstPlan = !std::filesystem::exists(ctx.outputDir / "build.ninja", ec);
        for (auto const& [what, hint] : pending_flag_words_notes())
            if (firstPlan) mcpp::diag::warning("build/flag-words", what, hint);
        pending_flag_words_notes().clear();
    }
    ctx.stdBmi     = state.stdBmiPath;
    ctx.stdObject  = state.stdObjectPath;
    // Copied, not moved: `describedStdModule` is read again once `ctx.plan`
    // exists (below), to recover the standard-library units' commands onto
    // it (StdModuleUnit, C5 / D5a-b). A `std::optional` move leaves the
    // source engaged with a moved-from value, so a plain move here would
    // hand build_database.cppm's render() a value and the plan an empty one.
    ctx.stdModule  = state.describedStdModule;
    // Every directory a package payload may legitimately have been INSTALLED
    // into. There is more than one: the global registry, plus the two
    // project-local data roots a custom git index installs into
    // (`config::project_xlings_data_roots`). make_plan uses these to anchor the
    // cache address of a dependency source that lives outside its own package
    // root, and the cacheability gate below uses the same list to decide
    // whether a package's sources really came from a store. ONE definition,
    // two uses — deriving the same fact twice is how the object layout and the
    // cache key drifted apart in the first place (#344).
    // Which source trees does the fast path have to watch besides this one?
    //
    // A package whose root is neither under `projectRoot` nor under a directory
    // mcpp OWNS is a `path` dependency — the shape every workspace member takes
    // towards its siblings — and its sources are read on every build. See
    // BuildContext::depSourceRoots for what the list is for.
    //
    // WHAT IS EXCLUDED, AND WHY IT IS "WHO WROTE THE DIRECTORY" RATHER THAN
    // "WHICH KIND OF DEPENDENCY". An xpkg payload under the store is written
    // once at install time and never edited. A git checkout under
    // `<mcpp home>/git/<hash>` is a pinned revision in a hash-addressed
    // directory: changing the revision changes the directory name, and the
    // manifest that names it is already swept. Neither can change under a warm
    // build, so sweeping them would buy nothing and cost a directory walk per
    // dependency on every invocation — which is the fast path this whole change
    // exists to keep.
    //
    // A `path` dependency is the opposite on both counts: it is the user's
    // working tree, and editing it is the point.
    {
        std::vector<std::filesystem::path> owned = state.storeRoots;
        owned.push_back(mcpp::home::root());
        std::vector<std::filesystem::path> roots;
        // The same enumeration answers a second reader: which packages were
        // read from an editable tree, with their source globs (the build
        // database lists them as the inputs that change the plan).
        auto qualified = [](const mcpp::manifest::Manifest& pm) {
            return pm.package.namespace_.empty()
                ? pm.package.name
                : pm.package.namespace_ + "." + pm.package.name;
        };
        for (std::size_t i = 0; i < state.packages.size(); ++i) {
            const auto& pkgRoot = state.packages[i].root;
            if (pkgRoot.empty()) continue;
            if (i > 0 && mcpp::build::path_is_under_any(pkgRoot, owned)) continue;
            auto normalized = pkgRoot.lexically_normal();
            const bool known = std::ranges::any_of(ctx.sourcePackages,
                [&](const BuildContext::SourcePackage& sp) {
                    return sp.root.lexically_normal() == normalized;
                });
            if (!known)
                ctx.sourcePackages.push_back({qualified(state.packages[i].manifest),
                                              normalized,
                                              state.packages[i].manifest.modules.sources});
            if (i == 0 || normalized == state.root->lexically_normal()) continue;
            if (std::find(roots.begin(), roots.end(), normalized) == roots.end())
                roots.push_back(std::move(normalized));
        }
        // A workspace plan's members are its projects: their trees are what
        // the fast path sweeps, the workspace's own package included.
        if (state.workspacePlan())
            for (std::size_t i = 1; i < state.packages.size(); ++i) {
                if (!state.packages[i].selectedMember) continue;
                auto normalized = state.packages[i].root.lexically_normal();
                if (std::find(roots.begin(), roots.end(), normalized) == roots.end())
                    roots.push_back(normalized);
            }
        ctx.depSourceRoots = std::move(roots);
    }
    // The selected members, in selection order (§15).
    if (state.workspacePlan()) {
        for (auto const& mp : state.selectedMemberPaths) {
            std::error_code ec;
            auto dir = std::filesystem::weakly_canonical(state.runtimeWorkspaceRoot / mp, ec);
            if (ec) dir = (state.runtimeWorkspaceRoot / mp).lexically_normal();
            for (std::size_t i = 1; i < state.packages.size(); ++i) {
                auto const& pkg = state.packages[i];
                if (!pkg.selectedMember) continue;
                std::error_code pec;
                auto root = std::filesystem::weakly_canonical(pkg.root, pec);
                if (pec) root = pkg.root.lexically_normal();
                if (root != dir) continue;
                ctx.workspaceMembers.push_back({
                    .name = pkg.manifest.package.namespace_.empty()
                        ? pkg.manifest.package.name
                        : pkg.manifest.package.namespace_ + "." + pkg.manifest.package.name,
                    .memberPath = mp,
                    .root = pkg.root,
                    .productDir = pkg.memberProducts,
                    .manifest = pkg.manifest,
                });
                break;
            }
        }
    }
    return {};
}

static std::expected<void, std::string> step13_runner_and_xlings(PrepareState& state, BuildContext& ctx) {
    // Where a runner may find the programs this project declared (#544). The
    // same resolution `fillXpkgDirs` hands to build programs, kept as
    // directories rather than env vars because the reader is mcpp's own
    // lookup, not a child process. See BuildContext::xlingsDepBinDirs.
    //
    // AND EVERY PACKAGE IN THE GRAPH, NOT ONLY THE ROOT — WHICH IS THE
    // CASE THIS FEATURE EXISTS FOR.
    //
    // A board-support package is precisely the thing that knows which emulator
    // or probe reaches its machine, and it declares that emulator under its own
    // `[xlings] deps`. Collecting only the ROOT's declarations meant a runner
    // could name a program by bare name only when the CONSUMER had also
    // declared it — which is the duplication the board package exists to
    // remove. Measured on `mcpplibs/aarch64-virt-rt`: with the board naming
    // `qemu-system-aarch64` bare, `mcpp run` searched PATH, found the shim or
    // nothing, and reported a missing runner while the emulator sat installed
    // in the payload the board had declared.
    //
    // Ordering is root-first: a consumer that declares its own payload gets to
    // decide, and a dependency supplies the answer when the consumer said
    // nothing. A payload that is declared but not installed contributes
    // nothing, and the lookup continues to PATH.
    //
    // AND THE SET COLLECTED HERE IS ALSO THE SET PROVISIONED. Looking in a
    // directory that nothing installed is a lookup that can only fail, and the
    // engine had exactly that shape: a dependency's declaration was searched
    // and never acted on. The two definitions are one expression below, so
    // they cannot drift — the third of the three hazards §12.6 named.
    {
        // THE SAME SPLIT THE EARLY PASS USED. Written once, above, next to the
        // provisioning that has to happen before build.mcpp; this site reads it
        // for the records below. Two copies of "what did the graph declare"
        // would be two definitions of the same word.
        auto split = state.graph_xlings_split();
        if (!split) {
            refusal::record(refusal::Code::ToolVersionConflict);
            return std::unexpected(split.error());
        }
        auto& xlingsSpecs = split->first;
        auto& fromGraph   = split->second;
        // THE ROOT'S OWN PASS RAN LONG AGO, AND THIS ONE MUST NOT REPEAT IT.
        // The stamp is keyed by the LIST, so provisioning root+graph together
        // would key a different list than the early pass wrote and re-run an
        // xlings round trip on every build. Only what the graph added is
        // provisioned here, under its own key.
        //
        // Since the graph's pass moved above build.mcpp this call is normally a
        // stamp hit. It is kept rather than deleted because the stamp is keyed
        // by content: if the early pass did not run, or ran on a different
        // list, this is still the site that makes the record true.
        if (!fromGraph.empty()) {
            if (auto cfg = state.get_cfg(true)) {
                if (auto pv = provision_xlings_addresses(
                        **cfg, fromGraph, *state.root,
                        "[xlings.workspace] entries declared by dependencies");
                    !pv) return std::unexpected(pv.error());
            }
        }
        xlingsSpecs.insert(xlingsSpecs.end(), fromGraph.begin(), fromGraph.end());
        // What a RUN would additionally have asked for. Recorded rather than
        // installed: this verb is not running anything, and installing it
        // anyway is the behaviour the tier exists to remove.
        if (state.toolPurpose == ToolPurpose::Build) {
            for (std::size_t i = 0; i < state.packages.size() && !ctx.runTierPending; ++i) {
                const auto& man = state.packages[i].manifest;
                const auto feats = i < state.activeFeaturesByPackage.size()
                    ? state.activeFeaturesByPackage[i] : std::vector<std::string>{};
                for (auto const& spec : applicable_xlings_addresses(
                         man, feats, ToolPurpose::Run, /*isRoot=*/i == 0
                         || state.packages[i].selectedMember))
                    if (std::ranges::find(xlingsSpecs, spec) == xlingsSpecs.end())
                        { ctx.runTierPending = true; break; }
            }
        }
        if (!xlingsSpecs.empty()) {
            if (auto cfg = state.get_cfg(true)) {
                auto xlEnv = mcpp::config::make_xlings_env(**cfg);
                for (auto const& spec : xlingsSpecs) {
                    auto ref = mcpp::xlings::paths::parse_xpkg_ref(spec);
                    if (auto dir = mcpp::xlings::paths::xpkg_payload(xlEnv, ref)) {
                        ctx.xlingsPayloads.push_back(*dir);
                        // `bin/`, then the payload root. The measurement that
                        // added the second entry is recorded with the rule, in
                        // runner_lookup::payload_search_dirs.
                        for (auto& d :
                             mcpp::build::runner_lookup::payload_search_dirs(*dir))
                            ctx.xlingsDepBinDirs.push_back(std::move(d));
                    }
                }
            }
        }
    }
    return {};
}

static std::expected<void, std::string> step13_prebuilt_check(PrepareState& state, BuildContext& ctx) {
    // ─── Prebuilt dependencies: check before planning to link them ─────
    //
    // Here rather than at each place a dependency manifest is loaded, because
    // there are three of those and the check needs the RESOLVED toolchain,
    // which only exists by now. One pass over the assembled package list is
    // also the only spelling under which a package cannot be checked twice
    // with two different answers.
    //
    // The current tag's SHAPE follows the package's: a package that publishes
    // a triple-only tag is saying its interface is `extern "C"`, and comparing
    // it against a full tag would refuse a combination it explicitly allows.
    // `tag_check` already treats an unnamed dimension as don't-care, so one
    // full tag on this side is correct for both.
    {
        const auto canonicalTriple = state.tc->targetTriple.empty()
            ? mcpp::toolchain::triple::host_triple().str()
            : [&] {
                  auto t = mcpp::toolchain::triple::parse(state.tc->targetTriple);
                  return t ? t->str() : state.tc->targetTriple;
              }();
        auto currentTag = mcpp::pack::cxx_surface_tag(
            *state.tc, canonicalTriple, state.m->cppStandard.level);
        // What THIS build targets on the device axis. Absent means it asks for
        // no accelerator, and every artifact then satisfies it vacuously —
        // which is correct, and is why a descriptor lists its CPU-only variant
        // first: the first accepted artifact wins.
        currentTag.accel = mcpp::pack::parse_accel(state.resolvedAccel());
        for (std::size_t i = 1; i < state.packages.size(); ++i) {
            auto const& pkg = state.packages[i];
            if (!mcpp::pack::is_distribution_package(pkg.manifest)) continue;
            mcpp::pack::PrebuiltCheck chk{
                .packageRoot  = pkg.root,
                .packageLabel = mcpp::manifest::package_id(pkg.manifest.package).canonical(),
                .current      = currentTag,
            };
            if (auto ok = mcpp::pack::check_prebuilt(pkg.manifest, chk); !ok)
                return std::unexpected(ok.error());
        }
    }
    return {};
}

static std::expected<void, std::string> step13_link_forms(PrepareState& state, BuildContext& ctx) {
    // ── #519: the form each dependency takes, APPLIED ──────────────────────
    //
    // The answers were computed before the root build program (see there).
    //
    // MATERIALISED AS A TARGET KIND, on purpose. A dependency resolved to
    // the shared form becomes an ordinary `SharedLibrary` target, so every
    // emitter mcpp already has applies to it unchanged — the ELF soname and
    // `$ORIGIN`, the PE import library and generated `.def`, the Mach-O
    // install name. That is the whole reason this axis needs no new backend
    // code on any of the three formats. It also means `make_plan` READS the
    // answer instead of deriving it a second time.
    {
        namespace lf = mcpp::build::linkage_form;

        // A non-root edge that writes the key gets its request IGNORED, and
        // says so — a silently dropped knob is how a knob becomes decoration.
        for (std::size_t i = 1; i < state.packages.size(); ++i)
            for (auto const& [depName, spec] : state.packages[i].manifest.dependencies)
                if (!spec.linkage.empty())
                    mcpp::diag::warning("build/dependency-linkage", std::format(
                        "'{}' asks for dependency '{}' to be linked as '{}'; only "
                        "the root project decides link forms, so this is ignored",
                        state.packages[i].manifest.package.name, depName, spec.linkage));

        for (auto const& [i, form] : state.dependencyLinkForms) {
            auto const& answer = form.answer;
            auto const& facts  = form.facts;

            if (!answer.diagnostic.empty())
                mcpp::diag::degraded("build/dependency-linkage", answer.diagnostic,
                    "this dependency is linked in the other form, which changes "
                    "whether its code travels inside the images that use it");
            // An explicit request honoured against the package's own default
            // (#642 E1): the build did what was asked, so this is information,
            // and it names both statements.
            if (!answer.note.empty())
                mcpp::ui::info("Linkage", answer.note);

            if (answer.linkage != lf::DepLinkage::Shared) continue;
            if (facts.isDistribution) continue;   // nothing here to build
            // A package that ALREADY declares a shared target has decided
            // for itself, and its remaining library targets are not part of
            // that decision. Flipping them would change what such a package
            // builds under the DEFAULT request, which is the one property this
            // axis promises never to touch. (No package in mcpp-index has both
            // shapes at once — compat.vulkan's `lib` is overridden to `shared`
            // on Linux rather than joined by it — but "unreachable today" is
            // how the last few of these got in.)
            if (facts.declaredShared) continue;
            for (auto& t : state.packages[i].manifest.targets)
                if (t.kind == mcpp::manifest::Target::Library)
                    t.kind = mcpp::manifest::Target::SharedLibrary;
        }
    }
    return {};
}

static std::expected<void, std::string> step13_make_plan(PrepareState& state, BuildContext& ctx) {
    // A workspace plan links each member's closure with that closure's own
    // flags (workspace design 2026-09-29 §15): each package's link flags,
    // its build program's included, with search paths made absolute against
    // the package, as the root's pooled list holds them outside a workspace.
    if (state.workspacePlan())
        for (std::size_t i = 1; i < state.packages.size(); ++i)
            state.packages[i].linkUsage.ldflags = normalized_dependency_ldflags(
                state.packages[i].root, state.packages[i].manifest.buildConfig.ldflags);
    auto planResult = mcpp::build::make_plan(*state.m, *state.tc, state.fp, state.scan.graph, state.report.topoOrder,
                                             state.packages, *state.root, ctx.outputDir,
                                             state.stdBmiPath, state.stdObjectPath, state.storeRoots);
    if (!planResult) return std::unexpected(planResult.error());
    ctx.plan        = std::move(*planResult);
    // The request the graph is planned for (§3, §15): the plan's members and
    // the requested features, which name no directory.
    if (state.workspacePlan()) {
        auto join = [](const std::vector<std::string>& v) {
            std::string out;
            for (auto const& x : v) { if (!out.empty()) out += '\x1e'; out += x; }
            return out;
        };
        ctx.workspaceGroup = join(state.selectedMemberPaths);
        ctx.workspaceRequest = state.overrides.workspace_request.empty()
            ? ctx.workspaceGroup : join(state.overrides.workspace_request);
    }
    ctx.plan.requestTag = mcpp::build::request_tag(
        ctx.workspaceGroup,
        state.workspacePlan() ? state.requestedFeatures : state.overrides.features);
    // SPEC-007 R4.3: a declared deploy outranks a search directory's file of
    // the same name, and a difference between the two is stated ONCE, by the
    // post-link placement edge, through the edge-advice channel
    // (mcpp.build.advice) that mcpp reports after a successful build. The
    // planning-time statement that stood here (#727) was a second statement
    // of the same fact, and could not see a directory a `prepare` action
    // fills (WS3 of the 2026-09-28 design).
    // Resolved far above, where the dependency graph first exists. It is
    // attached here rather than threaded through `make_plan` because nothing
    // that function does depends on it: the flag assembly that does reads the
    // plan, and every reader of `compute_flags` runs after this line.
    ctx.plan.targetSide = state.resolvedTargetSide;

    // C5 / D5a-b (design 2026-09-26 §3.5): the standard-library units this
    // configuration's build compiles, when it imports `std`. Recovered here,
    // once, from the SAME command derivation `ensure_built` and
    // `describe_std_module` both read (mcpp.toolchain.stdmod), and carried on
    // the plan (BuildPlan::stdModuleUnits) so compile_commands.json,
    // `emit --spec compile-commands` and the S1 document render the exact
    // same record and cannot disagree (P1, mcpp.build.compile_commands).
    if (state.describedStdModule) {
        const auto& sm = *state.describedStdModule;
        auto add_std_unit = [&](const std::filesystem::path& source,
                                const std::vector<std::string>& commands,
                                const std::filesystem::path& object,
                                const std::filesystem::path& bmi,
                                std::string_view module,
                                std::vector<std::string> requiresModules) {
            if (source.empty() || commands.empty()) return;
            auto inv = mcpp::build::recover_invocation(
                commands, source, state.tc->binaryPath, sm.cacheDir,
                mcpp::platform::is_windows);
            if (!inv) {
                state.planNotes.push_back({"MCPP_BUILD_DATABASE_STD_UNIT_UNDESCRIBED",
                    std::format("no command that builds the {} module names its "
                                "source '{}'; the unit is not listed",
                                module, source.string())});
                return;
            }
            ctx.plan.stdModuleUnits.push_back(mcpp::build::StdModuleUnit{
                .source = source,
                .workDirectory = std::move(inv->workDirectory),
                .arguments = std::move(inv->arguments),
                .object = object,
                .bmi = bmi,
                .module = std::string(module),
                .requiresModules = std::move(requiresModules),
            });
        };
        add_std_unit(state.tc->stdModuleSource, sm.stdCommands, sm.objectPath,
                    sm.bmiPath, "std", {});
        add_std_unit(state.tc->stdCompatSource, sm.compatCommands, sm.compatObjectPath,
                    sm.compatBmiPath, "std.compat", {"std"});
    }
    return {};
}

static std::expected<void, std::string> step13_cxx_private_runtime(PrepareState& state, BuildContext& ctx) {
    // A DEPENDENCY'S C++ SHARED LIBRARY OVER A C++ RUNTIME THAT IS A PACKAGE
    // (#641, item 5).
    //
    // The runtime package is linked like every other static package: its
    // objects go into the program. A dependency's shared library is linked from
    // its own package's objects, and `-nostdlib++` withholds the driver's
    // runtime, so the library has no C++ runtime at all. A private copy does
    // not come for free either: `llvm.libcxx` compiles its classes with hidden
    // visibility, so no image resolves against another's copy, and each image
    // then holds its own type information for the library's classes. Measured
    // on x86_64 Linux, an exception of `std::runtime_error` thrown in such a
    // library is not caught by that type in the program; libc++ documents the
    // same identity split for hidden types on arm64 Apple.
    //
    // So the private copy is linked only when the manifest states it for
    // shared libraries (`cxx_runtime = { shared = "self-contained" }`, the key
    // that already means a private runtime in each shared library for the
    // payload's runtime), and every other case is refused here, before
    // anything compiles. Each build this refuses failed at link before.
    if (state.resolvedTargetSide.cxx.fromGraph() && state.cxxLayerProviderIndex
        && *state.cxxLayerProviderIndex < state.packages.size()) {
        namespace dist = mcpp::build::dist;
        auto const& provider = state.packages[*state.cxxLayerProviderIndex].manifest;
        const auto providerName = mcpp::build::qualified_package_name(provider);
        auto const& bc = ctx.plan.manifest.buildConfig;
        const auto format = dist::format_for(
            state.tc->targetTriple,
            mcpp::platform::is_windows ? dist::Format::Pe
            : mcpp::platform::is_macos ? dist::Format::MachO
                                       : dist::Format::Elf);
        const bool privateCopy =
            dist::stated_shared_library_contract(bc.cxxRuntime, bc.cxxRuntimeShared,
                                                 bc.staticStdlib, format)
            == dist::Contract::SelfContained;
        // A refused library, and the statement that makes it shared when its
        // own package makes it so: an edge's `linkage = "static"` cannot change
        // a form the package constrains (`declaredShared`), so that remedy is
        // offered only where a request or the package's default decided.
        struct Refused { std::string name; std::string statedBy; };
        std::vector<Refused> withoutRuntime;
        const auto runtimeObjects = mcpp::build::package_link_objects(ctx.plan, providerName);
        for (auto& lu : ctx.plan.linkUnits) {
            if (lu.kind != mcpp::build::LinkUnit::SharedLibrary || !lu.dependencyOwned)
                continue;
            if (!mcpp::build::link_unit_holds_cxx(ctx.plan, lu)) continue;
            if (privateCopy) {
                for (auto const& o : runtimeObjects)
                    if (std::ranges::find(lu.objects, o) == lu.objects.end())
                        lu.objects.push_back(o);
                continue;
            }
            Refused r{ lu.targetName, {} };
            for (auto const& [i, form] : state.dependencyLinkForms) {
                if (!form.facts.declaredShared || i >= state.packages.size()) continue;
                for (auto const& t : state.packages[i].manifest.targets)
                    if (t.name == lu.targetName)
                        r.statedBy = form.facts.declaredSharedBy.empty()
                            ? std::string("its manifest declares a shared library target")
                            : form.facts.declaredSharedBy;
            }
            withoutRuntime.push_back(std::move(r));
        }
        if (!withoutRuntime.empty()) {
            std::string names, constrained;
            bool anyRequested = false;
            for (auto const& r : withoutRuntime) {
                names += (names.empty() ? "'" : ", '") + r.name + "'";
                if (r.statedBy.empty()) anyRequested = true;
                else constrained += std::format(
                    "       '{}' states its form itself ({}), so its edge cannot "
                    "link it static.\n", r.name, r.statedBy);
            }
            const std::string staticRemedy = anyRequested
                ? "       Link the dependency static, on its edge in [dependencies]:\n"
                  "\n"
                  "           <name> = { ..., linkage = \"static\" }\n"
                  "\n"
                  "       or give each shared library a private copy of the runtime:\n"
                : "       Give each shared library a private copy of the runtime:\n";
            refusal::record(refusal::Code::SharedLibraryCxxRuntime);
            return std::unexpected(std::format(
                "{} {} linked as a shared library, and this graph's C++ runtime is "
                "the package '{}@{}', whose objects are linked into the program.\n"
                "       A shared library built here would have no C++ runtime: the "
                "package compiles its runtime\n"
                "       with hidden visibility, so one image cannot use another "
                "image's copy.\n"
                "{}{}"
                "\n"
                "           [build]\n"
                "           cxx_runtime = {{ shared = \"self-contained\" }}\n"
                "\n"
                "       With a private copy, an exception of a standard library class "
                "thrown in the shared\n"
                "       library is not caught by that class in the program, because "
                "each copy has its own\n"
                "       type information.",
                names, withoutRuntime.size() == 1 ? "is" : "are",
                providerName, provider.package.version, constrained, staticRemedy));
        }
    }
    return {};
}

static std::expected<void, std::string> step13_cxx_process_runtime(PrepareState& state, BuildContext& ctx) {
    // ONE PROCESS, ONE C++ RUNTIME; ONE STATIC PACKAGE, ONE IMAGE (#646).
    //
    // Both are decided by `make_plan` and the contract table; this is where a
    // decision that cannot be delivered stops the build before it compiles.
    {
        namespace dist = mcpp::build::dist;
        auto const& bc = ctx.plan.manifest.buildConfig;
        const auto format = dist::format_for(
            state.tc->targetTriple,
            mcpp::platform::is_windows ? dist::Format::Pe
            : mcpp::platform::is_macos ? dist::Format::MachO
                                       : dist::Format::Elf);
        const dist::CxxSharedLoad load{
            .program = mcpp::build::image_loads_cxx_shared_library(
                ctx.plan, mcpp::build::LinkUnit::Binary),
            .tests   = mcpp::build::image_loads_cxx_shared_library(
                ctx.plan, mcpp::build::LinkUnit::TestBinary),
        };
        // The MSVC-ABI whole-project default (#718, §7.3) — read the same way
        // flags.cppm does, so the record this check reads and the flags a
        // build actually emits cannot disagree about which contract an
        // undeclared row resolved to.
        const std::optional<dist::Contract> msvcAbiDefault =
            mcpp::toolchain::is_msvc_target(*state.tc)
                ? std::optional(dist::msvc_abi_default_contract(
                      mcpp::toolchain::msvc_wants_static_crt(
                          bc.linkage, bc.cxxRuntime),
                      !state.tc->msvcRedistDir.empty()))
                : std::nullopt;
        const auto contracts = dist::role_contracts(
            dist::ContractStatement{
                .cxxRuntime       = bc.cxxRuntime,
                .cxxRuntimeTests  = bc.cxxRuntimeTests,
                .cxxRuntimeShared = bc.cxxRuntimeShared,
                .staticStdlib     = bc.staticStdlib,
                .msvcAbiDefault   = msvcAbiDefault,
            },
            format, load);
        // A ROW WITHOUT A REDISTRIBUTABLE DIRECTORY CANNOT DELIVER AN
        // EXPLICIT `toolchain-coupled`, AND SAYS SO BEFORE COMPILING.
        //
        // The undeclared case is silent (`msvc_abi_default_contract` already
        // resolved it to host-coupled above); an explicit statement that
        // cannot be met is an error, never a downgrade with a warning — the
        // same rule `mcpp pack`'s mode contradiction follows.
        if (mcpp::toolchain::is_msvc_target(*state.tc)
            && state.tc->msvcRedistDir.empty()) {
            struct { dist::Contract c; bool stated; std::string_view role; } rows[] = {
                {contracts.program, contracts.programStated, "distributable"},
                {contracts.tests,   contracts.testsStated,   "test"},
                {contracts.shared,  contracts.sharedStated,  "shared-library"},
            };
            for (auto const& r : rows) {
                if (r.c != dist::Contract::ToolchainCoupled || !r.stated) continue;
                refusal::record(refusal::Code::MsvcRedistUnavailable);
                return std::unexpected(std::format(
                    "cxx_runtime = \"toolchain-coupled\" cannot be delivered "
                    "for the {} target: this MSVC toolset carries no "
                    "VC\\Redist\\MSVC directory to stage vcruntime140.dll / "
                    "msvcp140.dll from.\n"
                    "       Use cxx_runtime = \"host-coupled\", or a toolset "
                    "that ships its redistributable.",
                    r.role));
            }
        }
        // A FILE OF THE C++ RUNTIME DECLARED WHERE THE CONTRACT SAYS THERE IS
        // NONE. Under host-coupled the system's runtime serves the program, and
        // the runtime placement resolver places no copy from any source; a
        // declared copy is the one statement it cannot honour, so the build
        // stops here, before compiling, with both statements named.
        if (mcpp::toolchain::is_msvc_target(*state.tc)) {
            const auto flags = mcpp::build::compute_flags(ctx.plan);
            if (!flags.runtimeErrors.empty()) {
                refusal::record(refusal::Code::CrtDeclaredUnderHostCoupled);
                std::string lines;
                for (auto const& e : flags.runtimeErrors)
                    lines += (lines.empty() ? "" : "\n       ") + e;
                return std::unexpected(lines);
            }
        }
        // F3a. A stated self-contained program over a coupled C++ shared
        // library of this build: the program would carry a static C++ runtime
        // and the library would load a shared one. The unstated case needs no
        // refusal, because `role_contracts` then gives the program the
        // library's contract.
        if (auto role = dist::runtime_split(contracts, format, load)) {
            std::string libraries;
            for (auto const& lu : ctx.plan.linkUnits) {
                if (lu.kind != mcpp::build::LinkUnit::SharedLibrary) continue;
                if (!mcpp::build::link_unit_holds_cxx(ctx.plan, lu)) continue;
                libraries += (libraries.empty() ? "'" : ", '") + lu.targetName + "'";
            }
            const bool tests = *role == dist::Role::Test;
            refusal::record(refusal::Code::ProgramCxxRuntimeSplit);
            return std::unexpected(std::format(
                "this build's {} state a self-contained C++ runtime and load the C++ "
                "shared library {}, which is linked against the {} C++ runtime.\n"
                "       The process would hold two C++ runtimes: the program exports the "
                "runtime symbols the\n"
                "       library references, the library binds some of them there and keeps "
                "the rest, and the two\n"
                "       halves disagree about shared state (measured: a string formatted in "
                "the library aborts\n"
                "       with std::bad_cast).\n"
                "       Remove the self-contained statement for {} (the `{}` value of "
                "[build] cxx_runtime), and\n"
                "       they take the shared library's contract, or give the shared library "
                "a private copy of the\n"
                "       runtime:\n"
                "\n"
                "           [build]\n"
                "           cxx_runtime = {{ shared = \"self-contained\" }}",
                tests ? "tests" : "programs",
                libraries.empty() ? std::string("'(unnamed)'") : libraries,
                dist::to_string(contracts.shared),
                tests ? "tests" : "programs", tests ? "tests" : "default"));
        }

        // MACH-O: EVERY IMAGE CARRIES ITS OWN HIDDEN libc++ (#646 F2).
        //
        // The Mach-O default is self-contained for every role, and each image
        // embeds the payload's `libc++.a` through `-load_hidden`, so the type
        // information of a standard library class exists once per image and libc++
        // compares it by address. Measured on macos-15 for this release: with the
        // default, a `std::runtime_error` thrown in a dylib is NOT caught by its
        // class in the program and two `std::error_code` categories compare
        // unequal; with `cxx_runtime = "host-coupled"` for every role, both hold.
        // The default is not changed here, because it is what every macOS build
        // ships today and changing it is its own record; a build that would meet
        // the split is told, once, what it is and how to avoid it.
        if (format == dist::Format::MachO && (load.program || load.tests)
            && contracts.shared == dist::Contract::SelfContained) {
            std::string libraries;
            for (auto const& lu : ctx.plan.linkUnits) {
                if (lu.kind != mcpp::build::LinkUnit::SharedLibrary) continue;
                if (!mcpp::build::link_unit_holds_cxx(ctx.plan, lu)) continue;
                libraries += (libraries.empty() ? "'" : ", '") + lu.targetName + "'";
            }
            mcpp::diag::degraded("build/cxx-runtime-identity",
                std::format("this build's program and the C++ shared library {} each "
                            "carry a private copy of the C++ runtime",
                            libraries.empty() ? std::string("'(unnamed)'") : libraries),
                "on Mach-O every image embeds the payload's libc++ with hidden "
                "visibility, so the type information of a standard library class exists "
                "once per image: measured on macOS, an exception of such a class thrown "
                "in the library is not caught by that class in the program, and two "
                "error categories compare unequal",
                "state one runtime for the process, for example [build] cxx_runtime = "
                "\"host-coupled\", when objects cross the boundary as exceptions or as "
                "libc++ values compared by identity");
        }

        // F1. A static package that several images reach. Refused where the
        // build cannot work (Mach-O and PE resolve every reference at link
        // time; Android's Java host loads an application's shared library
        // before anything that could supply the package), reported on other
        // ELF rows, where the library binds to the program's copy at run time
        // as it always has.
        if (!ctx.plan.staticPlacementConflicts.empty()) {
            const bool applicationRow = std::ranges::any_of(ctx.plan.linkUnits,
                [](auto const& lu) {
                    return lu.kind == mcpp::build::LinkUnit::SharedLibrary
                        && !lu.dependencyOwned && lu.entryMain.has_value();
                });
            const bool refuse = format == dist::Format::MachO
                             || format == dist::Format::Pe || applicationRow;
            std::string listing;
            for (auto const& c : ctx.plan.staticPlacementConflicts) {
                std::string reachers;
                if (c.program) reachers = "the program";
                for (auto const& image : c.images)
                    reachers += (reachers.empty() ? "'" : ", '") + image + "'";
                listing += std::format("       '{}' is reached by {}\n", c.package, reachers);
            }
            const std::string first = ctx.plan.staticPlacementConflicts.front().package;
            const std::string remedy = std::format(
                "       Link the package shared, so that every image loads one copy: on its "
                "edge in [dependencies],\n"
                "\n"
                "           {} = {{ ..., linkage = \"shared\" }}\n"
                "\n"
                "       or as the package's own default, in its manifest:\n"
                "\n"
                "           [targets.<name>]\n"
                "           linkage = \"shared\"", first);
            if (refuse) {
                refusal::record(refusal::Code::StaticPackageInTwoImages);
                return std::unexpected(std::format(
                    "a static package is linked into more than one image of this build, "
                    "and on this target\n"
                    "       an image cannot use another image's copy:\n{}{}",
                    listing, remedy));
            }
            mcpp::diag::degraded("build/static-placement",
                std::format("a static package is reachable from more than one image of "
                            "this build and is linked into the program only:\n{}",
                            listing),
                "the shared libraries bind to the program's copy at run time, which only "
                "an ELF process whose program links the package can do; the same graph "
                "is refused on Mach-O, PE and the Android application row",
                std::format("give the package the shared form, e.g. {} = {{ ..., linkage "
                            "= \"shared\" }}", first));
        }
    }
    return {};
}

static void step13_graph_and_schedule(PrepareState& state, BuildContext& ctx) {
    // The module graph outlives the plan for one consumer: `mcpp pack`, which
    // has to know which units are INTERFACE (published as source) and which
    // are implementation (published only as an object). The plan flattens that
    // away — a CompileUnit records what to compile, not what it provides — so
    // the packer would otherwise have to scan the tree a second time and could
    // then disagree with the build about what the package even contains.
    ctx.graph       = std::move(state.scan.graph);
    // mcpp#407. Both callers that produce a non-plain graph arrive here the
    // same way: dev-dependencies enabled, synthetic test targets appended. The
    // resulting `default` line names the test binaries and omits the package's
    // own target, and the output directory is shared with plain builds because
    // the fingerprint covers neither input. Stamping it on the plan is what
    // lets the graph say so about itself.
    ctx.plan.graphShape = (state.includeDevDeps || !state.extraTargets.empty()
                           || !state.memberTargets.empty())
        ? mcpp::build::GraphShape::WithTests
        : mcpp::build::GraphShape::Normal;
    // The device variant an override chose is stamped for the same reason: the
    // fast path runs without overrides, so a graph written under one must not
    // be the graph it replays.
    ctx.plan.accelOverridden = !state.overrides.accel.empty();

    // THE MACHINE'S JOB DEFAULT, resolved unconditionally and never fatally.
    //
    // `get_cfg` is lazy, so by this point the config may or may not have been
    // loaded -- a project with no dependencies can reach here without touching
    // it. Asking for it here rather than reading whatever `cfg_opt` happens to
    // hold is the point: otherwise the same project would honour
    // `[build] default_jobs` or ignore it depending on whether it has
    // dependencies, which is an answer that depends on an unrelated axis.
    //
    // A failure is discarded. This value is a concurrency hint, and a build
    // must not fail because the machine's preferred job count could not be
    // read; every other consumer of the config already reports its own
    // failures with a diagnostic that fits what it needed the config FOR.
    // `requireBootstrap=false` because nothing here needs the bootstrap
    // toolchain.
    int globalDefaultJobs = 0;
    if (auto c = state.get_cfg(/*requireBootstrap=*/false))
        globalDefaultJobs = static_cast<int>((*c)->defaultJobs);
    ctx.globalDefaultJobs = globalDefaultJobs;

    // Resolve the module-edge schedule ONCE, here, where both the toolchain and
    // the manifest are in hand. The backend writes the graph in this shape, the
    // graph records the tag, and `mcpp build --verbose` prints the reason — all
    // three read this, none of them re-derives it.
    {
        const auto decision = mcpp::build::schedule::decide(
            ctx.plan.toolchain,
            // Warned HERE and not at the fingerprint call above, which reads the
            // same switch a few hundred lines earlier: both get the normalised
            // value, only one of them says anything, so a typo produces exactly
            // one warning rather than two identical ones.
            mcpp::build::schedule::requested_switch(*state.m, [](std::string_view bad) {
                mcpp::ui::warning(std::format(
                    "ignoring invalid bmi_schedule '{}' (expected \"auto\", \"on\" or \"off\")", bad));
            }),
            mcpp::build::schedule::resolve_jobs(*state.m, [](std::string_view bad) {
                mcpp::ui::warning(std::format(
                    "ignoring invalid job count '{}' (expected a positive number or 'auto')", bad));
            }, globalDefaultJobs),
            // What this machine would pick if asked. Impure, so it is resolved
            // here and handed to the pure `decide`. Only DetachCodegen uses it,
            // and only when the user gave no job count — without it that
            // strategy ships `sched_cap = 0`, which disables the semaphore that
            // is its ONLY bound on how many compilers run at once.
            mcpp::platform::capacity::recommended_jobs(
                mcpp::platform::capacity::host_capacity()));
        ctx.plan.scheduleTag         = std::string(mcpp::build::schedule::to_string(decision.strategy));
        ctx.plan.scheduleNinjaJobs   = decision.ninjaJobs;
        ctx.plan.scheduleCompilerCap = decision.compilerCap;
        mcpp::log::verbose("build", std::format("schedule: {} — {}",
                                                ctx.plan.scheduleTag, decision.reason));

    }
    ctx.plan.runtimeBinding = state.runtimeBindingSnapshot;
    mcpp::build::merge_runtime_binding_contract(
        ctx.plan, state.runtimeBindingSnapshot);
    ctx.plan.compileDbPath = state.workRoot / "compile_commands.json";
    // GCC: a clean `*link:` for this build, so the payload's specs cannot
    // inject other homes' rpath entries into the artifact. AFTER the plan is
    // moved in — an earlier assignment was silently overwritten by that move,
    // which produced a generated file that nothing ever passed to the driver.
    // Generated here rather than in compute_flags, which runs twice per build.
    // A link input only: a plan that builds nothing (`plan_only`) neither reads
    // it nor runs the driver to produce it, and its compile arguments are the
    // same without it.
    if (state.tc->compiler == mcpp::toolchain::CompilerId::GCC && !state.overrides.plan_only)
        ctx.plan.gccCleanSpecs = mcpp::toolchain::write_clean_link_specs(
            state.tc->binaryPath, ctx.outputDir);
}

static std::expected<void, std::string> step13_build_graph_actions(PrepareState& state, BuildContext& ctx) {
    // ── Declared build-graph nodes → the plan ───────────────────────────────
    //
    // Collected here rather than inside make_plan because the engine-variable
    // vocabulary an action may reference includes values that only exist once
    // the plan does (outputDir is fingerprint-derived; a target's file name is
    // a link unit's output).
    //
    // The vocabulary is CLOSED on purpose. An action's command is an argv, not
    // a shell string, and the only interpolations are these four — which is
    // what makes an action portable (Windows has no shell to assume) and
    // cacheable (nothing can smuggle in ambient state).
    {
        // An engine variable that resolves to nothing must be an ERROR, not an
        // empty string: `${mcpp.target_file:tpyo}` would otherwise silently
        // become an edge with a blank path, and ninja reports that far away
        // from the typo that caused it.
        std::set<std::string> unresolvedTargets;
        std::set<std::string> unresolvedArtifacts;
        // `${mcpp.stage_dir}` used where there is no staged tree, and used by an
        // action whose role runs before the link. Both are refusals rather than
        // empty expansions: an empty path is a token the command still accepts,
        // and the tool then reads the build directory root -- which exists, so
        // the mistake produces a plausible artifact instead of a diagnostic.
        // Section 2 of the design record measured that shape: a valid, empty,
        // 52 KB installer with nothing said about it.
        std::set<std::string> stageDirNoPass, stageDirWrongRole;
        // Carried from `state.overrides` so the refusal below can say WHY there is
        // no tree, which is a different sentence from "you are not packaging".
        std::string stageDirWhy;
        // WHETHER *THIS* ACTION REFERENCED THE STAGED TREE, and deliberately a
        // flag rather than a set keyed on the action's id: an id is unique
        // within the package that declared it and nothing more, so two packages
        // may each submit a `dist` action called `package`. A set would then
        // hand one package's implicit dependency to the other's edge -- the
        // shape where a predicate is right and the object is wrong, which does
        // not fail, it answers about something else.
        //
        // The diagnostic sets below stay keyed by id because a diagnostic
        // NAMES ids and a collision there costs a duplicate line, not a wrong
        // edge.
        bool thisActionUsesStageDir = false;
        // Where the declaring package's binaries land: `bin/`, or in a
        // workspace plan the product directory of the member that declared
        // the action (§15 of the 2026-09-29 workspace design). Set per
        // package by `collect`.
        std::filesystem::path binDir = ctx.plan.outputDir / "bin";
        const bool stagePass = !state.overrides.pack_stage_dir.empty();
        auto substitute = [&](std::string s, const char* actionId,
                              mcpp::manifest::BuildAction::Role role) {
            auto rep = [&](std::string_view what, const std::string& with) {
                for (std::size_t p; (p = s.find(what)) != std::string::npos; )
                    s.replace(p, what.size(), with);
            };
            rep("${mcpp.out_dir}",    ctx.plan.outputDir.string());
            rep("${mcpp.bin_dir}",    binDir.string());
            rep("${mcpp.compile_db}", ctx.plan.compileDbPath.string());
            // The engine's own executable, absolute (2026.9.13.1+). An action
            // whose command is an argv with no shell has no portable way to
            // copy, touch or compare a file, and the engine is the one
            // program present wherever a build runs -- the reason a `check`
            // is wrapped with `mcpp __action-stamp` (ninja_backend.cppm). This
            // token lets a build program say the same thing: `${mcpp.self}
            // stage --verify content --output <dst> <src>` is the copy every
            // `stage_file` edge already performs. The same caveat as the
            // wrapper's: a version change regenerates build.ninja, and a
            // binary moved under an unchanged version leaves a stale path,
            // exactly as it would for the compiler.
            rep("${mcpp.self}",       mcpp::platform::fs::self_exe_path().string());
            // ABSOLUTE, unlike `${mcpp.target_file:}` and for the same reason
            // stated the other way round: the staged tree lives outside the
            // build directory and no ninja edge produces it, so there is no
            // edge-declared spelling to agree with. `${mcpp.out_dir}` above is
            // absolute on the same grounds.
            if (s.find("${mcpp.stage_dir}") != std::string::npos) {
                if (!stagePass) {
                    stageDirNoPass.insert(actionId);
                    stageDirWhy = state.overrides.pack_stage_reason;
                } else if (role != mcpp::manifest::BuildAction::Role::Artifact) {
                    stageDirWrongRole.insert(actionId);
                } else {
                    thisActionUsesStageDir = true;
                }
                rep("${mcpp.stage_dir}", state.overrides.pack_stage_dir.string());
            }
            constexpr std::string_view kTf = "${mcpp.target_file:";
            for (std::size_t p; (p = s.find(kTf)) != std::string::npos; ) {
                auto close = s.find('}', p);
                if (close == std::string::npos) break;
                auto name = s.substr(p + kTf.size(), close - p - kTf.size());
                // The link unit's BUILD-DIR-RELATIVE output, not an absolute
                // path. ninja identifies a file by the string an edge declares,
                // and the link edge declares `bin/app`; an absolute reference
                // to the same bytes is a DIFFERENT node, which ninja reports as
                // "missing and no known rule to make it". Commands run with
                // cwd = the build dir, so the relative form is also what the
                // tool being invoked should receive.
                std::string resolved;
                for (auto const& lu : ctx.plan.linkUnits)
                    if (lu.targetName == name)
                        resolved = lu.output.generic_string();
                if (resolved.empty()) unresolvedTargets.insert(name);
                s.replace(p, close - p + 1, resolved);
            }
            // `${mcpp.artifact:<package>/<target>}` (mcpp#711): a dependency's
            // program that an edge requested with `artifacts = [...]`, spelled
            // like `${mcpp.target_file:}` -- the link unit's build-dir-relative
            // output -- for the same reason. `<package>` is the dependency's
            // name with or without its namespace.
            constexpr std::string_view kArt = "${mcpp.artifact:";
            for (std::size_t p; (p = s.find(kArt)) != std::string::npos; ) {
                auto close = s.find('}', p);
                if (close == std::string::npos) break;
                const auto ref = s.substr(p + kArt.size(), close - p - kArt.size());
                const auto slash = ref.rfind('/');
                std::string resolved;
                if (slash != std::string::npos) {
                    const auto pkgName = ref.substr(0, slash);
                    const auto target  = ref.substr(slash + 1);
                    for (auto const& lu : ctx.plan.linkUnits) {
                        if (lu.artifactOf.empty() || lu.targetName != target) continue;
                        const auto dot = lu.artifactOf.rfind('.');
                        const auto shortName = dot == std::string::npos
                            ? lu.artifactOf : lu.artifactOf.substr(dot + 1);
                        if (lu.artifactOf == pkgName || shortName == pkgName)
                            resolved = lu.output.generic_string();
                    }
                }
                if (resolved.empty()) unresolvedArtifacts.insert(ref);
                s.replace(p, close - p + 1, resolved);
            }
            return s;
        };
        auto collect = [&](const mcpp::manifest::Manifest& mm) {
            // The declaring package, recorded here because this is the only
            // place that knows it: the build program emitted the action, and a
            // program has no idea which package the engine loaded it for.
            // mcpp#534's ordering edge is scoped to this name.
            auto owner = mcpp::build::qualified_package_name(mm);
            binDir = ctx.plan.outputDir / "bin";
            for (auto const& g : ctx.plan.linkGroups)
                if (!g.linkOnly && g.member == owner) binDir = ctx.plan.outputDir / g.productDir;
            for (auto a : mm.buildConfig.actions) {
                thisActionUsesStageDir = false;
                const auto sub = [&](std::string v) {
                    return substitute(std::move(v), a.id.c_str(), a.role);
                };
                for (auto& x : a.inputs)  x = sub(x);
                for (auto& x : a.outputs) x = sub(x);
                for (auto& x : a.command) x = sub(x);
                // Same closed vocabulary as outputs — a depfile commonly
                // wants to live at `${mcpp.out_dir}/<name>.d`, beside the
                // output it describes, and `prepare_actions` above
                // deliberately left a `${mcpp.` depfile untouched for
                // exactly this phase to resolve.
                if (!a.depfile.empty()) a.depfile = sub(a.depfile);
                // The same vocabulary for the command's environment and
                // directory (mcpp#708): `OUT=${mcpp.out_dir}/gen` is the value
                // an environment-configured generator most often wants.
                for (auto& x : a.env) x = sub(x);
                if (!a.cwd.empty()) a.cwd = sub(a.cwd);
                // THE DEPENDENCY IS IMPLIED BY THE USE, so a member author
                // cannot forget it. Without this the edge is dirty only when a
                // link output changes, and a staged set that grew a dependency's
                // shared library while the program's own bytes did not would
                // leave the previous distributable in place, reported as
                // up to date.
                if (thisActionUsesStageDir) {
                    a.consumesStageDir = true;
                    a.inputs.push_back(
                        mcpp::pack::stage_manifest_path(state.overrides.pack_stage_dir).string());
                }
                a.packageName = owner;
                ctx.plan.actions.push_back(std::move(a));
            }
            // Every package's declaration, on every pass. Sorted and de-duplicated
            // below so the refusal's list reads the same whatever order resolution
            // walked the graph in.
            for (auto const& f : mm.buildConfig.packFormats)
                ctx.plan.providedPackFormats.push_back(f);
        };
        collect(*state.m);
        for (std::size_t i = 1; i < state.packages.size(); ++i)
            collect(state.packages[i].manifest);
        std::ranges::sort(ctx.plan.providedPackFormats);
        ctx.plan.providedPackFormats.erase(
            std::ranges::unique(ctx.plan.providedPackFormats).begin(),
            ctx.plan.providedPackFormats.end());
        ctx.plan.packFormat = state.overrides.pack_format;
        if (!stageDirNoPass.empty()) {
            std::string ids;
            for (auto const& n : stageDirNoPass) ids += (ids.empty() ? "" : ", ") + n;
            if (!stageDirWhy.empty()) {
                return std::unexpected(std::format(
                    "build.mcpp action(s) [{}] reference ${{mcpp.stage_dir}}, and no "
                    "tree could be staged for this target.\n"
                    "  {}\n"
                    "  The format was requested and the provider was reached; what is "
                    "missing is the staged\n"
                    "  closure itself. A member that names a built file with "
                    "${{mcpp.target_file:<name>}} instead\n"
                    "  of reading the tree is unaffected on this target.",
                    ids, stageDirWhy));
            }
            return std::unexpected(std::format(
                "build.mcpp action(s) [{}] reference ${{mcpp.stage_dir}}, and this "
                "build is not packaging.\n"
                "  The staged tree is produced by `mcpp pack` after the link, so "
                "it does not exist during\n"
                "  a plain build and there is nothing for the placeholder to name.\n"
                "  Gate the submission on the format you provide:\n"
                "      mcpp::provides_pack_format(\"<name>\");            // always\n"
                "      if (std::string_view(mcpp::pack_format()) == \"<name>\")  "
                "// then submit\n"
                "  and reach the tree with `mcpp pack --format <name>`.", ids));
        }
        if (!stageDirWrongRole.empty()) {
            std::string ids;
            for (auto const& n : stageDirWrongRole) ids += (ids.empty() ? "" : ", ") + n;
            return std::unexpected(std::format(
                "build.mcpp action(s) [{}] reference ${{mcpp.stage_dir}} with a role "
                "other than \"artifact\".\n"
                "  Only an artifact action runs after the link, and the staged tree "
                "is a link output's\n"
                "  successor: a source, object or check action is scheduled before "
                "there is anything to stage.\n"
                "  use: role = \"artifact\"", ids));
        }
        if (!unresolvedTargets.empty()) {
            std::string bad, known;
            for (auto const& n : unresolvedTargets) bad += (bad.empty() ? "" : ", ") + n;
            for (auto const& lu : ctx.plan.linkUnits)
                known += (known.empty() ? "" : ", ") + lu.targetName;
            return std::unexpected(std::format(
                "build.mcpp action references unknown target(s) via "
                "${{mcpp.target_file:...}}: {}\n"
                "  targets in this build: [{}]\n"
                "  (a target gated by required_features is absent unless those "
                "features are active)",
                bad, known.empty() ? std::string("none") : known));
        }

        if (!unresolvedArtifacts.empty()) {
            std::string bad, known;
            for (auto const& n : unresolvedArtifacts) bad += (bad.empty() ? "" : ", ") + n;
            for (auto const& lu : ctx.plan.linkUnits)
                if (!lu.artifactOf.empty())
                    known += (known.empty() ? "" : ", ") + lu.artifactOf + "/" + lu.targetName;
            return std::unexpected(std::format(
                "build.mcpp action references unknown artifact(s) via "
                "${{mcpp.artifact:<package>/<target>}}: {}\n"
                "  artifacts in this build: [{}]\n"
                "  (an artifact exists when a dependency edge requests it with "
                "`artifacts = [\"<target>\"]`)",
                bad, known.empty() ? std::string("none") : known));
        }

        // role = "object": the outputs are LINK inputs, so attach them to the
        // link units that should receive them.
        //
        // The strings are pushed VERBATIM. ninja identifies a file by the string
        // an edge declares, and the action edge declares whatever
        // prepare_actions produced (an absolute path); handing the link edge a
        // prettier relative spelling of the same bytes creates a second node and
        // "missing and no known rule to make it" — the same trap
        // ${mcpp.target_file:} documents just above.
        std::set<std::string> unknownObjectTargets;
        for (auto const& a : ctx.plan.actions) {
            if (a.role != mcpp::manifest::BuildAction::Role::Object) continue;

            // Validate EVERY named target, not just the case where none of them
            // matched. Gating the check on "nothing attached" meant
            // `.target("app").target("aap")` attached to `app` and dropped the
            // typo without a word — while both the type comment and the docs
            // promise an unknown name is an error. A per-name check is also the
            // only one that scales: the failure it catches is a target that
            // exists in one configuration and not another.
            for (auto const& t : a.targets) {
                bool known = false;
                for (auto const& lu : ctx.plan.linkUnits)
                    if (lu.targetName == t) { known = true; break; }
                if (!known) unknownObjectTargets.insert(t);
            }

            bool attached = false;
            for (auto& lu : ctx.plan.linkUnits) {
                // Empty targets = every LINKED IMAGE, and a test binary is one.
                // Excluding it made `mcpp build` succeed while `mcpp test` died
                // with `undefined symbol` on the very symbol the action exists
                // to provide — the library code under test links the same
                // objects, so a blob/`.def`/pre-built `.o` has to reach it too.
                // Naming the test target instead is not a workaround: test link
                // units are DISCOVERED from tests/*.cpp, so their names are not
                // in mcpp.toml and a build.mcpp that spells one stops building
                // under plain `mcpp build`, where that unit does not exist.
                // (`[resources]` makes the opposite call on purpose: an icon
                // belongs to what ships, not to a test runner.)
                //
                // A STATIC LIBRARY IS ONE OF THEM, and leaving it out was
                // the whole of what C-6 needed. A package whose device code is
                // its point -- ggml's CUDA backend is 305 `.cu` files behind a
                // `kind = "lib"` target -- emitted its actions, watched every
                // one of them be dropped with a warning, and produced an
                // archive with no device code in it. The archive rule already
                // consumes `lu.objects`, so the objects an action produced
                // belong there for exactly the reason a compiled `.cpp`'s do:
                // the target's content is what it was told to contain.
                //
                // AND NOT A DEPENDENCY'S IMAGE. "Every linked image" means
                // every image THIS PACKAGE produces; a `kind = "shared"`
                // dependency contributes a link unit to this plan and is not
                // one of them. Without the qualifier the SYCL example's device
                // island was linked into `compat:opencl`'s ICD loader as well
                // -- a C library carrying `saxpy_device` -- and the process
                // held two copies of it. An action that means to reach a
                // dependency's target cannot: it is not this package's to
                // fill, and naming it explicitly already fails as unknown.
                const bool image = !lu.dependencyOwned
                               && (lu.kind == mcpp::build::LinkUnit::Binary
                                || lu.kind == mcpp::build::LinkUnit::SharedLibrary
                                || lu.kind == mcpp::build::LinkUnit::StaticLibrary
                                || lu.kind == mcpp::build::LinkUnit::TestBinary);
                const bool wanted = a.targets.empty()
                    ? image
                    : std::find(a.targets.begin(), a.targets.end(),
                                lu.targetName) != a.targets.end();
                if (!wanted) continue;
                for (auto const& o : a.outputs) lu.objects.emplace_back(o);
                attached = true;
            }

            // No consumer at all. The edge is excluded from `actionDefaults`
            // (its outputs are supposed to be reachable through a link edge), so
            // this is not "builds but unused" — the command never runs and the
            // build says nothing. Same shape, and same diagnostic, as
            // `resources/no-image`.
            if (!attached && a.targets.empty()) {
                mcpp::diag::degraded("action/no-target", std::format(
                    "build.mcpp action '{}' has role = \"object\" but this build "
                    "produces no target to put its outputs into",
                    a.id.empty() ? "<unnamed>" : a.id),
                    "the action never runs and its outputs are never produced",
                    "add a [targets.<name>] — a bin, a lib, a shared lib or a "
                    "test all take one — or name the targets explicitly with "
                    ".target(\"…\")");
            }
        }
        if (!unknownObjectTargets.empty()) {
            std::string bad, known;
            for (auto const& n : unknownObjectTargets) bad += (bad.empty() ? "" : ", ") + n;
            for (auto const& lu : ctx.plan.linkUnits)
                known += (known.empty() ? "" : ", ") + lu.targetName;
            return std::unexpected(std::format(
                "build.mcpp action with role = \"object\" names unknown "
                "target(s): {}\n"
                "  targets in this build: [{}]\n"
                "  (a target gated by required_features is absent unless those "
                "features are active; test binaries exist only under `mcpp "
                "test`, so name none and the outputs reach every target "
                "including them)",
                bad, known.empty() ? std::string("none") : known));
        }
    }
    ctx.plan.stdCompatBmiPath = state.stdCompatBmiPath;
    ctx.plan.stdCompatObjectPath = state.stdCompatObjectPath;
    return {};
}

static std::expected<void, std::string> step13_assembly_units(PrepareState& state, BuildContext& ctx) {
    // Clang: discover clang-scan-deps for P1689 dyndep scanning.
    if (mcpp::toolchain::is_clang(*state.tc)) {
        if (auto sd = mcpp::toolchain::clang::find_scan_deps(*state.tc)) {
            ctx.plan.scanDepsPath = *sd;
        }
    }

    // ─── Assembly units: validate + resolve the assembler ─────────────
    // .S/.s ride the C driver (GAS) — the MSVC dialect has no such path.
    // .asm is NASM: x86-family only, and the binary is resolved LAZILY —
    // only when the plan actually contains .asm units — as a hard failure,
    // never a silent skip (a dropped .o surfaces as undefined references
    // much later; fail here with the real cause instead).
    {
        bool hasGas = false, hasNasm = false;
        for (auto& cu : ctx.plan.compileUnits) {
            if (cu.kind == mcpp::SourceKind::GasAsm)       hasGas = true;
            else if (cu.kind == mcpp::SourceKind::NasmAsm) hasNasm = true;
        }
        if (hasGas && mcpp::toolchain::dialect_for(*state.tc).id == "msvc") {
            return std::unexpected(std::string(
                "GAS assembly sources (.S/.s) are not supported by the MSVC "
                "toolchain; use NASM syntax (.asm) or a MinGW/LLVM toolchain, "
                "or `!`-exclude them in [build].sources"));
        }
        if (hasNasm) {
            auto trip = mcpp::toolchain::triple::parse(state.tc->targetTriple)
                            .value_or(mcpp::toolchain::triple::host_triple());
            auto fmt = trip.nasm_format();
            if (!fmt) {
                return std::unexpected(std::format(
                    "NASM sources (.asm) are x86-only, but the target is {}; "
                    "gate them off non-x86 targets (a feature, or a "
                    "`!`-exclude glob in [build].sources)", trip.str()));
            }
            ctx.plan.nasmFormat = *fmt;

            // #232: nasm used to go through a bespoke `ensure_nasm` path
            // whose `if (cfgNasm)` guard silently swallowed a `get_cfg()`
            // bootstrap failure (misreporting it as "no nasm"), and whose
            // install fallback never refreshed the package index and
            // downgraded a failed install to a warning. Surface the real
            // config error, then provision through the SAME synchronous
            // gate the compiler toolchain uses (index refresh before
            // install, blocking install, hard error on failure) — see the
            // toolchain resolution block above (~line 872-899).
            auto cfgNasm = state.get_cfg(true);
            if (!cfgNasm) return std::unexpected(cfgNasm.error());

            std::optional<std::filesystem::path> nasmBin =
                mcpp::xlings::find_usable_nasm(mcpp::config::make_xlings_env(**cfgNasm));
            if (!nasmBin) {
                mcpp::fetcher::Fetcher nasmFetcher(**cfgNasm);
                mcpp::fetcher::InstallProgressHandler nasmProgress;
                auto nasmTarget = std::format("xim:nasm@{}",
                    mcpp::xlings::pinned::kNasmVersion);
                auto payload = nasmFetcher.resolve_xpkg_path(
                    nasmTarget, /*autoInstall=*/true, &nasmProgress);
                if (!payload) {
                    return std::unexpected(std::format(
                        "NASM sources (.asm) present but nasm provisioning "
                        "failed: {}", payload.error().message));
                }
                nasmBin = mcpp::xlings::find_sandbox_nasm(
                    mcpp::config::make_xlings_env(**cfgNasm));
            }
            if (!nasmBin) {
                return std::unexpected(std::string(
                    "NASM sources (.asm) present but no usable nasm (>= 2.16) "
                    "was found or installable; install one via `xlings install "
                    "nasm` or your system package manager"));
            }
            // A HOST TOOL THAT REACHES A BUILD IS NAMED THERE. The sandbox
            // copy is tried first (mcpp.xlings::find_usable_nasm), so this
            // fires only where that route could not serve: an offline machine
            // that already has an assembler. Saying nothing would leave two
            // machines assembling the same source with different tools and
            // no line in either build recording which.
            if (mcpp::xlings::nasm_is_from_host(
                    mcpp::config::make_xlings_env(**cfgNasm), *nasmBin)) {
                mcpp::diag::degraded("build/nasm-from-host", std::format(
                    "the assembler for this build is the host's ('{}'), not "
                    "the one this engine pins", nasmBin->string()),
                    "two machines can assemble the same source with different "
                    "assemblers, and the build records only this line",
                    "run `xlings install nasm` so the pinned copy is used");
            }
            ctx.plan.nasmPath = *nasmBin;
        }
    }
    return {};
}

// The resource compiler of the plan's resource units, and each unit's flags.
static std::expected<void, std::string>
step13_resource_compiler(PrepareState& state, BuildContext& ctx) {
    if (ctx.plan.resourceUnits.empty()) return {};
    namespace rsrc = mcpp::build::resources;
    const auto trip = mcpp::toolchain::triple::parse(state.tc->targetTriple)
                          .value_or(mcpp::toolchain::triple::host_triple());
    const auto  dialectId = mcpp::toolchain::dialect_for(*state.tc).id;
    const bool msvcStyle = (dialectId == "msvc");

    // Lazy + hard failure, exactly like nasm: a dropped resource
    // surfaces as "where did my icon go", which is unattributable.
    auto tool = rsrc::find_rc_tool(*state.tc, dialectId);
    if (!tool) {
        return std::unexpected(std::format(
            "[resources] needs a Windows resource compiler for the "
            "{} toolchain targeting {}, and none was found next to "
            "{}.\n  Expected {} in the toolchain's own bin directory "
            "(mcpp does not search PATH for build tools).",
            dialectId, trip.str(), state.tc->binaryPath.string(),
            msvcStyle ? "rc.exe or llvm-rc"
                      : "<triple>-windres, windres or llvm-windres"));
    }
    ctx.plan.rcPath  = tool->path;
    ctx.plan.rcStyle = tool->style;

    // UTF-8 input, always. `[package]` metadata is user text and
    // routinely non-ASCII; without this llvm-rc refuses the script
    // outright ("Non-ASCII 8-bit codepoint can't be interpreted in
    // the current codepage") rather than mangling it, so a project
    // with a Chinese description could not build at all.
    //
    // Include search: the script's package first, then whatever the
    // toolchain puts on INCLUDE. llvm-rc preprocesses but does NOT read
    // INCLUDE (rc.exe does), so the SDK dirs have to be spelled out for it --
    // that is what makes `#include <windows.h>` work, and it is the
    // supported way to get VS_VERSION_INFO defined. Each unit carries the
    // flags of its package; the plan's flags are the first unit's, so a
    // plan with one package states them once.
    const std::string ip = msvcStyle ? "/I" : "-I";
    std::vector<std::string> systemIncludes;
    if (msvcStyle && tool->name().find("llvm-rc") != std::string::npos) {
        for (auto const& ev : state.tc->envOverrides) {
            if (ev.key != "INCLUDE") continue;
            // Shared splitter: `;` only. See rsrc::split_env_list --
            // the drive colon is not a separator.
            for (auto dir : rsrc::split_env_list(ev.value))
                systemIncludes.push_back(ip + std::string(dir));
        }
    }
    for (auto& ru : ctx.plan.resourceUnits) {
        ru.flags.push_back(msvcStyle ? "/C" : "--codepage=65001");
        if (msvcStyle) ru.flags.push_back("65001");
        for (auto const& d : ru.includeDirs) ru.flags.push_back(ip + d.string());
        ru.flags.insert(ru.flags.end(), systemIncludes.begin(), systemIncludes.end());
    }
    ctx.plan.rcFlags = ctx.plan.resourceUnits.front().flags;
    return {};
}

static std::expected<void, std::string> step13_windows_resources(PrepareState& state, BuildContext& ctx) {
    // ─── Windows resources: [resources] → a tracked link input (mcpp#365) ──
    //
    // Four rules, in this order:
    //   1. Only the [resources] of the package being built is read: the root,
    //      or in a workspace plan each selected member, whose resources reach
    //      that member's images only. A dependency's version resource would
    //      fight its consumer's for ordinal 1, and a dependency that produces
    //      no PE image of its own has nothing to embed into.
    //   2. A DECLARED FILE THAT DOES NOT EXIST IS AN ERROR — on EVERY target.
    //      Whether a path exists is a fact about the working tree, not about
    //      the target; gating it on is_pe() meant a Linux or macOS CI could not
    //      see a typo in `icon = …` at all and only the Windows job went red,
    //      which is the same "find out late" failure the hard error exists to
    //      remove. Existence is checked everywhere; only COMPILATION is PE-only.
    //   3. On a non-PE target nothing is compiled — no units, no warning,
    //      byte-identical build. This is what makes `cfg(windows)` unnecessary
    //      (and it could not be used anyway: the conditional channel carries
    //      BuildInputs only).
    //   4. Nothing to embed into (an archive-only package) → say so and stop.
    //
    // The same pipeline carries the application manifest of `windows_code_page`
    // (#693). A PE executable embeds one that makes its process ANSI code page
    // UTF-8 when its target says `windows_code_page = "utf-8"`, or, with nothing
    // said, when it is built as a host tool (D6): such a tool receives mcpp's
    // UTF-8 paths on its command line. `legacy` opts out, and an ordinary target
    // that says nothing embeds nothing (M6: the program's encoding is its own).
    //
    // The host-tool default yields to a manifest the package embeds itself
    // through `[resources] files`: both would sit at ordinal 1, the package
    // said nothing about code pages, and its own manifest is the one it ships.
    // A DECLARED `utf-8` beside such a manifest is refused below instead.
    //
    // A SUBJECT is one package whose resources are planned: its manifest, the
    // directory its paths were written in, and the images it owns. Outside a
    // workspace plan the root is the only subject and owns every image that
    // is not a dependency's program, which is the historical rule.
    struct Subject {
        const mcpp::manifest::Manifest* m;
        std::filesystem::path           dir;
        std::string                     owner;   // empty: the root
    };
    std::vector<Subject> subjects;
    if (!state.workspacePlan()) {
        subjects.push_back({&*state.m, *state.root, {}});
    } else {
        for (auto const& pkg : state.packages)
            if (pkg.selectedMember)
                subjects.push_back({&pkg.manifest, pkg.root,
                                    mcpp::build::qualified_package_name(pkg.manifest)});
    }
    const bool hostToolBuild = state.overrides.tool_depth > 0;
    const auto trip = mcpp::toolchain::triple::parse(state.tc->targetTriple)
                          .value_or(mcpp::toolchain::triple::host_triple());
    const auto  dialectId = mcpp::toolchain::dialect_for(*state.tc).id;
    const bool msvcStyle = (dialectId == "msvc");

    for (auto const& S : subjects) {
        const auto& M = *S.m;
        const bool ownManifest = hostToolBuild
            && std::ranges::any_of(M.resources.files, [&](const auto& f) {
                   const auto abs = (f.is_absolute() ? f : (S.dir / f)).lexically_normal();
                   return mcpp::build::resources::scan_rc(abs).declaresManifest;
               });
        auto codePageOf = [&](const mcpp::manifest::Target& t) -> std::string_view {
            if (!t.windowsCodePage.empty()) return t.windowsCodePage;
            return (hostToolBuild && t.is_program() && !ownManifest) ? "utf-8" : "legacy";
        };
        const bool anyUtf8Image = std::ranges::any_of(M.targets, [&](const auto& t) {
            return t.is_program() && codePageOf(t) == "utf-8";
        });
        if (M.resources.declared() || anyUtf8Image) {
            namespace rsrc = mcpp::build::resources;
            const auto& R = M.resources;

            // Rule 2 — target-independent, so it runs before the is_pe() gate.
            auto resolve_declared = [&](const std::filesystem::path& p,
                                        std::string_view key)
                -> std::expected<std::filesystem::path, std::string>
            {
                // Lexical, not weakly_canonical: canonicalising resolves symlinks,
                // and a symlinked source tree would then bake a different path into
                // the generated script than the one the user wrote. (Same reason
                // mcpp#344 made the cache anchor lexical.)
                auto abs = (p.is_absolute() ? p : (S.dir / p)).lexically_normal();
                std::error_code ec;
                if (!std::filesystem::is_regular_file(abs, ec))
                    return std::unexpected(std::format(
                        "[resources] {} = \"{}\" does not exist (looked at {}).\n"
                        "  A declared resource is a build input like any other "
                        "source: mcpp will not quietly ship a binary without it. "
                        "Remove the key if the resource is not wanted.",
                        key, p.generic_string(), abs.generic_string()));
                return abs;
            };

            std::filesystem::path iconAbs;
            if (!R.icon.empty()) {
                auto r = resolve_declared(R.icon, "icon");
                if (!r) return std::unexpected(r.error());
                iconAbs = *r;
            }
            std::vector<std::filesystem::path> extraInputs;
            for (auto const& e : R.extraInputs) {
                auto r = resolve_declared(e, "extra-inputs");
                if (!r) return std::unexpected(r.error());
                extraInputs.push_back(*r);
            }
            std::vector<std::filesystem::path> scriptFiles;
            for (auto const& f : R.files) {
                auto r = resolve_declared(f, "files");
                if (!r) return std::unexpected(r.error());
                scriptFiles.push_back(*r);
            }

            // Rules 3 and 4 are early returns rather than nesting: the body below is
            // ~150 lines and an `else` around all of it reads as an accident.
            auto plan_resources = [&]() -> std::expected<void, std::string> {
                const std::string_view outExt = msvcStyle ? ".res" : ".o";
                // A member's resources are compiled in a directory of its own, so
                // two members' scripts and synthesised scripts never share a name.
                const auto resRel = S.owner.empty() ? std::filesystem::path("res")
                                                    : std::filesystem::path("res") / S.owner;
                const auto resDir = ctx.plan.outputDir / resRel;
                // Where the resource compiler looks for a script's includes and
                // files: the package's directory, then its include_dirs. The
                // scan resolves them the same way.
                std::vector<std::filesystem::path> rcIncludes{S.dir};
                for (auto const& d : M.buildConfig.includeDirs)
                    rcIncludes.push_back(d.is_absolute() ? d : (S.dir / d));
                std::error_code mkEc;
                std::filesystem::create_directories(resDir, mkEc);

                // Which link units embed resources: images, not archives. A `.res`
                // inside a static library is dropped by every linker that reads one.
                // Test binaries are images too, but deliberately excluded: an icon
                // and an OriginalFilename belong to what the project SHIPS, and a
                // test executable is not that. (`role = "object"` makes the opposite
                // call, for the opposite reason — see its note above.)
                // In a workspace plan a member's images are its link units and
                // its shared libraries, which are linked with the graph's.
                auto owns = [&](const mcpp::build::LinkUnit& lu) {
                    if (S.owner.empty()) return true;
                    if (lu.memberOf == S.owner) return true;
                    return lu.kind == mcpp::build::LinkUnit::SharedLibrary
                        && std::ranges::any_of(M.targets, [&](const auto& t) {
                               return t.kind == mcpp::manifest::Target::SharedLibrary
                                   && t.name == lu.targetName;
                           });
                };
                std::vector<std::size_t> peUnits;
                for (std::size_t i = 0; i < ctx.plan.linkUnits.size(); ++i) {
                    auto k = ctx.plan.linkUnits[i].kind;
                    // A dependency's program (mcpp#711) carries its own package's
                    // identity, not this one's.
                    if (!ctx.plan.linkUnits[i].artifactOf.empty()) continue;
                    if (!owns(ctx.plan.linkUnits[i])) continue;
                    if (k == mcpp::build::LinkUnit::Binary ||
                        k == mcpp::build::LinkUnit::SharedLibrary)
                        peUnits.push_back(i);
                }
                // Nothing to embed into. Compiling the scripts anyway would leave
                // orphan edges nothing depends on, and demanding a resource
                // compiler for them would fail a build that has no use for one.
                // A degradation, not a warning: the user asked for something and
                // got nothing, so `--strict` should see it.
                if (peUnits.empty()) {
                    mcpp::diag::degraded("resources/no-image", std::format(
                        "[resources] is declared but '{}' produces no executable or "
                        "shared library for {}", M.package.name, trip.str()),
                        "nothing embeds the icon or the version metadata",
                        "add a [targets.<name>] with kind = \"bin\" or \"shared\", "
                        "or drop the [resources] section");
                    return {};
                }

                // Two scripts with the same stem in different directories would
                // otherwise write the same artifact — a silent "multiple rules
                // generate" that ninja reports far from the cause.
                std::set<std::string> usedStems;
                auto add_unit = [&](const std::filesystem::path& src,
                                    std::string_view stem,
                                    std::vector<std::filesystem::path> inputs,
                                    std::size_t attachTo)
                    -> std::expected<void, std::string>
                {
                    if (!usedStems.insert(std::string(stem)).second)
                        return std::unexpected(std::format(
                            "[resources] two resource scripts are named '{}.rc'; "
                            "they would produce the same artifact. Rename one.", stem));
                    mcpp::build::ResourceUnit ru;
                    ru.package = mcpp::build::qualified_package_name(M);
                    ru.source = src;
                    ru.output = resRel / (std::string(stem) + std::string(outExt));
                    ru.includeDirs = rcIncludes;
                    ru.implicitInputs = std::move(inputs);
                    ctx.plan.resourceUnits.push_back(std::move(ru));
                    const auto& out = ctx.plan.resourceUnits.back().output;
                    if (attachTo == static_cast<std::size_t>(-1)) {
                        for (auto i : peUnits) ctx.plan.linkUnits[i].objects.push_back(out);
                    } else {
                        ctx.plan.linkUnits[attachTo].objects.push_back(out);
                    }
                    return {};
                };

                // Author-written scripts: compiled once, linked into every image.
                for (auto const& rcSrc : scriptFiles) {
                    auto scan = rsrc::scan_rc(rcSrc, rcIncludes);
                    if (scan.versionInfoNamedByString) {
                        // The mcpp#365 silent failure, caught on the way in. A
                        // degradation rather than a warning: the impact is exactly
                        // the thing this feature exists to remove — a shipped binary
                        // whose version metadata Windows cannot read — so a build
                        // that asked for `--strict` must not pass over it.
                        mcpp::diag::degraded("resources/versioninfo", std::format(
                            "{}: `{} VERSIONINFO` names the version resource '{}' "
                            "instead of ordinal 1",
                            rcSrc.filename().generic_string(), scan.versionInfoName,
                            scan.versionInfoName),
                            "Windows will not find it — GetFileVersionInfo looks up "
                            "MAKEINTRESOURCE(1) and every field comes back empty, "
                            "while every tool that prints the resource TYPE still "
                            "says it is fine",
                            "VS_VERSION_INFO is a macro from <windows.h>; add "
                            "`#include <windows.h>` to the script, or write "
                            "`1 VERSIONINFO`");
                    }
                    for (auto const& g : scan.gaps) {
                        mcpp::diag::degraded("resources/inputs",
                            std::format("{}: `{}` names its file through a macro, so "
                                        "mcpp cannot track it",
                                        rcSrc.filename().generic_string(), g),
                            "editing that file will not trigger a rebuild",
                            "list it in [resources] extra-inputs = [...]");
                    }
                    if (scan.declaresManifest && anyUtf8Image)
                        return std::unexpected(std::format(
                            "[resources] {} embeds an application manifest, and "
                            "`windows_code_page = \"utf-8\"` embeds another at the same "
                            "ordinal (1).\n  Keep one: add `<activeCodePage "
                            "xmlns=\"http://schemas.microsoft.com/SMI/2019/WindowsSettings\">"
                            "UTF-8</activeCodePage>` to your manifest and set "
                            "`windows_code_page = \"legacy\"`, or drop your manifest.",
                            rcSrc.filename().generic_string()));
                    auto inputs = std::move(scan.inputs);
                    inputs.insert(inputs.end(), extraInputs.begin(), extraInputs.end());
                    if (auto a = add_unit(rcSrc, rcSrc.stem().string(),
                                          std::move(inputs),
                                          static_cast<std::size_t>(-1)); !a)
                        return std::unexpected(a.error());
                }

                // The synthesised script: per image, because OriginalFilename and
                // the version block belong to a specific artifact, and the
                // manifest to a specific executable.
                const bool synthVersion = R.declared() && R.synthesize_version_info();
                auto wantsUtf8 = [&](const mcpp::build::LinkUnit& lu) {
                    if (lu.kind != mcpp::build::LinkUnit::Binary) return false;
                    if (!lu.artifactOf.empty()) return false;
                    for (auto const& t : M.targets)
                        if (t.name == lu.targetName)
                            return t.is_program() && codePageOf(t) == "utf-8";
                    return false;
                };
                if (!iconAbs.empty() || synthVersion || anyUtf8Image) {
                    // A version key mcpp cannot order (an upstream build number)
                    // leaves FILEVERSION's four numeric fields at zero while the
                    // string fields keep the real text. Say so — the properties
                    // dialog will disagree with `[package].version` and nothing
                    // else would explain why.
                    if (synthVersion && !M.package.version.empty()
                        && !mcpp::version_req::parse_version(M.package.version)) {
                        mcpp::diag::degraded("resources/version",
                            std::format("[package].version = \"{}\" has no numeric "
                                        "form", M.package.version),
                            "the embedded FILEVERSION / PRODUCTVERSION fields are "
                            "0,0,0,0 (the string fields keep the real version)",
                            "set [resources.version-info] explicitly, or use a "
                            "dotted numeric version");
                    }
                    for (auto i : peUnits) {
                        const auto& lu = ctx.plan.linkUnits[i];
                        const bool utf8 = wantsUtf8(lu);
                        if (iconAbs.empty() && !synthVersion && !utf8) continue;
                        std::filesystem::path manifestAbs;
                        if (utf8) {
                            manifestAbs = resDir / (lu.targetName + ".mcpp.manifest");
                            const auto manifestText = rsrc::utf8_code_page_manifest();
                            std::string had;
                            if (std::ifstream in(manifestAbs, std::ios::binary); in)
                                had.assign(std::istreambuf_iterator<char>(in), {});
                            if (had != manifestText) {
                                std::ofstream os(manifestAbs, std::ios::binary);
                                if (!os) return std::unexpected(std::format(
                                    "cannot write the application manifest '{}'",
                                    manifestAbs.string()));
                                os << manifestText;
                            }
                        }
                        // A script synthesised for the manifest alone carries
                        // nothing else: a package that declares no [resources]
                        // asked for no version resource.
                        mcpp::manifest::Resources forScript = R;
                        if (!synthVersion) forScript.versionInfo = false;
                        auto text = rsrc::synthesize_rc(
                            M.package, forScript, lu.output.filename().string(),
                            iconAbs, manifestAbs);
                        if (!text) return std::unexpected(text.error());
                        // A stable path, so `cp` + `files = [...]` reproduces the
                        // same resource byte for byte (the L0→L1 escape hatch).
                        auto rcPath = resDir / (lu.targetName + ".mcpp.rc");
                        // Write only on change: rewriting unconditionally would
                        // relink on every build.
                        std::string existing;
                        if (std::ifstream in(rcPath, std::ios::binary); in)
                            existing.assign(std::istreambuf_iterator<char>(in), {});
                        if (existing != *text) {
                            std::ofstream os(rcPath, std::ios::binary);
                            if (!os) return std::unexpected(std::format(
                                "cannot write generated resource script '{}'",
                                rcPath.string()));
                            os << *text;
                        }
                        std::vector<std::filesystem::path> inputs;
                        if (!iconAbs.empty()) inputs.push_back(iconAbs);
                        if (!manifestAbs.empty()) inputs.push_back(manifestAbs);
                        inputs.insert(inputs.end(), extraInputs.begin(), extraInputs.end());
                        if (auto a = add_unit(rcPath, lu.targetName + ".mcpp",
                                              std::move(inputs), i); !a)
                            return std::unexpected(a.error());
                    }
                }

                return {};
            };

            if (trip.is_pe())
                if (auto r = plan_resources(); !r) return std::unexpected(r.error());
        }
    }

    return step13_resource_compiler(state, ctx);
}

// The member path (relative to the workspace root) of a package root, when the
// root is a member of the workspace this build runs in; empty otherwise.
// Read by W3 (a member's non-public modules).
std::string workspace_member_of(const PrepareState& state, const std::filesystem::path& root) {
    if (!state.wsManifest || state.runtimeWorkspaceRoot.empty()) return {};
    const auto rel = root.lexically_normal()
                         .lexically_relative(state.runtimeWorkspaceRoot.lexically_normal())
                         .generic_string();
    if (rel.empty() || rel == "." || rel.starts_with("..")) return {};
    for (auto const& m : state.wsManifest->workspace.members) {
        if (m == rel) return rel;
        if (m.ends_with("/*") && rel.starts_with(m.substr(0, m.size() - 1))
            && rel.find('/', m.size() - 1) == std::string::npos)
            return rel;
    }
    return {};
}

static std::expected<void, std::string> step13_dependency_cache(PrepareState& state, BuildContext& ctx) {
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
    if (cfg2 && ctx.cacheMode == CacheMode::Global) {
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

        // ── Per-package keys, bottom-up ──────────────────────────────────
        // Axes A/B/C are whole-graph, so they are computed once. Axes D/E are
        // per package. Axis F is each direct dependency's key, which forces a
        // bottom-up order: `dependencyEdges` is a DAG (the modgraph validator
        // rejects cycles), so a simple memoized recursion suffices — with an
        // explicit in-progress guard so a cycle that slipped past validation
        // fails loudly instead of recursing until the stack dies.
        namespace ck = mcpp::build::cache_key;
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
            if (best != state.packages.size()) pkgSources[best].push_back(std::move(bestRel));
        }
        for (auto& v : pkgSources) std::ranges::sort(v);

        std::vector<std::string>    pkgKeys(state.packages.size());
        std::vector<nlohmann::json> pkgInputs(state.packages.size(),
                                              nlohmann::json::object());
        std::string                 keyCycleError;
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
        std::vector<char>           localTaint(state.packages.size(), 0);

        // Axis F is each direct dependency's OWN key, which forces a
        // bottom-up order: `dependencyEdges` is a DAG (the modgraph
        // validator rejects cycles among module imports, but this is the
        // package graph, which has no such upstream guard), so the fold
        // below walks `mcpp::graph::topological_order` — dependencies before
        // dependents — instead of recursing, with an explicit cycle check up
        // front standing in for the old in-progress guard.
        mcpp::graph::AdjacencyList pkgDeps(state.packages.size());
        for (auto& e : state.dependencyEdges)
            if (e.consumerPackageIndex < pkgDeps.size())
                pkgDeps[e.consumerPackageIndex].push_back(e.dependencyPackageIndex);
        auto keyOrder = mcpp::graph::topological_order(pkgDeps);
        if (!keyOrder) {
            keyCycleError = std::format(
                "dependency cycle through package '{}' while computing "
                "its build-cache key",
                state.packages[keyOrder.error().cycle.front()].manifest.package.name);
            return std::unexpected(keyCycleError);
        }

        for (auto idx : *keyOrder) {
            ck::PackageAxes pa;
            if (idx > 0 && idx - 1 < state.dep_cache_identities.size()) {
                pa.indexName   = state.dep_cache_identities[idx - 1].indexName;
                pa.packageName = state.dep_cache_identities[idx - 1].packageName;
                pa.version     = state.dep_cache_identities[idx - 1].version;
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
            const bool selfIsIndex = idx > 0
                && idx - 1 < state.dep_cache_identities.size()
                && state.dep_cache_identities[idx - 1].sourceKind == "version";
            if (!selfIsIndex) localTaint[idx] = 1;
            for (auto& e : state.dependencyEdges) {
                if (e.consumerPackageIndex != idx) continue;
                // `idx`'s dependencies precede it in `keyOrder`, so their
                // keys and taint are already folded in below.
                auto const& up = pkgKeys[e.dependencyPackageIndex];
                if (!up.empty()) pa.upstreamKeys.push_back(up);
                if (localTaint[e.dependencyPackageIndex]) localTaint[idx] = 1;
                for (auto& f : e.requestedFeatures) pa.features.push_back(f);
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

        for (std::size_t i = 1; i < state.packages.size(); ++i) {  // skip [0] = main
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
            if (!depIdent || depIdent->sourceKind != "version") continue;
            // ...and neither may anything it was built against be local.
            if (localTaint[i]) continue;
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
            if (!mcpp::build::path_is_under_any(pkgRoot.root, state.storeRoots))
                continue;

            const auto& depName = depIdent->packageName;
            const auto& depVer  = depIdent->version.empty()
                ? pkgRoot.manifest.package.version
                : depIdent->version;

            auto bmiT = mcpp::toolchain::bmi_traits(*state.tc);
            mcpp::bmi_cache::CacheKey key {
                .cacheRoot   = mcpp::home::cache_root(),
                .indexName   = depIdent->indexName,
                .packageName = depName,
                .version     = depVer,
                .keyHex      = pkgKeys[i],
                .inputs      = pkgInputs[i],
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

                if (!cu.providesModule.empty()) {
                    std::string bmi;
                    for (char c : cu.providesModule)
                        bmi.push_back(c == ':' ? '-' : c);
                    bmi += std::string(bmiT.bmiExt);
                    arts.bmiFiles.push_back(std::move(bmi));
                }
                arts.objFiles.push_back({cu.packageObjectRel.generic_string(),
                                         cu.object});
                unitIdx.push_back(u);
            }
            if (!addressable) continue;

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
                ctx.cachedDeps.push_back({depName, depVer, unitIdx.size()});
                continue;       // no populate task; it is already cached
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
            ctx.depsToPopulate.push_back({ std::move(key), std::move(arts) });
        }
    }
    // ──────────────────────────────────────────────────────────────────
    return {};
}

static std::expected<void, std::string> step13_runtime_provider_overrides(PrepareState& state, BuildContext& ctx) {
    // Apply [runtime.<capability>] provider = "<pkg>" overrides. Canonical
    // identity wins; the old short spelling is accepted only when it denotes
    // exactly one provider.  A same-short-name collision is never guessed.
    for (auto& [capKey, prov] : ctx.manifest.runtimeConfig.providerOverrides) {
        std::vector<mcpp::manifest::PackageId> candidates;
        for (auto const& entry : ctx.plan.runtimeProviders) {
            if (!entry.capability.starts_with(capKey)) continue;
            const auto withoutVersion = entry.provider.namespace_.empty()
                ? entry.provider.name
                : entry.provider.namespace_ + "." + entry.provider.name;
            if (entry.provider.canonical() == prov || withoutVersion == prov)
                candidates = {entry.provider};
        }
        if (candidates.empty()) {
            for (auto const& entry : ctx.plan.runtimeProviders) {
                if (entry.capability.starts_with(capKey)
                    && entry.provider.name == prov)
                    candidates.push_back(entry.provider);
            }
        }
        std::ranges::sort(candidates);
        candidates.erase(std::ranges::unique(candidates).begin(), candidates.end());
        if (candidates.empty()) {
            return std::unexpected(std::format(
                "[runtime.{}] provider = \"{}\" does not name a provider in "
                "the resolved dependency graph", capKey, prov));
        }
        if (candidates.size() != 1) {
            std::string choices;
            for (auto const& candidate : candidates)
                choices += (choices.empty() ? "" : ", ") + candidate.canonical();
            return std::unexpected(std::format(
                "[runtime.{}] provider = \"{}\" is ambiguous; use one exact "
                "canonical identity: [{}]", capKey, prov, choices));
        }
        const auto selected = candidates.front();
        std::stable_partition(ctx.plan.runtimeProviders.begin(),
                              ctx.plan.runtimeProviders.end(),
                              [&](const auto& pr) {
            return pr.capability.starts_with(capKey) && pr.provider == selected;
        });
    }
    return {};
}

static std::expected<void, std::string> step13_abi_enforcement(PrepareState& state, BuildContext& ctx) {
    // Capability-driven ABI enforcement, dimensional (see src/toolchain/abi.cppm
    // and .agents/docs/2026-06-27-abi-compat-model-single-pr-design.md). Each
    // dependency may constrain specific toolchain dimensions via `abi:`
    // capabilities (libc / cxxstdlib / arch / os / cxxabi); UNSPECIFIED
    // DIMENSIONS ARE DON'T-CARE. The legacy bare form `abi:glibc` maps to the
    // libc dimension only — so a glibc *C library* (glfw) builds fine under a
    // clang+libc++ toolchain on `*-linux-gnu` (libc is still glibc), which the
    // previous single-axis check wrongly rejected. The toolchain is resolved
    // before the dep graph, so this enforces/diagnoses rather than reselects —
    // abi-driven reselection is a resolution-ordering follow-up.
    {
        const auto prof = mcpp::toolchain::abi_profile(ctx.tc);
        std::vector<mcpp::toolchain::AbiConstraint> constraints;
        for (auto& cap : ctx.plan.runtimeCapabilities) {
            std::string provider;
            for (auto& [c, p] : ctx.plan.runtimeProviders)
                if (c == cap) { provider = p.canonical(); break; }
            if (auto con = mcpp::toolchain::parse_abi_capability(
                    cap, provider.empty() ? std::string_view{"?"} : std::string_view{provider}))
                constraints.push_back(std::move(*con));
        }
        if (auto mismatches = mcpp::toolchain::abi_check(prof, constraints);
            !mismatches.empty()) {
            const auto& mm = mismatches.front();
            return std::unexpected(std::format(
                "ABI incompatibility: dependency '{}' requires {}={}, but the "
                "resolved toolchain '{}' provides {}={}.\n"
                "       fix: select a {}-compatible toolchain "
                "(e.g. gcc@16.1.0 for glibc) or set [toolchain] in mcpp.toml.",
                mm.source, mcpp::toolchain::dim_name(mm.dim), mm.need,
                ctx.tc.label(), mcpp::toolchain::dim_name(mm.dim), mm.got,
                mm.need));
        }
    }
    return {};
}

static std::expected<void, std::string> step13_empty_link_check(PrepareState& state, BuildContext& ctx) {
    // ── A link unit with no inputs is not a build (mcpp#533) ────────────────
    //
    // Checked HERE, last, because objects arrive from three places and each
    // one is legitimate: the compile set, a `role = "object"` action
    // (`lu.objects.emplace_back` above), and a Windows resource unit. A check
    // placed before any of them would refuse a unit that was about to be
    // filled. If a fourth source is ever added, it must land before this line.
    //
    // WHY THIS IS AN ERROR AND NOT A WARNING. The two library kinds fail
    // differently and BOTH failures are worse than this message:
    //
    //   shared — `$cc -shared` over an empty response file. Measured on
    //            gcc 16.1.0: `gcc: fatal error: no input files`, which names
    //            the driver and not the target. Before `cc` was emitted
    //            unconditionally it was `/bin/sh: 1: -shared: not found`,
    //            which names neither.
    //   static — `ar rcs libfoo.a` with no members. Measured: exit 0, an
    //            8-byte archive, and a build that REPORTS SUCCESS. Every
    //            consumer then fails with undefined symbols, one repository
    //            further from the cause.
    //
    // The silent one is why this is not merely a nicer diagnostic. mcpp#533
    // reached here because a dependency's `install()` was skipped over a
    // package-identity collision, leaving a version directory with no source
    // tree; the shape is the same for any package whose sources fail to
    // materialise, which is why the check is on the link unit rather than on
    // the install path.
    for (auto const& lu : ctx.plan.linkUnits) {
        if (!lu.objects.empty()) continue;
        const char* kindName =
              lu.kind == mcpp::build::LinkUnit::SharedLibrary ? "shared library"
            : lu.kind == mcpp::build::LinkUnit::StaticLibrary ? "static library"
            : lu.kind == mcpp::build::LinkUnit::TestBinary    ? "test binary"
                                                              : "binary";
        return std::unexpected(std::format(
            "target '{}' ({}) has no inputs to link\n"
            "       no translation unit and no `role = \"object\"` action "
            "output reached it, and an empty link is not a build: `ar` writes "
            "an empty archive and reports success, so this would otherwise "
            "surface as undefined symbols in whatever consumes '{}'\n"
            "       if '{}' is an installed dependency, its package directory "
            "has no sources — reinstall it and check that its descriptor's "
            "install step ran",
            lu.targetName, kindName, lu.output.generic_string(),
            lu.targetName));
    }
    return {};
}

std::expected<BuildContext, std::string> phase13_finish(PrepareState& state) {
    BuildContext ctx;
    ctx.strict      = state.overrides.strict;
    ctx.manifest    = *state.m;
    ctx.tc          = *state.tc;
    ctx.fp          = state.fp;
    ctx.runtimeSelection = state.runtimeSelection;
    ctx.runtimeBinding = state.runtimeBindingSnapshot;
    ctx.profile     = state.effectiveProfile;
    ctx.activeFeatureRequest = state.workspacePlan() ? state.requestedFeatures
                                                     : state.overrides.features;
    ctx.compilerChoice = { std::string(tc_origin_name(state.tcOrigin)),
                           state.graphCompilerRequiredBy,
                           state.graphCompilerReplaced.empty() ? state.pinReplacedDefault
                                                         : state.graphCompilerReplaced };
    ctx.cacheMode   = state.cacheMode;
    ctx.projectRoot= *state.root;
    ctx.outputDir  = target_dir(*state.tc, state.fp, state.workRoot);

    if (auto r = step13_source_packages(state, ctx); !r) return std::unexpected(r.error());
    if (auto r = step13_runner_and_xlings(state, ctx); !r) return std::unexpected(r.error());
    if (auto r = step13_prebuilt_check(state, ctx); !r) return std::unexpected(r.error());
    if (auto r = step13_link_forms(state, ctx); !r) return std::unexpected(r.error());
    if (auto r = step13_make_plan(state, ctx); !r) return std::unexpected(r.error());
    if (auto r = step13_cxx_private_runtime(state, ctx); !r) return std::unexpected(r.error());
    if (auto r = step13_cxx_process_runtime(state, ctx); !r) return std::unexpected(r.error());
    step13_graph_and_schedule(state, ctx);
    if (auto r = step13_build_graph_actions(state, ctx); !r) return std::unexpected(r.error());
    if (auto r = step13_assembly_units(state, ctx); !r) return std::unexpected(r.error());
    if (auto r = step13_windows_resources(state, ctx); !r) return std::unexpected(r.error());
    if (auto r = step13_dependency_cache(state, ctx); !r) return std::unexpected(r.error());
    if (auto r = step13_lockfile(state, ctx); !r) return std::unexpected(r.error());
    if (auto r = step13_runtime_provider_overrides(state, ctx); !r) return std::unexpected(r.error());
    if (auto r = step13_abi_enforcement(state, ctx); !r) return std::unexpected(r.error());
    step13_resolution_json(state, ctx);
    if (auto r = step13_empty_link_check(state, ctx); !r) return std::unexpected(r.error());
    step13_report_packages(state, ctx);

    ctx.planNotes = std::move(state.planNotes);
    return ctx;
}

// How the report names each package of the plan (build progress design
// 2026-09-29, §4.2): as the requester wrote its key, with where it comes from.
// A package is named after the edge from the root (or the virtual root) when
// there is one, and after its first requester otherwise.
// HOW A PACKAGE LINE NAMES ITS PACKAGE (build output design revision 3,
// §5.8 and §5.10). A package inside the project -- the root, a member, a path
// dependency whose directory lies under the project root -- is named by its
// short name and located by its directory, which is always shown. Any other
// package keeps its full identity, since nothing else on its line says where
// it comes from: an index package (official when the default index serves its
// namespace, otherwise the index is named), a git dependency with its
// reference, a path outside the project with its relative directory. Before
// this, a path dependency was named by the consumer's key and `(path)`, and a
// workspace member by its directory without its version.
static void step13_report_packages(PrepareState& state, BuildContext& ctx) {
    std::vector<mcpp::build::PlanPackage> out;
    out.reserve(state.packages.size());
    const auto base = ctx.projectRoot.lexically_normal();
    auto relative = [&](const std::filesystem::path& root) {
        auto rel = root.lexically_normal().lexically_relative(base).generic_string();
        return rel.empty() ? std::string(".") : rel;
    };
    auto versioned = [](const mcpp::manifest::Manifest& m, std::string_view origin) {
        std::string d = m.package.version.empty() ? std::string{}
                                                  : std::format("v{}", m.package.version);
        if (!origin.empty()) d += std::format("{}({})", d.empty() ? "" : " ", origin);
        return d;
    };
    for (std::size_t i = 0; i < state.packages.size(); ++i) {
        const auto& pkg = state.packages[i];
        const auto& m = pkg.manifest;
        mcpp::build::PlanPackage p;
        p.name = mcpp::build::qualified_package_name(m);
        if (i == 0) {
            if (m.package.virtualRoot) continue;
            p.requested = true;
            p.subject = m.package.name;
            p.detail  = versioned(m, ".");
            p.source  = "project";
            out.push_back(std::move(p));
            continue;
        }
        p.requested = pkg.selectedMember;
        const GraphRequest* edge = nullptr;
        for (auto const& r : state.graphRequests) {
            if (r.dependencyPackageIndex != i || r.consumerPackageIndex == i) continue;
            if (!edge || r.consumerPackageIndex == 0) edge = &r;
            if (r.consumerPackageIndex == 0) break;
        }
        const std::string key = edge ? edge->key : p.name;
        const mcpp::manifest::DependencySpec* spec = nullptr;
        if (edge) {
            const auto& deps = state.packages[edge->consumerPackageIndex].manifest.dependencies;
            if (auto it = deps.find(key); it != deps.end()) spec = &it->second;
        }
        const auto dir = relative(pkg.root);
        const bool insideProject = dir == "." || !dir.starts_with("..");
        // The source is the resolution's record of the package, not the
        // consumer's key: a package reached through `[feature-deps]` or
        // another table has no entry in the consumer's `[dependencies]`.
        const ResolvedRecord* rec = nullptr;
        {
            auto rn = mcpp::pm::compat::resolve_package_name(m.package.name, m.package.namespace_);
            for (auto const& ns : {rn.namespace_, std::string(mcpp::pm::kDefaultNamespace), std::string{}})
                if (auto it = state.resolved.find(ResolvedKey{ns, rn.shortName});
                    it != state.resolved.end()) { rec = &it->second; break; }
        }
        const std::string kind = rec ? rec->source
                               : spec && spec->isPath() ? "path"
                               : spec && spec->isGit()  ? "git" : "version";
        // A path or git package is named without the default namespace: a
        // manifest that declares none takes it during resolution, and a bare
        // name means that namespace (package-identity §4.2), so the prefix
        // would state something its author never wrote.
        const auto declared =
            m.package.namespace_.empty() || m.package.namespace_ == mcpp::pm::kDefaultNamespace
                ? m.package.name : std::format("{}.{}", m.package.namespace_, m.package.name);
        if (pkg.selectedMember || (spec && spec->workspaceMember)
            || (kind == "path" && insideProject)) {
            p.subject = m.package.name;
            p.detail  = versioned(m, dir);
            p.source  = "project";
        } else if (kind == "path") {
            p.subject = declared;
            p.detail  = versioned(m, dir);
            p.source  = "path";
        } else if (kind == "git") {
            // The declared reference: `<url>#<kind>=<ref>` in the record, or
            // the consumer's spec.
            std::string refKind = spec ? spec->gitRefKind : std::string{};
            std::string ref     = spec ? spec->gitRev : std::string{};
            if (rec) {
                const auto hash = rec->sourceRef.rfind('#');
                const auto eq   = rec->sourceRef.find('=', hash == std::string::npos ? 0 : hash);
                if (hash != std::string::npos && eq != std::string::npos) {
                    refKind = rec->sourceRef.substr(hash + 1, eq - hash - 1);
                    ref     = rec->sourceRef.substr(eq + 1);
                }
            }
            if ((refKind.empty() || refKind == "rev") && ref.size() > 12) ref.resize(12);
            p.subject = declared;
            p.detail  = versioned(m, std::format("git {} {}", refKind.empty() ? "rev" : refKind, ref));
            p.source  = "git";
        } else {
            // An index package: official when the default index serves its
            // namespace, otherwise the index that does is named.
            const auto ns = m.package.namespace_.empty()
                ? std::string(mcpp::pm::kDefaultNamespace) : m.package.namespace_;
            const auto* index = state.findIndexForNs ? state.findIndexForNs(ns) : nullptr;
            const bool official = index == nullptr || index->is_builtin();
            p.subject = p.name.empty() ? key : p.name;
            p.detail  = versioned(m, official ? std::string{} : std::format("index {}", index->name));
            p.source  = official ? "official" : "index";
        }
        for (auto const& c : ctx.cachedDeps)
            if (c.name == p.name) p.cachedUnits = c.units;
        out.push_back(std::move(p));
    }
    ctx.plan.packages = std::move(out);
}

void focus_on_member(BuildContext& ctx) {
    if (ctx.workspaceMembers.size() != 1) return;
    auto& m = ctx.workspaceMembers.front();
    for (auto& g : ctx.plan.linkGroups)
        if (g.member == m.name) { swap_link_group(ctx.plan, g); break; }
    ctx.manifest = m.manifest;
    ctx.projectRoot = m.root;
}

} // namespace mcpp::build
