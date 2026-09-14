// mcpp.pack.pipeline — pack orchestration: build (re-preparing for musl static
// when needed), pick the main binary, plan + run the bundler.
// Bodies moved verbatim from the CLI layer. Zero behavior change.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.pack.pipeline;

import std;
import mcpp.build.prepare;
import mcpp.build.backend;
import mcpp.build.distribution;
import mcpp.build.flags;
import mcpp.build.ninja;
import mcpp.build.plan;
import mcpp.config;
import mcpp.fetcher.progress;
import mcpp.manifest;
import mcpp.pack;
import mcpp.pack.stage_tree;
import mcpp.pack.strip;
import mcpp.platform;
import mcpp.platform.env;
import mcpp.toolchain.model;
import mcpp.toolchain.probe;
import mcpp.toolchain.registry;
import mcpp.toolchain.triple;
import mcpp.ui;
import mcpp.xlings;

namespace mcpp::pack {

// #622 A10: the CLI exit code, plus the artifact(s) this pass reported as
// "Packed" -- the staged tree or archive for a built-in format, or the
// distributable(s) a DISPATCHED format's provider submitted for THIS
// request. `mcpp run --format <name>` takes `artifacts.front()` as its run
// operand rather than re-deriving it: the reported path IS the thing rule 3
// ("declare unconditionally, submit conditionally") makes unambiguous, and a
// second derivation is a second place for it to disagree. Empty whenever
// `rc != 0`.
export struct PackOutcome {
    int                                 rc = 0;
    std::vector<std::filesystem::path> artifacts;
};

// #634 A3: the two directory lists an Android row's closure is read against,
// asked of the row's own driver rather than derived from the NDK's layout:
// where the link searches for a library (`-print-search-dirs`), and the
// directory the driver finds bionic's `libc.so` in, which is the API level's
// stub directory -- the set of names the device itself provides.
//
// A driver that answers neither leaves both lists empty; `pack::run` then
// reports that it cannot tell the device's libraries from the program's rather
// than staging stubs.
struct DriverLibraryDirs {
    std::vector<std::filesystem::path> search;
    std::vector<std::filesystem::path> platform;
};

DriverLibraryDirs driver_library_dirs(const mcpp::toolchain::Toolchain& tc)
{
    DriverLibraryDirs out;
    const auto base = std::format("{} {}", mcpp::xlings::shq(tc.binaryPath.string()),
                                  tc.crossTargetFlag);
    if (auto r = mcpp::toolchain::run_capture(std::format(
            "{} -print-search-dirs {}", base, mcpp::platform::null_redirect))) {
        std::istringstream is{*r};
        std::string line;
        const std::string_view key = "libraries: =";
        const auto sep = mcpp::platform::env::path_list_separator();
        while (std::getline(is, line)) {
            if (!line.starts_with(key)) continue;
            std::string rest = mcpp::toolchain::trim_line(line.substr(key.size()));
            std::size_t at = 0;
            while (at <= rest.size()) {
                auto next = rest.find(sep, at);
                if (next == std::string::npos) next = rest.size();
                if (next > at)
                    out.search.push_back(
                        std::filesystem::path(rest.substr(at, next - at)).lexically_normal());
                at = next + sep.size();
            }
        }
    }
    if (auto r = mcpp::toolchain::run_capture(std::format(
            "{} -print-file-name=libc.so {}", base, mcpp::platform::null_redirect))) {
        // A driver that cannot place the library echoes the bare name back.
        std::filesystem::path lib(mcpp::toolchain::trim_line(*r));
        std::error_code ec;
        if (lib.has_parent_path() && std::filesystem::is_regular_file(lib, ec))
            out.platform.push_back(lib.parent_path().lexically_normal());
    }
    return out;
}

// #630 A9: build every triple but the PRIMARY one for a `kind = "app"`
// target whose form is a shared object on every requested row (Android).
// Each leg is its own `prepare_build` + `ninja` build, the same shape
// `build_and_pack_library`'s leg loop uses and for the same reason: the
// artifact a leg contributes is the one THIS run made, never a glob over
// `target/` that could silently pick up a stale binary from an earlier
// build. The primary triple is not built here — it takes the ordinary
// single-triple `build_and_pack` path, which is what stages the tree,
// deploys the declared files once and runs the one dispatch pass; this
// function only produces the OTHER legs' bytes and where they belong.
//
// Returns `nullopt` on the first leg that fails, having already printed the
// error — the same contract `build_and_pack_library` and `build_and_pack`
// use, so `cmd_pack` only has to check the outcome and return.
export std::optional<std::vector<SharedLeg>>
build_extra_android_legs(const std::string& targetName,
                         std::span<const std::string> triples,
                         const std::string& profile)
{
    std::vector<SharedLeg> out;
    for (auto const& triple : triples) {
        mcpp::build::BuildOverrides ov;
        ov.target_triple    = triple;
        // A packaged artifact leaves this machine — same fallback as every
        // other pack leg (`build_and_pack_library`, and the primary leg
        // below).
        ov.profile          = profile;
        ov.profile_fallback = "release";
        auto ctx = mcpp::build::prepare_build(false, false, {}, ov);
        if (!ctx) { mcpp::ui::error(ctx.error()); return std::nullopt; }

        auto be = mcpp::build::make_ninja_backend();
        mcpp::build::BuildOptions bo;
        if (auto br = be->build(ctx->plan, bo); !br) {
            if (!br.error().diagnosticOutput.empty()) {
                std::fputs(br.error().diagnosticOutput.c_str(), stderr);
                if (br.error().diagnosticOutput.back() != '\n') std::fputs("\n", stderr);
            }
            mcpp::ui::error(br.error().message);
            return std::nullopt;
        }

        // FROM THE PLAN, never a glob — see the function comment. A
        // dependency's own `shared` target also contributes a `SharedLibrary`
        // link unit to this plan, and `dependencyOwned` is what excludes it
        // (the same exclusion `pipeline.cppm`'s `is_program_link_unit` makes
        // for the primary leg).
        const mcpp::build::LinkUnit* lu = nullptr;
        for (auto const& u : ctx->plan.linkUnits)
            if (u.targetName == targetName
                && u.kind == mcpp::build::LinkUnit::SharedLibrary
                && !u.dependencyOwned) { lu = &u; break; }
        if (!lu) {
            mcpp::ui::error(std::format(
                "target '{}' produced no shared-object artifact for {}.\n"
                "  Every leg of a several-`--target` app pack must resolve to "
                "the same shared-object\n"
                "  form the primary `--target` does; this row did not.",
                targetName, triple));
            return std::nullopt;
        }

        auto t = mcpp::toolchain::triple::parse(triple);
        auto canonical = t ? t->str() : triple;
        auto abi = t ? mcpp::toolchain::triple::android_abi(*t) : triple;
        mcpp::ui::status("Packed leg", std::format("{}  [{}]", canonical, abi));
        // #634 A3: where THIS triple's closure is read from -- the same
        // channels the primary leg's plan names (`build_and_pack` below), from
        // this leg's own plan and driver.
        SharedLeg leg;
        leg.abi      = std::move(abi);
        leg.artifact = ctx->outputDir / lu->output;
        leg.searchDirs.push_back(leg.artifact.parent_path());
        for (auto const& d : ctx->plan.runtimeLibraryDirs) leg.searchDirs.push_back(d);
        for (auto const& d : ctx->plan.linkIntent.runtimeSearchDirs)
            leg.searchDirs.push_back(d);
        for (auto const& d : ctx->plan.linkIntent.linkLibraryDirs)
            leg.searchDirs.push_back(d);
        for (auto const& d : ctx->plan.linkIntent.transitiveNeededDirs)
            leg.searchDirs.push_back(d);
        auto dirs = driver_library_dirs(ctx->tc);
        for (auto const& d : dirs.search) leg.searchDirs.push_back(d);
        leg.platformDirs = std::move(dirs.platform);
        out.push_back(std::move(leg));
    }
    return out;
}

// Everything after CLI option parsing for `mcpp pack`.
//
// `wantTarget` is the target NAME the user asked for, empty when they did not.
// It exists because `mcpp pack <name>` now routes on `[targets.<name>].kind`:
// a name that resolves to a program has to reach the binary selection below,
// or a project with two `bin` targets would accept `mcpp pack app2` and
// silently bundle app1 — the shape where the command succeeds and the answer
// is wrong.
//
// `extraLegs` (#630 A9) is every OTHER triple a several-`--target` app-pack
// request named, already built by `build_extra_android_legs` above. This call
// still does exactly one build — of `opts.targetTriple`, the PRIMARY leg — and
// stages the primary's own artifact and the declared deploy files as it always
// has; `extraLegs`, when non-empty, only changes WHERE the primary's shared
// object lands (`Plan::extraSharedLegs`, read by `run_shared_program`) and adds
// the other legs, each with its own closure, beside it in the same staged tree,
// before the one dispatch pass runs. Empty for a single-`--target` pack.
export PackOutcome build_and_pack(Options opts, bool modeFromUser,
                          const std::string& wantTarget = {},
                          std::vector<SharedLeg> extraLegs = {}) {
    // `--target *-linux-musl` without an explicit `--mode` implies
    // `--mode static` — packaging a musl-static ELF as bundle-project
    // would feed patchelf a static binary and crash. The docs treat
    // this pair as equivalent; surface it in the code path too.
    if (!modeFromUser && opts.targetTriple.find("-musl") != std::string::npos) {
        opts.mode     = mcpp::pack::Mode::Static;
        modeFromUser  = true;   // user-equivalent intent — block manifest override
    }

    // ─── Build first (pack implies a fresh build) ────────────────────
    mcpp::build::BuildOverrides ov;
    if (opts.mode == mcpp::pack::Mode::Static && opts.targetTriple.empty())
        ov.target_triple = "x86_64-linux-musl";
    else
        ov.target_triple = opts.targetTriple;
    // A bundled program leaves this machine: release is the fallback, not dev.
    // `[build] default-profile` still decides when the project states one.
    ov.profile          = opts.profile;
    ov.profile_fallback = "release";

    // QUIET FOR A DISPATCHED FORMAT, AND ONLY UNTIL THE VALUE IS VALIDATED.
    //
    // `mcpp pack --format bogus` must write NOTHING to stdout and exit 2 --
    // the machine-output contract, asserted by
    // tests/e2e/202_machine_output_contract.sh, because this is the path a
    // client hits when it probes an mcpp for a capability. The set of valid
    // values is a property of the resolved graph, so the refusal cannot be
    // decided until prepare has run, and prepare narrates what it resolves.
    //
    // Nothing is lost when the value IS valid: the dispatch pass prepares a
    // second time and prints the same lines, so a successful
    // `pack --format <name>` narrates once rather than twice.
    const bool quietUntilValidated =
        opts.format == mcpp::pack::Format::Dispatched && !mcpp::ui::is_quiet();
    if (quietUntilValidated) mcpp::ui::set_quiet(true);
    auto ctx = mcpp::build::prepare_build(/*print_fp=*/false, /*includeDevDeps=*/false,
                             /*extraTargets=*/{}, ov);
    if (quietUntilValidated) mcpp::ui::set_quiet(false);
    if (!ctx) {
        mcpp::ui::error(ctx.error());
        return PackOutcome{2};
    }

    // Manifest may override mode only when neither --mode nor an
    // equivalent flag (--target *-musl → static) was given.
    if (!modeFromUser && !ctx->manifest.packConfig.defaultMode.empty()) {
        if (auto m = mcpp::pack::parse_mode(ctx->manifest.packConfig.defaultMode))
            opts.mode = *m;
    }

    // Re-derive target triple: if mode is Static we force the musl
    // triple even when the manifest's [pack].default_mode bumped us
    // here after `prepare_build` ran with the host toolchain.
    //
    // ...but NOT over a target the user asked for. `--mode static` on its own
    // has always meant "the musl-static ELF", and that stays; `--mode static
    // --target x86_64-windows-gnu` used to silently become a Linux build,
    // which was invisible while PE packaging did not exist and is a wrong
    // answer now that it does. An explicit `--target` is an instruction.
    if (opts.mode == mcpp::pack::Mode::Static
        && opts.targetTriple.empty()
        && ctx->tc.targetTriple.find("-musl") == std::string::npos) {
        // Need to re-prepare the build with the musl target.
        //
        // `ov` IS MUTATED RATHER THAN SHADOWED. It has to stay the record of
        // what produced `ctx`, because the dispatch pass below re-enters
        // prepare with the same overrides plus two fields -- and a second
        // overrides object left behind here would make that pass differ from
        // this build in a way nothing states.
        ov.target_triple = "x86_64-linux-musl";
        // Quiet on the same grounds as the first prepare: this one also runs
        // before `--format` has been validated.
        if (quietUntilValidated) mcpp::ui::set_quiet(true);
        auto ctx2 = mcpp::build::prepare_build(false, false, {}, ov);
        if (quietUntilValidated) mcpp::ui::set_quiet(false);
        if (!ctx2) { mcpp::ui::error(ctx2.error()); return PackOutcome{2}; }
        ctx = std::move(ctx2);
    }

    // ─── Is the requested format one anything provides? ──────────────
    //
    // BEFORE THE BUILD, because a refusal that arrives after a full compile is
    // a worse refusal, and because this is the earliest point at which it can
    // be exact: build programs have now run and declared what they provide.
    //
    // The set is read from a pass that asked for NOTHING. That is what the
    // "declare unconditionally, submit conditionally" rule buys -- a member
    // that declared only when asked would leave this list empty exactly when a
    // user names a format, and the refusal would name nothing.
    if (opts.format == mcpp::pack::Format::Dispatched) {
        auto const& provided = ctx->plan.providedPackFormats;
        if (std::ranges::find(provided, opts.formatName) == provided.end()) {
            std::string avail;
            for (auto b : mcpp::pack::kBuiltinPackFormats)
                avail += (avail.empty() ? "" : ", ") + std::string(b);
            for (auto const& f : provided) {
                if (mcpp::pack::is_builtin_pack_format(f)) continue;
                avail += ", " + f;
            }
            mcpp::ui::error(std::format(
                "unknown --format '{}'.\n"
                "  available in this build: {}\n"
                "  A format past `tar` and `dir` comes from a package in the "
                "resolved graph, which declares\n"
                "  it with `mcpp::provides_pack_format(\"<name>\")` in its build "
                "program. Add the package\n"
                "  that provides '{}' to [build-dependencies] and activate its "
                "feature.",
                opts.formatName, avail, opts.formatName));
            return PackOutcome{2};
        }
    }

    // A package claiming a built-in name is silently unreachable, since the
    // parser resolves `tar` and `dir` before consulting the graph at all.
    //
    // OUTSIDE THE DISPATCH BRANCH ABOVE, because the mistake is in the PACKAGE
    // and does not depend on what this invocation asked for. Reported on every
    // pack, so the author hears it on the plain `mcpp pack` they are most
    // likely to run.
    for (auto const& f : ctx->plan.providedPackFormats)
        if (mcpp::pack::is_builtin_pack_format(f))
            mcpp::ui::warning(std::format(
                "a package in this graph declares `mcpp:pack-format={}`, which "
                "is one of the archive shapes `mcpp pack` owns; `--format {}` "
                "will always select the built-in and never that package", f, f));

    auto be = mcpp::build::make_ninja_backend();
    mcpp::build::BuildOptions bo;
    auto br = be->build(ctx->plan, bo);
    if (!br) {
        // The compiler's own output, not just "build failed" — same reason as
        // in the library pipeline.
        if (!br.error().diagnosticOutput.empty()) {
            std::fputs(br.error().diagnosticOutput.c_str(), stderr);
            if (br.error().diagnosticOutput.back() != '\n') std::fputs("\n", stderr);
        }
        mcpp::ui::error(br.error().message);
        return PackOutcome{1};
    }

    // ─── Pick the main binary target ─────────────────────────────────
    //
    // An explicitly named target wins over the package-name convention: the
    // user said which one, and guessing past that is how `mcpp pack app2`
    // would produce app1's bundle under app2's name.
    //
    // #622 A3/A10: an `app` whose form on THIS row is a shared object still
    // links as `LinkUnit::SharedLibrary` (`mcpp.build.plan`), so "is this
    // link unit the program" cannot ask for `Binary` alone any more without
    // reintroducing the refusal `Target::is_program()` exists to remove
    // everywhere else this record sweeps (§2.3). `dependencyOwned` is
    // excluded: a dependency's own `shared` target contributes a link unit to
    // this plan too, and it is never the package being packed.
    auto is_program_link_unit = [&](const mcpp::build::LinkUnit& lu) {
        if (lu.kind == mcpp::build::LinkUnit::Binary) return true;
        if (lu.kind != mcpp::build::LinkUnit::SharedLibrary || lu.dependencyOwned)
            return false;
        for (auto const& t : ctx->manifest.targets)
            if (t.name == lu.targetName)
                return t.kind == mcpp::manifest::Target::Application;
        return false;
    };
    std::filesystem::path mainBinary;
    const mcpp::build::LinkUnit* chosenLu = nullptr;
    if (!wantTarget.empty()) {
        for (auto& lu : ctx->plan.linkUnits) {
            if (is_program_link_unit(lu) && lu.targetName == wantTarget) {
                mainBinary = ctx->outputDir / lu.output;
                chosenLu = &lu;
                break;
            }
        }
        if (mainBinary.empty()) {
            mcpp::ui::error(std::format(
                "target '{}' is not a program in this build", wantTarget));
            return PackOutcome{2};
        }
    }
    for (auto& lu : ctx->plan.linkUnits) {
        if (!mainBinary.empty()) break;
        if (is_program_link_unit(lu) && lu.targetName == ctx->manifest.package.name) {
            mainBinary = ctx->outputDir / lu.output;
            chosenLu = &lu;
            break;
        }
    }
    if (mainBinary.empty()) {
        // Fall back to the first binary target if package.name doesn't match.
        for (auto& lu : ctx->plan.linkUnits) {
            if (is_program_link_unit(lu)) {
                mainBinary = ctx->outputDir / lu.output;
                chosenLu = &lu;
                break;
            }
        }
    }
    if (mainBinary.empty()) {
        mcpp::ui::error("no binary target to pack");
        return PackOutcome{1};
    }
    // Passed to `make_plan` rather than re-derived from the file: `make_plan`
    // has only `mainBinary` and would otherwise have to ask the triple and
    // the manifest the same question a second time.
    const bool programIsSharedObject =
        chosenLu && chosenLu->kind == mcpp::build::LinkUnit::SharedLibrary;

    auto cfg = mcpp::config::load_or_init(/*quiet=*/false,
        mcpp::fetcher::make_bootstrap_progress_callback());
    if (!cfg) { mcpp::ui::error(cfg.error().message); return PackOutcome{4}; }

    // ─── What the build promised, and where its runtime lives ────────
    //
    // The C++ runtime contract has been resolved since the flags were
    // computed; `pack` simply had no way to see it (design §4.3), so on PE
    // nothing enforced it and on ELF the `ldd` closure agreed with it by
    // luck. Reading the RESOLVED value rather than the manifest string is the
    // point: a request that was downgraded (a per-role self-contained on
    // /MD, say) must not make the package behave as though it had been
    // honoured.
    {
        const auto flags = mcpp::build::compute_flags(ctx->plan);
        opts.carryToolchainRuntime =
            flags.contractByRole[static_cast<std::size_t>(
                mcpp::build::dist::Role::Distributable)]
            == mcpp::build::dist::Contract::ToolchainCoupled;
        opts.toolchainRuntimeDirs = ctx->plan.toolchain.linkRuntimeDirs;
        // Where a third-party dependency's shared library may be found. Both
        // channels, because they answer for different things: the runtime
        // library dirs are what `mcpp run` puts on the loader's path, and the
        // link intent's search dirs are what a dependency package declared.
        opts.depSearchDirs = ctx->plan.runtimeLibraryDirs;
        for (auto const& d : ctx->plan.linkIntent.runtimeSearchDirs)
            opts.depSearchDirs.push_back(d);
        // What the build placed relative to the executable (#615). The plan's
        // destinations are `bin/<to>/<file>`, and the executable is in `bin/`.
        for (auto const& d : ctx->plan.runtimeDeployFiles)
            opts.runtimeFiles.push_back(d.dest.lexically_relative("bin"));
        // #634 A3: the Android row reads its closure against the directories
        // its link declared -- a prebuilt library named through `[runtime]
        // link_library_dirs` is a file the link used and the device does not
        // have -- and against its driver's; see `driver_library_dirs`.
        if (programIsSharedObject) {
            for (auto const& d : ctx->plan.linkIntent.linkLibraryDirs)
                opts.depSearchDirs.push_back(d);
            for (auto const& d : ctx->plan.linkIntent.transitiveNeededDirs)
                opts.depSearchDirs.push_back(d);
            auto dirs = driver_library_dirs(ctx->tc);
            opts.toolchainLibraryDirs = std::move(dirs.search);
            opts.platformLibraryDirs  = std::move(dirs.platform);
        }
    }

    // ─── Build the plan + run ────────────────────────────────────────
    auto plan = mcpp::pack::make_plan(ctx->manifest, *cfg, opts,
        mainBinary, ctx->projectRoot, ctx->tc.targetTriple,
        // From the RESOLVED graph. `mcpp why runtime` on a real imgui project
        // lists `capability:opengl.glx.driver <- compat.glfw@3.4` — none of
        // which appears in the project's own manifest.
        ctx->plan.runtimeRequirements, programIsSharedObject);
    if (!plan) { mcpp::ui::error(plan.error().message); return PackOutcome{1}; }
    // #630 A9: see the field comment on `Plan::extraSharedLegs` and the
    // parameter comment on `extraLegs` above. A no-op (default-constructed,
    // empty) for every caller before this item.
    plan->extraSharedLegs = std::move(extraLegs);

    // The RESOLVED debug-information decision. On the plan, not in Options:
    // Options is the request, this is what it came out as once the manifest
    // and the toolchain had their say. Tools come from the build's own
    // toolchain so a cross bundle is stripped by the cross tool.
    plan->strip     = mcpp::pack::resolve_strip(opts, ctx->manifest.packConfig);
    plan->debugDir  = mcpp::pack::resolve_debug_dir(opts, ctx->manifest.packConfig,
                                                    ctx->projectRoot);
    plan->stripTools = mcpp::pack::StripTools{
        .strip   = mcpp::toolchain::binutils_tool(ctx->tc, "strip"),
        .objcopy = mcpp::toolchain::binutils_tool(ctx->tc, "objcopy"),
        // The CANONICAL triple, resolved the same way the library packer
        // resolves it: an empty `targetTriple` means "this host", and asking
        // the empty string would answer "in-band" for macOS and MSVC alike.
        .inBandDebugInfo = mcpp::pack::debug_info_is_in_band(
            ctx->tc.targetTriple.empty()
                ? mcpp::toolchain::triple::host_triple().str()
                : [&] {
                      auto t = mcpp::toolchain::triple::parse(ctx->tc.targetTriple);
                      return t ? t->str() : ctx->tc.targetTriple;
                  }()),
    };

    mcpp::ui::info("Packing", std::format("{} v{} ({}{})",
        plan->packageName, plan->packageVersion,
        mcpp::pack::mode_cli_name(plan->opts.mode),
        plan->strip ? ", stripped" : ""));

    // STAGING IS A SERVICE TO THE PROVIDER, NOT A PRECONDITION FOR DISPATCH.
    //
    // For `--format tar` and `--format dir` the staged tree IS the product, so
    // a staging failure -- no tree at all -- is the command failing. For a
    // DISPATCHED format a MISSING tree is still failing the same way (a
    // genuine I/O error staging steps 1-3), but an UNAVAILABLE CLOSURE is
    // not: `pack::run` now stages the program and its declared runtime files
    // before it asks whether this host can walk the artifact's dependency
    // closure, so a dispatched format receives that tree regardless of
    // whether the closure could be resolved. See §3 of
    // .agents/docs/2026-09-13-630-what-a-framework-still-hits-in-the-engine.md.
    //
    // Measured on macos-15 with mcpp 2026.9.11.1: `mcpp pack --format app`
    // never reached the dispatch, because `pack::run` refused a Mach-O PROGRAM
    // outright -- the built-in closure walk is `LD_TRACE_LOADED_OBJECTS`, which
    // is glibc's, and dyld ignores it and runs the program instead. That
    // refusal was correct about the built-in archive and said nothing about
    // whether a `.app` bundler could work, since a bundler that names one
    // program needs no closure walk at all. The engine was answering a
    // question the provider had not been asked.
    //
    // So a missing tree is REPORTED AND CARRIED rather than swallowed: the
    // reason is printed as a warning, `pack_stage_dir` stays empty, and
    // `${mcpp.stage_dir}` then refuses at expansion naming that reason. A
    // provider that reads the tree gets a precise diagnostic; one that does not
    // proceeds. Nothing is silently degraded -- what changes is who decides.
    std::string stageFailure;      // set only when NO tree exists at all.
    mcpp::pack::ClosureStatus closure;
    if (auto r = mcpp::pack::run(*plan, *cfg); !r) {
        if (opts.format != mcpp::pack::Format::Dispatched) {
            mcpp::ui::error(r.error().message);
            return PackOutcome{1};
        }
        stageFailure = r.error().message;
        mcpp::ui::warning(std::format(
            "no staged tree for --format {}: {}\n"
            "  A format that consumes ${{mcpp.stage_dir}} cannot be produced "
            "here; one that names a\n"
            "  built file with ${{mcpp.target_file:<name>}} is unaffected.",
            opts.formatName, stageFailure));
    } else if (!r->walked) {
        // The tree exists; only its dependency closure does not. Distinct
        // warning text -- "staged" is true here, unlike the branch above.
        closure = mcpp::pack::ClosureStatus{false, r->reason, r->needs};
        mcpp::ui::warning(std::format(
            "staged without its dependency closure: {}\n"
            "  A format that consumes ${{mcpp.stage_dir}} sees the program and its "
            "declared\n"
            "  runtime files but not its discovered dependencies; one that names a "
            "built file\n"
            "  with ${{mcpp.target_file:<name>}} is unaffected.",
            closure.reason));
    } else {
        closure.needs = r->needs;
    }

    // The staged tree is now on disk and final -- past the closure (walked or
    // not), the `$ORIGIN` rewriting, the strip and the debug split. Describe
    // it, so an action that consumes it has something whose CONTENT changes
    // when the staged set does, and so a provider can read whether the
    // closure was walked. Best-effort: see write_stage_manifest. Skipped when
    // no tree exists, so no manifest describes a tree that is not there.
    if (stageFailure.empty()) mcpp::pack::write_stage_manifest(plan->stagingRoot, closure);

    auto pathCtx = mcpp::fetcher::make_path_ctx(&*cfg, ctx->projectRoot);

    // ─── The dispatch pass ───────────────────────────────────────────
    //
    // A `role = "artifact"` action is a ninja edge, and the staged tree is
    // produced here, in C++, AFTER ninja has finished. So an artifact action
    // cannot depend on the staged tree in the pass that built it, and a
    // single-pass `--format <name>` is not expressible. Two passes are, and
    // every value this one needs was answered by the first:
    //
    //   `ov`                  the overrides that produced the build above
    //   `plan->stagingRoot`   from make_plan, which resolved the triple
    //   `opts.formatName`     the request, already checked against the graph
    //
    // NOTHING IS RE-DERIVED, and that is the whole discipline of this block.
    // `stagingRoot` is a function of the package name, the version, the
    // resolved triple and the mode; the resolved triple is not known until a
    // prepare has run, so computing it a second time before prepare -- from the
    // host triple, say -- is the shape where two derivations of one value agree
    // on every machine the author has and disagree on one they do not.
    if (opts.format == mcpp::pack::Format::Dispatched) {
        // WHICH ARTIFACT ACTIONS THIS BUILD ALREADY HAD, before a format was
        // requested. The dispatch below reports what the REQUEST introduced,
        // and this is the other half of that subtraction.
        std::set<std::pair<std::string, std::string>> preexistingArtifacts;
        for (auto const& a : ctx->plan.actions)
            if (a.role == mcpp::manifest::BuildAction::Role::Artifact)
                preexistingArtifacts.emplace(a.packageName, a.id);

        ov.pack_format    = opts.formatName;
        // Empty when staging was refused, which is what makes
        // `${mcpp.stage_dir}` refuse with the reason attached rather than
        // expand to a directory that does not exist.
        ov.pack_stage_dir = stageFailure.empty() ? plan->stagingRoot
                                                 : std::filesystem::path{};
        ov.pack_stage_reason = stageFailure;
        auto distCtx = mcpp::build::prepare_build(false, false, {}, ov);
        if (!distCtx) { mcpp::ui::error(distCtx.error()); return PackOutcome{2}; }

        // WHICH ACTIONS ARE THE DISTRIBUTABLE: the artifact actions the REQUEST
        // INTRODUCED. An action present in both passes existed before anyone
        // asked for a format -- a codesign stamp, a size budget -- and
        // reporting one as the package would be a wrong answer that looks like
        // a right one.
        //
        // THE FIRST VERSION ASKED A NARROWER QUESTION AND GOT IT WRONG. It
        // collected only actions naming `${mcpp.stage_dir}`, on the assumption
        // that a distributable consumes the staged closure. Not every format
        // does: an `.msi` built from ONE named program takes
        // `${mcpp.target_file:<name>}` and never looks at the tree, which is
        // the shape section 6 of the design record recommends -- "name the
        // input, do not harvest a directory", after a bind path that resolved
        // to nothing produced a valid, empty, 52 KB installer. So the member
        // that followed the guidance was the member the check refused, and the
        // workaround was to name the placeholder as an unused input purely to
        // satisfy it. Presence-in-this-pass is the property actually wanted,
        // and it needs nothing of the member.
        //
        // Identity is (package, id): an id is unique within the package that
        // declared it and nothing more.
        std::vector<std::string> distOutputs;
        // THE DISTRIBUTABLE IS THE TERMINAL ARTIFACT. A provider may submit a
        // chain (`dist-apk`: link, add libraries, align, sign); every output
        // is verified below, but the thing a user installs, and the operand
        // `mcpp run --format` hands the runner, is an output no other
        // introduced action consumes. Measured 2026-09-12: with the first
        // output taken as the operand, `adb-run` received the unsigned
        // `base.apk` and `adb install` refused it.
        std::vector<std::string> distInputs;
        for (auto const& a : distCtx->plan.actions) {
            if (a.role != mcpp::manifest::BuildAction::Role::Artifact) continue;
            if (preexistingArtifacts.contains({a.packageName, a.id})) continue;
            for (auto const& o : a.outputs) distOutputs.push_back(o);
            for (auto const& i : a.inputs)  distInputs.push_back(i);
        }
        auto absolute_of = [&](std::string const& p) {
            auto q = std::filesystem::path(p).is_absolute()
                   ? std::filesystem::path(p) : distCtx->plan.outputDir / p;
            return q.lexically_normal();
        };
        std::set<std::filesystem::path> consumed;
        for (auto const& i : distInputs) consumed.insert(absolute_of(i));
        // DECLARED AND THEN SUBMITTED NOTHING. The half of the contract a
        // member is most likely to get wrong is the gate, and a member whose
        // gate never opens leaves a pass that succeeds and produces no
        // package. Refused by name rather than reported as success.
        if (distOutputs.empty()) {
            mcpp::ui::error(std::format(
                "no action claimed --format '{}'.\n"
                "  A package declared it provides this format, and no build "
                "program submitted a new\n"
                "  `role = \"artifact\"` action when it was asked for.\n"
                "  The provider must gate on the request and not on anything "
                "else:\n"
                "      mcpp::provides_pack_format(\"{}\");                     "
                "// always\n"
                "      if (std::string_view(mcpp::pack_format()) == \"{}\") ..."
                "   // then submit",
                opts.formatName, opts.formatName, opts.formatName));
            return PackOutcome{1};
        }

        mcpp::ui::info("Distributing", std::format("{} v{} (--format {})",
            plan->packageName, plan->packageVersion, opts.formatName));

        // NO EXPLICIT GOALS. Everything but the dist edges is already up to
        // date from the build above, so a full drive costs a graph scan and
        // nothing else -- and an explicit goal set is how the 0.0.104 soname
        // aliases went missing, because an edge reachable only through
        // `default` is skipped under one.
        mcpp::build::BuildOptions dbo;
        auto dr = be->build(distCtx->plan, dbo);
        if (!dr) {
            if (!dr.error().diagnosticOutput.empty()) {
                std::fputs(dr.error().diagnosticOutput.c_str(), stderr);
                if (dr.error().diagnosticOutput.back() != '\n') std::fputs("\n", stderr);
            }
            mcpp::ui::error(dr.error().message);
            return PackOutcome{1};
        }

        // THE CRITERION IS THE FILE, NOT THE EXIT CODE. A cached build program
        // replaying the first pass's answer, or a tool that writes nothing and
        // exits 0, both leave ninja reporting success -- and section 2's
        // measured failure was a packaging step that succeeded while carrying
        // nothing.
        std::error_code ec;
        std::vector<std::filesystem::path> reported;
        std::vector<std::filesystem::path> intermediate;
        for (auto const& o : distOutputs) {
            auto abs = absolute_of(o);
            if (!std::filesystem::is_regular_file(abs, ec)
                && !std::filesystem::is_directory(abs, ec)) {
                mcpp::ui::error(std::format(
                    "--format {} reported success and produced nothing at {}",
                    opts.formatName, abs.string()));
                return PackOutcome{1};
            }
            if (consumed.contains(abs)) { intermediate.push_back(std::move(abs)); continue; }
            mcpp::ui::status("Packed", mcpp::ui::shorten_path(abs, pathCtx));
            reported.push_back(std::move(abs));
        }
        // Every output consumed by another: a cycle a provider should not
        // write, reported as all outputs rather than as nothing.
        if (reported.empty()) reported = std::move(intermediate);
        return PackOutcome{0, std::move(reported)};
    }

    auto outPath = (opts.format == mcpp::pack::Format::Tar)
        ? plan->archivePath : plan->stagingRoot;
    mcpp::ui::status("Packed", mcpp::ui::shorten_path(outPath, pathCtx));
    return PackOutcome{0, {outPath}};
}

} // namespace mcpp::pack
