// build_programs.cpp -- P9's build programs: the root's `build.mcpp`, and
// each member's, run once the target side is resolved (cut from
// target_side.cpp at that step's boundary, as check_file_lengths.sh asks).

module mcpp.build.prepare;
import :state;

import mcpp.build.prepare_inputs;

import std;
import mcpp.targetside;
import mcpp.diag;
import mcpp.build.refusal;
import mcpp.build.version_floor;
import mcpp.build.command_line;
import mcpp.home;
import mcpp.toolchain.platform;
import nlohmann.json;
import mcpp.log;
import mcpp.manifest;
import mcpp.source_kind;
import mcpp.modgraph.glob;
import mcpp.graph;
import mcpp.build.progress;   // the members' programs wait in order (design 2026-09-29 §4.2)
import mcpp.build.schedule.policy;   // resolve_jobs — how many programs compile at once (#748, B2)
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;
import mcpp.modgraph.validate;
import mcpp.toolchain.hostflags;   // the compile-token producer the package std module reuses
import mcpp.toolchain.cenv;        // [c-abi] declaration → compiler configuration (design 2026-09-18)
import mcpp.toolchain.cenv_probe;  // [c-abi] declaration is checked, not trusted (design §3.2)
import mcpp.toolchain.predefines;  // the macros this engine defines: contract and emission in one module
import mcpp.toolchain.cppfly;
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
import mcpp.freestanding.target;   // the target sysroot layout (libdir)
import mcpp.freestanding.linkline; // the ISA profile, for the std module command
import mcpp.toolchain.post_install;
import mcpp.toolchain.abi;
import mcpp.toolchain.triple;
import mcpp.build.linkage_form;   // #519 — which form each dependency takes
import mcpp.build.plan;
import mcpp.build.flags;          // compute_flags — the per-role contracts (#418)
import mcpp.build.graph_shape;  // #407: the graph says which mode wrote it
import mcpp.pack.abi_tag;      // the tag a prebuilt dependency is checked against
import mcpp.pack.prebuilt;     // …and the check itself
import mcpp.pack.stage_tree;   // where `${mcpp.stage_dir}` points, and its manifest
import mcpp.build.build_program;
import mcpp.build.backend;      // BuildOptions for the tool sub-build
import mcpp.build.ninja;        // make_ninja_backend — driving that sub-build
import mcpp.toolchain.post_install;
import mcpp.ui;
import mcpp.log;
import mcpp.wire;               // Severity, for PlanNote (#699 item 2, E3)

namespace mcpp::build {

std::expected<void, std::string> step9_root_build_program(PrepareState& state) {
    // ── L3: ROOT build.mcpp (moved after dependency resolution, design §3.1
    // item 4) ────────────────────────────────────────────────────────────────
    // Runs HERE — after dep resolution + feature activation (so the contract
    // env can expose MCPP_DEP_<NAME>_DIR exactly like the dep loop above does)
    // and BEFORE the modgraph scan / flag canonicalization / fingerprint (so
    // its generated=/source= sources and flag directives are fully visible).
    // Ordering invariants preserved relative to the pre-move call site:
    // materialize_generated_files (may produce build.mcpp itself) and the L1
    // cfg merge still run earlier — ONLY this call moved later.
    //
    // One wrinkle the old ordering hid: back then apply() mutated *m BEFORE
    // `packages[0] = makePackageRoot(*root, *m)` snapshotted buildConfig into
    // privateBuild/manifest — the copies the scan and per-TU flag assembly
    // actually read. Now the snapshot (and root feature activation on it)
    // already happened, so mirror the directive TAILS into packages[0]
    // explicitly, the same way the dep loop does for its package.
    // A package with no `build.mcpp` still runs one when a rule dependency
    // described it: `run_build_program` writes that program into the build
    // directory. This guard therefore asks the same question the function does,
    // and a guard that asked only about the file left the synthesis unreachable
    // -- the shaders went uncompiled and the refusal named the missing program
    // rather than the guard. Measured.
    // A workspace plan's virtual root has no program; its members' programs
    // run in `step9_member_build_programs`, below.
    if (state.m->package.virtualRoot) return {};
    if (std::filesystem::exists(*state.root / "build.mcpp")
        || !state.m->buildConfig.ruleModules.empty()) {
        auto host = state.host_tc_for_build_program();
        if (!host) return std::unexpected(host.error());
        mcpp::build::BuildProgramEnv bpEnv;
        bpEnv.targetTriple = state.resolvedTargetCanonical;
        // Everything the engine already knows and a build program would
        // otherwise hardcode: the payload ROOT (not the driver), the target's
        // C library, which compiler and which C++ standard library resolved,
        // and the three answers that keep a board package from naming a
        // toolchain. One call — see fill_target_build_env.
        fill_target_build_env(bpEnv, *state.m, state.tc ? &*state.tc : nullptr, state.cfg_opt ? &*state.cfg_opt : nullptr);
        apply_graph_cxx_abi(bpEnv, state.packages);
        bpEnv.toolsBin = state.projectSubosBin;
        bpEnv.profile      = state.effectiveProfile;
        bpEnv.accel        = state.resolvedAccel();
        fill_package_build_env(bpEnv, *state.m);
        state.fillPackEnv(bpEnv, 0);
        bpEnv.languageModules = state.m->language.modules;
        bpEnv.ruleModules  = state.m->buildConfig.ruleModules;
        if (auto dit = state.deviceSourcesByPackage.find(state.root->string()); dit != state.deviceSourcesByPackage.end())
            bpEnv.deviceSources = dit->second;
        // Set explicitly rather than relying on build_dir()'s root-relative
        // default: under BuildOverrides::work_dir the package root is shared
        // and may be read-only, and the default would write the compiled
        // helper straight into it. Same value as the default when work_dir is
        // unset, so an ordinary build is unchanged.
        bpEnv.artifactsDir = build_program_dir(state, 0);
        // Root mode keeps genBase empty: a relative `generated=` from the ROOT
        // package resolves against the package root (the documented contract),
        // not against OUT_DIR.
        // Same expression as the pre-move call site (and same order), so the
        // contract hash — and therefore the build.mcpp cache — is unchanged
        // across the move for feature-identical builds.
        bpEnv.features     = feature_closure(*state.m, parse_feature_request(state.overrides.features));
        bpEnv.outDir       = state.programOutDir(state.workRoot, *state.m, bpEnv.features);
        // mcpp#241 (root): consumer index 0, same owner as the dep loop.
        //
        // AND THE LINK FORM OF EACH DEPENDENCY (#642 E2), to this program only.
        // The root decides every dependency's form, and when this program runs
        // every input of that decision is final: the requests are the root
        // manifest's, and each dependency's own program has already run. A
        // DEPENDENCY's program is not offered the forms. It runs in discovery
        // order, before the programs of packages discovered after it, and those
        // programs supply facts the answer depends on (a `-L` they add makes a
        // package static-only), so the value it could be given would be a guess.
        {
            std::map<std::size_t, std::string> rootLinkForms;
            for (auto const& [idx, form] : state.dependencyLinkForms)
                if (form.recorded)
                    rootLinkForms.emplace(idx, std::string(
                        mcpp::build::linkage_form::to_string(form.answer.linkage)));
            state.fillDepDirs(bpEnv, 0, &rootLinkForms);
        }
        state.fillXpkgDirs(bpEnv, *state.m, 0);
        // #355: the host tools the ROOT package requested (consumer index 0).
        if (auto tit = state.toolEnvByConsumer.find(0u); tit != state.toolEnvByConsumer.end())
            bpEnv.toolPaths = tit->second;
        // The program, as the build programs' graph compiled it (mcpp#797).
        if (auto program = build_program_binary(state, 0); program) {
            bpEnv.programBinary   = program->path;
            bpEnv.programRunner   = program->runner;
            bpEnv.programIdentity = program->identity;
            bpEnv.programCompile  = program->compile;
        } else if (!state.overrides.plan_only) {
            return std::unexpected(program.error());
        } else {
            bpEnv.programError = program.error();
        }
        // #647 E1: THE RESOLVED GRAPH, FOR THE ROOT'S PROGRAM ONLY.
        //
        // Every package, dependencies before the packages that request them
        // (ties in discovery order), so a program that merges what libraries
        // contribute can apply them in override order without a sort of its
        // own. The root decides the graph, and every input of that decision is
        // final here -- the same reason `dep_linkage` is offered to this
        // program alone. A file, not variables: a graph with metadata does not
        // fit an environment block (`MAX_ARG_STRLEN`, the Windows limit).
        //
        // ITS DIGEST JOINS THE RE-RUN KEY. Editing a dependency's
        // `[package.metadata]` changes what this program would answer, so it
        // must run again; editing that dependency's sources does not, and the
        // document does not change.
        {
            std::vector<std::size_t> order;
            std::vector<bool> placed(state.packages.size(), false);
            while (order.size() < state.packages.size()) {
                std::size_t pick = state.packages.size();
                for (std::size_t i = 0; i < state.packages.size() && pick == state.packages.size(); ++i) {
                    if (placed[i]) continue;
                    bool ready = true;
                    for (auto const& r : state.graphRequests)
                        if (r.consumerPackageIndex == i && r.dependencyPackageIndex != i
                            && r.dependencyPackageIndex < state.packages.size()
                            && !placed[r.dependencyPackageIndex]) { ready = false; break; }
                    if (ready) pick = i;
                }
                // A cycle leaves nothing ready; its first member in discovery
                // order is taken so the document is still complete.
                if (pick == state.packages.size())
                    for (std::size_t i = 0; i < state.packages.size(); ++i)
                        if (!placed[i]) { pick = i; break; }
                placed[pick] = true;
                order.push_back(pick);
            }
            nlohmann::json doc;
            doc["kind"] = "mcpp.graph";
            doc["version"] = 1;
            nlohmann::json list = nlohmann::json::array();
            for (auto i : order) list.push_back(state.graph_package_entry(i, true, nullptr));
            doc["packages"] = std::move(list);
            const auto text = doc.dump(2) + "\n";
            const auto graphPath = bpEnv.artifactsDir / "graph.json";
            std::error_code gec;
            std::filesystem::create_directories(graphPath.parent_path(), gec);
            const auto tmp = graphPath.string() + ".tmp";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                out << text;
            }
            std::filesystem::rename(tmp, graphPath, gec);
            if (gec)
                return std::unexpected(std::format(
                    "cannot write the graph document '{}': {}",
                    graphPath.string(), gec.message()));
            bpEnv.graphFile   = graphPath;
            bpEnv.graphDigest = mcpp::toolchain::hash_string(text);
        }
        auto& bcRoot = state.m->buildConfig;
        const auto mark = state.markDirectiveTail(*state.m);
        const auto rldN = bcRoot.ldflags.size(), rsrcN = bcRoot.sources.size(),
                   rmodN = state.m->modules.sources.size();
        const auto ractN = bcRoot.actions.size();
        // #622 A4: how many `[runtime] deploy` entries existed before this
        // program ran — the manifest-sourced ones, already in `packages[0]`'s
        // snapshot. Anything past this index is a `mcpp::deploy()` residue
        // that needs the same mirror the flag/source tails get below.
        const auto rdeployN = state.m->runtimeConfig.linkIntent.deploy.size();
        // Same reason, one field wide: `mcpp::runtime_search_dir()` residue
        // needs the same mirror `deploy` does, or `resolve_runtime_contract`
        // (which reads `packages[0]`'s snapshot, not `*m`) never sees it.
        const auto rsearchDirN = state.m->runtimeConfig.linkIntent.runtimeSearchDirs.size();
        // What the dependencies supplied as runners, before the root's program
        // speaks. The root's emissions are appended to the same slots, so a
        // name both supply becomes one argv joining the two (#634, §9 item 8,
        // measured: `run-A.sh run-B.sh <artifact>`).
        const auto runnerBeforeRoot = bcRoot.runner;
        const auto namedBeforeRoot  = bcRoot.namedRunners;
        // The root's program is the requested package's.
        bpEnv.requested = !state.m->package.virtualRoot;
        auto bp = run_answering_requests(state, *state.m, bpEnv, 0,
            state.m->package.name, [&] {
                return mcpp::build::run_build_program(
                    *state.m, *state.root, host->first, host->second,
                    state.m->cppStandard, bpEnv);
            });
        if (!bp && !state.overrides.plan_only) {
            return std::unexpected(bp.error());
        }
        // #699 item 2 (E3): under `emit build-database` (`plan_only`), a
        // failing root build program describes the package without its
        // directives rather than costing the whole plan. Every mirror below
        // reads what the program would have added to `*m`, so skipping
        // straight past it (nothing runs on this path) is what "without its
        // directives" means; a later failure that follows from the gap
        // fails the member under the ordinary rule (E1).
        if (!bp) {
            state.planNotes.push_back({"MCPP_BUILD_DATABASE_PROGRAM_FAILED",
                bp.error(), mcpp::wire::Severity::Error,
                (*state.root / "build.mcpp").string()});
            // Named so the device-source check below (and anything else whose
            // premise is this program's directives) can tell a package whose
            // program failed apart from one that simply has no program.
            state.programFailedPackages.insert(state.root->string());
        }
        if (bp) {
            // THE SAME RULE THE DEPENDENCIES ARE HELD TO, WITH THE ROOT AS A PARTY.
            // Two suppliers of one runner are refused naming both, and the
            // manifest is the way to choose: a `[target.<triple>]` runner the
            // project writes outranks every supplied one where the runner is
            // looked up, so a name the manifest declares is not refused here.
            {
                const auto parsedTarget = state.tc
                    ? mcpp::toolchain::triple::parse(state.tc->targetTriple)
                    : std::nullopt;
                const auto rowKey = parsedTarget ? parsedTarget->str()
                                  : state.tc     ? state.tc->targetTriple : std::string{};
                const auto* row = [&]() -> const mcpp::manifest::TargetEntry* {
                    if (parsedTarget) return find_target_entry(*state.m, *parsedTarget);
                    auto it = state.m->targetOverrides.find(rowKey);
                    return it == state.m->targetOverrides.end() ? nullptr : &it->second;
                }();
                const auto manifestNames = [&](std::string_view name) {
                    if (!row) return false;
                    if (name.empty()) return !row->runner.empty();
                    return row->namedRunners.contains(std::string(name));
                };
                if (!state.runnerProvider.empty() && !runnerBeforeRoot.empty()
                    && bcRoot.runner.size() > runnerBeforeRoot.size()
                    && !manifestNames({})) {
                    return std::unexpected(std::format(
                        "the dependency '{}' and this project's build program both "
                        "supply the runner for this target, and the two would be "
                        "joined into one argv.\n"
                        "       Drop one of them, or state the runner in "
                        "[target.{}].runner.",
                        state.runnerProvider, rowKey));
                }
                for (auto const& [name, nr] : bcRoot.namedRunners) {
                    auto before = namedBeforeRoot.find(name);
                    auto who = state.namedRunnerProvider.find(name);
                    if (before == namedBeforeRoot.end() || before->second.argv.empty()
                        || who == state.namedRunnerProvider.end() || who->second.empty())
                        continue;
                    if (nr.argv.size() <= before->second.argv.size()) continue;
                    if (manifestNames(name)) continue;
                    return std::unexpected(std::format(
                        "the dependency '{}' and this project's build program both "
                        "supply a runner named '{}' for this target, and the two "
                        "would be joined into one argv.\n"
                        "       Drop one of them, or state it in "
                        "[target.{}.runners].{}.",
                        who->second, name, rowKey, name));
                }
            }
            auto& pkg0 = state.packages[0];
            // Compile-visible tail → privateBuild: the shared fold (same owner
            // as the dep loop; the root's TUs read privateBuild).
            state.foldDirectiveTailIntoPrivateBuild(pkg0, *state.m, mark);
            // Before the source residues are mirrored below: adopting an action's
            // outputs APPENDS to bcRoot.sources, and those appends must be inside
            // the tail that gets copied into the packages[0] snapshot the scan reads.
            state.adoptActionOutputs(*state.m, *state.root, ractN);
            // The root's build program has spoken; a floor it stated is checked
            // now, with the facts every package (it included) established.
            if (auto err = state.checkVersionFloors(); err) return std::unexpected(*err);
            // Root residues — apply() mutated *m, but packages[0].manifest is a
            // value-copy snapshot taken at makePackageRoot, so everything the
            // scan/fingerprint read from the snapshot needs the tail mirrored:
            // sources → the scan walks packages[0].manifest, not *m.
            pkg0.manifest.buildConfig.sources.insert(
                pkg0.manifest.buildConfig.sources.end(),
                bcRoot.sources.begin() + rsrcN, bcRoot.sources.end());
            pkg0.manifest.modules.sources.insert(
                pkg0.manifest.modules.sources.end(),
                state.m->modules.sources.begin() + rmodN, state.m->modules.sources.end());
            // The units its actions generate, as they declared them (D6).
            for (auto const& [path, unit] : state.m->modules.declaredUnits)
                pkg0.manifest.modules.declaredUnits.insert_or_assign(path, unit);
            // The snapshot's manifest is what later readers of the root's
            // flags see -- mirror the flag/include tails, as the old
            // pre-snapshot ordering implicitly did.
            pkg0.manifest.buildConfig.cflags.insert(
                pkg0.manifest.buildConfig.cflags.end(),
                bcRoot.cflags.begin() + static_cast<std::ptrdiff_t>(mark.cflags),
                bcRoot.cflags.end());
            pkg0.manifest.buildConfig.cxxflags.insert(
                pkg0.manifest.buildConfig.cxxflags.end(),
                bcRoot.cxxflags.begin() + static_cast<std::ptrdiff_t>(mark.cxxflags),
                bcRoot.cxxflags.end());
            pkg0.manifest.buildConfig.includeDirs.insert(
                pkg0.manifest.buildConfig.includeDirs.end(),
                bcRoot.includeDirs.begin() + static_cast<std::ptrdiff_t>(mark.includeDirs),
                bcRoot.includeDirs.end());
            pkg0.manifest.buildConfig.includeDirsAfter.insert(
                pkg0.manifest.buildConfig.includeDirsAfter.end(),
                bcRoot.includeDirsAfter.begin()
                    + static_cast<std::ptrdiff_t>(mark.includeDirsAfter),
                bcRoot.includeDirsAfter.end());
            // Link flags → the final link reads *m (already applied); keep the
            // linkUsage snapshot and fingerprint metadata equivalent too.
            pkg0.linkUsage.ldflags.insert(pkg0.linkUsage.ldflags.end(),
                bcRoot.ldflags.begin() + rldN, bcRoot.ldflags.end());
            pkg0.manifest.buildConfig.ldflags.insert(
                pkg0.manifest.buildConfig.ldflags.end(),
                bcRoot.ldflags.begin() + rldN, bcRoot.ldflags.end());
            // #622 A4: `mcpp::deploy()` residue → `packages[0].manifest`, the
            // object `resolve_runtime_contract` (plan.cppm) actually reads.
            // Without this mirror a directive-sourced deploy entry lands in `*m`
            // and nowhere the planner looks — the same gap this block already
            // closes for sources/flags, one more field wide.
            pkg0.manifest.runtimeConfig.linkIntent.deploy.insert(
                pkg0.manifest.runtimeConfig.linkIntent.deploy.end(),
                state.m->runtimeConfig.linkIntent.deploy.begin() + static_cast<std::ptrdiff_t>(rdeployN),
                state.m->runtimeConfig.linkIntent.deploy.end());
            // `mcpp::runtime_search_dir()` residue → `packages[0].manifest`, the
            // same object and the same reason as the `deploy` mirror above: without
            // it a directive-sourced entry lands in `*m` and `resolve_runtime_contract`
            // never looks there.
            pkg0.manifest.runtimeConfig.linkIntent.runtimeSearchDirs.insert(
                pkg0.manifest.runtimeConfig.linkIntent.runtimeSearchDirs.end(),
                state.m->runtimeConfig.linkIntent.runtimeSearchDirs.begin()
                    + static_cast<std::ptrdiff_t>(rsearchDirN),
                state.m->runtimeConfig.linkIntent.runtimeSearchDirs.end());
        }
    }
    return {};
}

// The graph document a build program reads (#647 E1): every package the
// program's own package reaches, itself included, dependencies before the
// packages that request them (ties in discovery order), with `root` marking
// the package whose program reads it. For a root that is the whole graph.
static std::expected<std::pair<std::filesystem::path, std::string>, std::string>
write_graph_document(PrepareState& state, std::size_t subject,
                     const std::filesystem::path& artifactsDir) {
    std::vector<bool> reached(state.packages.size(), subject == 0);
    reached[subject] = true;
    for (bool grew = true; grew;) {
        grew = false;
        for (auto const& r : state.graphRequests)
            if (r.consumerPackageIndex < reached.size() && reached[r.consumerPackageIndex]
                && r.dependencyPackageIndex < reached.size()
                && !reached[r.dependencyPackageIndex]) {
                reached[r.dependencyPackageIndex] = true;
                grew = true;
            }
    }
    std::vector<std::size_t> order;
    std::vector<bool> placed(state.packages.size(), false);
    std::size_t wanted = 0;
    for (bool b : reached) wanted += b ? 1 : 0;
    while (order.size() < wanted) {
        std::size_t pick = state.packages.size();
        for (std::size_t i = 0; i < state.packages.size() && pick == state.packages.size(); ++i) {
            if (placed[i] || !reached[i]) continue;
            bool ready = true;
            for (auto const& r : state.graphRequests)
                if (r.consumerPackageIndex == i && r.dependencyPackageIndex != i
                    && r.dependencyPackageIndex < state.packages.size()
                    && reached[r.dependencyPackageIndex]
                    && !placed[r.dependencyPackageIndex]) { ready = false; break; }
            if (ready) pick = i;
        }
        // A cycle leaves nothing ready; its first member in discovery
        // order is taken so the document is still complete.
        if (pick == state.packages.size())
            for (std::size_t i = 0; i < state.packages.size(); ++i)
                if (!placed[i] && reached[i]) { pick = i; break; }
        placed[pick] = true;
        order.push_back(pick);
    }
    nlohmann::json doc;
    doc["kind"] = "mcpp.graph";
    doc["version"] = 1;
    // The requests a document lists are those made by its own packages. A
    // request from outside the program's closure (another workspace member, a
    // workspace plan's virtual root) is a fact about the plan, not about the
    // package, and would make the document, and with it the program's cache
    // key, depend on which members the command selected.
    nlohmann::json list = nlohmann::json::array();
    for (auto i : order) {
        auto entry = state.graph_package_entry(i, true, &reached);
        entry["root"] = i == subject;
        list.push_back(std::move(entry));
    }
    doc["packages"] = std::move(list);
    const auto text = doc.dump(2) + "\n";
    const auto graphPath = artifactsDir / "graph.json";
    std::error_code gec;
    std::filesystem::create_directories(graphPath.parent_path(), gec);
    const auto tmp = graphPath.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
    }
    std::filesystem::rename(tmp, graphPath, gec);
    if (gec)
        return std::unexpected(std::format(
            "cannot write the graph document '{}': {}",
            graphPath.string(), gec.message()));
    return std::pair{graphPath, mcpp::toolchain::hash_string(text)};
}

// The build programs of a workspace plan's members (workspace design
// 2026-09-29 §15). A member is the project being developed, so its program
// runs where a root's does: after every dependency's program, with the link
// form of each dependency and the graph document of what it reaches, and with
// its own artifacts directory, `<member>/target/.build-mcpp`, where it was
// when the member was the root of its own build. A workspace member reached as
// a dependency of another member runs here too, so its program sees the same
// environment in every selection.
std::expected<void, std::string> step9_member_build_programs(PrepareState& state) {
    if (!state.m->package.virtualRoot) return {};
    // Dependencies first: a member's program runs after the programs of the
    // members it depends on, whose directives are then in place in the graph
    // it reads. Discovery order, which the packages are numbered in, puts a
    // member before its dependency when its key sorts first.
    mcpp::graph::AdjacencyList deps(state.packages.size());
    for (auto const& r : state.graphRequests)
        if (r.consumerPackageIndex < deps.size() && r.dependencyPackageIndex < deps.size()
            && r.consumerPackageIndex != r.dependencyPackageIndex)
            deps[r.consumerPackageIndex].push_back(r.dependencyPackageIndex);
    // Mutual dev-dependencies between members are legal, so a cycle skips its
    // closing edge and every other dependency still comes first.
    std::vector<std::size_t> roots(state.packages.size());
    std::iota(roots.begin(), roots.end(), std::size_t{0});
    const auto order = *mcpp::graph::depth_first_order(deps, roots, mcpp::graph::Cycles::Skip);
    auto hasProgram = [&](std::size_t i) {
        if (i == 0 || !state.isWorkspaceMemberPackage(i) || !state.compilesHere(i)) return false;
        std::error_code bpEc;
        return std::filesystem::exists(state.packages[i].root / "build.mcpp", bpEc)
            || !state.packages[i].manifest.buildConfig.ruleModules.empty();
    };
    // Their compiles may overlap (below), and their runs follow this order, so
    // every program not yet run is waiting for its turn; its line says so
    // (build progress design 2026-09-29, §3.2), named as the program names
    // itself.
    for (auto const i : order) {
        if (!hasProgram(i)) continue;
        const auto& m = state.packages[i].manifest;
        mcpp::build::progress::program_scheduled(
            m.package.namespace_.empty() ? m.package.name
                                         : m.package.namespace_ + "." + m.package.name,
            state.packages[i].selectedMember);
    }
    // THE ENVIRONMENT A MEMBER'S PROGRAM IS GIVEN. Computed from the plan as it
    // stands when it is asked, so the compile phase and the run each ask for it.
    auto program_env = [&](std::size_t i)
        -> std::expected<mcpp::build::BuildProgramEnv, std::string> {
        auto& pkg = state.packages[i];
        mcpp::build::BuildProgramEnv bpEnv;
        bpEnv.targetTriple = state.resolvedTargetCanonical;
        fill_target_build_env(bpEnv, *state.m, state.tc ? &*state.tc : nullptr,
                              state.cfg_opt ? &*state.cfg_opt : nullptr);
        apply_graph_cxx_abi(bpEnv, state.packages);
        bpEnv.toolsBin = state.projectSubosBin;
        bpEnv.profile  = state.effectiveProfile;
        bpEnv.accel    = state.resolvedAccel();
        fill_package_build_env(bpEnv, pkg.manifest);
        // The stage of the member this program acts for: its own, or the one
        // packed member that reaches it (`PrepareState::fillPackEnv`).
        state.fillPackEnv(bpEnv, i);
        bpEnv.requested       = pkg.selectedMember;
        bpEnv.languageModules = pkg.manifest.language.modules;
        bpEnv.ruleModules  = pkg.manifest.buildConfig.ruleModules;
        if (auto dit = state.deviceSourcesByPackage.find(pkg.root.string());
            dit != state.deviceSourcesByPackage.end())
            bpEnv.deviceSources = dit->second;
        bpEnv.artifactsDir = build_program_dir(state, i);
        if (i < state.activeFeaturesByPackage.size())
            bpEnv.features = state.activeFeaturesByPackage[i];
        // Beside the cache above, which is in the project under a plan's
        // work directory too: `emit build-database` reuses the build's run.
        bpEnv.outDir = state.programOutDir(*state.root, pkg.manifest, bpEnv.features);
        {
            std::map<std::size_t, std::string> linkForms;
            for (auto const& [idx, form] : state.dependencyLinkForms)
                if (form.recorded)
                    linkForms.emplace(idx, std::string(
                        mcpp::build::linkage_form::to_string(form.answer.linkage)));
            state.fillDepDirs(bpEnv, i, &linkForms);
        }
        state.fillXpkgDirs(bpEnv, pkg.manifest, i);
        if (auto tit = state.toolEnvByConsumer.find(i); tit != state.toolEnvByConsumer.end())
            bpEnv.toolPaths = tit->second;
        // The program, as the build programs' graph compiled it (mcpp#797).
        if (auto program = build_program_binary(state, i); program) {
            bpEnv.programBinary   = program->path;
            bpEnv.programRunner   = program->runner;
            bpEnv.programIdentity = program->identity;
            bpEnv.programCompile  = program->compile;
        } else {
            bpEnv.programError = program.error();
        }
        auto graph = write_graph_document(state, i, bpEnv.artifactsDir);
        if (!graph) return std::unexpected(graph.error());
        bpEnv.graphFile   = graph->first;
        bpEnv.graphDigest = graph->second;
        return bpEnv;
    };

    // THE COMPILES WERE MADE TOGETHER: every program this plan runs is compiled
    // in one build of the build programs' graph, before the dependencies'
    // programs run (features.cpp, mcpp#797). The programs run here, one at a
    // time, in the order above.
    for (auto const i : order) {
        if (!hasProgram(i)) continue;
        auto& pkg = state.packages[i];
        auto host = state.host_tc_for_build_program();
        if (!host) return std::unexpected(host.error());
        auto envOf = program_env(i);
        if (!envOf) return std::unexpected(envOf.error());
        auto& bpEnv = *envOf;

        auto& bc = pkg.manifest.buildConfig;
        const auto mark = state.markDirectiveTail(pkg.manifest);
        const auto actN = bc.actions.size();
        const auto runnerN = bc.runner.size();
        auto namedBefore = bc.namedRunners;
        const bool exclusiveBefore = bc.runExclusive;
        auto bp = run_answering_requests(state, pkg.manifest, bpEnv, i,
            pkg.manifest.package.name, [&] {
                return mcpp::build::run_build_program(
                    pkg.manifest, pkg.root, host->first, host->second,
                    pkg.manifest.cppStandard, bpEnv);
            });
        if (!bp) {
            if (!state.overrides.plan_only)
                return std::unexpected(std::format(
                    "workspace member '{}': {}", pkg.manifest.package.name, bp.error()));
            state.planNotes.push_back({"MCPP_BUILD_DATABASE_PROGRAM_FAILED",
                std::format("workspace member '{}': {}", pkg.manifest.package.name, bp.error()),
                mcpp::wire::Severity::Error, (pkg.root / "build.mcpp").string()});
            state.programFailedPackages.insert(pkg.root.string());
            continue;
        }
        state.foldDirectiveTailIntoPrivateBuild(pkg, pkg.manifest, mark);
        state.adoptActionOutputs(pkg.manifest, pkg.root, actN);
        // How the artifact is executed reaches the plan's root, where `mcpp
        // run` reads it, by the rule the dependencies' runners follow: one
        // supplier per runner.
        auto& rootBc = state.m->buildConfig;
        if (bc.runner.size() > runnerN) {
            if (!rootBc.runner.empty() && !state.runnerProvider.empty())
                return std::unexpected(std::format(
                    "'{}' and '{}' both supply a runner for this target.\n"
                    "       Drop one of them, or state the runner in "
                    "[target.<triple>].runner.",
                    state.runnerProvider, pkg.manifest.package.name));
            rootBc.runner.assign(bc.runner.begin() + static_cast<std::ptrdiff_t>(runnerN),
                                 bc.runner.end());
            state.runnerProvider = pkg.manifest.package.name;
        }
        for (auto const& [name, nr] : bc.namedRunners) {
            auto before = namedBefore.find(name);
            const bool grew = before == namedBefore.end()
                           || nr.argv.size() > before->second.argv.size()
                           || (nr.longLived && !before->second.longLived);
            if (!grew) continue;
            auto& slot = rootBc.namedRunners[name];
            auto& who  = state.namedRunnerProvider[name];
            if (!slot.argv.empty() && !who.empty())
                return std::unexpected(std::format(
                    "'{}' and '{}' both supply a runner named '{}' for this target.",
                    who, pkg.manifest.package.name, name));
            slot = nr;
            who  = pkg.manifest.package.name;
        }
        if (bc.runExclusive && !exclusiveBefore) rootBc.runExclusive = true;
        // The platform's fields the member's program stated, as a dependency's
        // program's are (SPEC-013 §4.1).
        merge_program_platform(state, std::exchange(bc.programPlatform, {}),
                               pkg.manifest.package.name);
    }
    if (auto err = state.checkVersionFloors(); err) return std::unexpected(*err);
    // What the programs published (an include directory, an interface
    // define) reaches the members' consumers, as after the dependencies'
    // programs.
    state.computeUsageRequirements();
    return {};
}

} // namespace mcpp::build
