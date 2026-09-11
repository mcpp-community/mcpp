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
import mcpp.toolchain.model;
import mcpp.toolchain.registry;
import mcpp.toolchain.triple;
import mcpp.ui;

namespace mcpp::pack {

// Everything after CLI option parsing for `mcpp pack`.
//
// `wantTarget` is the target NAME the user asked for, empty when they did not.
// It exists because `mcpp pack <name>` now routes on `[targets.<name>].kind`:
// a name that resolves to a program has to reach the binary selection below,
// or a project with two `bin` targets would accept `mcpp pack app2` and
// silently bundle app1 — the shape where the command succeeds and the answer
// is wrong.
export int build_and_pack(Options opts, bool modeFromUser,
                          const std::string& wantTarget = {}) {
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
        return 2;
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
        if (!ctx2) { mcpp::ui::error(ctx2.error()); return 2; }
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
            return 2;
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
        return 1;
    }

    // ─── Pick the main binary target ─────────────────────────────────
    //
    // An explicitly named target wins over the package-name convention: the
    // user said which one, and guessing past that is how `mcpp pack app2`
    // would produce app1's bundle under app2's name.
    std::filesystem::path mainBinary;
    if (!wantTarget.empty()) {
        for (auto& lu : ctx->plan.linkUnits) {
            if (lu.kind == mcpp::build::LinkUnit::Binary && lu.targetName == wantTarget) {
                mainBinary = ctx->outputDir / lu.output;
                break;
            }
        }
        if (mainBinary.empty()) {
            mcpp::ui::error(std::format(
                "target '{}' is not a program in this build", wantTarget));
            return 2;
        }
    }
    for (auto& lu : ctx->plan.linkUnits) {
        if (!mainBinary.empty()) break;
        if (lu.kind == mcpp::build::LinkUnit::Binary
            && lu.targetName == ctx->manifest.package.name)
        {
            mainBinary = ctx->outputDir / lu.output;
            break;
        }
    }
    if (mainBinary.empty()) {
        // Fall back to the first binary target if package.name doesn't match.
        for (auto& lu : ctx->plan.linkUnits) {
            if (lu.kind == mcpp::build::LinkUnit::Binary) {
                mainBinary = ctx->outputDir / lu.output;
                break;
            }
        }
    }
    if (mainBinary.empty()) {
        mcpp::ui::error("no binary target to pack");
        return 1;
    }

    auto cfg = mcpp::config::load_or_init(/*quiet=*/false,
        mcpp::fetcher::make_bootstrap_progress_callback());
    if (!cfg) { mcpp::ui::error(cfg.error().message); return 4; }

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
    }

    // ─── Build the plan + run ────────────────────────────────────────
    auto plan = mcpp::pack::make_plan(ctx->manifest, *cfg, opts,
        mainBinary, ctx->projectRoot, ctx->tc.targetTriple,
        // From the RESOLVED graph. `mcpp why runtime` on a real imgui project
        // lists `capability:opengl.glx.driver <- compat.glfw@3.4` — none of
        // which appears in the project's own manifest.
        ctx->plan.runtimeRequirements);
    if (!plan) { mcpp::ui::error(plan.error().message); return 1; }

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
    // a staging failure is the command failing. For a DISPATCHED format it is
    // an input the provider may or may not want, and treating it as a
    // precondition made every dispatched format unreachable on any target
    // whose built-in bundling is refused.
    //
    // Measured on macos-15 with mcpp 2026.9.11.1: `mcpp pack --format app`
    // never reached the dispatch, because `pack::run` refuses a Mach-O PROGRAM
    // outright -- the built-in closure walk is `LD_TRACE_LOADED_OBJECTS`, which
    // is glibc's, and dyld ignores it and runs the program instead. That
    // refusal is correct about the built-in archive and says nothing about
    // whether a `.app` bundler can work, since a bundler that names one
    // program needs no closure walk at all. The engine was answering a
    // question the provider had not been asked.
    //
    // So the failure is REPORTED AND CARRIED rather than swallowed: the reason
    // is printed as a warning, `pack_stage_dir` stays empty, and
    // `${mcpp.stage_dir}` then refuses at expansion naming that reason. A
    // provider that reads the tree gets a precise diagnostic; one that does not
    // proceeds. Nothing is silently degraded -- what changes is who decides.
    std::string stageFailure;
    if (auto r = mcpp::pack::run(*plan, *cfg); !r) {
        if (opts.format != mcpp::pack::Format::Dispatched) {
            mcpp::ui::error(r.error().message);
            return 1;
        }
        stageFailure = r.error().message;
        mcpp::ui::warning(std::format(
            "no staged tree for --format {}: {}\n"
            "  A format that consumes ${{mcpp.stage_dir}} cannot be produced "
            "here; one that names a\n"
            "  built file with ${{mcpp.target_file:<name>}} is unaffected.",
            opts.formatName, stageFailure));
    }

    // The staged tree is now on disk and final -- past the closure, the
    // `$ORIGIN` rewriting, the strip and the debug split. Describe it, so an
    // action that consumes it has something whose CONTENT changes when the
    // staged set does. Best-effort: see write_stage_manifest. Skipped when
    // staging did not happen, so no manifest describes a tree that is not
    // there.
    if (stageFailure.empty()) mcpp::pack::write_stage_manifest(plan->stagingRoot);

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
        if (!distCtx) { mcpp::ui::error(distCtx.error()); return 2; }

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
        for (auto const& a : distCtx->plan.actions) {
            if (a.role != mcpp::manifest::BuildAction::Role::Artifact) continue;
            if (preexistingArtifacts.contains({a.packageName, a.id})) continue;
            for (auto const& o : a.outputs) distOutputs.push_back(o);
        }
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
            return 1;
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
            return 1;
        }

        // THE CRITERION IS THE FILE, NOT THE EXIT CODE. A cached build program
        // replaying the first pass's answer, or a tool that writes nothing and
        // exits 0, both leave ninja reporting success -- and section 2's
        // measured failure was a packaging step that succeeded while carrying
        // nothing.
        std::error_code ec;
        for (auto const& o : distOutputs) {
            auto abs = std::filesystem::path(o).is_absolute()
                     ? std::filesystem::path(o) : distCtx->plan.outputDir / o;
            if (!std::filesystem::is_regular_file(abs, ec)
                && !std::filesystem::is_directory(abs, ec)) {
                mcpp::ui::error(std::format(
                    "--format {} reported success and produced nothing at {}",
                    opts.formatName, abs.string()));
                return 1;
            }
            mcpp::ui::status("Packed", mcpp::ui::shorten_path(abs, pathCtx));
        }
        return 0;
    }

    auto outPath = (opts.format == mcpp::pack::Format::Tar)
        ? plan->archivePath : plan->stagingRoot;
    mcpp::ui::status("Packed", mcpp::ui::shorten_path(outPath, pathCtx));
    return 0;
}

} // namespace mcpp::pack
