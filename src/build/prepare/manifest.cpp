// manifest.cpp -- P0: the effective manifest and the workspace it belongs to,
// from the one loader every command uses.

module mcpp.build.prepare;
import :state;

import mcpp.build.prepare_inputs;

import std;
import mcpp.targetside;
import mcpp.diag;
import mcpp.build.version_floor;
import mcpp.manifest;
import mcpp.version;          // this binary's release, for E9's floor
import mcpp.xpkg_version;     // the release ordering
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
import mcpp.pack.abi_tag;      // the tag a prebuilt dependency is checked against
import mcpp.pack.prebuilt;     // …and the check itself
import mcpp.pack.stage_tree;   // where `${mcpp.stage_dir}` points, and its manifest
import mcpp.build.build_program;
import mcpp.build.backend;      // BuildOptions for the tool sub-build
import mcpp.build.ninja;        // make_ninja_backend — driving that sub-build
import mcpp.xlings;
import mcpp.xlings.runtime_selection;
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
import mcpp.project;

namespace mcpp::build {

// AND ONLY FOR THE MANIFESTS THE AUTHOR IS LOOKING AT: the root, and in a
// workspace plan each selected member. A layer name this engine does not know
// is a typo there, and a version gap in a dependency's. The reserved `mcpp:`
// prefix exists so the first is an error rather than a silently disabled
// behaviour; refusing the second as well meant the layer vocabulary could
// never be extended by a published package (`warn_unknown_xpkg_keys` carries
// that half).
static std::expected<void, std::string>
refuse_unknown_capability(const mcpp::manifest::Manifest& m,
                          const std::filesystem::path& manifestPath) {
    if (m.unknownCapabilities.empty()) return {};
    auto const& cap = m.unknownCapabilities.front();
    auto why = mcpp::targetside::parse_capability(cap);
    return std::unexpected(std::format(
        "{}: {}", manifestPath.string(),
        why ? std::format("`{}` names no capability mcpp knows.", cap)
            : why.error()));
}

// The schema warnings of a manifest the author is looking at, and its cfg()
// sections that cannot apply. Under --strict they become errors -- same policy
// as the feature/platform schema checks.
static std::expected<void, std::string>
report_manifest_statements(const mcpp::manifest::Manifest& m, bool strict) {
    std::vector<std::string> warnings = m.schemaWarnings;
    // #540: a cfg() predicate mcpp cannot evaluate must say so.
    //
    // A PREDICATE THAT ANSWERS FALSE AND A PREDICATE THAT WAS NEVER
    // UNDERSTOOD USED TO READ THE SAME. `cfgpred` returns false for an unknown
    // key and for an unknown bareword, and a `[target.<pred>.build]` section
    // whose predicate is false is dropped without a word — so a typo, and every
    // `cfg(c-abi = …)` section docs/14 documented before this release, produced
    // a successful build configured as if the section had not been written.
    //
    // Reported here rather than in the manifest parser because the vocabulary
    // lives with the evaluator, and a second copy of it in `toml.cppm` is the
    // exact defect this release is fixing four other instances of.
    //
    // Scoped to the manifests the author is looking at (the root, and in a
    // workspace plan each selected member), which matches the existing policy
    // for every other schema warning: a dependency may adopt a predicate a
    // consumer's older mcpp does not know, and its build stays quiet.
    for (auto const& cc : m.conditionalConfigs) {
        auto unknown = cfgpred::unknown_tokens(cc.predicate);
        if (!unknown.empty()) {
            std::string names;
            for (auto const& u : unknown) {
                if (!names.empty()) names += ", ";
                names += '\'' + u + '\'';
            }
            warnings.push_back(std::format(
                "[target.'{}'] names {} in its cfg() predicate, which mcpp does "
                "not know, so the section never applies (ignored). {}",
                cc.predicate, names, cfgpred::vocabulary_sentence()));
        }
        // A RESOLVED layer is answered AFTER dependency resolution, so a
        // dependency selected by one would form a cycle with the resolution
        // that produces the answer — docs/14 states this. The section's build
        // inputs are honoured by the second pass; its dependencies cannot be,
        // and saying so is the difference between a documented limit and a
        // silent drop.
        //
        // `accelerator` is not one of these (see kCfgEarlyLayerKeys), so
        // `[target.'cfg(accelerator = "cuda")'.dependencies]` is honoured and
        // never reaches this warning: nothing about it is circular, because the
        // accel is an input to the build rather than an answer from the graph.
        if (cfgpred::uses_layer(cc.predicate)
            && !(cc.dependencies.empty() && cc.devDependencies.empty()
                 && cc.buildDependencies.empty() && cc.featureDeps.empty())) {
            warnings.push_back(std::format(
                "[target.'{}'] conditions dependencies on a target-side layer "
                "(ignored). A layer is resolved from the dependency graph, so a "
                "dependency chosen by one would decide the answer it is asking "
                "for. Build inputs under this predicate DO apply; move the "
                "dependency to an unconditional [dependencies] entry, or "
                "condition it on the triple instead.",
                cc.predicate));
        }
        // The same reason holds for a row's library form: whether a package
        // is linked shared is decided while the graph is resolved, before a
        // layer has an answer.
        if (cfgpred::uses_layer(cc.predicate) && !cc.targetKinds.empty()) {
            warnings.push_back(std::format(
                "[target.'{}'] conditions a target's kind or linkage on a "
                "target-side layer (ignored). A layer is resolved from the "
                "dependency graph, and a library's form is decided while that "
                "graph is resolved; condition the statement on the triple "
                "instead.",
                cc.predicate));
        }
    }

    // Surface non-fatal manifest schema warnings (e.g. unsupported [targets.*]
    // keys).
    for (auto const& w : warnings) {
        if (strict) return std::unexpected(w);
        mcpp::diag::warning("manifest/schema", w);
    }
    return {};
}

// The plan's root for a selection of workspace members (workspace design
// 2026-09-29 §15): a virtual root that holds the values the plan shares, and a
// member edge to each selected member, which puts the member in the graph and
// links nothing into the root. A rooted workspace's own package is a member
// like any other ("."), so it is the same node, with the same commands, in
// every selection that reaches it.
// What the workspace root's own manifest states about itself, reported once
// for the command that plans its members (SPEC-004 §9.10):
//   W7  a package table on a root without [package] acts on no package --
//       warning, an error under --strict;
//   the workspace layer read by an engine older than this one is ignored by
//   it without a word (2026.10.8.1 builds with a different toolchain), so a
//   root that writes `[workspace.X]` without stating this engine as its floor
//   gets a note.
static std::expected<void, std::string>
report_workspace_root(const mcpp::manifest::Manifest& ws, bool strict) {
    for (auto const& w : ws.virtualRootPackageTables) {
        if (strict) return std::unexpected(w);
        mcpp::diag::warning("manifest/schema", w);
    }
    if (ws.layer) {
        const auto have = mcpp::xpkg_version::parse(mcpp::MCPP_VERSION);
        const auto floor = ws.workspace.inherited.mcppFloor;
        const auto need = floor.empty() ? std::nullopt : mcpp::xpkg_version::parse(floor);
        // The first release that reads the layer.
        const auto first = mcpp::xpkg_version::parse("2026.10.10.1");
        if (have && first && (!need || mcpp::xpkg_version::compare(*need, *first) < 0)) {
            std::string tables;
            for (auto const& t : ws.workspace.layerTables)
                tables += std::format("{}[workspace.{}]", tables.empty() ? "" : ", ", t);
            mcpp::diag::note("manifest/workspace-layer", std::format(
                "{} is read by mcpp 2026.10.10.1 and later; an older mcpp ignores it and "
                "builds the members without it. State the floor so an older one refuses "
                "instead: [workspace.package] mcpp = \">=2026.10.10.1\"", tables));
        }
    }
    return {};
}

static std::expected<void, std::string>
select_workspace_members(PrepareState& state, const std::filesystem::path& wsRoot,
                         mcpp::manifest::Manifest ws,
                         const std::vector<std::string>& selection) {
    if (auto r = report_workspace_root(ws, state.overrides.strict); !r) return r;
    const auto all = mcpp::project::workspace_members(ws, wsRoot);
    {
        auto first = mcpp::project::load_member_manifest(ws, wsRoot, selection.front());
        if (!first) return std::unexpected(first.error());
        state.m = mcpp::project::virtual_workspace_root(ws, *first, wsRoot);
    }
    state.runtimeWorkspaceRoot = wsRoot;
    state.wsManifest = std::move(ws);
    state.root = wsRoot;
    state.selectedMemberPaths = selection;

    // `--features` names features of the selected members: each member takes
    // the tokens it declares, and a token no selected member declares is
    // refused.
    state.requestedFeatures = state.overrides.features;
    std::vector<std::string> tokens;
    for (auto const& t : mcpp::build::feature_request_tokens(state.overrides.features))
        tokens.push_back(t);
    std::set<std::string> claimed;

    for (auto const& mp : selection) {
        const auto dir = (wsRoot / mp).lexically_normal();
        std::error_code canonEc;
        auto canonical = std::filesystem::weakly_canonical(dir, canonEc);
        if (canonEc) canonical = dir;
        state.selectedMembers[canonical] = mcpp::project::product_directory_name(all, mp);
        if (auto t = state.overrides.member_targets.find(mp); t != state.overrides.member_targets.end())
            state.memberTargets[canonical] = t->second;
        auto member = mcpp::project::load_member_manifest(*state.wsManifest, wsRoot, mp);
        if (!member) return std::unexpected(member.error());
        state.selectedMemberManifests.emplace_back(dir, *member);
        if (auto r = refuse_unknown_capability(*member, dir / "mcpp.toml"); !r)
            return r;
        if (auto r = report_manifest_statements(*member, state.overrides.strict); !r)
            return r;
        std::vector<std::string> requested;
        for (auto const& t : tokens) {
            if (auto fwd = mcpp::pm::split_feature_forward_token(t)) {
                // `<dependency>/<feature>`: a forward of each selected member
                // that declares the dependency key, applied to its edges.
                const bool declares = member->dependencies.contains(fwd->first)
                    || member->devDependencies.contains(fwd->first)
                    || member->buildDependencies.contains(fwd->first);
                if (declares) {
                    state.memberCliForwards[canonical].push_back(*fwd);
                    claimed.insert(t);
                }
                continue;
            }
            if (member->featuresMap.contains(t) || t == "default") {
                requested.push_back(t);
                claimed.insert(t);
            }
        }
        mcpp::manifest::DependencySpec spec;
        spec.path = dir.string();
        spec.namespace_ = member->package.namespace_;
        spec.shortName = member->package.name;
        spec.workspaceMember = true;
        spec.features = std::move(requested);
        spec.declaredIn = "workspace";
        const auto key = member->package.namespace_.empty()
            ? member->package.name
            : member->package.namespace_ + "." + member->package.name;
        state.m->dependencies[key] = std::move(spec);
    }
    for (auto const& t : tokens) {
        if (claimed.contains(t)) continue;
        return std::unexpected(std::format(
            "--features requests '{}', which no selected workspace member "
            "declares", t));
    }
    state.overrides.features.clear();

    std::string shown;
    for (auto const& [key, spec] : state.m->dependencies)
        shown += (shown.empty() ? "" : ", ") + key;
    mcpp::ui::status("Workspace", selection.size() == 1
        ? std::format("building member '{}'", shown)
        : std::format("building {} members: {}", selection.size(), shown));
    return {};
}

// STEP FUNCTION (mcpp#722 / T6 follow-on): phase0's own "Workspace
// handling" section.
static std::expected<void, std::string> step0_workspace_handling(PrepareState& state) {
    // A command at a workspace's root selects members: the command's own list
    // (`--workspace`, one group of it per plan), `-p`, the first member with a
    // program at a virtual root, or the workspace's own package.
    if (state.m->workspace.present) {
        std::vector<std::string> selection = state.overrides.workspace_members;
        if (selection.empty() && !state.overrides.package_filter.empty()) {
            // `-p <name>`: the package identity first, the member's
            // directory as a fallback -- one resolver shared with every
            // other `-p`/`--package` command
            // (mcpp::project::resolve_member_dir, #725).
            auto matched = mcpp::project::resolve_member_dir(
                *state.m, *state.root, state.overrides.package_filter);
            if (!matched) return std::unexpected(matched.error());
            auto rel = matched->empty() ? std::string(".")
                : matched->lexically_normal().lexically_relative(
                      state.root->lexically_normal()).generic_string();
            if (rel.empty()) rel = ".";
            selection.push_back(rel);
        } else if (selection.empty() && state.m->package.name.empty()) {
            // Virtual workspace: find a member with a program target ("is
            // this the program", #622 A3's `is_program()`, so a member whose
            // only target is `kind = "app"` is picked exactly as one whose
            // target is `bin` is), or use last member.
            for (auto& mp : state.m->workspace.members) {
                auto mm = mcpp::manifest::load(*state.root / mp / "mcpp.toml",
                                               {.insideWorkspace = true});
                if (!mm) continue;
                if (std::ranges::any_of(mm->targets, [](auto const& t) { return t.is_program(); })) {
                    selection.push_back(mp);
                    break;
                }
            }
            if (selection.empty() && !state.m->workspace.members.empty())
                selection.push_back(state.m->workspace.members.back());
        } else if (selection.empty()) {
            selection.push_back(".");
        }
        if (selection.empty())
            return std::unexpected(std::string("the workspace lists no members"));
        const auto wsRoot = *state.root;
        auto ws = std::move(*state.m);
        return select_workspace_members(state, wsRoot, std::move(ws), selection);
    }
    // Inside a member: the command selects that member, planned from the
    // workspace's root like every other selection (§15), so its products and
    // its build directory are the ones `-p` and `--workspace` use.
    if (state.effective && state.effective->member && state.effective->workspace
        && !state.overrides.preloaded_manifest) {
        const auto wsRoot = state.effective->workspaceRoot;
        auto ws = std::move(*state.effective->workspace);
        std::vector<std::string> selection = state.overrides.workspace_members;
        if (selection.empty() && !state.overrides.package_filter.empty()) {
            auto matched = mcpp::project::resolve_member_dir(
                ws, wsRoot, state.overrides.package_filter);
            if (!matched) return std::unexpected(matched.error());
            auto rel = matched->empty() ? std::string(".")
                : matched->lexically_normal().lexically_relative(
                      wsRoot.lexically_normal()).generic_string();
            selection.push_back(rel.empty() ? std::string(".") : rel);
        } else if (selection.empty()) {
            auto rel = state.root->lexically_normal()
                           .lexically_relative(wsRoot.lexically_normal()).generic_string();
            // The member as `[workspace] members` spells it, so the
            // selection, the product directory and the header agree.
            for (auto const& mp : ws.workspace.members)
                if (std::filesystem::path(mp).lexically_normal()
                    == std::filesystem::path(rel).lexically_normal()) { rel = mp; break; }
            selection.push_back(rel);
        }
        return select_workspace_members(state, wsRoot, std::move(ws), selection);
    }
    if (state.overrides.preloaded_manifest) {
        auto wsRoot = mcpp::project::find_workspace_root(*state.root);
        if (!wsRoot.empty()) {
            if (auto wsm = mcpp::manifest::load(wsRoot / "mcpp.toml");
                wsm && wsm->workspace.present) {
                state.runtimeWorkspaceRoot = wsRoot;
                state.wsManifest = std::move(*wsm);
            }
        }
        // A preloaded manifest was inherited at its dependency load site,
        // which gives a member everything but the root-position keys. This
        // build IS rooted at it (a host-tool sub-build), so it takes those
        // too, from the workspace that lists it (#710).
        if (state.wsManifest
            && mcpp::project::is_workspace_member(*state.wsManifest, state.runtimeWorkspaceRoot, *state.root))
            mcpp::project::inherit_workspace_root_position(
                *state.m, *state.wsManifest, state.runtimeWorkspaceRoot);
    }
    return {};
}

std::expected<void, std::string> check_engine_floors(const PrepareState& state,
                                                     bool rootOnly) {
    const auto have = mcpp::xpkg_version::parse(mcpp::MCPP_VERSION);
    if (!have) return {};   // a development build with an unparsable version states no order
    auto check = [&](const mcpp::manifest::Manifest& m,
                     std::string_view where) -> std::expected<void, std::string> {
        if (m.package.mcppFloor.empty()) return {};
        const auto need = mcpp::xpkg_version::parse(m.package.mcppFloor);
        if (!need || mcpp::xpkg_version::compare(*have, *need) >= 0) return {};
        const std::string who = m.package.namespace_.empty()
            ? m.package.name : m.package.namespace_ + "." + m.package.name;
        return std::unexpected(std::format(
            "package '{}' ({}) requires mcpp >= {}; this is mcpp {}.\n"
            "       hint: pin \"mcpp\": \"{}\" (or newer) in .xlings.json and run "
            "`xlings install`, or run `xlings install mcpp@{}`",
            who, where, m.package.mcppFloor, mcpp::MCPP_VERSION,
            m.package.mcppFloor, m.package.mcppFloor));
    };
    if (rootOnly) {
        if (!state.m) return {};
        return check(*state.m, state.root ? state.root->generic_string() : "the project");
    }
    for (auto const& p : state.packages)
        if (auto r = check(p.manifest, p.root.generic_string()); !r) return r;
    return {};
}

std::expected<void, std::string> phase0_manifest_and_workspace(PrepareState& state) {
    // A refusal decided early and released late. `host_can_serve` answers
    // "does a payload on this machine produce this target", which is knowable
    // before dependency resolution and is only half the question: a package in
    // the graph can supply the target's system, and the graph is not known
    // here. Held until it is, and released only if nothing supplies it.

    // THE LOCATED APPLE SDK, RESOLVED ONCE AND READ ONCE.
    //
    // `xcrun` is a process. Calling it at the refusal below and again where
    // the answer is stored would be two calls whose answers can differ -- the
    // developer directory can be switched between them -- and this repository
    // has a standing rule that a value crossing two sites is resolved at one.
    // The iOS floor was not written and was taken from the located SDK. The
    // refusal of a dependency's platform floor names where the value came
    // from, and after the fill below the manifest no longer says.
    state.iosFloorFromSdk = false;
    // Non-empty when a target row's convention replaced a toolchain the user
    // had set with `mcpp toolchain default`. Reported on the status line,
    // because a substitution nobody is told about is a rule that can only be
    // learned by experiment — writing the same value a second time in
    // `[target.<triple>]` and observing that it works.
    // THE HOST SPEC AS IT STOOD BEFORE A TARGET ROW'S CONVENTION REPLACED IT,
    // whatever its origin. `build.mcpp` is compiled and run on this machine,
    // so its compiler is a host fact; the row's pin is a target fact. Before
    // this snapshot existed, `host_tc_for_build_program` read `tcSpec` after
    // the row had overwritten it and resolved the row's payload "for the
    // host" -- which works by accident for a payload whose compiler can also
    // target the host (an NDK clang) and cannot work for one that cannot:
    // `em++` produces WebAssembly under every invocation, and every project
    // with a build program failed under `--target wasm32-emscripten` inside
    // `emcc.py` (#622, measured by the dist-web member's first build).
    //
    // Empty when the row replaced nothing — no [toolchain], no global
    // default, no [target.<row>] entry existed before the row's pin applied.
    // THIS IS NOT "the row's pin remains the only spec there is": on a
    // fresh $HOME whose first-ever invocation names a hosted `--target`
    // (nothing to be "before"), that reading resolved the SAME payload the
    // row just picked — `em++` again — as the host compiler, which is the
    // exact defect this field exists to close, just with no prior value to
    // restore. `host_tc_for_build_program` resolves the platform's own
    // native default in that case instead (`native_first_run_spec()`), the
    // same one a plain `mcpp build` would have installed.
    // THE PACKAGE WHOSE `requires` CHOSE THE COMPILER, AND WHAT IT ASKED FOR.
    //
    // Non-empty only when the graph's requirement actually changed the answer.
    // Reported on the status line for the same reason `pinReplacedDefault` is:
    // a compiler the user did not name is a decision they did not make, and one
    // reported without its reason is a rule learned by experiment.
    // The C library the target triple asked for, taken before the triple is
    // canonicalised. Empty when the project declined to name one.
    // The target as the project spelled it, when that differs from the
    // canonical identity. Report only; empty means they coincide.
    // The target row's toolchain convention, held until the graph is known.
    // Empty when the row names none or the project named its own.
    // AND WHETHER THAT PIN IS A CONVENTION OR A CAPABILITY, RECORDED AT
    // THE SAME READ.
    //
    // A hosted row's pin answers "which payload supplies this target's C
    // library", so a graph that supplies one instead makes it inapplicable.
    // A freestanding row's pin answers a different question — the table says
    // so in its own words: "the pin is llvm on every host because clang/lld
    // are cross-compilers by construction". A host g++ cannot emit
    // riscv64-none-elf at all, and no dependency changes that.
    //
    // Taken here rather than re-derived at the decision point, because the row
    // is read exactly once and both facts come out of that read.
    state.targetPinIsCapability = false;
    // THE ROW'S PIN, KEPT EVEN WHEN THE PROJECT NAMED ITS OWN COMPILER —
    // which is exactly when `targetPinCandidate` above is left empty.
    //
    // The candidate answers "should mcpp apply its convention"; this answers
    // "what does the convention SAY", and the two differ precisely in the case
    // that needs a diagnosis: a project that overrode the convention and has
    // nothing supplying what the convention was there to supply.
    // Whether the resolved toolchain spec names the machine's own Visual
    // Studio. Decided inside `resolve_target_toolchain`, read by
    // `host_tc_for_build_program`, which is why it is declared out here.
    state.tcSpecIsMsvc = false;

    state.root = state.overrides.project_root.empty()
        ? mcpp::project::find_manifest_root(std::filesystem::current_path())
        : std::optional<std::filesystem::path>(state.overrides.project_root);
    if (!state.root) {
        return std::unexpected("no mcpp.toml found in current directory or any parent");
    }
    // THE PROJECT'S PATH IS PART OF EVERY DOCUMENT A BUILD WRITES, and those
    // documents are UTF-8 text (build.ninja, compile_commands.json). A
    // directory whose path has no UTF-8 spelling used to fail the first build
    // with `internal: unhandled exception: [json.exception.type_error.316]`
    // (#693, measured on Linux with a Latin-1 name and on a Windows code page
    // 1252 host with a name that page can spell). It is refused here, by name.
    if (!mcpp::modgraph::try_narrow(*state.root)) {
        return std::unexpected(std::format(
            "the project directory '{}' has no UTF-8 spelling.\n"
            "       {}\n"
            "       Every file a build writes names this directory in UTF-8; "
            "rename or move it.",
            mcpp::modgraph::escaped_spelling(*state.root),
            mcpp::modgraph::no_utf8_spelling_reason()));
    }
    // NOTE: `workRoot` is deliberately NOT derived here. `root` is not final
    // yet — the workspace block below reassigns it to the selected member
    // (`root = memberDir`), and anchoring the write root to the pre-switch
    // value puts a member's target/, mcpp.lock and .mcpp/ at the WORKSPACE
    // root. See the derivation right after that block.

    // A registry package in `compat` form (Form B) ships NO mcpp.toml — its
    // manifest is synthesized from the `.lua` descriptor by the resolver. So a
    // nested build of such a package cannot re-read one off disk, and the
    // caller hands over the manifest it already synthesized instead.
    //
    // Passing it in rather than re-deriving it is also the more correct of the
    // two: re-deriving could produce a DIFFERENT manifest than the one the
    // parent resolved against (the L1 cfg merge and feature-activated deps
    // have already been folded in by then).
    // THE EFFECTIVE MANIFEST, FROM THE ONE LOADER EVERY COMMAND USES.
    //
    // A command issued inside a member directory receives the member's
    // manifest after workspace inheritance, exactly as `publish`, `pack`,
    // `emit xpkg` and `toolchain list` do (#690, W4). A command at the
    // workspace root receives the root manifest as written; the `-p <member>`
    // switch below loads and inherits the member it names.
    //
    // A PRELOADED manifest (a host-tool sub-build) is already effective: the
    // resolver loaded it at the dependency's load site, where a member
    // inherits (see `inherit_as_workspace_member`). It is not inherited a second time; the
    // workspace it belongs to is still recorded below, so that its own sibling
    // dependencies inherit as members.
    state.m =
        std::unexpected(std::string{});
    if (state.overrides.preloaded_manifest) {
        state.m = *state.overrides.preloaded_manifest;
    } else {
        auto loaded = mcpp::project::load_effective_manifest(*state.root);
        if (!loaded) return std::unexpected(loaded.error());
        state.m = loaded->manifest;
        state.effective = std::move(*loaded);
    }

    // The root's; a workspace plan's members are refused as they are selected.
    if (auto r = refuse_unknown_capability(*state.m, *state.root / "mcpp.toml"); !r)
        return r;

    // A DISTRIBUTION package is not a source tree, and building "in" one is a
    // failure that looks like a success: `interface/` holds declarations whose
    // definitions are in the prebuilt archive, so the build compiles the
    // declarations, produces a near-empty library, links nothing, and reports
    // Finished. The archive it was supposed to carry never enters the picture.
    //
    // Only the ROOT is refused. As a dependency this is exactly what the
    // package is for — the consumer compiles the interface and links the
    // artifact, which is the whole design.
    if (!state.overrides.preloaded_manifest && mcpp::pack::is_distribution_package(*state.m)) {
        return std::unexpected(std::format(
            "'{}' is a distribution package produced by `mcpp pack`, not a source tree.\n"
            "  Its sources are interface declarations; the definitions are in the\n"
            "  prebuilt artifacts beside them, so building here would produce an\n"
            "  empty library and say it succeeded.\n"
            "  Use it: add it to a project as a dependency —\n"
            "      [dependencies]\n"
            "      {} = {{ path = \"{}\" }}",
            state.root->string(), state.m->package.name, state.root->string()));
    }

    if (auto r = step0_workspace_handling(state); !r) return std::unexpected(r.error());


    if (auto bad = mcpp::project::unresolved_workspace_dependency_error(*state.m, *state.root))
        return std::unexpected(*bad);

    if (state.overrides.inherited_runtime_selection) {
        state.runtimeSelection = *state.overrides.inherited_runtime_selection;
    } else {
        std::optional<std::reference_wrapper<const mcpp::manifest::Manifest>> wsRef;
        if (state.wsManifest) wsRef = std::cref(*state.wsManifest);
        auto selected = mcpp::xlings::runtime::select_runtime(
            *state.m, wsRef, *state.root, state.runtimeWorkspaceRoot);
        if (!selected) return std::unexpected(selected.error());
        state.runtimeSelection = std::move(*selected);
    }

    // Where mcpp WRITES — derived here because `root` is only final now: the
    // workspace block above may have moved it to the selected member. Defaults
    // to the project root, so every existing invocation is byte-for-byte
    // unchanged; the tool-provisioning pass points it at the tool store
    // instead (BuildOverrides::work_dir).
    state.workRoot =
        state.overrides.work_dir.empty() ? *state.root : state.overrides.work_dir;
    {
        std::error_code wdEc;
        std::filesystem::create_directories(state.workRoot, wdEc);
    }

    if (state.m->package.sourceProvenance.empty()) {
        state.m->package.sourceProvenance =
            "path+" + state.root->lexically_normal().generic_string();
    }

    // A `compat`-form (Form B) package's sources live under a wrap directory
    // inside the version dir, which is why its descriptor writes globs like
    // `*/src/foo.cc` — the `*` stands for the tarball's top-level folder,
    // whose name the descriptor cannot know. `[build] sources` has always
    // expanded those; `targets.<x>.main` did NOT, so a bin target in such a
    // package handed ninja a literal `*` and died with
    // `missing and no known rule to make it`.
    //
    // Nothing could reach that path before #355 (a dependency's bin targets
    // were never built), which is why it went unnoticed. Resolve it here, once
    // the manifest is final and before anything reads `t.main`.
    for (auto& t : state.m->targets) {
        if (t.main.empty() || t.main.find('*') == std::string::npos) continue;
        auto hits = mcpp::modgraph::expand_glob(*state.root, t.main);
        if (hits.size() == 1) {
            t.main = std::filesystem::relative(hits.front(), *state.root).generic_string();
        } else {
            return std::unexpected(std::format(
                "target '{}': `main = \"{}\"` matched {} files; it must name "
                "exactly one entry source",
                t.name, t.main, hits.size()));
        }
    }

    // Inject synthetic targets (e.g. test binaries from `mcpp test`). In a
    // workspace plan they are the selected member's tests, and that member
    // takes them when it is loaded (graph.cpp).
    if (!state.workspacePlan())
        for (auto& t : state.extraTargets) state.m->targets.push_back(t);

    // The root's; a workspace plan's members are reported as they are selected.
    if (auto r = report_manifest_statements(*state.m, state.overrides.strict); !r)
        return r;

    // Load mcpp.lock once, up front: it is a resolution input for git deps
    // (#329), which decide the commit to build long before anything is
    // fetched. Keyed by package name — the same key the writer at the end of
    // this function emits, both taken from the root manifest's [dependencies].
    {
        // Read where the project keeps it. A planning pass that writes
        // elsewhere (plan_only) still resolves against the project's lock.
        auto lockPath = (state.overrides.plan_only ? *state.root : state.workRoot) / "mcpp.lock";
        // A workspace keeps one lock at its root (§15). Until its first build
        // has written it, a selected member's own lock from before is read in
        // its place, so the upgrade does not move a git dependency.
        if (state.workspacePlan() && !std::filesystem::exists(lockPath))
            for (auto const& mp : state.selectedMemberPaths)
                if (std::filesystem::exists(state.runtimeWorkspaceRoot / mp / "mcpp.lock")) {
                    lockPath = state.runtimeWorkspaceRoot / mp / "mcpp.lock";
                    break;
                }
        if (std::filesystem::exists(lockPath)) {
            if (auto lock = mcpp::pm::load(lockPath); lock) {
                for (auto const& p : lock->packages) {
                    if (!p.namespace_.empty())
                        state.packageIdentityLockAnchors.emplace(
                            p.name, p.namespace_);
                    if (auto parsed = mcpp::pm::parse_git_source(p.source); parsed)
                        state.gitLockAnchors.emplace(p.name, std::move(*parsed));
                }
            } else {
                // Degraded, not a plain warning: the engine silently does less
                // than asked — every git branch dep falls back to `ls-remote`
                // and may advance past the commit the lock recorded.
                mcpp::diag::degraded("lockfile",
                    std::format("mcpp.lock could not be read: {}",
                                lock.error().message),
                    "git branch dependencies are re-resolved over the network "
                    "and may move onto a newer commit than the one recorded",
                    "delete mcpp.lock and rebuild to regenerate it");
            }
        }
    }

    // Global-cache mode: --cache > MCPP_BUILD_CACHE > [build] cache > global.
    // An unparseable value is a warning (error under --strict) and falls
    // through to the next source rather than silently meaning "global" — a typo
    // that quietly re-enabled the cache would be the hardest kind of surprise
    // to attribute.
    // Selection lives in resolve_cache_mode (above) so the fast paths settle it
    // identically. This block only adds the diagnostics, which the fast paths
    // have no business emitting: an unparseable value must be reported once, by
    // the invocation that actually resolves the build.
    state.cacheMode = resolve_cache_mode(*state.m, state.overrides.cache_mode);
    {
        const char* envMode = std::getenv("MCPP_BUILD_CACHE");
        for (auto [value, origin] : std::initializer_list<
                 std::pair<std::string_view, std::string_view>>{
                 {state.overrides.cache_mode,        "--cache"},
                 {envMode ? envMode : "",      "MCPP_BUILD_CACHE"},
                 {state.m->buildConfig.cacheMode,    "[build] cache"}}) {
            if (value.empty() || parse_cache_mode(value)) continue;
            auto msg = std::format(
                "{} has unknown cache mode '{}' (expected: global | local | off)",
                origin, value);
            if (state.overrides.strict) return std::unexpected(msg);
            mcpp::diag::warning("build/cache-mode", msg);
        }
    }

    return {};
}

} // namespace mcpp::build
