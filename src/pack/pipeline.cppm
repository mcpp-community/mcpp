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
import mcpp.pack;
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

    auto ctx = mcpp::build::prepare_build(/*print_fp=*/false, /*includeDevDeps=*/false,
                             /*extraTargets=*/{}, ov);
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
        mcpp::build::BuildOverrides ov2;
        ov2.target_triple = "x86_64-linux-musl";
        auto ctx2 = mcpp::build::prepare_build(false, false, {}, ov2);
        if (!ctx2) { mcpp::ui::error(ctx2.error()); return 2; }
        ctx = std::move(ctx2);
    }

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
    }

    // ─── Build the plan + run ────────────────────────────────────────
    auto plan = mcpp::pack::make_plan(ctx->manifest, *cfg, opts,
        mainBinary, ctx->projectRoot, ctx->tc.targetTriple,
        // From the RESOLVED graph. `mcpp why runtime` on a real imgui project
        // lists `capability:opengl.glx.driver <- compat.glfw@3.4` — none of
        // which appears in the project's own manifest.
        ctx->plan.runtimeRequirements);
    if (!plan) { mcpp::ui::error(plan.error().message); return 1; }

    mcpp::ui::info("Packing", std::format("{} v{} ({})",
        plan->packageName, plan->packageVersion,
        mcpp::pack::mode_cli_name(plan->opts.mode)));

    auto r = mcpp::pack::run(*plan, *cfg);
    if (!r) {
        mcpp::ui::error(r.error().message);
        return 1;
    }

    auto pathCtx = mcpp::fetcher::make_path_ctx(&*cfg, ctx->projectRoot);
    auto outPath = (opts.format == mcpp::pack::Format::Tar)
        ? plan->archivePath : plan->stagingRoot;
    mcpp::ui::status("Packed", mcpp::ui::shorten_path(outPath, pathCtx));
    return 0;
}

} // namespace mcpp::pack
