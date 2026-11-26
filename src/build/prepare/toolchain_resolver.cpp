// toolchain_resolver.cpp -- P2: the definition of the toolchain resolver that
// P5 calls once the dependency graph exists (`state.resolve_target_toolchain`):
// the request's parse and version, the explicit and first-run paths, the
// refusals, detection and the MSVC and MinGW bindings, and the companions the
// toolchain states. Cut from toolchain.cpp at the phase boundary (P1 | P2).

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
      if constexpr (mcpplibs::os::is_windows) {
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
        if (!mcpplibs::os::is_windows) {
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

// A TOOLCHAIN NOBODY STATED FOR THIS BUILD MOVED (SPEC-006 §2.8): the
// machine's default or mcpp's answers another version than the one this
// project last built with. Said once per change, with the statement that keeps
// the old one; the first build of a project, and the first after `mcpp clean`,
// has nothing to compare with and says nothing.
static void notice_default_toolchain_change(PrepareState& state, const mcpp::toolchain::ToolchainSpec& spec) {
    const std::string fact = std::format("toolchain:{}",
        state.overrides.target_triple.empty() ? std::string("native") : state.overrides.target_triple);
    const std::string now = spec.spec_str();
    const auto before = mcpp::diag::project_fact(fact);
    mcpp::diag::remember_project_fact(fact, now);
    if (!before || *before == now) return;
    const std::string statement = state.overrides.target_triple.empty()
        ? std::format("[toolchain] default = \"{}\"", *before)
        : std::format("[target.{}] toolchain = \"{}\"", state.overrides.target_triple, *before);
    mcpp::diag::notice(mcpp::diag::Severity::Note, "toolchain/default-changed",
        std::format("{} -> {}", *before, now),
        std::format("{} is now {}; this project last built with {}", state.tcSpecSource(), now, *before),
        std::format("to keep a version, state it in mcpp.toml: {}", statement));
}

// THE ONE RESOLUTION OF THE REQUEST, AND THE LOCK OF ONE THE PROJECT STATES
// (SPEC-006 §2.6, §2.7; mcpp#988). The version is what `mcpp toolchain install` and
// `mcpp toolchain default` answer for the same request
// (resolve_toolchain_spec). A partial version the project's mcpp.toml states
// is recorded in mcpp.lock and read back from it, so `gcc@16` stays on one
// release until `mcpp update`; a full version is its own record and writes
// none, so a project that states one gains no lock. One that `--toolchain` or a
// consumer chose for this build is resolved and neither recorded nor compared;
// the machine's and mcpp's defaults are followed, and their change is said
// once. Under `--locked` the lock is kept as it is: a lock written before the
// record has none, and the build resolves and says so rather than refusing,
// so an upgrade does not fail a CI that asserts its lock.
static void step2_resolve_request_version(PrepareState& state, const mcpp::config::GlobalConfig& cfg,
                                          mcpp::toolchain::ToolchainSpec& spec) {
    const auto pkg = mcpp::toolchain::to_xim_package(spec);
    if (pkg.ximName.empty()) return;
    const std::string request = std::format("xim:{}@{}", pkg.ximName, pkg.ximVersion);
    const bool chosenForThisBuild = state.overrides.tool_depth != 0 || state.tcFromCommandLine
                                    || state.tcFromConsumer;
    const bool stated = !chosenForThisBuild
        && (state.tcOrigin == TcOrigin::ManifestToolchain || state.tcOrigin == TcOrigin::TargetSection);
    const bool defaulted = !chosenForThisBuild
        && (state.tcOrigin == TcOrigin::GlobalDefault || state.tcOrigin == TcOrigin::TargetPin
            || state.tcOrigin == TcOrigin::FirstRun);
    std::optional<mcpp::pm::LockedToolchain> recorded;
    if (stated && !state.lockReadPath.empty())
        if (auto lock = mcpp::pm::load(state.lockReadPath))
            for (auto const& t : lock->toolchains)
                if (t.request == request) recorded = t;
    spec = recorded ? mcpp::toolchain::with_resolved_xim_version(spec, recorded->version)
                    : mcpp::toolchain::resolve_toolchain_spec(cfg, spec);
    const std::string version = mcpp::toolchain::to_xim_package(spec).ximVersion;
    if (defaulted) notice_default_toolchain_change(state, spec);
    if (!stated || version == pkg.ximVersion) return;
    if (!recorded && mcpp::invocation::get("MCPP_LOCKED").value_or("") == "1")
        mcpp::diag::note("toolchain/lock", std::format(
            "mcpp.lock has no record of the toolchain {}; it resolved to {}, and --locked keeps "
            "the lock as it is (a build without --locked records it)", request, version));
    else
        state.toolchainLocked = mcpp::pm::LockedToolchain{ request, version };
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
        auto cfg = state.get_cfg(true);
        if (!cfg) return std::unexpected(cfg.error());
        step2_resolve_request_version(state, **cfg, *spec);
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

        mcpp::fetcher::Fetcher fetcher(**cfg);

        mcpp::ui::info("Resolving", "toolchain");
        mcpp::fetcher::InstallProgressHandler progress;
        auto payload = fetcher.resolve_xpkg_path(pkg.target(),
            engineChoseUnservable ? mcpp::fetcher::PayloadIntent::Locate : mcpp::fetcher::PayloadIntent::Use,
            &progress);
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

        // A PAYLOAD NAMED OUTRIGHT (`xim:<name>@<version>`, design 2026-11-12
        // §16 T2) is resolved as `path:<root>` with the table it states of
        // itself (`.mcpp-toolchain.json`, schema 2): the same step, the same
        // probes and refusals as a directory a manifest names.
        if (spec->describedPayload) {
            auto d = mcpp::toolchain::read_payload_descriptor(payload->root);
            if (!d) return std::unexpected(d.error());
            if (!*d)
                return std::unexpected(std::format(
                    "toolchain '{}': the package has no {} at its root; a payload named "
                    "outright states its drivers there (\"schema\": 2, with \"family\", "
                    "\"prefix\", \"tools\")",
                    *state.tcSpec, mcpp::toolchain::payload_descriptor_filename));
            auto const& desc = **d;
            mcpp::manifest::LocalToolchain lt;
            lt.path   = payload->root.generic_string();
            lt.prefix = desc.prefix;
            lt.family = desc.family;
            if (!desc.sysroot.empty()) lt.sysroot = (payload->root / desc.sysroot).generic_string();
            for (auto const& [role, p] : desc.tools)
                lt.tools.emplace_back(role, (payload->root / p).generic_string());
            if (!desc.frontend.empty()
                && std::ranges::none_of(lt.tools, [](auto const& t) { return t.first == "cxx"; }))
                lt.tools.emplace_back("cxx", (payload->root / desc.frontend).generic_string());
            if (lt.family.empty()) {
                // The family is the drivers' when the payload does not say: its
                // C++ driver's name, else the drivers in `bin/`.
                for (auto const& [role, p] : lt.tools)
                    if (role == "cxx") {
                        auto stem = std::filesystem::path(p).stem().string();
                        lt.family = stem.ends_with("clang++") ? "llvm" : stem.ends_with("g++") ? "gcc" : "";
                    }
                if (lt.family.empty())
                    if (auto fam = mcpp::toolchain::parse_toolchain_spec("path:" + lt.path); fam)
                        lt.family = mcpp::toolchain::family_name(fam->family);
            }
            SourceDecision origin;
            origin.subject    = "toolchain.build";
            origin.cls        = SourceClass::Pinned;
            origin.originKind = "manifest";
            origin.originKey  = *state.tcSpec;
            state.localToolchain       = std::move(lt);
            state.localToolchainOrigin = std::move(origin);
            auto local = *spec;
            local.localRoot = payload->root;
            mcpp::ui::info("Resolved", std::format("{} → {}", spec->display(), payload->root.string()));
            return step2_use_local_toolchain(state, local);
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
            if (auto provided = state.provide_runtime_payload(pkg, payload->root); !provided)
                return std::unexpected(provided.error());
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
            else if (!state.pinReplacedDefault.empty() && state.platformToolchainPin)
                chosenBy = std::format(
                    "\n             the platform's ({}), replacing your "
                    "{} — override with `[target.{}] toolchain`",
                    state.platform.display(), state.pinReplacedDefault,
                    state.overrides.target_triple.empty()
                        ? mcpp::toolchain::triple::host_triple().str()
                        : state.overrides.target_triple);
            else if (!state.pinReplacedDefault.empty())
                chosenBy = std::format(
                    "\n             target default for {}, replacing your "
                    "{} — override with `[target.{}] toolchain`",
                    state.overrides.target_triple, state.pinReplacedDefault,
                    state.overrides.target_triple);
            else if (state.platformToolchainPin)
                chosenBy = std::format("  (the platform's, {})", state.platform.display());
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
        // What to write instead is this host's first run (one answer, as the
        // offline refusal below names it).
        const auto use = state.native_first_run_spec();
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
            "         {} = \"{}\"\n"
            "\n"
            "       or set a machine default with `mcpp toolchain default "
            "{}`, and see `mcpp toolchain list` for what is available.\n"
            "       (On Windows, `msvc@system` is different and remains "
            "supported: it names a family whose installation mcpp locates.)\n"
            "       Host LIBRARIES are a separate question and are not refused "
            "— a project may link them and owns the result.",
            kCurrentPlatform, kCurrentPlatform, use, use));
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
        const std::string release = mcpp::effects::no_auto_install()
            ? std::string("or unset MCPP_NO_AUTO_INSTALL to let mcpp auto-install.")
            : std::format("or {} to let mcpp auto-install.", mcpp::effects::offline_remedy());
        // Windows without a usable MSVC must not be told to install llvm:
        // that default resolves to clang targeting the MSVC ABI, which is
        // exactly what this machine cannot build. Name the toolchain that
        // will actually work there instead.
        if (mcpplibs::os::is_windows
            && !state.msvc_usable_either_origin()) {
            refusal::record(refusal::Code::OfflineDownloadRequired);
            return std::unexpected(std::format(
                "no toolchain configured, and the MSVC ABI is not usable here: {}.\n"
                "       run one of:\n"
                "         mcpp toolchain install {} --target {}\n"
                "         mcpp toolchain default {} --target {}\n"
                "       {}",
                mcpp::toolchain::msvc::describe_msvc_probe(mcpp::toolchain::msvc::probe_msvc()),
                pins::install_words(pins::kFirstRunNoMsvc), pins::kFirstRunNoMsvcTarget,
                pins::kFirstRunNoMsvc,  pins::kFirstRunNoMsvcTarget, release));
        }
        // The toolchain the first run would install: this host's row (pins::host_default_toolchain).
        const auto use = state.native_first_run_spec();
        refusal::record(refusal::Code::OfflineDownloadRequired);
        return std::unexpected(std::format(
            "no toolchain configured.\n"
            "       run one of:\n"
            "         mcpp toolchain install {}\n"
            "         mcpp toolchain default {}\n"
            "       {}",
            pins::install_words(use), use, release));
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

        if constexpr (mcpplibs::os::is_macos || mcpplibs::os::is_windows) {
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
                            mcpp::fetcher::PayloadIntent::Use, &progress);
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
        if (auto provided = state.provide_runtime_payload(defaultPkg, payload->root); !provided)
            return std::unexpected(provided.error());

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
        // D17: said once, and said exactly: which half of the MSVC ABI is
        // missing and where it was looked for -- not "no Visual Studio found"
        // on a machine whose Visual Studio was found and whose Windows SDK
        // was on another drive (design 2026-10-10 §12.1, S1) -- and how to
        // move to the MSVC ABI once it is complete. Nothing was specified, so
        // MinGW is chosen and recorded; a toolchain that was specified is
        // never replaced by it (D18, `msvc_unavailable_guidance`).
        static bool told = false;
        if (!std::exchange(told, true)) {
            const auto why = mcpp::toolchain::msvc::describe_msvc_probe(
                mcpp::toolchain::msvc::probe_msvc());
            mcpp::ui::info("First run",
                std::format("no toolchain configured, and the MSVC ABI is not usable here: {}. "
                            "Using {} for {} (MinGW-w64, self-contained). Once Visual Studio "
                            "and the Windows SDK are both in place: `mcpp toolchain default {}`",
                            why.empty() ? std::string("its STL or SDK is incomplete") : why,
                            *state.tcSpec, state.overrides.target_triple,
                            mcpp::toolchain::triple::pins::kFirstRunMsvc));
        }
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
      // A target mcpp does not read is not continued with silently (mcpp#989):
      // every setting that follows the target is read from it.
      if (!state.tc->targetTriple.empty() && !mcpp::toolchain::triple::parse(state.tc->targetTriple))
          mcpp::diag::degraded("toolchain/triple", std::format(
              "{} reports the target '{}', which mcpp does not read", state.tc->binaryPath.string(),
              state.tc->targetTriple),
              "the settings that follow the target -- its row, its sysroot, its C library and its "
              "loader -- are not applied to this build",
              "name the target: --target <triple>, or [toolchain] in mcpp.toml");

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
step2_bind_msvc_toolset(PrepareState& state) {
      // THE MSVC TOOLSET OF THE CLANG ROW, chosen once and recorded before the
      // runtime identity below reads its SDK version. See bind_msvc_sysroot.
      const std::string* declared = nullptr;
      if (auto tt = mcpp::toolchain::triple::parse(state.tc->targetTriple)) {
          if (auto s = base_statement(state, *tt); !s) return std::unexpected(s.error());
          declared = base_statement_text(state, *tt);
      }
      if (state.tc->compiler == mcpp::toolchain::CompilerId::Clang
          && mcpp::toolchain::is_msvc_target(*state.tc)) {
          auto bound = bind_msvc_sysroot(*state.tc, declared, [&] { return state.get_cfg(true); });
          if (!bound) return std::unexpected(bound.error());
      } else if (state.tc->compiler == mcpp::toolchain::CompilerId::MSVC) {
          if (auto ok = check_cl_row_sysroot(*state.tc, declared); !ok)
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
      if (!state.tc->base.msvc.sdkVersion.empty()) {
          mcpp::runtime::bind_windows_ucrt(
              state.runtimeBindingSnapshot, state.tc->base.msvc.sdkVersion);
          state.tc->runtimeContractHash = state.runtimeBindingSnapshot.contractHash;
          // And the artifact's runtime, when a stated base re-derived it:
          // the floor and the base's identity are both part of its contract.
          if (state.targetRuntimeBinding) {
              mcpp::runtime::bind_windows_ucrt(
                  *state.targetRuntimeBinding, state.tc->base.msvc.sdkVersion);
              state.tc->runtimeContractHash = state.targetRuntimeBinding->contractHash;
          }
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
          // Tests built on another machine run here and nothing is compiled
          // (`mcpp test --archive`): the toolchain is neither refused nor
          // replaced. Measured on a Windows runner with Visual Studio masked:
          // every archived test was "not run" for the compiler it did not use.
          if (!state.overrides.compiles()) return {};
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
              && !mcpp::effects::offline_mode()
              && !mcpp::effects::no_auto_install()
              && mcpplibs::os::is_windows;
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
                          pins::kFirstRunNoMsvc, pins::kFirstRunNoMsvcTarget));

          state.overrides.target_triple = std::string(pins::kFirstRunNoMsvcTarget);
          // The x86_64-windows-gnu row is defaultStatic; the target block that
          // normally applies that already ran, so mirror just this one field.
          if (state.m->buildConfig.linkage.empty()) state.m->buildConfig.linkage = "static";

          auto gnuSpec = mcpp::toolchain::parse_toolchain_spec(
              std::string(pins::kFirstRunNoMsvc));
          if (!gnuSpec) return std::unexpected(gnuSpec.error());
          if (auto t = mcpp::toolchain::triple::parse(state.overrides.target_triple))
              gnuSpec->target = *t;
          auto gnuPkg = mcpp::toolchain::to_xim_package(*gnuSpec);

          auto cfgR = state.get_cfg(true);
          if (!cfgR) return std::unexpected(cfgR.error());
          mcpp::fetcher::Fetcher fetcherR(**cfgR);
          mcpp::fetcher::InstallProgressHandler progressR;
          auto payloadR = fetcherR.resolve_xpkg_path(gnuPkg.target(),
                              mcpp::fetcher::PayloadIntent::Use, &progressR);
          if (!payloadR) {
              return std::unexpected(std::format(
                  "switching to the MinGW-w64 toolchain ({}) failed: {}\n"
                  "       install it manually with:\n"
                  "         mcpp toolchain install {} --target {}",
                  pins::kFirstRunNoMsvc, payloadR.error().message,
                  pins::install_words(pins::kFirstRunNoMsvc), pins::kFirstRunNoMsvcTarget));
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
          if (auto provided = state.provide_runtime_payload(gnuPkg, payloadR->root); !provided)
              return std::unexpected(provided.error());

          // Persist both axes so the repair happens once, not on every build.
          if (mcpp::config::write_default_toolchain(**cfgR, pins::kFirstRunNoMsvc))
              (*cfgR)->defaultToolchain = std::string(pins::kFirstRunNoMsvc);
          if (mcpp::config::write_default_target(**cfgR, state.overrides.target_triple))
              (*cfgR)->defaultTarget = state.overrides.target_triple;

          state.tcSpec   = std::string(pins::kFirstRunNoMsvc);
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

// THE BINUTILS THE PROJECT STATES BESIDE ITS GCC (SPEC-006 §2.9):
// `default = { package = "gcc@16.2.0", companions = { binutils = "xim:binutils@2.47" } }`
// replaces the binutils the payload declares. Binutils is the driver's
// assembler and linker, and GCC probed the assembler's features when it was
// built: one older than the one the payload declares may not assemble what
// that GCC emits, so it is refused. Only a statement of the project's
// mcpp.toml has companions; `--toolchain` names a spec without them.
static std::expected<void, std::string> step2_stated_companions(PrepareState& state) {
    if (state.tcOrigin != TcOrigin::ManifestToolchain || state.tcFromCommandLine || state.tcFromConsumer
        || state.overrides.tool_depth != 0 || !state.tc)
        return {};
    const auto* stated = state.m->toolchain.companions_for(kCurrentPlatform);
    if (!stated) return {};
    for (auto const& [name, ref] : *stated) {
        if (state.tc->compiler != mcpp::toolchain::CompilerId::GCC)
            return std::unexpected(std::format(
                "[toolchain].{}: companions.{} = \"{}\" is stated for {}, which is not a GCC; "
                "binutils is GCC's assembler and linker", kCurrentPlatform, name, ref, state.tc->label()));
        auto cfg = state.get_cfg(true);
        if (!cfg) return std::unexpected(cfg.error());
        mcpp::fetcher::Fetcher fetcher(**cfg);
        mcpp::fetcher::InstallProgressHandler progress;
        const std::string target = ref.find(':') == std::string::npos ? "xim:" + ref : ref;
        auto payload = fetcher.resolve_xpkg_path(target, mcpp::fetcher::PayloadIntent::Use, &progress);
        if (!payload)
            return std::unexpected(std::format("[toolchain].{}: companions.{} = \"{}\": {}",
                                               kCurrentPlatform, name, ref, payload.error().message));
        const auto bin = payload->root / "bin";
        std::error_code ec;
        if (!std::filesystem::exists(bin / "as", ec))
            return std::unexpected(std::format(
                "[toolchain].{}: companions.{} = \"{}\" has no assembler at {}",
                kCurrentPlatform, name, ref, (bin / "as").string()));
        const std::string version = payload->root.filename().string();
        const auto declared = mcpp::provider::declared_dependency(state.tc->binaryPath, name);
        if (declared && declared->find_first_of("0123456789") != std::string::npos) {
            namespace vr = mcpp::version_req;
            auto have = vr::parse_version(version);
            auto floor = vr::parse_version(std::string_view(*declared).substr(declared->find_first_of("0123456789")));
            if (have && floor && *have < *floor) {
                refusal::record(refusal::Code::CompanionTooOld);
                return std::unexpected(std::format(
                    "[toolchain].{}: companions.{} = \"{}\" is older than the {} {}'s payload declares ({}): "
                    "GCC chose its assembler features by that binutils when it was built\n"
                    "       help: state binutils {} or newer, or remove `companions` to use the declared one",
                    kCurrentPlatform, name, ref, name, state.tc->label(), *declared, *declared));
            }
        }
        state.tc->binutilsDir = bin;
        mcpp::ui::info("Companion", std::format("{} {} ({})", name, version, bin.string()));
    }
    return {};
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
      } else if (mcpp::effects::install_sources() == mcpp::effects::InstallSources::None
               || mcpp::effects::no_auto_install()) {
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
      if (auto r = step2_stated_companions(state); !r) return std::unexpected(r.error());
      if (auto r = step2_paired_system(state); !r) return std::unexpected(r.error());
      if (auto r = step2_bind_msvc_toolset(state); !r) return std::unexpected(r.error());
      if (auto r = step2_resolve_base(state); !r) return std::unexpected(r.error());
      if (auto r = step2_resolve_run_under(state); !r) return std::unexpected(r.error());
      step2_windows_runtime_identity(state);
      if (auto r = step2_msvc_abi_without_msvc_repair(state); !r) return std::unexpected(r.error());
      step2_musl_default_static_linkage(state);

    return {};
    };

    return {};
}

} // namespace mcpp::build
