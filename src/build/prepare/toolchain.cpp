// toolchain.cpp -- P1 and P2: the toolchain specification and the target
// axis, and the definition of the toolchain resolver that P5 calls once the
// dependency graph exists.

module mcpp.build.prepare;
import :state;

import mcpp.build.prepare_inputs;

import std;
import mcpp.diag;
import mcpp.build.refusal;
import mcpp.build.version_floor;
import mcpp.platform.axis;
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
import mcpp.xlings;
import mcpp.xlings.runtime_selection;
import mcpp.runtime.binding;
import mcpp.toolchain.post_install;
import mcpp.platform;
import mcpp.platform.macos;
import mcpp.fetcher;
import mcpp.fetcher.progress;
import mcpp.ui;

namespace mcpp::build {

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
    if (ec || shown.empty() || shown.native().starts_with("..")) shown = file;
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
    state.get_cfg = [&](bool requireBootstrap = true) -> std::expected<mcpp::config::GlobalConfig*, std::string> {
        if (!state.cfg_opt) {
            auto c = mcpp::config::load_or_init(/*quiet=*/false,
                mcpp::fetcher::make_bootstrap_progress_callback());
            if (!c) return std::unexpected(c.error().message);
            state.cfg_opt = std::move(*c);
        }
        // Commands that need bootstrap tools (build, run, toolchain install)
        // pass requireBootstrap=true to get an early, clear error.
        if (requireBootstrap && !state.bootstrap_checked) {
            state.bootstrap_checked = true;
            auto problem = mcpp::config::check_base_init(*state.cfg_opt);
            if (!problem.empty()) {
                return std::unexpected(std::format(
                    "{}\n  hint: run `mcpp self init --force` to reset and re-initialize",
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
        auto resolved = mcpp::platform::runtime::resolve_runtime_binding(
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
    // `[xlings] deps`. `mcpp::xlings::runtime` is the sole runtime-selection
    // policy and `RuntimeBinding::subosDir` is its resolved answer; a second
    // derivation is how a build ends up with two subos and no way to say which
    // one it used. The per-package payload paths a program may also need are
    // already answered, separately, by `MCPP_XPKG_*_DIR`.
    state.projectSubosBin = [&]() -> std::string {
        using Mode = mcpp::xlings::runtime::RuntimeSelection::Mode;
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

    // THE DECLARED RUNTIME PAYLOAD IS PROVIDED BEFORE THE FIRST FIXUP THAT
    // CONSUMES IT (mcpp#660), and not earlier: a build whose toolchain needs
    // no C runtime payload must not download one. At most once per build. An
    // inherited binding is not exempt, because the parent build may have used
    // a toolchain that needed no payload. The two values derived above are
    // refreshed with the binding, because detection and the fingerprint read
    // them after the fixups.
    state.runtimePayloadProvided = false;
    state.provide_runtime_payload = [&](const mcpp::toolchain::XimToolchainPackage& pkg) {
        if (state.runtimePayloadProvided) return;
        if (mcpp::toolchain::post_install_fixup_kind(pkg).empty()) return;
        state.runtimePayloadProvided = true;
        auto cfgP = state.get_cfg(true);
        if (!cfgP) return;
        if (!mcpp::toolchain::ensure_declared_runtime(**cfgP, state.runtimeBindingSnapshot))
            return;
        state.runtimePayload = state.runtimeBindingSnapshot.libc;
        state.runtimeLibDir = state.runtimeBindingSnapshot.libraryDirs.empty()
            ? std::filesystem::path{} : state.runtimeBindingSnapshot.libraryDirs.front();
    };

    // mcpp#427: a toolchain fixup that could not run is a DEGRADATION, not a
    // failure — the build continues without it. But it has to be said, or the
    // eventual `stdlib.h: No such file or directory` arrives with no way to
    // connect it to its cause.
    //
    // Deduplicated by payload: `ensure_post_install_fixup` is called from up
    // to four seams in one build (manifest toolchain, default toolchain,
    // MinGW first-run, build.mcpp host toolchain) and they routinely resolve
    // the SAME payload. Saying it once is the rule mcpp#417 already paid for.
    auto fixupNoticed = std::make_shared<std::set<std::string>>();
    state.report_fixup = [fixupNoticed](
            const mcpp::toolchain::FixupOutcome& outcome,
            const std::filesystem::path& payloadRoot) {
        if (outcome.skippedReason.empty()) return;
        if (!fixupNoticed->insert(payloadRoot.generic_string()).second) return;
        // Only the fact this line ADDS. The `Runtime` note above already gave
        // the cause and the remedy for the same absence; repeating them here
        // would be the second copy of one message, which is the habit mcpp#417
        // exists to break.
        mcpp::ui::info("Toolchain", std::format(
            "used as installed — not patched against a C runtime ({})",
            outcome.skippedReason));
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
        state.m->buildConfig.optLevel = pr.optLevel;
        state.m->buildConfig.debug    = pr.debug;
        state.m->buildConfig.lto      = pr.lto;
        state.m->buildConfig.strip    = pr.strip;
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
        if (auto c = state.get_cfg(true)) roots.push_back((*c)->xlingsHome() / "data" / "xpkgs");
        for (auto& d : mcpp::config::project_xlings_data_roots(state.workRoot))
            roots.push_back(d / "xpkgs");
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
    if (const char* tcEnv = std::getenv("MCPP_TOOLCHAIN"); tcEnv && *tcEnv) {
        state.tcSpec   = std::string(tcEnv);
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
            (*c)->xlingsHome() / "data" / "xpkgs");
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
        const bool msvcUsable = mcpp::platform::is_windows
                             && state.msvc_usable_either_origin();
        return std::string(
            mcpp::toolchain::triple::pins::host_default_toolchain(msvcUsable));
    };

    state.windowsGnuFirstRun = false;
    if constexpr (mcpp::platform::is_windows) {
        if (!state.tcSpec.has_value() && state.overrides.target_triple.empty()
            && state.m->buildConfig.target.empty()
            && !state.msvc_usable_either_origin()) {
            auto cfgW = state.get_cfg(true);
            if (!cfgW || (*cfgW)->defaultTarget.empty()) {
                state.overrides.target_triple =
                    std::string(mcpp::toolchain::triple::pins::kFirstRunWinGnuTarget);
                state.windowsGnuFirstRun = true;
            }
        }
    }

    // `[target.<triple>]`'s build-shaping keys, applied the same way whichever
    // path found the row (#704). The section's toolchain is a statement about
    // this row the author wrote down, so it replaces `[toolchain]` and the
    // global default; `--toolchain` and a consumer's decision for a host tool
    // are statements about THIS invocation and keep precedence over it.
    return {};
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
    // The `[target.<triple>]` section this request matched, or null. A
    // pointer rather than the map iterator the single-function version held,
    // since iterator validity is not this struct's business to reason about
    // and the callers only ever read `->second`.
    const mcpp::manifest::TargetEntry* sectionEntry = nullptr;
    bool hasExplicitSection = false;
    bool hasToolchainOverride = false;
    const mcpp::toolchain::triple::TargetInfo* known = nullptr;
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
    ctx.parsed = triple::parse(state.overrides.target_triple);

    // THE REQUEST IS COMPLETED FROM THE VOCABULARY BEFORE ANYTHING
    // READS IT, AND THE ORDER RELATIVE TO THE `[target.X]` LOOKUP IS PART
    // OF THE CONTRACT.
    //
    // `parse` fills a missing env segment lexically so the identity stays
    // total — `x86_64-linux` IS `x86_64-linux-gnu`, and a unit test says so.
    // Every gate below then asked about the filled value instead of about
    // the request. See `triple::resolve_request` for the two measurements.
    //
    // The lookup that follows keys on `parsed->str()`, so completing after
    // it would match sections against a triple this build is not going to
    // use. A project wanting the `planned` row keeps its escape hatch by
    // WRITING the segment: `--target aarch64-linux-gnu` skips completion
    // entirely, because a written segment is a request rather than a gap.
    if (ctx.parsed) {
        ctx.req    = triple::resolve_request(*ctx.parsed);
        ctx.parsed = ctx.req.triple;
    }

    // [target.X] lookup is spelling-independent: a section keyed
    // `x86_64-w64-mingw32` matches `--target x86_64-windows-gnu` and
    // vice versa. Unparseable keys/inputs compare exactly (escape hatch).
    auto it = state.m->targetOverrides.find(state.overrides.target_triple);
    if (it == state.m->targetOverrides.end() && ctx.parsed) {
        for (auto o = state.m->targetOverrides.begin();
             o != state.m->targetOverrides.end(); ++o) {
            if (auto k = triple::parse(o->first);
                k && k->str() == ctx.parsed->str()) { it = o; break; }
        }
    }
    ctx.hasExplicitSection   = it != state.m->targetOverrides.end();
    ctx.sectionEntry         = ctx.hasExplicitSection ? &it->second : nullptr;
    ctx.hasToolchainOverride = ctx.hasExplicitSection
                             && !it->second.toolchain.empty();

    ctx.known = ctx.parsed ? triple::find_known_target(*ctx.parsed) : nullptr;

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
    if (!ctx.known && !ctx.hasExplicitSection) {
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
        refusal::record(refusal::Code::UnknownTarget);
        return std::unexpected(std::format(
            "unknown target '{}'{}\n"
            "       known targets: `mcpp toolchain list`; a custom triple needs an\n"
            "       explicit [target.{}] section in mcpp.toml",
            ctx.requestedSpelling,
            sug ? std::format(" — did you mean '{}'?", *sug) : "",
            ctx.requestedSpelling));
    }
    if (ctx.known && ctx.known->tier == "planned" && !ctx.hasToolchainOverride) {
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
            "       An explicit [target.{}] toolchain override can opt in early.",
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
    if (ctx.parsed && ctx.parsed->is_ios()) {
        const auto which = ctx.parsed->is_ios_simulator()
            ? mcpp::platform::macos::sdk_iphonesim
            : mcpp::platform::macos::sdk_iphoneos;
        state.appleSdkLocated = mcpp::platform::macos::sdk_path(which);
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
            if (auto v = mcpp::platform::macos::sdk_version(which)) {
                state.m->buildConfig.iosDeploymentTarget = *v;
                state.iosFloorFromSdk = true;
            }
        }
        if (!state.appleSdkLocated) {
            // A CODE, BECAUSE THE MATRIX COMPARES REASONS AND NOT ONLY
            // OUTCOMES. A refusal with no code is recorded as `other`,
            // which `check_matrix_reasons.sh` refuses on the ground that
            // it freezes an unnamed branch into the expected table.
            refusal::record(refusal::Code::AppleSdkAbsent);
            return std::unexpected(std::format(
                "target {} needs the {} SDK, which this machine does not "
                "provide.\n"
                "       It is not redistributable, so mcpp LOCATES it "
                "rather than installing it: `xcrun --sdk {} "
                "--show-sdk-path` must answer, which needs Xcode on macOS "
                "(not the Command Line Tools alone -- those ship the "
                "macOS SDK only).\n"
                "       Check `xcode-select -p`, and note that the "
                "compiler is not what is missing: these rows pin "
                "`xim:llvm`, which every other Apple row also uses.",
                ctx.parsed->str(), which, which));
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
    if (ctx.known && ctx.known->tier != "planned" && !ctx.hasToolchainOverride
        && ctx.parsed
        && !mcpp::toolchain::host_can_serve(*ctx.parsed)) {
        std::string servable;
        for (auto const& info : triple::known_targets()) {
            auto t = triple::parse(info.canonical);
            if (!t || info.tier == "planned") continue;
            if (!mcpp::toolchain::host_can_serve(*t)) continue;
            if (!servable.empty()) servable += ", ";
            servable += t->str();
        }
        state.unservedTargetDiagnosis = std::format(
            "target '{}' cannot be built on this host.\n"
            "       No toolchain payload here produces it, and nothing in "
            "the dependency graph\n"
            "       supplies its system side.\n"
            "       this host can build with the payload alone: {}\n"
            "       To build it anyway, depend on a package that implements "
            "the target's system\n"
            "       (its kernel interface and C library), or supply your own "
            "cross toolchain with\n"
            "       an explicit [target.{}] toolchain = \"…\" section.",
            ctx.parsed->str(),
            servable.empty() ? "(nothing — `mcpp toolchain list`)" : servable,
            ctx.parsed->str());
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
    if (ctx.known && !ctx.hasToolchainOverride && !ctx.known->pin.empty()
        && !tc_origin_is_user_explicit(state.tcOrigin)) {
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
    if (ctx.known && ctx.known->defaultStatic && state.m->buildConfig.linkage.empty())
        state.m->buildConfig.linkage = "static";
    return {};
}

static std::expected<void, std::string> step1_target_and_static_overrides(PrepareState& state) {
    // ─── --target / --static overrides ──────────────────────────────────
    // Target-axis default resolution when no --target flag was passed:
    // [build] target (project default, ≙ cargo build.target) >
    // [toolchain] default_target (global config) > host.
    if (state.overrides.target_triple.empty() && !state.m->buildConfig.target.empty())
        state.overrides.target_triple = state.m->buildConfig.target;
    // Remembered, not requested: this one came out of the global config, so
    // it must not outrank anything the user wrote down (see the pin below).
    bool targetFromGlobalDefault = false;
    if (state.overrides.target_triple.empty()) {
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
    state.resolvedAccel = [&]() -> std::string {
        const auto sets = mcpp::pack::parse_accel(
            state.overrides.accel.empty() ? state.m->buildConfig.accel : state.overrides.accel);
        return sets.empty() ? std::string{} : mcpp::pack::accel_str(sets);
    };
    // The cfg context, with the accelerator layer filled from the resolved
    // accel's backend names. `cfg(accelerator = "cuda")` is a membership test
    // over these (prepare_inputs::Ctx::layer_matches); before this the field
    // was declared, documented, and never written, so the key matched nothing.
    state.cfgCtx = [&]() {
        auto c = cfgpred::context_for(state.overrides.target_triple);
        for (auto const& set : mcpp::pack::parse_accel(state.resolvedAccel()))
            c.accelerators.push_back(set.backend);
        return c;
    };
    state.targetPlatform = mcpp::platform::TargetPlatform::for_os(state.cfgCtx().os);

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
    const bool abiExceptionsRendered = state.m->buildConfig.abiExceptions
                                     && state.cfgCtx().os == "emscripten";
    if (abiExceptionsRendered) {
        state.add_once(state.m->buildConfig.dialectCxxflags, "-fexceptions");
        state.add_once(state.m->buildConfig.ldflags, "-fexceptions");
        state.add_once(state.m->buildConfig.graphLdflags, "-fexceptions");
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
    if (auto r = step1_define_early_toolchain_closures(state); !r) return std::unexpected(r.error());
    if (auto r = step1_target_and_static_overrides(state); !r) return std::unexpected(r.error());
    if (auto r = step1_device_axis_and_layer_merge(state); !r) return std::unexpected(r.error());

    return {};
}

// A phase-local struct passed by reference to the steps of ONE call to
// `state.resolve_target_toolchain` -- the same pattern WorklistItemCtx
// (graph.cpp) and HostToolCtx (features.cpp) use for the steps of one
// worklist item / one requested tool. Only the parsed spec and the Windows
// installed-toolset probe outlive the branch that computes them; every other
// local below (the first-run defaults, the explicit-spec resolution's own
// payload/frontend locals, and so on) is read only within the one step that
// declares it and stays a plain local there, exactly as it was in the single
// function this splits.
struct ToolchainResolveCtx {
    std::optional<mcpp::toolchain::ToolchainSpec> parsedSpec;
    // Windows only (see step2_parse_toolchain_spec); resolvable on every
    // platform so the struct itself has one shape.
    std::optional<mcpp::toolchain::msvc::MsvcInstallation> installedPin;
    std::vector<std::string> installedPinNotes;
};

static std::expected<void, std::string>
step2_parse_toolchain_spec(PrepareState& state, ToolchainResolveCtx& ctx) {
      auto tcOriginAxis = mcpp::toolchain::Origin::Managed;
      if (state.tcSpec.has_value() && *state.tcSpec != "system") {
        // A parse FAILURE is not the same as an unparseable spec being
        // absent: `gcc@system` now fails here by name (see
        // parse_toolchain_spec), and swallowing that would put the error back
        // where it used to happen — somewhere else, saying something else.
        auto s = mcpp::toolchain::parse_toolchain_spec(*state.tcSpec);
        if (!s) return std::unexpected(std::format(
            "{} = '{}': {}", state.tcSpecSource(), *state.tcSpec, s.error()));
        ctx.parsedSpec = std::move(*s);
        tcOriginAxis = mcpp::toolchain::origin_of(*ctx.parsedSpec);
      }
      // ASSIGNED, NOT DECLARED. `host_tc_for_build_program` reads it and is
      // defined outside this lambda, so the declaration lives in the enclosing
      // scope; the value is still decided here, where the spec is parsed.
      state.tcSpecIsMsvc =
        ctx.parsedSpec && tcOriginAxis == mcpp::toolchain::Origin::SystemMsvc;

      // A PINNED TOOLSET THIS MACHINE ALREADY HAS IS USED WHERE IT IS.
      //
      // `msvc@14.44.35207` names one Microsoft build, and the ecosystem package
      // of that version unpacks the same installer payloads Visual Studio does,
      // so an installed copy is the same toolset without a download. `xim:`
      // opts out: it asks for the package, whose SDK is pinned with it.
      if constexpr (mcpp::platform::is_windows) {
          if (ctx.parsedSpec && !state.tcSpecIsMsvc
              && ctx.parsedSpec->family == mcpp::toolchain::Family::Msvc
              && !ctx.parsedSpec->ecosystemOnly && !ctx.parsedSpec->version.empty())
              ctx.installedPin = mcpp::toolchain::msvc::system_installation_matching(
                  ctx.parsedSpec->version, mcpp::toolchain::msvc::ToolsetNeeds{},
                  &ctx.installedPinNotes);
      }
      return {};
}

static void
step2_use_installed_pin(PrepareState& state, ToolchainResolveCtx& ctx) {
        for (auto const& n : ctx.installedPinNotes) mcpp::ui::info("note", n);
        state.explicit_compiler = ctx.installedPin->clPath;
        mcpp::ui::info("Resolved", std::format(
            "{} → msvc {} (installed: {})", ctx.parsedSpec->display(),
            ctx.installedPin->display_version(), ctx.installedPin->clPath.string()));
}

static std::expected<void, std::string>
step2_use_system_msvc(PrepareState& state) {
        if (!mcpp::platform::is_windows) {
            return std::unexpected(std::format(
                "toolchain '{}' is only available on Windows hosts", *state.tcSpec));
        }
        auto inst = mcpp::toolchain::msvc::detect_installation();
        if (!inst) {
            return std::unexpected(mcpp::toolchain::msvc::install_guidance());
        }
        state.explicit_compiler = inst->clPath;
        mcpp::ui::info("Resolved", std::format(
            "msvc@system → msvc {} ({})",
            inst->display_version(), inst->clPath.string()));
        return {};
}

static std::expected<void, std::string>
step2_resolve_explicit_spec(PrepareState& state, ToolchainResolveCtx& ctx) {
        auto spec = ctx.parsedSpec;
        if (spec->version.empty()) {
            return std::unexpected(std::format(
                "{} = '{}' is invalid; expected '<pkg>@<version>'",
                state.tcSpecSource(), *state.tcSpec));
        }
        // A `--target <triple>` build carries the (already canonical) triple
        // into the spec's target axis: the payload mapping then resolves the
        // right package/frontend (e.g. aarch64-linux-musl-g++ for a cross
        // musl build, never the host g++). Escape-hatch triples outside the
        // language don't parse and leave the spec on the host target.
        if (!state.overrides.target_triple.empty()) {
            if (auto t = mcpp::toolchain::triple::parse(state.overrides.target_triple))
                spec->target = *t;
        }
        auto pkg = mcpp::toolchain::to_xim_package(*spec);

        // AND INSTALLED ANYWAY WHEN THE USER DECLARED IT, SKIPPED ONLY WHEN
        // THE ENGINE CHOSE IT.
        //
        // `unservedTargetDiagnosis` is decided a thousand lines above and
        // released a thousand lines below — deliberately, because whether the
        // dependency GRAPH supplies the target's system is not knowable until
        // it is resolved. This install sits between the two.
        //
        // A DECLARED toolchain installs anyway. The held diagnosis means no
        // payload HERE produces the target, not that the target is
        // unbuildable: a retargetable clang plus a graph package supplying
        // the target's system is exactly the arrangement the openkal rows
        // exist for. Skipping the install behind the diagnosis was tried and
        // measured twice (mcpp#782): the openkal macos leg resolved
        // `llvm@22.1.8` for years because the suite installed it out of
        // band, and both the line move (autoInstall skip) and, on the next
        // run, even a successful 23.1.3 install still died — the skip and
        // the spec's target axis together made the resolution refuse before
        // the graph release could ever run. A user's declaration outranks
        // the payload matrix (the same standing the `[target.X] toolchain`
        // escape hatch has).
        //
        // An ENGINE-CHOSEN toolchain skips. There the spec carries the
        // target axis because the ROW asked for it (e.g. the musl rows), the
        // package is published per host arch, and on a host of the wrong
        // arch the install cannot succeed — measured on ubuntu-24.04-arm,
        // `--target x86_64-linux-musl`: xim:x86_64-linux-musl-gcc@16.1.0 is
        // x86_64-only, and the hard install failure used to preempt the
        // refusal that names the target correctly. The held diagnosis is
        // that refusal, released early with one cause per message. The same
        // rule applies to an unservable foreign GNU payload. Native ARM64
        // GNU now has a managed LLVM/glibc payload and does not enter this
        // refusal path.
        const bool engineChoseUnservable =
            !state.unservedTargetDiagnosis.empty() && !spec->target.empty()
            && !tc_origin_is_user_explicit(state.tcOrigin);

        auto cfg = state.get_cfg(true);
        if (!cfg) return std::unexpected(cfg.error());
        mcpp::fetcher::Fetcher fetcher(**cfg);

        mcpp::ui::info("Resolving", "toolchain");
        mcpp::fetcher::InstallProgressHandler progress;
        auto payload = fetcher.resolve_xpkg_path(pkg.target(), /*autoInstall=*/!engineChoseUnservable, &progress);
        if (!payload && !state.unservedTargetDiagnosis.empty() && !spec->target.empty()) {
            // The held diagnosis is already the right words for this; releasing
            // it here rather than at its usual site keeps one sentence per cause.
            refusal::record(refusal::Code::HostCannotServe);
            return std::unexpected(state.unservedTargetDiagnosis);
        }
        if (!payload) {
            // `windows = "msvc@19.44"` in a manifest is the retired
            // cl-version spelling; saying "no such xim package" would send
            // the reader looking for a toolset that cannot exist.
            if (spec->family == mcpp::toolchain::Family::Msvc) {
                if (auto hint = mcpp::toolchain::msvc::cl_version_spelling_hint(
                        spec->version))
                    return std::unexpected(*hint);
            }
            return std::unexpected(std::format(
                "toolchain '{}': {}", *state.tcSpec, payload.error().message));
        }

        // A pinned MSVC toolset: the payload root IS a VS-shaped root and the
        // package version IS the toolset directory name, so cl.exe is
        // derived, not searched for. Nothing here can silently pick a
        // different toolset — which is the defect this path exists to close.
        //
        // It also skips the two steps below: the bin/-shaped frontend lookup
        // (cl.exe is four levels deeper) and the ELF post-install fixup
        // (there is nothing to patchelf on a PE toolchain).
        if (spec->family == mcpp::toolchain::Family::Msvc) {
            // One rule, one place: where a managed toolset lives and why the
            // fetcher's `root` must not be used for it (mcpp.toolchain.
            // registry). Install and build asked the same question and each
            // answered it in its own words.
            auto inst = mcpp::toolchain::resolve_managed_msvc(
                mcpp::config::make_xlings_env(**cfg), pkg);
            if (!inst) return std::unexpected(inst.error());
            state.explicit_compiler = inst->clPath;
            mcpp::ui::info("Resolved", std::format(
                "{} → msvc {} ({})", spec->display(),
                inst->display_version(), inst->clPath.string()));
        } else {
            auto frontendR = mcpp::toolchain::payload_frontend(payload->root, pkg);
            // A payload that describes itself and describes itself wrongly is
            // refused by name -- not reported as a missing frontend, which is
            // a different repair.
            if (!frontendR) return std::unexpected(frontendR.error());
            state.explicit_compiler = *frontendR;
            if (!std::filesystem::exists(state.explicit_compiler)) {
                return std::unexpected(std::format(
                    "toolchain payload '{}' has no known C++ frontend in {}",
                    pkg.target(),
                    mcpp::toolchain::payload_frontend_dir(payload->root, pkg).string()));
            }
            // Same post-install fixup as `mcpp toolchain install` — this
            // manifest [toolchain] path previously ran none, so a freshly
            // auto-installed payload kept its stale install-time cfg /
            // unpatched runtime libs.
            state.provide_runtime_payload(pkg);
            if (auto fixed = mcpp::toolchain::ensure_post_install_fixup(
                    **cfg, payload->root, pkg,
                    state.runtimeBindingSnapshot.runtimeId, state.runtimeLibDir); !fixed)
                return std::unexpected(std::format(
                    "toolchain post-install fixup: {}", fixed.error()));
            else state.report_fixup(*fixed, payload->root);
            // Canonical rendering, whatever spelling the manifest/config used:
            // "Resolved gcc@16.1.0 → x86_64-linux-musl → <frontend>".
            //
            // AND IT SAYS SO WHEN MCPP CHOSE. A toolchain the user wrote down
            // needs no explanation — they can read their own manifest. One this
            // engine selected from a target row is a decision the user did not
            // make, and a status line that reports the outcome without the
            // reason leaves them to discover the rule by experiment.
            std::string chosenBy;
            // A COMPILER THE GRAPH ASKED FOR IS ANNOUNCED WITH THE PACKAGE
            // THAT ASKED. Without the name this reads as mcpp ignoring the
            // user's default; with it, it reads as the dependency it is.
            // The second line appears only when something was displaced —
            // "replacing nothing" is not worth a line.
            if (!state.graphCompilerRequiredBy.empty())
                chosenBy = std::format(
                    "\n             required by {} (`requires = "
                    "[\"mcpp:compiler={}\"]`){}",
                    state.graphCompilerRequiredBy, state.graphCompilerFamily,
                    state.graphCompilerReplaced.empty()
                        ? std::string{}
                        : std::format(", not your {} — this project only",
                                      state.graphCompilerReplaced));
            else if (!state.pinReplacedDefault.empty())
                chosenBy = std::format(
                    "\n             target default for {}, replacing your "
                    "{} — override with `[target.{}] toolchain`",
                    state.overrides.target_triple, state.pinReplacedDefault,
                    state.overrides.target_triple);
            else if (state.tcOrigin == TcOrigin::TargetPin
                  || state.tcOrigin == TcOrigin::FirstRun)
                chosenBy = std::format("  ({})", tc_origin_name(state.tcOrigin));
            mcpp::ui::info("Resolved",
                std::format("{} → {}{}", spec->display(),
                    mcpp::ui::shorten_path(state.explicit_compiler,
                        mcpp::fetcher::make_path_ctx(&**state.get_cfg(true), *state.root)),
                    chosenBy));
        }
        return {};
}

static std::expected<void, std::string>
step2_system_toolchain_refusal(PrepareState& state) {
        // REFUSED. THE COMPILER IS THE ONE AXIS THAT IS NOT THE PROJECT'S TO
        // TAKE FROM THE HOST.
        //
        // mcpp's host-dependence policy is not uniform across axes, and the
        // split is the point rather than an inconsistency:
        //
        //   LIBRARIES are the program's business. A project may link a host
        //   library or its own `.so`; mcpp says what that costs and what the
        //   supported route is, and does not refuse as long as the result
        //   builds and runs. The developer owns the artifact and guarantees it.
        //
        //   THE TOOLCHAIN is mcpp's own contract. Everything mcpp promises —
        //   that `import std` is available, that the runtime closure is
        //   computable, that two machines and CI produce the same build — is a
        //   statement about a compiler mcpp resolved and can identify. A
        //   compiler picked off `PATH` makes every one of those promises
        //   unverifiable, and a build tool that cannot state what it built with
        //   is answering in the wrong version (see
        //   `.agents/docs/…a-build-must-be-able-to-state-its-own-version`).
        //
        // So this is refused rather than warned about, and it is refused HERE,
        // before any resolution work, so the message is the first thing the
        // user sees rather than a consequence three layers down.
        //
        // `msvc@system` is a different spelling and stays supported: it names a
        // FAMILY whose installation mcpp locates and identifies, on the one
        // platform where the compiler cannot be redistributed.
        return std::unexpected(std::format(
            "[toolchain] {} = \"system\" is not supported: mcpp builds only "
            "with toolchains it manages.\n"
            "       A compiler taken from PATH cannot be identified or "
            "reproduced, so `import std` availability, the runtime closure and "
            "\"the same build on another machine\" all stop being things mcpp "
            "can promise.\n"
            "       Name one instead — mcpp installs it on first use:\n"
            "\n"
            "         [toolchain]\n"
            "         {} = \"gcc@16.1.0\"\n"
            "\n"
            "       or set a machine default with `mcpp toolchain default "
            "gcc@16.1.0`, and see `mcpp toolchain list` for what is available.\n"
            "       (On Windows, `msvc@system` is different and remains "
            "supported: it names a family whose installation mcpp locates.)\n"
            "       Host LIBRARIES are a separate question and are not refused "
            "— a project may link them and owns the result.",
            kCurrentPlatform, kCurrentPlatform));
}

static std::expected<void, std::string>
step2_offline_refusal(PrepareState& state) {
        // CI / offline / test opt-out: hard-error instead of silently
        // pulling ~800 MB of toolchain. Preserves the original M5.5
        // contract for environments that need it.
        //
        // `--offline` / MCPP_OFFLINE subsumes MCPP_NO_AUTO_INSTALL: the older
        // name only ever covered this one gate, which made "don't use the
        // network" three separate concepts with three spellings. The old var is
        // kept working (it predates offline mode and CI still exports it).
        namespace pins = mcpp::toolchain::triple::pins;
        // Name the knob that actually fired, not a fixed one: telling a user
        // who passed `--offline` to unset MCPP_NO_AUTO_INSTALL sends them
        // looking for a variable they never set.
        std::string_view release = mcpp::platform::env::offline_mode()
            ? "or drop --offline / unset MCPP_OFFLINE to let mcpp auto-install."
            : "or unset MCPP_NO_AUTO_INSTALL to let mcpp auto-install.";
        // Windows without a usable MSVC must not be told to install llvm:
        // that default resolves to clang targeting the MSVC ABI, which is
        // exactly what this machine cannot build. Name the toolchain that
        // will actually work there instead.
        if (mcpp::platform::is_windows
            && !state.msvc_usable_either_origin()) {
            refusal::record(refusal::Code::OfflineDownloadRequired);
            return std::unexpected(std::format(
                "no toolchain configured (and no Visual Studio found).\n"
                "       run one of:\n"
                "         mcpp toolchain install {} --target {}\n"
                "         mcpp toolchain default {} --target {}\n"
                "       {}",
                pins::kSuggestGccMingw, pins::kFirstRunWinGnuTarget,
                pins::kFirstRunWinGnu,  pins::kFirstRunWinGnuTarget, release));
        }
        if constexpr (mcpp::platform::is_macos || mcpp::platform::is_windows) {
            refusal::record(refusal::Code::OfflineDownloadRequired);
            return std::unexpected(std::format(
                "no toolchain configured.\n"
                "       run one of:\n"
                "         mcpp toolchain install {}\n"
                "         mcpp toolchain default {}\n"
                "       {}",
                pins::kSuggestLlvm, pins::kFirstRunMac, release));
        } else {
            refusal::record(refusal::Code::OfflineDownloadRequired);
            return std::unexpected(std::format(
                "no toolchain configured.\n"
                "       run one of:\n"
                "         mcpp toolchain install {}\n"
                "         mcpp toolchain default {}\n"
                "       {}",
                pins::kSuggestGccMusl, pins::kFirstRunLinuxOther, release));
        }
}

static std::expected<void, std::string>
step2_first_run_auto_install(PrepareState& state) {
        // First-run UX: no project-level [toolchain], no global default,
        // and the user just ran `mcpp build` (or similar). Auto-install
        // the platform's canonical default so the user gets a working
        // binary out of the box without any config. We pin it as the
        // global default so the next invocation is silent.
        // Users can switch any time via `mcpp toolchain default <spec>`.
        //
        // macOS: LLVM/Clang — Apple doesn't ship GCC; upstream LLVM with
        //        bundled libc++ is the self-contained choice.
        // Linux: glibc gcc — the platform-native ABI. A musl-static default
        //        cannot link the glibc world (X11/GL/system libs), so it
        //        breaks GUI/native packages out of the box. musl-static stays
        //        opt-in via `mcpp build --target x86_64-linux-musl` for users
        //        who explicitly want portable static binaries.
        // Linux default is arch-aware, and the table is
        // `pins::host_default_toolchain` (`mcpp self env` reports it):
        //   x86_64  → glibc gcc (native ABI). musl-static stays opt-in via
        //             --target.
        //   aarch64 → llvm with the managed glibc (2026.10.8.1), on GNU/Linux
        //             and on Android (Termux) alike: an Android host is
        //             recognised (`linux_::is_android_host`) but not given a
        //             different toolchain (design 2026-10-10 §13, D21).
        // `native_first_run_spec()` (declared above) is this exact selection
        // — on Windows it re-checks `msvc_usable_either_origin()`, which here
        // is redundant (the seed above already diverted the unusable case
        // onto the windows-gnu target before this block runs) but harmless.
        std::string defaultSpec = state.native_first_run_spec();
        auto defaultParsed = mcpp::toolchain::parse_toolchain_spec(defaultSpec);
        // The legacy "-musl" spelling normalizes to (gcc, <host>-linux-musl),
        // so the resolver finds the `<host_arch>-linux-musl-g++` frontend
        // without any manual triple seeding.
        bool muslDefault = defaultParsed->target.is_musl();
        auto defaultPkg = mcpp::toolchain::to_xim_package(*defaultParsed);

        if constexpr (mcpp::platform::is_macos || mcpp::platform::is_windows) {
            mcpp::ui::info("First run",
                std::format("no toolchain configured — installing {} (LLVM/Clang) as default",
                            defaultSpec));
        } else {
            mcpp::ui::info("First run",
                std::format("no toolchain configured — installing {} ({}) as default",
                            defaultSpec, muslDefault ? "musl, static" : "glibc, native ABI"));
        }

        auto cfg = state.get_cfg(true);
        if (!cfg) return std::unexpected(cfg.error());
        mcpp::fetcher::Fetcher fetcher(**cfg);

        mcpp::fetcher::InstallProgressHandler progress;
        auto payload = fetcher.resolve_xpkg_path(defaultPkg.target(),
                            /*autoInstall=*/true, &progress);
        if (!payload) {
            return std::unexpected(std::format(
                "auto-installing default toolchain {} failed: {}\n"
                "       you can install it manually with:\n"
                "         mcpp toolchain install {}",
                defaultSpec, payload.error().message, defaultSpec));
        }
        auto defaultFrontendR =
            mcpp::toolchain::payload_frontend(payload->root, defaultPkg);
        if (!defaultFrontendR) return std::unexpected(defaultFrontendR.error());
        state.explicit_compiler = *defaultFrontendR;
        if (!std::filesystem::exists(state.explicit_compiler)) {
            return std::unexpected(std::format(
                "default toolchain payload {} has no known C++ frontend in {}",
                defaultPkg.target(),
                mcpp::toolchain::payload_frontend_dir(payload->root, defaultPkg).string()));
        }

        // The freshly-installed toolchain needs the SAME post-install fixup
        // (patchelf / specs / cfg wiring against the sandbox glibc) that
        // `mcpp toolchain install` performs — without it a fresh sandbox
        // gcc cannot find the C library (stdlib.h: No such file or
        // directory) and a fresh llvm keeps its stale install-time cfg.
        state.provide_runtime_payload(defaultPkg);
        if (auto fixed = mcpp::toolchain::ensure_post_install_fixup(
                **cfg, payload->root, defaultPkg,
                state.runtimeBindingSnapshot.runtimeId, state.runtimeLibDir); !fixed)
            return std::unexpected(std::format(
                "default toolchain post-install fixup: {}", fixed.error()));
        else state.report_fixup(*fixed, payload->root);

        // Persist the default so we don't ask again next time.
        if (auto wr = mcpp::config::write_default_toolchain(**cfg, defaultSpec); wr) {
            (*cfg)->defaultToolchain = defaultSpec;
            mcpp::ui::status("Default", std::format("set to {}", defaultSpec));
        } // best-effort: a failed config write only loses the persistence,
          // not the running build.
        state.tcSpec   = defaultSpec;
        state.tcOrigin = TcOrigin::FirstRun;

        // AND IF A TARGET WAS ASKED FOR, RESOLVE FOR IT — THIS BRANCH JUST
        // INSTALLED A HOST COMPILER AND WAS ABOUT TO BUILD WITH IT.
        //
        // Everything above answers "this machine has no toolchain, give it
        // one", and the answer is a HOST payload. `--target` was never read
        // here, so on a machine that had never built anything,
        // `mcpp build --target x86_64-windows-gnu` installed a native gcc and
        // compiled Windows sources with it. Measured in CI 2026-08-25:
        //
        //     First run  no toolchain configured — installing gcc@16.1.0 …
        //      Resolved  gcc@16.1.0 → …/xim-x-gcc/16.1.0/bin/g++
        //                                        ↑ no target in the path
        //
        // against the same command on a machine that already had one:
        //
        //      Resolved  gcc@16.1.0 → x86_64-windows-gnu → …/mingw-cross-gcc/…
        //
        // REUSES THE PATH THAT ALREADY KNOWS HOW, rather than repeating what
        // it does. `resolve_target_toolchain` maps a spec plus a target onto a
        // payload and installs it; the default just chosen is the spec. A
        // second implementation here would be a second answer to one question,
        // which is the shape this release exists to remove.
        // RECORDED HERE, ACTED ON BELOW — the Windows first-run block that
        // follows SETS `state.overrides.target_triple` itself, and returning from
        // here would skip it. Its own comment says why that matters: it
        // persists BOTH axes, and persisting only the target leaves
        // `mcpp toolchain list` disagreeing with what the build used.
        state.firstRunNeedsTargetPass = !state.overrides.target_triple.empty();
        return {};
}

static void
step2_windows_gnu_first_run_persist(PrepareState& state) {
      // Windows first run that got diverted to winlibs GCC: announce it and
      // persist BOTH axes, so the next invocation is silent and
      // `mcpp toolchain list` shows the same pair the build actually used.
      // Persisting only the target would leave the toolchain axis implicit
      // (derived from the vocabulary pin) and the two views would disagree.
      //
      // NOT WHEN THE DEPENDENCY GRAPH SUPPLIED THE ANSWER. This branch's
      // condition is `tcSpec.has_value()`, and since 2026.8.26.2 a package's
      // `requires = ["mcpp:compiler=…"]` can be what made it true — so a bare
      // Windows box building ONE project with an llvm-requiring dependency
      // would have persisted llvm as the MACHINE's default, and the next
      // project, which asked for nothing, would inherit it.
      //
      // A requirement is a property of the package that states it. It decides
      // this build and nothing else; the first-run answer for the machine is
      // still the one this branch was written for.
      if (state.windowsGnuFirstRun && state.tcSpec.has_value()
          && tc_origin_may_persist(state.tcOrigin)) {
        mcpp::ui::info("First run",
            std::format("no toolchain configured and no Visual Studio found — "
                        "using {} for {} (MinGW-w64, self-contained)",
                        *state.tcSpec, state.overrides.target_triple));
        if (auto cfgW = state.get_cfg(true); cfgW) {
            if (mcpp::config::write_default_toolchain(**cfgW, *state.tcSpec))
                (*cfgW)->defaultToolchain = *state.tcSpec;
            if (mcpp::config::write_default_target(**cfgW, state.overrides.target_triple))
                (*cfgW)->defaultTarget = state.overrides.target_triple;
            mcpp::ui::status("Default",
                std::format("set to {} → {}", *state.tcSpec, state.overrides.target_triple));
        }
        state.tcOrigin = TcOrigin::FirstRun;
      }
}

static std::expected<void, std::string>
step2_detect_toolchain(PrepareState& state) {
      auto detected = mcpp::toolchain::detect(
          state.explicit_compiler, state.runtimePayload, state.runtimeBindingSnapshot.contractHash);
      if (!detected) return std::unexpected(detected.error().message);
      state.tc = std::move(*detected);

      // Something about the resolution the user has to be told, but which is
      // not a failure. Today's only producer is the Windows SDK axis: a managed
      // toolset binds the SDK it was installed with, so a `WindowsSdkDir` in
      // the environment does not apply — and an override that is ignored
      // SILENTLY is indistinguishable from one that was never set.
      if (!state.tc->resolutionNote.empty())
          mcpp::ui::info("note", state.tc->resolutionNote);
      return {};
}

static std::expected<void, std::string>
step2_retarget_for_retargetable_driver(PrepareState& state) {
      // ── A retargetable driver has to be TOLD what it is targeting ────────
      //
      // `tc.targetTriple` comes from `-dumpmachine`, and for every cross target
      // that worked before this it was right for a reason that does not
      // generalise: those targets use a DISTINCT compiler binary
      // (`x86_64-w64-mingw32-g++`, `aarch64-linux-musl-g++`), whose own
      // -dumpmachine reports the cross triple. Clang is ONE binary that emits
      // every target it was built with, so -dumpmachine always answers with the
      // host — and nothing downstream ever learns otherwise.
      //
      // Measured before this line existed:
      //
      //   $ mcpp build --target riscv64-none-elf
      //       Resolved llvm@22.1.8 → riscv64-none-elf → …/bin/clang++
      //       Finished dev [unoptimized + debuginfo] in 0.47s
      //   $ ls target/
      //       x86_64-linux-gnu/          ← an ELF for the host, reported as riscv64
      //
      // That is E1: success reported, host artifact produced. The output
      // directory, the fingerprint, the cache key and the flag layer all read
      // `tc.targetTriple`, so correcting it here corrects all of them at once —
      // which is the point of there being one field rather than five answers.
      //
      // THIS USED TO BE SCOPED TO FREESTANDING, WITH THIS REASON:
      //
      //     The hosted cross targets already resolve a per-target binary, and
      //     overwriting their probed triple would replace a measured fact with
      //     an assumed one for no gain.
      //
      // That was true while every hosted cross was served by a payload. It
      // stops being true when the TARGET SIDE comes from the dependency graph:
      // the C library, the C++ runtime and the platform's own implementation are
      // then packages built from source, and the compiler is an ordinary clang —
      // whose `-dumpmachine` answers the host, exactly as the paragraph above
      // describes for freestanding.
      //
      // Measured 2026-08-23, with an explicit `[target.aarch64-macos]
      // toolchain = "llvm@…"`. The manifest's cfg evaluation used the REQUESTED
      // target, so the C library's aarch64 headers were on the command line; the
      // toolchain's own triple was still the host's, so code generation was
      // x86_64. Two answers to one question, in one command:
      //
      //     okm_float_assert.c: the C library and the compiler disagree about
      //     LDBL_DIG  ('33 == 18')          33 = aarch64 binary128, 18 = x87
      //
      // ⇒ The condition is now the property the first paragraph of this comment
      // already names: a RETARGETABLE driver has to be told. gcc is not one — a
      // gcc payload IS its target — so the mingw and musl-gcc crosses keep
      // answering from `-dumpmachine`, which for them remains a measured fact.
      if (!state.overrides.target_triple.empty()) {
          if (auto want = mcpp::toolchain::triple::parse(state.overrides.target_triple);
              want && (want->is_freestanding()
                       || state.tc->compiler == mcpp::toolchain::CompilerId::Clang))
          {
              state.tc->targetTriple = want->str();

              // AND THE GATE THAT ALREADY EXISTS FOR THIS, APPLIED WHERE THE
              // ANSWER IS KNOWN.
              //
              // `discover_link_runtime_dirs` refuses to report these
              // directories for a target that carries its own sysroot, and the
              // refusal never fired: that function runs during DETECTION,
              // before this line, when `targetTriple` is still the HOST's. The
              // gate read a host triple and answered correctly about it.
              //
              // The artefact is what showed it. An Android link line carried
              //
              //     -L <ndk>/toolchains/llvm/prebuilt/linux-x86_64/lib/
              //        x86_64-unknown-linux-gnu
              //
              // whose last component is this machine's triple, produced by
              // `root / "lib" / targetTriple` -- so the string names the
              // question that was asked. Those are the compiler's own host
              // runtime directories; an Android artefact must resolve libc++,
              // the crt objects and the loader from the NDK's sysroot, and the
              // hermetic check reported exactly that failure with six host
              // objects.
              //
              // Cleared rather than re-derived. Re-running the discovery with
              // the final triple would also change what every OTHER clang cross
              // target gets, and those are measured as they stand; the claim
              // being made here is only the one the gate already states.
              if (want->has_own_sysroot()) state.tc->linkRuntimeDirs.clear();

              // And the flag that says it to the driver — for a HOSTED target
              // only. Freestanding already emits its own `--target`, together
              // with the ISA flags that must accompany it
              // (freestanding/target.cppm); a second one here would be the same
              // decision in two places.
              if (!want->is_freestanding()
                  && state.tc->compiler == mcpp::toolchain::CompilerId::Clang) {
                  state.tc->crossTargetFlag =
                      "--target=" + want->llvm_triple(
                          min_platform_version(*state.m, *want, state.tc->binaryPath));

                  // AND THE SAME FLAG ON THE std MODULE'S OWN COMMANDS, FOR A
                  // PAYLOAD THAT SERVES MORE THAN ONE TARGET.
                  //
                  // The std module is built by its own command assembly
                  // (clang.cppm), not by the compile flags, so a decision made
                  // only here reaches every translation unit and not that. For
                  // most toolchains the omission cannot be seen: a payload
                  // whose compiler IS its target finds its own headers, and a
                  // package-provided module carries the target inside
                  // `stdModuleFlags`.
                  //
                  // ONE NDK SERVES BOTH ANDROID ARCHES, which is the property
                  // that makes this necessary and is stated in the row's own
                  // pin: `android-ndk@<v>` names no arch, so `--target` is the
                  // only thing that says which. Without it the precompile
                  // resolved libc++'s `#include <__config>` against the
                  // building machine and stopped there.
                  //
                  // NOT `has_own_sysroot()`, though both rows that answer true
                  // to it are SDKs with their own sysroot. Emscripten's `em++`
                  // serves exactly one target and needs no flag -- the verified
                  // wasm loop is measured without it -- so widening the gate to
                  // the predicate would add a flag to a command that does not
                  // want one. The property here is "one payload, several
                  // targets", and Android is the only row that has it; a future
                  // row brings its own measurement.
                  if (want->is_android()) {
                      state.tc->stdModuleTargetFlags = " " + state.tc->crossTargetFlag;
                      // BIONIC'S ctype HEADER AND A MODULE'S EXPORT RULES.
                      //
                      // bionic declares `isalnum` and its neighbours
                      // `static inline`, and libc++'s module surface exports
                      // them with `using std::isalnum`. A using-declaration
                      // cannot export a name with internal linkage, so the
                      // precompile fails on 14 names at once. Defining the
                      // macro empty makes those declarations extern, which is
                      // what every other C library this engine compiles
                      // against already does.
                      //
                      // Scoped to the std module and not to every unit: the
                      // rule being satisfied is about exporting from a module,
                      // and a translation unit that includes <ctype.h>
                      // directly is entitled to bionic's inline definitions.
                      // `xim:android-ndk`'s own install-time self-test reaches
                      // the identical conclusion from the other direction.
                      //
                      // AND THE PAYLOAD MAY SAY SO ITSELF. The recipe applies
                      // this same define in that self-test, so it is a fact
                      // the payload already holds; `std_module_defines` in
                      // `.mcpp-toolchain.json` is the channel for it, and the
                      // define below is what a payload that ships no
                      // descriptor still gets. The two are not added
                      // together: a descriptor that names defines is the
                      // payload's complete answer for this channel, and
                      // appending to it would mean a payload could not
                      // withdraw a define this engine once needed.
                      auto stdDefines = [&]() -> std::vector<std::string> {
                          auto desc =
                              mcpp::toolchain::payload_descriptor_for_compiler(
                                  state.tc->binaryPath);
                          if (desc && *desc && !(*desc)->stdModuleDefines.empty())
                              return (*desc)->stdModuleDefines;
                          return { "__BIONIC_CTYPE_INLINE=" };
                      }();
                      for (auto const& def : stdDefines)
                          state.tc->stdModuleTargetFlags += " -D" + def;
                  }

                  // ── iOS: THE COMPILER IS OURS, THE SDK IS THE MACHINE'S ──
                  //
                  // The three iOS rows pin `llvm@23.1.3` -- any sufficiently
                  // new clang emits arm64 Mach-O for an iOS deployment target
                  // -- and take their headers and stub libraries from the
                  // machine's Xcode, which is where the whole item shrinks to
                  // a located sysroot. `aarch64-macos` is verified on exactly
                  // this split and is the precedent.
                  //
                  // LOCATED HERE, ONCE. Three later sites need the answer (the
                  // compile flags, the link line, and the std module's own
                  // command), and a function that probes the machine is the
                  // wrong thing to call three times: `xcrun` shells out, and
                  // three answers can differ if the developer directory
                  // changes mid-build.
                  //
                  // AND ITS ABSENCE IS A REFUSAL THAT NAMES THE SDK. The
                  // recorded host-surface rule is that a host dependency must
                  // be minimal, named, and never a fallthrough; the iOS SDK
                  // and `simctl` are the two this platform adds, both in the
                  // "proprietary runtime that exists only on its own OS"
                  // category. A build that continued without the SDK would
                  // fail in the driver's header search, naming a file rather
                  // than the thing that is missing.
                  if (want->is_ios()) {
                      // READ, NOT RE-DERIVED. The refusal above located it
                      // before any payload was resolved, and that is the one
                      // `xcrun` call this build makes.
                      //
                      // An empty answer here cannot happen through the
                      // `--target` path, and a line that prints when it does
                      // is cheaper than a branch that pretends it cannot: the
                      // row could be reached one day by a route that skipped
                      // the gate, and an iOS build with no `-isysroot` is a
                      // macOS artefact with an iOS triple on it.
                      if (!state.appleSdkLocated) {
                          return std::unexpected(std::format(
                              "internal: target {} reached toolchain "
                              "resolution without its SDK being located; the "
                              "gate that locates it did not run for this "
                              "request", want->str()));
                      }
                      state.tc->appleSdkRoot = *state.appleSdkLocated;
                      auto sdk = state.appleSdkLocated;
                      // AND THE std MODULE'S OWN COMMAND, WHICH IS A SEPARATE
                      // CHANNEL. Same reason the Android rows set it: the
                      // module is precompiled by `clang.cppm`'s own assembly
                      // rather than by the compile flags, so a decision made
                      // only in the flag builder reaches every translation
                      // unit and not the module they all import. Without the
                      // SDK here the precompile resolves libc++'s
                      // `#include <__config>` against the macOS SDK and the
                      // module is built for the wrong platform.
                      //
                      // QUOTED, as every path this string carries is (see the
                      // package-provided producer, which uses `shq` for each
                      // `-isystem`). The string is spliced into a shell
                      // command, and an Xcode installed as `Xcode 16.app` is
                      // a path with a space in it.
                      state.tc->stdModuleTargetFlags =
                          " " + state.tc->crossTargetFlag
                          + " -isysroot " + mcpp::xlings::shq(sdk->string());
                  }
              }
          }
          if (auto want = mcpp::toolchain::triple::parse(state.overrides.target_triple);
              want && want->is_freestanding())
          {
              // `import std` is structurally hosted, and turning it off is the
              // SAME fact as the line above, not a second policy: libc++'s
              // std.cppm is one module over the whole library, including the
              // parts that are threads, filesystem and iostreams. There is no
              // subset of it to precompile.
              //
              // Left on, the failure is neither early nor legible — measured:
              //
              //   error: std module precompile failed (rc=1):
              //   .../include/c++/v1/__config:13:10: fatal error:
              //       '__config_site' file not found
              //
              // which reads as a broken toolchain payload and says nothing about
              // the target. The freestanding std subset a user actually wants is
              // an ordinary package (`mcpplibs.std.freestanding`), so mcpp's job
              // here is to stop pretending the hosted one exists and to say
              // where the other one is.
              state.tc->clear_std_modules();

              // ── The target's C library, resolved like its compiler ─────────
              //
              // The row in kKnownTargets names it, exactly as it names the
              // toolchain pin, and it is installed through the same channel a
              // project's `[xlings] deps` use (see the materialization above).
              // Resolved HERE because the config is already open; the flag
              // builder only reads the result.
              //
              // Absent is not an error at this point: the install happens
              // earlier in this function and may legitimately not have run yet
              // on a first pass. What follows would then simply not add the
              // paths, and the link fails naming the missing libc — which is the
              // truthful message either way.
              if (const std::string want_sysroot =
                      mcpp::toolchain::triple::effective_sysroot(
                          *want, sysroot_override(*state.m, *want));
                  !want_sysroot.empty()) {
                  if (auto cfg3 = state.get_cfg(true); cfg3) {
                      auto ref = mcpp::xlings::paths::parse_xpkg_ref(want_sysroot);
                      auto xl  = mcpp::config::make_xlings_env(**cfg3);
                      // INSTALLED, NOT MERELY LOOKED UP — THE SAME CHANNEL
                      // THE ROW'S TOOLCHAIN PIN GOES THROUGH.
                      //
                      // The row names two things and only one of them used to
                      // be made to exist: `pin` went through
                      // `resolve_xpkg_path(…, autoInstall=true, …)` while
                      // `sysroot` was a pure lookup that returned nullopt and
                      // let the whole block below be skipped without a word.
                      //
                      // Measured 2026-08-26 in a clean environment (an empty
                      // home, so mcpp's registry starts fresh):
                      //
                      //     Target riscv64-none-elf
                      //            c-abi  picolibc-riscv (…, prebuilt)
                      //     error: 'stdio.h' file not found
                      //
                      // The report named the C library and the build could not
                      // find its headers. mcpp's own bare-metal CI installs it
                      // by hand, which is why no test ever saw this — every
                      // bare-metal e2e runs on a machine where the gap has
                      // already been papered over.
                      //
                      // OFFLINE AND `MCPP_NO_AUTO_INSTALL` ARE THE FETCHER'S
                      // DECISION, not re-derived here. One question, one place
                      // that answers it — asking it twice is the shape this
                      // whole release exists to remove.
                      mcpp::fetcher::Fetcher srFetcher(**cfg3);
                      mcpp::fetcher::InstallProgressHandler srProgress;
                      std::optional<std::filesystem::path> dir;
                      if (auto p = srFetcher.resolve_xpkg_path(
                              want_sysroot, /*autoInstall=*/true, &srProgress))
                          dir = p->root;
                      else
                          dir = mcpp::xlings::paths::xpkg_payload(xl, ref);
                      if (dir) {
                          if (auto spec = mcpp::freestanding::resolve(*want)) {
                              const auto inc =
                                  *dir / "include" / std::string(spec->libdir);
                              const auto lib =
                                  *dir / "lib"     / std::string(spec->libdir);
                              std::error_code ec2;
                              state.tc->targetSysrootRoot = *dir;
                              state.tc->targetSysrootPkg  = ref.name;
                              if (std::filesystem::is_directory(inc, ec2))
                                  state.tc->targetSysrootInclude = inc;
                              if (std::filesystem::is_directory(lib, ec2))
                                  state.tc->targetSysrootLib = lib;
                          }
                      }
                  }
              }
          }
      }
      return {};
}

static std::expected<void, std::string>
step2_bind_msvc_toolset(PrepareState& state) {
      // THE MSVC TOOLSET OF THE CLANG ROW, chosen once and recorded before the
      // runtime identity below reads its SDK version. See bind_msvc_sysroot.
      if (state.tc->compiler == mcpp::toolchain::CompilerId::Clang
          && mcpp::toolchain::is_msvc_target(*state.tc)) {
          auto bound = bind_msvc_sysroot(*state.tc, *state.m, [&] { return state.get_cfg(true); });
          if (!bound) return std::unexpected(bound.error());
      } else if (state.tc->compiler == mcpp::toolchain::CompilerId::MSVC) {
          if (auto ok = check_cl_row_sysroot(*state.tc, *state.m); !ok)
              return std::unexpected(ok.error());
      }
      return {};
}

static void
step2_windows_runtime_identity(PrepareState& state) {
      // The Windows runtime identity, flowing BACK into the contract.
      //
      // Everything else about the runtime is known before a toolchain is
      // resolved, and deliberately so (see the RuntimeBinding block above). The
      // Windows SDK is the exception: it is a property of the toolchain, and
      // until it reached the contract hash the version axis simply did not
      // exist one layer below the compiler — two SDKs produced one cache key.
      //
      // `ucrt@<v>` is a COMPATIBILITY FLOOR, not a payload binding like
      // `glibc@<v>`: ucrtbase.dll is an OS component and mcpp ships no
      // redistributable for it. See mcpp.runtime.binding.
      if (!state.tc->windowsSdkVersion.empty()) {
          mcpp::platform::runtime::bind_windows_ucrt(
              state.runtimeBindingSnapshot, state.tc->windowsSdkVersion);
          state.tc->runtimeContractHash = state.runtimeBindingSnapshot.contractHash;
      }
}

static std::expected<void, std::string>
step2_msvc_abi_without_msvc_repair(PrepareState& state) {
      // ── Targeting the MSVC ABI without a usable MSVC ─────────────────────
      //
      // One judgement, one place. This used to be two separate concerns and
      // only one of them was implemented: `msvc@system` with no Windows SDK
      // was caught here, while clang-targeting-MSVC on a machine with no
      // Visual Studio at all — the default on every bare Windows box — fell
      // straight through to clang's own "'vector' file not found", from which
      // no user could infer that a working alternative was one flag away.
      // Deriving the same judgement in two places is how the second case went
      // unnoticed, so they are now one condition with two outcomes.
      const bool targetsMsvcAbi =
          state.tc->compiler == mcpp::toolchain::CompilerId::MSVC
          || mcpp::toolchain::is_msvc_target(*state.tc);
      if (targetsMsvcAbi && !state.msvc_usable_either_origin()) {
          // Native cl.exe is ALWAYS a deliberate choice: mcpp never selects
          // msvc@system on its own — it cannot install one — so the only way it
          // reaches config.toml is a user typing `mcpp toolchain default msvc`.
          // Without this, that user (who evidently wants MSVC and is probably
          // just missing the SDK component) would be silently moved to MinGW
          // instead of being told which component to install.
          //
          // The residual imprecision is deliberate and bounded: a *global*
          // default of llvm@20.1.7 is indistinguishable from the one mcpp used
          // to write itself, so an explicitly-typed one gets repaired too. The
          // value is identical either way and the machine cannot build with it;
          // a user who wants that failure can pin it in mcpp.toml, which is
          // honoured exactly.
          const bool userChoseMsvcItself =
              state.tc->compiler == mcpp::toolchain::CompilerId::MSVC;
          // AND NOT A COMPILER THE GRAPH REQUIRED. The repair below rewrites
          // the machine's default to winlibs GCC, which is right when mcpp's
          // own default cannot work here. A family a package REQUIRED is not
          // mcpp's default to revise: switching to gcc would satisfy nothing —
          // `check_requirements` refuses the build three thousand lines later —
          // while having changed the user's configuration on the way there.
          // Refusing at the decision is what the rest of this release is about.
          const bool mayRepair =
              !tc_origin_is_user_explicit(state.tcOrigin)
              && tc_origin_may_persist(state.tcOrigin)
              && !userChoseMsvcItself
              && !mcpp::platform::env::offline_mode()
              && !mcpp::platform::env::no_auto_install()
              && mcpp::platform::is_windows;
          if (!mayRepair) {
              return std::unexpected(msvc_unavailable_guidance(*state.tc));
          }
          // mcpp chose this default itself and it cannot work on this machine.
          // Revise it — including for users who already have `llvm@20.1.7`
          // persisted by an older mcpp: the first-run branch never fires again
          // for them, so this gate (which runs on EVERY build) is what repairs
          // them without a single manual command.
          namespace pins = mcpp::toolchain::triple::pins;
          mcpp::ui::info("Toolchain",
              std::format("{} targets the MSVC ABI but no Visual Studio "
                          "(MSVC STL + Windows SDK) was found — switching to {} → {}",
                          state.tcSpec.value_or("the configured default"),
                          pins::kFirstRunWinGnu, pins::kFirstRunWinGnuTarget));

          state.overrides.target_triple = std::string(pins::kFirstRunWinGnuTarget);
          // The x86_64-windows-gnu row is defaultStatic; the target block that
          // normally applies that already ran, so mirror just this one field.
          if (state.m->buildConfig.linkage.empty()) state.m->buildConfig.linkage = "static";

          auto gnuSpec = mcpp::toolchain::parse_toolchain_spec(
              std::string(pins::kFirstRunWinGnu));
          if (!gnuSpec) return std::unexpected(gnuSpec.error());
          if (auto t = mcpp::toolchain::triple::parse(state.overrides.target_triple))
              gnuSpec->target = *t;
          auto gnuPkg = mcpp::toolchain::to_xim_package(*gnuSpec);

          auto cfgR = state.get_cfg(true);
          if (!cfgR) return std::unexpected(cfgR.error());
          mcpp::fetcher::Fetcher fetcherR(**cfgR);
          mcpp::fetcher::InstallProgressHandler progressR;
          auto payloadR = fetcherR.resolve_xpkg_path(gnuPkg.target(),
                              /*autoInstall=*/true, &progressR);
          if (!payloadR) {
              return std::unexpected(std::format(
                  "switching to the MinGW-w64 toolchain ({}) failed: {}\n"
                  "       install it manually with:\n"
                  "         mcpp toolchain install {} --target {}",
                  pins::kFirstRunWinGnu, payloadR.error().message,
                  pins::kSuggestGccMingw, pins::kFirstRunWinGnuTarget));
          }
          auto gnuFrontendR =
              mcpp::toolchain::payload_frontend(payloadR->root, gnuPkg);
          if (!gnuFrontendR) return std::unexpected(gnuFrontendR.error());
          state.explicit_compiler = *gnuFrontendR;
          if (!std::filesystem::exists(state.explicit_compiler)) {
              return std::unexpected(std::format(
                  "MinGW-w64 payload {} has no known C++ frontend in {}",
                  gnuPkg.target(),
                  mcpp::toolchain::payload_frontend_dir(payloadR->root, gnuPkg).string()));
          }
          state.provide_runtime_payload(gnuPkg);
          if (auto fixed = mcpp::toolchain::ensure_post_install_fixup(
                  **cfgR, payloadR->root, gnuPkg,
                  state.runtimeBindingSnapshot.runtimeId, state.runtimeLibDir); !fixed)
              return std::unexpected(std::format(
                  "MinGW toolchain post-install fixup: {}", fixed.error()));
          else state.report_fixup(*fixed, payloadR->root);

          // Persist both axes so the repair happens once, not on every build.
          if (mcpp::config::write_default_toolchain(**cfgR, pins::kFirstRunWinGnu))
              (*cfgR)->defaultToolchain = std::string(pins::kFirstRunWinGnu);
          if (mcpp::config::write_default_target(**cfgR, state.overrides.target_triple))
              (*cfgR)->defaultTarget = state.overrides.target_triple;

          state.tcSpec   = std::string(pins::kFirstRunWinGnu);
          state.tcOrigin = TcOrigin::FirstRun;
          auto redetected = mcpp::toolchain::detect(
              state.explicit_compiler, state.runtimePayload,
              state.runtimeBindingSnapshot.contractHash);
          if (!redetected) return std::unexpected(redetected.error().message);
          state.tc = std::move(*redetected);
      }
      return {};
}

static void
step2_musl_default_static_linkage(PrepareState& state) {
    // For musl-gcc the toolchain is fully self-contained
    // (`<root>/x86_64-linux-musl/{include,lib}` is its own sysroot).
    // musl-gcc's `-dumpmachine` reports `x86_64-linux-musl`.
    bool isMuslTc = mcpp::toolchain::is_musl_target(*state.tc);

    // A musl toolchain only really makes sense with static linkage —
    // dynamic-musl binaries depend on a system /lib/ld-musl-x86_64.so.1
    // that most distros don't ship. Default linkage to "static" when
    // the resolved toolchain is musl, unless the user has already opted
    // out via `--static` or [target.<triple>].linkage. (There is no
    // [build].linkage — the parser only reads it under a target section.)
    if (isMuslTc && state.m->buildConfig.linkage.empty()) {
        state.m->buildConfig.linkage = "static";
    }
}

std::expected<void, std::string> phase2_define_toolchain_resolver(PrepareState& state) {
    state.resolve_target_toolchain = [&]() -> std::expected<void, std::string> {
      ToolchainResolveCtx ctx;
      if (auto r = step2_parse_toolchain_spec(state, ctx); !r) return std::unexpected(r.error());

      if (ctx.installedPin) {
        step2_use_installed_pin(state, ctx);
      } else if (state.tcSpecIsMsvc) {
        if (auto r = step2_use_system_msvc(state); !r) return std::unexpected(r.error());
      } else if (ctx.parsedSpec && !ctx.parsedSpec->localRoot.empty()) {
        if (auto r = step2_use_local_toolchain(state, *ctx.parsedSpec); !r)
            return std::unexpected(r.error());
      } else if (ctx.parsedSpec) {
        if (auto r = step2_resolve_explicit_spec(state, ctx); !r) return std::unexpected(r.error());
      } else if (state.tcSpec.has_value() && *state.tcSpec == "system") {
        if (auto r = step2_system_toolchain_refusal(state); !r) return std::unexpected(r.error());
      } else if (mcpp::platform::env::offline_mode()
               || mcpp::platform::env::no_auto_install()) {
        if (auto r = step2_offline_refusal(state); !r) return std::unexpected(r.error());
      } else {
        if (auto r = step2_first_run_auto_install(state); !r) return std::unexpected(r.error());
      }

      step2_windows_gnu_first_run_persist(state);

      // AND NOW RESOLVE FOR THE TARGET, IF ONE WAS ASKED FOR.
      //
      // The first-run branch above answers "this machine has no toolchain, give
      // it one", and the answer is a HOST payload; `--target` was never read
      // there. On a machine that had never built anything,
      // `mcpp build --target x86_64-windows-gnu` therefore installed a native
      // gcc and compiled Windows sources with it — measured in CI 2026-08-25:
      //
      //     First run  no toolchain configured — installing gcc@16.1.0 …
      //      Resolved  gcc@16.1.0 → …/xim-x-gcc/16.1.0/bin/g++
      //                                        ↑ no target in the path
      //
      // against the same command where one already existed:
      //
      //      Resolved  gcc@16.1.0 → x86_64-windows-gnu → …/mingw-cross-gcc/…
      //
      // REUSES THE PATH THAT ALREADY KNOWS HOW rather than repeating it. The
      // default just chosen is the spec; mapping a spec plus a target onto a
      // payload (installing it if absent — `autoInstall` was always true there)
      // is what the top of this function does. Depth is one: the second pass
      // takes the `tcSpec.has_value()` branch the first run just made true.
      // ONE-SHOT, AND THE FLAG IS SET BEFORE THE CALL, NOT AFTER.
      //
      // This line sits OUTSIDE the first-run branch — it has to, because the
      // Windows block just above sets the target itself — so it is evaluated on
      // every pass. The first version relied on `firstRunNeedsTargetPass` being
      // false on the second pass; it is a captured variable that nothing
      // resets, so every pass recursed again. Measured in a consumer's CI as
      // the same `Resolved` line four times and then
      //
      //     ##[error]Process completed with exit code 139
      //
      // — SIGSEGV, a stack that ran out. A recursion whose termination depends
      // on state the recursive call does not change is not a depth-one
      // recursion, however its comment reads.
      if (!state.targetPassDone
          && (state.firstRunNeedsTargetPass
              || (state.windowsGnuFirstRun && state.tcSpec.has_value()))) {
        state.targetPassDone = true;
        return state.resolve_target_toolchain();
      }

      if (auto r = step2_detect_toolchain(state); !r) return std::unexpected(r.error());
      if (auto r = step2_apply_local_toolchain(state); !r) return std::unexpected(r.error());
      if (auto r = step2_retarget_for_retargetable_driver(state); !r) return std::unexpected(r.error());
      if (auto r = step2_bind_msvc_toolset(state); !r) return std::unexpected(r.error());
      step2_windows_runtime_identity(state);
      if (auto r = step2_msvc_abi_without_msvc_repair(state); !r) return std::unexpected(r.error());
      step2_musl_default_static_linkage(state);

    return {};
    };

    return {};
}

} // namespace mcpp::build
