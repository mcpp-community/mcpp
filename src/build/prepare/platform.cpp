// platform.cpp -- the platform of the target graph through the prepare phases
// (SPEC-013; design 2026-10-28 §5, §7-§9). Declared in `:state`.
//
//   P1   which platform (`step1_select_platform`): before the conditional
//        tables merge, so `cfg(platform = ...)` is answered with the rest of
//        the target's keys.
//   P4b  the platform package is the root's first dependency
//        (`step4b_seed_platform`); once the graph has been walked, its
//        `[platform]` is read and merged with this machine's and the
//        project's overrides, and the layers and tools the graph lacks are
//        added to it (`step4b_apply_platform`, walked again by the caller);
//        a version group the graph contradicts is refused
//        (`step4b_check_platform_versions`).
//   P5   its toolchain (`step5_platform_toolchain`).
//   P13  the statement every reader uses (`step13_finish_platform`):
//        directives merged in, flags evaluated for the target, tools located.

module mcpp.build.prepare;
import :state;

import std;
import mcpp.build.prepare_inputs;
import mcpp.build.refusal;
import mcpp.config;
import mcpp.invocation;
import mcpp.manifest;
import mcpp.platform;
import mcpp.pm.compat;
import mcpp.targetside;
import mcpp.toolchain.triple;
import mcpp.toolchain.platform;
import mcpp.ui;
import mcpp.freestanding.linkline;

namespace mcpp::build {

namespace fs = std::filesystem;
namespace mp = mcpp::manifest::platform;

namespace {

mcpp::toolchain::triple::Triple target_of(const PrepareState& state) {
    if (!state.overrides.target_triple.empty())
        if (auto t = mcpp::toolchain::triple::parse(state.overrides.target_triple)) return *t;
    return mcpp::toolchain::triple::host_triple();
}

// The package short name a reference names, before the package is fetched:
// the reference's own name (a git reference always has one), or the
// `[package]` of a directory.
std::string known_short_name(const mp::PackageRef& r) {
    if (!r.spec.shortName.empty()) return r.spec.shortName;
    if (!r.spec.path.empty()) {
        if (auto m = mcpp::manifest::load(fs::path(r.spec.path) / "mcpp.toml"))
            return mcpp::pm::compat::resolve_package_name(m->package.name, m->package.namespace_)
                .shortName;
        return {};
    }
    return {};
}

// Whether this invocation's command line stated a variable (`--platform` is
// `MCPP_PLATFORM` for the invocation), rather than the environment.
bool stated_on_command_line(std::string_view name) {
    return std::ranges::any_of(mcpp::invocation::overrides(), [&](auto const& o) {
        return o.first == name && o.second.has_value();
    });
}

// The project's override tables: the plan root's, which hold the workspace
// layer's first (`merge_layer_root_position`).
std::vector<mp::Override> project_tables(const mcpp::manifest::Manifest& m) {
    return m.platformOverrides;
}

// Which layers each package of the graph provides, by layer.
std::map<std::string, std::string> provided_layers(const PrepareState& state) {
    std::map<std::string, std::string> out;
    for (std::size_t i = 1; i < state.packages.size(); ++i) {
        auto const& pm = state.packages[i].manifest;
        for (auto const& entry : pm.provides) {
            auto cap = mcpp::targetside::parse_capability(entry);
            if (!cap || !*cap) continue;
            auto const& c = **cap;
            std::string layer(mcpp::targetside::cap_layer_name(c.layer));
            if (!mp::is_layer(layer)) continue;
            out.try_emplace(layer, pm.package.version.empty()
                ? pm.package.name
                : std::format("{}@{}", pm.package.name, pm.package.version));
        }
    }
    return out;
}

std::string qualified(const mcpp::manifest::Manifest& m) {
    auto n = mcpp::pm::compat::resolve_package_name(m.package.name, m.package.namespace_);
    return mcpp::pm::compat::qualified_name(n.namespace_, n.shortName);
}

// The key a worklist item for a reference is written under: its name, or
// the short name its source gives.
std::string item_key(const mp::PackageRef& r) {
    if (!r.name.empty()) return r.name;
    return known_short_name(r);
}

// The selector evaluator the platform module asks: this graph's context.
mcpp::platform::Matches matcher(cfgpred::Ctx c) {
    return [c = std::move(c)](std::string_view sel) { return cfgpred::matches(std::string(sel), c); };
}

} // namespace

cfgpred::Ctx platform_cfg_context(const PrepareState& state) {
    return cfgpred::context_for(target_of(state).str(), state.platformPackageShortName);
}

std::expected<void, std::string> step1_select_platform(PrepareState& state) {
    // A host sub-build (build programs, tools) is the host graph: its platform
    // is the one its caller states (the build programs' graph under a host
    // platform), or the host conventions -- and a platform's own tools are
    // always built so (the bootstrap boundary, §6.2).
    const bool nested = state.overrides.host_target || state.overrides.tool_depth > 0
        || state.overrides.build_program_graph;
    if (nested && state.overrides.platform_choice.empty()) return {};
    // THE HOST GRAPH'S PLATFORM, chosen by the outermost prepare (§6.1).
    if (!nested) {
        mp::Override config;
        if (auto cfg = state.get_cfg(false); cfg) config = (*cfg)->hostPlatform;
        const auto hostFlag = mcpp::invocation::get("MCPP_HOST_PLATFORM").value_or("");
        mp::Override project = state.m->hostPlatform;
        auto hs = mcpp::platform::select_host(hostFlag, stated_on_command_line("MCPP_HOST_PLATFORM"),
                                              project, config);
        if (!hs) return std::unexpected(hs.error());
        state.hostPlatformSel = std::move(*hs);
    }
    const auto t = target_of(state);
    const auto flag = !state.overrides.platform_choice.empty()
        ? state.overrides.platform_choice
        : mcpp::invocation::get("MCPP_PLATFORM").value_or("");
    const auto envKey = target_env_name(t, "PLATFORM");
    const auto env = mcpp::invocation::get(envKey).value_or("");
    auto project = project_tables(*state.m);
    std::vector<mp::Override> config;
    if (auto cfg = state.get_cfg(false); cfg) config = (*cfg)->platformOverrides;
    std::string column;
    if (auto* row = mcpp::toolchain::triple::find_known_target(t)) column = std::string(row->platform);
    mcpp::platform::ChoiceInputs in{ flag, env, envKey, project, config, column };
    if (state.overrides.platform_choice.empty() && !stated_on_command_line("MCPP_PLATFORM"))
        in.flagKey = "MCPP_PLATFORM";
    auto sel = mcpp::platform::select_target(in, matcher(cfgpred::context_for(t.str())));
    if (!sel) return std::unexpected(sel.error());
    state.platform = mcpp::platform::Resolved{};
    state.platform.selection = std::move(*sel);
    if (!state.platform.selection.none) {
        auto const& ref = state.platform.selection.ref;
        // A directory is read now, and its manifest's own error is the answer
        // when it cannot be.
        if (!ref.spec.path.empty() && ref.name.empty()) {
            auto m = mcpp::manifest::load(fs::path(ref.spec.path) / "mcpp.toml");
            if (!m)
                return std::unexpected(std::format("{}: the platform {}: {}",
                    mcpp::platform::origin_text(state.platform.selection.origin),
                    mp::display(ref), m.error().format()));
        }
        state.platformPackageShortName = known_short_name(ref);
        if (state.platformPackageShortName.empty())
            return std::unexpected(std::format(
                "{}: the platform {} names no package; write {{ name = \"<package>\", ... }}",
                mcpp::platform::origin_text(state.platform.selection.origin),
                mp::display(state.platform.selection.ref)));
        // READ NOW WHEN IT IS ON DISK: a directory, or the package a first
        // pass fetched. The target base is decided before the graph exists,
        // and a platform may state one (§7).
        fs::path early = state.overrides.platform_root;
        if (early.empty() && !state.platform.selection.ref.spec.path.empty())
            early = state.platform.selection.ref.spec.path;
        if (!early.empty())
            if (auto m = mcpp::manifest::load(early / "mcpp.toml"); m && m->platformDeclared) {
                state.platformData = m->platform;
                state.platformEarly = true;
            }
    }
    return {};
}

void step4b_seed_platform(PrepareState& state) {
    if (state.platform.selection.none) return;
    auto const& ref = state.platform.selection.ref;
    WorkItem item;
    item.name = item_key(ref);
    item.spec = ref.spec;
    if (item.spec.shortName.empty()) item.spec.shortName = state.platformPackageShortName;
    if (item.spec.namespace_.empty()) {
        item.spec.namespace_ = std::string(mcpp::pm::kDefaultNamespace);
        item.spec.namespaceOmitted = true;
    }
    if (item.spec.candidates.empty())
        item.spec.candidates = {{ .namespace_ = item.spec.namespace_, .shortName = item.spec.shortName }};
    item.spec.declaredIn = "the platform";
    item.requestedBy = state.m->package.name + " (platform)";
    item.originalConstraint = item.spec.version;
    item.consumerDepIndex = kMainConsumer;
    state.worklist.push_front(std::move(item));
}

namespace {
thread_local std::optional<fs::path> g_platformRestart;
} // namespace

std::optional<fs::path> take_platform_restart() {
    auto r = std::move(g_platformRestart);
    g_platformRestart.reset();
    return r;
}

std::expected<bool, std::string> step4b_apply_platform(PrepareState& state) {
    auto& P = state.platform;
    if (P.selection.none) return false;
    const auto t = target_of(state);

    // ── The package, once: find it, read it, merge what overrides it ──────
    if (!P.active) {
        const mcpp::modgraph::PackageRoot* found = nullptr;
        const auto& ref = P.selection.ref;
        for (std::size_t i = 1; i < state.packages.size(); ++i) {
            auto const& pr = state.packages[i];
            auto n = mcpp::pm::compat::resolve_package_name(pr.manifest.package.name,
                                                             pr.manifest.package.namespace_);
            const bool byPath = !ref.spec.path.empty()
                && fs::weakly_canonical(pr.root) == fs::weakly_canonical(fs::path(ref.spec.path));
            if (byPath || n.shortName == state.platformPackageShortName) { found = &pr; break; }
        }
        if (!found)
            return std::unexpected(std::format(
                "the platform {} ({}) was not resolved into the graph",
                mp::display(ref), mcpp::platform::origin_text(P.selection.origin)));
        if (!found->manifest.platformDeclared) {
            refusal::record(refusal::Code::PlatformNotAPlatform);
            return std::unexpected(std::format(
                "{} is selected as the platform for {} ({}), and its mcpp.toml has no "
                "[platform] section.\n"
                "       A platform package states the configuration of its targets there "
                "(SPEC-013 §2); select a platform package, or none (--platform none).",
                qualified(found->manifest), t.str(),
                mcpp::platform::origin_text(P.selection.origin)));
        }
        P.package = qualified(found->manifest);
        P.version = found->manifest.package.version;
        P.root = found->root;
        auto hosted = mcpp::platform::serves(found->manifest.platform, P.package, t);
        if (!hosted) {
            refusal::record(refusal::Code::PlatformTargetUnserved);
            return std::unexpected(std::format("{} ({})", hosted.error(),
                mcpp::platform::origin_text(P.selection.origin)));
        }
        P.selection.hosted = *hosted;
        state.platformData = *hosted ? mcpp::platform::hosted_view(found->manifest.platform)
                                     : found->manifest.platform;
        P.fields = state.platformData;
        const auto ctx = platform_cfg_context(state);
        std::vector<mp::Override> config;
        if (auto cfg = state.get_cfg(false); cfg) config = (*cfg)->platformOverrides;
        for (auto const* o : mcpp::platform::matching(config, matcher(ctx)))
            mcpp::platform::merge(P.fields, o->fields);
        auto project = project_tables(*state.m);
        for (auto const* o : mcpp::platform::matching(project, matcher(ctx)))
            mcpp::platform::merge(P.fields, o->fields);
        P.active = true;
        state.platformInjected.insert(P.package);
        // A base the platform states, which this pass decided without: the
        // next pass reads the package before deciding it (§7).
        if (P.fields.sysroot.set && !state.platformEarly && P.fields.sysroot.origin.tier == mp::Tier::Platform) {
            g_platformRestart = P.root;
            return std::unexpected(std::string("mcpp:platform-restart"));
        }
    }

    // ── The layers the graph lacks (§4.3) ─────────────────────────────────
    bool pushed = false;
    const auto provided = provided_layers(state);
    std::map<std::string, std::vector<std::string>> byKey;   // item key -> layers
    std::map<std::string, const mp::LayerEntry*> entryOf;
    for (auto layer : mp::kLayers) {
        auto it = P.fields.layers.find(std::string(layer));
        if (it == P.fields.layers.end()) continue;
        auto const& e = it->second;
        if (e.ref.off) {
            P.layers[std::string(layer)] = { "none", e.origin.tier };
            continue;
        }
        if (auto pv = provided.find(std::string(layer)); pv != provided.end()) {
            // Whose: the platform's when the provider is a package the
            // platform added, the graph's otherwise.
            const auto providerName = pv->second.substr(0, pv->second.find('@'));
            bool ours = false;
            for (auto const& q : state.platformInjected)
                if (q == providerName || q.ends_with("." + providerName)
                    || providerName.ends_with("." + q.substr(q.find('.') + 1)))
                    ours = true;
            P.layers[std::string(layer)] = { pv->second, ours ? e.origin.tier : mp::Tier::Graph };
            continue;
        }
        const auto key = item_key(e.ref);
        if (key.empty())
            return std::unexpected(std::format(
                "{}: the package for {} names no package; write {{ name = \"<package>\", ... }}",
                mcpp::platform::origin_text(e.origin), layer));
        if (state.platformInjected.contains("layer:" + key)) continue;   // already walked
        byKey[key].push_back(std::string(layer));
        entryOf[key] = &e;
    }
    for (auto const& [key, layers] : byKey) {
        auto const& e = *entryOf.at(key);
        WorkItem item;
        item.name = key;
        item.spec = e.ref.spec;
        if (item.spec.shortName.empty()) item.spec.shortName = known_short_name(e.ref);
        if (item.spec.namespace_.empty()) {
            item.spec.namespace_ = std::string(mcpp::pm::kDefaultNamespace);
            item.spec.namespaceOmitted = true;
        }
        if (item.spec.candidates.empty())
            item.spec.candidates = {{ .namespace_ = item.spec.namespace_,
                                      .shortName = item.spec.shortName }};
        std::string names;
        for (auto const& l : layers) { if (!names.empty()) names += ", "; names += l; }
        item.spec.declaredIn = std::format("the platform's {}", names);
        item.requestedBy = std::format("{} (platform: {})", state.m->package.name, names);
        item.originalConstraint = item.spec.version;
        item.consumerDepIndex = kMainConsumer;
        // The root declares it, as it would by hand: every reader of the
        // root's edges (the link's closure among them) sees it.
        state.m->dependencies[key] = item.spec;
        state.platformLayerSpecs[key] = item.spec;
        state.worklist.push_back(std::move(item));
        state.platformInjected.insert("layer:" + key);
        state.platformInjected.insert(mcpp::pm::compat::qualified_name(
            state.worklist.back().spec.namespace_, state.worklist.back().spec.shortName));
        pushed = true;
    }

    // ── The tools the graph does not build (§4.4) ─────────────────────────
    std::set<std::string> requested;   // tools the root already asks the graph for
    for (auto const& [k, spec] : state.m->buildDependencies)
        for (auto const& tool : spec.tools) requested.insert(tool);
    std::map<std::string, std::vector<std::string>> toolsByKey;
    std::map<std::string, const mp::ToolEntry*> toolEntryOf;
    for (auto const& [name, e] : P.fields.tools) {
        if (!e.ref.program.empty()) continue;            // found, not built
        if (requested.contains(name)) continue;          // the graph builds it already
        const auto key = item_key(e.ref);
        if (key.empty())
            return std::unexpected(std::format(
                "{}: the package for the tool {} names no package; write {{ name = \"<package>\", ... }}",
                mcpp::platform::origin_text(e.origin), name));
        if (state.platformInjected.contains("tool:" + name)) continue;
        toolsByKey[key].push_back(name);
        toolEntryOf[key] = &e;
    }
    for (auto const& [key, tools] : toolsByKey) {
        auto const& e = *toolEntryOf.at(key);
        WorkItem item;
        item.name = key;
        item.spec = e.ref.spec;
        if (item.spec.shortName.empty()) item.spec.shortName = known_short_name(e.ref);
        if (item.spec.namespace_.empty()) {
            item.spec.namespace_ = std::string(mcpp::pm::kDefaultNamespace);
            item.spec.namespaceOmitted = true;
        }
        if (item.spec.candidates.empty())
            item.spec.candidates = {{ .namespace_ = item.spec.namespace_,
                                      .shortName = item.spec.shortName }};
        item.spec.tools = tools;
        std::string names;
        for (auto const& n : tools) { if (!names.empty()) names += ", "; names += n; }
        item.spec.declaredIn = std::format("the platform's tools ({})", names);
        item.requestedBy = std::format("{} (platform tools: {})", state.m->package.name, names);
        item.originalConstraint = item.spec.version;
        item.consumerDepIndex = kMainConsumer;
        item.buildOnly = true;
        item.fromBuildTable = true;
        // The root asks for them as it would for a declared build dependency,
        // so the tool provisioning pass builds them for this machine.
        state.m->buildDependencies[key] = item.spec;
        for (auto const& n : tools) state.platformInjected.insert("tool:" + n);
        state.platformInjected.insert(mcpp::pm::compat::qualified_name(item.spec.namespace_,
                                                                       item.spec.shortName));
        state.worklist.push_back(std::move(item));
        pushed = true;
    }
    return pushed;
}

std::expected<void, std::string> step4b_check_platform_versions(PrepareState& state) {
    auto const& P = state.platform;
    if (!P.active || P.fields.versions.empty()) return {};
    // Asked of the edges as they stand: the check runs before each round's
    // drain, before `buildTimeOnly` is decided.
    const auto instances = package_instances(state);
    for (std::size_t i = 1; i < state.packages.size(); ++i) {
        // A package built for this machine only (a tool's) is not linked with
        // the target's, and a group's exact pins are about one link.
        if (!instances.target[i] && instances.host[i]) continue;
        auto const& pm = state.packages[i].manifest;
        auto n = mcpp::pm::compat::resolve_package_name(pm.package.name, pm.package.namespace_);
        auto it = P.fields.versions.find(n.shortName);
        if (it == P.fields.versions.end()) continue;
        if (pm.package.version.empty() || pm.package.version == it->second) continue;
        // A package taken from a directory or a repository is a development
        // tree, which the version group does not bind.
        if (auto r = state.resolved.find({n.namespace_, n.shortName});
            r != state.resolved.end() && r->second.source != "version")
            continue;
        std::string by;
        if (auto r = state.resolved.find({n.namespace_, n.shortName}); r != state.resolved.end())
            by = r->second.requestedBy;
        refusal::record(refusal::Code::PlatformVersionGroup);
        return std::unexpected(std::format(
            "{} {} is in this build, and the platform {} is a version group that names {} {}.\n"
            "       requested by: {}\n"
            "       The family's packages pin one another exactly, so two versions of one of\n"
            "       them cannot be linked into one program. Either select the platform release\n"
            "       of that version ([target.<selector>] platform = \"{}@<version>\"), or write\n"
            "       {} = \"{}\" where it is requested.",
            n.shortName, pm.package.version, P.display(), n.shortName, it->second,
            by.empty() ? std::string("the graph") : by,
            P.package.substr(P.package.find('.') + 1), n.shortName, it->second));
    }
    return {};
}

// A WORKSPACE'S MEMBERS STAND ON THE PLATFORM'S LAYERS AS A ROOT DOES (§4.3).
// The packages the platform added are the virtual root's dependencies, and a
// member links its own closure; so each member of the plan is given an edge to
// each layer provider the platform added, as if it had declared it. (Measured:
// a build programs' graph under a host platform linked its programs without
// the C library and the C++ run-time, and `okld check` refused `free`.)
void step4b_platform_member_edges(PrepareState& state) {
    if (!state.platform.active || !state.workspacePlan() || state.platformLayerSpecs.empty()) return;
    // The packages of the plan by short name, so a member's edge to a layer
    // provider is a lookup.
    std::map<std::string, std::vector<std::size_t>, std::less<>> byName;
    for (std::size_t l = 1; l < state.packages.size(); ++l) {
        auto const& pm = state.packages[l].manifest;
        byName[mcpp::pm::compat::resolve_package_name(pm.package.name, pm.package.namespace_)
                   .shortName].push_back(l);
    }
    auto give = [&](std::size_t member, const std::string& key, const mcpp::manifest::DependencySpec& spec) {
        auto& deps = state.packages[member].manifest.dependencies;
        if (deps.contains(key)) return;
        deps[key] = spec;
        auto it = byName.find(spec.shortName);
        if (it == byName.end()) return;
        for (auto const l : it->second) {
            DependencyEdge e;
            e.consumerPackageIndex = member;
            e.dependencyPackageIndex = l;
            e.requestedFeatures = spec.features;
            e.defaultFeatures = spec.defaultFeatures;
            state.dependencyEdges.push_back(std::move(e));
        }
    };
    for (std::size_t m = 1; m < state.packages.size(); ++m) {
        if (!state.packages[m].selectedMember) continue;
        for (auto const& [key, spec] : state.platformLayerSpecs) give(m, key, spec);
    }
}

void step5_platform_toolchain(PrepareState& state) {
    auto const& P = state.platform;
    if (!P.active || !P.fields.toolchain.set) return;
    // The platform's toolchain stands where the target row's pin stands: it
    // replaces a default, never a toolchain the project or this machine named
    // for the target (§4.5), and the graph supplying the system does not
    // cancel it -- the platform is what supplies it.
    if (tc_origin_is_user_explicit(state.tcOrigin)) return;
    state.targetPinCandidate = P.fields.toolchain.value;
    state.targetPinIsCapability = true;
    state.platformToolchainPin = true;
}

} // namespace mcpp::build

namespace mcpp::build {

namespace {

// A program a platform names, as a runner's program is found: the programs
// the graph built for this machine, then the declared payloads, then PATH.
std::optional<fs::path> locate_program(const PrepareState& state, const BuildContext& ctx,
                                       const std::string& name) {
    if (auto it = state.rootTools.find(name); it != state.rootTools.end()) return it->second;
    for (auto const& dir : ctx.xlingsDepBinDirs)
        for (auto const& n : { name, name + ".exe" }) {
            std::error_code ec;
            if (fs::is_regular_file(dir / n, ec)) return dir / n;
        }
    return mcpp::invocation::which(name);
}

std::vector<std::string> split_words(std::string_view v) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : v) {
        if (c == ' ' || c == '\t') { if (!cur.empty()) out.push_back(std::exchange(cur, {})); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

} // namespace

void step13_platform_statement(PrepareState& state) {
    auto& P = state.platform;
    const auto t = target_of(state);
    const auto cfgCtx = platform_cfg_context(state);

    // ── The statement every reader uses: platform → build programs → this
    // machine → the project → the environment (§4.5) ──────────────────────
    mp::Statement s = P.active ? state.platformData : mp::Statement{};
    mp::Statement program = state.programPlatform;
    // The root's own program states them as well (applied into its manifest);
    // it is the last program to run, and it speaks for the project.
    mcpp::platform::merge(program, state.m->buildConfig.programPlatform);
    // A build program's runner and named runners (`mcpp::runner`), the BSP
    // spelling of the same fields.
    if (!state.m->buildConfig.runner.empty()) {
        program.runner.cmd = state.m->buildConfig.runner;
        program.runner.set = true;
        program.runner.origin = mp::Origin{ mp::Tier::Program, {}, 0,
            state.runnerProvider.empty() ? state.m->package.name : state.runnerProvider };
        if (state.m->buildConfig.runExclusive) {
            program.runner.exclusive = true;
            program.runner.exclusiveSet = true;
        }
    }
    for (auto const& [name, nr] : state.m->buildConfig.namedRunners) {
        auto it = state.namedRunnerProvider.find(name);
        program.runners[name] = mp::Words{ nr.argv, mp::Origin{ mp::Tier::Program, {}, 0,
            it == state.namedRunnerProvider.end() ? state.m->package.name : it->second }, true };
    }
    mcpp::platform::merge(s, program);
    std::vector<mp::Override> config;
    if (auto cfg = state.get_cfg(false); cfg) config = (*cfg)->platformOverrides;
    for (auto const* o : mcpp::platform::matching(config, matcher(cfgCtx))) mcpp::platform::merge(s, o->fields);
    auto project = project_tables(*state.m);
    for (auto const* o : mcpp::platform::matching(project, matcher(cfgCtx))) mcpp::platform::merge(s, o->fields);
    const auto envKey = target_env_name(t, "RUNNER");
    if (auto e = mcpp::invocation::get(envKey); e && !e->empty()) {
        if (*e == "none") {
            s.runner = mp::Runner{};
        } else {
            s.runner.cmd = split_words(*e);
            s.runner.origin = mp::Origin{ mp::Tier::Env, {}, 0, envKey };
            s.runner.set = !s.runner.cmd.empty();
        }
    }
    P.fields = std::move(s);
    P.flags = mcpp::platform::evaluate_flags(P.fields, matcher(cfgCtx));
}

std::expected<void, std::string> step13_finish_platform(PrepareState& state, BuildContext& ctx) {
    auto P = state.platform;
    const auto t = target_of(state);

    // ── The programs the link and the seal run (§4.4, §4.6) ───────────────
    if (P.active) {
        std::set<std::string> needed;
        for (auto const& in : P.fields.link.inputs)
            if (!in.from.empty()) needed.insert(in.from.front());
        for (auto const& step : P.fields.link.seal)
            if (!step.empty()) needed.insert(step.front());
        for (auto const& [name, e] : P.fields.tools)
            if (auto found = locate_program(state, ctx, e.ref.program.empty() ? name : e.ref.program))
                P.tools[name] = found->generic_string();
        const bool linksSomething = std::ranges::any_of(ctx.plan.linkUnits, [](auto const& lu) {
            return lu.kind != mcpp::build::LinkUnit::StaticLibrary;
        });
        for (auto const& name : needed) {
            if (P.tools.contains(name)) continue;
            if (auto found = locate_program(state, ctx, name)) {
                P.tools[name] = found->generic_string();
                continue;
            }
            if (!linksSomething || state.overrides.plan_only) continue;
            refusal::record(refusal::Code::PlatformToolAbsent);
            auto it = P.fields.tools.find(name);
            const std::string from = it == P.fields.tools.end()
                ? std::string("the platform names no package that builds it")
                : std::format("{} builds it ({})", mp::display(it->second.ref),
                              mcpp::platform::origin_text(it->second.origin));
            return std::unexpected(std::format(
                "the platform {} links {} with `{}`, and it is neither built by the graph nor found "
                "on this machine; {}.\n"
                "       Name the package that builds it for this machine:\n"
                "\n"
                "           [target.'cfg(platform = \"{}\")'.tools]\n"
                "           {} = \"<package>@<version>\"      # or {{ path = \"<dir>\" }}, {{ program = \"<name>\" }}",
                P.display(), t.str(), name, from, mp::platform_name(state.platformPackageShortName),
                name));
        }
    }
    // A platform that links with ld.lld needs one beside the compiler; its
    // absence is said here, not as a link line that starts with a flag.
    if (P.active && P.fields.link.driver.value == "lld" && !state.overrides.plan_only
        && std::ranges::any_of(ctx.plan.linkUnits, [](auto const& lu) {
               return lu.kind != mcpp::build::LinkUnit::StaticLibrary; })
        && mcpp::freestanding::resolve_lld_any(state.tc->binaryPath).empty()) {
        refusal::record(refusal::Code::LldRequiredAbsent);
        return std::unexpected(std::format(
            "the platform {} links {} with ld.lld itself ({}), and the toolchain has no ld.lld "
            "beside {}.\n"
            "       Use a toolchain whose payload carries it -- the platform's own is {} -- with "
            "`[target.<selector>] toolchain`, or remove the line that names this one.",
            P.display(), t.str(), mcpp::platform::origin_text(P.fields.link.driver.origin),
            state.tc->binaryPath.generic_string(),
            P.fields.toolchain.set ? P.fields.toolchain.value : std::string("an llvm toolchain")));
    }
    // THE PLATFORM, REPORTED (SPEC-013 §9): which one and why, then what it
    // contributes beyond the layers the Target lines already showed.
    if (P.active) {
        mcpp::ui::info("Platform", std::format("{} ({}{})", P.display(),
                       mcpp::platform::origin_text(P.selection.origin),
                       P.selection.hosted ? ", hosted" : ""));
        for (auto const& [label, value] : mcpp::platform::report_rows(P))
            if (!mp::is_layer(label))
                mcpp::ui::info("", std::format("{:<17} {}", label, value));
    }
    ctx.plan.platform = std::move(P);
    // THE HOST GRAPH'S, when the build programs were built under it (§6.1).
    ctx.hostPlatform = state.hostPlatform;
    if (ctx.hostPlatform.active)
        mcpp::ui::info("Host", std::format("platform {} ({}): build programs are {} images run by {}",
            ctx.hostPlatform.display(), mcpp::platform::origin_text(ctx.hostPlatform.selection.origin),
            state.hostPlatformTarget,
            state.hostPlatformRunner.empty() ? std::string("its runner")
                : std::filesystem::path(state.hostPlatformRunner.front()).filename().string()));
    if (state.overrides.configuration_only) {
        if (ctx.hostPlatform.active) {
            ctx.hostTriple = state.hostPlatformTarget;
            ctx.hostCompiler = ctx.hostPlatform.fields.toolchain.value;
            ctx.hostCompilerOrigin = "platform";
        } else if (auto h = state.host_tc_for_build_program()) {
            ctx.hostTriple = h->second.targetTriple;
            ctx.hostCompiler = h->second.binaryPath.generic_string();
            ctx.hostCompilerOrigin = "host";
        } else {
            return std::unexpected(h.error());
        }
    }
    return {};
}

} // namespace mcpp::build

namespace mcpp::build {

// THE HOST GRAPH'S PLATFORM, LOADED (SPEC-013 §6): the package, and the
// target among its own that has this machine's architecture -- what the build
// programs are compiled for and run as. Read when the build programs are
// built; the package is fetched as any dependency is.
std::expected<void, std::string> load_host_platform(PrepareState& state) {
    auto& H = state.hostPlatform;
    auto const& sel = state.hostPlatformSel;
    if (sel.none || H.active) return {};
    const auto& ref = sel.ref;
    fs::path root;
    std::optional<mcpp::manifest::Manifest> m;
    if (!ref.spec.path.empty()) {
        root = ref.spec.path;
        auto r = mcpp::manifest::load(root / "mcpp.toml");
        if (!r) return std::unexpected(std::format("{}: the host platform {}: {}",
            mcpp::platform::origin_text(sel.origin), mp::display(ref), r.error().format()));
        m = std::move(*r);
    } else if (!ref.spec.version.empty()) {
        if (!state.loadVersionDep)
            return std::unexpected("the host platform is read once the graph's loader exists");
        auto r = state.loadVersionDep(ref.name, ref.spec.namespace_, ref.spec.shortName, ref.spec.version);
        if (!r) return std::unexpected(std::format("{}: the host platform {}: {}",
            mcpp::platform::origin_text(sel.origin), mp::display(ref), r.error()));
        root = r->first;
        m = std::move(r->second);
    } else {
        return std::unexpected(std::format(
            "{}: the host platform {} is named by a version or a directory",
            mcpp::platform::origin_text(sel.origin), mp::display(ref)));
    }
    if (!m->platformDeclared) {
        refusal::record(refusal::Code::PlatformNotAPlatform);
        return std::unexpected(std::format(
            "{} is selected as the host graph's platform ({}), and its mcpp.toml has no "
            "[platform] section", qualified(*m), mcpp::platform::origin_text(sel.origin)));
    }
    const std::string arch(mcpp::toolchain::platform::host_arch);
    for (auto const& w : m->platform.targets.value) {
        auto t = mcpp::toolchain::triple::parse(w);
        if (t && t->arch == arch) { state.hostPlatformTarget = t->str(); break; }
    }
    if (state.hostPlatformTarget.empty()) {
        refusal::record(refusal::Code::PlatformTargetUnserved);
        return std::unexpected(std::format(
            "the host graph's platform {} ({}) serves no {} target, so this machine's build "
            "programs cannot be built for it; its targets are {}",
            qualified(*m), mcpp::platform::origin_text(sel.origin), arch,
            [&] { std::string l; for (auto const& w : m->platform.targets.value)
                      l += (l.empty() ? "" : ", ") + w; return l; }()));
    }
    H.active = true;
    H.selection = sel;
    H.package = qualified(*m);
    H.version = m->package.version;
    H.root = root;
    H.fields = m->platform;
    return {};
}

// The spelling a sub-build is told the host platform by (`platform_choice`).
std::string host_platform_choice(const PrepareState& state) {
    auto const& r = state.hostPlatformSel.ref;
    if (!r.spec.path.empty()) return "path:" + r.spec.path;
    return std::format("{}.{}@{}", r.spec.namespace_.empty() ? std::string(mcpp::pm::kDefaultNamespace)
                                                             : r.spec.namespace_,
                       r.spec.shortName, r.spec.version);
}

} // namespace mcpp::build
