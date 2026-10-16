// xlings.cpp -- P3: the xlings payloads the root declares, provisioned
// before the dependency graph is built.

module mcpp.build.prepare;
import :state;

import mcpp.build.prepare_inputs;

import std;
import mcpp.diag;
import mcpp.build.refusal;
import mcpp.provider;
import mcpp.provider.select;
import mcpp.provider.address_set;
import mcpp.build.version_floor;
import mcpp.log;
import mcpp.manifest;
import mcpp.source_kind;
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
import mcpp.build.build_program;
import mcpp.build.backend;      // BuildOptions for the tool sub-build
import mcpp.build.ninja;        // make_ninja_backend — driving that sub-build
import mcpp.config;
import mcpp.toolchain.post_install;
import mcpp.platform;
import mcpp.fetcher;
import mcpp.fetcher.progress;
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
import mcpp.log;

namespace mcpp::build {

// STEP FUNCTION (mcpp#722 / T6 follow-on): materializing root
// generated_files, the host-toolchain closures phase3 assigns onto
// state, and the index-refresh section, extracted verbatim.
static std::expected<void, std::string>
step3_define_host_tc_closures_and_refresh_index(PrepareState& state) {

    // Sysroot comes from the toolchain payload itself (GCC -print-sysroot,
    // Clang clang++.cfg). mcpp does not override it — the payload is
    // self-describing. See docs: 2026-05-21-linux-sysroot-missing-kernel-headers.md

    // ── L3: project-local `build.mcpp` imperative build program ─────────────
    // The ROOT program is compiled with the HOST toolchain and run AFTER
    // dependency resolution + feature activation (so it receives
    // MCPP_DEP_<NAME>_DIR like a dependency's does — design §3.1 item 4) and
    // BEFORE the modgraph scan (so its `generated=`/`source=` sources are
    // picked up) — see the call site further below, after the dep build.mcpp
    // loop. Its stdout directives augment buildConfig; a declared-input cache
    // re-runs it only when its source/inputs/env/contract change. It cannot
    // gate the top-level dependency graph (leaf-only rule). Under a cross
    // --target it runs with a host-resolved toolchain and sees MCPP_TARGET =
    // the cross triple (G3).
    // See .agents/docs/2026-06-30-l3-build-mcpp-implementation-design.md,
    // 2026-07-17-asm-sources-and-general-build-capabilities-design.md §2.4 and
    // 2026-07-19-large-source-pkg-platform-fixes-and-buildmcpp-generation-design.md.
    // Root [generated_files]: materialize before build.mcpp and the modgraph
    // scan so synthesized sources are globbed like any on-disk file — and
    // BEFORE dependency resolution, since generated_files may produce
    // build.mcpp itself. (The per-dependency call sits in the dep resolution
    // loop below; the root manifest needs its own.)
    if (!state.m->buildConfig.generatedFiles.empty()) {
        std::vector<std::filesystem::path> staleGenerated;
        if (auto r = materialize_generated_files(
                *state.root, *state.m, state.overrides.plan_only ? &staleGenerated : nullptr); !r) {
            return std::unexpected(r.error());
        }
        for (auto const& path : staleGenerated)
            state.planNotes.push_back({"MCPP_GENERATED_FILE_NOT_MATERIALIZED",
                std::format("'{}' is declared in [build] generated_files and its "
                            "content on disk differs from the declaration; this "
                            "command does not write the project, and `mcpp build` "
                            "writes it", path.string())});
    }

    // Canonical rendering of the resolved target (for the env contract).
    if (!state.overrides.target_triple.empty()) {
        auto tt = mcpp::toolchain::triple::parse(state.overrides.target_triple);
        state.resolvedTargetCanonical = tt ? tt->str() : state.overrides.target_triple;
    }

    // Host toolchain for build.mcpp (G3): under a cross --target the resolved
    // `tc` is the cross toolchain, whose products cannot run here — resolve a
    // host-target toolchain from the same spec vocabulary (the spec WITHOUT
    // the --target axis), lazily and only when a build.mcpp actually exists
    // (root or dependency).
    // The spec `host_tc_for_build_program` resolves, as text: the build's own
    // spec on a native build; on a cross build the spec the target row's pin
    // replaced, or the platform's native first-run default when it replaced
    // nothing (#622, see the cross branch below). Also what a host-tool
    // sub-build is handed when its package names no toolchain (#710), so the
    // tool is built by the compiler the store key records.
    state.host_spec_for_build_program = [&]() -> std::string {
        // The bootstrap toolchain, when one was stated (mcpp#755).
        if (!state.bootstrapSpec.empty()) return state.bootstrapSpec;
        if (!state.tcSpec) return {};
        if (state.overrides.target_triple.empty()) return *state.tcSpec;
        return (state.tcOrigin == TcOrigin::TargetPin && state.hostSpecBeforeRowPin.has_value()
                && !state.hostSpecBeforeRowPin->empty() && *state.hostSpecBeforeRowPin != "system")
            ? *state.hostSpecBeforeRowPin
            : (state.tcOrigin == TcOrigin::TargetPin ? state.native_first_run_spec() : *state.tcSpec);
    };
    state.host_tc_for_build_program = [&]() -> std::expected<
            std::pair<std::filesystem::path, mcpp::toolchain::Toolchain>, std::string> {
        // A HOST TOOLCHAIN'S C LIBRARY IS THE PAYLOAD'S, WHATEVER THE
        // PROJECT'S TARGET SIDE IS.
        //
        // `build.mcpp` is compiled AND RUN on the machine doing the build. Its
        // C library therefore comes from the compiler payload — even for a
        // project whose TARGET takes its C library from the dependency graph.
        // The two are different machines and this function's whole job is to
        // keep them apart.
        //
        // AND THE NATIVE BRANCH BELOW RETURNS THE MAIN `tc`, WHICH CARRIES
        // THE OTHER ANSWER. `build_program.cppm`'s own header states the
        // invariant — "`tc` is always a HOST-targeting toolchain" — and for
        // every field but this one the native branch satisfied it, because on a
        // native build the compiler IS the host compiler. `cAbiPrebuilt` is the
        // first field where "same compiler" and "same target side" come apart.
        //
        // AN INVARIANT, NOT A BUG FIX FOR ANY MEASURED FAILURE. It was
        // written while chasing a `features.h: No such file` on openkal-musl's
        // CI and it is NOT that failure's cause: measured on `origin/main` and
        // on this branch, the gcc std module carries zero `-isystem`/
        // `-idirafter` rows either way — that toolchain reaches its C library
        // through the specs the post-install fixup rewrites, and the real
        // defect was in resolving WHICH glibc payload those specs name.
        //
        // Kept because the invariant is worth being true: a helper compiled and
        // run on the build machine must not inherit the target's C-library
        // origin, and the next field that comes apart would find no rule here.
        //
        // ⇒ Stated once, so every consumer (the std module build,
        // `host_base_flags`) gets it without asking.
        auto as_host = [](mcpp::toolchain::Toolchain t) {
            t.cAbiPrebuilt = true;
            return t;
        };
        // `explicit_compiler` IS EMPTY FOR ONE RESOLUTION PATH, AND THIS IS
        // THE ONLY CALLER THAT NOTICED BY CRASHING (#527).
        //
        // Every branch that resolves a toolchain from the index assigns
        // `explicit_compiler`; the `[toolchain] system` branch does not, because
        // it has nothing to assign yet — `detect` finds the PATH compiler a few
        // hundred lines below and stores the resolved ABSOLUTE path in
        // `tc->binaryPath`. The main build reads the compiler from `tc` and is
        // fine; this closure returned the local variable and handed "" to
        // `posix_spawnp`, which is `exit 127: posix_spawnp('') failed`.
        //
        // AND THE FIX IS NOT "SUPPORT THE HOST". `tc->binaryPath` is the
        // compiler this build is ALREADY using for every other translation
        // unit; build.mcpp is compiled with the project's toolchain by
        // definition (see this lambda's header). Reading it from the place it
        // was resolved makes the two paths agree — it grants no capability the
        // project did not already have, and the host-dependence warning at the
        // `system` branch is what states the cost.
        //
        // The CROSS branch below is a different question and deliberately
        // unchanged: there `explicit_compiler` is empty because NO host
        // toolchain was resolved at all, and its classified refusal is correct.
        // A native build compiles its build programs with its own toolchain,
        // unless a bootstrap toolchain was stated (mcpp#755): then the
        // resolution below serves it, exactly as it serves a cross build.
        if (state.overrides.target_triple.empty() && state.bootstrapSpec.empty())
            return std::pair{
                state.explicit_compiler.empty() ? state.tc->binaryPath : state.explicit_compiler,
                as_host(*state.tc)};
        if (state.hostTcCache)
            return std::pair{state.hostTcCache->first, as_host(state.hostTcCache->second)};
        if (!state.tcSpec || *state.tcSpec == "system" || state.tcSpecIsMsvc) {
            // A READABLE REFUSAL THAT HAD NO CODE, so the target matrix
            // recorded four identical `other` cells for it. The sentence was
            // right; the classification was missing. Measured on windows-2022
            // with `msvc@system` declared and any cross target.
            refusal::record(refusal::Code::HostToolToolchain);
            return std::unexpected(std::string(
                "build.mcpp under a cross --target needs a resolvable host "
                "toolchain — set one via [toolchain] or `mcpp toolchain default`"));
        }
        // THE ROW'S CONVENTION IS NOT THE HOST'S COMPILER. When the target
        // row's pin replaced a spec the user or the machine had chosen, the
        // build program resolves the replaced one: it is what a native build
        // on this machine would use, and it is what the user wrote.
        //
        // A PIN THAT REPLACED NOTHING IS NOT "RESOLVED AS BEFORE" ANY MORE
        // (#622). "Before" meant falling through to `*tcSpec`, which at this
        // point (`tcOrigin == TargetPin`) IS the row's own pin — a TARGET
        // answer. For a row whose payload can only ever emit its target
        // (`emscripten@…` → em++, WebAssembly under every invocation) that
        // resolved a cross compiler as the HOST toolchain for build.mcpp,
        // which is compiled AND RUN on this machine: the compile itself
        // "succeeds" (clang accepts the syntax) and the failure surfaces one
        // step later, inside the payload's own driver, trying to produce a
        // program this machine can execute (measured: emcc.py's
        // `phase_compile_inputs` hits `assert os.path.exists(output_file)`
        // and raises, on the very first `mcpp build --target
        // wasm32-emscripten` in a fresh $HOME, before any [toolchain] default
        // has ever been resolved or persisted). A row whose payload happens
        // to double as a host compiler (an NDK clang) hid the same defect by
        // accident.
        //
        // "Nothing to fall back on" must mean "resolve the platform's native
        // default now", exactly as a plain `mcpp build` would on a virgin
        // machine — not "reuse the target's answer". `native_first_run_spec()`
        // is that exact selection (declared once, above, and used by the
        // first-run installer itself), reused rather than re-derived so the
        // two cannot silently drift apart.
        const std::string hostSpecText = state.host_spec_for_build_program();
        auto spec = mcpp::toolchain::parse_toolchain_spec(hostSpecText);
        if (!spec || spec->version.empty()) {
            return std::unexpected(std::format(
                "toolchain spec '{}' is invalid for the build.mcpp host resolve", hostSpecText));
        }
        // Deliberately NO target injection: the spec resolves for the host.
        auto pkg = mcpp::toolchain::to_xim_package(*spec);
        auto cfgH = state.get_cfg(true);
        if (!cfgH) return std::unexpected(cfgH.error());
        mcpp::fetcher::Fetcher fetcher(**cfgH);
        mcpp::fetcher::InstallProgressHandler progress;
        auto payload = fetcher.resolve_xpkg_path(pkg.target(), /*autoInstall=*/true, &progress);
        if (!payload) {
            return std::unexpected(std::format(
                "host toolchain for build.mcpp ('{}'): {}", hostSpecText,
                payload.error().message));
        }
        auto frontendR = mcpp::toolchain::payload_frontend(payload->root, pkg);
        if (!frontendR) return std::unexpected(frontendR.error());
        auto frontend = *frontendR;
        if (!std::filesystem::exists(frontend)) {
            return std::unexpected(std::format(
                "host toolchain payload '{}' has no known C++ frontend in {}",
                pkg.target(),
                    mcpp::toolchain::payload_frontend_dir(payload->root, pkg).string()));
        }
        state.provide_runtime_payload(pkg);
        // SAME THREE ARGUMENTS THE NATIVE CALL USES (line ~3550), not the
        // one-argument form. `detect()` probes `payloadPaths` — the
        // fine-grained glibc/linux-headers xpkg directories `resolve_link_model`
        // attaches as explicit `-isystem` rows — from the SECOND argument, and
        // does so only when it is given; passing only `frontend` leaves
        // `tc.payloadPaths` unset, so `host_base_flags`/`host_compile_tokens`
        // fell back to `tc.sysroot` alone (from the payload's own
        // `*sysroot_spec: --sysroot=%R`, `%R` being wherever the fixup pointed
        // it — nothing, on a sandbox with no leaked subos sysroot to fill it
        // in by accident).
        //
        // Measured in the xlings sandbox against the released 2026.9.12.3, on
        // a fresh registry (a real, non-symlinked gcc@16.1.0 payload, no
        // ambient /usr/include, no subos state to leak): "Resolved host
        // toolchain for build.mcpp: gcc 16.1.0 (x86_64-linux-gnu)" — the right
        // FAMILY, since #622's first fix already keeps the pre-row spec — and
        // then the `mcpp` module compile failed with `features.h: No such
        // file or directory`, because that gcc's specs alone name no C
        // library. `echo | g++ -x c++ -E -v -` there lists only the payload's
        // own `c++/16.1.0`, `include`, `include-fixed` — no glibc directory.
        // On a development machine the same probe happens to pass, but for a
        // reason that has nothing to do with this code path: the shared-store
        // gcc's search list there ends with a SUBOS's `usr/include`, leaked
        // into `%R` by machine state the payload never declared (the same
        // shape as "host /usr/include silently completes a payload
        // toolchain") — which is exactly the kind of thing a fresh sandbox
        // does not have lying around to hide the gap.
        //
        // `runtimePayload` and `runtimeBindingSnapshot` (declared once, near
        // the top of this function) are the HOST's C-library identity — never
        // re-derived from `--target`, see their own declarations — so passing
        // them here is not a parallel derivation; it is the one this function
        // already had in scope and the native call already trusts.
        auto htc = mcpp::toolchain::detect(
            frontend, state.runtimePayload, state.runtimeBindingSnapshot.contractHash);
        if (!htc) return std::unexpected(htc.error().message);
        if (state.overrides.target_triple.empty() && !state.bootstrapSpec.empty()) {
            // The bootstrap toolchain of a native build (mcpp#755): said with
            // its own verb, because the line after it names another toolchain.
            mcpp::ui::info("Bootstrap", std::format(
                "{} → {}", hostSpecText,
                mcpp::ui::shorten_path(frontend, mcpp::fetcher::make_path_ctx(*cfgH, *state.root))));
            SourceDecision d;
            d.subject = "toolchain.bootstrap";
            d.value   = hostSpecText;
            d.cls     = SourceClass::Pinned;
            d.originKind = state.overrides.bootstrap_spec.empty() ? "manifest" : "build-program";
            d.originKey  = state.overrides.bootstrap_spec.empty() ? "[toolchain] bootstrap"
                                                                  : "the toolchain that ran the toolchain phase";
            d.considered.push_back(frontend.generic_string());
            record_source(state, std::move(d));
        } else {
            mcpp::ui::info("Resolved", std::format(
                "host toolchain for build.mcpp: {}", htc->label()));
        }
        state.hostTcCache = std::pair{frontend, *htc};
        return std::pair{state.hostTcCache->first, as_host(state.hostTcCache->second)};
    };

    // Resolve dependencies: walk the **transitive** graph from the main
    // manifest, BFS-style. Each unique `(namespace, shortName)` is fetched
    // once, its `[build].include_dirs` are propagated to the main
    // manifest, and its own `[dependencies]` are queued for processing
    // (its `[dev-dependencies]` are NOT — those are private to the dep's
    // own test runs).
    //
    // Conflict policy: C++ modules require globally-unique module names
    // and ODR-respecting symbols, so the same `(ns, name)` resolved to
    // two different exact versions is an error — mcpp prints both
    // requesting parents and asks the user to align them.

    // Refresh the builtin package index only when a dependency cannot be
    // resolved from the local copy (#315).
    //
    // This used to fire whenever the refresh marker was older than an hour,
    // whether or not anything was actually missing — so every build with a
    // registry dependency paid a multi-repo network sync once an hour, which is
    // minutes on a slow or blocked network for data it already had. The policy
    // now lives in mcpp.pm.index_refresh and is shared with `mcpp add` and the
    // xim install gate, which had each derived their own (and disagreed).
    //
    // Nothing here decides anything itself — in particular the "a miss proves
    // nothing for this namespace" rule must not be re-derived; see that module.
    //
    // The root's declarations, and in a workspace plan each selected member's
    // (`PrepareState::declaredByRoot`): the virtual root declares only its
    // members, by `path`, so asking about its edges alone never refreshed for
    // a member's registry dependency, which each member, planned as its own
    // root before 2026.9.29.1, did. A member's dependency is routed by the
    // member's own `[indices]` and directory, as the walk routes it.
    const bool anyDeclared = !state.m->dependencies.empty()
        || std::ranges::any_of(state.selectedMemberManifests,
               [](auto const& mm) { return !mm.second.dependencies.empty(); });
    if (anyDeclared) {
        if (auto cfg2 = state.get_cfg(true)) {
            auto xlEnv  = mcpp::config::make_xlings_env(**cfg2);
            auto policy = mcpp::pm::policy_for(**cfg2);
            // Returns true once a refresh has been applied: one sync covers
            // every dependency.
            auto consider = [&](const mcpp::pm::IndexRoute& route,
                                const mcpp::manifest::Manifest& mf) {
                for (auto& [depName, spec] : mf.dependencies) {
                    auto decision = mcpp::pm::decide_for_dependency(
                        route, depName, spec, xlEnv, *state.targetPlatform, policy);
                    if (!decision.shouldRefresh) {
                        mcpp::log::verbose("index", std::format(
                            "{}: {}", decision.subject,
                            mcpp::pm::reason_text(decision.reason)));
                        continue;
                    }
                    // A failed refresh is not a failed build: the dependency
                    // walk below may still resolve everything from what is on
                    // disk, and if it cannot, it reports the actual missing
                    // package with the index's age attached. Failing here
                    // instead would turn a transient network blip into a hard
                    // stop for a build that needed no network at all.
                    if (auto r = mcpp::pm::apply(decision, xlEnv); !r)
                        mcpp::ui::warning(r.error());
                    return true;
                }
                return false;
            };
            // Same routing the dependency walk below uses (the `index_route`
            // lambda is declared further down; this is the identical value).
            bool synced = consider(
                mcpp::pm::IndexRoute{ &state.m->indices, *state.root, *cfg2 }, *state.m);
            for (auto const& [dir, member] : state.selectedMemberManifests) {
                if (synced) break;
                synced = consider(mcpp::pm::IndexRoute{ &member.indices, dir, *cfg2 }, member);
            }
        }
    }
    return {};
}

std::expected<void, std::string> phase3_xlings_before_graph(PrepareState& state) {
    if (auto r = step3_define_host_tc_closures_and_refresh_index(state); !r)
        return std::unexpected(r.error());


    // Set up project-level .mcpp/ directory for custom indices and/or the
    // [xlings] build environment (L-1). This creates .mcpp/.xlings.json with
    // custom non-builtin index entries (so xlings can clone them) plus the
    // [xlings] deps/workspace/subos/envs materialized verbatim.
    // A pointer, not a reference: state.wsManifest and state.m outlive every
    // phase, so this stays valid wherever it is read from, but a PrepareState
    // member cannot itself be a reference (see the PrepareState comment).
    state.runtimeOwnerManifest = state.wsManifest ? &*state.wsManifest : &*state.m;
    // The TARGET's C library, if this target has one. Resolved here and not by
    // any package, for the same reason the compiler pin is: it is a property
    // of the target.
    //
    // It rides the SAME channel as `[xlings] deps` rather than getting an
    // install path of its own — one materialization, one place that can be
    // wrong. What it must NOT do is depend on the project having an `[xlings]`
    // section: a bare-metal project written to the template has none, and the
    // whole point is that it never mentions a libc.
    // FROM THE REQUESTED TRIPLE, NOT FROM THE TOOLCHAIN — AND THE TWO WERE
    // THE SAME VALUE ALL ALONG.
    //
    // This read of `tc->targetTriple` was the ONLY thing tying the compiler's
    // resolution to a point before dependency resolution, and it never wanted
    // the compiler: `tc->targetTriple` is corrected to the requested triple a
    // few lines after the toolchain is detected, so the value here is the one
    // `--target` named. Taking it from the request instead lets the toolchain
    // be resolved where the information it needs actually exists.
    std::string targetSysroot;
    {
        auto tt = state.overrides.target_triple.empty()
            ? std::optional{mcpp::toolchain::triple::host_triple()}
            : mcpp::toolchain::triple::parse(state.overrides.target_triple);
        // Not on an MSVC-ABI row: there the key names an MSVC toolset, which
        // `bind_msvc_sysroot` locates or installs itself -- an installed
        // toolset of the pinned version must win over a download, and
        // `msvc@system` is not a package at all.
        if (tt && !tt->is_msvc_env())
            targetSysroot = mcpp::toolchain::triple::effective_sysroot(
                *tt, sysroot_override(*state.m, *tt));
    }
    const bool materializeRootRuntime =
        !state.overrides.inherited_runtime_binding
        && (!state.runtimeOwnerManifest->xlings.empty() || !targetSysroot.empty());
    if (!state.m->indices.empty() || materializeRootRuntime) {
        auto cfg2 = state.get_cfg(true);
        if (cfg2) {
            mcpp::provider::ProjectEnv penv;
            if (materializeRootRuntime) {
                penv.deps  = state.runtimeOwnerManifest->xlings.deps;
                // Appended, never substituted: a project may legitimately
                // declare other xim packages, and a target sysroot is one more
                // entry rather than a replacement for the list. Deduplicated
                // because a manifest written before this axis existed still
                // names it, and declaring it twice is not an error the author
                // should have to hear about.
                if (!targetSysroot.empty()
                    && std::ranges::find(penv.deps, targetSysroot) == penv.deps.end())
                    penv.deps.push_back(targetSysroot);
                penv.subos = state.runtimeOwnerManifest->xlings.subos;
                for (auto const& [k, v] : state.runtimeOwnerManifest->xlings.workspace)
                    penv.workspace.emplace_back(k, v);
                // `[feature-xlings.<f>]` becomes part of the project's
                // environment only while `<f>` is active. It is written into
                // the same two fields, because from xlings' side there is no
                // such thing as a feature: the file states what this project
                // uses, and the feature decided that.
                for (auto const& f :
                         feature_closure(*state.runtimeOwnerManifest,
                                         parse_feature_request(state.overrides.features)))
                    if (auto it = state.runtimeOwnerManifest->xlings.featureDeps.find(f);
                        it != state.runtimeOwnerManifest->xlings.featureDeps.end())
                        for (auto const& address : it->second) {
                            if (std::ranges::find(penv.deps, address) == penv.deps.end())
                                penv.deps.push_back(address);
                            const auto entry =
                                mcpp::manifest::parse_address(address);
                            if (std::ranges::none_of(penv.workspace,
                                    [&](auto const& kv) { return kv.first == entry.target; }))
                                penv.workspace.emplace_back(entry.target, entry.pin());
                        }
            }
            // ONE PACKAGE, ONE VERSION, INSIDE ONE MANIFEST TOO. The
            // conditional merge already unified the two tool AXES by package;
            // what it cannot see is `[xlings.workspace]` and
            // `[feature-xlings.<f>]` naming one package at two versions, which
            // reaches here as two addresses and used to install both.
            std::vector<mcpp::provider::addrset::Claim> rootClaims;
            std::vector<char> rootOnRequest;
            for (auto const& spec : applicable_xlings_addresses(
                     *state.runtimeOwnerManifest,
                     feature_closure(*state.runtimeOwnerManifest,
                                     parse_feature_request(state.overrides.features)),
                     state.toolPurpose, /*isRoot=*/true)) {
                rootClaims.push_back({spec, "this project", 0});
                rootOnRequest.push_back(state.runtimeOwnerManifest->xlings.on_request(spec));
            }
            auto rootUnified = mcpp::provider::addrset::unify(rootClaims);
            if (!rootUnified) {
                refusal::record(refusal::Code::ToolVersionConflict);
                return std::unexpected(rootUnified.error());
            }
            for (auto const& note : rootUnified->overrides)
                mcpp::diag::warning("xlings/version-override", note);
            // What the registry installs for the project itself: an
            // overridden package and one declared on request are left out and
            // recorded (mcpp#755). Decided BEFORE the environment file is
            // written, because an overridden package is not part of it.
            auto rootInstall = payloads_to_provision(state, *rootUnified, rootClaims,
                                                     rootOnRequest);
            if (!rootInstall) return std::unexpected(rootInstall.error());
            std::vector<std::string> declaredDeps = std::move(*rootInstall);
            if (materializeRootRuntime && !state.xlingsOverridden.empty()) {
                namespace addrset = mcpp::provider::addrset;
                std::erase_if(penv.deps, [&](const std::string& a) {
                    return state.xlingsOverridden.contains(addrset::package_key(a));
                });
                std::erase_if(penv.workspace, [&](const auto& kv) {
                    return state.xlingsOverridden.contains(addrset::package_key(kv.first));
                });
            }
            // Two halves, two roots. The custom-indices half belongs to
            // `state.workRoot`, where this invocation writes. The runtime-
            // environment half (`penv`: deps/subos/workspace) belongs to the
            // runtime's owner, `runtimeSelection.ownerRoot`: the workspace
            // root when a member builds (e2e 205), the project root otherwise.
            // Under `plan_only` (`emit build-database`) nothing is written
            // into the project (SPEC-005 R2.1, mcpp#724 side finding B, e2e
            // 817), so the owner's half goes to the planning directory too.
            // The build programs' graph shares the indices of the build that
            // asked, whose project already holds them (`project_index_root`).
            const auto& runtimeRoot = state.overrides.plan_only
                ? state.workRoot : state.runtimeSelection.ownerRoot;
            const auto indices = state.overrides.project_index_root.empty()
                ? state.m->indices : decltype(state.m->indices){};
            if (runtimeRoot == state.workRoot) {
                mcpp::config::ensure_project_index_dir(
                    **cfg2, state.workRoot, indices, penv);
            } else {
                if (!indices.empty())
                    mcpp::config::ensure_project_index_dir(
                        **cfg2, state.workRoot, indices, {});
                if (materializeRootRuntime)
                    mcpp::config::ensure_project_index_dir(
                        **cfg2, runtimeRoot, {}, penv);
            }

            // `[xlings] deps` are DECLARED above and, until now, nothing
            // installed them (mcpp-index #281 §9).
            //
            // `ensure_project_index_dir` writes them into `.mcpp/.xlings.json`
            // verbatim and stops there, so a manifest saying
            // `deps = ["xim:mesa"]` produced a file naming mesa, no project
            // SubOS, and `fatal error: gbm.h: No such file or directory`. The
            // declaration looked accepted and did nothing — which is the worst
            // shape a config key can have.
            //
            // This is the same "declare it and mcpp provisions it on first use"
            // contract `[toolchain]` has had all along; that path is a few
            // hundred lines up ("First run — no toolchain configured …
            // installing … as default"). A build environment should not have
            // two grades of declaration.
            //
            // ORDER IS LOAD-BEARING: this must run BEFORE the runtime binding
            // resolves, because a named `[xlings] subos` that does not exist
            // yet is a hard error ("selected SubOS '…' does not exist;
            // create/bootstrap that environment"), and provisioning is what
            // creates it. Placed here, next to the index sync below, both
            // first-use provisioning steps sit in one place.
            //
            // `install_packages` rather than `fetcher.install`: the install
            // DESTINATION is chosen by package scope (project vs global), and
            // the project scope is what materializes the project SubOS. It also
            // carries the live progress UI and captured child errors, matching
            // the toolchain and custom-index paths.
            // Only what the MANIFEST declared, deliberately not `penv.deps`.
            //
            // A cross-compilation target sysroot is APPENDED to that list a few
            // lines up, and provisioning it here would change behaviour for
            // projects that never asked for it: a name that does not resolve
            // would turn a build that used to proceed into a hard failure. The
            // contract being added is "what you declared gets installed", and
            // the sysroot entry is mcpp's own inference rather than the
            // author's declaration.
            // Only the tiers this verb needs, and only what the ROOT
            // declared. The graph's own declarations are provisioned after
            // resolution, which is the first moment they are known — see the
            // second pass near `xlingsDepBinDirs`.
            if (materializeRootRuntime && !declaredDeps.empty()) {
                if (auto pv = provision_xlings_addresses(
                        **cfg2, declaredDeps, state.runtimeSelection.ownerRoot,
                        "[xlings.workspace] entries");
                    !pv) return std::unexpected(pv.error());
            }

            // On first build, the project index data root may be empty because
            // ensure_project_index_dir only writes .xlings.json but does not
            // trigger clone/link creation. Local path indices are read directly;
            // remote custom indices are synced quietly before dependency resolution.
            bool hasCustomIndices = false;
            for (auto& [idxName, spec] : state.m->indices) {
                if (!spec.is_builtin()) {
                    hasCustomIndices = true;
                    break;
                }
            }
            if (hasCustomIndices) {
                bool needsClone = !mcpp::config::project_index_data_initialized(state.projectIndexRoot());
                if (needsClone) {
                    bool needsRemoteUpdate = false;
                    for (auto& [idxName, spec] : state.m->indices) {
                        if (spec.is_builtin() || spec.is_local()) continue;
                        needsRemoteUpdate = true;
                        break;
                    }
                    // A first sync is a refresh of an index that has no local
                    // copy yet, and `[index] auto_refresh = false` means that no
                    // refresh happens implicitly (docs/05). The opt-outs are the
                    // policy's (#648 A5); offline, the sync is a no-op as before
                    // and resolution reports what is missing.
                    //
                    // WHY THIS ONE DOES NOT GO THROUGH `decide_for_miss`/`apply`.
                    // Those answer "may this run refresh the index that would
                    // resolve a dependency", and their debounce and one-sync-per-
                    // process guard are about that one index. This sync creates a
                    // local copy that does not exist yet, of a DIFFERENT set of
                    // repositories, and nothing else will create it: taking the
                    // guard would let a refresh of the builtin index earlier in
                    // the same run suppress a clone the build cannot proceed
                    // without. Only the opt-outs are shared, and they are read
                    // from the same `policy_for`.
                    const auto refreshPolicy = mcpp::pm::policy_for(**cfg2);
                    if (needsRemoteUpdate && !refreshPolicy.offline && !refreshPolicy.autoRefresh) {
                        return std::unexpected(std::string(
                            "the project's custom index repositories have never been synced, "
                            "and [index] auto_refresh = false forbids syncing them implicitly\n"
                            "       run `mcpp index update` once, then build again"));
                    }
                    if (needsRemoteUpdate && !refreshPolicy.offline) {
                        mcpp::ui::status("Fetching", "custom index repos (first use)");
                        auto projEnv = mcpp::config::make_project_xlings_env(**cfg2, state.projectIndexRoot());
                        int rc = mcpp::provider::open(projEnv)->update_index(/*quiet=*/true);
                        if (rc != 0) {
                            return std::unexpected(
                                "project custom index update failed; run `mcpp index update` for details");
                        }
                    }
                }
            }
        }
    }

    return {};
}

} // namespace mcpp::build
