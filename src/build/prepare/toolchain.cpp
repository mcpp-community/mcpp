// toolchain.cpp -- P1: the toolchain specification and the target axis, and
// the run-wide keys a plan's root states. P2, the toolchain resolver P5 calls
// once the dependency graph exists, is toolchain_resolver.cpp.

module mcpp.build.prepare;
import :state;

import mcpp.build.prepare_inputs;

import std;
import mcpp.build.engine_package;
import mcpp.diag;
import mcpp.build.refusal;
import mcpp.build.version_floor;
import mcpplibs.os.platform;
import mcpplibs.os.env;
import mcpp.toolchain.platform;
import mcpp.toolchain.apple_sdk;
import mcpp.effects;
import mcpp.build.command_line;
import mcpp.manifest;
import mcpp.source_kind;
import mcpp.toolchain.hostflags;   // the compile-token producer the package std module reuses
import mcpp.toolchain.detect;
import mcpp.toolchain.dialect;
import mcpp.toolchain.fingerprint;
import mcpp.toolchain.msvc;
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
import mcpp.toolchain.target_schema;
import mcpp.toolchain.target_select;
import mcpp.toolchain.target_rows;
import mcpp.toolchain.base;
import mcpp.toolchain.base_probe;
import mcpp.home;
import mcpp.build.plan;
import mcpp.build.flags;          // compute_flags — the per-role contracts (#418)
import mcpp.build.graph_shape;  // #407: the graph says which mode wrote it
import mcpp.pack.abi_tag;      // the tag a prebuilt dependency is checked against
import mcpp.pack.prebuilt;     // …and the check itself
import mcpp.pack.stage_tree;   // where `${mcpp.stage_dir}` points, and its manifest
import mcpp.build.build_program;
import mcpp.build.backend;      // BuildOptions for the tool sub-build
import mcpp.build.ninja;        // make_ninja_backend — driving that sub-build
import mcpp.config;
import mcpp.provider;
import mcpp.provider.select;
import mcpp.provider.runtime_selection;
import mcpp.runtime.binding;
import mcpp.toolchain.post_install;
import mcpp.fetcher;
import mcpp.fetcher.progress;
import mcpp.ui;
import mcpp.invocation;
import mcpp.version_req;
import mcpp.run;

namespace mcpp::build {

static void resolve_host_tools(PrepareState& state);
static void state_project_keys(PrepareState& state);

// D11's sentence: which spec, replacing what, declared where. The key is the
// platform entry `for_platform` answered with (`[toolchain].linux`, else
// `.default`), and the file is the manifest that wrote it -- the workspace
// root's, for an entry a member received from `[workspace.toolchain]`.
static void warn_toolchain_override(const std::string& given, const std::string& declared,
                                    const mcpp::manifest::Toolchain& tc,
                                    std::string_view platform,
                                    const std::filesystem::path& manifest) {
    const std::string key = tc.byPlatform.contains(std::string(platform))
        ? std::string(platform) : std::string("default");
    auto file = manifest;
    if (auto it = tc.fileByPlatform.find(key); it != tc.fileByPlatform.end()) file = it->second;
    std::error_code ec;
    auto shown = std::filesystem::relative(file, std::filesystem::current_path(), ec);
    if (ec || shown.empty() || *shown.begin() == "..") shown = file;
    mcpp::diag::warning("toolchain/override", std::format(
        "--toolchain {} replaces {} declared at [toolchain].{} ({})",
        given, declared, key, shown.generic_string()));
}

// STEP FUNCTIONS (mcpp#722 / T6), split at the points where phase1's
// own banners mark a new concern: the closures phase1 assigns onto
// `state` (each captures only `state`), the target/static override
// resolution, and the device axis plus the L1 conditional-section merge.

static std::expected<void, std::string> step1_define_early_toolchain_closures(PrepareState& state) {
    // ─── Toolchain resolution (docs/21) ────────────────────────────────
    //
    // THE WHOLE CHAIN, in the order it is applied. It was documented twice, as
    // "3 steps" here and "4 steps" further down, and neither list had been
    // true for a long time — between them they named five of the nine inputs
    // below and disagreed about two. A comment that undercounts the inputs to
    // a decision is worse than none: it tells the next reader they have seen
    // the whole thing.
    //
    // The WHAT (which spec) is settled first, then the HOW (which binary).
    // Anything that WRITES `tcSpec` also writes `tcOrigin`, and that is the
    // invariant this table rests on — the enumerator names below are real, so
    // this comment cannot quietly stop matching the code.
    //
    //   WHICH SPEC                                       tcOrigin
    //   1. mcpp.toml [toolchain].<platform> / .default   ManifestToolchain
    //   2. global config.toml [toolchain] default        GlobalDefault
    //   3. mcpp.toml [target.<triple>].toolchain         TargetSection
    //      (--target / [build] target / config default
    //       select the section; 3 outranks 1 and 2)
    //   4. the target vocabulary's pin (triple.cppm)     TargetPin
    //      — a convention, and it stands down when a
    //        REMEMBERED target would overrule a spec the
    //        user wrote down
    //   5. the platform's first-run default, installed   FirstRun
    //      and persisted by this very invocation
    //
    //   WHICH BINARY, from the spec settled above
    //   6. `msvc@system`  → probe the machine (no xim package exists)
    //   7. `<family>@<v>` → xim payload; msvc resolves through
    //                       resolve_managed_msvc, everything else through
    //                       the bin/-shaped frontend lookup
    //   8. bare `system`  → the PATH compiler. A deliberate escape hatch, and
    //                       the ONLY host-compiler route: there is no
    //                       `gcc@system` (see parse_toolchain_spec)
    //   9. offline / MCPP_NO_AUTO_INSTALL → hard error rather than a silent
    //                       ~800 MB download
    //
    // AND ONE REVISION, after 1-9 have produced a toolchain: a spec targeting
    // the MSVC ABI on a machine with no usable MSVC is switched to MinGW-w64
    // — but only when `tc_origin_is_user_explicit` says mcpp chose it itself.
    state.bootstrap_checked = false;
    // Before the configuration's bootstrap, the first reader of them.
    state_project_keys(state);
    state.get_cfg = [&](bool requireBootstrap = true) -> std::expected<mcpp::config::GlobalConfig*, std::string> {
        if (!state.cfg_opt) {
            auto c = mcpp::config::load_or_init(/*quiet=*/false,
                mcpp::fetcher::make_bootstrap_progress_callback(), {},
                state.overrides.describes() ? mcpp::config::Initialization::ReadOnly
                                          : mcpp::config::Initialization::Bootstrap);
            if (!c) return std::unexpected(c.error().message);
            state.cfg_opt = std::move(*c);
        }
        // Execution requires the bootstrap tools. A planning pass reads the
        // installed toolchain without requiring an initialized provider or
        // installing tools it will not execute.
        if (requireBootstrap && !state.overrides.describes() && !state.bootstrap_checked) {
            state.bootstrap_checked = true;
            resolve_host_tools(state);
            auto problem = mcpp::config::check_base_init(*state.cfg_opt);
            if (!problem.empty()) {
                return std::unexpected(std::format(
                    "{}\n  help: run `mcpp self init --force` to reset and re-initialize",
                    problem));
            }
        }
        return &*state.cfg_opt;
    };

    // Resolve one exact runtime contract before resolving/fixing a toolchain.
    // The fixup is itself a consumer of RuntimeBinding: doing it first would
    // recreate #392 by letting directory order choose a libc and only later
    // discovering what the project selected.
    if (state.overrides.inherited_runtime_binding) {
        state.runtimeBindingSnapshot = *state.overrides.inherited_runtime_binding;
    } else {
        auto cfgRuntime = state.get_cfg(true);
        if (!cfgRuntime) return std::unexpected(cfgRuntime.error());
        auto resolved = mcpp::runtime::resolve_runtime_binding(
            state.runtimeSelection, {}, (**cfgRuntime).xlingsHome());
        if (!resolved) return std::unexpected(resolved.error());
        state.runtimeBindingSnapshot = std::move(*resolved);
        // A degradation that nobody prints is indistinguishable from no
        // degradation, which is the failure this whole area keeps paying for.
        // A note is not a warning: nothing is wrong with the build, some facts
        // are simply unavailable — so it is reported once, at info level.
        if (!state.runtimeBindingSnapshot.note.empty())
            mcpp::ui::info("Runtime", state.runtimeBindingSnapshot.note);
    }
    // THE `bin` THIS PROJECT'S BUILD PROGRAMS SEE FIRST — derived ONCE,
    // here, from the selection that has just been resolved.
    //
    // Empty unless the manifest declared `[xlings].subos`. That is deliberate:
    // prepending the SHARED `subos/default/bin` would make what a build sees
    // depend on what else has been installed on this machine, so a project
    // that has not asked for an environment of its own gets the `PATH` mcpp
    // was started with, byte for byte.
    //
    // NOT RE-DERIVED AT THE TWO DELIVERY SITES BELOW, AND NOT FROM
    // `[xlings] deps`. `mcpp::provider::runtime` is the sole runtime-selection
    // policy and `RuntimeBinding::subosDir` is its resolved answer; a second
    // derivation is how a build ends up with two subos and no way to say which
    // one it used. The per-package payload paths a program may also need are
    // already answered, separately, by `MCPP_XPKG_*_DIR`.
    state.projectSubosBin = [&]() -> std::string {
        using Mode = mcpp::provider::runtime::RuntimeSelection::Mode;
        if (state.runtimeBindingSnapshot.selection.mode != Mode::NamedSubos)
            return {};
        auto bin = state.runtimeBindingSnapshot.subosDir / "bin";
        std::error_code ec;
        if (!std::filesystem::is_directory(bin, ec)) return {};
        return bin.string();
    }();

    state.runtimePayload = state.runtimeBindingSnapshot.libc;
    state.runtimeLibDir = state.runtimeBindingSnapshot.libraryDirs.empty()
        ? std::filesystem::path{} : state.runtimeBindingSnapshot.libraryDirs.front();

    // THE DECLARED RUNTIME PAYLOAD IS PROVIDED WITH THE TOOLCHAIN THAT RUNS ON
    // IT (mcpp#660), and not earlier: a build whose toolchain needs no C
    // runtime payload must not download one. At most once per build. An
    // inherited binding is not exempt, because the parent build may have used
    // a toolchain that needed no payload. The two values derived above are
    // refreshed with the binding, because detection and the fingerprint read
    // them afterwards.
    state.runtimePayloadProvided = false;
    state.provide_runtime_payload = [&](const mcpp::toolchain::XimToolchainPackage& pkg,
                                        const std::filesystem::path& payloadRoot)
        -> std::expected<void, std::string> {
        if (state.runtimePayloadProvided) return {};
        if (mcpp::toolchain::runtime_payload_kind(pkg, payloadRoot).empty()) return {};
        state.runtimePayloadProvided = true;
        auto cfgP = state.get_cfg(true);
        if (!cfgP) return {};
        auto changed = mcpp::toolchain::ensure_declared_runtime(**cfgP, state.runtimeBindingSnapshot);
        if (!changed) return std::unexpected(changed.error());
        if (!*changed) return {};
        state.runtimePayload = state.runtimeBindingSnapshot.libc;
        state.runtimeLibDir = state.runtimeBindingSnapshot.libraryDirs.empty()
            ? std::filesystem::path{} : state.runtimeBindingSnapshot.libraryDirs.front();
        return {};
    };



    // Toolchain resolution priority: see the table at the top of this
    // function. Stated once, where `tcOrigin` is introduced — this used to be
    // a second, shorter and differently-wrong list of the same thing.
    //
    // Resolve the build profile, overlaid by any [profile.<name>] from the
    // manifest → buildConfig. `effectiveProfile` outlives the block: the
    // build.mcpp env contract exposes it as MCPP_PROFILE.
    {
        auto& pname = state.effectiveProfile;
        // Precedence lives in resolve_profile_name (above) so execute.cppm's
        // fast paths settle it identically without running prepare_build.
        // Release is opt-in via --release / --profile release; a project that
        // wants its plain `mcpp build` optimized sets
        // [build].default-profile = "release" (mcpp's own mcpp.toml does this,
        // so the released binary stays -O2).
        pname = resolve_profile_name(*state.m, state.overrides.profile, state.overrides.profile_fallback);
        mcpp::manifest::Profile pr;
        if (pname == "dev" || pname == "debug") { pr.optLevel = "0"; pr.debug = true; }
        else if (pname == "dist")               { pr.optLevel = "3"; pr.strip = true; }
        // (built-in dist intentionally leaves lto off: several packaged gcc
        //  payloads ship without the LTO plugin; enable via [profile.dist].)
        else                                    { pr.optLevel = "2"; } // release
        if (auto it = state.m->profiles.find(pname); it != state.m->profiles.end()) pr = it->second;
        // #519 — a profile may override the whole-graph form. OPTIONAL, so a
        // profile that does not mention it leaves `[build]` standing; a plain
        // value would reset it, because the block above REPLACES `pr` wholesale
        // with the declared profile.
        if (pr.dependencyLinkageDeclared)
            state.m->buildConfig.dependencyLinkage = pr.dependencyLinkage;
        // `mcpp pack --linkage`: the package's program property, stated for
        // this build over the project's (SPEC-014 §1).
        if (!state.overrides.dependency_linkage.empty())
            state.m->buildConfig.dependencyLinkage = state.overrides.dependency_linkage;
        state.m->buildConfig.optLevel = pr.optLevel;
        if (!pr.debugWritten.empty() && pr.debugWritten != "lines")
            return std::unexpected(std::format(
                "[profile.{}] debug = \"{}\": expected true, false or \"lines\"", pname, pr.debugWritten));
        state.m->buildConfig.debug    = pr.debug;
        state.m->buildConfig.debugLines = pr.debugLines;
        // THE DEPENDENCIES' DEBUG INFORMATION (design 2026-11-20 §5.15): the packages from the
        // store and git get the line tables under the profiles a developer debugs, unless the
        // profile states otherwise. Their debug information made 8.5 of a test program's 11.5 MB
        // and 15.4 of a workspace's 22 GB (mcpp#927); a backtrace through them keeps its lines.
        {
            if (pr.dependencyDebugDeclared && pr.dependencyDebug != "true" && pr.dependencyDebug != "false"
                && pr.dependencyDebug != "lines")
                return std::unexpected(std::format(
                    "[profile.{}] dependency_debug = \"{}\": expected true, false or \"lines\"", pname,
                    pr.dependencyDebug));
            const std::string graph = pr.debug ? (pr.debugLines ? "lines" : "true") : "false";
            const std::string wanted = pr.dependencyDebugDeclared                  ? pr.dependencyDebug
                                       : (pname == "dev" || pname == "debug") && pr.debug ? std::string("lines")
                                                                                          : graph;
            state.m->buildConfig.dependencyDebug = wanted == graph ? std::string{} : wanted;
        }
        state.m->buildConfig.lto      = pr.lto;
        state.m->buildConfig.strip    = pr.strip;
        // `ndebug` and `trim_paths` follow the industry's convention for a
        // profile that does not state them (design 2026-10-23 §15.3-§15.4):
        // on for the profiles that produce what is shipped, off for the ones
        // a developer debugs. A custom profile states what it wants.
        const bool shipping = pname == "release" || pname == "dist";
        state.m->buildConfig.ndebug    = pr.ndebugDeclared ? pr.ndebug : shipping;
        // THE SAME FOR THE TEST SURFACE, BUT FOR THE TESTS' OWN FILES. A test
        // binary runs where it was built and commonly finds its fixtures
        // through `__FILE__` -- measured on mcpp's own unit tests under its
        // release default, which then looked for `workspace@0.0.0/src` -- so
        // a test program's own files keep their paths (`CompileUnit::
        // testUnit`). The code under test is trimmed as `mcpp build` trims
        // it: it is the code that ships, and `mcpp build` and `mcpp test`
        // are one configuration, so switching between them compiles nothing
        // again (before 2026.11.8.1 a release test was a configuration of its
        // own, and compiled the whole graph again).
        state.m->buildConfig.trimPaths = pr.trimPathsDeclared ? pr.trimPaths : shipping;
        // `bmi_cutoff` (mcpp#946 O2, SPEC-018): a GCC BMI whose line maps alone
        // moved is kept, unless the profile or the environment states `exact`.
        // The environment is the override of one run.
        {
            auto cutoff = mcpp::invocation::get("MCPP_BMI_CUTOFF").value_or(
                pr.bmiCutoffDeclared ? pr.bmiCutoff : std::string("ignore-locations"));
            if (cutoff != "ignore-locations" && cutoff != "exact")
                return std::unexpected(std::format(
                    "bmi_cutoff = \"{}\" ([profile.{}] or MCPP_BMI_CUTOFF): expected \"ignore-locations\" or \"exact\"",
                    cutoff, pname));
            // What is packed with debug information names the lines of its
            // sources: exact, whatever the profile states.
            state.m->buildConfig.bmiIgnoreLocations =
                cutoff == "ignore-locations" && !(state.overrides.distributes && pr.debug);
        }
        // `split_debuginfo` (design 2026-10-31 §4.1 (b)): unpacked for the
        // profiles a developer debugs. A debugger finds a unit's `.dwo` through
        // the build directory the skeleton names, which `trim_paths` replaces.
        if (pr.splitDebuginfoDeclared && pr.splitDebuginfo != "unpacked" && pr.splitDebuginfo != "off")
            return std::unexpected(std::format(
                "[profile.{}] split_debuginfo = \"{}\": expected \"unpacked\" or \"off\"",
                pname, pr.splitDebuginfo));
        const bool unpacked = pr.splitDebuginfoDeclared ? pr.splitDebuginfo == "unpacked"
                                                        : (pname == "dev" || pname == "debug");
        state.m->buildConfig.splitDebuginfo = unpacked && pr.debug && !state.m->buildConfig.trimPaths;
        // SAID ONCE PER HOME, the first time a shipping profile takes the new
        // defaults without stating them: the artifact changed, and the line
        // that changes it back is one key.
        if (shipping && !(pr.ndebugDeclared && pr.trimPathsDeclared) && !state.overrides.describes()
            && state.overrides.tool_depth == 0 && !state.overrides.host_target) {
            std::error_code ec;
            const auto marker = mcpp::home::root() / "cache" / "notes" / "profile-ndebug-trim-paths";
            if (!std::filesystem::exists(marker, ec)) {
                mcpp::ui::info("note", std::format(
                    "the {} profile now defines NDEBUG and trims the source paths an artifact "
                    "carries (`app@1.2.0/src/main.cpp`). To keep `assert` or absolute paths, write "
                    "`ndebug = false` or `trim_paths = false` under [profile.{}]. Said once.",
                    pname, pname));
                std::filesystem::create_directories(marker.parent_path(), ec);
                std::ofstream(marker) << "2026.10.24.1\n";
            }
        }
        state.m->buildConfig.cflags.insert(state.m->buildConfig.cflags.end(),
                                     pr.cflags.begin(), pr.cflags.end());
        state.m->buildConfig.cxxflags.insert(state.m->buildConfig.cxxflags.end(),
                                       pr.cxxflags.begin(), pr.cxxflags.end());
        // A workspace plan's root compiles nothing; its selected members take
        // these as they are loaded (graph.cpp).
        state.profileCflags   = pr.cflags;
        state.profileCxxflags = pr.cxxflags;
        state.m->buildConfig.ldflags.insert(state.m->buildConfig.ldflags.end(),
                                      pr.ldflags.begin(), pr.ldflags.end());
        state.m->buildConfig.graphLdflags.insert(state.m->buildConfig.graphLdflags.end(),
                                           pr.ldflags.begin(), pr.ldflags.end());
        // The profile's graph-wide words, after `[build] dialect_cxxflags`: the
        // root's set is the one the standard library module, the scan and every
        // C++ unit read (mcpp#1025).
        state.m->buildConfig.dialectCxxflags.insert(state.m->buildConfig.dialectCxxflags.end(),
                                                    pr.dialectCxxflags.begin(), pr.dialectCxxflags.end());
    }

    // Every directory a package payload may legitimately have been INSTALLED
    // into: the global registry, plus the two project-local data roots a custom
    // git index installs into. Defined HERE, above its first use, because three
    // separate questions now depend on the same answer — where a dependency's
    // cache address is anchored, whether its sources came from a store at all,
    // and whether a `standard` declaration in its manifest was written by an
    // author or by a descriptor generator. One definition, three uses; deriving
    // the same fact twice is how two of them start disagreeing.
    state.storeRoots = [&]() -> std::vector<std::filesystem::path> {
        std::vector<std::filesystem::path> roots;
        auto c = state.get_cfg(true);
        if (c) for (auto& s : mcpp::provider::open(mcpp::config::make_xlings_env(**c))->stores()) roots.push_back(s);
        auto project = [&](const std::filesystem::path& dir) {
            for (auto& s : mcpp::provider::layout().stores_of(dir / ".mcpp"))
                roots.push_back(s);
            for (auto& d : mcpp::config::project_xlings_data_roots(dir))
                roots.push_back(mcpp::provider::layout().store_in(d));
        };
        project(state.workRoot);
        if (!state.overrides.project_index_root.empty()) project(state.overrides.project_index_root);
        // The engine interface's package: written once per mcpp version and
        // never edited, like a payload (mcpp#797).
        roots.push_back(mcpp::build::engine::root());
        return roots;
    }();

    // [package] platforms — fixed vocabulary owned by mcpp (it owns the
    // target/triple system): the platform name of every row it has
    // (`platform_name`, beside `artifact_naming`). Unknown values: warning, or
    // error under --strict.
    for (auto& pf : state.m->package.platforms) {
        if (!mcpp::toolchain::triple::is_platform_name(pf)) {
            auto msg = std::format(
                "[package] platforms contains unknown platform '{}' "
                "(expected: {})", pf,
                mcpp::toolchain::triple::platform_names_joined());
            if (state.overrides.strict) return std::unexpected(msg);
            mcpp::diag::warning("manifest/platforms", msg);
        }
    }

    state.tcSpec = state.m->toolchain.for_platform(kCurrentPlatform);
    // Where the spec came from decides whether mcpp may later revise it.
    // See TcOrigin: mcpp can rewrite a default it chose itself, but must not
    // silently overrule one the user wrote down.
    state.tcOrigin = state.tcSpec.has_value() ? TcOrigin::ManifestToolchain
                                       : TcOrigin::None;
    // `--toolchain` shares `ManifestToolchain`'s precedence and not its
    // spelling: the messages that refuse a spec name where it was written, and
    // a value from the command line credited to a manifest key sends the
    // reader to a file that does not contain it.
    state.tcFromCommandLine = false;
    state.tcFromConsumer    = false;
    state.tcSpecSource = [&]() -> std::string {
        if (state.tcOrigin == TcOrigin::ManifestToolchain && state.tcFromConsumer)
            return std::format("the toolchain chosen for this host tool by {}",
                               state.overrides.tool_chain);
        if (state.tcOrigin == TcOrigin::ManifestToolchain && state.tcFromCommandLine)
            return "--toolchain";
        switch (state.tcOrigin) {
            case TcOrigin::ManifestToolchain:
                return std::format("[toolchain].{}", kCurrentPlatform);
            case TcOrigin::TargetSection:
                return std::format("[target.{}].toolchain", state.overrides.target_triple);
            case TcOrigin::GlobalDefault:
                return "the default toolchain (`mcpp toolchain default`)";
            default:
                return std::format("the toolchain mcpp chose ({})",
                                   tc_origin_name(state.tcOrigin));
        }
    };
    // `--toolchain` (arriving as MCPP_TOOLCHAIN, the same side channel
    // `--offline` and `--jobs` use) beats everything, including the manifest.
    //
    // This is the usable form of "which compiler". On this repository the
    // choice is worth 2.48x — gcc@16.1.0 builds mcpp in 79.9s, llvm@22.1.8 in
    // 32.2s — but CHANGING THE DEFAULT is an ecosystem decision, not a
    // performance one: it invalidates every published package's fingerprint and
    // the three platforms do not yet ship the same llvm. Selecting per build
    // costs nobody anything and needs no coordination.
    //
    // It counts as user-explicit, so mcpp will not quietly revise it.
    // What the manifest declared, before the command line replaces it: D11
    // says so when the two differ.
    const auto manifestSpec = state.tcSpec;
    if (auto tcEnv = mcpp::invocation::get("MCPP_TOOLCHAIN")) {
        state.tcSpec   = *tcEnv;
        state.tcOrigin = TcOrigin::ManifestToolchain;
        state.tcFromCommandLine = true;
    }
    if (!state.overrides.toolchain.empty()) {
        state.tcSpec   = state.overrides.toolchain;
        state.tcOrigin = TcOrigin::ManifestToolchain;
        state.tcFromConsumer = true;
    }
    // D11: `--toolchain` replacing a toolchain the manifest declared is done
    // as asked, and said, so a build that ignores `[toolchain]` is never a
    // surprise. A warning and not `degraded`: nothing was left undone, so
    // `--strict` does not fail on it. Not for a host tool's sub-build, whose
    // toolchain is its consumer's decision.
    if (state.tcFromCommandLine && !state.tcFromConsumer && manifestSpec
        && *manifestSpec != *state.tcSpec)
        warn_toolchain_override(*state.tcSpec, *manifestSpec,
            state.m->toolchain, kCurrentPlatform, state.m->sourcePath);
    if (!state.tcSpec.has_value()) {
        auto cfg = state.get_cfg(true);
        if (cfg && !(*cfg)->defaultToolchain.empty()) {
            state.tcSpec   = (*cfg)->defaultToolchain;
            state.tcOrigin = TcOrigin::GlobalDefault;
        }
    }
    // A toolchain named by path, the toolchain phase's statement, and the
    // bootstrap toolchain (mcpp#755). Rewrites `tcSpec` into the one spelling
    // every later reader parses: a managed spec, or `path:<absolute dir>`.
    if (auto r = step1_local_toolchain(state); !r) return std::unexpected(r.error());

    // `[target.<triple>]`'s build-shaping keys, applied the same way whichever
    // path found the row (#704). The section's toolchain is a statement about
    // this row the author wrote down, so it replaces `[toolchain]` and the
    // global default; `--toolchain` and a consumer's decision for a host tool
    // are statements about THIS invocation and keep precedence over it.
    return {};
}

// The closures that answer "is MSVC usable here" and "what would a native
// build pick", and the Windows first run that seeds the GNU target from them.
static void step1_define_host_default_closures(PrepareState& state) {
    // ─── Windows first run without Visual Studio ────────────────────────
    // The host triple on Windows is MSVC-ABI, so the historical default
    // (llvm) resolves to clang targeting MSVC — which uses the MSVC STL and
    // the Windows SDK. Neither ships with Windows; both arrive only with
    // Visual Studio's "Desktop development with C++" workload. On a bare box
    // that default installs fine and then fails at compile time with no
    // actionable message.
    //
    // Seed only the TARGET axis and let the block right below derive the
    // rest: the vocabulary table already maps x86_64-windows-gnu to its pin
    // (winlibs GCC) and to static linkage, so the toolchain answer stays a
    // single derivation instead of being spelled out a second time here.
    // "Is MSVC usable here" — either origin. Asking `has_usable_msvc()` (which
    // probes the machine) would answer "no" on a box that has a pinned
    // msvc@<toolset> payload and no Visual Studio, and every decision below
    // would then divert a perfectly good toolchain to mingw.
    state.msvc_usable_either_origin = [&]() -> bool {
        auto c = state.get_cfg(true);
        if (!c) return mcpp::toolchain::msvc::has_usable_msvc();
        return mcpp::toolchain::msvc::msvc_available_here(
            mcpp::provider::open(mcpp::config::make_xlings_env(**c))->store());
    };

    // THE PLATFORM'S CANONICAL NATIVE DEFAULT — a spec string only; no
    // install, no persistence. Two places need "what would a native
    // `mcpp build` pick here, with no --target": the first-run installer
    // further below (which goes on to install and persist it), and
    // `host_tc_for_build_program`'s cross branch (which needs a genuine HOST
    // compiler when nothing was ever recorded as one — see its own comment
    // for why #622 happened). One derivation, called from both, so they
    // cannot drift the way a hand-copied second copy would.
    // The host's answer is `pins::host_default_toolchain` (WS8): one function,
    // which `mcpp self env --format json` reports too. A machine with no
    // usable MSVC gets the GNU pin, not an MSVC-ABI clang it cannot use --
    // mirrors the windows-gnu seed below, which this function's other caller
    // runs after.
    state.native_first_run_spec = [&]() -> std::string {
        const bool msvcUsable = mcpplibs::os::is_windows
                             && state.msvc_usable_either_origin();
        return std::string(
            mcpp::toolchain::triple::pins::host_default_toolchain(msvcUsable));
    };

    state.windowsGnuFirstRun = false;
    if constexpr (mcpplibs::os::is_windows) {
        if (!state.tcSpec.has_value() && state.overrides.target_triple.empty()
            && state.m->buildConfig.target.empty()
            && !state.msvc_usable_either_origin()) {
            auto cfgW = state.get_cfg(true);
            if (!cfgW || (*cfgW)->defaultTarget.empty()) {
                state.overrides.target_triple =
                    std::string(mcpp::toolchain::triple::pins::kFirstRunNoMsvcTarget);
                state.windowsGnuFirstRun = true;
            }
        }
    }
}

// A phase-local struct passed by reference to the steps that resolve one
// `--target` / manifest-target request -- the same pattern WorklistItemCtx
// (graph.cpp) and HostToolCtx (features.cpp) use. Each field is a local the
// original single-function body declared once (inside its
// `if (!target_triple.empty())` block) and read again in a later part of the
// same request's resolution.
struct TargetOverrideCtx {
    std::string requestedSpelling;
    std::optional<mcpp::toolchain::triple::Triple> parsed;
    mcpp::toolchain::triple::RequestResolution req;
    mcpp::toolchain::target::Selection sel;
    // The `[target.<triple>]` section this request matched, or null. A
    // pointer rather than the map iterator the single-function version held,
    // since iterator validity is not this struct's business to reason about
    // and the callers only ever read `->second`.
    const mcpp::manifest::TargetEntry* sectionEntry = nullptr;
    bool hasExplicitSection = false;
    bool hasToolchainOverride = false;
    // The target came from the configuration's `default_target`, beside its
    // `[toolchain] default`: a pair the configuration states.
    bool targetFromGlobalDefault = false;
    const mcpp::toolchain::triple::TargetInfo* known = nullptr;
    // The platform the project, the command line or the machine chose for the
    // request (its short name), or empty.
    std::string chosenPlatform;
};

// The body of the `apply_target_section` closure the single-function version
// of this step captured. Used at two points -- an explicit `[target.<triple>]`
// section for the resolved request, and the host's own row on a build with no
// `--target` -- so it is a named helper rather than a per-call closure.
static void step1_apply_target_section(PrepareState& state,
                                        const mcpp::manifest::TargetEntry& e) {
    if (!e.toolchain.empty() && state.tcFromCommandLine && !state.tcFromConsumer
        && state.tcSpec && *state.tcSpec != e.toolchain)
        mcpp::diag::warning("toolchain/override", std::format(
            "--toolchain {} replaces {} declared at [target.{}].toolchain ({})",
            *state.tcSpec, e.toolchain, state.overrides.target_triple.empty()
                ? std::string(mcpp::toolchain::triple::host_triple().str())
                : state.overrides.target_triple,
            state.m->sourcePath.filename().string()));
    if (!e.toolchain.empty() && !state.tcFromCommandLine && !state.tcFromConsumer) {
        // A CROSS TARGET'S SECTION NAMES THE TARGET'S COMPILER, NOT THE HOST'S:
        // the spec it replaced is what the build program resolves, as a row's
        // pin's is (`[target.x86_64-linux-musl] toolchain = "gcc@16.1.0"` on a
        // Windows host whose `[toolchain] windows` is llvm).
        if (!state.overrides.target_triple.empty() && state.tcSpec && *state.tcSpec != e.toolchain)
            state.hostSpecBeforeRowPin = *state.tcSpec;
        state.tcSpec   = e.toolchain;
        state.tcOrigin = TcOrigin::TargetSection;
    }
    if (!e.linkage.empty()) state.m->buildConfig.linkage = e.linkage;
    // #336: a per-target C++ runtime contract overrides the project
    // default, so "self-contained everywhere except this triple" is
    // expressible without touching the cfg() input channel.
    if (!e.cxxRuntime.empty()) state.m->buildConfig.cxxRuntime = e.cxxRuntime;
}

static std::expected<void, std::string>
step1_resolve_target_triple_request(PrepareState& state, TargetOverrideCtx& ctx) {
    namespace triple = mcpp::toolchain::triple;
    // THE SPELLING THE PROJECT WROTE, KEPT FOR EVERY DIAGNOSTIC BELOW.
    // `state.overrides.target_triple` is canonicalised further down, and until
    // this variable existed the refusals quoted the canonical form:
    // `--target aarch64-linux` produced "target 'aarch64-linux-gnu' is
    // registered but not yet supported", a string the reader never typed
    // and cannot find in their own command.
    ctx.requestedSpelling = state.overrides.target_triple;
    define_early_platform(state);
    ctx.parsed = triple::parse(state.overrides.target_triple);

    // THE REQUEST IS COMPLETED FROM THE VOCABULARY BEFORE ANYTHING
    // READS IT, AND THE ORDER RELATIVE TO THE `[target.X]` LOOKUP IS PART
    // OF THE CONTRACT.
    //
    // `parse` fills a missing env segment lexically so the identity stays
    // total — `x86_64-linux` IS `x86_64-linux-gnu`, and a unit test says so.
    // Every gate below then asked about the filled value instead of about
    // the request. The selection (mcpp.toolchain.target_select) now answers it.
    //
    // The lookup that follows keys on `parsed->str()`, so completing after
    // it would match sections against a triple this build is not going to
    // use. A project wanting the `planned` row keeps its escape hatch by
    // WRITING the segment: `--target aarch64-linux-gnu` skips completion
    // entirely, because a written segment is a request rather than a gap.
    // THE SECTION THE REQUEST NAMES AS WRITTEN, before the selection: a
    // `[target.<request>] toolchain` is a declared family the selection keeps.
    const mcpp::manifest::TargetEntry* written = nullptr;
    if (auto w = state.m->targetOverrides.find(state.overrides.target_triple);
        w != state.m->targetOverrides.end())
        written = &w->second;

    // THE SELECTION (SPEC-016 §4). The request is a pattern; the candidates are
    // the rows it reaches with the providers that serve them, ordered by
    // priority, and the first available on this host is the identity.
    // Explicit choices win: a declared compiler family keeps the providers that
    // pair with it, and a provider chosen by name (`builtin:<name>`, or the
    // platform package a provider names) serves alone.
    if (ctx.parsed) {
        std::string family;
        auto declared = written && !written->toolchain.empty() ? std::optional<std::string>(written->toolchain)
                      : (state.tcSpec && (state.tcFromCommandLine || tc_origin_is_user_explicit(state.tcOrigin)))
                          ? state.tcSpec : std::nullopt;
        if (declared)
            if (auto spec = mcpp::toolchain::parse_toolchain_spec(*declared))
                family = std::string(mcpp::toolchain::family_name(spec->family));
        std::vector<mcpp::manifest::platform::Override> config;
        if (auto cfg = state.get_cfg(true); cfg) config = (*cfg)->platformOverrides;
        std::string platform;
        const std::string provider = named_provider(state, *ctx.parsed, config, &platform, &ctx.chosenPlatform);
        auto sctx = target_context(state.m->platformOverrides, config, family,
                                   provider == "none" ? std::string{} : provider,
                                   state.msvc_usable_either_origin);
        sctx.platformPackages = provider != "none";
        // A platform the project or the machine chose for the request serves it:
        // another package's provider (openkal's musl identity of the system) is
        // not a candidate, and the request's own row waits for the platform's.
        if (!platform.empty())
            sctx.platformChosen = [platform](std::string_view ref) { return platform_short_name(ref) == platform; };
        ctx.sel = mcpp::toolchain::target::select(state.overrides.target_triple, ctx.parsed->arch,
                                                  ctx.parsed->os, ctx.parsed->env,
                                                  ctx.parsed->envExplicit, sctx);
        if (ctx.sel.row) {
            if (auto chosen = triple::parse(ctx.sel.row->triple)) {
                chosen->envExplicit = ctx.parsed->envExplicit;
                ctx.parsed = *chosen;
            }
        }
        // What the gates below ask of the request: the rows of its system, and
        // of those the ones this mcpp builds.
        ctx.req = triple::RequestResolution{};
        ctx.req.triple = *ctx.parsed;
        for (auto const* r : mcpp::toolchain::target::rows_of(
                 ctx.parsed->arch, ctx.parsed->os, ctx.parsed->env, ctx.parsed->envExplicit)) {
            ctx.req.siblings.push_back(r->triple);
            if (r->tier != "planned") ctx.req.supported.push_back(r->triple);
        }
        ctx.req.completedFromVocabulary = ctx.sel.row && ctx.sel.row->triple != ctx.parsed->str();
        auto& choice = state.targetChoice;
        choice = TargetChoice{};
        choice.request = state.overrides.target_request.value_or(state.overrides.target_triple);
        choice.identity = ctx.parsed->str();
        choice.candidates = ctx.sel.candidates;
        if (ctx.sel.provider) choice.provider = std::string(ctx.sel.provider->name);
        choice.priority = ctx.sel.priority;
        choice.fallback = ctx.sel.fallback;
        if (ctx.sel.fallback && !ctx.sel.candidates.empty()) choice.why = ctx.sel.candidates.front().why;
    }

    // [target.X] lookup is spelling-independent: a section keyed
    // `x86_64-w64-mingw32` matches `--target x86_64-windows-gnu` and
    // vice versa. Unparseable keys/inputs compare exactly (escape hatch).
    ctx.sectionEntry         = find_request_entry(*state.m, state.overrides.target_triple, ctx.parsed);
    ctx.hasExplicitSection   = ctx.sectionEntry != nullptr;
    ctx.hasToolchainOverride = ctx.hasExplicitSection && !ctx.sectionEntry->toolchain.empty();

    ctx.known = ctx.parsed ? triple::find_known_target(*ctx.parsed) : nullptr;
    // The row with the facts of the provider that serves it here, rather than
    // of its default provider: the toolchain it pins, the C library it brings,
    // the linkage and the platform package that implements it.
    if (ctx.known && ctx.sel.provider && ctx.sel.provider != ctx.known->provider) {
        state.selectedTarget = triple::view_of(*ctx.known->row, ctx.sel.provider);
        ctx.known = &*state.selectedTarget;
    }

    return {};
}

static std::expected<void, std::string>
step1_validate_target_tier(PrepareState& state, TargetOverrideCtx& ctx) {
    namespace triple = mcpp::toolchain::triple;
    // Validation: a typo must never silently fall through to the host
    // toolchain (the worst failure mode — you think you cross-compiled).
    // An explicit [target.X] section is the escape hatch for custom
    // triples outside the vocabulary.
    // Several rows serve this (arch, os) and the lexical default names none
    // of them, so there is nothing to complete the request WITH. Refusing
    // and listing them is the only honest answer; picking one would be an
    // invented convention. No group has this shape today — the rule is here
    // so the first one that does gets a diagnosis rather than a guess.
    if (ctx.parsed && ctx.req.ambiguous && !ctx.hasExplicitSection) {
        std::string opts;
        for (auto s : ctx.req.supported) {
            if (!opts.empty()) opts += ", ";
            opts += std::string(s);
        }
        refusal::record(refusal::Code::AmbiguousRequest);
        return std::unexpected(std::format(
            "target '{}' does not say which C library, and several are "
            "supported here.\n"
            "       candidates: {}\n"
            "       Name one of them.",
            ctx.requestedSpelling, opts));
    }
    // A row a platform defines is known to this build by its definition
    // (SPEC-016 §2), whether or not the project writes a section for it.
    if (!ctx.known && !ctx.hasExplicitSection && !(ctx.parsed && ctx.parsed->defined())) {
        // "UNKNOWN" IS A CLAIM ABOUT THE VOCABULARY, AND IT WAS FALSE FOR
        // A WHOLE arch+os FAMILY.
        //
        // Measured on 2026.8.26.1: `--target riscv64-linux` reported
        // `unknown target 'riscv64-linux'` while `riscv64-linux-musl` was
        // sitting in `kKnownTargets` as `planned`. The lexical fill had
        // produced `riscv64-linux-gnu` — a row that genuinely does not
        // exist — and the gate reported on the fill.
        //
        // A non-empty sibling group means the family IS registered, so this
        // is the planned refusal wearing the wrong word. It names the row
        // that exists, which is also the one the reader would have to write
        // to opt in.
        if (!ctx.req.siblings.empty()) {
            std::string rows;
            for (auto s : ctx.req.siblings) {
                if (!rows.empty()) rows += ", ";
                rows += std::string(s);
            }
            refusal::record(refusal::Code::TierPlanned);
            return std::unexpected(std::format(
                "target '{}' is registered but not yet supported (planned) — "
                "no toolchain is published for it yet.\n"
                "       registered rows for this system: {}\n"
                "       An explicit [target.<triple>] toolchain override can "
                "opt in early.",
                ctx.requestedSpelling, rows));
        }
        auto sug = triple::did_you_mean(ctx.requestedSpelling);
        // Whether the index's platform column was looked at again (mcpp#967).
        const auto column = index_platform_miss_note(ctx.requestedSpelling);
        refusal::record(refusal::Code::UnknownTarget);
        return std::unexpected(std::format(
            "unknown target '{}'{}\n"
            "       known targets: `mcpp toolchain list`; a custom triple needs an\n"
            "       explicit [target.{}] section in mcpp.toml{}",
            ctx.requestedSpelling,
            sug ? std::format(" — did you mean '{}'?", *sug) : "",
            ctx.requestedSpelling,
            column.empty() ? std::string{} : "\n       " + column));
    }
    // A TOOLCHAIN NAMED FOR THE BUILD IS AS EXPLICIT AS ONE THE SECTION NAMES.
    // The refusal says that no toolchain is published for the row; a build that
    // names one -- `[target.<row>] toolchain`, or `--toolchain`, which the
    // command line states as MCPP_TOOLCHAIN -- has one; so does a consumer's
    // sub-build given its consumer's. Before 2026.11.13.1 only the section did,
    // and a platform's conformance run that names its toolchain on the command
    // line was refused.
    const bool toolchainNamed = toolchain_named(state, ctx.sectionEntry);
    // AND SO IS ONE A PLATFORM CHOSEN FOR THE REQUEST NAMES FOR THE ROW
    // (2026.11.14.1). A platform that lists the row among its targets with its
    // toolchain serves it; the row's `planned` says only that the engine's own
    // providers do not yet. The platform's statement is read with the platform
    // (`step1_select_platform`, or `step4b_apply_platform` for a package this
    // pass fetches), and the build is refused there when it does not list the
    // row. openkal-platform 0.28.0 lists `i686-openkal`, and `--target openkal`
    // (its targets as one set) was refused on that leg.
    if (ctx.known && ctx.known->tier == "planned" && !toolchainNamed && !ctx.chosenPlatform.empty()) {
        state.plannedRow = ctx.parsed->str();
        return {};
    }
    if (ctx.known && ctx.known->tier == "planned" && !toolchainNamed) {
        refusal::record(refusal::Code::TierPlanned);
        // The subject is what the user wrote. When completion filled a
        // segment, both are shown — otherwise the sentence is about a
        // string that appears nowhere in their command.
        const std::string subject =
            ctx.requestedSpelling == ctx.parsed->str()
                ? std::format("'{}'", ctx.requestedSpelling)
                : std::format("'{}' (which resolves to '{}')",
                              ctx.requestedSpelling, ctx.parsed->str());
        return std::unexpected(std::format(
            "target {} is registered but not yet supported (planned) — "
            "no toolchain is published for it yet.\n"
            "       A toolchain named for the build opts in early: [target.{}] toolchain, or --toolchain.",
            subject, ctx.parsed->str()));
    }
    return {};
}

static std::expected<void, std::string>
step1_apple_sdk_check(PrepareState& state, TargetOverrideCtx& ctx) {
    // AN APPLE SDK IS LOCATED, SO ITS ABSENCE IS KNOWN NOW.
    //
    // REFUSED HERE AND NOT WITH THE TOOLCHAIN, which is a decision about
    // WHEN rather than about the message. The iOS rows need the machine's
    // iPhoneOS or iPhoneSimulator SDK, and that is knowable before any
    // payload is resolved -- so a machine without Xcode used to download
    // a 700 MB compiler and then be told the thing it was missing was not
    // the compiler.
    //
    // AND UNLIKE `host_can_serve` BELOW, THIS IS NOT DEFERRED. That
    // refusal waits for the dependency graph because a package can supply
    // a target's C library and platform interface. An Apple SDK is not
    // redistributable, so no package supplies it: there is nothing a later
    // line could learn that would change this answer.
    //
    // The escape hatch that opens the tier gate does NOT open this one.
    // Declaring a toolchain says which compiler; it says nothing about
    // where the headers and stub libraries are, and every compiler needs
    // them.
    //
    // FROM THE BASE STATEMENT (design 2026-10-23 §4.4): `iphoneos@system` by
    // default, `iphoneos@<version>`, or `{ path }`, located by Apple's own
    // conventions -- `SDKROOT` on every host, `xcrun` and the developer
    // directory on an Apple one. A native macOS build that states its SDK is
    // located the same way; one that states nothing keeps `xcrun`'s answer.
    if (ctx.parsed && ctx.parsed->os == "macos") {
        auto stmt = base_statement(state, *ctx.parsed);
        if (!stmt) return std::unexpected(stmt.error());
        if (stmt->form == mcpp::toolchain::base::Form::Locator
            || stmt->form == mcpp::toolchain::base::Form::Path) {
            auto sdk = mcpp::toolchain::base::locate_apple(*stmt, mcpp::toolchain::apple::sdk_macos);
            if (!sdk) {
                refusal::record(refusal::Code::AppleSdkAbsent);
                return std::unexpected(std::format(
                    "target {} names the macosx SDK {}, which this machine does not provide: {}",
                    ctx.parsed->str(), stmt->text, sdk.error()));
            }
            state.appleSdkLocated = sdk->root;
            state.appleSdkFound = *sdk;
        }
    }
    if (ctx.parsed && ctx.parsed->is_ios()) {
        const auto which = ctx.parsed->is_ios_simulator()
            ? mcpp::toolchain::apple::sdk_iphonesim
            : mcpp::toolchain::apple::sdk_iphoneos;
        auto stmt = base_statement(state, *ctx.parsed);
        if (!stmt) return std::unexpected(stmt.error());
        mcpp::toolchain::base::Statement asked = *stmt;
        // Only a locator and a path name an SDK; a package or "" keep the
        // meaning they had on this row, and the SDK is the default one.
        if (asked.form != mcpp::toolchain::base::Form::Locator
            && asked.form != mcpp::toolchain::base::Form::Path) {
            asked.form = mcpp::toolchain::base::Form::Locator;
            asked.family = std::string(which);
            asked.version = "system";
            asked.text = std::format("{}@system", which);
        }
        auto sdk = mcpp::toolchain::base::locate_apple(asked, which);
        std::string why = sdk ? std::string{} : sdk.error();
        if (sdk) {
            state.appleSdkLocated = sdk->root;
            state.appleSdkFound = *sdk;
        }
        // AN UNSET FLOOR IS THE LOCATED SDK'S VERSION, READ RATHER THAN
        // LEFT TO THE DRIVER. `docs/20` promised that an unversioned
        // triple meant the SDK's own default; measured on macos-15 with
        // Xcode 16.4, clang given `arm64-apple-ios` with no version
        // refused thread-local storage for the target, which libc++abi
        // uses, so the default it chose was older than any SDK on the
        // machine. The version `xcrun` reports for the located SDK is the
        // one the SDK was made for, and it enters the manifest here so
        // that the fingerprint slot, the effective triple and every
        // report read one value.
        if (state.appleSdkLocated && state.m->buildConfig.iosDeploymentTarget.empty()) {
            if (!state.appleSdkFound->version.empty()) {
                state.m->buildConfig.iosDeploymentTarget = state.appleSdkFound->version;
                state.iosFloorFromSdk = true;
            }
        }
        // THE SLICE DECIDES THE ROW (D16). An SDK that no longer carries the
        // target's architecture is refused naming the slice, not left to fail
        // at the first link.
        if (state.appleSdkLocated) {
            const auto slice = std::format("{}-ios{}",
                ctx.parsed->arch == "aarch64" ? "arm64" : ctx.parsed->arch,
                ctx.parsed->is_ios_simulator() ? "-simulator" : "");
            if (mcpp::toolchain::apple::sdk_has_target(*state.appleSdkLocated, slice)
                == mcpp::toolchain::apple::Slice::absent) {
                refusal::record(refusal::Code::AppleSdkAbsent);
                return std::unexpected(std::format(
                    "target {}: the {} SDK at {} carries no {} slice (its libSystem.tbd "
                    "does not list it). Name an SDK that does: [target.{}] sysroot = "
                    "\"{}@<version>\" or {{ path = \"…\" }}.",
                    ctx.parsed->str(), which, state.appleSdkLocated->string(), slice,
                    ctx.parsed->str(), which));
            }
        }
        if (!state.appleSdkLocated) {
            // A CODE, BECAUSE THE MATRIX COMPARES REASONS AND NOT ONLY
            // OUTCOMES. A refusal with no code is recorded as `other`,
            // which `check_matrix_reasons.sh` refuses on the ground that
            // it freezes an unnamed branch into the expected table.
            refusal::record(refusal::Code::AppleSdkAbsent);
            if (stmt->stated())
                return std::unexpected(std::format(
                    "target {} names the {} SDK {} ({}), which this machine does not "
                    "provide: {}", ctx.parsed->str(), which, asked.text,
                    stmt->originFile.empty() ? stmt->originKey : stmt->originFile, why));
            return std::unexpected(std::format(
                "target {} needs the {} SDK, which this machine does not "
                "provide.\n"
                "       It is not redistributable, so mcpp LOCATES it "
                "rather than installing it. {}.\n"
                "       On macOS install Xcode (not the Command Line Tools alone -- "
                "those ship the macOS SDK only) so that `xcrun --sdk {} "
                "--show-sdk-path` answers. Elsewhere, produce one from your own "
                "Xcode.xip (e.g. with xtool) and set SDKROOT, or write "
                "[target.{}] sysroot = {{ path = \"…\" }}.\n"
                "       The compiler is not what is missing: these rows pin "
                "`xim:llvm`, which every other Apple row also uses.",
                ctx.parsed->str(), which, why, which, ctx.parsed->str()));
        }
    }
    return {};
}

static std::expected<void, std::string>
step1_wasm_shared_lib_check(PrepareState& state, TargetOverrideCtx& ctx) {
    namespace triple = mcpp::toolchain::triple;
    // A `shared` TARGET NAMES A LINK CONTRACT THIS ENGINE DOES NOT RENDER.
    //
    // `-sSIDE_MODULE` is a different Emscripten link mode from the
    // ordinary one (one static image, `artifact_naming`'s `.js`+`.wasm`
    // pair) and mcpp emits no flag for it. Falling through to the
    // ordinary link would still WRITE a `.so`-shaped file — the fallback
    // naming's `sharedLibExt` is empty, so the linker would be asked for
    // an empty-named output — so this is caught here, by NAME, rather
    // than reached as an obscure link failure.
    //
    // REFUSED HERE AND NOT AT PLAN TIME, same reasoning as the Apple SDK
    // check above: `parsed` and the manifest's own target list are both
    // already known, resolving neither an emsdk payload nor any other
    // toolchain, so an offline build (no emsdk installed) gets this
    // sentence instead of downloading the SDK first.
    if (ctx.parsed && ctx.parsed->object_format()
                      == triple::ObjectFormat::Wasm) {
        for (auto const& t : state.m->targets) {
            if (t.kind != mcpp::manifest::Target::SharedLibrary) continue;
            return std::unexpected(std::format(
                "[targets.{}] kind = \"shared\" is not supported on "
                "wasm32-emscripten: a side module needs -sSIDE_MODULE, "
                "which mcpp does not render",
                t.name));
        }
    }
    return {};
}

static void
step1_host_can_serve_check(PrepareState& state, TargetOverrideCtx& ctx) {
    namespace triple = mcpp::toolchain::triple;
    // Known, supported — and IMPOSSIBLE ON THIS HOST.
    //
    // Without this the target falls through to the host toolchain and the
    // build SUCCEEDS, which is the failure the check above calls the worst
    // one, arriving through a different door. Measured on Linux:
    //
    //   $ mcpp build --target x86_64-windows-msvc
    //       Resolved gcc@16.1.0 → x86_64-windows-msvc → …/xim-x-gcc/bin/g++
    //       Finished dev [unoptimized + debuginfo] in 0.07s
    //   $ ls target/
    //       x86_64-linux-gnu/          ← an ELF, reported as a Windows build
    //
    // The vocabulary tier says "mcpp supports this target"; it never said
    // "this machine can produce it". `host_can_serve` is the answer to the
    // second question and lives beside the payload resolution it has to
    // agree with.
    //
    // The escape hatch stays open on purpose: an explicit `[target.X]`
    // toolchain override means the author is supplying the cross toolchain
    // themselves, and mcpp's payload matrix has no standing to refuse it.
    // DIAGNOSED HERE, REPORTED LATER, AND THE DIFFERENCE IS THE POINT.
    //
    // Whether a payload on this machine produces this target is knowable
    // now. Whether anything ELSE produces it is not: a dependency can
    // supply the target's platform interface and C library, and the
    // dependency graph does not exist yet at this line. Refusing here
    // therefore answered a narrower question than the one it claimed —
    // measured, a project that only had to add a dependency was told its
    // machine could not build the target at all.
    //
    // The refusal is kept in full, because it is right whenever nothing
    // supplies the target side, which remains the ordinary case. It is
    // carried to where the graph is known and released there. Nothing
    // between here and there consumes the answer: what follows is toolchain
    // and dependency resolution, and a target no payload serves resolves to
    // a driver that simply will not be asked to emit anything.
    //
    // The escape hatch stays open on purpose: an explicit `[target.X]`
    // toolchain override means the author is supplying the cross toolchain
    // themselves, and mcpp's payload matrix has no standing to refuse it.
    // THE SELECTION SAYS WHETHER ITS ANSWER IS AVAILABLE HERE (SPEC-016 §4.6):
    // the candidate it took is the first available one, or, when none is, the
    // first of all, which this diagnosis refuses unless the graph supplies the
    // target's system after all (held until the graph is known).
    const auto chosen = std::ranges::find_if(ctx.sel.candidates, [&](auto const& c) {
        return c.row == ctx.sel.row && c.provider == ctx.sel.provider;
    });
    const bool available = chosen != ctx.sel.candidates.end() ? chosen->available
                         : (ctx.parsed && mcpp::toolchain::host_can_serve(*ctx.parsed));
    if (ctx.known && ctx.known->tier != "planned" && !ctx.hasToolchainOverride
        && ctx.parsed && !available) {
        std::string candidates;
        for (auto const& c : ctx.sel.candidates)
            candidates += std::format("\n         {:<22} {:<12} priority {:<4} {}", c.row->triple,
                                      c.provider ? c.provider->name : std::string_view("-"), c.priority,
                                      c.available ? std::string("available") : c.why);
        std::string servable;
        for (auto const* r : mcpp::toolchain::target::rows()) {
            if (r->tier == "planned") continue;
            auto one = mcpp::toolchain::target::select(r->triple, r->arch, r->os, r->env, true,
                target_context(state.m->platformOverrides, {}, {}, {}, state.msvc_usable_either_origin));
            if (one.candidates.empty() || one.fallback || !one.candidates.front().available) continue;
            if (!servable.empty()) servable += ", ";
            servable += r->triple;
        }
        state.unservedTargetDiagnosis = std::format(
            "target '{}' cannot be built on this host: no provider of it is available here, and "
            "nothing in the dependency graph supplies its system side.\n"
            "       candidates:{}\n"
            "       this host builds: {}\n"
            "       To build it anyway: depend on a package that implements the target's system "
            "(its kernel interface and C library),\n"
            "       choose a platform that serves it (`--platform <package>`), or name a cross "
            "toolchain this machine has by path:\n"
            "         [toolchain]\n"
            "         {} = {{ path = \"<dir>\", prefix = \"<driver prefix>\" }}   # docs/20\n"
            "       `mcpp why target {}` lists the candidates and why each is or is not available.",
            ctx.parsed->str(), candidates.empty() ? std::string(" (none)") : candidates,
            servable.empty() ? "(nothing — `mcpp toolchain list`)" : servable,
            mcpp::toolchain::triple::host_triple().os, ctx.requestedSpelling);
    }
}

static void
step1_capture_display_and_canonicalize(PrepareState& state, TargetOverrideCtx& ctx) {
    // CAPTURED BEFORE CANONICALISATION, BECAUSE CANONICALISATION IS
    // EXACTLY WHAT DESTROYS IT.
    //
    // `str()` renders the filled-in identity, so `x86_64-linux` becomes
    // `x86_64-linux-gnu` here and every later `parse` of that string reports
    // an env segment the project never wrote. The request has to be taken
    // from the ONLY triple that still knows the difference: this one.
    if (ctx.parsed && ctx.parsed->envExplicit) state.requestedCAbi = ctx.parsed->env;
    // AND THE SPELLING THE PROJECT USED, FOR THE REPORT ONLY.
    //
    // The canonical form is the identity — the output directory, the cache
    // key, the subject of a `cfg()` — and it must stay filled. The REPORT is
    // a different thing: it says what was asked for and what resolved, and
    // heading it `x86_64-linux-gnu` above a line reading `c-abi musl` states
    // a contradiction the build does not actually contain. A project that
    // declined to name a C library is shown as having declined.
    if (ctx.parsed && !ctx.parsed->envExplicit && !ctx.parsed->env.empty()) {
        auto asWritten = *ctx.parsed;
        asWritten.env.clear();
        state.targetDisplayName = asWritten.str();
    }

    // Canonical from here on: cfg evaluation, spec attachment and the
    // target/ output directory all see one spelling.
    if (ctx.parsed) state.overrides.target_triple = ctx.parsed->str();

    if (ctx.hasExplicitSection) step1_apply_target_section(state, *ctx.sectionEntry);
}

static std::expected<void, std::string>
step1_target_row_pin_and_capability_check(PrepareState& state, TargetOverrideCtx& ctx) {
    // Convention from the vocabulary table (triple.cppm): the target's
    // pinned toolchain (host-awareness — native musl-gcc vs triple-named
    // cross, winlibs mingw vs Linux-hosted cross — lives in the payload
    // mapping, not here) and its default linkage. GCC 16 pin rationale:
    // GCC 15 drops module template instantiations at link (remediation
    // doc A2; packages shipped 2026-07-08/09, GitHub+GitCode).
    // A convention, not an instruction: on the Windows-GNU first-run path
    // this is what turns the seeded target into `gcc@16.1.0`.
    //
    // It must not fire when it would overrule a toolchain the user wrote
    // down. The pin is mcpp's own default for a target row — `gcc@16.1.0`
    // for Windows-GNU, because the mingw payload is what supplies that
    // target's headers and C library — and an explicit `[toolchain]` line
    // is not a default. This is the promise the no-Visual-Studio fallback
    // is built on: mcpp revises its own defaults, never yours.
    //
    // HOW THE TARGET WAS NAMED IS NOT PART OF THE QUESTION, and it used to
    // be. The guard read `targetFromGlobalDefault && user_explicit`, so a
    // target given on the command line disabled it — and then the row's pin
    // replaced a toolchain the project had stated. Measured 2026-08-23:
    // `--target x86_64-windows-gnu` with an explicit `llvm@22.1.8` resolved
    // `x86_64-w64-mingw32-g++`, and gcc cannot compile libc++'s std module.
    //
    // A project that means to use a different compiler for a pinned target
    // is stating something about its own build, and a project whose target
    // side comes from its dependency graph is the ordinary reason to do so:
    // the payload the row names supplies headers and a C library that such
    // a project does not use. The narrower reading of this guard was
    // patched with an openkal-specific exception; stating the rule
    // correctly removes the need for one.
    // RECORDED, NOT APPLIED. The convention answers "which payload
    // supplies this target's C library", and whether it is needed depends on
    // whether the dependency graph supplies one instead. That is knowable
    // only after resolution, so the decision waits for
    // `resolve_target_toolchain` and only the candidate is kept here.
    if (ctx.known && !ctx.known->pin.empty() && ctx.parsed
        && !ctx.parsed->pin_is_capability()) {
        state.targetRowPin  = std::string(ctx.known->pin);
        state.targetRowName = ctx.parsed->str();
    }
    // A pair the configuration states for this machine's own target is kept
    // (`configured_native_pair`).
    const bool configuredNativePair = ctx.parsed
        && configured_native_pair(ctx.targetFromGlobalDefault, state.tcOrigin, *ctx.parsed,
                                  mcpp::toolchain::triple::host_triple());
    if (ctx.known && !ctx.hasToolchainOverride && !ctx.known->pin.empty()
        && !tc_origin_is_user_explicit(state.tcOrigin) && !configuredNativePair) {
        state.targetPinCandidate = std::string(ctx.known->pin);
        state.targetPinIsCapability = ctx.parsed && ctx.parsed->pin_is_capability();
    }
    // A USER'S EXPLICIT TOOLCHAIN OVERRIDES A CONVENTION, NOT A
    // CAPABILITY — AND UNTIL THIS LINE IT OVERRODE BOTH.
    //
    // The block above deliberately steps aside for an explicit
    // `[toolchain] default`: a hosted row's pin says "this payload supplies
    // the target's C library", and an author who names their own compiler
    // has said they will supply it instead. A bare-metal row's pin says
    // something the author cannot override — the table's own words: "the
    // pin is llvm on every host because clang/lld are cross-compilers by
    // construction". A host g++ does not emit riscv64 whatever anyone
    // declares.
    //
    // Measured 2026-08-26:
    //
    //     [toolchain] default = "gcc@16.1.0"
    //     $ mcpp build --target riscv64-none-elf
    //       g++: error: unrecognized argument in option '-mabi=lp64d'
    //       g++: note: valid arguments to '-mabi=' are: ms sysv
    //
    // — a message about an option, for a decision made here. Refusing at
    // the decision costs one line; the alternative is a compiler complaining
    // about flags the reader never wrote.
    if (ctx.known && ctx.parsed && ctx.parsed->pin_is_capability()
        && tc_origin_is_user_explicit(state.tcOrigin) && state.tcSpec.has_value()) {
        auto declared = mcpp::toolchain::parse_toolchain_spec(*state.tcSpec);
        // WHICH DECLARATIONS THE ROW ACCEPTS IS THE ROW'S PIN, NOT A FIXED
        // FAMILY.
        //
        // This asked `family != Llvm`, which was right while every
        // capability-pinned row pinned llvm. `wasm32-emscripten` pins
        // `emsdk@6.0.9`, and emsdk NORMALISES to the llvm family -- `em++`
        // is clang -- so a declared `llvm@22.1.8` passed this gate, was
        // never refused, and resolved the generic llvm payload for a target
        // it cannot emit. The condition is now the pin's own family, which
        // is the question the row was always answering.
        const auto pinFamily = [&]() -> std::optional<mcpp::toolchain::Family> {
            if (ctx.known->pin.empty()) return mcpp::toolchain::Family::Llvm;
            if (auto ps = mcpp::toolchain::parse_toolchain_spec(
                    std::string(ctx.known->pin)))
                return ps->family;
            return std::nullopt;
        }();
        const bool declaredMatchesPin =
            declared && pinFamily && declared->family == *pinFamily
            // An emsdk row is llvm-family, so the family alone cannot
            // separate `emsdk@6.0.9` from `llvm@22.1.8`. The pin's own
            // spelling is what does.
            && (ctx.known->pin.empty()
                || state.tcSpec->find(ctx.known->pin.substr(0, ctx.known->pin.find('@')))
                   != std::string::npos);
        if (declared && !declaredMatchesPin) {
            // THE REASON TRAVELS WITH THE ROW. The rows refuse for the
            // same rule and NOT for the same reason, and one sentence
            // covering all of them would be wrong about the others: a
            // PE+musl target is not bare metal, a wasm target is neither,
            // and a reader told the wrong one stops reading.
            //
            // Measured before the third arm existed: `--target
            // wasm32-emscripten` with a declared gcc was refused correctly
            // and explained with "No gcc payload emits a PE with a musl C
            // library", which is a true sentence about a different row.
            //
            // IT HAPPENED AGAIN, AND ADDING AN ARM IS ONLY HALF THE FIX.
            // Android became a capability row and this chain still had
            // three arms, so a declared `llvm@22.1.8` against
            // `aarch64-linux-android` was refused correctly and explained
            // with the PE+musl sentence -- the identical wrong answer the
            // paragraph above records for wasm, reached the same way: by a
            // fourth case falling into a final `else` that was written as
            // the third case's answer.
            //
            // So the last arm now NAMES ITS OWN ROW and the fallthrough is
            // generic. A capability added later gets a sentence that is
            // merely unspecific instead of one that is false, and the
            // refusal still names the pin either way.
            std::string_view why = ctx.parsed->is_freestanding()
                ? "A freestanding target has no per-host cross payload: "
                  "clang and lld are\n"
                  "       cross-compilers by construction and gcc is not."
                : ctx.parsed->loader_supplies_os()
                ? "A target whose loader supplies the system is emitted by "
                  "clang and lld for\n"
                  "       every host: the compiler is told no operating system, "
                  "and gcc has no such target."
                : ctx.parsed->is_wasm()
                ? "Nothing but Emscripten emits WebAssembly: `em++` is a "
                  "clang whose target,\n"
                  "       sysroot and JavaScript glue all come from its own "
                  "payload."
                : ctx.parsed->is_android()
                ? "An Android target needs bionic, not just an aarch64 or "
                  "x86_64 back end:\n"
                  "       its headers, its per-API-level stubs and its "
                  "loader path are inside the\n"
                  "       NDK, and no package adds them to another compiler."
                : (ctx.parsed->is_pe() && ctx.parsed->is_musl())
                ? "No gcc payload emits a PE with a musl C library — the "
                  "mingw payload emits\n"
                  "       PE with the MinGW CRT, which is the separate "
                  "`-gnu` row."
                : "This row's toolchain is the only one that can emit the "
                  "target at all.";
            refusal::record(refusal::Code::CapabilityPin);
            return std::unexpected(std::format(
                "target '{}' cannot be emitted by '{}'.\n"
                "       {}\n"
                "       The row names `{}` as a capability rather than as a "
                "preference, so\n"
                "       this one line is not a convention you can override.\n"
                "       remove the `[toolchain]` line for this target, or set "
                "it to `{}`.",
                ctx.parsed->str(), *state.tcSpec, why,
                ctx.known->pin.empty() ? std::string_view("llvm") : ctx.known->pin,
                ctx.known->pin.empty() ? std::string_view("llvm") : ctx.known->pin));
        }
    }
    // The provider that serves the build states the default linkage, for a
    // built-in row and for a row a platform package defines alike: the latter
    // is not in the built-in table, and reading the table alone left a static
    // platform's programs (DJGPP's) shared in every later step that asks.
    const bool defaultStatic = ctx.known ? ctx.known->defaultStatic
        : ctx.sel.row && mcpp::toolchain::triple::view_of(*ctx.sel.row, ctx.sel.provider).defaultStatic;
    if (defaultStatic && state.m->buildConfig.linkage.empty())
        state.m->buildConfig.linkage = "static";
    return {};
}

static std::expected<void, std::string> step1_target_and_static_overrides(PrepareState& state) {
    // ─── --target / --static overrides ──────────────────────────────────
    // Target-axis default resolution when no --target flag was passed:
    // [build] target (project default, ≙ cargo build.target) >
    // [toolchain] default_target (global config) > host.
    if (state.overrides.target_triple.empty() && !state.m->buildConfig.target.empty()
        && !state.overrides.host_target)
        state.overrides.target_triple = state.m->buildConfig.target;
    // Remembered, not requested: this one came out of the global config, so
    // it must not outrank anything the user wrote down (see the pin below).
    bool targetFromGlobalDefault = false;
    if (state.overrides.target_triple.empty() && !state.overrides.host_target) {
        if (auto cfg = state.get_cfg(true); cfg && !(*cfg)->defaultTarget.empty()) {
            state.overrides.target_triple = (*cfg)->defaultTarget;
            targetFromGlobalDefault = true;
        }
    }
    // Normalize the triple (alias spellings → canonical), validate against
    // the known-target vocabulary, then apply the manifest [target.<triple>]
    // override and the vocabulary-table convention (pin + default linkage).
    if (!state.overrides.target_triple.empty()) {
        TargetOverrideCtx ctx;
        ctx.targetFromGlobalDefault = targetFromGlobalDefault;
        if (auto r = step1_resolve_target_triple_request(state, ctx); !r)
            return std::unexpected(r.error());
        if (auto r = step1_validate_target_tier(state, ctx); !r)
            return std::unexpected(r.error());
        if (auto r = step1_apple_sdk_check(state, ctx); !r)
            return std::unexpected(r.error());
        if (auto r = step1_wasm_shared_lib_check(state, ctx); !r)
            return std::unexpected(r.error());
        step1_host_can_serve_check(state, ctx);
        step1_capture_display_and_canonicalize(state, ctx);
        if (auto r = step1_target_row_pin_and_capability_check(state, ctx); !r)
            return std::unexpected(r.error());
    }
    // A HOST BUILD READS ITS OWN ROW (#704). `[target.<triple>]` is looked up
    // by the triple the build produces, and a build without `--target`
    // produces the host's. Before this the row applied only when a triple was
    // named: `[target.x86_64-linux-gnu] cxx_runtime` shaped `--target
    // x86_64-linux-gnu` and was ignored by `mcpp build` on that same machine,
    // while the row's `.build` table and its `sysroot` already applied to both.
    // The triple is not written into `state.overrides.target_triple`: that would make
    // the host build a target build and turn the row's env segment into a
    // requested C library.
    else if (auto* hostRow = find_target_entry(*state.m, mcpp::toolchain::triple::host_triple()))
        step1_apply_target_section(state, *hostRow);
    if (state.overrides.force_static) state.m->buildConfig.linkage = "static";

    // #254: everything compiled INTO this build is resolved for the TARGET —
    // an xpkg descriptor's per-OS sections (sources, flags, deps) and its xpm
    // asset/version table all describe code that will run on the target, not
    // on the machine building it. Previously a compile-time host constant,
    // which is invisible natively (host == target) and picks the wrong leg
    // under --target.
    //
    // Computed HERE, not earlier: `state.overrides.target_triple` is only complete
    // above — it is filled from `[build] target` and the config default, then
    // canonicalized. Reading it before that point would silently fall back to
    // the host for any project that sets its target in the manifest rather
    // than on the command line.
    return {};
}


static std::expected<void, std::string> step1_device_axis_and_layer_merge(PrepareState& state) {
    // ── The device axis, resolved ONCE ────────────────────────────────────
    //
    // `--accel` / `--no-accel` over `[build] accel`. `--no-accel` arrives as the
    // sentinel "(none)", which parse_accel reads as nothing, and printing the
    // parsed form back normalises the spelling -- so every reader below sees
    // one string, and a build program sees the same one in MCPP_ACCEL. Read
    // at call time rather than captured: a `[target.'cfg(...)'.build]` section
    // may set `accel`, and the merge that applies it runs a few lines down.
    //
    // "NO ACCELERATOR" IS THE EMPTY STRING HERE, NOT `accel_str`'s "(none)".
    //
    // `accel_str` is a DISPLAY function: it prints `(none)` for an empty set so
    // an ABI tag reads as a sentence. Handing that spelling on as a value made
    // two readers wrong at once. A build program saw `MCPP_ACCEL=(none)` while
    // the manual promised an empty string, so a rule package asking "is there
    // an accelerator" got a yes and a backend named `(none)`; and the
    // fingerprint's own guard, `if (!accel.empty())`, was true for every
    // project on earth, appending `#accel=(none)` to builds that had asked for
    // nothing. Measured 2026-09-05 with a build program that wrote the value to
    // a file, which is the only way to see it -- a program's stdout is shown
    // only when it fails.
    const auto validateAccel = [&]() -> std::expected<void, std::string> {
        const bool overridden = !state.overrides.accel.empty();
        auto parsed = mcpp::pack::parse_accel_request(
            overridden ? state.overrides.accel : state.m->buildConfig.accel);
        if (!parsed) return std::unexpected(std::format("{}: {}",
            overridden ? "--accel" : "[build] accel", parsed.error()));
        return {};
    };
    if (auto r = validateAccel(); !r) return r;
    state.resolvedAccel = [&state]() -> std::string {
        const auto sets = mcpp::pack::parse_accel_request(
            state.overrides.accel.empty() ? state.m->buildConfig.accel : state.overrides.accel);
        // Both the initial statement and the conditional merge below are
        // checked before a consumer can use this closure.
        return !sets || sets->empty() ? std::string{} : mcpp::pack::accel_str(*sets);
    };
    // The cfg context, with the accelerator layer filled from the resolved
    // accel's backend names. `cfg(accelerator = "cuda")` is a membership test
    // over these (prepare_inputs::Ctx::layer_matches); before this the field
    // was declared, documented, and never written, so the key matched nothing.
    state.cfgCtx = [&]() {
        auto c = cfgpred::context_for(state.overrides.target_triple,
                                      state.platformPackageShortName);
        for (auto const& set : mcpp::pack::parse_accel(state.resolvedAccel()))
            c.accelerators.push_back(set.backend);
        return c;
    };
    // The descriptor blocks the target reads (design 2026-10-29 §5.1): the
    // row's own, or those the platform states when it was read early (a
    // platform fetched in the first pass restarts the prepare so that it is).
    state.targetPlatform = [&] {
        if (state.platformEarly && state.platformData.descriptorKeys.set)
            return mcpp::toolchain::platform::TargetPlatform::with_keys(state.platformData.descriptorKeys.value);
        // A row a platform defines names its own blocks (SPEC-016 §2).
        if (auto t = mcpp::toolchain::triple::parse(state.overrides.target_triple))
            if (auto* d = t->defined(); d && !d->descriptorKeys.empty())
                return mcpp::toolchain::platform::TargetPlatform::with_keys(
                    std::vector<std::string>(d->descriptorKeys.begin(), d->descriptorKeys.end()));
        return mcpp::toolchain::platform::TargetPlatform::for_os(state.cfgCtx().os);
    }();

    // ── L1: merge conditional [target.'cfg(...)'] sections ───────────────────
    // Evaluated now (target resolved) against the resolved target — the
    // --target triple for a cross build, else the host.
    //
    // #229: merge_conditional_config MUST run here — before
    // `packages[0] = makePackageRoot(*root, *m)` snapshots `m->buildConfig`
    // into `packages[0].privateBuild`/`.manifest` — because that snapshot,
    // not `*m`, is what the modgraph scan and per-TU compile-flag assembly
    // actually read afterward. Every dependency (path/git/version alike) gets
    // the SAME treatment, at the mirror-image point in its own load path
    // (right before ITS `makePackageRoot`/`propagateLinkFlags`) — see the
    // dependency-manifest-acquisition block below. That makes this the root
    // package's half of the one funnel, not a special case: every package is
    // merged exactly once, immediately before it is captured into `packages[]`.
    if (!state.m->conditionalConfigs.empty()) {
        merge_conditional_config(*state.m, state.cfgCtx());
    }
    if (auto r = validateAccel(); !r) return r;
    // `[target.<selector>.abi] threads` -- the ROOT's statement, rendered once,
    // into channels that already reach the whole artefact: the graph-global
    // dialect flag set (every C++ translation unit, the std module's own
    // commands, the scan, every dependency's cache key), the C flags of every
    // package (the root here, each dependency where it is loaded), and the link.
    //
    // For hosted targets that are not PE. On PE the MSVC runtime is always
    // multithreaded and mingw-w64's threading model belongs to its payload; a
    // freestanding target has no thread library to select.
    state.abiThreadsRendered = [&] {
        if (!state.m->buildConfig.abiThreads) return false;
        const auto abiTriple = mcpp::toolchain::triple::parse(
            state.overrides.target_triple.empty()
                ? mcpp::toolchain::triple::host_triple().str()
                : state.overrides.target_triple);
        return abiTriple && !abiTriple->is_pe() && !abiTriple->is_freestanding();
    }();
    state.add_once = [](std::vector<std::string>& v, std::string_view flag) {
        if (std::ranges::find(v, flag) == v.end()) v.emplace_back(flag);
    };
    if (state.abiThreadsRendered) {
        state.add_once(state.m->buildConfig.dialectCxxflags, "-pthread");
        state.add_once(state.m->buildConfig.cflags, "-pthread");
        state.add_once(state.m->buildConfig.ldflags, "-pthread");
        state.add_once(state.m->buildConfig.graphLdflags, "-pthread");
    }
    // `[target.<selector>.abi] exceptions` -- design 2026-09-12 (the UI
    // framework record), section 2.1, A1: the second `abi` member, the
    // ROOT's statement, rendered once. Reaches the dialect flag set (every
    // C++ translation unit, the std module's own commands, the scan, every
    // dependency's cache key) and the link -- NOT the C flags, unlike
    // `threads`: `-fexceptions` has no C-language meaning worth carrying to
    // a `.c` translation unit.
    //
    // Rendered only where the target's default is OFF: Emscripten's native
    // toolchain builds without exceptions unless asked. gcc, clang and MSVC
    // already link with exceptions on, so a host build with the member
    // declared is byte-identical to one without -- the same property
    // `threads` has on PE.
    //
    // The MODEL (design 2026-10-29 §6): "wasm" is WebAssembly's own exception
    // handling (`-fwasm-exceptions`: smaller and faster, and it needs Safari
    // 15.2 / Node 17 or later); "js" and `true` are the JavaScript-based model
    // `true` has meant since 2026.9.12.3. The flag is in the dialect set, so the
    // model is part of every cache key.
    const bool emscripten = state.cfgCtx().os == "emscripten";
    // THE ROW'S MODEL WHERE THE PROJECT STATES NONE (design 2026-11-15 N26):
    // wasm32-emscripten's row states `exceptions = "wasm"`, and a program's
    // `catch` was never reached under the SDK's default. Data, not a rule of
    // the engine; `exceptions = false` states none.
    if (emscripten && !state.m->buildConfig.abiExceptionsDeclared)
        if (auto t = mcpp::toolchain::triple::parse(state.overrides.target_triple))
            if (auto const* row = mcpp::toolchain::target::find_row(t->str()); row && !row->exceptions.empty()) {
                state.m->buildConfig.abiExceptions = true;
                state.m->buildConfig.abiExceptionsModel = std::string(row->exceptions);
                // The long jump follows WebAssembly exception handling (D6),
                // stated for the C translation units too, which do not read
                // the C++ flag; under another model it is the SDK's default.
                if (!state.m->buildConfig.abiLongjmpDeclared && row->exceptions == "wasm") {
                    state.m->buildConfig.abiLongjmpDeclared = true;
                    state.m->buildConfig.abiLongjmpModel = "wasm";
                }
            }
    const bool abiExceptionsRendered = state.m->buildConfig.abiExceptions && emscripten;
    if (abiExceptionsRendered) {
        const char* flag = state.m->buildConfig.abiExceptionsModel == "wasm"
            ? "-fwasm-exceptions" : "-fexceptions";
        state.add_once(state.m->buildConfig.dialectCxxflags, flag);
        state.add_once(state.m->buildConfig.ldflags, flag);
        state.add_once(state.m->buildConfig.graphLdflags, flag);
    }
    // `[target.<selector>.abi] longjmp` -- the third member (2026.11.12.1):
    // how `sigsetjmp`/`siglongjmp` cross the WebAssembly boundary. The flag
    // changes the CODE emcc emits for the two calls, so it reaches both
    // sides -- every translation unit's flags (C and C++ alike; the C library
    // calls are what it is about) and the link, the channels `threads` uses.
    // Declared on any row but Emscripten's it is refused, not ignored: the
    // member names a WebAssembly model, and a build that wrote it for another
    // target has a selector that matches the wrong row.
    if (state.m->buildConfig.abiLongjmpDeclared) {
        if (!emscripten)
            return std::unexpected(std::format(
                "[target.<selector>.abi] longjmp states a WebAssembly long-jump model, "
                "and the target '{}' is not an Emscripten row; the member belongs to "
                "`[target.'cfg(os = \"emscripten\")'.abi]` alone",
                state.overrides.target_triple.empty()
                    ? mcpp::toolchain::triple::host_triple().str()
                    : state.overrides.target_triple));
        // THE TWO MEMBERS NAME ONE MECHANISM (design 2026-11-12 D6): WebAssembly's
        // exception handling carries both the throw and the long jump, and the
        // JavaScript one carries both. A build that states one model for
        // exceptions and the other for long jumps is refused here, with both
        // statements, rather than by the SDK's driver after the compiles.
        // Unstated, the long-jump model is the SDK's own default, which
        // follows the exception model.
        const bool wasmLongjmp = state.m->buildConfig.abiLongjmpModel == "wasm";
        if (abiExceptionsRendered
            && wasmLongjmp != (state.m->buildConfig.abiExceptionsModel == "wasm"))
            return std::unexpected(std::format(
                "[target.<selector>.abi] longjmp = \"{}\" and exceptions = \"{}\" name two "
                "unwinding mechanisms for one program: state the same model for both, or leave "
                "`longjmp` unstated and it follows `exceptions`",
                state.m->buildConfig.abiLongjmpModel,
                state.m->buildConfig.abiExceptionsModel.empty()
                    ? std::string("js") : state.m->buildConfig.abiExceptionsModel));
        const std::string flag = "-sSUPPORT_LONGJMP="
            + (wasmLongjmp ? std::string("wasm") : std::string("emscripten"));
        state.add_once(state.m->buildConfig.dialectCxxflags, flag);
        state.add_once(state.m->buildConfig.cflags, flag);
        state.add_once(state.m->buildConfig.ldflags, flag);
        state.add_once(state.m->buildConfig.graphLdflags, flag);
    }
    // #851: emcc runs wasm-opt and minifies the JavaScript launcher AT THE
    // LINK, from the optimisation level the link is given; a link without
    // one is -O0 whatever the compiles were. The profile's level reaches it.
    if (emscripten) {
        const auto& lvl = state.m->buildConfig.optLevel;
        const std::string opt = "-O" + (lvl.empty() ? std::string("0") : lvl);
        state.add_once(state.m->buildConfig.ldflags, opt);
        state.add_once(state.m->buildConfig.graphLdflags, opt);
    }
    // `[build].defines` must reach the scanner (P1689) and the compile edge,
    // and must participate in the fingerprint. Fold before dependency
    // resolution / fingerprinting.
    report_flag_words_changes(*state.m);
    fold_build_defines_into_flags(state.m->buildConfig);

    // ORIGIN, RESOLVED ONCE.
    //
    // The spec used to be parsed TWICE from the same string a dozen lines
    // apart — once to ask "is this msvc@system", once to get the package —
    // and each call site drew its own conclusions from the result. Two parses
    // of one string is two places for the answer to differ, which is the shape
    // §1 of the three-axes design is about: a platform special case whose cost
    // is paid at every site that has to know about it.
    //
    // `Origin::SystemMsvc` is located on the machine and never resolved
    // through an xim package — mcpp does not install the machine's Visual
    // Studio. `Origin::Managed` is everything else, including a VERSIONED
    // msvc spec, and that is the point: what the manifest says is what gets
    // used, on every machine, instead of whatever this one happens to have.
    // RESOLVED HERE, RUN AFTER THE DEPENDENCY GRAPH — AND THE SPLIT IS THE
    // WHOLE POINT.
    //
    // A target row's convention does not name a preferred compiler. It names
    // the payload that supplies THAT TARGET'S C library. Whether the user's own
    // toolchain can serve the target instead depends on whether something ELSE
    // supplies the target side — and that is knowable only once the graph is
    // resolved, which is after this point in the function.
    //
    // Deciding early was measured to be wrong in both directions. Applying the
    // convention unconditionally replaced a toolchain the user had set with
    // `mcpp toolchain default`, for a payload their project never used. NOT
    // applying it turned a working zero-dependency cross build into a failing
    // one, because clang alone carries no C runtime for `x86_64-windows-gnu`
    // while the payload the row names does.
    //
    // The body does not MOVE; only its execution does. Everything between
    // here and the call site was measured to read `tc` exactly once, and that
    // one read wanted the target triple rather than the compiler.
    // `std::function` AND NOT `auto`, BECAUSE THE FIRST-RUN BRANCH INSIDE
    // CALLS BACK INTO IT. That branch installs a host default and then has to
    // resolve THAT default for the requested target — which is what the top of
    // this same function does. Recursing reuses it; writing it a second time
    // there would be a second answer to one question. Depth is one: the second
    // pass takes the `tcSpec.has_value()` branch that the first-run path just
    // made true.
    state.firstRunNeedsTargetPass = false;
    // Guards the one recursive call below. Set before the call so the second
    // pass cannot reach it, whatever else changed in between.
    state.targetPassDone = false;
    return {};
}

std::expected<void, std::string> phase1_toolchain_spec_and_axes(PrepareState& state) {
    // P3 (mcpp#797): a build plugin built as the root is planned for this
    // machine with the engine interface, decided before the toolchain is.
    if (!state.m->package.virtualRoot)
        if (auto r = configure_build_plugin_root(
                state, *state.m, *state.root,
                feature_closure(*state.m, parse_feature_request(state.overrides.features)));
            !r)
            return std::unexpected(r.error());
    if (auto r = step1_define_early_toolchain_closures(state); !r) return std::unexpected(r.error());
    step1_define_host_default_closures(state);
    if (auto r = step1_target_and_static_overrides(state); !r) return std::unexpected(r.error());
    // The platform, once the target is known and before the conditional
    // tables merge: `cfg(platform = ...)` is one of their keys (SPEC-013 §3).
    if (auto r = step1_select_platform(state); !r) return std::unexpected(r.error());
    if (auto r = step1_device_axis_and_layer_merge(state); !r) return std::unexpected(r.error());

    return {};
}

// THE PROJECT'S TIER OF THE KEYS OF THE WHOLE RUN (SPEC-004 §2): `[build]
// offline`, `[build] allow_host_tools` and `[xlings] archives` of the build's
// root, stated as their variables unless the caller set them
// (`mcpp::effects::state_key`); the machine's tier is stated as the
// configuration loads, below this one. A root that states none withdraws the
// project's tier an earlier plan of the run stated: members that differ in one
// are planned apart (`root_position_key`), one after another. An archive
// directory outside the project is this machine's, and a manifest that names
// one says so once.
static void state_project_keys(PrepareState& state) {
    if (!state.m || state.overrides.tool_depth > 0) return;
    using mcpp::effects::Tier;
    const auto& b = state.m->buildConfig;
    if (b.offline)
        mcpp::effects::state_key("MCPP_OFFLINE", Tier::Project, *b.offline ? "1" : "0",
                                 "mcpp.toml [build] offline");
    else
        mcpp::effects::withdraw_key("MCPP_OFFLINE", Tier::Project);
    if (b.allowHostToolsStated) {
        std::string joined;
        for (auto const& t : b.allowHostTools) joined += (joined.empty() ? "" : ",") + t;
        mcpp::effects::state_key("MCPP_ALLOW_HOST_TOOLS", Tier::Project, joined.empty() ? "none" : joined,
                                 "mcpp.toml [build] allow_host_tools");
    } else {
        mcpp::effects::withdraw_key("MCPP_ALLOW_HOST_TOOLS", Tier::Project);
    }
    if (!state.m->xlings.archivesStated) {
        mcpp::effects::withdraw_key("MCPP_ARCHIVES", Tier::Project);
        return;
    }
    const auto base = state.m->sourcePath.parent_path();
    std::vector<std::filesystem::path> dirs;
    for (auto const& d : state.m->xlings.archives) {
        const std::filesystem::path p = d;
        dirs.push_back(p.is_absolute() ? p.lexically_normal() : (base / p).lexically_normal());
        const auto rel = dirs.back().lexically_relative(state.root ? *state.root : base);
        if (rel.empty() || *rel.begin() == "..")
            mcpp::diag::notice(mcpp::diag::Severity::Note, "machine-path-in-manifest",
                std::format("[xlings] archives\x1f{}", dirs.back().generic_string()),
                std::format("mcpp.toml [xlings] archives names {}, a directory of this machine", d),
                "another machine lacks it; a machine's directory belongs in config.toml [xlings] archives");
    }
    mcpp::effects::state_key("MCPP_ARCHIVES", Tier::Project, mcpplibs::os::env::join_list(dirs),
                             "mcpp.toml [xlings] archives");
}

// THE BOOTSTRAP TOOLS FROM THE MACHINE'S PATH (`[build] allow_host_tools`,
// design 2026-11-21, offline and toolchain defaults §3.3; SPEC-004 §2): the
// variable the tiers stated (`state_project_keys`, the configuration's
// machine tier) answers every reader of this run and the programs it starts
// (`effects::host_tool_allowed`); each tool taken is a host input, said once.
static void resolve_host_tools(PrepareState& state) {
    const auto origin = mcpp::effects::key_origin("MCPP_ALLOW_HOST_TOOLS");
    if (origin.tier == mcpp::effects::Tier::None) return;
    for (std::string_view tool : {"ninja", "patchelf"}) {
        if (!mcpp::effects::host_tool_allowed(tool)) continue;
        const auto name = std::string(tool) + std::string(mcpplibs::os::exe_suffix);
        const auto path = mcpp::invocation::which(name);
        if (!path) continue;
        auto out = mcpp::run::capture_stdout({path->string(), "--version"});
        std::string version = out.exit_code == 0 ? out.output.substr(0, out.output.find('\n')) : std::string{};
        if (auto sp = version.rfind(' '); sp != std::string::npos) version = version.substr(sp + 1);
        SourceDecision d;
        d.subject = std::format("tool:bootstrap:{}", tool);
        d.value = path->string();
        d.detail = version;
        d.cls = SourceClass::Host;
        switch (origin.tier) {
            case mcpp::effects::Tier::Project:
                d.originKind = "manifest";
                d.originFile = state.m ? state.m->sourcePath.string() : std::string{};
                d.originKey = "[build] allow_host_tools";
                break;
            case mcpp::effects::Tier::Machine:
                d.originKind = "config";
                d.originFile = state.cfg_opt ? state.cfg_opt->configFile.string() : std::string{};
                d.originKey = "[build] allow_host_tools";
                break;
            default:
                d.originKind = "env";
                d.originKey = "MCPP_ALLOW_HOST_TOOLS";
        }
        record_source(state, std::move(d));
    }
}

} // namespace mcpp::build
